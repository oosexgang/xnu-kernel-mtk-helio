#!/bin/bash
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
B=$ROOT/build/mesa-host-mac
SRC=$B/mesa-$VERSION
mkdir -p "$B/out"
cd "$B"
PATCHES=$(cat "$ROOT"/userland/tools/mesa/*.patch | shasum -a 256 | cut -d' ' -f1)
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.hakios-patches" 2>/dev/null)" != "$PATCHES" ]; then
	if [ ! -f mesa-$VERSION.tar.xz ]; then
		ARCHIVE=$ROOT/kernel/deps/build/mesa/mesa-$VERSION.tar.xz
		[ -f "$ARCHIVE" ] || { echo "mk-host-mac.sh: no $ARCHIVE: run userland/tools/mk-mesa.sh once (it fetches it)" >&2; exit 1; }
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
[ -x "$VENV/bin/python3" ] || { echo "mk-host-mac.sh: no $VENV: run userland/tools/mk-mesa.sh once" >&2; exit 1; }
command -v meson >/dev/null && command -v ninja >/dev/null || { echo "mk-host-mac.sh: meson and ninja must be on the PATH" >&2; exit 1; }
export PATH=$VENV/bin:$PATH
[ -f build/build.ninja ] || meson setup build "$SRC" --wrap-mode=nofallback --buildtype=debugoptimized \
	-Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= -Dglx=disabled \
	-Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled \
	-Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
	-Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C build src/gallium/targets/osmesa/libOSMesa.8.dylib > ninja.log
cp build/src/gallium/targets/osmesa/libOSMesa.8.dylib out/
echo "mk-host-mac.sh: $B/out/libOSMesa.8.dylib"
