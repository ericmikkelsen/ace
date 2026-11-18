#!/bin/sh
# Assemble the browser demo from a wasm/build.sh output directory.
#   wasm/web/build-site.sh <build out dir> <site dir>
# Then: node wasm/web/serve.mjs <site dir>   and open http://127.0.0.1:8765
# The .br files are what a real server should send with Content-Encoding: br.
set -e
OUT=$1; SITE=$2; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$SITE"
cp "$OUT/ace-web.js" "$OUT/ace-web.wasm" "$HERE/worker.js" "$HERE/index.html" "$SITE/"
cp "$OUT/grammar.dat" "$SITE/grammar.dat"
printf '{"file": "grammar.dat", "bytes": %d}\n' "$(wc -c < "$SITE/grammar.dat")" > "$SITE/grammar.json"
brotli -q 11 -f -o "$SITE/ace-web.wasm.br" "$SITE/ace-web.wasm"
brotli -q 11 -f -o "$SITE/ace-web.js.br" "$SITE/ace-web.js"
brotli -q 11 -f -o "$SITE/grammar.dat.br" "$SITE/grammar.dat"
ls -la "$SITE"
