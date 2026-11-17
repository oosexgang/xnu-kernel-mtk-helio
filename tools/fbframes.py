#!/usr/bin/env python3
"""The pictures of boot-arg fbframes= out of the dump records tools/xnu-cycle.sh took (LOG.frames/dmesg-ramoops-N).

Each record mtkloader wrote begins "HKFR" and its number (4 bytes, little-endian), then a piece of the kernel's
hakios_host_frames buffer; put back in order they are pictures, each a 16-byte head ("HKFP", width and height as
16-bit, uptime and byte count as 32-bit, little-endian) and RGB565 pixels. Records without the mark (a dump of
Android's own) are left out.

    tools/fbframes.py logs/ios27-runNNN.log.frames logs/frNNN      ->  logs/frNNN-<uptime>s.png
"""
import struct
import sys
import zlib
from pathlib import Path


def png(path, width, height, rgb):
    rows = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pieces = {}
    for f in sorted(Path(sys.argv[1]).glob('dmesg-ramoops-*')):
        data = f.read_bytes()
        # after Android's own "====SEC.USEC-C" line, which mtkloader writes so that the record is not zapped
        at = data.index(b'\n') + 1 if data.startswith(b'====') and b'\n' in data[:32] else 0
        if data[at:at + 4] != b'HKFR' or len(data) < at + 8:
            continue
        number = struct.unpack_from('<I', data, at + 4)[0]
        pieces[number] = data[at + 8:]
    if not pieces:
        sys.exit('fbframes.py: no record with the HKFR mark')
    buf = b''.join(pieces[k] for k in sorted(pieces))
    missing = [k for k in range(max(pieces) + 1) if k not in pieces]
    if missing:
        print(f'fbframes.py: records {missing} are missing; the pictures after them are cut', file=sys.stderr)
    at, count = 0, 0
    while at + 16 <= len(buf) and buf[at:at + 4] == b'HKFP':
        width, height, uptime, size = struct.unpack_from('<HHII', buf, at + 4)
        pixels = buf[at + 16:at + 16 + size]
        if len(pixels) < size:
            print(f'fbframes.py: the picture at {uptime} s is cut ({len(pixels)} of {size} bytes)', file=sys.stderr)
            break
        rgb = bytearray()
        for i in range(0, size, 2):
            v = pixels[i] | pixels[i + 1] << 8
            r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
            rgb += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
        out = f'{sys.argv[2]}-{uptime}s.png'
        png(out, width, height, bytes(rgb))
        print(f'fbframes.py: {width}x{height} at {uptime} s: {out}')
        at += 16 + size
        count += 1
    if count == 0:
        sys.exit('fbframes.py: no picture in the records')


if __name__ == '__main__':
    main()
