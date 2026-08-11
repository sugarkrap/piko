#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int basename_matches(const char *cmdline, const char *name)
{
	const char *base;
	const char *slash;

	if (!cmdline || !*cmdline || !name || !*name)
		return 0;

	slash = strrchr(cmdline, '/');
	base = slash ? slash + 1 : cmdline;
	return strcmp(base, name) == 0;
}

static int kill_named_processes(const char *name)
{
	DIR *proc_dir;
	struct dirent *entry;
	int matches = 0;
	int errors = 0;
	pid_t self = getpid();

	proc_dir = opendir("/proc");
	if (!proc_dir) {
		perror("opendir /proc");
		return 1;
	}

	while ((entry = readdir(proc_dir)) != NULL) {
		char *endptr;
		char path[64];
		char cmdline[256];
		FILE *file;
		pid_t pid;
		size_t bytes_read;

		if (!isdigit((unsigned char)entry->d_name[0]))
			continue;

		pid = (pid_t)strtol(entry->d_name, &endptr, 10);
		if (*endptr != '\0' || pid <= 1 || pid == self)
			continue;

		snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
		file = fopen(path, "rb");
		if (!file)
			continue;

		bytes_read = fread(cmdline, 1, sizeof(cmdline) - 1, file);
		fclose(file);
		if (bytes_read == 0)
			continue;

		cmdline[bytes_read] = '\0';
		if (!basename_matches(cmdline, name))
			continue;

		if (kill(pid, SIGTERM) == -1) {
			fprintf(stderr, "pkillx: kill %ld failed: %s\n", (long)pid,
				strerror(errno));
			errors = 1;
			continue;
		}

		matches++;
	}

	closedir(proc_dir);
	return errors ? 1 : (matches > 0 ? 0 : 1);
}

int main(int argc, char **argv)
{
	int index;
	int rc = 1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s name [name ...]\n", argv[0]);
		return 2;
	}

	for (index = 1; index < argc; index++) {
		if (kill_named_processes(argv[index]) == 0)
			rc = 0;
	}

	return rc;
}