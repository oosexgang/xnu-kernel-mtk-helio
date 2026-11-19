#!/bin/bash
# The native Metal library (userland/src/mali/rwa) built on this Mac against a host build of Mesa 25.0.7 with panfrost,
# with rwacheck: every CoreAnimation pipeline of a recipes file compiled the way the phone's window server asks for it
# (rwa_render_create), with no GPU -- kmali's buffers are the process's own memory (RWA_HOST=1).
#   MESA_HOST_BUILD=DIR   the meson build directory (DIR/../mesa-25.0.7 the source tree, or MESA_HOST_SRC=)
#   rwacheck LIBRARY.metallib RECIPES.txt [PROCESS]
#   airdump LIBRARY.metallib FUNCTION OUT.bc, rwavariant LIBRARY.metallib CAPTURE (the "cap ..." lines of a run)
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${MESA_HOST_BUILD:?MESA_HOST_BUILD: a host Mesa build directory with panfrost}
S=${MESA_HOST_SRC:-$B/../mesa-25.0.7}
OUT=${OUT:-$HERE/../out/rwa-host}
R=$HERE/../rwa
mkdir -p "$OUT"
DEFS=$(python3 - "$B/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('panfrost/lib/pan_shader.c'):
        print(' '.join(a for a in shlex.split(e['command']) if a.startswith('-D') and not a.startswith('-DPAN_ARCH')))
        break
PY
)
INC=(-I"$HERE/../../../../tools/mesa/air" -I"$B/src" -I"$S/src" -I"$S/include" -I"$B/src/compiler/nir" -I"$S/src/compiler/nir" -I"$B/src/compiler"
	-I"$S/src/compiler" -I"$B/src/panfrost/lib" -I"$S/src/panfrost/lib" -I"$B/src/panfrost/lib/genxml"
	-I"$S/src/panfrost/lib/genxml" -I"$S/src/panfrost" -I"$S/src/panfrost/lib" -I"$B/src/util/format" -I"$S/src/compiler/air"
	-I"$HERE/../../../../tools/mesa/air" -I"$R")
W=(-Wall -Wno-unused-function -Wno-missing-braces -Wno-unused-variable)
for f in rwa_shader rwa_mali rwa_helio kmali; do
	# shellcheck disable=SC2086
	clang -std=c11 -O1 -g "${W[@]}" -DPAN_ARCH=7 -DRWA_HOST_FAKE=1 -DRWA_PANDECODE=1 $DEFS "${INC[@]}" -c "$R/$f.c" -o "$OUT/$f.o"
done
# shellcheck disable=SC2086
clang -std=c11 -O1 -g -Wall $DEFS "${INC[@]}" -c "$HERE/rwacheck.c" -o "$OUT/rwacheck.o"
L=$B/src
LIBS=(
	$L/panfrost/lib/genxml/libpanfrost_decode.a $L/panfrost/lib/genxml/libpandecode-arch-v7.a $L/panfrost/lib/genxml/libpandecode-arch-v4.a $L/panfrost/lib/genxml/libpandecode-arch-v5.a $L/panfrost/lib/genxml/libpandecode-arch-v6.a $L/panfrost/midgard/libpanfrost_midgard_disasm.a $L/panfrost/lib/genxml/libpandecode-arch-v9.a $L/panfrost/lib/genxml/libpandecode-arch-v10.a $L/panfrost/lib/libpan-arch-v7.a $L/panfrost/lib/libpan-format-v7.a $L/panfrost/lib/libpan-format-v6.a $L/panfrost/lib/libpanfrost_lib.a
	$L/panfrost/compiler/libpanfrost_bifrost.a $L/panfrost/compiler/libpanfrost_bifrost_disasm.a
	$L/panfrost/compiler/valhall/libpanfrost_valhall_disasm.a $L/panfrost/util/libpanfrost_util.a
	$L/panfrost/shared/libpanfrost_shared.a $L/compiler/air/libair.a $L/compiler/nir/libnir.a $L/compiler/libcompiler.a
	$L/util/libmesa_util.a $L/util/libmesa_util_sse41.a $L/c11/impl/libmesa_util_c11.a $L/util/blake3/libblake3.a -lz)
clang++ -o "$OUT/rwacheck" "$OUT/rwacheck.o" "$OUT/rwa_mali.o" "$OUT/rwa_shader.o" "$OUT/rwa_helio.o" "$OUT/kmali.o" "${LIBS[@]}" \
	-framework IOKit -framework CoreFoundation
# airdump: a function's AIR bitcode out of a metallib (for llvm-dis); rwavariant: a captured pipeline built here
# shellcheck disable=SC2086
clang -std=c11 -O1 -g -Wall $DEFS "${INC[@]}" -c "$HERE/airdump.c" -o "$OUT/airdump.o"
clang++ -o "$OUT/airdump" "$OUT/airdump.o" $L/compiler/air/libair.a $L/compiler/nir/libnir.a $L/compiler/libcompiler.a \
	$L/util/libmesa_util.a $L/util/libmesa_util_sse41.a $L/c11/impl/libmesa_util_c11.a $L/util/blake3/libblake3.a -lz \
	-framework CoreFoundation
# shellcheck disable=SC2086
clang -std=c11 -O1 -g -Wall -DPAN_ARCH=7 -DRWA_HOST_FAKE=1 $DEFS "${INC[@]}" -c "$HERE/rwavariant.c" -o "$OUT/rwavariant.o"
clang++ -o "$OUT/rwavariant" "$OUT/rwavariant.o" "$OUT/rwa_mali.o" "$OUT/rwa_shader.o" "$OUT/rwa_helio.o" "$OUT/kmali.o" \
	"${LIBS[@]}" -framework IOKit -framework CoreFoundation
echo "$OUT/rwacheck $OUT/airdump $OUT/rwavariant"
