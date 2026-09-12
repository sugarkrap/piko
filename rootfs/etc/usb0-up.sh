#!/bin/sh

USB_IP="192.168.129.201"
USB_MASK="255.255.255.0"
DHCPD_CONF="/etc/udhcpd-usb0.conf"
DHCPD_PIDFILE="/var/run/udhcpd.usb0.pid"
DHCPD_LEASES="/var/run/udhcpd.usb0.leases"

ifconfig usb0 "$USB_IP" netmask "$USB_MASK" up

if test -f "$DHCPD_PIDFILE"; then
	read -r previous_pid < "$DHCPD_PIDFILE"
	if test -n "$previous_pid" && test -r "/proc/$previous_pid/comm"; then
		read -r previous_name < "/proc/$previous_pid/comm"
		if test "$previous_name" = udhcpd; then
			kill "$previous_pid"
			waited=0
			while test -d "/proc/$previous_pid" && test "$waited" -lt 5; do
				sleep 1
				waited=$((waited + 1))
			done
		fi
	fi
	rm -f "$DHCPD_PIDFILE"
fi

rm -f "$DHCPD_LEASES"
udhcpd "$DHCPD_CONF"
