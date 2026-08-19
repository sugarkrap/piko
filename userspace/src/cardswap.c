
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

#define DEFAULT_PATH	"/var/swap"
#define DEFAULT_MIB	64

#define PROC_SWAPS	"/proc/swaps"

#define LOCK_PATH	"/tmp/.cardswap.lock"

#define CHUNK		(64 * 1024)

#define MIN_MIB		4
#define MAX_MIB		2048

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
    return 0;

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

  if (is_swapped_on (path))
    return 0;

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
      if (fill_zeroes (fd, pagesize, size) != 0)
	{
	  close (fd);
	  return 1;
	}
      if (ftruncate (fd, size) != 0)
	fprintf (stderr, "%s: ftruncate: %s\n", Prog, strerror (errno));
    }

  if (write_signature (fd, pagesize, pages) != 0)
    {
      close (fd);
      return 1;
    }

  if (fsync (fd) != 0)
    fprintf (stderr, "%s: fsync: %s\n", Prog, strerror (errno));
  close (fd);
  sync ();

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

  if (swapoff (path) != 0)
    {
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
