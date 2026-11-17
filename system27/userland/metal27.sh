#!/bin/bash
# The Metal driver for the Mali (userland/src/hakiosmetal.m over userland/src/metal, as userland/build.sh
# METAL=1 builds it for 16.5) for the 27 release's processes, which are arm64e, with the system's clang
# (host/bin/xnu-clang takes it for arm64e). With metal_props, which names the plug-in on IOAcceleratorES,
# and psgrep. Then the two things the plug-in needs beside it:
#   libOSMesa.8.dylib     Mesa with panfrost and its AIR compiler for arm64e (MESA=FILE: mesa26e.sh's,
#                         with DRIVERS=softpipe,panfrost and tools/mesa's AIR patch)
#   QuartzCore-default.metallib   CoreAnimation's shaders in AIR: the 27 release's own QuartzCore keeps an air64
#                         slice beside the GPU's in its default.metallib, and this takes that slice out
# Output: $OBJ/metal (OBJ: as inj26.sh), for root26-sys.sh METAL27=$OBJ/metal.
# (metal_drv_test is not built: it reads the thread state's pc, lr and fp directly, which arm64e's
# headers hide behind accessors)
set -e
RN=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DK=${DK:-$(cd "$RN/../.." && pwd)}
DR=${DR:-$DK/darwin-renoir}
REL=${REL:-$DK/ios27}
source $DR/xnu-env.sh
U=$(dirname "$RN")/userland
O=${OBJ:-$RN/userland/obj26}/metal
mkdir -p $O
DYN="xnu-clang -arch arm64e -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 -mios-version-min=16.0"
cd $U
for f in air buffer log texture blit draw command recipes import perf plib shape argenc run worker surfaces render compute cmdbuf pipeline drawable capture; do
	${DYN/xnu-clang/xnu-clang++} -Os -std=c++20 -fno-exceptions -fno-rtti -I../tools/mesa/air -c src/metal/$f.cpp -o $O/$f.o
done
$DYN -Os -bundle -framework Foundation -framework IOKit -framework IOSurface -framework Metal -lobjc -lc++ \
	-I../tools/mesa/air -Isrc -o $O/HakiosMetal src/hakiosmetal.m $O/*.o
$DYN -framework CoreFoundation -framework IOKit -o $O/metal_props src/metal_props.c
$DYN -o $O/psgrep src/psgrep.c
MESA=${MESA:-${OBJ:-$RN/userland/obj26}/libOSMesa.8.dylib}
# a captured draw of the window server's drawn again on the Mali at boot (tools/air/ca27_replay.c, boot-arg rwa_replay=1)
$DYN -I../tools/mesa/air -o $O/ca27_replay ../tools/air/ca27_replay.c "$MESA"
install_name_tool -change "$(otool -L $O/ca27_replay | awk '/libOSMesa/ {print $1; exit}')" /hakios/lib/libOSMesa.8.dylib $O/ca27_replay 2>/dev/null || true
for f in HakiosMetal metal_props psgrep ca27_replay; do
	$RN/tools/pacstrip macho $O/$f $O/$f.strip > /dev/null && mv $O/$f.strip $O/$f
	${LDID:-$THEOS/toolchain/linux/iphone/bin/ldid} -S $O/$f
	file -b $O/$f | cut -c1-50
done
MESA=${MESA:-${OBJ:-$RN/userland/obj26}/libOSMesa.8.dylib}
[ -f "$MESA" ] || { echo "metal27.sh: no Mesa in $MESA (mesa26e.sh with DRIVERS=softpipe,panfrost)"; exit 1; }
nm -gU "$MESA" | grep -q " _rwa_device_open$" || { echo "metal27.sh: $MESA has no AIR compiler (rwa_device_open)"; exit 1; }
cp "$MESA" $O/libOSMesa.8.dylib
# the air64 slice of a fat file: lipo does not know the architecture, so it is cut out by its fat header
python3 - "$REL/rootfs/System/Library/Frameworks/QuartzCore.framework/default.metallib" $O/QuartzCore-default.metallib <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
magic, n = struct.unpack_from('>II', b, 0)
assert magic == 0xcafebabe, 'not a fat file'
for i in range(n):
    cputype, sub, off, size, align = struct.unpack_from('>iiIII', b, 8 + 20 * i)
    if cputype == 0x1000017:          # air64
        out = b[off:off + size]
        assert out[:4] == b'MTLB'
        open(sys.argv[2], 'wb').write(out)
        print('QuartzCore-default.metallib: the air64 slice,', size, 'bytes')
        break
else:
    raise SystemExit('no air64 slice')
PY
