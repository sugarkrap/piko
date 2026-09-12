#!/bin/sh

WIFI_SSID="D6F7"
STATIC_IP="10.208.47.22"
STATIC_MASK="255.255.255.0"
STATIC_GW="10.208.47.194"
STATIC_DNS="8.8.8.8"
NTP_SYNC="yes"
DHCP_PIDFILE="/var/run/udhcpc.wlan0.pid"

ifconfig wlan0 up 2>/dev/null

iwconfig wlan0 mode managed 2>/dev/null
iwconfig wlan0 key off 2>/dev/null
iwconfig wlan0 power off 2>/dev/null
iwconfig wlan0 essid "$WIFI_SSID" 2>/dev/null

sleep 2

if test -f "$DHCP_PIDFILE"; then
	read -r previous_pid < "$DHCP_PIDFILE"
	if test -n "$previous_pid" && test -r "/proc/$previous_pid/comm"; then
		read -r previous_name < "/proc/$previous_pid/comm"
		if test "$previous_name" = udhcpc; then
			kill "$previous_pid"
			waited=0
			while test -d "/proc/$previous_pid" && test "$waited" -lt 5; do
				sleep 1
				waited=$((waited + 1))
			done
		fi
	fi
	rm -f "$DHCP_PIDFILE"
fi

if udhcpc -i wlan0 -n -T 3 -t 5 -p "$DHCP_PIDFILE" 2>/dev/null; then
	echo "wifi-up: DHCP lease obtained on wlan0"
else
	echo "wifi-up: DHCP failed/timed out, falling back to static $STATIC_IP"
	ifconfig wlan0 "$STATIC_IP" netmask "$STATIC_MASK" up
	if test -n "$STATIC_GW"; then
		route add default gw "$STATIC_GW" 2>/dev/null
	fi
	if test -n "$STATIC_DNS"; then
		echo "nameserver $STATIC_DNS" > /etc/resolv.conf
	fi
fi

if test "$NTP_SYNC" = "yes" -a -x /usr/sbin/ntpsync; then
	/usr/sbin/ntpsync -q > /dev/null 2>&1 &
fi
