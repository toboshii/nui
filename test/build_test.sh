#!/bin/sh
# The nui library's own tests (32-bit MinGW i686). Build outputs and the FFXiMain.dll copy go to
# NUI_TEST_OUT (default: a temp folder), never into the repo; the game install is only read
# (assets), never loaded.
#   sh test/build_test.sh [--self-test]
#
# --self-test also builds the module set once per UIHOOK_FAULT_* (a deliberately broken UiHook) and
# requires hook_matrix to fail each one with the FAIL tag of the check aimed at it; then the same for
# the cache's and the renderer's NUI_FAULT_* variants.
#
#   sync.py check    every vendored copy equals src (the consumers' tests call this too)
#   frozen_check.py  the layouts frozen at 1.0.0 (test/frozen/) are still in layout_*.txt, line for line
#   hook_test.exe    UiHook.h against the real unpacked EndScene wrapper, with a recording stub
#   hook_matrix.exe  UiHook 2 across modules (hook_mod.cpp variants: names, revisions 1-3, a module that
#                    cannot find the wrapper, one whose DllMain does not unregister, a pre-release ABI 1
#                    module), every load/unload order of 2 and 3 modules, the frozen layout
#                    (layout_uihook.txt)
#   cr_testhost.dll  the Lua test host consumers use (a real D3D8 device on a hidden window, frames
#                    through the patched wrapper); built here, used by the consumers' Lua tests
#   build_cache_test.sh   NuiCache.h alone (test_cache.exe: fake and real textures, every module order)
#   build_render_test.sh  the cache through the whole renderer (test_render.exe: module DLLs built from
#                    src/, real frames, pixels, golden_fmt.json)
#   build_pad_test.sh     PadHook.h (1.3.0) with a stand-in xinput1_3.dll: patch, mask rules, heartbeat, modules
# --self-test runs every fault variant of all four (UiHook, cache, renderer, pad hook).
#
# A consumer's own build_test.sh calls this with NUI_TEST_OUT=<its out dir> and then runs its tests.
# NUI_GXX / NUI_FFXI_COPY / NUI_DLL override the compiler and the unpacked-DLL source. The copy must
# be an unpacked FFXiMain.dll (the shipped one has its .text encrypted in section POL1).
set -e
cd "$(dirname "$0")"
GXX="${NUI_GXX:-i686-w64-mingw32-g++}"
PY="${NUI_PYTHON:-python}"
OUT="${NUI_TEST_OUT:-${TEMP:-/tmp}/nui_test}"
FFXI_COPY="${NUI_FFXI_COPY:?set NUI_FFXI_COPY to an unpacked FFXiMain.dll}"
mkdir -p "$OUT/ffxi"
cp "$FFXI_COPY" "$OUT/ffxi/FFXiMain.dll"
echo "== nui sync check"
if [ -n "$NUI_SKIP_SYNC" ]; then
    echo "skipped (NUI_SKIP_SYNC): canonical src changed and not yet released/synced to the consumers"
else
    "$PY" ../sync.py check ${NUI_DLL:+--dll "$NUI_DLL"}      # NUI_DLL=<a built DLL>: it must carry the canonical tree too
fi
echo "== frozen ABIs (UiHook 2 rev 1, NuiCache major 1, as released in nui 1.0.0)"
"$PY" frozen_check.py
[ "$1" = "--self-test" ] && "$PY" frozen_check.py --self-test
FLAGS="-std=c++17 -O2 -m32 -Wall -Wextra -static -static-libgcc -static-libstdc++"
"$GXX" $FLAGS -shared hook_client.cpp -o "$OUT/hook_client.dll"
"$GXX" $FLAGS hook_test.cpp -o "$OUT/hook_test.exe"
"$GXX" $FLAGS -shared testhost.cpp -o "$OUT/cr_testhost.dll"
"$GXX" $FLAGS hook_matrix.cpp -o "$OUT/hook_matrix.exe"
echo "== cr_testhost keeps the FPU's precision (D3DCREATE_FPU_PRESERVE): a number above 2^24 survives the device"
LUA="${NUI_LUA:-/c/Program Files (x86)/Lua/5.1/lua.exe}"
"$LUA" -e "package.cpath = [[$(cygpath -m "$OUT")/?.dll;]] .. package.cpath local T = require('cr_testhost') assert(T.make_device()) T.fill(0) local a = tonumber('17293316') local s = tostring(a + 1) print('cr_testhost: 17293316 + 1 = ' .. s) if s ~= '17293317' then print('FAIL [fpu] cr_testhost: single-precision FPU') os.exit(1) end"
FFXI_W="$(cygpath -w "$OUT/ffxi/FFXiMain.dll")"
LAYOUT_W="$(cygpath -w "$(pwd)/layout_uihook.txt")"

# build_mods <dir> [extra flags...]: the hook_mod.cpp variants hook_matrix loads
build_mods() {
    dir="$1"; shift
    mkdir -p "$dir"
    "$GXX" $FLAGS -shared "$@" '-DHOOKMOD_NAME="a"' -DUIHOOK_REV=1 hook_mod.cpp -o "$dir/mod_a1.dll" &
    "$GXX" $FLAGS -shared "$@" '-DHOOKMOD_NAME="b"' -DUIHOOK_REV=1 hook_mod.cpp -o "$dir/mod_b1.dll" &
    "$GXX" $FLAGS -shared "$@" '-DHOOKMOD_NAME="c"' -DUIHOOK_REV=1 hook_mod.cpp -o "$dir/mod_c1.dll" &
    "$GXX" $FLAGS -shared "$@" '-DHOOKMOD_NAME="b"' -DUIHOOK_REV=2 hook_mod.cpp -o "$dir/mod_b2.dll" &
    "$GXX" $FLAGS -shared "$@" '-DHOOKMOD_NAME="c"' -DUIHOOK_REV=3 -DHOOKMOD_ORDER=5 hook_mod.cpp -o "$dir/mod_c3.dll" &
    "$GXX" $FLAGS -shared "$@" '-DHOOKMOD_NAME="s"' -DUIHOOK_REV=1 -DUIHOOK_TEST_SIG_BLIND hook_mod.cpp -o "$dir/mod_s1.dll" &
    "$GXX" $FLAGS -shared "$@" '-DHOOKMOD_NAME="d"' -DUIHOOK_REV=1 -DHOOKMOD_NO_DETACH hook_mod.cpp -o "$dir/mod_d1.dll" &
    "$GXX" $FLAGS -shared -DHOOKMOD_ABI1 hook_mod.cpp -o "$dir/mod_x0.dll" &
    wait
    for m in a1 b1 c1 b2 c3 s1 d1 x0; do [ -f "$dir/mod_$m.dll" ] || { echo "build failed: $dir/mod_$m.dll"; exit 1; }; done
}
rm -rf "$OUT/mods"
build_mods "$OUT/mods"

echo "== hook_test"
"$OUT/hook_test.exe" "$FFXI_W" "$(cygpath -w "$OUT/hook_client.dll")"
echo "== hook_matrix"
"$OUT/hook_matrix.exe" "$FFXI_W" "$(cygpath -w "$OUT/mods")" "$LAYOUT_W"

if [ "$1" = "--self-test" ]; then
    echo "== hook self-test: every broken UiHook must be caught by its check"
    bad=0
    for pair in NO_HANDOVER:handover HANDOVER_FIRST:owner NO_REV_TAKEOVER:owner SKIP_FOREIGN:draw \
                LAYOUT_SWAP:layout NO_LIVENESS:liveness NO_ABI1_CHECK:abi1 SCAN_FIRST:join \
                NO_LOCK_BREAK:lock SPLIT_PATCH:patch NO_DRAIN:drain; do
        fault="${pair%%:*}"; tag="${pair#*:}"
        dir="$OUT/fault_$fault"
        rm -rf "$dir"
        build_mods "$dir" "-DUIHOOK_FAULT_$fault"
        if "$OUT/hook_matrix.exe" "$FFXI_W" "$(cygpath -w "$dir")" "$LAYOUT_W" > "$dir/out.txt" 2>&1; then
            echo "NOT CAUGHT  UIHOOK_FAULT_$fault: hook_matrix passed"; bad=1
        elif grep -q "^FAIL \[$tag\]" "$dir/out.txt"; then
            echo "caught      UIHOOK_FAULT_$fault by [$tag]: $(grep -c '^FAIL ' "$dir/out.txt") FAIL lines, first: $(grep -m1 "^FAIL \[$tag\]" "$dir/out.txt" | cut -c1-150)"
        else
            echo "WRONG CHECK UIHOOK_FAULT_$fault: failed, but no FAIL [$tag]:"; grep -m3 '^FAIL' "$dir/out.txt"; bad=1
        fi
    done
    [ $bad = 0 ] && echo "hook self-test: all 11 faults caught" || { echo "hook self-test: FAILED"; exit 1; }
fi

echo "== cache (build_cache_test.sh)"
NUI_GXX="$GXX" NUI_TEST_OUT="$OUT" sh ./build_cache_test.sh ${1:+"$1"}
echo "== renderer + cache (build_render_test.sh)"
NUI_GXX="$GXX" NUI_TEST_OUT="$OUT" NUI_FFXI_COPY="$OUT/ffxi/FFXiMain.dll" sh ./build_render_test.sh ${1:+"$1"}
echo "== layer build path (build_build_test.sh)"
NUI_GXX="$GXX" NUI_TEST_OUT="$OUT" NUI_FFXI_COPY="$OUT/ffxi/FFXiMain.dll" sh ./build_build_test.sh ${1:+"$1"}
echo "== pad hook (build_pad_test.sh)"
NUI_GXX="$GXX" NUI_TEST_OUT="$OUT" sh ./build_pad_test.sh ${1:+"$1"}
echo "nui library tests: all pass"
