#!/bin/zsh
# get.sh — downloads Bluetooth Tether and starts its guided setup.
#
# Paste this one line into Terminal and press Return:
#
#   /bin/zsh -c "$(curl -fsSL https://raw.githubusercontent.com/anshukg/mac-bluetooth-tether/main/get.sh)"
#
# Running the same line again updates Bluetooth Tether to the latest version.
# Options for the installer go after the line, preceded by "":  ... get.sh)" "" --try

set -u
REPO_URL="${BT_TETHER_REPO:-https://github.com/anshukg/mac-bluetooth-tether.git}"
DEFAULT_DEST="$HOME/Library/Application Support/bt-tether-source"
DEST="${BT_TETHER_SOURCE:-$DEFAULT_DEST}"

print
print -r -- "Bluetooth Tether: getting ready"

if [[ "$(uname -m)" != arm64 ]]; then
    print -r -- "Sorry: this Mac has an Intel processor. Bluetooth Tether only works on Macs with"
    print -r -- "Apple silicon (M1 or later). Nothing was changed."
    exit 1
fi

tools_ready() {
    xcode-select -p >/dev/null 2>&1 && xcrun --find git >/dev/null 2>&1 && xcrun --find clang >/dev/null 2>&1
}
if ! tools_ready; then
    print
    print -r -- "First, Apple's free command line tools are needed."
    print -r -- "A window opens now: click Install, then Agree. It takes about 5 to 15 minutes."
    print -r -- "Leave this window open; setup continues by itself when they are installed."
    xcode-select --install >/dev/null 2>&1
    started=$SECONDS
    print -n "  waiting for the command line tools"
    until tools_ready; do
        if (( SECONDS - started > 3600 )); then
            print
            print -r -- "The command line tools did not finish installing within an hour."
            print -r -- "Install them with:  xcode-select --install   then paste the line again."
            exit 1
        fi
        print -n "."
        sleep 5
    done
    print " done"
fi

print -r -- "  downloading the latest version"
fresh_clone() {
    rm -rf "$DEST.new"
    mkdir -p "${DEST:h}"
    git clone -q "$REPO_URL" "$DEST.new" || return 1
    rm -rf "$DEST"
    mv "$DEST.new" "$DEST"
}
if [[ -d "$DEST/.git" ]]; then
    if ! git -C "$DEST" pull -q --ff-only 2>/dev/null; then
        [[ "$DEST" == "$DEFAULT_DEST" ]] && fresh_clone
    fi
else
    [[ -e "$DEST" && "$DEST" != "$DEFAULT_DEST" ]] && {
        print -r -- "$DEST exists and is not a Bluetooth Tether download; nothing was changed."; exit 1; }
    fresh_clone || {
        print -r -- "The download failed. Check the internet connection, then paste the line again."
        exit 1
    }
fi

# The installer asks questions, so it needs the keyboard even when this script was piped in.
if [[ -t 0 ]]; then
    exec /bin/zsh "$DEST/install.sh" "$@"
elif { : </dev/tty; } 2>/dev/null; then
    exec /bin/zsh "$DEST/install.sh" "$@" </dev/tty
else
    exec /bin/zsh "$DEST/install.sh" "$@"
fi
