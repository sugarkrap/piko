// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cardswap -- put a swapfile on the SD card, and take it away again.
 *
 * This board has around 52 MiB of usable RAM and no swap at all built in,
 * which is what makes a browser tab or a big image the difference between
 * "slow" and "the OOM killer took your session". The SD card is the only
 * writable storage with room to spare -- the root jffs2 is ~68 MiB total --
 * so the swap goes there, at /mnt/card/.zaurus/swap, alongside the rest of
 * the card-hosted software (see /etc/zaurus-card.sh for that layout).
 *
 * Sized for a ~512 MiB card (see DEFAULT_MIB below) -- a card that small
 * would need its default overridden on the command line. zram (see
 * zramswap.c) sits in front of this as a faster, RAM-resident swap layer;
 * this one remains the backstop once zram's fixed capacity is full, and
 * the only swap at all when no card is inserted.
 *
 * Usage:
 *   cardswap on  [path [MiB]]   create if needed, sign, and swapon(2)
 *   cardswap off [path]         swapoff(2); the file is left in place
 *   cardswap status [path]      exit 0 if that file is currently swapped on
 *
 * Defaults: /mnt/card/.zaurus/swap, 256 MiB.
 *
 * WHY THIS EXISTS AS A BINARY. There is no shell way to do this here: the
 * device's busybox is built without mkswap, swapon and swapoff (verified on
 * hardware -- `which` finds none of the three), exactly as it is built
 * without kill and pkill. So the same answer as userspace/src/kill.c: a
 * small static tool that calls the syscalls directly, rather than a script
 * that assumes an applet this rootfs does not have.
 *
 * WHY SWAP ON VFAT WORKS. A swapfile needs the filesystem to be able to map
 * file offsets to disk blocks, because the kernel writes swap pages straight
 * to the block device and never through the filesystem. FAT implements that
 * (fs/fat/inode.c sets .bmap = _fat_bmap), so mm/page_io.c's
 * generic_swapfile_activate() can build the extent list. Two consequences
 * shape the code below:
 *
 *   * the file must have no holes -- every block must really be allocated,
 *     so it is written out in full with write(2) and never ftruncate(2)d
 *     into existence;
 *   * once swapon(2) has succeeded, the file's blocks must not move. They
 *     will not while the filesystem stays mounted, which is the whole of
 *     the contract we need.
 *
 * THE HAZARD, STATED PLAINLY. Swap on removable media means pulling the
 * card while pages live on it loses those pages, and the processes that
 * owned them. Nothing in userspace can prevent that. What we can do -- and
 * do -- is make every *orderly* removal path turn swap off first:
 * /usr/sbin/sdcard on the mdev remove event, and mb-applet-card's Eject
 * before it unmounts. The applet's whole reason to exist is to give the
 * user an orderly path; this just adds one more thing to it.
 *
 * Deliberately depends on nothing but libc, and is built -static like
 * md5sum/brightd/kill: this rootfs ships no dynamic linker.
 */

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/swap.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PATH	"/mnt/card/.zaurus/swap"
#define DEFAULT_MIB	256

#define PROC_SWAPS	"/proc/swaps"

/* Serialises `on` against `off`. /usr/sbin/sdcard runs its whole body in
 * the background -- it has to, because neither call may block mdev -- so
 * an insert arriving while a remove is still being processed would
 * otherwise have one process signing the file while the other is tearing
 * the swap area down.
 * flock(2) rather than a lockfile-with-a-pid: the lock dies with the
 * process, so a killed or crashed run cannot leave a stale one behind on a
 * filesystem that persists across reboots. */
#define LOCK_PATH	"/tmp/.cardswap.lock"

/* Chunk used to write the file out. Small on purpose: this runs on a
 * ~52 MiB machine, quite possibly while that memory is already the problem
 * we are trying to fix. */
#define CHUNK		(64 * 1024)

/* Sanity bound on the requested size. The upper limit is FAT's own 4 GiB
 * file ceiling minus a margin; the lower one is a size at which a swap
 * area is not worth the write cycles. */
#define MIN_MIB		4
#define MAX_MIB		2048

/* Leave this much of the card free after the swapfile, so enabling swap
 * can never be the thing that fills a user's card up. Scaled up along with
 * DEFAULT_MIB for the bigger card: still a small fraction of a ~512 MiB
 * card, but enough headroom for the rest of /mnt/card/.zaurus (MPlayer,
 * opkg packages, user files) to grow into. */
#define SPARE_MIB	32

static const char *Prog = "cardswap";

static void
usage (void)
{
  fprintf (stderr,
	   "usage: %s on  [path [MiB]]   create/sign/enable a swapfile\n"
	   "       %s off [path]         disable it (file is kept)\n"
	   "       %s status [path]      exit 0 if enabled\n"
	   "\n"
	   "defaults: %s, %d MiB\n",
	   Prog, Prog, Prog, DEFAULT_PATH, DEFAULT_MIB);
}

/* Take the lock described at LOCK_PATH, waiting for it if held.
 *
 * Never fatal: if /tmp is unwritable there is nothing better to do than
 * carry on unserialised, which is exactly how this behaved before the lock
 * existed. The fd is deliberately leaked -- it is released by exit. */
static void
take_lock (void)
{
  int fd = open (LOCK_PATH, O_RDWR | O_CREAT, 0600);

  if (fd < 0)
    return;

  while (flock (fd, LOCK_EX) != 0 && errno == EINTR)
    ;
}

/* True if PATH appears as a swap area in /proc/swaps.
 *
 * The first column is the filename, and the kernel octal-escapes any space
 * in it. Our paths have none, so a plain first-field compare is exact. */
static int
is_swapped_on (const char *path)
{
  char  line[512];
  FILE *f;
  int   found = 0;

  if ((f = fopen (PROC_SWAPS, "r")) == NULL)
    return 0;

  /* Skip the header line ("Filename  Type  Size  Used  Priority"). */
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

/* mkdir -p for the swapfile's parent, so a card that has never held
 * card-hosted software still gets its .zaurus directory. Failures are the
 * caller's problem -- the open() that follows reports them properly. */
static void
make_parent_dirs (const char *path)
{
  char  buf[512];
  char *p;

  snprintf (buf, sizeof (buf), "%s", path);

  for (p = buf + 1; *p != '\0'; p++)
    if (*p == '/')
      {
	*p = '\0';
	mkdir (buf, 0755);
	*p = '/';
      }
}

/* Refuse to write the swapfile onto the root filesystem.
 *
 * This is the one mistake here with a genuinely bad outcome: the root
 * jffs2 is about 68 MiB in total, so a 256 MiB file on it fills the ROM
 * many times over and leaves a device that cannot even write a log line.
 * It happens by accident rather than by typo -- if the card is not mounted,
 * /mnt/card/.zaurus/swap is just a path on the root filesystem and every
 * open() along the way succeeds. Comparing the target's device against
 * "/"'s catches exactly that, without needing to know which mount point
 * the caller had in mind.
 *
 * Returns 0 when the target is safely on some other filesystem. */
static int
check_not_on_rootfs (const char *path)
{
  struct stat st_root, st_dir;
  char        dir[512];
  char       *slash;

  snprintf (dir, sizeof (dir), "%s", path);
  if ((slash = strrchr (dir, '/')) != NULL && slash != dir)
    *slash = '\0';
  else
    snprintf (dir, sizeof (dir), "/");

  if (stat ("/", &st_root) != 0)
    return 0;			/* cannot tell; do not stand in the way */

  /* The directory may not exist yet, in which case walk up to something
   * that does -- that is the filesystem the file would land on. */
  while (stat (dir, &st_dir) != 0)
    {
      if ((slash = strrchr (dir, '/')) == NULL || slash == dir)
	{
	  if (stat ("/", &st_dir) != 0)
	    return 0;
	  break;
	}
      *slash = '\0';
    }

  if (st_dir.st_dev == st_root.st_dev)
    {
      fprintf (stderr,
	       "%s: %s is on the root filesystem, not a card -- refusing.\n"
	       "%s: (is /mnt/card actually mounted?)\n",
	       Prog, path, Prog);
      return -1;
    }

  return 0;
}

/* Is there room for a SIZE-byte file where PATH is going?
 *
 * EXISTING is the size the file already occupies there (0 if absent), which
 * we get to count as available because it is about to be reused. */
static int
check_space (const char *path, off_t size, off_t existing)
{
  struct statvfs vfs;
  char           dir[512];
  char          *slash;
  unsigned long long avail;

  snprintf (dir, sizeof (dir), "%s", path);
  if ((slash = strrchr (dir, '/')) != NULL && slash != dir)
    *slash = '\0';
  else
    snprintf (dir, sizeof (dir), "/");

  if (statvfs (dir, &vfs) != 0)
    return 0;			/* cannot tell; let open()/write() decide */

  avail = (unsigned long long) vfs.f_bavail * (unsigned long long) vfs.f_frsize;
  avail += (unsigned long long) existing;

  if (avail < (unsigned long long) size
      + (unsigned long long) SPARE_MIB * 1024ULL * 1024ULL)
    {
      fprintf (stderr,
	       "%s: not enough free space on the card for a %lu MiB swapfile\n"
	       "%s: (%lu MiB free, %d MiB kept spare)\n",
	       Prog, (unsigned long) (size / (1024 * 1024)),
	       Prog, (unsigned long) (avail / (1024 * 1024)), SPARE_MIB);
      return -1;
    }

  return 0;
}

/* Write the v1 swap signature into the file's first page.
 *
 * Layout is include/linux/swap.h's union swap_header, read back by
 * mm/swapfile.c:read_swap_header(): version 1 and last_page at byte 1024
 * (just past the bootbits), and the string "SWAPSPACE2" in the last ten
 * bytes of the page. Everything else in that page must be zero -- notably
 * nr_badpages, which the kernel rejects outright for a file-backed swap
 * area.
 *
 * The page size is the RUNNING KERNEL's, not a constant: the header sits
 * at the end of page 0 and the kernel reads it back at its own PAGE_SIZE,
 * so sysconf() is the only correct source for it. */
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

  /* Little-endian by hand rather than by struct: this is an on-disk format
   * with a fixed byte order and the compiler's idea of __u32 alignment is
   * not part of the contract. ARM here is little-endian, and the kernel
   * byte-swaps the other case anyway. */
  for (i = 0; i < 4; i++)
    page[1024 + i] = (unsigned char) ((1UL >> (8 * i)) & 0xff);		/* version     */
  for (i = 0; i < 4; i++)
    page[1028 + i] = (unsigned char) ((last_page >> (8 * i)) & 0xff);	/* last_page   */
  /* 1032..1035 nr_badpages stays zero, as do the uuid and volume label. */

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

/* Fill the file from OFFSET to SIZE with zeroes.
 *
 * write(2) rather than ftruncate(2) on purpose: the kernel's swap extent
 * walk treats an unmapped block as a fatal "swapfile has holes", so every
 * block has to be really allocated. On FAT it would be allocated either
 * way, but writing it out is what makes this correct on any filesystem the
 * card might be reformatted to. */
static int
fill_zeroes (int fd, off_t offset, off_t size)
{
  static char zeroes[CHUNK];
  off_t       done = offset;

  if (lseek (fd, offset, SEEK_SET) != offset)
    {
      fprintf (stderr, "%s: lseek: %s\n", Prog, strerror (errno));
      return -1;
    }

  while (done < size)
    {
      size_t  want = (size - done > CHUNK) ? CHUNK : (size_t) (size - done);
      ssize_t n = write (fd, zeroes, want);

      if (n <= 0)
	{
	  if (n < 0 && errno == EINTR)
	    continue;
	  fprintf (stderr, "%s: write: %s\n", Prog,
		   n < 0 ? strerror (errno) : "short write (card full?)");
	  return -1;
	}
      done += n;
    }

  return 0;
}

static int
do_on (const char *path, unsigned long mib)
{
  struct stat st;
  long        pagesize = sysconf (_SC_PAGESIZE);
  off_t       size;
  off_t       existing = 0;
  unsigned long pages;
  int         fd;
  int         reuse = 0;

  if (pagesize <= 0)
    pagesize = 4096;

  size  = (off_t) mib * 1024 * 1024;
  pages = (unsigned long) (size / pagesize);

  if (pages < 2)
    {
      fprintf (stderr, "%s: %lu MiB is too small to be a swap area\n",
	       Prog, mib);
      return 1;
    }

  take_lock ();

  /* Already on? Then this is a no-op, and saying so is not an error --
   * the mdev hook fires again on every coldplug scan. */
  if (is_swapped_on (path))
    return 0;

  if (check_not_on_rootfs (path) != 0)
    return 1;

  /* An existing file of exactly the right size is reused as-is. That is
   * what makes the second and every later insertion of the same card
   * near-instant instead of a 256 MiB rewrite: only the signature page is
   * touched, and only to be sure of it. */
  if (stat (path, &st) == 0 && S_ISREG (st.st_mode))
    {
      existing = st.st_size;
      if (st.st_size == size)
	reuse = 1;
    }

  if (!reuse && check_space (path, size, existing) != 0)
    return 1;

  make_parent_dirs (path);

  if ((fd = open (path, O_RDWR | O_CREAT, 0600)) < 0)
    {
      fprintf (stderr, "%s: open %s: %s\n", Prog, path, strerror (errno));
      return 1;
    }

  if (!reuse)
    {
      fprintf (stderr, "%s: creating %lu MiB swapfile at %s\n",
	       Prog, mib, path);
      /* Start at the signature page: it is written separately below, and
       * writing it twice on a card this slow is a waste of a page. */
      if (fill_zeroes (fd, pagesize, size) != 0)
	{
	  close (fd);
	  return 1;
	}
      /* Only ever shrinks: the full length has just been written, so this
       * cannot punch a hole. It matters when an older, larger swapfile is
       * being replaced by a smaller one -- without it the leftover tail
       * would sit on the card forever, doing nothing. */
      if (ftruncate (fd, size) != 0)
	fprintf (stderr, "%s: ftruncate: %s\n", Prog, strerror (errno));
    }

  if (write_signature (fd, pagesize, pages) != 0)
    {
      close (fd);
      return 1;
    }

  /* The extent walk reads the file's block map, so everything we just
   * wrote has to have reached the card first. */
  if (fsync (fd) != 0)
    fprintf (stderr, "%s: fsync: %s\n", Prog, strerror (errno));
  close (fd);
  sync ();

  /* Priority 0 via flags=0: the kernel then assigns a descending default
   * priority. With one swap area there is nothing to prioritise against. */
  if (swapon (path, 0) != 0)
    {
      fprintf (stderr, "%s: swapon %s: %s\n", Prog, path, strerror (errno));
      return 1;
    }

  fprintf (stderr, "%s: swap enabled on %s (%lu MiB)\n", Prog, path, mib);
  return 0;
}

static int
do_off (const char *path)
{
  take_lock ();

  /* Deliberately NOT gated on is_swapped_on(): the kernel decides what a
   * swap area is by INODE, but /proc/swaps prints the path the area had
   * when it was enabled, and those two can drift apart. A lazily
   * unmounted card leaves the entry reading "/.zaurus/swap" -- the mount
   * point stripped off -- and a version of this that checked the listing
   * first then declined to act would refuse to turn off the very swap
   * area it was asked about (found live on 2026-08-02). Just make the
   * call: swapoff(2) on something that is not a swap area is a cheap
   * EINVAL, and the only honest test of "can this be turned off" is
   * trying. */
  if (swapoff (path) != 0)
    {
      /* Neither of these is a failure to report: EINVAL is "that is not a
       * swap area" (already off, or never was) and ENOENT is "no such
       * file" (the card is gone). Both mean there is nothing to do. */
      if (errno == EINVAL || errno == ENOENT)
	return 0;
      fprintf (stderr, "%s: swapoff %s: %s\n", Prog, path, strerror (errno));
      return 1;
    }

  fprintf (stderr, "%s: swap disabled on %s\n", Prog, path);
  return 0;
}

int
main (int argc, char *argv[])
{
  const char   *path = DEFAULT_PATH;
  unsigned long mib  = DEFAULT_MIB;

  if (argc < 2)
    {
      usage ();
      return 2;
    }

  if (argc >= 3)
    path = argv[2];

  if (strcmp (argv[1], "on") == 0)
    {
      if (argc >= 4)
	{
	  char *end;
	  mib = strtoul (argv[3], &end, 10);
	  if (*end != '\0' || mib < MIN_MIB || mib > MAX_MIB)
	    {
	      fprintf (stderr, "%s: size must be %d..%d MiB\n",
		       Prog, MIN_MIB, MAX_MIB);
	      return 2;
	    }
	}
      return do_on (path, mib);
    }

  if (strcmp (argv[1], "off") == 0)
    return do_off (path);

  if (strcmp (argv[1], "status") == 0)
    {
      if (is_swapped_on (path))
	{
	  printf ("%s: on\n", path);
	  return 0;
	}
      printf ("%s: off\n", path);
      return 1;
    }

  usage ();
  return 2;
}
