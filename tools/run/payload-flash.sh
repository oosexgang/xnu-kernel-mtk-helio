#!/bin/bash
# Write build/payload.bin (the kernel and the small root) into the tail of the phone's system partition, where the
# loader reads it when the boot image has none of its own (HAKIOS_PAYLOAD=flash tools/run/xnu-cycle.sh): boot_a then
# holds only the loader and Android, whatever the kernel's size. The last 96 MiB of the partition are the payload's:
# a 512-byte header ("HKPL", version 1, the size, a Fletcher-64 of the 32-bit words) and the payload after it,
# read back and compared before the script says it is done. Needs Android up with root, as tools/run/system-flash.sh.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD=${HAKIOS_BOARD:-$("$ROOT/tools/boards.py" which)}
PART=$("$ROOT/tools/boards.py" get "$BOARD" loader.system_partition)
P="${1:-$ROOT/build/payload.bin}"
[ -f "$P" ] || { echo "payload-flash.sh: no $P: tools/run/mkpayload.sh makes it"; exit 1; }
DEV=/dev/block/by-name/$PART
BYTES=$(adb -s "$S" shell "su -c 'blockdev --getsize64 $DEV'" | tr -d '\r')
case "$BYTES" in ''|*[!0-9]*) echo "payload-flash.sh: cannot read the size of $DEV ($BYTES)"; exit 1 ;; esac
TAIL=$((96 * 1024 * 1024))
[ "$BYTES" -ge $((2 * TAIL)) ] || { echo "payload-flash.sh: $PART is too small ($BYTES bytes)"; exit 1; }
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 - "$P" "$OUT/tail.bin" "$TAIL" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
data += b'\0' * (-len(data) % 4)
if len(data) > int(sys.argv[3]) - 512:
    sys.exit(f'payload-flash.sh: the payload is {len(data)} bytes, more than the tail holds')
a = b = 0
for (w,) in struct.iter_unpack('<I', data):
    a = (a + w) % 0xffffffff
    b = (b + a) % 0xffffffff
head = struct.pack('<IIQQ', 0x4c504b48, 1, len(data), (b << 32) | a).ljust(512, b'\0')
open(sys.argv[2], 'wb').write(head + data)
print(f'payload-flash.sh: {len(data)} bytes, sum {(b << 32) | a:016x}')
PY
SEEK=$(( (BYTES - TAIL) / 512 ))
TMP=/data/local/tmp/hakios-payload.bin
adb -s "$S" push "$OUT/tail.bin" $TMP > /dev/null
adb -s "$S" shell "su -c 'dd if=$TMP of=$DEV bs=512 seek=$SEEK conv=notrunc 2>/dev/null; sync'"
N=$(( $(wc -c < "$OUT/tail.bin") ))
GOT=$(adb -s "$S" shell "su -c 'dd if=$DEV bs=512 skip=$SEEK count=$(( (N + 511) / 512 )) 2>/dev/null | head -c $N | sha256sum'" | cut -d' ' -f1)
WANT=$(shasum -a 256 "$OUT/tail.bin" | cut -d' ' -f1)
adb -s "$S" shell "rm -f $TMP"
[ "$GOT" = "$WANT" ] || { echo "payload-flash.sh: what was read back is not what was written"; exit 1; }
echo "payload-flash.sh: in the last 96 MiB of $PART, read back and the same"
