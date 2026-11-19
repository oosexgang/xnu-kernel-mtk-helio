#!/bin/bash
# tools/rwacache.sh OUT.bin: the native Mali library's stages compiled on the Mac for the phone to load
# (userland/src/mali/rwa/rwa_shader.c, "the stage cache"): every recipe of userland/share/rwm-recipes27.txt built by
# rwacheck through the library built for the Mac (userland/src/mali/tools/mk-rwa-host.sh), each stage it compiles
# appended to OUT.bin (RWA_CACHE_OUT). The image takes it with RWA_CACHE_BIN=OUT.bin (system27/userland/root26-sys.sh).
#   MESA_HOST_BUILD=... tools/rwacache.sh build/rwa-cache.bin
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HOST=${RWA_HOST_OUT:-$ROOT/build/rwa-host}
LIB=${QC_METALLIB:-$ROOT/build/ios27/rel-nat1/root26-sys-tree/share/QuartzCore-default.metallib}
OUT=$HOST bash "$ROOT/userland/src/mali/tools/mk-rwa-host.sh" >/dev/null
rm -f "$OUT"
RWA_CACHE_OUT="$OUT" NIR_DEBUG=novalidate "$HOST/rwacheck" "$LIB" "$ROOT/userland/share/rwm-recipes27.txt" backboardd | tail -1
echo "rwacache: $(wc -c < "$OUT") bytes in $OUT"
