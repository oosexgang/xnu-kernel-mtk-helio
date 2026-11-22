#!/bin/bash
# The host tests: everything that can be checked on this machine, with no phone and no kernel toolchain.
#   - the tools (workbench/tools_test.py): the TOML reader, the board files' checks, the generated tables
#   - the loader compiles for the stand-in phone of workbench/devices (syntax and types; nothing is linked),
#     so that a key the loader uses and the generator does not write is found here
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
python3 workbench/tools_test.py
python3 workbench/bootargs_test.py   # (docs/boot-args.md is what tools/bootargs.py writes: `make docs`)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
tools/boards.py --alone --devices workbench/devices --soc-dir workbench/soc gen --out "$out" > /dev/null
if command -v clang > /dev/null; then
	printf '%s\n' '#include <stdint.h>' '#define TOUCH_FW_SIZE 0u' '#define TOUCH_FW_FLAG 0u' 'static const uint8_t touch_fw[1] = { 0 };' > "$out/touchfw.h"
	echo 'static const unsigned char font8x16[95][16] = { { 0 } };' > "$out/font8x16.h"
	clang --target=aarch64-linux-gnu -ffreestanding -fno-builtin -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra -Werror \
		-Wno-unused-function -fsyntax-only -I"$out/phone-a" -I"$out" loader/mtkloader.c
	echo "tests: the loader compiles for the stand-in phone"
else
	echo "tests: no clang on this machine: the loader was not compiled"
fi

# the drivers: each test #includes one driver's .c file and runs it against workbench/fakekernel (the kernel
# functions it calls, made of ordinary C) and, where it talks to hardware, a model of that hardware.
# A test lists its scenarios when run without one; each runs in its own process.
HERE=$PWD/workbench
# the kernel's files of ours at their paths in the tree (drivers/board is bsd/dev, include/board bsd/sys: kernel/lib.sh),
# so that a driver's includes are found as the kernel's build finds them
# shellcheck source=../kernel/lib.sh
source kernel/lib.sh
overlay_view "$out/tree"
SRC=$out/tree/bsd/dev
CC=${CC:-cc}
DRIVER_FLAGS=(-std=gnu11 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter -O2 -pthread
	-I"$HERE" -I"$HERE/fakekernel" -I"$SRC" -I"$SRC/.." -I"$out" "$HERE/fakekernel/fakekernel.c")
failed=0
for test in input_queue_test station_test apple80211_test sparse_driver_test guard_test audio_device_test battery_driver_test keys_driver_test example_pwrkey_test; do
	source=$HERE/$test.c
	[ -f "$source" ] || source=$PWD/docs/examples/$test.c    # (the example driver lives beside its guide)
	echo "== $test"
	if ! "$CC" "${DRIVER_FLAGS[@]}" -I"$PWD/docs/examples" "$source" -o "$out/$test"; then
		failed=1
		continue
	fi
	list=$("$out/$test")
	if [ -z "$list" ]; then
		echo "FAIL: $test lists no scenario"
		failed=1
	fi
	for scenario in $list; do
		"$out/$test" "$scenario" || failed=1
	done
done
[ "$failed" = 0 ] || { echo "tests: FAILED (above)"; exit 1; }
echo "tests: ok"
