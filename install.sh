#!/bin/zsh
# install.sh — Bluetooth tethering for this Mac, through your phone.
#
#   ./install.sh           check, build, test with your phone, then install
#                          (asks for your password once, and for Bluetooth permission once)
#   ./install.sh --try     check, build and run the one-minute test only; installs nothing
#   ./install.sh --check   check this Mac and list its paired phones; changes nothing
#   ./install.sh --phone AA:BB:CC:DD:EE:FF   choose the phone when several are paired
#
# Running it again is safe: it updates an existing installation and keeps its pairing.

set -u
HERE="${0:A:h}"
TOOL="$HERE/bt-tether"
STATE="$HOME/Library/Application Support/bt-tether"
CONFIG="$STATE/config"
APP_TOOL="$STATE/app/bt-tether"
AGENT_LABEL="com.anshu.bt-tether.auto"

MODE=install
PHONE_ARG=""
while (( $# )); do
    case "$1" in
        --try)   MODE=try ;;
        --check) MODE=check ;;
        --phone) PHONE_ARG="${2:-}"; shift ;;
        -h|--help) sed -n '2,12p' "$0" | sed -E 's/^# ?//'; exit 0 ;;
        *) print -u2 "unknown option: $1 (see ./install.sh --help)"; exit 2 ;;
    esac
    shift
done
INTERACTIVE=0
[[ -t 0 && -t 1 ]] && INTERACTIVE=1

step() { print; print -r -- "==> $*"; }
stop_here() { print; print -r -- "STOPPED: $*"; exit 1; }
wait_enter() { (( INTERACTIVE )) && read -r "?$1 " _; }

# ------------------------------------------------------------------------------------------
step "Checking this Mac"
"$TOOL" doctor
DOCTOR=$?
PHONES=("${(@f)$("$TOOL" _phones)}")
[[ -z "${PHONES[1]:-}" ]] && PHONES=()
if [[ "$MODE" == check ]]; then
    (( ${#PHONES} )) && { print "Paired phones:"; print -rl -- "${PHONES[@]}" | sed 's/^/  /'; }
    exit $DOCTOR
fi
(( DOCTOR == 0 )) || stop_here "fix the items marked FAIL above, then run ./install.sh again. Nothing was changed."

# ------------------------------------------------------------------------------------------
step "Building (first time: downloads BTstack, about a minute)"
zsh "$HERE/scripts/build.sh" || stop_here "the build failed (details above). Nothing was installed."

# ------------------------------------------------------------------------------------------
step "Choosing the phone"
OLD_PHONE="" OLD_FACTORY="" OLD_HOST="" OLD_TAP=""
if [[ -f "$CONFIG" ]]; then
    OLD_PHONE=$(zsh -c "source '$CONFIG'; print -r -- \$PHONE")
    OLD_FACTORY=$(zsh -c "source '$CONFIG'; print -r -- \$FACTORY")
    OLD_HOST=$(zsh -c "source '$CONFIG'; print -r -- \$HOST_IF")
    OLD_TAP=$(zsh -c "source '$CONFIG'; print -r -- \$TAP_IF")
fi
CHOSEN=""
if [[ -n "$PHONE_ARG" ]]; then
    CHOSEN=$(print -rl -- "${PHONES[@]}" | awk -F'\t' -v a="${(U)PHONE_ARG//-/:}" 'toupper($1)==a' | head -1)
    [[ -n "$CHOSEN" ]] || stop_here "$PHONE_ARG is not a phone paired with this Mac"
elif [[ -n "$OLD_PHONE" ]] && print -rl -- "${PHONES[@]}" | grep -q -i "^$OLD_PHONE"$'\t'; then
    CHOSEN=$(print -rl -- "${PHONES[@]}" | grep -i "^$OLD_PHONE"$'\t' | head -1)
elif (( ${#PHONES} == 1 )); then
    CHOSEN="${PHONES[1]}"
elif (( INTERACTIVE )); then
    local_index=1
    for entry in "${PHONES[@]}"; do print -r -- "  $local_index) ${entry#*$'\t'} (${entry%%$'\t'*})"; (( local_index += 1 )); done
    read -r "?Which phone? (number) " pick
    [[ "$pick" == <-> && pick -ge 1 && pick -le ${#PHONES} ]] || stop_here "no phone chosen"
    CHOSEN="${PHONES[$pick]}"
else
    print -rl -- "${PHONES[@]}" | sed 's/^/  /'
    stop_here "several phones are paired; run again with --phone ADDRESS"
fi
PHONE="${CHOSEN%%$'\t'*}"
PHONE_NAME="${CHOSEN#*$'\t'}"
print -r -- "  phone: $PHONE_NAME ($PHONE)"

# This Mac's own Bluetooth address, and a separate one used only by the tether: the same
# address with the "locally administered" bit set and the last byte moved by two.
FACTORY="${OLD_FACTORY:-$(/usr/sbin/system_profiler SPBluetoothDataType 2>/dev/null |
    awk '/Bluetooth Controller:/{f=1} f && $1=="Address:" {print $2; exit}')}"
[[ "$FACTORY" == ??:??:??:??:??:?? ]] || stop_here "could not read this Mac's Bluetooth address"
octets=(${(s.:.)FACTORY})
TETHER=$(printf '%02X:%s:%s:%s:%s:%02X' $(( 16#${octets[1]} | 2 )) "${octets[2]}" "${octets[3]}" \
    "${octets[4]}" "${octets[5]}" $(( (16#${octets[6]} + 2) & 255 )))

# A free pair of virtual Ethernet ports (or the pair this tool already owns).
HOST_IF="$OLD_HOST" TAP_IF="$OLD_TAP"
if [[ -z "$HOST_IF" ]]; then
    for n in 0 2 4 6 8 10 12 14; do
        mac=$(/sbin/ifconfig "feth$n" 2>/dev/null | awk '/ether/{print toupper($2)}')
        if [[ "$mac" == "$TETHER" ]] ||
           { ! /sbin/ifconfig "feth$n" >/dev/null 2>&1 && ! /sbin/ifconfig "feth$(( n + 1 ))" >/dev/null 2>&1; }; then
            HOST_IF="feth$n"; TAP_IF="feth$(( n + 1 ))"; break
        fi
    done
    [[ -n "$HOST_IF" ]] || stop_here "no free virtual network port pair (feth0 to feth15 are all in use)"
fi
WIFI_IF=$(/usr/sbin/networksetup -listallhardwareports 2>/dev/null | awk '/Hardware Port: Wi-Fi/{getline; print $2; exit}')

mkdir -p "$STATE"
{
    print -r -- "# Written by install.sh on $(date '+%F %T'). Run install.sh again to change it."
    print -r -- "PHONE=${(qq)PHONE}"
    print -r -- "PHONE_NAME=${(qq)PHONE_NAME}"
    print -r -- "FACTORY=${(qq)FACTORY}"
    print -r -- "TETHER=${(qq)TETHER}"
    print -r -- "HOST_IF=${(qq)HOST_IF}"
    print -r -- "TAP_IF=${(qq)TAP_IF}"
    print -r -- "WIFI_IF=${(qq)WIFI_IF}"
} >"$CONFIG.new" && mv "$CONFIG.new" "$CONFIG"

# ------------------------------------------------------------------------------------------
step "Checking that the phone answers and offers Bluetooth tethering"
out=$("$HERE/build/tap" "${PHONE//:/-}" --nap 2>&1)
case "$out" in
    *nap=1*) print "  $PHONE_NAME answers and offers Bluetooth tethering" ;;
    *nap=0*) stop_here "$PHONE_NAME answers but does not offer Bluetooth tethering. Nothing was installed." ;;
    *)
        print "  $PHONE_NAME did not answer over Bluetooth. Keep it nearby, unlocked, with Bluetooth on."
        if (( INTERACTIVE )); then
            read -r "?Redo this Mac's pairing with the phone now? [y/N] " yes
            [[ "$yes" == [yY]* ]] || stop_here "run ./install.sh again when the phone is nearby. Nothing was installed."
            "$TOOL" repair-pairing || stop_here "the phone still does not answer. Nothing was installed."
        else
            stop_here "keep the phone nearby and run again; if it keeps failing run: $TOOL repair-pairing"
        fi
        ;;
esac

# ------------------------------------------------------------------------------------------
# An installation that is already running steps aside while this one is checked and installed.
if [[ -x "$APP_TOOL" ]]; then
    "$APP_TOOL" status 2>/dev/null | grep -q "tethering: ON" && "$APP_TOOL" stop
    /bin/launchctl bootout "gui/$(id -u)/$AGENT_LABEL" >/dev/null 2>&1 || true
fi

step "One-minute test with the phone"
print "  On the phone: switch on Bluetooth tethering (Samsung: Settings > Connections >"
print "  Mobile Hotspot and Tethering > Bluetooth tethering) and keep it unlocked."
print "  Bluetooth devices on this Mac pause for about a minute, then come back."
print "  If the phone asks to pair with \"Mac Tether\", tap Pair."
wait_enter "Press Enter when ready."
if ! "$TOOL" test; then
    [[ -f "$STATE/Bluetooth Tether.app/Contents/Info.plist" ]] &&
        /bin/launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/$AGENT_LABEL.plist" >/dev/null 2>&1
    stop_here "the test did not pass (reason above). Fix it and run ./install.sh again."
fi
if [[ "$MODE" == try ]]; then
    [[ -f "$STATE/Bluetooth Tether.app/Contents/Info.plist" ]] &&
        /bin/launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/$AGENT_LABEL.plist" >/dev/null 2>&1
    print; print "Your Mac and phone work together. Run ./install.sh to install it for good."
    exit 0
fi

# ------------------------------------------------------------------------------------------
step "Installing"
"$TOOL" _update-app || stop_here "could not copy the program into place"
"$APP_TOOL" _install-root || stop_here "the network helper was not installed; run ./install.sh again"
"$APP_TOOL" _install-login || stop_here "the login item did not start; run ./install.sh again"

step "Done"
"$APP_TOOL" status
cat <<'DONE'

From now on nothing needs doing. When this Mac has no other network and the phone is nearby,
it goes online through the phone over Bluetooth by itself (a hotspot icon appears in the menu
bar), and when Wi-Fi is back it switches back and your Bluetooth devices return.

While tethering is on, other Bluetooth devices on this Mac pause and the Bluetooth switch
shows off: the radio is busy carrying your internet. Commands, if you ever want them:
  bt-tether status | only | wifi | stop | auto off | doctor | uninstall
DONE
