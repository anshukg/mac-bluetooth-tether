#!/bin/zsh
# uninstall.sh — removes everything install.sh added (asks for your password once).
#   ./uninstall.sh                  remove everything, including the tether's pairing key
#   ./uninstall.sh --keep-pairing   keep the pairing key and settings for a later reinstall
TOOL="$HOME/Library/Application Support/bt-tether/app/bt-tether"
[[ -x "$TOOL" ]] || TOOL="${0:A:h}/bt-tether"
exec "$TOOL" uninstall "$@"
