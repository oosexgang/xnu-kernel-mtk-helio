#!/bin/bash
# One XNU run on the first phone: build mtkloader (extra args go to the compiler, e.g.
# -DXNU_CHECKPOINT=17), pack it with Android and the XNU payload, flash boot_a, let XNU run and
# Android come back, then print what XNU left in pstore. Slot b is never touched.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
# test settings: XNU picked by default, watchdog and watcher on, XNU comes back by itself
# XNU_SECS: how long the kernel runs before it hands the log back (the watcher waits a bit longer)
# (a plain run, without XNU_SECS or XNU_ARGS, is the board file's [boot]: its seconds, cpus and bootargs)
BOARD=${HAKIOS_BOARD:-$("$ROOT/tools/boards.py" which)}
SECS=${XNU_SECS:-$("$ROOT/tools/boards.py" get "$BOARD" boot.seconds || echo 45)}
# XNU_ARGS: the kernel's own command line (cores and so on); the clock comes from this machine
# XNU_DEFCONFIG=NAME: the arguments of arch/arm64/configs/NAME_defconfig (tools/defconfig.py --list), with
# XNU_ARGS replacing or adding to them
if [ -n "${XNU_DEFCONFIG:-}" ]; then
	# (XNU_ARGS split into its arguments on purpose)
	# shellcheck disable=SC2086
	XNU_ARGS=$(python3 "$ROOT/tools/defconfig.py" "$XNU_DEFCONFIG" ${XNU_ARGS:-}) || exit 1
fi
if [ -z "${XNU_ARGS:-}" ]; then
	XNU_ARGS="cpus=$("$ROOT/tools/boards.py" get "$BOARD" boot.cpus || echo 1) $("$ROOT/tools/boards.py" get "$BOARD" boot.bootargs || true)"
fi
# the device tree offers the kernel exactly this many cores; "cpus=" in XNU_ARGS should match
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
WATCH=${XNU_WATCH:-$((SECS + 240))}
# No MENU_DEFAULT_XNU: the image boots Android, and XNU is asked for once (tools/run/boot-xnu.sh), so
# a kernel that hangs costs a power cycle and not a rescue through BROM.
TEST_FLAGS=(-DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"")
for tool in adb fastboot python3; do
	command -v $tool >/dev/null || { echo "xnu-cycle.sh: $tool is not in PATH"; exit 1; }
done
[ -f "$ROOT/build/payload.bin" ] || { echo "xnu-cycle.sh: no build/payload.bin: make it with tools/run/mkpayload.sh"; exit 1; }
mkdir -p "$(dirname "$OUT")"
# the phone must be the one the board file describes (its [boot] android_device), when Android is up to ask
WANT=$("$ROOT/tools/boards.py" get "$BOARD" boot.android_device || true)
if [ -n "$WANT" ] && [ "$(adb -s $S get-state 2>/dev/null)" = device ]; then
	GOT=$(adb -s $S shell getprop ro.product.device | tr -d '\r')
	[ "$GOT" = "$WANT" ] || { echo "xnu-cycle.sh: the phone says it is \"$GOT\", the board $BOARD is \"$WANT\": not flashing it"; exit 1; }
fi
"$ROOT/arch/arm64/boot/build.sh" "${TEST_FLAGS[@]}" "$@" >/dev/null
python3 "$ROOT/arch/arm64/boot/build_flashed.py" >/dev/null
# HAKIOS_PAYLOAD=flash: the payload goes into the system partition's tail (Android is up now), not into boot_a
if [ "${HAKIOS_PAYLOAD:-}" = flash ]; then
	[ "$(adb -s $S get-state 2>/dev/null)" = device ] || { echo "xnu-cycle.sh: HAKIOS_PAYLOAD=flash needs Android up"; exit 1; }
	"$ROOT/tools/run/payload-flash.sh"
fi
[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
for i in $(seq 1 120); do fastboot devices | grep -q $S && break; sleep 1; done
# (fastboot -s waits for ever for a phone that is not there: a run would hang for hours)
fastboot devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
fastboot -s $S flash boot_a "$ROOT/build/boot_a.img" >/dev/null 2>&1
fastboot -s $S reboot >/dev/null 2>&1
echo "waiting for Android, to ask it for one XNU boot..."
for i in $(seq 1 100); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 4
"$ROOT/tools/run/boot-xnu.sh"
echo "XNU is running; Android comes back by itself"
WAIT_ITERS=$(( (SECS + 300) / 3 ))
for i in $(seq 1 $WAIT_ITERS); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 3
for i in $(seq 1 10); do adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$OUT" 2>/dev/null && [ -s "$OUT" ] && break; sleep 2; done
# what the kernel kept for the host (kern.hakios.host_keep: the pipeline recipes, whole however long the run was),
# which mtkloader put into the pmsg zone: appended to the log, when it is this run's and not Android's own
adb -s $S shell 'su -c "cat /sys/fs/pstore/pmsg-ramoops-0"' > "$OUT.keep" 2>/dev/null || true
if head -c 64 "$OUT.keep" | grep -q 'kept for the host by the kernel'; then
	{ echo; cat "$OUT.keep"; } >> "$OUT"
	echo "kept for the host: $(head -1 "$OUT.keep" | sed 's/^=== kept for the host by the kernel: //; s/ ===$//')"
fi
rm -f "$OUT.keep"
# the pictures of boot-arg fbframes=, which mtkloader laid over ramoops' dump zone (dmesg-ramoops-N, records that begin
# "HKFR"): each record as it is, into $OUT.frames/, for tools/log/fbframes.py
if [ -n "$(adb -s $S shell 'su -c "ls /sys/fs/pstore/"' 2>/dev/null | tr -d '\r' | grep '^dmesg-ramoops-')" ]; then
	mkdir -p "$OUT.frames"
	for f in $(adb -s $S shell 'su -c "ls /sys/fs/pstore/"' 2>/dev/null | tr -d '\r' | grep '^dmesg-ramoops-'); do
		# (as base64: su's terminal turns a 0x0a byte into 0x0d 0x0a in a binary file)
		adb -s $S shell "su -c 'base64 /sys/fs/pstore/$f'" 2>/dev/null | tr -d '\r' | base64 -D > "$OUT.frames/$f" 2>/dev/null || true
	done
	echo "frames: $(ls "$OUT.frames" | wc -l | tr -d ' ') dump records in $OUT.frames (tools/log/fbframes.py)"
fi
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
