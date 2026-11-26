#!/bin/sh
# PadHook.h on 32-bit Linux, where Windows exes cannot run. windows.h here is a minimal stand-in: it
# declares what PadHook.h names and defines only what the tests call. Needs g++-multilib (g++ -m32).
#   pad_logic     the mask rules, the heartbeat, route 0's thunk bytes
#   pad_chord     the chord step
#   pad_wrapper   route 1: the signature over two client builds, then the client's own wrapper bytes run
#                 through the real patch, thunk, adapter and dispatcher, with the stack checked on every
#                 call; and route 0's thunk in the client's slot, which must crash in a child process
# objdump then disassembles route 0's thunk, and route 1's patched wrapper, thunk and adapter.
# --self-test rebuilds pad_wrapper once per route 1 fault and requires it to fail.
#   sh test/pad_linux/run.sh [--self-test]
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/nui_pad_linux"
mkdir -p "$OUT"
FLAGS="-m32 -std=c++17 -O2 -Wall -Wextra -I. -I../../src"
g++ $FLAGS pad_logic.cpp -o "$OUT/pad_logic"
g++ $FLAGS pad_chord.cpp -o "$OUT/pad_chord"
g++ $FLAGS pad_wrapper.cpp -o "$OUT/pad_wrapper"
(cd "$OUT" && ./pad_logic && ./pad_chord && ./pad_wrapper)
dis() { objdump -D -b binary -mi386 --adjust-vma="$2" "$OUT/$1" | sed -n '/<.data>:/,$p' | tail -n +2; }
echo "-- route 0's thunk"
dis thunk.bin 0x10000010
read SITE THUNK ADAPTER < "$OUT/vma.txt"
echo "-- route 1: the patched wrapper"
dis wrapper.bin "$SITE"
echo "-- route 1: the thunk"
dis wrap_thunk.bin "$THUNK"
echo "-- route 1: the adapter"
dis adapter.bin "$ADAPTER"

if [ "$1" = "--self-test" ]; then
    for fault in OLD_THUNK FIRST_MATCH; do
        g++ $FLAGS "-DPADHOOK_FAULT_$fault" pad_wrapper.cpp -o "$OUT/pad_wrapper_fault"
        if (cd "$OUT" && ./pad_wrapper_fault >/dev/null 2>&1) 2>/dev/null; then
            echo "self-test: PADHOOK_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: PADHOOK_FAULT_$fault caught"
    done
fi
