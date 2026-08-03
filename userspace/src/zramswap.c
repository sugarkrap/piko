// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zramswap -- a compressed swap device in RAM, ahead of the SD card's.
 *
 * This board has around 52 MiB of usable RAM (see docs/HOWTO-SWAP.md) and,
 * until now, the only swap area was 64 MiB (now bigger, see cardswap.c) on
 * the SD card -- which comes and goes with the card and costs a real I/O
 * round-trip on removable media for every page. zram puts a second swap
 * device in RAM itself: pages written to /dev/zram0 are LZO-compressed and
 * kept in memory rather than sent anywhere, so a page that compresses even
 * 2:1 buys back more room than it cost, at RAM speed, with nothing to wait
 * on and nothing to lose if a card is pulled. It is not a replacement for
 * the card swap -- there is no way to give back the RAM a full zram device
 * is holding, so the card remains the backstop once zram itself is full.
 *
 * swapon(2) is told to prefer zram over the card explicitly
 * (SWAP_FLAG_PREFER, see PRIORITY below) rather than relying on bring-up
 * order: rcS brings zram up before the card is even necessarily present,
 * which would happen to rank it first anyway under the kernel's default
 * "earlier swapon = higher priority" rule, but that is exactly the kind of
 * ordering accident this project has been bitten by before (see cardswap.c
 * and /usr/sbin/sdcard for two more). Saying it outright costs one flag.
 *
 * Usage:
 *   zramswap on  [MiB]   create/resize zram0, sign, and swapon(2)
 *   zramswap off         swapoff(2), then reset the device (frees its RAM)
 *   zramswap status       exit 0 if currently swapped on
 *
 * Default: 32 MiB of uncompressed capacity. That is a ceiling, not a
 * reservation -- actual RAM used is whatever the stored pages compress to,
 * normally well under half that, and zsmalloc allocates it as pages
 * actually land there rather than up front. Still sized with real caution
 * on a ~52 MiB machine: see the comment on DEFAULT_MIB.
 *
 * WHY THIS EXISTS AS A BINARY. Same hole as cardswap.c and kill.c: this
 * busybox has no mkswap, swapon or swapoff applet, so there is no shell
 * path to a swap area here regardless of what it is backed by.
 *
 * WHY NOT JUST REUSE cardswap.c's ON-DISK CODE. It very nearly is reused --
 * write_signature() below is the same layout, because the v1 swap header
 * format doesn't care what is on the other end of the fd. But the two
 * differ in exactly the ways that make a shared helper not worth it:
 * zram is a block device that already exists in full (no holes, no
 * fill_zeroes(), no free-space or same-filesystem-as-root checks -- none
 * of those questions mean anything for a RAM-backed device), and it needs
 * a resize dance through /sys/block/zram0/{disksize,reset} that a file on
 * a card never does. Two small self-contained tools, like the rest of this
 * directory, rather than one with two modes.
 *
 * Deliberately depends on nothing but libc, and is built -static like
 * cardswap/md5sum/kill: this rootfs ships no dynamic linker.
 */

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEV_PATH	"/dev/zram0"
#define DISKSIZE_PATH	"/sys/block/zram0/disksize"
#define RESET_PATH	"/sys/block/zram0/reset"

#define PROC_SWAPS	"/proc/swaps"

/* Serialises `on` against `off`, same reasoning and same mechanism as
 * cardswap.c's LOCK_PATH -- a separate path because the two tools guard
 * different resources and there is no reason for one to block the other. */
#define LOCK_PATH	"/tmp/.zramswap.lock"

/* 32 MiB of *uncompressed* capacity on a ~52 MiB machine. Real usage is
 * whatever that compresses to (typically well under half, going by LZO's
 * usual ratios on general-purpose memory contents), but the number chosen
 * here is deliberately conservative rather than "as much as the ratio
 * seems to allow": incompressible pages (already-compressed images, audio)
 * still cost close to 1:1, and this device has no RAM to spare for a swap
 * layer guessing wrong. Override on the command line to try something
 * bigger once real numbers are in from /sys/block/zram0/mm_stat. */
#define DEFAULT_MIB	32
#define MIN_MIB		2
#define MAX_MIB		256

/* Preferred unconditionally over the card's swap -- see the header comment. */
#define PRIORITY	32000

static const char *Prog = "zramswap";

static void
usage (void)
{
  fprintf (stderr,
	   "usage: %s on  [MiB]   create/resize zram0, sign, and enable it\n"
	   "       %s off          disable it and free its RAM\n"
	   "       %s status        exit 0 if enabled\n"
	   "\n"
	   "default: %d MiB (uncompressed capacity)\n",
	   Prog, Prog, Prog, DEFAULT_MIB);
}

/* See cardswap.c's take_lock() -- identical reasoning, separate lock file. */
static void
take_lock (void)
{
  int fd = open (LOCK_PATH, O_RDWR | O_CREAT, 0600);

  if (fd < 0)
    return;

  while (flock (fd, LOCK_EX) != 0 && errno == EINTR)
    ;
}

/* True if PATH appears as a swap area in /proc/swaps. Identical to
 * cardswap.c's is_swapped_on() -- see there for why a plain first-field
 * compare is exact for the paths this project uses. */
static int
is_swapped_on (const char *path)
{
  char  line[512];
  FILE *f;
  int   found = 0;

  if ((f = fopen (PROC_SWAPS, "r")) == NULL)
    return 0;

  if (fgets (line, sizeof (line), f) != NULL)
    {
      while (fgets (line, sizeof (line), f) != NULL)
	{
	  char name[512];

	  if (sscanf (line, "%511s", name) != 1)
	    continue;
	  if (strcmp (name, path) == 0)
	    {
	      found = 1;
	      break;
	    }
	}
    }

  fclose (f);
  return found;
}

/* Write VALUE to the sysfs attribute at PATH. zram's disksize/reset
 * attributes both take a single write(2) with no read-back needed. */
static int
write_sysfs (const char *path, const char *value)
{
  size_t  len = strlen (value);
  int     fd;
  ssize_t n;

  if ((fd = open (path, O_WRONLY)) < 0)
    return -1;

  n = write (fd, value, len);
  close (fd);

  return (n == (ssize_t) len) ? 0 : -1;
}

/* Current disksize of /dev/zram0, in bytes, or -1 if it cannot be read
 * (device not present -- CONFIG_ZRAM not built, or /sys not mounted). */
static long long
read_disksize (void)
{
  FILE     *f;
  long long v = -1;

  if ((f = fopen (DISKSIZE_PATH, "r")) == NULL)
    return -1;
  if (fscanf (f, "%lld", &v) != 1)
    v = -1;
  fclose (f);

  return v;
}

/* Write the v1 swap signature into the device's first page. Byte-for-byte
 * the same layout as cardswap.c's write_signature() -- the on-disk format
 * doesn't know or care that the other end is RAM rather than an SD card --
 * kept as a second copy rather than a shared function for the reason given
 * in the header comment above. */
static int
write_signature (int fd, long pagesize, unsigned long pages)
{
  unsigned char *page;
  unsigned long  last_page = pages - 1;
  int            i;

  if ((page = calloc (1, (size_t) pagesize)) == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", Prog);
      return -1;
    }

  for (i = 0; i < 4; i++)
    page[1024 + i] = (unsigned char) ((1UL >> (8 * i)) & 0xff);		/* version     */
  for (i = 0; i < 4; i++)
    page[1028 + i] = (unsigned char) ((last_page >> (8 * i)) & 0xff);	/* last_page   */

  memcpy (page + pagesize - 10, "SWAPSPACE2", 10);

  if (lseek (fd, 0, SEEK_SET) != 0)
    {
      fprintf (stderr, "%s: lseek: %s\n", Prog, strerror (errno));
      free (page);
      return -1;
    }
  if (write (fd, page, (size_t) pagesize) != (ssize_t) pagesize)
    {
      fprintf (stderr, "%s: writing swap signature: %s\n", Prog,
	       strerror (errno));
      free (page);
      return -1;
    }

  free (page);
  return 0;
}

static int
do_on (unsigned long mib)
{
  long          pagesize = sysconf (_SC_PAGESIZE);
  long long     size;
  long long     current;
  unsigned long pages;
  char          sizebuf[32];
  int           fd;

  if (pagesize <= 0)
    pagesize = 4096;

  size  = (long long) mib * 1024 * 1024;
  pages = (unsigned long) (size / pagesize);

  if (pages < 2)
    {
      fprintf (stderr, "%s: %lu MiB is too small to be a swap area\n",
	       Prog, mib);
      return 1;
    }

  take_lock ();

  /* Already on? No-op, same as cardswap.c -- this runs unconditionally
   * from rcS on every boot. */
  if (is_swapped_on (DEV_PATH))
    return 0;

  current = read_disksize ();
  if (current < 0)
    {
      fprintf (stderr,
	       "%s: %s not readable -- is CONFIG_ZRAM built into this kernel?\n",
	       Prog, DISKSIZE_PATH);
      return 1;
    }

  /* Resize only if needed: reset() requires the device to be unused, and
   * skipping it when the size already matches (the common case on every
   * boot but the first) avoids that dance entirely. */
  if (current != size)
    {
      /* reset(2)'s write is documented to fail if the device is still held
       * open anywhere -- harmless to attempt even when nothing is holding
       * it, and the disksize write right after is what actually catches a
       * real failure here. */
      write_sysfs (RESET_PATH, "1");

      snprintf (sizebuf, sizeof (sizebuf), "%lld", size);
      if (write_sysfs (DISKSIZE_PATH, sizebuf) != 0)
	{
	  fprintf (stderr, "%s: writing %s to %s: %s\n", Prog, sizebuf,
		   DISKSIZE_PATH, strerror (errno));
	  return 1;
	}
    }

  if ((fd = open (DEV_PATH, O_RDWR)) < 0)
    {
      fprintf (stderr, "%s: open %s: %s\n", Prog, DEV_PATH, strerror (errno));
      return 1;
    }

  if (write_signature (fd, pagesize, pages) != 0)
    {
      close (fd);
      return 1;
    }

  if (fsync (fd) != 0)
    fprintf (stderr, "%s: fsync: %s\n", Prog, strerror (errno));
  close (fd);

  if (swapon (DEV_PATH, SWAP_FLAG_PREFER | PRIORITY) != 0)
    {
      fprintf (stderr, "%s: swapon %s: %s\n", Prog, DEV_PATH,
	       strerror (errno));
      return 1;
    }

  fprintf (stderr, "%s: swap enabled on %s (%lu MiB, priority %d)\n", Prog,
	   DEV_PATH, mib, PRIORITY);
  return 0;
}

static int
do_off (void)
{
  take_lock ();

  if (swapoff (DEV_PATH) != 0)
    {
      /* Same reasoning as cardswap.c's do_off(): EINVAL/ENOENT both mean
       * there is nothing to do, not a failure. */
      if (errno != EINVAL && errno != ENOENT)
	{
	  fprintf (stderr, "%s: swapoff %s: %s\n", Prog, DEV_PATH,
		   strerror (errno));
	  return 1;
	}
    }

  /* Actually free the RAM back rather than leaving a disksize-but-idle
   * device sitting there -- unlike the card's swapfile, there is nothing
   * to "keep" here; the whole point of turning it off is to give the
   * memory back. Best effort: a device that never got past a failed `on`
   * has nothing to reset, and that is not this command's problem. */
  write_sysfs (RESET_PATH, "1");

  fprintf (stderr, "%s: swap disabled on %s\n", Prog, DEV_PATH);
  return 0;
}

int
main (int argc, char *argv[])
{
  unsigned long mib = DEFAULT_MIB;

  if (argc < 2)
    {
      usage ();
      return 2;
    }

  if (strcmp (argv[1], "on") == 0)
    {
      if (argc >= 3)
	{
	  char *end;
	  mib = strtoul (argv[2], &end, 10);
	  if (*end != '\0' || mib < MIN_MIB || mib > MAX_MIB)
	    {
	      fprintf (stderr, "%s: size must be %d..%d MiB\n",
		       Prog, MIN_MIB, MAX_MIB);
	      return 2;
	    }
	}
      return do_on (mib);
    }

  if (strcmp (argv[1], "off") == 0)
    return do_off ();

  if (strcmp (argv[1], "status") == 0)
    {
      if (is_swapped_on (DEV_PATH))
	{
	  printf ("%s: on\n", DEV_PATH);
	  return 0;
	}
      printf ("%s: off\n", DEV_PATH);
      return 1;
    }

  usage ();
  return 2;
}
