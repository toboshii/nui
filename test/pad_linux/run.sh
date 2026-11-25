#!/bin/sh
# PadHook.h's pure logic (the mask rules, the chord step, the heartbeat, the thunk bytes) on 32-bit Linux,
# where Windows exes cannot run. windows.h here is a minimal stand-in: it declares what PadHook.h names and
# defines only what the pure functions call. Needs g++-multilib (g++ -m32). objdump disassembles the thunk.
#   sh test/pad_linux/run.sh
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/nui_pad_linux"
mkdir -p "$OUT"
g++ -m32 -std=c++17 -Wall -Wextra -I. -I../../src pad_logic.cpp -o "$OUT/pad_logic"
g++ -m32 -std=c++17 -Wall -Wextra -I. -I../../src pad_chord.cpp -o "$OUT/pad_chord"
(cd "$OUT" && ./pad_logic && ./pad_chord)
objdump -D -b binary -mi386 --adjust-vma=0x10000010 "$OUT/thunk.bin" | tail -5
