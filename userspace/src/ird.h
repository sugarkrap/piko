#ifndef PIKO_IRD_H
#define PIKO_IRD_H

#define IRD_SOCK_DEFAULT	"/var/run/ird.sock"
#define IRD_CONF_DEFAULT	"/etc/ird.conf"
#define IRD_LIRC_DEFAULT	"/dev/lirc0"
#define IRD_DEVNAME_DEFAULT	"/proc/sys/net/irda/devname"
#define IRD_NET_DEFAULT		"/sys/class/net"

#define IRD_IFACE		"irda0"
#define IRD_LINE_MAX		512

#ifndef AF_IRDA
#define AF_IRDA			23
#endif

#define IRD_SOL_IRLMP		266
#define IRD_ENUMDEVICES		1
#define IRD_WAITDEVICE		11

struct ird_device_info {
	unsigned int	saddr;
	unsigned int	daddr;
	char		info[22];
	unsigned char	charset;
	unsigned char	hints[2];
};

struct ird_device_list {
	unsigned int		len;
	struct ird_device_info	dev[1];
};

enum ird_mode {
	IRD_OFF,
	IRD_IRDA,
	IRD_BLASTER,
};

const char *ird_sock_path(void);

#endif
