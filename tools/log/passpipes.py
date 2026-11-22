#!/usr/bin/env python3
"""
passpipes.py LOG: the frame log of boot-arg rwm_passlog (lines "P b|c|d|e|r ...", userland/src/metal/render.cpp) with
each draw's pipeline as boot-arg rwm_pipelog=1 described it ("pl LABEL w.. f.. b.. fmt.. N=..", userland/src/metal/
pipeline.cpp): which colour outputs its fragment shader stores to (w), which targets it fetches (f), which attachments
blend (b). One line per draw, after its pass's attachments. The kernel keeps a "!!" line without the "!!".
"""
import re
import sys

text = open(sys.argv[1], 'rb').read().decode('latin-1')
pipes = {}
for m in re.finditer(r'(?:^|\s)pl (\S+) (w\S+ f\S+ b\S+[^\r\n]*)', text):
    pipes.setdefault(m.group(1), m.group(2).strip())
print('%d pipelines described' % len(pipes))
for m in re.finditer(r'(?:^|\]: |\s)P ([bcder]) ?([^\r\n]*)', text):
    kind, rest = m.group(1), m.group(2)
    if kind == 'd':
        label = rest.split()[0]
        print('    d %s   [%s]' % (rest, pipes.get(label, 'no pl line')))
    elif kind == 'b':
        print('pass %s' % rest)
    else:
        print('  %s %s' % (kind, rest))
