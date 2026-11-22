#!/bin/bash
# Build tools/bishaders.c against a host build of Mesa 25.0.7 with panfrost and run it: userland/src/mali/shaders_v7.h,
# the first triangle's shaders compiled by Mesa's Bifrost compiler and their Renderer State words.
#   MESA_HOST_BUILD=DIR   the meson build directory (its compile_commands.json gives the flags; DIR/../mesa-25.0.7
#                         the source tree, or MESA_HOST_SRC=)
# The phone's build (mali27.sh) only reads the generated header; this runs when the shaders change.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=${MESA_HOST_BUILD:?MESA_HOST_BUILD: a host Mesa build directory with panfrost}
S=${MESA_HOST_SRC:-$B/../mesa-25.0.7}
OUT=${OUT:-$HERE/../out/tools}
mkdir -p "$OUT"
# the defines Mesa compiled its panfrost library with (the struct layouts depend on them)
DEFS=$(python3 - "$B/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('panfrost/lib/pan_shader.c'):
        print(' '.join(a for a in shlex.split(e['command']) if a.startswith('-D') and not a.startswith('-DPAN_ARCH')))
        break
PY
)
INC=(-I"$B/src" -I"$S/src" -I"$S/include" -I"$B/src/compiler/nir" -I"$S/src/compiler/nir" -I"$B/src/compiler"
	-I"$S/src/compiler" -I"$B/src/panfrost/lib" -I"$S/src/panfrost/lib" -I"$B/src/panfrost/lib/genxml"
	-I"$S/src/panfrost/lib/genxml" -I"$S/src/panfrost" -I"$B/src/util/format")
# shellcheck disable=SC2086
clang -std=c11 -O1 -g -w -DPAN_ARCH=7 $DEFS "${INC[@]}" -I"$S/src/compiler/air" -I"$HERE/../../../../userland/tools/mesa/air" \
	-c "$HERE/../compiler.c" -o "$OUT/compiler.o"
clang -std=c11 -O1 -g -w -c "$HERE/bishaders.c" -o "$OUT/bishaders.o"
clang -std=c11 -O1 -g -w -c "$HERE/airkernels.c" -o "$OUT/airkernels.o"
clang -std=c11 -O1 -g -w -c "$HERE/airrender.c" -o "$OUT/airrender.o"
L=$B/src
LIBS=(
	$L/panfrost/lib/libpan-arch-v7.a $L/panfrost/lib/libpan-format-v7.a $L/panfrost/lib/libpan-format-v6.a $L/panfrost/lib/libpanfrost_lib.a \
	$L/panfrost/compiler/libpanfrost_bifrost.a $L/panfrost/compiler/libpanfrost_bifrost_disasm.a \
	$L/panfrost/compiler/valhall/libpanfrost_valhall_disasm.a $L/panfrost/util/libpanfrost_util.a \
	$L/panfrost/shared/libpanfrost_shared.a $L/compiler/nir/libnir.a $L/compiler/libcompiler.a \
	$L/util/libmesa_util.a $L/util/libmesa_util_sse41.a $L/c11/impl/libmesa_util_c11.a $L/util/blake3/libblake3.a
	$L/compiler/air/libair.a -lz)
clang++ -o "$OUT/bishaders" "$OUT/bishaders.o" "$OUT/compiler.o" "${LIBS[@]}"
clang++ -o "$OUT/airkernels" "$OUT/airkernels.o" "$OUT/compiler.o" "${LIBS[@]}"
clang++ -o "$OUT/airrender" "$OUT/airrender.o" "$OUT/compiler.o" "${LIBS[@]}"
"$OUT/bishaders" "$HERE/../shaders_v7.h"
