#!/bin/sh
# Assets::load on a made-up install, natively on Linux, with no game files (hd_test.cpp): the same art as an HD
# texture pack stores it, at 2, 4 and 8 times the game's size and in every texture format the loader reads, must
# give the stock atlas, frame recipe and glyphs exactly. windows.h here is a minimal stand-in. Needs only g++. Built
# with AddressSanitizer and UBSan, so a read past the end of a DAT fails the run even where the checks would not.
#   sh test/hd_linux/run.sh
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/nui_hd_linux"
mkdir -p "$OUT"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -I. -I../../src \
    hd_test.cpp ../../src/assets.cpp -o "$OUT/hd_test"
"$OUT/hd_test"
