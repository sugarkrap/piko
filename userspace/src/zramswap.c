
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

#define LOCK_PATH	"/tmp/.zramswap.lock"

#define DEFAULT_MIB	32
#define MIN_MIB		2
#define MAX_MIB		256

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

static void
take_lock (void)
{
  int fd = open (LOCK_PATH, O_RDWR | O_CREAT, 0600);

  if (fd < 0)
    return;

  while (flock (fd, LOCK_EX) != 0 && errno == EINTR)
    ;
}

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
    page[1024 + i] = (unsigned char) ((1UL >> (8 * i)) & 0xff);
  for (i = 0; i < 4; i++)
    page[1028 + i] = (unsigned char) ((last_page >> (8 * i)) & 0xff);

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

  if (current != size)
    {
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
      if (errno != EINVAL && errno != ENOENT)
	{
	  fprintf (stderr, "%s: swapoff %s: %s\n", Prog, DEV_PATH,
		   strerror (errno));
	  return 1;
	}
    }

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
