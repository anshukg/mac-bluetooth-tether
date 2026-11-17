#!/bin/zsh
# install.sh — guided setup of Bluetooth Tether on this Mac.
#
#   ./install.sh           guided setup: checks, builds, tests with your phone, installs
#   ./install.sh --try     everything up to the test with your phone; installs nothing
#   ./install.sh --check   only checks this Mac and lists its phones; changes nothing
#   ./install.sh --phone AA:BB:CC:DD:EE:FF   use this phone when several are paired
#
# Safe to run again at any time: it updates an existing installation and keeps its pairing.
# Every step explains what will happen before it happens, waits for you when something
# needs your hands, and offers to try again when something does not work.

set -u
HERE="${0:A:h}"
TOOL="$HERE/bt-tether"
STATE="$HOME/Library/Application Support/bt-tether"
CONFIG="$STATE/config"
APP_TOOL="$STATE/app/bt-tether"
AGENT_LABEL="com.anshu.bt-tether.auto"
AGENT_PLIST="$HOME/Library/LaunchAgents/$AGENT_LABEL.plist"

MODE=install
PHONE_ARG=""
while (( $# )); do
    case "$1" in
        --try)   MODE=try ;;
        --check) MODE=check ;;
        --phone) PHONE_ARG="${2:-}"; shift ;;
        -h|--help) sed -n '2,11p' "$0" | sed -E 's/^# ?//'; exit 0 ;;
        *) print -u2 "unknown option: $1 (see ./install.sh --help)"; exit 2 ;;
    esac
    shift
done

INTERACTIVE=0
[[ -t 0 && -t 1 ]] && INTERACTIVE=1
if (( INTERACTIVE )); then
    B=$'\e[1m'; G=$'\e[32m'; R=$'\e[31m'; Y=$'\e[33m'; N=$'\e[0m'
else
    B=""; G=""; R=""; Y=""; N=""
fi
TOTAL=5
[[ "$MODE" == try ]] && TOTAL=4

step() { print; print -r -- "${B}Step $1 of $TOTAL: $2${N}"; }
good() { print -r -- "  ${G}✓${N} $*"; }
warn() { print -r -- "  ${Y}!${N} $*"; }
fail() { print -r -- "  ${R}✗${N} $*"; }
say()  { print -r -- "    $*"; }
bye() {
    print
    print -r -- "${B}Setup stopped:${N} $*"
    print -r -- "Nothing else was changed. Run the same command again whenever you are ready."
    exit 1
}
press_return() {   # waits for Return (interactive only)
    (( INTERACTIVE )) || return 0
    print
    read -r "?  ${B}Press Return${N} $1 " _
}
try_again() {      # true when the person wants another try
    (( INTERACTIVE )) || return 1
    local answer
    print
    read -r "?  Press Return to try again, or type q and Return to stop: " answer
    [[ "$answer" != [qQ]* ]]
}
yes_no() {
    (( INTERACTIVE )) || return 1
    local answer
    read -r "?  $1 [y/n] " answer
    [[ "$answer" == [yY]* ]]
}
wait_for() {       # seconds, label, command...: polls every 3 s and prints a dot
    local limit=$1 label=$2 start=$SECONDS
    shift 2
    print -n "    $label"
    while (( SECONDS - start < limit )); do
        if "$@"; then print " done"; return 0; fi
        print -n "."
        sleep 3
    done
    print " gave up after $(( limit / 60 )) minutes"
    return 1
}

# ---- small checks -------------------------------------------------------------------------
bt_field() {
    /usr/sbin/system_profiler SPBluetoothDataType 2>/dev/null |
        awk -v key="$1:" '/Bluetooth Controller:/{f=1} f && $1==key {print $2; exit}'
}
bt_on()       { [[ "$(bt_field State)" == On ]]; }
tools_ready() { xcode-select -p >/dev/null 2>&1 && xcrun --find clang >/dev/null 2>&1 && xcrun --find git >/dev/null 2>&1; }
has_phone()   { [[ -n "$("$TOOL" _phones 2>/dev/null)" ]]; }
default_if()  { /sbin/route -n get default 2>/dev/null | awk '/interface/{print $2}'; }

# ---- check only ---------------------------------------------------------------------------
if [[ "$MODE" == check ]]; then
    "$TOOL" doctor
    result=$?
    phones=$("$TOOL" _phones)
    [[ -n "$phones" ]] && { print "Paired phones:"; print -r -- "$phones" | awk -F'\t' '{print "  " $2 " (" $1 ")"}'; }
    exit $result
fi

# ---- welcome ------------------------------------------------------------------------------
print
print -r -- "${B}Bluetooth Tether setup${N}"
print
print -r -- "This sets up your Mac to go online through your Android phone over Bluetooth,"
print -r -- "by itself, whenever the Mac has no other network."
print
print -r -- "It takes about 5 minutes. Along the way:"
print -r -- "  • your phone asks once to pair with \"Mac Tether\" (you tap Pair)"
if [[ "$MODE" == install ]]; then
    print -r -- "  • macOS asks for your Mac password once"
    print -r -- "  • macOS asks once whether \"Bluetooth Tether\" may use Bluetooth (you click Allow)"
fi
print -r -- "  • macOS may ask whether Terminal may use Bluetooth (you click Allow)"
print
print -r -- "Have your phone with you, unlocked. To stop at any time, press Control and C together;"
print -r -- "nothing breaks, and you can start again whenever you like."
press_return "to begin."

mkdir -p "$STATE"

# An installation that is already running steps aside until this setup finishes.
AGENT_PAUSED=0
FINISHED=0
resume_agent() {
    (( AGENT_PAUSED && ! FINISHED )) && [[ -f "$AGENT_PLIST" ]] &&
        /bin/launchctl bootstrap "gui/$(id -u)" "$AGENT_PLIST" >/dev/null 2>&1
}
trap resume_agent EXIT
if [[ -x "$APP_TOOL" ]]; then
    if "$APP_TOOL" status 2>/dev/null | grep -q "tethering: ON"; then
        say "Bluetooth Tether is tethering right now; switching it off for the setup."
        "$APP_TOOL" stop >/dev/null 2>&1
    fi
    if /bin/launchctl print "gui/$(id -u)/$AGENT_LABEL" >/dev/null 2>&1; then
        /bin/launchctl bootout "gui/$(id -u)/$AGENT_LABEL" >/dev/null 2>&1 && AGENT_PAUSED=1
    fi
fi

# ---- step 1: this Mac ---------------------------------------------------------------------
step 1 "Checking this Mac"

[[ "$(uname -m)" == arm64 ]] ||
    { fail "This Mac has an Intel processor."; bye "Bluetooth Tether only works on Macs with Apple silicon (M1 or later)."; }
good "Apple silicon Mac"

version=$(sw_vers -productVersion)
major=${version%%.*}
if (( major < 13 )); then
    fail "macOS $version"; bye "Bluetooth Tether needs macOS 13 or later. Update macOS in System Settings > General > Software Update."
elif (( major < 26 )); then
    warn "macOS $version: Bluetooth Tether was tested on macOS 26; the test in step 4 tells whether it works here"
else
    good "macOS $version"
fi

chipset=$(bt_field Chipset); transport=$(bt_field Transport)
if [[ "$transport" == PCIe && "$chipset" == BCM_* ]]; then
    good "Bluetooth chip $chipset"
else
    fail "Bluetooth chip ${chipset:-unknown} (${transport:-unknown})"
    bye "This Mac's Bluetooth chip is not one Bluetooth Tether can drive (it needs a Broadcom chip on PCIe)."
fi
if ioreg -r -c AppleConvergedIPCInterface -l 2>/dev/null | grep -q '"ACIPCInterfaceProtocol" = "acl"'; then
    good "direct access to the Bluetooth chip is available"
else
    fail "this Mac does not offer direct access to its Bluetooth chip"
    bye "Bluetooth Tether cannot work on this Mac."
fi

if tools_ready; then
    good "Apple's command line tools are installed"
else
    warn "Apple's free command line tools are needed to build Bluetooth Tether."
    (( INTERACTIVE )) || bye "Install them by running:  xcode-select --install   then run this again."
    say "A window opens asking to install them: click Install, then Agree."
    say "It takes about 5 to 15 minutes. Leave this window open; setup continues by itself."
    press_return "to open that window."
    xcode-select --install >/dev/null 2>&1
    wait_for 3600 "waiting for the command line tools" tools_ready ||
        bye "The command line tools did not finish installing. Install them with:  xcode-select --install"
    good "Apple's command line tools are installed"
fi

if bt_on; then
    good "Bluetooth is on"
else
    warn "Bluetooth is off."
    (( INTERACTIVE )) || bye "Switch Bluetooth on (menu bar or Control Center), then run this again."
    say "Switch it on: click the Bluetooth icon in the menu bar, or open Control Center,"
    say "and turn Bluetooth on."
    wait_for 600 "waiting for Bluetooth" bt_on || bye "Bluetooth stayed off."
    good "Bluetooth is on"
fi

address=$(bt_field Address)
if [[ "$address" == ??:??:??:??:??:?? ]] && (( 16#${address[1,2]} & 2 )); then
    [[ -x "$APP_TOOL" ]] && "$APP_TOOL" stop >/dev/null 2>&1
    address=$(bt_field Address)
    (( 16#${address[1,2]} & 2 )) && bye "Bluetooth is in an unusual state. Restart the Mac, then run this again."
fi
good "everything this Mac needs is in place"

# ---- step 2: build ------------------------------------------------------------------------
step 2 "Building Bluetooth Tether (about a minute)"
say "This downloads BTstack, the open-source Bluetooth software it uses, and compiles it."
until zsh "$HERE/scripts/build.sh" >"$STATE/build.log" 2>&1; do
    fail "The build did not finish. The last lines it printed:"
    tail -4 "$STATE/build.log" | sed 's/^/      /'
    say "The usual cause is no internet connection."
    try_again || bye "The build did not finish (full log: $STATE/build.log)."
done
good "built"

# ---- step 3: the phone --------------------------------------------------------------------
step 3 "Connecting your phone"

if ! has_phone; then
    warn "No phone is paired with this Mac yet. Let's pair it now."
    (( INTERACTIVE )) || bye "Pair your phone in System Settings > Bluetooth, then run this again."
    say "1. On the phone, open Settings > Bluetooth and stay on that screen."
    say "   (Samsung: Settings > Connections > Bluetooth.)"
    say "2. On this Mac, Bluetooth settings open now. Find the phone under"
    say "   \"Nearby Devices\" and click Connect."
    say "3. Both screens show the same number. Tap Pair on the phone,"
    say "   and click Connect or Accept on the Mac."
    /usr/bin/open "x-apple.systempreferences:com.apple.BluetoothSettings" 2>/dev/null
    wait_for 900 "waiting for the phone to be paired" has_phone ||
        bye "No phone was paired. Pair it in System Settings > Bluetooth, then run this again."
fi

PHONES=("${(@f)$("$TOOL" _phones)}")
[[ -z "${PHONES[1]:-}" ]] && PHONES=()
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
    [[ -n "$CHOSEN" ]] || bye "$PHONE_ARG is not a phone paired with this Mac."
elif (( ${#PHONES} == 1 )); then
    CHOSEN="${PHONES[1]}"
elif [[ -n "$OLD_PHONE" ]] && print -rl -- "${PHONES[@]}" | grep -q -i "^$OLD_PHONE"$'\t' && ! (( INTERACTIVE )); then
    CHOSEN=$(print -rl -- "${PHONES[@]}" | grep -i "^$OLD_PHONE"$'\t' | head -1)
elif (( INTERACTIVE )); then
    say "Several phones are paired with this Mac. Which one should Bluetooth Tether use?"
    index=1
    for entry in "${PHONES[@]}"; do
        say "  $index) ${entry#*$'\t'}"
        (( index += 1 ))
    done
    while true; do
        read -r "?    Type its number and press Return: " pick
        [[ "$pick" == <-> ]] && (( pick >= 1 && pick <= ${#PHONES} )) && break
        say "Please type a number from 1 to ${#PHONES}."
    done
    CHOSEN="${PHONES[$pick]}"
else
    print -rl -- "${PHONES[@]}" | sed 's/^/      /'
    bye "Several phones are paired. Run again with --phone followed by the address of the one to use."
fi
PHONE="${CHOSEN%%$'\t'*}"
PHONE_NAME="${CHOSEN#*$'\t'}"
good "phone: $PHONE_NAME"

# This Mac's own Bluetooth address, and a separate one used only by the tether: the same
# address with the "locally administered" bit set and the last byte moved by two.
FACTORY="${OLD_FACTORY:-$(bt_field Address)}"
[[ "$FACTORY" == ??:??:??:??:??:?? ]] || bye "Could not read this Mac's Bluetooth address."
octets=(${(s.:.)FACTORY})
TETHER=$(printf '%02X:%s:%s:%s:%s:%02X' $(( 16#${octets[1]} | 2 )) "${octets[2]}" "${octets[3]}" \
    "${octets[4]}" "${octets[5]}" $(( (16#${octets[6]} + 2) & 255 )))

# A free pair of virtual network ports (or the pair this tool already owns).
HOST_IF="$OLD_HOST" TAP_IF="$OLD_TAP"
if [[ -z "$HOST_IF" ]]; then
    for n in 0 2 4 6 8 10 12 14; do
        mac=$(/sbin/ifconfig "feth$n" 2>/dev/null | awk '/ether/{print toupper($2)}')
        if [[ "$mac" == "$TETHER" ]] ||
           { ! /sbin/ifconfig "feth$n" >/dev/null 2>&1 && ! /sbin/ifconfig "feth$(( n + 1 ))" >/dev/null 2>&1; }; then
            HOST_IF="feth$n"; TAP_IF="feth$(( n + 1 ))"; break
        fi
    done
    [[ -n "$HOST_IF" ]] || bye "No free virtual network port (feth0 to feth15 are all in use)."
fi
WIFI_IF=$(/usr/sbin/networksetup -listallhardwareports 2>/dev/null | awk '/Hardware Port: Wi-Fi/{getline; print $2; exit}')
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

print
say "Now switch on ${B}Bluetooth tethering${N} on the phone:"
say "  Samsung:   Settings > Connections > Mobile Hotspot and Tethering > Bluetooth tethering"
say "  Pixel, Motorola and most others:"
say "             Settings > Network & internet > Hotspot & tethering > Bluetooth tethering"
say "  Any phone: open Settings, search for \"Bluetooth tethering\", and switch it on."
say "Keep the phone unlocked and next to the Mac."
say "If macOS asks whether Terminal may use Bluetooth, click Allow."
press_return "when Bluetooth tethering is on."

misses=0
while true; do
    out=$("$HERE/build/tap" "${PHONE//:/-}" --nap 2>&1)
    case "$out" in
        *nap=1*) good "$PHONE_NAME answers and offers Bluetooth tethering"; break ;;
        *nap=0*)
            fail "$PHONE_NAME answers, but it does not offer Bluetooth tethering."
            say "Switch on Bluetooth tethering on the phone (see above)." ;;
        *)
            (( misses += 1 ))
            fail "$PHONE_NAME did not answer over Bluetooth."
            say "Check that the phone is unlocked, its Bluetooth is on, and it is next to the Mac."
            say "If macOS asked whether Terminal may use Bluetooth, click Allow."
            if (( misses >= 2 )) && yes_no "Redo this Mac's pairing with the phone? It often fixes this."; then
                "$TOOL" repair-pairing
                continue
            fi ;;
    esac
    try_again || bye "The phone did not answer."
done

# ---- step 4: test -------------------------------------------------------------------------
step 4 "Testing with your phone (about a minute)"
say "During the test:"
say "  • Bluetooth headphones, keyboards and mice on this Mac pause for up to a minute."
say "    They come back by themselves. The trackpad and built-in keyboard keep working."
say "  • The first time, the phone asks to pair with \"Mac Tether\": tap ${B}Pair${N}."
say "    If it also asks about contacts or call history, either answer is fine."
say "  • Keep the phone unlocked."
press_return "to start the test."
until "$TOOL" test 2>&1 | sed 's/^\[bt-tether\] /    /'; (( ${pipestatus[1]} == 0 )); do
    try_again || bye "The test did not pass (the reason is above)."
done
good "your Mac and phone work together"

if [[ "$MODE" == try ]]; then
    print
    print -r -- "${B}${G}It works on this Mac.${N} Nothing was installed."
    print -r -- "To install it, run:  ./install.sh"
    exit 0
fi

# ---- step 5: install ----------------------------------------------------------------------
step 5 "Installing"
"$TOOL" _update-app >/dev/null || bye "Could not copy Bluetooth Tether into place."
good "copied into your Library folder"

say "macOS now asks for your Mac password, to install a small network helper."
say "Use the password you log in to this Mac with."
until "$APP_TOOL" _install-root 2>&1 | sed 's/^\[bt-tether\] /    /'; (( ${pipestatus[1]} == 0 )); do
    say "The password box was cancelled or did not work."
    try_again || bye "The network helper was not installed."
done
good "network helper installed"

say "macOS now asks whether \"Bluetooth Tether\" may use Bluetooth: click ${B}Allow${N}."
"$APP_TOOL" _install-login >/dev/null 2>&1
until "$APP_TOOL" _access; do
    warn "Bluetooth Tether does not have Bluetooth access yet."
    (( INTERACTIVE )) || { say "Allow it in System Settings > Privacy & Security > Bluetooth."; break; }
    say "Privacy & Security > Bluetooth opens now. Switch on \"Bluetooth Tether\"."
    /usr/bin/open "x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth" 2>/dev/null
    press_return "when it is switched on."
    /bin/launchctl kickstart -k "gui/$(id -u)/$AGENT_LABEL" >/dev/null 2>&1
    wait_for 30 "checking" "$APP_TOOL" _access && break
    try_again || break
done
"$APP_TOOL" _access && good "Bluetooth Tether is running and starts by itself at every login"
FINISHED=1

# ---- done ---------------------------------------------------------------------------------
print
print -r -- "${B}${G}All set.${N}"
print
print -r -- "From now on there is nothing to do. When this Mac has no other network and the phone"
print -r -- "is nearby, it goes online through the phone by itself, and a hotspot icon appears in"
print -r -- "the menu bar. When Wi-Fi is back, it switches back on its own."
print
print -r -- "While it uses the phone, Bluetooth headphones, keyboards and mice pause and the"
print -r -- "Bluetooth switch shows off. That is normal: they come back when Wi-Fi does."

tether_online() {
    "$APP_TOOL" status 2>/dev/null | grep -q "tethering: ON" &&
        [[ "$(default_if)" == "$HOST_IF" ]]
}
back_on_wifi() {
    "$APP_TOOL" status 2>/dev/null | grep -q "tethering: off" &&
        [[ -n "$(default_if)" && "$(default_if)" != "$HOST_IF" ]] &&
        [[ ! -d "$STATE/radio.lock" ]]
}
if (( INTERACTIVE )) && [[ -n "$WIFI_IF" ]]; then
    print
    if yes_no "Would you like to see it work now? It takes about a minute."; then
        print
        say "1. Switch Wi-Fi off: click the Wi-Fi icon in the menu bar and turn Wi-Fi off."
        say "   (If this Mac also has a network cable plugged in, unplug it too.)"
        if wait_for 180 "waiting for the Mac to go online through the phone" tether_online; then
            code=$(/usr/bin/curl -s -o /dev/null -m 20 -w '%{http_code}' https://www.apple.com/library/test/success.html)
            if [[ "$code" == 200 ]]; then
                good "You are online through your phone over Bluetooth. Look for the hotspot icon in the menu bar."
            else
                warn "Connected through the phone, but a test web page did not load yet."
            fi
            print
            say "2. Now switch Wi-Fi back on."
            if wait_for 180 "waiting for Wi-Fi" back_on_wifi; then
                good "Back on Wi-Fi, and your Bluetooth devices are back."
            else
                warn "It has not switched back yet. Check that Wi-Fi is on and connected."
            fi
        else
            warn "It did not switch over within 3 minutes."
            say "Check that Wi-Fi is off and the phone is unlocked nearby with Bluetooth tethering on."
            say "Typing  bt-tether status  shows what is happening. Remember to switch Wi-Fi back on."
        fi
    fi
fi

print
print -r -- "Useful if you ever need them:"
print -r -- "  bt-tether status       what is happening now"
print -r -- "  bt-tether auto off     pause the automatic switching (bt-tether auto on resumes it)"
print -r -- "  bt-tether doctor       check that everything is in order"
print -r -- "  bt-tether uninstall    remove Bluetooth Tether completely"
print
print -r -- "You can close this window now."
