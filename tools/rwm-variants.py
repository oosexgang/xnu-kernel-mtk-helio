#!/usr/bin/env python3
"""
rwm-variants.py LOG... : the configurations of the generic pipelines' variants a run built ("rwm variant LABEL #N
built in T ms: vHEX fHEX", src/metal/pipeline.cpp) merged into userland/share/rwm-variants27.txt (RWM_VARIANTS_OUT=FILE:
another), a line each, "LABEL vHEX fHEX". The window server builds them ahead from it (prebuild_main) instead of at the
draws, where each stopped a frame for 1 to 4 s. Configurations already in the file are kept; refused ones are left out.
"""
import os
import re
import sys

OUT = os.environ.get('RWM_VARIANTS_OUT') or os.path.join(os.path.dirname(__file__), '..', 'userland', 'share',
                                                          'rwm-variants27.txt')
line_re = re.compile(r'rwm variant (\S+) #\d+ built in \d+ ms: v([0-9a-f]*) f([0-9a-f]+)')

known = []
if os.path.exists(OUT):
    for l in open(OUT):
        l = l.strip()
        if l and not l.startswith('#'):
            known.append(l)
added = 0
for log in sys.argv[1:]:
    for raw in open(log, 'rb'):
        m = line_re.search(raw.decode('utf-8', 'replace'))
        if m:
            entry = f'{m.group(1)} v{m.group(2)} f{m.group(3)}'
            if entry not in known:
                known.append(entry)
                added += 1
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, 'w') as f:
    f.write('# generic pipelines\' variants the window server builds ahead (tools/rwm-variants.py, src/metal/pipeline.cpp)\n')
    for e in known:
        f.write(e + '\n')
print(f'{OUT}: {len(known)} configurations ({added} new)')
