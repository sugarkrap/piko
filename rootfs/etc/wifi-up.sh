#!/bin/sh
# Run by mdev when wlan0 appears (hostap_cs bound to an inserted Prism2
# card). Associates to the configured AP and assigns an address.
#
# DHCP-first as of 2026-07-27: the earlier "DHCP is unreliable" note was
# actually a symptom of the iw_mode-targeting-the-wrong-module bug (see
# repo memory's WiFi flakiness fix entry) -- the runtime Master->Managed
# mode switch it caused was disrupting the DHCP exchange, not udhcpc
# itself. Now that hostap_cs loads directly in managed mode, try a real
# DHCP lease first and only fall back to a static address if a server
# doesn't answer in time -- this is the ONLY remote-access path to the
# device (see AGENTS.md), so wlan0 must never come up unaddressed.

# --- network settings (edit these) ---
WIFI_SSID="D6F7"                # AP to join (open network)
STATIC_IP="10.208.47.22"        # fallback address if DHCP fails (must be free)
STATIC_MASK="255.255.255.0"
STATIC_GW="10.208.47.194"       # AP/router (optional; for off-subnet/internet)
STATIC_DNS="8.8.8.8"
NTP_SYNC="yes"                  # set the clock from the network once online

ifconfig wlan0 up 2>/dev/null

# Associate. For an OPEN network, iwconfig alone is enough (proven on this
# hardware); wpa_supplicant is only needed for WEP/WPA -- to use it instead,
# put the network in /etc/wpa_supplicant/wpa_supplicant.conf and replace the
# two iwconfig lines with:
#   wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf -D wext
iwconfig wlan0 mode managed 2>/dev/null
iwconfig wlan0 key off 2>/dev/null      # fully open (no stale WEP privacy bit)
iwconfig wlan0 power off 2>/dev/null    # disable power-save (drops broadcast/ARP)
iwconfig wlan0 essid "$WIFI_SSID" 2>/dev/null

# Give the association a moment before requesting an address.
sleep 2

# Try DHCP: up to 5 discover attempts, 3s apart (~15s worst case) before
# giving up. On success busybox udhcpc daemonizes itself and keeps
# renewing the lease in the background for as long as wlan0 stays up; on
# failure (-n) it exits non-zero and we fall back to the static config
# below instead of leaving wlan0 addressless.
if udhcpc -i wlan0 -n -T 3 -t 5 2>/dev/null; then
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

# --- time ---
# Best-effort clock sync, deliberately the LAST thing here and backgrounded:
# wlan0 is the only remote-access path to this device (see AGENTS.md), so
# nothing in this section may delay, block or fail the addressing above.
# ntpsync writes the RTC itself on success, so a synced time survives the
# next reboot without any further step.
if test "$NTP_SYNC" = "yes" -a -x /usr/sbin/ntpsync; then
	/usr/sbin/ntpsync -q > /dev/null 2>&1 &
fi
