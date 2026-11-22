#!/bin/bash
# Build build/loader/fbdiag.img: fbdiag in front of Android's kernel (your rooted boot image,
# backup/boot_a_magisk.img unless ANDROID_BOOT names another). The first thing a port runs
# (Documentation/porting.md, step 1). HAKIOS_BOARD=NAME when more than one phone's package is here.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
"$HERE/build.sh" > /dev/null                    # (the board's header, the font, the linker: as the loader's)
OUT=$ROOT/build/loader
BOARD=${HAKIOS_BOARD:-$("$ROOT/tools/boards.py" which)}
PLATFORM=$ROOT/build/boards/$BOARD
if [ -x "$ROOT/build/zig/zig" ]; then LLD=("$ROOT/build/zig/zig" ld.lld); else LLD=(ld.lld); fi
CFLAGS=(--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra
	-I"$PLATFORM" -I"$OUT")
LOAD=$(sed -n 's/^#define HAKIOS_LOADER_KERNEL_LOAD *\(0x[0-9a-f]*\)ULL$/\1/p' "$PLATFORM/hakios_platform.h")
clang "${CFLAGS[@]}" -c "$HERE/fbdiag_start.S" -o "$OUT/fbdiag_start.o"
clang "${CFLAGS[@]}" -c "$HERE/fbdiag.c" -o "$OUT/fbdiag.o"
"${LLD[@]}" --defsym=KERNEL_LOAD="$LOAD" -T "$HERE/link.ld" --oformat binary "$OUT/fbdiag_start.o" "$OUT/fbdiag.o" -o "$OUT/fbdiag.bin"
BOOT=${ANDROID_BOOT:-$ROOT/backup/boot_a_magisk.img}
python3 "$HERE/chainboot.py" "$BOOT" "$OUT/fbdiag.bin" "$OUT/fbdiag.img"
