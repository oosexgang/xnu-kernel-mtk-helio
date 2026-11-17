#!/bin/bash
# hakios-mali for the 27 release's processes (arm64e): the library that writes the Mali-G52's job descriptors itself
# (userland/src/mali, no Gallium, no Panfrost) and its probe, mali-probe, which runs every stage against the kernel's
# HakiosMali. Built with the system's clang (host/bin/xnu-clang takes it for arm64e), as metal27.sh builds the Metal
# driver. The descriptors are packed by Mesa's generated genxml/v7_pack.h, made here from the Mesa tree's v7.xml by
# Mesa's gen_pack.py (MESA_SRC: kernel/deps/build/mesa/mesa-25.0.7, which kernel/deps/setup.sh unpacks).
#
# Output: $OBJ/mali/{mali-probe, libhakios-mali.a} (OBJ: as mtk27.sh's, build/ios27/obj), for root26-sys.sh
# MALI27=$OBJ/mali, which puts mali-probe into the small image as /hakios/bin/mali-probe.
# On this machine, against the fake GPU: make -C userland/src/mali test.
set -e
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$ROOT/kernel/env.sh"
MESA_SRC=${MESA_SRC:-$ROOT/kernel/deps/build/mesa/mesa-25.0.7}
O=${OBJ:-$ROOT/build/ios27/obj}/mali
SDK=${SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}
PACSTRIP=$ROOT/system27/tools/pacstrip
LDID=${LDID:-$ROOT/system27/tools/macos/ldid}
S=$ROOT/userland/src/mali
[ -f "$MESA_SRC/src/panfrost/lib/genxml/v7.xml" ] || { echo "mali27.sh: no Mesa tree in $MESA_SRC (kernel/deps/setup.sh)"; exit 1; }
[ -d "$SDK" ] || { echo "mali27.sh: no $SDK"; exit 1; }
if [ ! -x "$PACSTRIP" ] || [ "$ROOT/system27/tools/pacstrip.c" -nt "$PACSTRIP" ]; then
	# as mtk27.sh env builds it
	clang -O2 -w -I"$ROOT/system27/tools/macos" -include "$ROOT/system27/tools/macos/cfr.h" -o "$PACSTRIP" \
		"$ROOT/system27/tools/pacstrip.c"
fi
mkdir -p $O/gen/genxml
python3 "$MESA_SRC/src/panfrost/lib/genxml/gen_pack.py" "$MESA_SRC/src/panfrost/lib/genxml/v7.xml" > $O/gen/genxml/v7_pack.h

CC="xnu-clang -arch arm64e -isysroot $SDK -mios-version-min=16.0"
CXX="xnu-clang++ -arch arm64e -isysroot $SDK -mios-version-min=16.0"
$CC -std=c11 -O2 -I$O/gen -I"$MESA_SRC/src" -I"$MESA_SRC/include" -I"$MESA_SRC/src/panfrost/lib" -c $S/v7.c -o $O/v7.o
for f in gpu pass draw blit metal fake_gpu iokit_transport probe; do
	$CXX -std=c++17 -O2 -fno-exceptions -fno-rtti -I$S -c $S/$f.cpp -o $O/$f.o
done
# the compiler inside the driver (compiler.c): Mesa's Bifrost compiler from the arm64e Mesa build beside MESA_SRC
# (mk27.sh gl: kernel/deps/build/mesa/build-softpipe-panfrost-arm64e), compiled with that build's own defines, as the
# structures of compiler.c and of the libraries must agree; MESA_BUILD= another, or NO_COMPILER=1 without it
MESA_BUILD=${MESA_BUILD:-$(dirname "$MESA_SRC")/build-softpipe-panfrost-arm64e}
COMPILER_OBJ=; COMPILER_LIBS=(); PROBE_DEFS=
if [ "${NO_COMPILER:-0}" != 1 ] && [ -f "$MESA_BUILD/compile_commands.json" ]; then
	DEFS=$(python3 - "$MESA_BUILD/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['file'].endswith('panfrost/lib/pan_shader.c'):
        print(' '.join(a for a in shlex.split(e['command']) if a.startswith('-D') and not a.startswith('-DPAN_ARCH')))
        break
PY
)
	B=$MESA_BUILD/src
	# shellcheck disable=SC2086
	$CC -std=c11 -O2 -w -DPAN_ARCH=7 $DEFS -I"$B" -I"$MESA_SRC/src" -I"$MESA_SRC/include" -I"$B/compiler/nir" \
		-I"$MESA_SRC/src/compiler/nir" -I"$B/compiler" -I"$MESA_SRC/src/compiler" -I"$B/panfrost/lib" \
		-I"$MESA_SRC/src/panfrost/lib" -I"$B/panfrost/lib/genxml" -I"$MESA_SRC/src/panfrost/lib/genxml" \
		-I"$MESA_SRC/src/panfrost" -I"$B/util/format" -I"$MESA_SRC/src/compiler/air" -I"$ROOT/tools/mesa/air" \
		-c $S/compiler.c -o $O/compiler.o
	COMPILER_OBJ=$O/compiler.o
	COMPILER_LIBS=($B/panfrost/lib/libpan-arch-v7.a $B/panfrost/lib/libpan-format-v7.a $B/panfrost/lib/libpan-format-v6.a
		$B/panfrost/lib/libpanfrost_lib.a $B/panfrost/compiler/libpanfrost_bifrost.a
		$B/panfrost/compiler/libpanfrost_bifrost_disasm.a $B/panfrost/compiler/valhall/libpanfrost_valhall_disasm.a
		$B/panfrost/util/libpanfrost_util.a $B/panfrost/shared/libpanfrost_shared.a $B/compiler/nir/libnir.a
		$B/compiler/libcompiler.a $B/util/libmesa_util.a $B/c11/impl/libmesa_util_c11.a $B/util/blake3/libblake3.a -lz)
	PROBE_DEFS=-DHAKIOS_MALI_COMPILER
	COMPILER_LIBS+=($B/compiler/air/libair.a)
	# Apple's kernels for mali-probe's apple-kernels stage: PortraitFilters' .metallib of the extracted IPSW (REL), which
	# the pruned system image leaves out; root26-sys.sh puts it into the small image's share/
	REL=${REL:-$ROOT/build/ios27/ios27}
	PF=$REL/rootfs/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib
	[ -f "$PF" ] && cp "$PF" $O/PortraitFilters.metallib
	# and ARKitCore's, whose resample_v and resample_f are the apple-render stage's render pipeline
	AK=$REL/rootfs/System/Library/SubFrameworks/ARKitCore.framework/default.metallib
	[ -f "$AK" ] && cp "$AK" $O/ARKitCore.metallib
	$CXX -std=c++17 -O2 -fno-exceptions -fno-rtti $PROBE_DEFS -I$S -c $S/probe.cpp -o $O/probe.o
	# the native Metal library (userland/src/mali/rwa): air_metal.h's contract with the job descriptors written here,
	# /hakios/lib/libhakiosmali.dylib, which the Metal plug-in loads in place of libhakiosair.dylib with boot-arg
	# mali_native=1 (userland/src/metal/air.cpp)
	for f in rwa_shader rwa_mali kmali; do
		# shellcheck disable=SC2086
		$CC -std=c11 -O2 -w -DPAN_ARCH=7 $DEFS -I"$ROOT/tools/mesa/air" -I"$B" -I"$MESA_SRC/src" -I"$MESA_SRC/include" -I"$B/compiler/nir" \
			-I"$MESA_SRC/src/compiler/nir" -I"$B/compiler" -I"$MESA_SRC/src/compiler" -I"$B/panfrost/lib" \
			-I"$MESA_SRC/src/panfrost/lib" -I"$B/panfrost/lib/genxml" -I"$MESA_SRC/src/panfrost/lib/genxml" \
			-I"$MESA_SRC/src/panfrost" -I"$B/util/format" -I"$MESA_SRC/src/compiler/air" -I"$ROOT/tools/mesa/air" \
			-I$S/rwa -c $S/rwa/$f.c -o $O/$f.o
	done
	$CXX -dynamiclib -install_name /hakios/lib/libhakiosmali.dylib -compatibility_version 9.0.0 -current_version 9.0.0 -Wl,-exported_symbol,_rwa_* -o $O/libhakiosmali.dylib \
		$O/rwa_mali.o $O/rwa_shader.o $O/kmali.o "${COMPILER_LIBS[@]}" -framework IOKit -framework CoreFoundation -lc++
	$PACSTRIP macho $O/libhakiosmali.dylib $O/libhakiosmali.dylib.strip > /dev/null && mv $O/libhakiosmali.dylib.strip $O/libhakiosmali.dylib
	"$LDID" -S $O/libhakiosmali.dylib
	ls -la $O/libhakiosmali.dylib
fi
rm -f $O/libhakios-mali.a
ar rcs $O/libhakios-mali.a $O/v7.o $O/gpu.o $O/pass.o $O/draw.o $O/blit.o $O/metal.o $O/fake_gpu.o $O/iokit_transport.o
$CXX -o $O/mali-probe $O/probe.o $O/libhakios-mali.a $COMPILER_OBJ "${COMPILER_LIBS[@]}" -framework IOKit \
	-framework CoreFoundation -framework IOSurface -lc++
# arm64e code made runnable on a processor without pointer authentication, then signed (root26-sys.sh signs it
# again, ad hoc, with the rest of the small image)
$PACSTRIP macho $O/mali-probe $O/mali-probe.strip > /dev/null && mv $O/mali-probe.strip $O/mali-probe
"$LDID" -S $O/mali-probe
file -b $O/mali-probe | cut -c1-60
ls -la $O/mali-probe $O/libhakios-mali.a
