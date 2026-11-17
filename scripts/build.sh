#!/bin/zsh
# build.sh — fetches BTstack at the pinned commit, applies the macOS patch, and builds every
# tool into build/. Needs Apple's command line tools (xcode-select --install) and internet
# access the first time. Changes nothing outside this folder.

set -euo pipefail
ROOT="${0:A:h:h}"
source "$ROOT/third_party/btstack.lock"
VENDOR="$ROOT/.vendor/btstack-$BTSTACK_COMMIT"
PATCH="$ROOT/third_party/btstack-macos.patch"
SRC="$ROOT/src"
CHIP="$SRC/chip"
OUT="$ROOT/build"
OBJ="$OUT/obj"
MIN=(-mmacosx-version-min=13.0)

print -r -- "fetching BTstack $BTSTACK_COMMIT"
mkdir -p "$ROOT/.vendor"
if [[ ! -d "$VENDOR/.git" ]]; then
    git clone --quiet --filter=blob:none --no-checkout "$BTSTACK_URL" "$VENDOR"
    git -C "$VENDOR" fetch --quiet --depth 1 origin "$BTSTACK_COMMIT"
    git -C "$VENDOR" checkout --quiet --detach "$BTSTACK_COMMIT"
fi
[[ "$(git -C "$VENDOR" remote get-url origin)" == "$BTSTACK_URL" ]] || {
    print -u2 -- "BTstack copy has the wrong source; delete $VENDOR and run again"; exit 1; }
[[ "$(git -C "$VENDOR" rev-parse HEAD)" == "$BTSTACK_COMMIT" ]] || {
    print -u2 -- "BTstack copy is not at the pinned commit; delete $VENDOR and run again"; exit 1; }
if git -C "$VENDOR" apply --reverse --check "$PATCH" 2>/dev/null; then
    :
elif git -C "$VENDOR" diff --quiet && git -C "$VENDOR" apply --check "$PATCH"; then
    git -C "$VENDOR" apply "$PATCH"
else
    print -u2 -- "BTstack copy has changes that do not match the patch; delete $VENDOR and run again"
    exit 1
fi

print -r -- "compiling"
rm -rf "$OBJ"
mkdir -p "$OBJ"
INCLUDES=(
    -I"$SRC"
    -I"$CHIP"
    -I"$VENDOR/src"
    -I"$VENDOR/src/classic"
    -I"$VENDOR/platform/posix"
    -I"$VENDOR/3rd-party/bluedroid/encoder/include"
    -I"$VENDOR/3rd-party/bluedroid/decoder/include"
    -I"$VENDOR/3rd-party/rijndael"
)
CFLAGS=(-std=c11 -fblocks -O2 -g -Wall -Wextra "${MIN[@]}")
PANHOST_SOURCES=(
    "$SRC/panhost.c"
    "$CHIP/BroadcomAddressTest.c"
    "$CHIP/hci_transport_skywalk.c"
    "$CHIP/SkywalkHCINativeDriver.c"
    "$VENDOR/src/ad_parser.c"
    "$VENDOR/src/btstack_linked_list.c"
    "$VENDOR/src/btstack_memory.c"
    "$VENDOR/src/btstack_memory_pool.c"
    "$VENDOR/src/btstack_run_loop.c"
    "$VENDOR/src/btstack_run_loop_base.c"
    "$VENDOR/src/btstack_tlv.c"
    "$VENDOR/src/btstack_util.c"
    "$VENDOR/src/hci.c"
    "$VENDOR/src/hci_cmd.c"
    "$VENDOR/src/hci_dump.c"
    "$VENDOR/src/l2cap.c"
    "$VENDOR/src/l2cap_signaling.c"
    "$VENDOR/src/classic/bnep.c"
    "$VENDOR/src/classic/btstack_link_key_db_tlv.c"
    "$VENDOR/src/classic/pan.c"
    "$VENDOR/src/classic/sdp_server.c"
    "$VENDOR/src/classic/sdp_util.c"
    "$VENDOR/platform/posix/btstack_run_loop_posix.c"
    "$VENDOR/platform/posix/btstack_tlv_posix.c"
    "$VENDOR/platform/posix/hci_dump_posix_stdout.c"
)
OBJECTS=()
index=0
for source in "${PANHOST_SOURCES[@]}"; do
    object="$OBJ/${index}-${source:t:r}.o"
    clang "${CFLAGS[@]}" "${INCLUDES[@]}" -c "$source" -o "$object"
    OBJECTS+=("$object")
    (( index += 1 ))
done
clang "${OBJECTS[@]}" "${MIN[@]}" -framework CoreFoundation -framework IOKit -o "$OUT/panhost"

clang -std=c11 -O2 -Wall -Wextra "${MIN[@]}" -I"$CHIP" \
    "$CHIP/SkywalkHCINativeDriver.c" "$CHIP/SkywalkHCIAddressProbe.c" \
    -framework CoreFoundation -framework IOKit -o "$OUT/SkywalkHCIAddressProbe"
clang -std=c11 -O2 -Wall -Wextra "${MIN[@]}" "$SRC/BluetoothPowerControl.c" \
    -framework AppKit -framework IOBluetooth -o "$OUT/BluetoothPowerControl"
clang -fobjc-arc -O2 -Wno-deprecated-declarations "${MIN[@]}" "$SRC/tap.m" \
    -framework Foundation -framework IOBluetooth -o "$OUT/tap"
clang -fobjc-arc -O2 -Wno-deprecated-declarations "${MIN[@]}" "$SRC/pair.m" \
    -framework Foundation -framework IOBluetooth -o "$OUT/pair"
clang -O2 -Wall "${MIN[@]}" "$SRC/audioroute.c" \
    -framework CoreAudio -framework CoreFoundation -o "$OUT/audioroute"
clang -fobjc-arc -O2 "${MIN[@]}" "$SRC/tetherbar.m" -framework Cocoa -o "$OUT/tetherbar"
clang -O2 -Wall "${MIN[@]}" "$SRC/launcher.c" -o "$OUT/BluetoothTether"

for tool in panhost SkywalkHCIAddressProbe BluetoothPowerControl tap pair audioroute tetherbar BluetoothTether; do
    codesign --force --sign - --timestamp=none "$OUT/$tool" >/dev/null 2>&1
done
rm -rf "$OBJ"
print -r -- "built: $(cd "$OUT" && print -r -- *(.x))"
