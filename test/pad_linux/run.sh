#!/bin/sh
# PadHook.h on 32-bit Linux, where Windows exes cannot run. windows.h here is a minimal stand-in: it
# declares what PadHook.h names and defines only what the tests call. Needs g++-multilib (g++ -m32).
#   pad_logic     the mask rules, the heartbeat, route 0's thunk bytes
#   pad_chord     the chord step
#   pad_wrapper   route 1: the signature over two client builds, then the client's own wrapper bytes run
#                 through the real patch, thunk, adapter and dispatcher, with the stack checked on every
#                 call; and route 0's thunk in the client's slot, which must crash in a child process
#   pad_dinput    (rev 4) the DirectInput press rules on both reads, the chord and its taps given back as
#                 events, the sticks and the POV, the lost-release rule, the dispatcher on a bus, and who
#                 drives the DirectInput dispatch
#   pad_disites   (rev 4) the DirectInput signatures over the client's own bytes and every refusal, a scan of
#                 the whole saved image when it is at hand, then those bytes executed through the real patch,
#                 thunks and dispatcher, with the stack, the registers, the flags and eax after the cmp, and
#                 the client's store checked on every call; the patch put in while a thread is inside each
#                 read; and layout_padhook.txt against the header
# objdump then disassembles route 0's thunk, route 1's patched wrapper, thunk and adapter, and the patched
# DirectInput sites and their thunks.
# The saved image is NUI_FFXI_IMAGE, or else the one in toboshii/ffxi-private when it sits next to the nui
# checkout. Without it the image scan is skipped.
# --self-test rebuilds each test once per fault it must catch and requires each build to fail.
#   sh test/pad_linux/run.sh [--self-test]
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/nui_pad_linux"
mkdir -p "$OUT"
FLAGS="-m32 -std=c++17 -O2 -Wall -Wextra -I. -I../../src"
g++ $FLAGS pad_logic.cpp -o "$OUT/pad_logic"
g++ $FLAGS pad_chord.cpp -o "$OUT/pad_chord"
g++ $FLAGS pad_wrapper.cpp -o "$OUT/pad_wrapper"
# pad_dinput also reaches padhook_join, whose status lines gcc thinks may be cut short (they may: by design)
DIFLAGS="$FLAGS -no-pie -Wno-format-truncation"
g++ $DIFLAGS pad_dinput.cpp -o "$OUT/pad_dinput"
g++ $DIFLAGS pad_disites.cpp -o "$OUT/pad_disites"
LAYOUT="$(pwd)/../layout_padhook.txt"
IMAGE="${NUI_FFXI_IMAGE:-}"
[ -z "$IMAGE" ] && [ -f ../../../ffxi-private/dump/FFXiMain_20261102.bin ] && IMAGE="$(cd ../../../ffxi-private/dump && pwd)/FFXiMain_20261102.bin"
(cd "$OUT" && ./pad_logic && ./pad_chord && ./pad_wrapper && ./pad_dinput && ./pad_disites "$LAYOUT" "$IMAGE")
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
read DI_A DI_B DI_TS DI_TE < "$OUT/di_vma.txt"
echo "-- DirectInput: the state read, patched (from its last instructions)"
dis di_site_a.bin "$DI_A" | tail -n 5
echo "-- DirectInput: the event read's success branch, patched"
dis di_site_b.bin "$DI_B" | awk '/call/ { last = NR } { line[NR] = $0 } END { for (i = last - 2; i <= last; ++i) print line[i] }'
echo "-- DirectInput: the state read's thunk"
dis di_thunk_state.bin "$DI_TS"
echo "-- DirectInput: the event read's thunk"
dis di_thunk_events.bin "$DI_TE"

if [ "$1" = "--self-test" ]; then
    for fault in OLD_THUNK FIRST_MATCH; do
        g++ $FLAGS "-DPADHOOK_FAULT_$fault" pad_wrapper.cpp -o "$OUT/pad_wrapper_fault"
        if (cd "$OUT" && ./pad_wrapper_fault >/dev/null 2>&1) 2>/dev/null; then
            echo "self-test: PADHOOK_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: PADHOOK_FAULT_$fault caught"
    done
    # rev 4: each fault, the test that must fail
    for case in DI_NO_CHORD:pad_dinput DI_TAKE_HELD:pad_dinput DI_LEAK_HELD:pad_dinput DI_NO_TAP:pad_dinput \
                DI_NO_CENTRE:pad_dinput DI_NO_EVENTS:pad_dinput DI_NO_DRAIN:pad_dinput DI_NO_PRIME:pad_dinput \
                DI_FIRST_MATCH:pad_disites DI_NO_CMP:pad_disites DI_NO_STORE:pad_disites; do
        fault=${case%%:*}; test=${case#*:}
        g++ $DIFLAGS -Wno-unused-variable -Wno-unused-but-set-variable "-DPADHOOK_FAULT_$fault" "$test.cpp" -o "$OUT/pad_di_fault"
        if (cd "$OUT" && ./pad_di_fault "$LAYOUT" "$IMAGE" >/dev/null 2>&1) 2>/dev/null; then
            echo "self-test: PADHOOK_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: PADHOOK_FAULT_$fault caught"
    done
fi
