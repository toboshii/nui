#!/bin/sh
# KeyBinds.h in a Windows process (32-bit MinGW i686): no game files needed. Outputs go to NUI_TEST_OUT (default: a
# temp folder). test/binds_linux runs the scan over the real client's bytes on Linux; this runs the live read.
#   sh test/build_binds_test.sh [--self-test]
#
#   binds_test.exe main      a stand-in FFXiMain.dll (binds_game.cpp) with the eight signature sites, loaded late:
#                            not read before it loads, then the copy, a change between two copies, the mode object,
#                            and its code unchanged
#   binds_test.exe nomatch   a stand-in whose keyboard mode test has another byte: refused for good
# --self-test rebuilds binds_test once per KEYBINDS_FAULT_* that a stand-in can show and requires it to fail.
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
OUT="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}/binds"
RUN="${NUI_RUN:-}"
NORUN="${NUI_NORUN:-}"          # set: build only (where Windows exes cannot run)
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra -static -static-libgcc -static-libstdc++ -I../src"
mkdir -p "$OUT/game" "$OUT/game_nomatch"

PIDS=""
build() { "$GXX" $FLAGS "$@" & PIDS="$PIDS $!"; }
build -shared binds_game.cpp -o "$OUT/game/FFXiMain.dll"
build -DGAME_NOMATCH -shared binds_game.cpp -o "$OUT/game_nomatch/FFXiMain.dll"
build binds_test.cpp -o "$OUT/binds_test.exe"
for pid in $PIDS; do wait "$pid" || { echo "build failed" >&2; exit 1; }; done

if [ -n "$NORUN" ]; then echo "built in $OUT (NUI_NORUN: not run)"; exit 0; fi
cd "$OUT"
echo "== KeyBinds"
$RUN ./binds_test.exe main
$RUN ./binds_test.exe nomatch

if [ "$1" = "--self-test" ]; then
    cd - >/dev/null
    for fault in MOVE_ALL; do
        "$GXX" $FLAGS "-DKEYBINDS_FAULT_$fault" binds_test.cpp -o "$OUT/binds_fault.exe"
        if (cd "$OUT" && $RUN ./binds_fault.exe main >/dev/null 2>&1) 2>/dev/null; then
            echo "self-test: KEYBINDS_FAULT_$fault was not caught" >&2
            exit 1
        fi
        echo "self-test: KEYBINDS_FAULT_$fault caught"
    done
fi
echo "binds tests passed"
