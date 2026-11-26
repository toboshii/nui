#!/bin/sh
# PadHook.h's own tests (32-bit MinGW i686): no game files needed. Outputs go to NUI_TEST_OUT (default:
# a temp folder). On Linux, set NUI_RUN=wine to run the exes under Wine.
#   sh test/build_pad_test.sh [--self-test]
#
#   pad_test.exe main      route 0 on a stand-in xinput1_3.dll (pad_fake_xinput.cpp) with the real entry's
#                          shape: the patch bytes, pass-through, recording, the mask rules, renumbering, the
#                          heartbeat, the chord, a revision 4 module (pad_mod.cpp) taking the dispatch and
#                          handing it back from its DllMain, and the layout (layout_padhook.txt)
#   pad_test.exe nomatch   a stand-in client whose wrapper the signature does not match: route 0 instead
#   pad_test.exe both      both routes available: route 1 is taken, the export left alone
#   pad_test.exe r2first   a revision 2 module patched route 0 first: revision 3 joins it, no second patch
#   pad_test.exe foreign   another tool's hot-patch already there: refused, never retried
#   pad_test_nohot.exe nohot     an entry that is not hot-patchable: refused, bytes untouched
#   pad_test_nohot.exe wrapper   the field case, route 1: a stand-in FFXiMain.dll (pad_game.cpp) with the
#                          client's exact wrapper bytes, its __cdecl shim xinputdll.dll (pad_shim.cpp) over
#                          the not hot-patchable fake, a real revision 2 module (pad_mod_r2.dll, from the
#                          frozen header in padhook_rev2/), and every check of `main` through the wrapper
# --self-test rebuilds pad_test once per PADHOOK_FAULT_* and requires it to fail.
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
OUT="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}/pad"
RUN="${NUI_RUN:-}"
NORUN="${NUI_NORUN:-}"          # set: build only (where Windows exes cannot run)
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra -static -static-libgcc -static-libstdc++"
mkdir -p "$OUT" "$OUT/game" "$OUT/game_nomatch"
cp layout_padhook.txt "$OUT/"

PIDS=""
build() { "$GXX" $FLAGS "$@" & PIDS="$PIDS $!"; }
build -shared pad_fake_xinput.cpp -o "$OUT/pad_fake_xinput.dll" -Wl,--kill-at
build -DFAKE_NOHOT -shared pad_fake_xinput.cpp -o "$OUT/pad_fake_nohot.dll" -Wl,--kill-at
build -shared pad_game.cpp -o "$OUT/game/FFXiMain.dll"
build -DGAME_NOMATCH -shared pad_game.cpp -o "$OUT/game_nomatch/FFXiMain.dll"
build -shared pad_shim.cpp -o "$OUT/xinputdll.dll"
build -DPADHOOK_REV=4 '-DPADMOD_NAME="r4"' '-DPADHOOK_DLL="pad_fake_xinput.dll"' -shared pad_mod.cpp -o "$OUT/pad_mod_r4.dll"
build -DPADMOD_REV2 '-DPADMOD_NAME="r2"' '-DPADHOOK_DLL="pad_fake_nohot.dll"' -shared pad_mod.cpp -o "$OUT/pad_mod_r2.dll"
build -DPADMOD_REV2 '-DPADMOD_NAME="r2"' '-DPADHOOK_DLL="pad_fake_xinput.dll"' -shared pad_mod.cpp -o "$OUT/pad_mod_r2x.dll"
build '-DPADHOOK_DLL="pad_fake_xinput.dll"' pad_test.cpp -o "$OUT/pad_test.exe"
build '-DPADHOOK_DLL="pad_fake_nohot.dll"' pad_test.cpp -o "$OUT/pad_test_nohot.exe"
for pid in $PIDS; do wait "$pid" || { echo "build failed" >&2; exit 1; }; done

if [ -n "$NORUN" ]; then echo "built in $OUT (NUI_NORUN: not run)"; exit 0; fi
cd "$OUT"
echo "== PadHook"
$RUN ./pad_test.exe main layout_padhook.txt
$RUN ./pad_test.exe nomatch
$RUN ./pad_test.exe both
$RUN ./pad_test.exe r2first
$RUN ./pad_test.exe foreign
$RUN ./pad_test_nohot.exe nohot
$RUN ./pad_test_nohot.exe wrapper

if [ "$1" = "--self-test" ]; then
    cd - >/dev/null
    # each fault: the define, the stand-in DLL it is built for, the mode that must fail
    for case in NO_HEARTBEAT:pad_fake_xinput:main TAKE_HELD:pad_fake_xinput:main LEAK_HELD:pad_fake_xinput:main \
                NO_RENUMBER:pad_fake_xinput:main NO_CHORD:pad_fake_xinput:main \
                OLD_THUNK:pad_fake_nohot:wrapper FIRST_MATCH:pad_fake_nohot:wrapper EXPORT_FIRST:pad_fake_xinput:both; do
        fault=${case%%:*}; rest=${case#*:}; dll=${rest%%:*}; mode=${rest#*:}
        "$GXX" $FLAGS "-DPADHOOK_FAULT_$fault" "-DPADHOOK_DLL=\"$dll.dll\"" pad_test.cpp -o "$OUT/pad_fault.exe"
        if (cd "$OUT" && $RUN ./pad_fault.exe "$mode" layout_padhook.txt >/dev/null 2>&1) 2>/dev/null; then
            echo "self-test: PADHOOK_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: PADHOOK_FAULT_$fault caught"
    done
fi
echo "pad tests passed"
