#!/usr/bin/env python3
"""
mali-dump.py LOG [GENXML] : the descriptors of a failed (and of the last finished) fragment job, as the kernel kept them
("mali dump WHAT ADDRESS +OFFSET: HEX", bsd/dev/mtk_mali.c dump_fragment), decoded with Mesa's description of the
Mali's v7 (src/panfrost/lib/genxml/v7.xml): the job, the framebuffer's parameters, its ZS/CRC extension and each
render target, field by field, the bad and the good side by side where both are there.
"""
import os, re, sys
import xml.etree.ElementTree as ET

log = sys.argv[1]
xml = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', '..', 'kernel', 'deps', 'build',
                                                           'mesa', 'mesa-25.0.7', 'src', 'panfrost', 'lib', 'genxml',
                                                           'v7.xml')
root = ET.parse(xml).getroot()
structs = {s.get('name'): s for s in root.iter('struct')}
enums = {e.get('name'): {int(v.get('value'), 0): v.get('name') for v in e.iter('value')} for e in root.iter('enum')}

# the dumps: WHAT -> {offset: bytes}
dumps, notes = {}, []
for line in open(log, errors='replace'):
    m = re.search(r'mali dump (\S+) (0x[0-9a-f]+) \+(0x[0-9a-f]+|0): ([0-9a-f]+)', line)
    if m:
        dumps.setdefault(m.group(1), {})[int(m.group(3), 16)] = bytes.fromhex(m.group(4))
        continue
    m = re.search(r'mali dump (\S+): (.*)$', line)
    if m:
        if m.group(0) not in notes:
            notes.append(m.group(0).strip())
blobs = {}
for what, parts in dumps.items():
    data = bytearray()
    for off in sorted(parts):
        data[off:off + len(parts[off])] = parts[off]
    blobs[what] = bytes(data)


def bits(data, start, size):
    value = int.from_bytes(data[:max(0, (start + size + 7) // 8)].ljust((start + size + 7) // 8, b'\0'), 'little')
    return (value >> start) & ((1 << size) - 1)


def field_start(f):
    s = f.get('start')
    if ':' in s:
        w, b = s.split(':')
        return int(w) * 32 + int(b)
    return int(s)


def decode(name, data, indent='  '):
    out = []
    for f in structs[name].iter('field'):
        start, size, typ = field_start(f), int(f.get('size')), f.get('type')
        if typ == 'Render Target YUV Overlay':
            continue
        if typ in structs and size >= 32 and typ not in ('address',):
            out.append('%s%s:' % (indent, f.get('name')))
            out += decode(typ, data[start // 8:], indent + '  ')
            continue
        v = bits(data, start, size)
        mod = f.get('modifier') or ''
        if mod.startswith('minus('):
            v += int(mod[6:-1])
        elif mod == 'log2':
            v = 1 << v
        elif mod.startswith('shr('):
            v <<= int(mod[4:-1])
        if typ in enums:
            shown = '%s (%d)' % (enums[typ].get(v, '?'), v)
        elif typ == 'address':
            shown = '%#x' % v
        elif typ == 'float':
            import struct
            shown = '%g' % struct.unpack('<f', struct.pack('<I', v & 0xffffffff))[0]
        else:
            shown = str(v)
        out.append('%s%s: %s' % (indent, f.get('name'), shown))
    return out


for n in notes:
    print(n)
for side in sorted({w.split('-')[0] for w in blobs}):
    job, fbd = blobs.get(side + '-job'), blobs.get(side + '-fbd')
    print('==== %s' % side)
    if job:
        print(' Job Header'); print('\n'.join(decode('Job Header', job)))
        print(' Fragment Job Payload'); print('\n'.join(decode('Fragment Job Payload', job[32:])))
    if fbd:
        print(' Framebuffer Parameters'); print('\n'.join(decode('Framebuffer Parameters', fbd)))
        params = fbd
        rts = bits(params, 11 * 32 + 19, 4) + 1
        has_zs = bits(params, 12 * 32 + 21, 1)
        at = 128
        if has_zs:
            print(' ZS CRC Extension'); print('\n'.join(decode('ZS CRC Extension', fbd[at:])))
            at += 64
        for k in range(rts):
            if at + 64 > len(fbd):
                print(' (render target %d is past the dump)' % k)
                break
            print(' Render Target %d' % k); print('\n'.join(decode('Render Target', fbd[at:])))
            at += 64
    # what the framebuffer points at (bsd/dev/mtk_mali.c dump_fragment, from run 285 on)
    parts = [('tiler', 'Tiler Context', 64, 1), ('dcd', 'Draw', 128, 3), ('rsd', 'Renderer State', 64, 1),
             ('tex', 'Texture', 32, 4), ('smp', 'Sampler', 32, 2), ('tls', 'Local Storage', 32, 1)]
    for suffix, struct_name, step, count in parts:
        blob = blobs.get(side + '-' + suffix)
        if not blob or struct_name not in structs:
            continue
        for k in range(count):
            if (k + 1) * step > len(blob):
                break
            print(' %s %d (%s)' % (struct_name, k, suffix)); print('\n'.join(decode(struct_name, blob[k * step:])))
        if suffix == 'rsd' and len(blob) > 64:
            for k in range(3):
                if 64 + (k + 1) * 16 <= len(blob) and 'Blend' in structs:
                    print(' Blend %d' % k); print('\n'.join(decode('Blend', blob[64 + k * 16:])))
    fault = blobs.get(side + '-fault')
    if fault:
        # 128 bytes before the fault pointer and 128 from it: as texture and sampler descriptors, the 32-byte ones a
        # pointer aligned so would name
        for k in range(len(fault) // 32):
            off = k * 32 - 128
            print(' at fault %+d: %s' % (off, fault[k * 32:k * 32 + 32].hex()))
        print(' Texture at the fault'); print('\n'.join(decode('Texture', fault[128:])))
        print(' Sampler at the fault'); print('\n'.join(decode('Sampler', fault[128:])))
    sh = blobs.get(side + '-shader')
    if sh:
        print(' shader: %s' % sh[:64].hex())
