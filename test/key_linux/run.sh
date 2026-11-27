#!/bin/sh
# KeyHook.h on 32-bit Linux, where Windows exes cannot run. It uses the stand-in windows.h of test/pad_linux. Needs
# g++-multilib (g++ -m32).
#   key_logic   the press rules on both reads, the lost-release rule, the heartbeat, the dispatcher
#   key_sites   the signatures over the client's own bytes and every refusal, then those bytes executed: a stand-in
#               input update with both read sites byte for byte, through the real patch, thunks and dispatcher,
#               with the stack, the registers, and the flags and eax after each cmp checked on every call; the
#               patch put in while a thread is inside the read; and layout_keyhook.txt against the header
# objdump then disassembles the patched sites and both thunks.
# --self-test rebuilds the tests once per KEYHOOK_FAULT_* and requires each build to fail.
#   sh test/key_linux/run.sh [--self-test]
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/nui_key_linux"
mkdir -p "$OUT"
FLAGS="-m32 -no-pie -std=c++17 -O2 -Wall -Wextra -I../pad_linux -I../../src"
LAYOUT="$(pwd)/../layout_keyhook.txt"
g++ $FLAGS key_logic.cpp -o "$OUT/key_logic"
g++ $FLAGS key_sites.cpp -o "$OUT/key_sites"
(cd "$OUT" && ./key_logic && ./key_sites "$LAYOUT")
dis() { objdump -D -b binary -mi386 --adjust-vma="$2" "$OUT/$1" | sed -n '/<.data>:/,$p' | tail -n +2; }
read SITE_A SITE_B THUNK_S THUNK_E < "$OUT/vma.txt"
echo "-- the state read, patched"
dis site_a.bin "$SITE_A" | head -n 16
echo "-- the event read, patched"
dis site_b.bin "$SITE_B" | head -n 16
echo "-- the state read's thunk"
dis thunk_state.bin "$THUNK_S"
echo "-- the event read's thunk"
dis thunk_events.bin "$THUNK_E"

if [ "$1" = "--self-test" ]; then
    # each fault: the define, the test that must fail
    for case in NO_HEARTBEAT:key_logic TAKE_HELD:key_logic LEAK_HELD:key_logic NO_EVENTS:key_logic NO_DRAIN:key_logic NO_PRIME:key_logic \
                NO_CMP:key_sites FIRST_MATCH:key_sites; do
        fault=${case%%:*}; test=${case#*:}
        g++ $FLAGS "-DKEYHOOK_FAULT_$fault" "$test.cpp" -o "$OUT/key_fault"
        if (cd "$OUT" && ./key_fault "$LAYOUT" >/dev/null 2>&1) 2>/dev/null; then
            echo "self-test: KEYHOOK_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: KEYHOOK_FAULT_$fault caught"
    done
fi
