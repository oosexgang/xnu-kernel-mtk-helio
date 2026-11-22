#!/bin/bash
# Make build/payload.bin, what the loader starts: the kernel and the small root image
# (userland/root.apfs, or the file named by HAKIOS_RAMDISK), in the loader's "XNUP" container.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/kernel/env.sh"
K=$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
RD=${HAKIOS_RAMDISK:-$ROOT/userland/root.apfs}
[ -f "$K" ] || { echo "mkpayload.sh: no kernel at $K: kernel/build.sh makes it" >&2; exit 1; }
[ -f "$RD" ] || { echo "mkpayload.sh: no $RD: userland/build.sh makes it" >&2; exit 1; }
mkdir -p "$ROOT/build"
python3 "$ROOT/loader/mkpayload.py" -o "$ROOT/build/payload.bin" "kernel=$K" "ramdisk=$RD"
