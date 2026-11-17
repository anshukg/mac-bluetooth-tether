#!/bin/zsh
# net-root.sh — the root-side half of bt-tether. A root-owned copy is installed by
# `bt-tether install-boot` and run by two LaunchDaemons:
#
#   boot  at every startup: creates the virtual port; if the Mac went down while tethering
#         was on, switches Apple's Bluetooth back on.
#   sync  whenever bt-tether changes the link-state file: "up" starts DHCP on the port,
#         "down" clears its address.
#
# Also usable by hand through `bt-tether net-setup` (action "up") and net-teardown ("down").
#
#   net-root.sh boot|up|sync|down <user> <mac> <host-if> <tap-if>

set -u
action="$1"; user="$2"; mac="$3"; host_if="$4"; tap_if="$5"
ROOT_DIR="/Library/Application Support/bt-tether"
LINK_STATE="$ROOT_DIR/link-state"
POWER_TOOL="$ROOT_DIR/BluetoothPowerControl"

# The link-state file is written by the user's bt-tether. Only two exact words are accepted.
link_state() {
    local word=""
    [[ -f "$LINK_STATE" ]] && word=$(/usr/bin/head -c 8 "$LINK_STATE" 2>/dev/null | /usr/bin/tr -cd 'a-z')
    print -r -- "$word"
}

apply_link_state() {
    case "$(link_state)" in
        up)   /usr/sbin/ipconfig set "$host_if" DHCP ;;
        down) /usr/sbin/ipconfig set "$host_if" NONE 2>/dev/null || true ;;
        *)    [[ -f "$LINK_STATE" ]] || /usr/sbin/ipconfig set "$host_if" DHCP ;;   # no boot job: per-restart mode
    esac
}

create_port() {
    /sbin/ifconfig "$host_if" >/dev/null 2>&1 || /sbin/ifconfig "$host_if" create
    /sbin/ifconfig "$tap_if"  >/dev/null 2>&1 || /sbin/ifconfig "$tap_if" create
    /sbin/ifconfig "$host_if" peer "$tap_if" 2>/dev/null || true
    /sbin/ifconfig "$host_if" lladdr "$mac"
    /sbin/ifconfig "$host_if" mtu 1500 up
    /sbin/ifconfig "$tap_if" mtu 1500 up
    # Opening bpfN makes the kernel create bpfN+1, so this leaves spare devices to hand over.
    for index in {0..11}; do
        ( : </dev/bpf$index ) 2>/dev/null || true
    done
    /usr/sbin/chown "$user" /dev/bpf*
}

case "$action" in
    boot)
        create_port
        if [[ "$(link_state)" == "up" ]]; then
            # The Mac restarted or lost power while tethering was on, which leaves Apple's
            # Bluetooth switched off. Put it back and record that tethering is off.
            print -r -- down >"$LINK_STATE"
            if [[ -x "$POWER_TOOL" ]]; then
                for attempt in {1..30}; do
                    "$POWER_TOOL" set 1 --wait-ms 4000 >/dev/null 2>&1 && break
                    sleep 2
                done
            fi
        fi
        apply_link_state
        ;;
    up)
        create_port
        apply_link_state
        ;;
    sync)
        apply_link_state
        ;;
    down)
        /usr/sbin/ipconfig set "$host_if" NONE 2>/dev/null || true
        /sbin/ifconfig "$host_if" destroy 2>/dev/null || true
        /sbin/ifconfig "$tap_if" destroy 2>/dev/null || true
        /usr/sbin/chown root /dev/bpf* 2>/dev/null || true
        ;;
    *)
        print -u2 "usage: net-root.sh boot|up|sync|down <user> <mac> <host-if> <tap-if>"
        exit 2
        ;;
esac
exit 0
