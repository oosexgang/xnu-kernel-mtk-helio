#!/usr/bin/env python3
"""
texshot.py LOG [OUT.png]: the textures boot-arg rwm_texshot=WxH or =N read back and wrote into LOG ("texshot#i k: ..."
lines, userland/src/metal/texture.cpp), each as a PNG eight times as big (OUT-i.png), over a grey checkerboard so that what is transparent
shows as the board and not as black. The "texshot begin" line (how many of its pixels have alpha) is printed too.
"""
import base64
import re
import struct
import sys
import zlib

text = open(sys.argv[1], 'rb').read().decode('latin-1')
out = sys.argv[2] if len(sys.argv) > 2 else 'texshot.png'
S = 8


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


heads = {}
for m in re.finditer(r'texshot#(\d+) begin (\d+) (\d+)([^\n]*)', text):
    heads.setdefault(int(m.group(1)), m)
if not heads:
    sys.exit('texshot.py: no "texshot#i begin" line in the log')
for i, begin in sorted(heads.items()):
    print('texshot#%d %s' % (i, begin.group(4).strip()))
    w, h = int(begin.group(2)), int(begin.group(3))
    rows = {}
    for m in re.finditer(r'texshot#%d (\d+): (\S+)' % i, text):
        rows.setdefault(int(m.group(1)), base64.b64decode(m.group(2)))
    data = b''.join(rows[k] for k in sorted(rows))
    if len(data) < w * h * 4:
        print('  %d of %d bytes; the rest drawn as zero' % (len(data), w * h * 4))
        data += bytes(w * h * 4 - len(data))
    raw = bytearray()
    for y in range(h * S):
        raw.append(0)
        for x in range(w * S):
            o = ((y // S) * w + x // S) * 4
            r, g, b, a = data[o:o + 4]
            board = 160 if ((x // 4) + (y // 4)) % 2 else 96
            raw += bytes(int(c * a / 255 + board * (255 - a) / 255) for c in (r, g, b))
    name = out[:-4] + '-%d.png' % i if out.endswith('.png') else '%s-%d.png' % (out, i)
    with open(name, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * S, h * S, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))
    print('  %s' % name)
