#!/usr/bin/env python3
"""Documentation/boot-args.md, written from the kernel's sources: every boot argument the kernel reads, where it is
read, and what the comment beside it says.

  tools/bootargs.py              print the reference
  tools/bootargs.py --write      write Documentation/boot-args.md
  tools/bootargs.py --check      fail if Documentation/boot-args.md is not what the sources say (`make check`)

A boot argument is found by its read (PE_parse_boot_argn, PE_parse_boot_arg_str, TUNABLE) in the files
HakiOS adds to the kernel's tree (the places of kernel/lib.sh's OVERLAY_PLACES, and kernel/overlay) and in the
lines kernel/patches add; it is said under the file's path in the tree. Its description is the comment in the same file that
names it as `name=`: so an argument is documented by writing that comment, once, where the code is.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / 'kernel/overlay'
LIB = ROOT / 'kernel/lib.sh'
PATCHES = ROOT / 'kernel/patches'
OUT = ROOT / 'Documentation/boot-args.md'

READ = re.compile(r'(?:PE_parse_boot_argn|PE_parse_boot_arg_str|PE_parse_boot_arg)\(\s*"([A-Za-z0-9_]+)"|'
                  r'TUNABLE(?:_WRITEABLE|_DEV_WRITEABLE|_DT)?\([^;]*?"([A-Za-z0-9_]+)"')
COMMENT = re.compile(r'/\*.*?\*/', re.S)
SOURCES = ('.c', '.cpp', '.h', '.s', '.m')


def comment_lines(text):
    """The lines of every block comment, without the comment's own marks."""
    lines = []
    for comment in COMMENT.findall(text):
        for line in comment[2:-2].split('\n'):
            lines.append(re.sub(r'^\s*\*? ?', '', line.rstrip()) if line.strip() not in ('', '*') else '')
        lines.append('')
    return lines


def describe(name, lines):
    """What a comment says of `name=`: from the line that names it to the end of that entry -- the next
    blank line, the next line that starts another argument, or the end of the sentence."""
    named = re.compile(r'(?<![\w-])' + re.escape(name) + r'(?:=|\b(?= *(?:\[|:|and\b|,)))')
    best = ''
    for index, line in enumerate(lines):
        match = named.search(line)
        if match is None or '=' not in line[match.start():match.start() + len(name) + 1] and 'oot-arg' not in line:
            continue
        indent = len(line) - len(line.lstrip())
        text = line.strip()
        for more in lines[index + 1:index + 6]:
            if more.strip() == '' or re.match(r'\s*[A-Za-z0-9_]+=\S', more):
                break
            # a table of arguments: its continuation lines are indented further than the entry itself
            if re.match(r'^\s*' + re.escape(name) + r'=\S*\s{2,}', line) and len(more) - len(more.lstrip()) <= indent:
                break
            text += ' ' + more.strip()
        text = re.sub(r'\s+', ' ', text)
        # the sentence that names the argument, and no more
        start = text.find(name)
        before = max(text.rfind('. ', 0, start), text.rfind('): ', 0, start))
        text = text[before + 2 if before >= 0 else 0:].lstrip(': ')
        end = re.search(r'[.;](?= [A-Z(]|$)', text[text.find(name):])
        if end:
            text = text[:text.find(name) + end.end()]
        text = re.sub(r'^\(?[Bb]oot-args? ', '', text).strip()
        if len(text) > len(best) and len(text) > len(name) + 4:
            best = text
            if 'oot-arg' in line:
                break                   # (a comment written to say what the argument is wins)
    return best[:300].rstrip() + ('…' if len(best) > 300 else '')


def places():
    """kernel/lib.sh's OVERLAY_PLACES: [(directory of the repository, directory of the tree)]."""
    block = re.search(r'OVERLAY_PLACES=\((.*?)\n\)', LIB.read_text(), re.S).group(1)
    return [tuple(place.split(':', 1)) for place in re.findall(r'"([^"]+)"', block)]


def overlay_files():
    """[(file of the repository, its path in the tree)] of every file HakiOS adds, in the tree's order."""
    files = [(path, str(path.relative_to(OVERLAY))) for path in OVERLAY.rglob('*') if path.is_file()]
    for directory, prefix in places():
        base = ROOT / directory
        files += [(path, f'{prefix}/{path.relative_to(base)}') for path in base.rglob('*')
                  if path.is_file() and path.name != 'README.md']
    return sorted(files, key=lambda pair: Path(pair[1]).parts)


def collect():
    """{file of the tree: {name: description}} over the files of ours and the patches' added lines."""
    found = {}
    for path, tree in overlay_files():
        if path.suffix not in SOURCES:
            continue
        text = path.read_text(errors='replace')
        names = [a or b for a, b in READ.findall(text)]
        if names:
            lines = comment_lines(text)
            found[tree] = {name: describe(name, lines) for name in sorted(set(names))}
    for patch in sorted(PATCHES.glob('*.patch')):
        file, added = None, {}
        for line in patch.read_text(errors='replace').split('\n'):
            if line.startswith('+++ b/'):
                file = line[6:]
            elif line.startswith('+') and not line.startswith('+++') and file:
                added.setdefault(file, []).append(line[1:])
        for file, lines in added.items():
            text = '\n'.join(lines)
            names = [a or b for a, b in READ.findall(text)]
            if names:
                comments = comment_lines(text)
                found.setdefault(file, {}).update({name: describe(name, comments) for name in sorted(set(names))})
    return found


AREAS = [
    ('Display, graphics and input', ('iokit/Kernel/MiniIOMobileFramebuffer', 'iokit/Kernel/MiniIOSurface', 'iokit/Kernel/Hakios',
                                     'bsd/dev/mtk_mali', 'bsd/dev/mtk_tcm', 'bsd/dev/mtk_keys', 'bsd/dev/hakios_input',
                                     'bsd/dev/hakios_fbgrab', 'bsd/dev/hakios_virtio_input', 'osfmk/arm64/hakios_fb')),
    ('Power, CPU and battery', ('bsd/dev/mtk_battery', 'bsd/dev/hakios_battery', 'bsd/dev/mtk_scp',
                                'osfmk/arm64/psci_smp', 'osfmk/arm64/hakios_cpu', 'iokit/Kernel/GenericARMPE')),
    ('Sound and camera', ('bsd/dev/hakios_audio', 'bsd/dev/hakios_guard', 'bsd/dev/hakios_virtio_sound',
                          'bsd/dev/mtk_camera')),
    ('Wi-Fi, Bluetooth and network', ('bsd/dev/mtk_wifi', 'bsd/dev/mtk_connsys', 'bsd/dev/mtk_bt', 'bsd/dev/hakios_wifi',
                                      'bsd/dev/hakios_wpa', 'bsd/dev/hakios_sta', 'bsd/dev/hakios_net',
                                      'bsd/dev/hakios_virtio_net', 'bsd/dev/hakios_apple80211', 'bsd/net/')),
    ('Storage and file systems', ('bsd/dev/mtk_emmc', 'bsd/dev/hakios_virtio_blk', 'bsd/dev/hakios_sparse',
                                  'bsd/dev/hakios_image', 'bsd/dev/hakios_apfs_check', 'bsd/miscfs/',
                                  'bsd/dev/memdev', 'bsd/vfs/')),
]
REST = 'Kernel core, processes and diagnostics'


def area_of(file):
    for title, prefixes in AREAS:
        if any(file.startswith(prefix) for prefix in prefixes):
            return title
    return REST


def cell(text):
    return text.replace('|', '\\|') if text else '—'


def render(found):
    total = sum(len(names) for names in found.values())
    bare = sum(1 for names in found.values() for text in names.values() if not text)
    out = ['# Boot arguments',
           '',
           '<!-- GENERATED by tools/bootargs.py from the kernel files of ours and kernel/patches: do not edit. `make docs` rewrites it. -->',
           '',
           f'Every boot argument the kernel reads: {total} of them, by the file that reads them. The text is the comment',
           'beside the code, so the file named in each heading has the full explanation.',
           '',
           '- Pass them in `XNU_ARGS="..."` to `tools/run/xnu-cycle.sh`, or for good in the board file\'s `[boot] bootargs`.',
           '- An argument that is not given leaves its feature off unless the text says otherwise: drivers of a phone\'s',
           '  hardware do nothing until asked.',
           f'- {bare} arguments have no comment naming them yet and show "—". To document one, write a comment with',
           '  `name=` in it next to where it is read, then run `make docs`.',
           '- Apple\'s own boot arguments (`debug=`, `serial=`, `-v` ...) work as in any XNU and are not listed.',
           '']
    by_area = {}
    for file, names in found.items():
        by_area.setdefault(area_of(file), []).append((file, names))
    for title in [t for t, _ in AREAS] + [REST]:
        if title not in by_area:
            continue
        out += [f'## {title}', '']
        for file, names in sorted(by_area[title]):
            out += [f'### `{file}`', '', '| Argument | What the source says |', '| --- | --- |']
            out += [f'| `{name}` | {cell(text)} |' for name, text in sorted(names.items())]
            out.append('')
    return '\n'.join(out)


def main():
    text = render(collect())
    if '--check' in sys.argv:
        if not OUT.exists() or OUT.read_text() != text:
            print(f'{OUT.relative_to(ROOT)} is not what the sources say: run `make docs`', file=sys.stderr)
            return 1
        print(f'{OUT.relative_to(ROOT)}: up to date')
    elif '--write' in sys.argv:
        OUT.parent.mkdir(exist_ok=True)
        OUT.write_text(text)
        print(f'wrote {OUT.relative_to(ROOT)}')
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
