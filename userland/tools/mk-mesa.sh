#!/bin/bash
# Build Mesa for the system's userland: the OpenGL ES library behind userland/src/eagl.c (see there and
# userland/src/gl_osmesa.c). Mesa is fetched (a pinned release), patched with userland/tools/mesa/*.patch and
# built with the cross compiler of the kernel (kernel/deps/host/bin/xnu-clang, see kernel/deps/setup.sh)
# against the system's SDK that userland/build.sh uses; nothing of it is in the repository.
#
#   DRIVERS="softpipe,freedreno"     (the first phone: DRIVERS="softpipe,panfrost", see userland/tools/mesa/panfrost-darwin.patch)
#                        the gallium drivers to build. softpipe: a rasterizer in plain C; it needs no
#                        LLVM and nothing of the machine but its processor. freedreno: the Adreno's;
#                        it talks to the kernel's GPU driver (the service HakiosGPU) through
#                        userland/tools/mesa/hakios-drm, which stands in for libdrm (userland/tools/mesa/freedreno-darwin.patch
#                        makes Mesa build it where there is no DRM). The library takes the GPU when the
#                        kernel has one and the environment does not say OSMESA_SOFTWARE=1.
#
#   MESA_ARCH=arm64e     for the 27 release's processes, which are arm64e (darwin/userland/metal27.sh);
#                        built by the system's clang (xnu-clang takes it for arm64e), into out-arm64e
#
# What comes out, in kernel/deps/build/mesa/out:
#   libOSMesa.8.dylib    everything in one library (install name /hakios/lib/libOSMesa.8.dylib): the
#                        OpenGL (ES) entry points gl*, the compiler of the shading language, the drivers,
#                        and the off-screen interface that hands out contexts
#   exported-gl.txt      the gl* functions it exports, and
#   gl.xml               their prototypes (the Khronos registry as Mesa has it): for userland/mkeagl.py
# Needs on this machine: meson, ninja, bison, flex, python3 with venv (Mako is installed into a
# private environment), llvm-ar.
set -e
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
source "$ROOT/kernel/env.sh"
B=$ROOT/kernel/deps/build/mesa
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/kernel/deps/host/bin/xnu-clang
ARCH=${MESA_ARCH:-arm64}
OUT=out; [ $ARCH = arm64 ] || OUT=out-$ARCH
mkdir -p "$B/$OUT"
cd "$B"

# The sources are the archive's with userland/tools/mesa/*.patch on top. When the patches change, a tree that was
# patched with the earlier ones is of no use (and builds without a word into a library that does not match
# the rest): the sources are taken from the archive again and everything is built anew, about a minute.
PATCHES=$(cat "$ROOT"/userland/tools/mesa/*.patch | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.hakios-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/userland/tools/mesa/*.patch; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$PATCHES" > mesa-$VERSION/.hakios-patches
fi
# ours, kept in the repository: always the current ones
rm -rf mesa-$VERSION/src/hakios-drm
cp -r "$ROOT/userland/tools/mesa/hakios-drm" mesa-$VERSION/src/hakios-drm
# the AIR frontend (docs/air.md): the Metal runtime will call it in this
# library's own process, and userland/tools/mesa/air-frontend.patch is the meson that
# reaches it. Copied rather than linked, because this tree is cross-built.
rm -rf mesa-$VERSION/src/compiler/air
cp -r "$ROOT/userland/tools/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH=$B/venv/bin:$PATH

# the compiler is the kernel's wrapper: it names the target (arm64-apple-ios) and the linker (ld64).
# Archiver and strip: LLVM's where there is one, otherwise Apple's (a Mac with only the command line tools)
AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip
command -v llvm-ar >/dev/null || { AR=ar; RANLIB=ranlib; STRIP=strip; }
CROSS=cross.ini; [ $ARCH = arm64 ] || CROSS=cross-$ARCH.ini
cat > $CROSS <<INI
[binaries]
c = ['$CC', '-arch', '$ARCH', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', '$ARCH', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', '$ARCH', '-isysroot', '$SDK']
ar = '$AR'
ranlib = '$RANLIB'
strip = '$STRIP'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = '$ARCH'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${DRIVERS//,/-}
[ $ARCH = arm64 ] || BUILD=$BUILD-$ARCH
# panfrost travels in the image that goes into the first phone's 32 MB boot partition: built for size
# (11 MB instead of 14.5), which costs the GPU nothing -- it is the GPU that draws
OPT=
case "$DRIVERS" in *panfrost*) OPT=-Doptimization=s ;; esac
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file $CROSS --wrap-mode=nofallback \
	--prefix=/hakios --libdir=lib --buildtype=release $OPT -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja-$ARCH.log
# air_run draws with a translated Metal shader and reads the pixels back; it is
# the only way to find out whether the Adreno takes work that did not come from
# the window server. Built only when asked, since it is a test program.
if [ "${AIR:-}" = 1 ]; then
	# beside the driver it draws with: freedreno's directory for the Adreno, panfrost's for the Mali
	case ",$DRIVERS," in *,panfrost,*) AIR_DIR=panfrost ;; *) AIR_DIR=freedreno ;; esac
	ninja -C $BUILD src/gallium/drivers/$AIR_DIR/air_run >> ninja-$ARCH.log
	cp $BUILD/src/gallium/drivers/$AIR_DIR/air_run $OUT/air_run
	echo "mk-mesa.sh: $B/$OUT/air_run ($(wc -c < $OUT/air_run) bytes)"
fi

$STRIP -x -o $OUT/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /hakios/lib/libOSMesa.8.dylib $OUT/libOSMesa.8.dylib
nm -gU $OUT/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > $OUT/exported-gl.txt
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml $OUT/gl.xml
# (userland/build.sh comes here again when this no longer matches)
cat "$ROOT"/userland/tools/mesa/*.patch "$ROOT"/userland/tools/mesa/hakios-drm/* | sha256sum > $OUT/built-from
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $B/$OUT/libOSMesa.8.dylib, $(wc -c < $OUT/libOSMesa.8.dylib | tr -d " ") bytes," \
	"$(wc -l < $OUT/exported-gl.txt) gl* functions"
