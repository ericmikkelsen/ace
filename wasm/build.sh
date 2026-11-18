#!/bin/sh
# Build the engine as WebAssembly for one grammar, and compile that grammar's image.
#
#   wasm/build.sh <deps dir> <grammar config.tdl> <out dir>
#
# <deps dir> comes from wasm/build-deps.sh. Produces <out dir>/ace (JS loader),
# <out dir>/ace.wasm and <out dir>/grammar.dat. Run with: node <out dir>/ace -g <out dir>/grammar.dat
#
# Two stages, because the grammar's quickcheck extractor is C generated from the
# grammar (natively it's compiled to a shared library and dlopen'd at parse time):
#   1. build the engine without it, and have it write the extractor C;
#   2. compile that C into the module, rebuild, and compile the grammar image.
# If the engine's function signatures drift (wasm-ld "function signature
# mismatch" warnings), regenerate wasm-protos.h with wasm/gen-protos.py.
set -e
DEPS=$(cd "$1" && pwd); CONFIG=$2; OUT=$(mkdir -p "$3" && cd "$3" && pwd)
SRC=$(cd "$(dirname "$0")/.." && pwd)
export LC_ALL=${LC_ALL:-en_US.UTF-8}   # grammars must be read as UTF-8
cd "$SRC"
make clean >/dev/null 2>&1 || true
make -j8 ace WASM=1 DEPS="$DEPS" > "$OUT/build-stage1.log" 2>&1
(cd "$(dirname "$CONFIG")" && ACE_QC_C_ONLY=1 ACE_QC_C_OUT="$OUT/qcex.c" node "$SRC/ace" -G "$OUT/stage1.dat" -g "$(basename "$CONFIG")" > "$OUT/stage1.log" 2>&1)
rm -f "$OUT/stage1.dat"
emcc -O3 -c "$OUT/qcex.c" -o "$OUT/qcex.o"
rm -f ace ace.wasm
make ace WASM=1 DEPS="$DEPS" QC_C="$OUT/qcex.c" > "$OUT/build-stage2.log" 2>&1
if grep -q "signature mismatch" "$OUT/build-stage2.log"; then echo "wasm-ld reported signature mismatches; see $OUT/build-stage2.log" >&2; exit 1; fi
cp ace ace.wasm "$OUT/"
(cd "$(dirname "$CONFIG")" && node "$OUT/ace" -G "$OUT/grammar.dat" -g "$(basename "$CONFIG")" > "$OUT/compile.log" 2>&1)
ls -la "$OUT/ace" "$OUT/ace.wasm" "$OUT/grammar.dat"
