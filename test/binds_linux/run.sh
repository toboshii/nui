#!/bin/sh
# KeyBinds.h's pure part on 32-bit Linux. Needs g++-multilib (g++ -m32).
#   binds_scan   the signatures over the client's own bytes, every refusal, the places, and the copy. Given the
#                saved image of the 2026-11-02 client, it also scans that whole image and checks what it copies.
# --self-test rebuilds the test once per KEYBINDS_FAULT_* and requires each build to fail.
#   sh test/binds_linux/run.sh [--self-test] [image]
# The image may also come from NUI_FFXI_IMAGE. Without one, the test runs on the bytes it carries.
set -e
SELF=""
if [ "$1" = "--self-test" ]; then SELF=1; shift; fi
IMAGE="${1:-$NUI_FFXI_IMAGE}"
if [ -n "$IMAGE" ]; then
    [ -f "$IMAGE" ] || { echo "no image at $IMAGE" >&2; exit 1; }
    IMAGE="$(cd "$(dirname "$IMAGE")" && pwd)/$(basename "$IMAGE")"
fi
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/nui_binds_linux"
mkdir -p "$OUT"
FLAGS="-m32 -no-pie -std=c++17 -O2 -Wall -Wextra -I../../src"
g++ $FLAGS binds_scan.cpp -o "$OUT/binds_scan"
"$OUT/binds_scan" $IMAGE

if [ -n "$SELF" ]; then
    for fault in FIRST_MATCH NO_AGREE NO_PLACE MOVE_ALL; do
        g++ $FLAGS "-DKEYBINDS_FAULT_$fault" binds_scan.cpp -o "$OUT/binds_fault"
        if "$OUT/binds_fault" $IMAGE >/dev/null 2>&1; then
            echo "self-test: KEYBINDS_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: KEYBINDS_FAULT_$fault caught"
    done
fi
