#!/usr/bin/env python3
"""ca27_module.py LIBRARY NAME SIZE FNV OUT: one function's module out of a .metallib, the very bytes the phone handed
the library for it -- the "cap functions" line of boot-arg rwm_capture= gives the name, size and FNV-1a of each, and
userland/tools/air/ca27_replay.c draws with what this writes.

Both forms are tried, the module unwrapped from LLVM's bitcode wrapper and the blob as it lies in the library, and the
one whose size and hash are the captured ones is written. None of them: it says so and writes nothing, since a replay
of another module answers nothing about this one.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import metallib  # noqa: E402


def fnv(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xffffffffffffffff
    return h


def main():
    path, name, size, want, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4], 16), sys.argv[5]
    b, header, funcs = metallib.parse(path)
    found = [f for f in funcs if f.get('name') == name]
    if not found:
        sys.exit('ca27_module.py: %s has no function %s' % (path, name))
    for f in found:
        start = header['bitcode'][0] + f['offt'][2]
        blob = b[start:start + f['mdsz']]
        bare, _ = metallib.bitcode_of(b, header, f)
        for form, bytes_ in (('unwrapped', bare), ('as stored', blob)):
            if len(bytes_) == size and fnv(bytes_) == want:
                with open(out, 'wb') as fp:
                    fp.write(bytes_)
                print('%s: %d bytes %016x, %s' % (name, size, want, form))
                return
        print('%s: unwrapped %d bytes %016x, as stored %d bytes %016x; captured %d bytes %016x' %
              (name, len(bare), fnv(bare), len(blob), fnv(blob), size, want), file=sys.stderr)
    sys.exit('ca27_module.py: not the captured module')


if __name__ == '__main__':
    main()
