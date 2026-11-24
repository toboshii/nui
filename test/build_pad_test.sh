#!/bin/sh
# PadHook.h's own tests (32-bit MinGW i686): no game files needed. Outputs go to NUI_TEST_OUT (default:
# a temp folder). On Linux, set NUI_RUN=wine to run the exes under Wine.
#   sh test/build_pad_test.sh [--self-test]
#
#   pad_test.exe main      a stand-in xinput1_3.dll (pad_fake_xinput.cpp) with the real entry's shape: the
#                          patch bytes, pass-through, recording, the mask rules, renumbering, the heartbeat, the chord, a
#                          revision 3 module (pad_mod.cpp) taking the dispatch and handing it back from its
#                          DllMain, and the layout (layout_padhook.txt)
#   pad_test.exe foreign   another tool's hot-patch already there: refused, never retried
#   pad_test_nohot.exe     an entry that is not hot-patchable: refused, bytes untouched
# --self-test rebuilds pad_test once per PADHOOK_FAULT_* and requires it to fail.
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
OUT="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}/pad"
RUN="${NUI_RUN:-}"
NORUN="${NUI_NORUN:-}"          # set: build only (where Windows exes cannot run)
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra -static -static-libgcc -static-libstdc++"
mkdir -p "$OUT"
cp layout_padhook.txt "$OUT/"

"$GXX" $FLAGS -shared pad_fake_xinput.cpp -o "$OUT/pad_fake_xinput.dll" -Wl,--kill-at &
"$GXX" $FLAGS -DFAKE_NOHOT -shared pad_fake_xinput.cpp -o "$OUT/pad_fake_nohot.dll" -Wl,--kill-at &
"$GXX" $FLAGS -DPADHOOK_REV=3 '-DPADMOD_NAME="r3"' '-DPADHOOK_DLL="pad_fake_xinput.dll"' -shared pad_mod.cpp \
    -o "$OUT/pad_mod_r3.dll" &
"$GXX" $FLAGS '-DPADHOOK_DLL="pad_fake_xinput.dll"' pad_test.cpp -o "$OUT/pad_test.exe" &
"$GXX" $FLAGS '-DPADHOOK_DLL="pad_fake_nohot.dll"' pad_test.cpp -o "$OUT/pad_test_nohot.exe" &
wait

if [ -n "$NORUN" ]; then echo "built in $OUT (NUI_NORUN: not run)"; exit 0; fi
cd "$OUT"
echo "== PadHook"
$RUN ./pad_test.exe main layout_padhook.txt
$RUN ./pad_test.exe foreign
$RUN ./pad_test_nohot.exe nohot

if [ "$1" = "--self-test" ]; then
    cd - >/dev/null
    for fault in NO_HEARTBEAT TAKE_HELD LEAK_HELD NO_RENUMBER NO_CHORD; do
        "$GXX" $FLAGS "-DPADHOOK_FAULT_$fault" '-DPADHOOK_DLL="pad_fake_xinput.dll"' pad_test.cpp -o "$OUT/pad_fault.exe"
        if (cd "$OUT" && $RUN ./pad_fault.exe main layout_padhook.txt >/dev/null); then
            echo "self-test: PADHOOK_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: PADHOOK_FAULT_$fault caught"
    done
fi
echo "pad tests passed"
