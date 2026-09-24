#include "ird.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_CLIENTS	8

static const char *mode_name[] = { "off", "irda", "blaster" };

static const char *irda_up[] = { "irda", "pxaficp_ir", "ircomm", "ircomm-tty", NULL };
static const char *irda_down[] = { "ircomm-tty", "ircomm", "pxaficp_ir", "irda", NULL };
static const char *cir_first[] = { "rc-core", NULL };
static const char *cir_last[] = { "piko-cir", NULL };
static const char *cir_down[] = { "piko-cir", "rc-core", NULL };
static const char *cir_decoders[] = {
	"ir-nec-decoder", "ir-rc5-decoder", "ir-rc6-decoder", "ir-jvc-decoder",
	"ir-sony-decoder", "ir-sharp-decoder", "ir-sanyo-decoder", NULL
};

static int		clients[MAX_CLIENTS];
static int		subscribed[MAX_CLIENTS];
static enum ird_mode	intent = IRD_OFF;
static int		want_discovery;
static pid_t		scout = -1;
static int		scout_pipe = -1;
static volatile int	running = 1;

static const char *env_or(const char *key, const char *fallback)
{
	const char *v = getenv(key);

	return v && *v ? v : fallback;
}

const char *ird_sock_path(void)
{
	return env_or("IRD_SOCK", IRD_SOCK_DEFAULT);
}

static const char *conf_path(void)	{ return env_or("IRD_CONF", IRD_CONF_DEFAULT); }
static const char *lirc_path(void)	{ return env_or("IRD_LIRC", IRD_LIRC_DEFAULT); }
static const char *devname_path(void)	{ return env_or("IRD_DEVNAME", IRD_DEVNAME_DEFAULT); }
static const char *net_path(void)	{ return env_or("IRD_NET", IRD_NET_DEFAULT); }
static int dry_run(void)		{ return getenv("IRD_DRYRUN") != NULL; }

static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int run_modprobe(const char *mod, int remove)
{
	pid_t pid;
	int st;

	if (dry_run())
		return 0;

	pid = fork();
	if (pid < 0)
		return -1;

	if (!pid) {
		char *const argv_add[] = { (char *)"/sbin/modprobe", (char *)mod, NULL };
		char *const argv_del[] = { (char *)"/sbin/modprobe", (char *)"-r",
					   (char *)mod, NULL };
		char *const envp[] = { NULL };

		execve("/sbin/modprobe", remove ? argv_del : argv_add, envp);
		_exit(127);
	}

	if (waitpid(pid, &st, 0) < 0)
		return -1;

	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int load_all(const char *const *mods, int optional)
{
	int i;

	for (i = 0; mods[i]; i++) {
		if (run_modprobe(mods[i], 0) && !optional)
			return -1;
	}

	return 0;
}

static void drop_all(const char *const *mods)
{
	int i;

	for (i = 0; mods[i]; i++)
		run_modprobe(mods[i], 1);
}

static int iface_present(void)
{
	char path[256];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", net_path(), IRD_IFACE);

	return stat(path, &st) == 0;
}

static int iface_flags(short *flags)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	int rc;

	if (fd < 0)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", IRD_IFACE);

	rc = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if (!rc)
		*flags = ifr.ifr_flags;

	close(fd);

	return rc;
}

static int iface_set_up(int up)
{
	struct ifreq ifr;
	int fd;
	int rc;

	if (dry_run())
		return 0;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", IRD_IFACE);

	if (ioctl(fd, SIOCGIFFLAGS, &ifr)) {
		close(fd);
		return -1;
	}

	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;

	rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
	close(fd);

	return rc;
}

static void sync_devname(void)
{
	char host[64];
	FILE *f;

	if (gethostname(host, sizeof(host)))
		return;
	host[sizeof(host) - 1] = '\0';

	f = fopen(devname_path(), "w");
	if (!f)
		return;

	fprintf(f, "%s\n", host);
	fclose(f);
}

static enum ird_mode derive_mode(void)
{
	struct stat st;
	short flags;

	if (stat(lirc_path(), &st) == 0)
		return IRD_BLASTER;

	if (iface_present() && !iface_flags(&flags) && (flags & IFF_UP))
		return IRD_IRDA;

	return IRD_OFF;
}

static void scout_loop(int out)
{
	char buf[sizeof(struct ird_device_list) + 32 * sizeof(struct ird_device_info)];
	int sk = socket(AF_IRDA, SOCK_STREAM, 0);

	if (sk < 0) {
		dprintf(out, "note irda sockets unavailable\n");
		_exit(0);
	}

	while (1) {
		struct ird_device_list *list;
		socklen_t len = sizeof(int);
		socklen_t dlen = sizeof(buf);
		int val = 5000;
		unsigned int i;

		if (getsockopt(sk, IRD_SOL_IRLMP, IRD_WAITDEVICE, &val, &len)) {
			if (errno == EINTR)
				break;
			continue;
		}

		if (getsockopt(sk, IRD_SOL_IRLMP, IRD_ENUMDEVICES, buf, &dlen))
			continue;

		list = (struct ird_device_list *)buf;

		for (i = 0; i < list->len && i < 32; i++) {
			char name[sizeof(list->dev[i].info) + 1];

			memcpy(name, list->dev[i].info, sizeof(list->dev[i].info));
			name[sizeof(name) - 1] = '\0';

			if ((int)list->dev[i].daddr != val)
				continue;

			dprintf(out, "peer %08x %s\n", list->dev[i].daddr,
				name[0] ? name : "unnamed");
		}
	}

	close(sk);
	_exit(0);
}

static void scout_stop(void)
{
	if (scout > 0) {
		kill(scout, SIGTERM);
		waitpid(scout, NULL, 0);
		scout = -1;
	}
	if (scout_pipe >= 0) {
		close(scout_pipe);
		scout_pipe = -1;
	}
}

static void scout_start(void)
{
	int fds[2];

	scout_stop();

	if (pipe(fds))
		return;

	scout = fork();
	if (scout < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}

	if (!scout) {
		close(fds[0]);
		signal(SIGTERM, SIG_DFL);
		scout_loop(fds[1]);
		_exit(0);
	}

	close(fds[1]);
	scout_pipe = fds[0];
}

static void teardown(enum ird_mode from)
{
	if (from == IRD_IRDA) {
		scout_stop();
		iface_set_up(0);
		drop_all(irda_down);
	} else if (from == IRD_BLASTER) {
		drop_all(cir_down);
		drop_all(cir_decoders);
	}
}

static int apply_mode(enum ird_mode want, char *err, size_t n)
{
	enum ird_mode now = derive_mode();

	if (now != want)
		teardown(now);

	if (want == IRD_OFF) {
		teardown(IRD_IRDA);
		teardown(IRD_BLASTER);
		return 0;
	}

	if (want == IRD_IRDA) {
		if (load_all(irda_up, 0)) {
			snprintf(err, n, "cannot load the irda stack");
			return -1;
		}
		if (!iface_present()) {
			snprintf(err, n, "no %s after loading", IRD_IFACE);
			return -1;
		}
		if (iface_set_up(1)) {
			snprintf(err, n, "cannot bring %s up", IRD_IFACE);
			return -1;
		}
		sync_devname();
		if (want_discovery)
			scout_start();
		return 0;
	}

	if (load_all(cir_first, 0)) {
		snprintf(err, n, "cannot load rc-core");
		return -1;
	}
	load_all(cir_decoders, 1);
	if (load_all(cir_last, 0)) {
		snprintf(err, n, "cannot load piko-cir");
		return -1;
	}

	if (derive_mode() != IRD_BLASTER) {
		snprintf(err, n, "no %s after loading", lirc_path());
		return -1;
	}

	return 0;
}

static void conf_load(void)
{
	FILE *f = fopen(conf_path(), "r");
	char line[128];

	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');

		if (nl)
			*nl = '\0';

		if (!strncmp(line, "mode=", 5)) {
			if (!strcmp(line + 5, "irda"))
				intent = IRD_IRDA;
			else if (!strcmp(line + 5, "blaster"))
				intent = IRD_BLASTER;
			else
				intent = IRD_OFF;
		} else if (!strncmp(line, "discovery=", 10)) {
			want_discovery = atoi(line + 10) ? 1 : 0;
		}
	}

	fclose(f);
}

static void conf_save(void)
{
	char tmp[280];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.new", conf_path());

	f = fopen(tmp, "w");
	if (!f)
		return;

	fprintf(f, "mode=%s\n", mode_name[intent]);
	fprintf(f, "discovery=%d\n", want_discovery);

	if (fflush(f) || fclose(f)) {
		unlink(tmp);
		return;
	}

	if (rename(tmp, conf_path()))
		unlink(tmp);
}

static void broadcast(const char *line)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i] >= 0 && subscribed[i])
			dprintf(clients[i], "%s", line);
	}
}

static void reply_status(int fd)
{
	dprintf(fd, "ok mode=%s intent=%s discovery=%d scout=%s\n",
		mode_name[derive_mode()], mode_name[intent], want_discovery,
		scout > 0 ? "up" : "down");
}

static void handle(int fd, int slot, char *line)
{
	char err[160];
	char *arg = strchr(line, ' ');

	if (arg)
		*arg++ = '\0';

	if (!strcmp(line, "status")) {
		reply_status(fd);
		return;
	}

	if (!strcmp(line, "subscribe")) {
		subscribed[slot] = 1;
		dprintf(fd, "ok subscribed\n");
		return;
	}

	if (!strcmp(line, "discovery")) {
		if (!arg) {
			dprintf(fd, "ok discovery=%d\n", want_discovery);
			return;
		}
		want_discovery = !strcmp(arg, "on");
		conf_save();
		if (derive_mode() == IRD_IRDA) {
			if (want_discovery)
				scout_start();
			else
				scout_stop();
		}
		reply_status(fd);
		return;
	}

	if (!strcmp(line, "off") || !strcmp(line, "irda")
	    || !strcmp(line, "blaster")) {
		enum ird_mode want = IRD_OFF;

		if (!strcmp(line, "irda"))
			want = IRD_IRDA;
		else if (!strcmp(line, "blaster"))
			want = IRD_BLASTER;

		err[0] = '\0';
		if (apply_mode(want, err, sizeof(err))) {
			dprintf(fd, "err %s\n", err[0] ? err : "failed");
			return;
		}

		intent = want;
		conf_save();
		reply_status(fd);
		broadcast("event mode\n");
		return;
	}

	dprintf(fd, "err unknown command\n");
}

static void on_term(int sig)
{
	(void)sig;
	running = 0;
}

int main(void)
{
	struct sockaddr_un addr;
	char err[160];
	int srv;
	int i;

	for (i = 0; i < MAX_CLIENTS; i++)
		clients[i] = -1;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	conf_load();

	unlink(ird_sock_path());

	srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) {
		say("ird: cannot make a socket");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ird_sock_path());

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr))) {
		say("ird: cannot bind %s", ird_sock_path());
		return 1;
	}

	chmod(ird_sock_path(), 0666);

	if (listen(srv, 4)) {
		say("ird: cannot listen");
		return 1;
	}

	err[0] = '\0';
	if (apply_mode(intent, err, sizeof(err)))
		say("ird: cannot resume %s: %s", mode_name[intent], err);

	while (running) {
		fd_set rfds;
		int max = srv;

		FD_ZERO(&rfds);
		FD_SET(srv, &rfds);

		if (scout_pipe >= 0) {
			FD_SET(scout_pipe, &rfds);
			if (scout_pipe > max)
				max = scout_pipe;
		}

		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i] < 0)
				continue;
			FD_SET(clients[i], &rfds);
			if (clients[i] > max)
				max = clients[i];
		}

		if (select(max + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (FD_ISSET(srv, &rfds)) {
			int fd = accept(srv, NULL, NULL);

			for (i = 0; i < MAX_CLIENTS && fd >= 0; i++) {
				if (clients[i] >= 0)
					continue;
				clients[i] = fd;
				subscribed[i] = 0;
				fd = -1;
			}
			if (fd >= 0)
				close(fd);
		}

		if (scout_pipe >= 0 && FD_ISSET(scout_pipe, &rfds)) {
			char buf[IRD_LINE_MAX];
			ssize_t got = read(scout_pipe, buf, sizeof(buf) - 1);

			if (got > 0) {
				buf[got] = '\0';
				broadcast(buf);
			} else {
				scout_stop();
			}
		}

		for (i = 0; i < MAX_CLIENTS; i++) {
			char buf[IRD_LINE_MAX];
			ssize_t got;
			char *nl;

			if (clients[i] < 0 || !FD_ISSET(clients[i], &rfds))
				continue;

			got = read(clients[i], buf, sizeof(buf) - 1);
			if (got <= 0) {
				close(clients[i]);
				clients[i] = -1;
				continue;
			}

			buf[got] = '\0';
			nl = strchr(buf, '\n');
			if (nl)
				*nl = '\0';

			handle(clients[i], i, buf);
		}
	}

	scout_stop();
	unlink(ird_sock_path());

	return 0;
}
