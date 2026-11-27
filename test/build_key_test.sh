#!/bin/sh
# KeyHook.h's own tests (32-bit MinGW i686): no game files needed. Outputs go to NUI_TEST_OUT (default: a temp
# folder). On Linux, set NUI_RUN=wine to run the exes under Wine. test/key_linux runs the logic and the patch on
# 32-bit Linux; this runs the real hook in a Windows process.
#   sh test/build_key_test.sh [--self-test]
#
#   key_test.exe main      a stand-in FFXiMain.dll (key_game.cpp) with the client's two keyboard read sites byte for
#                          byte, loaded late: wait, the patch, pass-through, the press rules through the client's own
#                          code, the heartbeat, a revision 2 module (key_mod.cpp) taking the dispatch and handing it
#                          back from its DllMain, the status, and the layout (layout_keyhook.txt)
#   key_test.exe nomatch   a stand-in whose cmps have another encoding: refused for good, its bytes untouched
# --self-test rebuilds key_test once per KEYHOOK_FAULT_* and requires it to fail.
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
OUT="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}/key"
RUN="${NUI_RUN:-}"
NORUN="${NUI_NORUN:-}"          # set: build only (where Windows exes cannot run)
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra -static -static-libgcc -static-libstdc++"
mkdir -p "$OUT" "$OUT/game" "$OUT/game_nomatch"
cp layout_keyhook.txt "$OUT/"

PIDS=""
build() { "$GXX" $FLAGS "$@" & PIDS="$PIDS $!"; }
build -shared key_game.cpp -o "$OUT/game/FFXiMain.dll"
build -DGAME_NOMATCH -shared key_game.cpp -o "$OUT/game_nomatch/FFXiMain.dll"
build -DKEYHOOK_REV=2 -shared key_mod.cpp -o "$OUT/key_mod_r2.dll"
build key_test.cpp -o "$OUT/key_test.exe"
for pid in $PIDS; do wait "$pid" || { echo "build failed" >&2; exit 1; }; done

if [ -n "$NORUN" ]; then echo "built in $OUT (NUI_NORUN: not run)"; exit 0; fi
cd "$OUT"
echo "== KeyHook"
$RUN ./key_test.exe main layout_keyhook.txt
$RUN ./key_test.exe nomatch

if [ "$1" = "--self-test" ]; then
    cd - >/dev/null
    for fault in NO_HEARTBEAT TAKE_HELD LEAK_HELD NO_EVENTS NO_CMP NO_PRIME; do
        "$GXX" $FLAGS "-DKEYHOOK_FAULT_$fault" key_test.cpp -o "$OUT/key_fault.exe"
        if (cd "$OUT" && $RUN ./key_fault.exe main layout_keyhook.txt >/dev/null 2>&1) 2>/dev/null; then
            echo "self-test: KEYHOOK_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: KEYHOOK_FAULT_$fault caught"
    done
fi
echo "key tests passed"
