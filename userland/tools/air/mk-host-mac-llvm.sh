#!/bin/bash
# mk-host-mac-llvm.sh: userland/tools/air/mk-host-mac.sh with llvmpipe as well -- softpipe draws no 16-bit shader, and every one of
# the 27 release's CoreAnimation shaders (*_lph) is 16-bit, so a captured pipeline (userland/tools/air/ca27_replay.c) can be drawn
# on this Mac only by llvmpipe (GALLIUM_DRIVER=llvmpipe RWA_SOFTWARE=1). LLVM is LLVM_DIR, an LLVM release for this
# Mac's architecture with llvm-config and static libraries (default: ../llvm19/LLVM-19.1.7-macOS-X64 beside the tree).
# Out: build/mesa-host-mac-llvm/out/libOSMesa.8.dylib.
#
# What follows is mk-host-mac.sh's own description.
# Build Mesa for THIS Mac (macOS 14 and on, the machine's own clang) with the AIR frontend and the Metal library
# (rwa_*, userland/tools/mesa/air/air_metal.c) in it, drawing on softpipe: the library the phone's window server draws through,
# run without the phone. userland/tools/air/mk-host.sh is the same for a Linux machine; this one takes userland/tools/mk-mesa.sh's
# sources and patches as they are (hakios-drm stands in for libdrm, as on the phone) and builds natively.
#
# Nothing is fetched when userland/tools/mk-mesa.sh has run once: its archive is taken from kernel/deps/build/mesa.
#
# What comes out, in build/mesa-host-mac/out:
#   libOSMesa.8.dylib     the library with the AIR frontend and rwa_* on softpipe (panfrost is built as on the phone, and
#                         finds no GPU here); softpipe and panfrost only, as the phone's, since freedreno's ir3 parser
#                         wants a newer bison than macOS has
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
B=$ROOT/build/mesa-host-mac-llvm
LLVM_DIR=${LLVM_DIR:-$(cd "$ROOT/.." && pwd)/llvm19/LLVM-19.1.7-macOS-X64}
[ -x "$LLVM_DIR/bin/llvm-config" ] || { echo "mk-host-mac-llvm.sh: no $LLVM_DIR/bin/llvm-config" >&2; exit 1; }
SRC=$B/mesa-$VERSION
mkdir -p "$B/out"
cd "$B"
PATCHES=$(cat "$ROOT"/userland/tools/mesa/*.patch | shasum -a 256 | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.hakios-patches" 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		ARCHIVE=$ROOT/kernel/deps/build/mesa/mesa-$VERSION.tar.xz
		[ -f "$ARCHIVE" ] || { echo "mk-host-mac-llvm.sh: no $ARCHIVE: run userland/tools/mk-mesa.sh once (it fetches it)" >&2; exit 1; }
		cp "$ARCHIVE" .
	fi
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf "$SRC" build
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/userland/tools/mesa/*.patch; do
		patch -s --no-backup-if-mismatch -d "$SRC" -p1 < "$p"
	done
	echo "$PATCHES" > "$SRC/.hakios-patches"
fi
# ours, always the current ones (as userland/tools/mk-mesa.sh does)
rm -rf "$SRC/src/hakios-drm" "$SRC/src/compiler/air"
cp -r "$ROOT/userland/tools/mesa/hakios-drm" "$SRC/src/hakios-drm"
cp -r "$ROOT/userland/tools/mesa/air" "$SRC/src/compiler/air"
# Python with mako, as userland/tools/mk-mesa.sh made it; meson and ninja from the PATH. Nothing is installed here.
VENV=$ROOT/kernel/deps/build/mesa/venv
[ -x "$VENV/bin/python3" ] || { echo "mk-host-mac-llvm.sh: no $VENV: run userland/tools/mk-mesa.sh once" >&2; exit 1; }
command -v meson >/dev/null && command -v ninja >/dev/null || { echo "mk-host-mac-llvm.sh: meson and ninja must be on the PATH" >&2; exit 1; }
export PATH=$VENV/bin:$PATH
# LLVM's own static libraries in such a release are LTO bitcode of that LLVM (0B17C0DE), which the Command Line Tools'
# linker cannot read: Mesa is compiled and linked with that release's clang and ld64.lld. It was also built against a
# zstd that is not on this Mac (llvm-config --system-libs: /usr/local/lib/libzstd.a); LLVM uses it only to compress
# debug sections, so eight stubs that say no stand in for it, through an llvm-config that names them instead.
SDK=$(/usr/bin/xcrun --show-sdk-path)
cat > zstd-stub.c <<'C'
#include <stddef.h>
typedef struct ZSTD_CCtx_s ZSTD_CCtx;
size_t ZSTD_compressBound(size_t s) { return s + 64; }
unsigned ZSTD_isError(size_t c) { return c > (size_t)-120; }
const char *ZSTD_getErrorName(size_t c) { (void)c; return "zstd is not in this build"; }
size_t ZSTD_decompress(void *d, size_t dc, const void *s, size_t sc) { (void)d; (void)dc; (void)s; (void)sc; return (size_t)-1; }
ZSTD_CCtx *ZSTD_createCCtx(void) { return NULL; }
size_t ZSTD_freeCCtx(ZSTD_CCtx *c) { (void)c; return 0; }
size_t ZSTD_CCtx_setParameter(ZSTD_CCtx *c, int p, int v) { (void)c; (void)p; (void)v; return (size_t)-1; }
size_t ZSTD_compress2(ZSTD_CCtx *c, void *d, size_t dc, const void *s, size_t sc) { (void)c; (void)d; (void)dc; (void)s; (void)sc; return (size_t)-1; }
C
"$LLVM_DIR/bin/clang" -isysroot "$SDK" -O2 -c zstd-stub.c -o zstd-stub.o && rm -f libzstd-stub.a && \
	"$LLVM_DIR/bin/llvm-ar" rcs libzstd-stub.a zstd-stub.o
cat > llvm-config <<SH
#!/bin/sh
out=\$("$LLVM_DIR/bin/llvm-config" "\$@") || exit \$?
printf '%s\\n' "\$out" | sed "s#/usr/local/lib/libzstd.a#$B/libzstd-stub.a#g"
SH
chmod +x llvm-config
cat > llvm-native.ini <<INI
[binaries]
llvm-config = '$B/llvm-config'
c = ['$LLVM_DIR/bin/clang', '-isysroot', '$SDK']
cpp = ['$LLVM_DIR/bin/clang++', '-isysroot', '$SDK']
objc = ['$LLVM_DIR/bin/clang', '-isysroot', '$SDK']
[built-in options]
c_link_args = ['-fuse-ld=lld', '-lc++']
cpp_link_args = ['-fuse-ld=lld', '-lc++', '-lc++abi']
objc_link_args = ['-fuse-ld=lld']
INI
[ -f build/build.ninja ] || meson setup build "$SRC" --native-file llvm-native.ini --wrap-mode=nofallback \
	--buildtype=debugoptimized -Dplatforms= -Dgallium-drivers=llvmpipe,softpipe,panfrost -Dvulkan-drivers= -Dglx=disabled \
	-Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=enabled -Dshared-llvm=disabled -Dcpp_rtti=false -Db_asneeded=false -Dshared-glapi=enabled -Dgles1=disabled \
	-Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
	-Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C build src/gallium/targets/osmesa/libOSMesa.8.dylib > ninja.log
cp build/src/gallium/targets/osmesa/libOSMesa.8.dylib out/
echo "mk-host-mac-llvm.sh: $B/out/libOSMesa.8.dylib"
