#!/bin/sh
# One-time setup for the Wi-Fi window: let wpa_cli -- and therefore the window
# -- talk to the running wpa_supplicant.
#
# wpa_supplicant owns the radio and runs as root.  Everything that changes a
# Wi-Fi setting is asked of it over a unix socket in /var/run/wpa_supplicant,
# and access to that socket is a file permission, not a privilege.  FreeBSD's
# rc.conf way of starting wpa_supplicant (ifconfig_wlan0="... WPA ...") does
# not configure that socket, so out of the box nothing but root can change the
# wireless configuration and the Wi-Fi window would be read-only.
#
# This appends two settings to /etc/wpa_supplicant.conf and reloads the daemon.
# Run it once, as root:
#
#     sudo sh enable-control-interface.sh
#
# Afterwards the socket is group-owned by wheel, so anyone in that group can
# join a network from the desktop without a password.  Edit the group name
# below if that is not what you want on a shared machine.
#
# update_config=1 is the second setting and a different kind of permission: it
# is what allows wpa_supplicant to write the file at all.  Without it the
# daemon answers save_config with FAIL -- "CTRL_IFACE: SAVE_CONFIG - Failed to
# update configuration" -- so a network joined from the desktop works until the
# next boot and is then gone, and the window can only report that it could not
# save.  With it, the network you picked is the network the machine comes back
# on.

set -e

CONF=${WPA_CONF:-/etc/wpa_supplicant.conf}
GROUP=${WPA_GROUP:-wheel}
SOCKET_DIR=/var/run/wpa_supplicant

if [ ! -f "$CONF" ]; then
    echo "$CONF does not exist; is wpa_supplicant in use on this machine?" >&2
    exit 1
fi

# One backup, before the first change, whichever change that turns out to be.
changed=no
backup_once() {
    if [ "$changed" = no ]; then
        cp "$CONF" "$CONF.bak.$$"
        echo "backup of the previous file: $CONF.bak.$$"
        changed=yes
    fi
}

if grep -q '^[[:space:]]*ctrl_interface=' "$CONF"; then
    echo "$CONF already sets ctrl_interface"
else
    backup_once
    cat >> "$CONF" <<EOF

# Added by PolluxOS so the desktop Wi-Fi window can change networks without
# root: it asks over this socket, and the group ownership is what makes asking
# possible for a user who is in that group.
ctrl_interface=$SOCKET_DIR
ctrl_interface_group=$GROUP
EOF
    echo "added ctrl_interface to $CONF"
fi

if grep -q '^[[:space:]]*update_config=1' "$CONF"; then
    echo "$CONF already sets update_config=1"
else
    backup_once
    cat >> "$CONF" <<EOF

# Added by PolluxOS: without this, wpa_supplicant refuses to write its own
# configuration, so a network joined from the desktop is forgotten at the next
# boot.
update_config=1
EOF
    echo "added update_config=1 to $CONF"
fi

# A socket, not the pid file that sits in the same directory.
have_socket() {
    for candidate in "$SOCKET_DIR"/wlan*; do
        if [ -S "$candidate" ]; then
            return 0
        fi
    done
    return 1
}

# A reload is enough if the daemon picks the new setting up, and it does not
# interrupt an existing connection -- which matters when the machine you are
# working on is reached over the network you are about to touch.
PID=$(cat "$SOCKET_DIR"/wlan*.pid 2>/dev/null | head -1 || true)
if [ -n "$PID" ]; then
    kill -HUP "$PID" 2>/dev/null || true
    sleep 1
fi

if have_socket; then
    echo "control socket is up:"
    ls -l "$SOCKET_DIR" | sed 's/^/    /'
    echo "the Wi-Fi window can now join networks, and they will be remembered."
    exit 0
fi

echo "the reload did not create a socket; restarting wpa_supplicant"
echo "(this drops the wireless link for a few seconds, and the address with it)"
IFACE=$(basename "$(ls "$SOCKET_DIR"/wlan*.pid 2>/dev/null | head -1)" .pid)
service wpa_supplicant restart || pkill -TERM wpa_supplicant || true
sleep 3
# The restart leaves the interface associated but with no address: the rc.conf
# path (ifconfig_wlan0="up scan WPA DHCP") runs DHCP, and restarting the
# supplicant does not.  Without this the machine comes back on the network
# only after a reboot -- which is exactly what happened the first time this
# script was run over ssh.
if [ -n "$IFACE" ] && ! ifconfig "$IFACE" 2>/dev/null | grep -q "inet "; then
    echo "renewing the address on $IFACE"
    dhclient "$IFACE" || true
fi
if have_socket; then
    echo "control socket is up:"
    ls -l "$SOCKET_DIR" | sed 's/^/    /'
    exit 0
fi

echo "no control socket appeared; check /var/log/messages and $CONF" >&2
exit 1
