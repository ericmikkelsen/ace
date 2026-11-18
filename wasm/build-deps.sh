#!/bin/sh
# Build the engine's dependencies for wasm32 with Emscripten.
#   wasm/build-deps.sh <librepp source dir> <boost regex src dir with posix_api.cpp + wide_posix_api.cpp> <out dir>
# Boost headers are taken from BOOST_INCLUDE (default: Homebrew's).
set -e
REPP=$1; BSRC=$2; OUT=$3
BOOST_INCLUDE=${BOOST_INCLUDE:-/opt/homebrew/include}
mkdir -p "$OUT/include" "$OUT/lib" "$OUT/obj"
ln -sfn "$BOOST_INCLUDE/boost" "$OUT/include/boost"
for f in posix_api wide_posix_api; do
  em++ -O3 -std=c++17 -fexceptions -isystem "$OUT/include" -c "$BSRC/$f.cpp" -o "$OUT/obj/$f.o"
done
emar rcs "$OUT/lib/boost-regex-posix.a" "$OUT/obj/posix_api.o" "$OUT/obj/wide_posix_api.o"
for f in load_repp preprocessor unicode; do
  emcc -O3 -funsigned-char -w -Wno-error=implicit-function-declaration -Wno-error=implicit-int -isystem "$OUT/include" -I "$REPP/include" -c "$REPP/librepp/$f.c" -o "$OUT/obj/$f.o"
done
emar rcs "$OUT/lib/librepp.a" "$OUT/obj/load_repp.o" "$OUT/obj/preprocessor.o" "$OUT/obj/unicode.o"
cp "$REPP/include/librepp.h" "$OUT/include/"
echo "deps built in $OUT"
