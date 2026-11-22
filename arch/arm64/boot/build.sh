#!/bin/bash
# Build the loader for one board: build/loader/mtkloader.bin. MMU off means device memory: no SIMD,
# strict alignment, no builtins. Extra arguments go to the compiler (e.g. -DXNU_WDT_SECONDS=0).
#
#   HAKIOS_BOARD=NAME   the board (a device package in arch/arm64/boot/dts/); not needed when one phone is there
#   TOUCH_FW=FILE       the touch controller's firmware image from your own phone (default
#                       fw-touch/hdl_firmware.img); without it the loader carries none
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT=$ROOT/build/loader
BOARD=${HAKIOS_BOARD:-$("$ROOT/tools/boards.py" which)}
"$ROOT/tools/boards.py" gen > /dev/null
PLATFORM=$ROOT/build/boards/$BOARD
[ -f "$PLATFORM/hakios_platform.h" ] || { echo "build.sh: no board \"$BOARD\" with a [loader] section" >&2; exit 1; }
mkdir -p "$OUT"

# an ELF linker: zig's (tools/get-zig.sh; macOS has none of its own), or ld.lld of this machine
if [ -x "$ROOT/build/zig/zig" ]; then LLD=("$ROOT/build/zig/zig" ld.lld)
elif command -v ld.lld >/dev/null; then LLD=(ld.lld)
else echo "build.sh: no ld.lld: run tools/get-zig.sh" >&2; exit 1; fi

# What is the phone's or this machine's and never the repository's: made here, each time it is missing.
FW=${TOUCH_FW:-$ROOT/fw-touch/hdl_firmware.img}
if [ -f "$FW" ]; then
	[ "$OUT/touchfw.h" -nt "$FW" ] || python3 "$ROOT/tools/mktouchfw.py" "$FW" ROMBOOT_APP_CODE "$OUT/touchfw.h" > /dev/null
else
	echo "build.sh: no $FW: the loader is built without the touch controller's firmware"
	printf '%s\n' '#ifndef TOUCHFW_H' '#define TOUCHFW_H' '#include <stdint.h>' '#define TOUCH_FW_SIZE 0u' \
		'#define TOUCH_FW_FLAG 0x0000u' 'static const uint8_t touch_fw[1] = { 0 };' '#endif' > "$OUT/touchfw.h"
fi
if [ ! -f "$OUT/font8x16.h" ]; then
	if [ "$(uname -s)" = Darwin ] && swift "$ROOT/tools/mkfont8x16.swift" > "$OUT/font8x16.h.new" 2>/dev/null; then
		mv "$OUT/font8x16.h.new" "$OUT/font8x16.h"
	else
		rm -f "$OUT/font8x16.h.new"
		echo "build.sh: no font could be made on this machine: the loader's text on the panel is blank (its log is whole)"
		echo 'static const unsigned char font8x16[95][16] = { { 0 } };' > "$OUT/font8x16.h"
	fi
fi

CFLAGS=(--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra
	-I"$PLATFORM" -I"$OUT")
LOAD=$(sed -n 's/^#define HAKIOS_LOADER_KERNEL_LOAD *\(0x[0-9a-f]*\)ULL$/\1/p' "$PLATFORM/hakios_platform.h")
clang "${CFLAGS[@]}" "$@" -c "$HERE/start.S" -o "$OUT/start.o"
clang "${CFLAGS[@]}" "$@" -c "$HERE/mtkloader.c" -o "$OUT/mtkloader.o"
"${LLD[@]}" --defsym=KERNEL_LOAD="$LOAD" -T "$HERE/link.ld" --oformat binary "$OUT/start.o" "$OUT/mtkloader.o" -o "$OUT/mtkloader.bin"
echo "build.sh: $BOARD: $(wc -c < "$OUT/mtkloader.bin" | tr -d ' ') bytes in build/loader/mtkloader.bin"
