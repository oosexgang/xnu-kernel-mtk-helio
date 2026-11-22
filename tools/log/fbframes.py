#!/usr/bin/env python3
"""The pictures of boot-arg fbframes= out of the dump records tools/run/xnu-cycle.sh took (LOG.frames/dmesg-ramoops-N).

Each record mtkloader wrote begins "HKFR" and its number (4 bytes, little-endian), then a piece of the kernel's
hakios_host_frames buffer; put back in order they are pictures, each a 16-byte head ("HKFP", width and height as
16-bit, uptime and byte count as 32-bit, little-endian) and RGB565 pixels. Records without the mark (a dump of
Android's own) are left out.

    tools/log/fbframes.py logs/ios27-runNNN.log.frames logs/frNNN      ->  logs/frNNN-<uptime>s.png
    tools/log/fbframes.py A.log.frames,B.log.frames logs/frAB          (the records of two runs of one picture together)
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
    # DIR[,DIR...]: the records of several runs of the same picture, put together -- Android's boot overwrites some
    # of the dump zone's records, not the same ones each time (runs 311, 313)
    for f in [f for d in sys.argv[1].split(',') for f in sorted(Path(d).glob('dmesg-ramoops-*'))]:
        data = f.read_bytes()
        # after Android's own "====SEC.USEC-C" line, which mtkloader writes so that the record is not zapped
        at = data.index(b'\n') + 1 if data.startswith(b'====') and b'\n' in data[:32] else 0
        if data[at:at + 4] != b'HKFR' or len(data) < at + 8:
            continue
        number = struct.unpack_from('<I', data, at + 4)[0]
        pieces.setdefault(number, data[at + 8:])
    if not pieces:
        sys.exit('fbframes.py: no record with the HKFR mark')
    if any(scatter_pieces(d) for d in sys.argv[1].split(',')):
        return scatter_main()
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


RECORD = 4066   # the payload of one of mtkloader's records (dmesg-ramoops-N)


def records_of(d):
    out = {}
    for f in sorted(Path(d).glob('dmesg-ramoops-*')):
        data = f.read_bytes()
        at = data.index(b'\n') + 1 if data.startswith(b'====') and b'\n' in data[:32] else 0
        if data[at:at + 4] == b'HKFR' and len(data) >= at + 8:
            out[struct.unpack_from('<I', data, at + 4)[0]] = data[at + 8:]
    return out


def scatter_pieces(d):
    """fbframes_scatter=1: the pieces ("HKFQ", with the panel row each starts at) at their true places in the buffer,
    records Android overwrote left as holes: [(row, width, height, uptime, rgb565 bytes)]"""
    recs = records_of(d)
    if not recs:
        return []
    buf = bytearray(RECORD * (max(recs) + 1))
    have = bytearray(len(buf))
    for k, payload in recs.items():
        buf[k * RECORD:k * RECORD + len(payload)] = payload
        have[k * RECORD:k * RECORD + len(payload)] = b'\1' * len(payload)
    found, at = [], 0
    while True:
        at = buf.find(b'HKFQ', at)
        if at < 0:
            break
        width, height, uptime, size, row = struct.unpack_from('<HHIII', buf, at + 4)
        body = at + 20
        if width and height and size == width * height * 2 and all(have[body:body + size]):
            found.append((row, width, height, uptime, bytes(buf[body:body + size])))
            at = body + size
        else:
            at += 4
    return found


def scatter_main():
    """the rows of every run given put into one picture of the panel's width, from the first row to the last any run
    brought; rows nobody brought are magenta"""
    rows = {}
    width = 0
    for d in sys.argv[1].split(','):
        for row, w, h, uptime, px in scatter_pieces(d):
            width = width or w
            if w != width:
                continue
            for y in range(h):
                rows.setdefault(row + y, px[y * w * 2:(y + 1) * w * 2])
    if not rows:
        sys.exit('fbframes.py: no scattered piece came back whole')
    lo, hi = min(rows), max(rows) + 1
    rgb = bytearray()
    missing = 0
    for y in range(lo, hi):
        line = rows.get(y)
        if line is None:
            missing += 1
            rgb += b'\xff\x00\xff' * width
            continue
        for i in range(0, len(line), 2):
            v = line[i] | line[i + 1] << 8
            r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
            rgb += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    out = f'{sys.argv[2]}.png'
    png(out, width, hi - lo, bytes(rgb))
    print(f'fbframes.py: {width}x{hi - lo} (panel rows {lo} to {hi}), {missing} rows missing: {out}')


if __name__ == '__main__':
    main()
