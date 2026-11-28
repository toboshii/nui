#!/bin/sh
# KeyBinds.h's pure part on 32-bit Linux. Needs g++-multilib (g++ -m32).
#   binds_scan   the signatures over the client's own bytes, every refusal, the places, and the copy. Given the
#                saved image of the 2026-11-02 client, it also scans that whole image and checks what it copies.
# --self-test rebuilds the test once per KEYBINDS_FAULT_* and requires each build to fail.
#   sh test/binds_linux/run.sh [--self-test] [image]
# The image defaults to ../ffxi-private/dump/FFXiMain_20261102.bin next to this checkout, when it is there.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/nui_binds_linux"
mkdir -p "$OUT"
FLAGS="-m32 -no-pie -std=c++17 -O2 -Wall -Wextra -I../../src"
SELF=""
if [ "$1" = "--self-test" ]; then SELF=1; shift; fi
IMAGE="${1:-../../../ffxi-private/dump/FFXiMain_20261102.bin}"
[ -f "$IMAGE" ] || IMAGE=""
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
