#!/bin/bash
# tools/rwacache.sh OUT.bin: the native Mali library's stages compiled on the Mac for the phone to load
# (userland/src/mali/rwa/rwa_shader.c, "the stage cache"): every recipe of userland/share/rwm-recipes27.txt built by
# rwacheck through the library built for the Mac (userland/src/mali/tools/mk-rwa-host.sh), each stage it compiles
# appended to OUT.bin (RWA_CACHE_OUT). The image takes it with RWA_CACHE_BIN=OUT.bin (system27/userland/root26-sys.sh).
# GEN_LOG=LOG: and every variant configuration of userland/share/rwm-variants27.txt of the generic pipelines a run with
# boot-arg rwm_capture_gen=1 said in LOG (rwagen).
#   MESA_HOST_BUILD=... [GEN_LOG=logs/ios27-runN.log] tools/rwacache.sh build/rwa-cache.bin
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HOST=${RWA_HOST_OUT:-$ROOT/build/rwa-host}
LIB=${QC_METALLIB:-$ROOT/build/ios27/rel-nat1/root26-sys-tree/share/QuartzCore-default.metallib}
OUT=$HOST bash "$ROOT/userland/src/mali/tools/mk-rwa-host.sh" >/dev/null
rm -f "$OUT"
RWA_CACHE_OUT="$OUT" NIR_DEBUG=novalidate "$HOST/rwacheck" "$LIB" "$ROOT/userland/share/rwm-recipes27.txt" backboardd | tail -1
if [ -n "${GEN_LOG:-}" ]; then
	RWA_CACHE_OUT="$OUT" NIR_DEBUG=novalidate "$HOST/rwagen" "$LIB" "$GEN_LOG" "$ROOT/userland/share/rwm-variants27.txt" |
		tail -1
fi
# one record a key: the same stage compiled twice (a recipe and a variant may share one) kept once
python3 - "$OUT" <<'PY2'
import struct, sys
p = sys.argv[1]
d = open(p, 'rb').read()
out, seen, i = [], set(), 0
while i + 16 <= len(d):
	magic, key, n = struct.unpack_from('<IQI', d, i)
	if magic != 0x43415752:
		sys.exit('rwacache: no record at %d' % i)
	if key not in seen:
		seen.add(key)
		out.append(d[i:i + 16 + n])
	i += 16 + n
open(p, 'wb').write(b''.join(out))
print('rwacache: %d records, %d different' % (len(out) + 0, len(seen)))
PY2
echo "rwacache: $(wc -c < "$OUT") bytes in $OUT"
