#!/usr/bin/env python3
"""The boards this project knows, as data: devices/NAME/board.toml and soc/SOC.toml.

  tools/boards.py list                 the boards, one a line
  tools/boards.py show BOARD           one board with its SoC's values filled in
  tools/boards.py status BOARD         what the board has on, what it could turn on, what the rest waits for
  tools/boards.py check                validate every file; non-zero exit and the reasons if one is wrong
  tools/boards.py gen [--out DIR]      write what the code is built with, for the boards that are there, into
                                       DIR (build/boards): the kernel's table (hakios_board_table.h) and, for
                                       each board with a [loader] section, the loader's header
                                       (NAME/hakios_platform.h). The builds run this themselves.
  tools/boards.py new NAME --soc SOC [--description TEXT] [--dtb FILE]
                                       start devices/NAME/ from the template; with --dtb, fill in what the phone's
                                       device tree says (tools/devicetree.py), and write soc/SOC.toml if it is new
  tools/boards.py which                the one phone that is here (what loader/build.sh builds for when not told)
  tools/boards.py get BOARD SECTION.KEY   one value, for the shell scripts (lists: space-separated)

A board file says what a device has; a SoC file says where the blocks of its chip are, once for every
board with that chip. The kernel is built with one table of the boards that are there and picks its row
at boot by /chosen/darwin-board. The loader is built for one board: it is linked at the address that
phone's boot loader starts a kernel at and travels in that phone's own boot image. docs/porting.md walks
through adding a device.

A board is a device package: a directory NAME with board.toml in it. Packages are looked for in
devices/ and in the directories of --devices DIR and of HAKIOS_DEVICES; SoC files in soc/ and --soc-dir
DIR. The repository itself has a template; phones are packages kept apart from it (tools/device.py
brings one in).
"""
import argparse
import os
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import minitoml  # noqa: E402  (TOML where Python has no reader of its own yet: tools/minitoml.py)
from devicetree import from_dtb  # noqa: E402  (what a phone's device tree says: tools/devicetree.py)

ROOT = Path(__file__).resolve().parents[1]
DEVICES = ROOT / 'devices'
SOCS = ROOT / 'soc'
BOARD_HEADER = ROOT / 'kernel/overlay/bsd/dev/hakios_board.h'
# What `gen` writes, into one directory (--out; build/boards unless told otherwise). None of it is kept
# in the repository: it is made from whichever device packages are there when something is built.
OUT = ROOT / 'build/boards'
KERNEL_TABLE = 'hakios_board_table.h'   # the kernel's table (bsd/dev/hakios_board.c includes it)
LOADER_HEADER = 'hakios_platform.h'     # the loader's, one a board: NAME/hakios_platform.h
MORE_DEVICES = []
MORE_SOCS = []
ALONE = []              # (--alone: only the directories of --devices and --soc-dir; the tests' stand-ins)

NAME = re.compile(r'^[a-z0-9][a-z0-9-]*$')

# The kinds of value. In C: A an address or a size of 64 bits (0x...ULL), X a number of 32 bits written
# in hexadecimal (0x...u), D one written in decimal (...u), S a string, B a flag, LIST a list of numbers
# or of strings, ALIST a list of addresses.
A, X, D, S, B = 'address', 'hex', 'number', 'string', 'flag'
LIST = ('list',)
ALIST = ('addresses',)

# key: (kind, required)
SOC_SCHEMA = {
    '': {'name': (S, True), 'description': (S, True)},
    'platform': {'io_base': (A, True), 'io_size': (A, True), 'gic_dist': (A, True), 'gic_redist': (A, True),
                 'uart0': (A, True), 'cores': (D, True), 'core_shift': (D, True), 'big_cores': (LIST, True),
                 'psci': (('smc', 'hvc'), True), 'debug_core0': (A, False)},
    'blocks': {key: (A, False) for key in (
        'topckgen', 'infracfg_ao', 'gpio', 'spm', 'toprgu', 'apmixed', 'pwrap', 'keypad', 'scp_sram', 'scp_cfg',
        'scp_clkctrl', 'ap_dma_btif_tx', 'i2c2', 'btif', 'i2c4', 'spi2', 'msdc0', 'msdc1', 'mipi_rx', 'mfgcfg',
        'mali', 'mmsys', 'disp_mutex0', 'smi_common', 'disp_ovl0', 'conn', 'conn_mcu', 'conn_hif_on',
        'conn_top_misc_off', 'conn_wbg', 'conn_misc_on', 'camsys', 'smi_larb3', 'seninf', 'camsv0', 'camsv1')},
    'cpufreq': {'armpll_big_con1': (X, True), 'big_mhz': (D, True)},
}
BOARD_SCHEMA = {
    '': {'name': (S, True), 'description': (S, True), 'soc': (S, True), 'features': (LIST, True)},
    'loader': {'kernel_load': (A, True), 'chain_stub': (A, True), 'xnu_base': (A, True), 'xnu_size': (A, True),
               'marker': (A, True), 'panic_log': (A, True), 'panic_log_size': (X, True), 'scratch': (A, True),
               'test_buffer': (A, True), 'core_order': (LIST, True), 'system_partition': (S, True),
               'big_cores_clock': (B, False)},
    'ramoops': {'console': (A, True), 'console_size': (X, True), 'pmsg': (A, True), 'pmsg_size': (X, True),
                'dump': (A, False), 'dump_size': (X, False)},
    'display': {'fb_base': (A, True), 'fb_size': (A, True), 'fb_pitch': (D, True), 'width': (D, True),
                'height': (D, True)},
    'fbdiag': {'marker': (A, True), 'probes': (ALIST, True), 'dump': (ALIST, False)},
    'battery': {'design_uah': (D, True)},
    'connsys': {'emi': (A, True), 'emi_size': (X, True), 'wifi_dma': (A, True), 'wifi_dma_size': (X, True)},
    'boot': {'android_device': (S, False), 'cpus': (D, False), 'bootargs': (LIST, False), 'seconds': (D, False)},
}

# What a feature needs before a board may list it: blocks of the SoC file, sections of the board file.
# (The names are those of hakios_board.h, which is where a new one is added.)
FEATURE_BLOCKS = {
    'emmc-msdc': ('msdc0',),
    'touch-td4160': ('spi2', 'gpio'),
    'keys-mt6358': ('keypad', 'pwrap', 'apmixed', 'topckgen'),
    'gpu-mali-g52': ('topckgen', 'infracfg_ao', 'spm', 'mfgcfg', 'mali'),
    'connsys-mt6631': ('spm', 'pwrap', 'gpio', 'toprgu', 'infracfg_ao', 'btif', 'ap_dma_btif_tx', 'conn', 'conn_mcu',
                       'conn_hif_on', 'conn_top_misc_off', 'conn_wbg', 'conn_misc_on'),
    'scp-sensors': ('scp_sram', 'scp_cfg', 'scp_clkctrl', 'topckgen', 'infracfg_ao'),
    'camera-s5kjn1': ('pwrap', 'gpio', 'topckgen', 'infracfg_ao', 'spm', 'smi_common', 'mmsys', 'camsys', 'apmixed',
                      'mipi_rx', 'seninf', 'camsv1', 'smi_larb3', 'i2c4'),
    'battery-mt6358': ('pwrap',),
    'display-ovl-flip': ('disp_ovl0',),
}
FEATURE_SECTIONS = {'battery-mt6358': ('battery',), 'connsys-mt6631': ('connsys',), 'display-ovl-flip': ('display',),
                    'touch-td4160': ('display',), 'emmc-msdc': ('loader',)}
FEATURE_BOOTARGS = {
    'emmc-msdc': 'emmc_root=1: the root on the flash, read by the kernel',
    'touch-td4160': 'touchscreen=2',
    'keys-mt6358': 'buttons=2; mtk_cpufreq=1 looks at the clocks, =2 raises them; mali_mhz=N',
    'gpu-mali-g52': 'mali=1',
    'connsys-mt6631': 'connsys_step=N, steps 1 and 2 only read; connsys_bt=1',
    'scp-sensors': 'scp_step=N, step 1 only reads',
    'camera-s5kjn1': 'mtkcam=1',
    'battery-mt6358': 'mtkbatt=1',
    'display-ovl-flip': 'fb_flip=1',
}
# drivers of one part, wired the way one phone has it (docs/drivers.md)
PART_DRIVERS = ('touch', 'camera', 'connsys')
# the loader's own parts of a feature, compiled in only for a board that lists it
LOADER_FEATURES = ('touch-td4160', 'connsys-mt6631', 'gpu-mali-g52', 'camera-s5kjn1')
# the board's sections the kernel's table carries, and the loader's header with the prefix of each
TABLE_SECTIONS = ('display', 'battery', 'connsys')
HEADER_SECTIONS = (('platform', 'HAKIOS_SOC_'), ('blocks', 'HAKIOS_SOC_'), ('cpufreq', 'HAKIOS_SOC_'),
                   ('loader', 'HAKIOS_LOADER_'), ('ramoops', 'HAKIOS_RAMOOPS_'), ('display', 'HAKIOS_DISPLAY_'),
                   ('fbdiag', 'HAKIOS_FBDIAG_'))


def shown(path):
    """A path as it is said to people: from the repository's top where it is inside it."""
    try:
        return Path(path).resolve().relative_to(ROOT.resolve())
    except ValueError:
        return Path(path)


def soc_files():
    """Every SoC file there is, soc/'s first: a name that is there twice is the first one's."""
    seen = {}
    for directory in ([] if ALONE else [SOCS]) + MORE_SOCS:
        for path in sorted(Path(directory).glob('*.toml')):
            seen.setdefault(path.stem, path)
    return [seen[name] for name in sorted(seen)]


def board_files():
    """Every device package's board file, devices/'s first (the template is not a board)."""
    seen = {}
    for directory in ([] if ALONE else [DEVICES]) + MORE_DEVICES:
        for path in sorted(Path(directory).glob('*/board.toml')):
            if not path.parent.name.startswith('_'):
                seen.setdefault(path.parent.name, path)
    return [seen[name] for name in sorted(seen)]


class Problems(list):
    def add(self, where, what):
        self.append(f'{where}: {what}')


def feature_bits():
    """The features the kernel knows, HAKIOS_HAS_* of hakios_board.h: [(bit, name as board.toml spells it)]."""
    found = re.findall(r'^\s*HAKIOS_HAS_([A-Z0-9_]+)\s*=\s*1u\s*<<\s*(\d+)', BOARD_HEADER.read_text(), re.M)
    return sorted((int(bit), name.lower().replace('_', '-')) for name, bit in found)


def c_name(feature):
    return 'HAKIOS_HAS_' + feature.upper().replace('-', '_')


def feature_needs(feature, board, soc):
    """What a board that lists the feature must have and has not, in words; [] when it has everything."""
    soc_file = f'soc/{board.get("soc")}.toml'
    blocks = soc.get('blocks', {})
    needs = [f'{block} in [blocks] of {soc_file}' for block in FEATURE_BLOCKS.get(feature, ()) if block not in blocks]
    needs += [f'[{section}] in the board file' for section in FEATURE_SECTIONS.get(feature, ()) if section not in board]
    return needs


def status_text(name, board, soc):
    """One board as a porter wants it: what is on, what could be, what each of the rest waits for."""
    listed = board.get('features', [])
    out = [f'{name}: {board["description"]} ({board["soc"]})', '']

    def line(what, state):
        out.append(f'  {what:18} {state}')

    line('loader', 'built for this board (loader/build.sh)' if all(s in board for s in ('loader', 'ramoops', 'display'))
         else 'needs [loader], [ramoops] and [display] in the board file (docs/porting.md)')
    out.append('')
    for _, feature in feature_bits():
        needs = feature_needs(feature, board, soc)
        part = feature.split('-')[0] in PART_DRIVERS
        how = FEATURE_BOOTARGS.get(feature)
        if feature in listed:
            line(feature, 'on' + (': STILL NEEDS ' + ', '.join(needs) if needs else f' ({how})' if how else ''))
        elif needs:
            line(feature, 'needs ' + ', '.join(needs))
        elif part:
            line(feature, "one part's driver: only if this phone has that part wired the same way (docs/drivers.md)")
        else:
            line(feature, 'can be listed: what it needs is there' + (f' (then {how})' if how else ''))
    out += ['', 'A feature that is on is started only with its boot argument, in brackets; docs/porting.md has',
            'the order to try them in and the log line that says each one worked.', '']
    return '\n'.join(out)


def check_table(where, table, schema, problems):
    for key, value in table.items():
        if isinstance(value, dict):
            continue
        if key not in schema:
            problems.add(where, f'unknown key "{key}" (known: {", ".join(sorted(schema))})')
            continue
        kind = schema[key][0]
        if kind is LIST:
            if not isinstance(value, list) or len({type(v) for v in value}) > 1 or \
                    any(type(v) not in (int, str) for v in value):
                problems.add(where, f'{key}: a list of numbers or of strings')
            elif any(type(v) is int and not 0 <= v <= 0xffffffff for v in value):
                problems.add(where, f'{key}: values of 32 bits')
        elif kind is ALIST:
            if not isinstance(value, list) or any(type(v) is not int or not 0 <= v <= 0xffffffffffffffff for v in value):
                problems.add(where, f'{key}: a list of addresses')
        elif isinstance(kind, tuple):
            if value not in kind:
                problems.add(where, f'{key} = {value!r}: one of {", ".join(kind)}')
        elif kind is A:
            if type(value) is not int or not 0 <= value <= 0xffffffffffffffff:
                problems.add(where, f'{key} = {value!r}: an address expected')
        elif kind in (X, D):
            if type(value) is not int or not 0 <= value <= 0xffffffff:
                problems.add(where, f'{key} = {value!r}: a number of 32 bits expected')
        elif kind is S:
            if type(value) is not str:
                problems.add(where, f'{key} = {value!r}: a string expected')
        elif kind is B:
            if type(value) is not bool:
                problems.add(where, f'{key} = {value!r}: true or false expected')
    for key, (_, required) in schema.items():
        if required and key not in table:
            problems.add(where, f'"{key}" is missing')


def check_file(path, data, schema, problems):
    rel = shown(path)
    check_table(rel, data, schema[''], problems)
    for section, value in data.items():
        if not isinstance(value, dict):
            continue
        if section not in schema:
            problems.add(rel, f'unknown section [{section}] (known: {", ".join(s for s in schema if s)})')
        else:
            check_table(f'{rel} [{section}]', value, schema[section], problems)


def overlap(a, a_size, b, b_size):
    return a < b + b_size and b < a + a_size


def load(problems=None):
    """Every SoC and board, checked. Returns (socs, boards) by name; problems are collected or raised."""
    own = problems is None
    problems = Problems() if own else problems
    socs, boards = {}, {}
    for path in soc_files():
        try:
            data = minitoml.loads(path.read_text())
        except ValueError as error:
            problems.add(shown(path), f'not TOML: {error}')
            continue
        check_file(path, data, SOC_SCHEMA, problems)
        if data.get('name') != path.stem:
            problems.add(shown(path), f'name = {data.get("name")!r}, but the file is {path.name}')
        platform = data.get('platform')
        if isinstance(platform, dict) and all(type(platform.get(k)) is int for k in ('cores', 'io_base', 'io_size')) and \
                isinstance(platform.get('big_cores'), list):
            if any(type(c) is not int or c >= platform['cores'] for c in platform['big_cores']):
                problems.add(f'{shown(path)} [platform]', f'big_cores names a core the chip has not ({platform["cores"]} cores)')
            blocks = data.get('blocks', {})
            for block, address in (blocks.items() if isinstance(blocks, dict) else ()):
                if type(address) is int and not platform['io_base'] <= address < platform['io_base'] + platform['io_size']:
                    problems.add(f'{shown(path)} [blocks]', f'{block} = {address:#x} is outside the window of [platform] '
                                 f'({platform["io_base"]:#x} + {platform["io_size"]:#x}): the kernel could not map it')
        socs[path.stem] = data
    known = dict((name, bit) for bit, name in feature_bits())
    for path in board_files():
        name = path.parent.name
        where = shown(path)
        try:
            data = minitoml.loads(path.read_text())
        except ValueError as error:
            problems.add(where, f'not TOML: {error}')
            continue
        check_file(path, data, BOARD_SCHEMA, problems)
        if not NAME.match(name):
            problems.add(where, f'the directory {name!r}: lower-case letters, digits and dashes')
        if data.get('name') != name:
            problems.add(where, f'name = {data.get("name")!r}, but the directory is {name}')
        soc = socs.get(data.get('soc'))
        if soc is None:
            problems.add(where, f'soc = {data.get("soc")!r}: there is no soc/{data.get("soc")}.toml')
            soc = {}
        features = data.get('features', [])
        if isinstance(features, list):
            for feature in features:
                if feature not in known:
                    problems.add(where, f'features: the kernel knows no "{feature}" (it knows: {", ".join(sorted(known))})')
                    continue
                for need in feature_needs(feature, data, soc):
                    problems.add(where, f'features: "{feature}" needs {need}')
            if len(set(features)) != len(features):
                problems.add(where, 'features: one is listed twice')
        loader, platform = data.get('loader'), soc.get('platform', {})
        if isinstance(loader, dict):
            for section in ('ramoops', 'display'):
                if section not in data:
                    problems.add(where, f'[loader] needs [{section}]: the loader has no other way to say anything')
            order = loader.get('core_order')
            cores = platform.get('cores')
            if isinstance(order, list) and type(cores) is int:
                if sorted(order) != list(range(cores)):
                    problems.add(f'{where} [loader]', f'core_order: every core of the chip once (0 to {cores - 1})')
                elif order[0] != 0:
                    problems.add(f'{where} [loader]', 'core_order: begins with the core the boot loader leaves the '
                                 'loader on, which is core 0')
            if loader.get('big_cores_clock') and 'cpufreq' not in soc:
                problems.add(f'{where} [loader]', f'big_cores_clock needs [cpufreq] in soc/{data.get("soc")}.toml')
            partition = loader.get('system_partition')
            if type(partition) is str and not 1 <= len(partition.strip()) <= 35:
                problems.add(f'{where} [loader]', 'system_partition: a GPT partition name, 1 to 35 characters')
            # the pieces of memory the loader is told of must not lie on each other
            ramoops, display = data.get('ramoops', {}), data.get('display', {})
            try:
                areas = [('xnu_base + xnu_size', loader['xnu_base'], loader['xnu_size']),
                         ('marker', loader['marker'], 0x1000),
                         ('panic_log', loader['panic_log'], loader['panic_log_size']),
                         ('scratch', loader['scratch'], 0x200000),
                         ('test_buffer', loader['test_buffer'], 0x400000),
                         ('chain_stub', loader['chain_stub'], 0x1000),
                         ('the ramoops console', ramoops['console'], ramoops['console_size']),
                         ('the ramoops pmsg zone', ramoops['pmsg'], ramoops['pmsg_size']),
                         ('the framebuffer', display['fb_base'], display['fb_size'])]
            except (KeyError, TypeError):
                areas = []
            if any(type(v) is not int for area in areas for v in area[1:]):
                areas = []          # (a value of the wrong kind: said above)
            for i, (first, a, a_size) in enumerate(areas):
                for second, b, b_size in areas[i + 1:]:
                    if overlap(a, a_size, b, b_size):
                        problems.add(where, f'{first} ({a:#x} + {a_size:#x}) and {second} ({b:#x} + {b_size:#x}) overlap')
        display = data.get('display')
        if isinstance(display, dict) and all(type(display.get(k)) is int for k in ('fb_pitch', 'width', 'height', 'fb_size')):
            if display['fb_pitch'] < display['width'] * 4:
                problems.add(f'{where} [display]', 'fb_pitch is in bytes: four a pixel at least')
            elif display['fb_pitch'] * display['height'] > display['fb_size']:
                problems.add(f'{where} [display]', 'one picture (fb_pitch x height) does not fit in fb_size')
        boot = data.get('boot', {})
        if isinstance(boot, dict):
            for argument in boot.get('bootargs', []) if isinstance(boot.get('bootargs'), list) else []:
                if type(argument) is not str or not re.match(r'^-?[A-Za-z_][A-Za-z0-9_.-]*(=[^\s"]+)?$', argument):
                    problems.add(f'{where} [boot]', f'bootargs: {argument!r} is not NAME or NAME=VALUE')
            if type(boot.get('cpus')) is int and type(platform.get('cores')) is int and \
                    not 1 <= boot['cpus'] < platform['cores']:
                problems.add(f'{where} [boot]', f'cpus: 1 to {platform["cores"] - 1} (one core is the loader\'s watcher)')
        boards[name] = data
    if own and problems:
        raise SystemExit('\n'.join(f'boards.py: {p}' for p in problems))
    return socs, boards


def c_string(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def c_value(kind, value):
    if kind is A:
        return f'0x{value:08x}ULL'
    if kind is X:
        return f'0x{value:x}u'
    if kind is D:
        return f'{value}u'
    if kind is S:
        return c_string(value)
    if kind is B:
        return '1' if value else '0'
    raise ValueError(kind)


GENERATED = '/* Made by tools/boards.py gen from the device packages and soc/ that were there: not kept, not edited. */'


def table_text(socs, boards):
    """The kernel's table: one row a board (bsd/dev/hakios_board.h says what the fields are)."""
    bits = feature_bits()
    names = [''] * (max(bit for bit, _ in bits) + 1 if bits else 1)
    for bit, name in bits:
        names[bit] = name
    out = [GENERATED, '', f'static const char *const hakios_feature_names[{len(names)}] = {{']
    out += [f'\t{c_string(name)},' for name in names]
    out += ['};', f'static const unsigned hakios_feature_count = {len(names)};', '',
            'static const struct hakios_board hakios_boards[] = {']
    for name in sorted(boards):
        board, soc = boards[name], socs[boards[name]['soc']]
        features = ' | '.join(c_name(f) for f in board['features']) or '0'
        platform = soc['platform']
        out += ['\t{', f'\t\t.name = {c_string(name)},', f'\t\t.description = {c_string(board["description"])},',
                f'\t\t.soc = {c_string(board["soc"])},', f'\t\t.features = {features},']
        if 'system_partition' in board.get('loader', {}):
            out.append(f'\t\t.system_partition = {c_string(board["loader"]["system_partition"])},')
        out += ['\t\t.platform = {',
                f'\t\t\t.gic_dist = {c_value(A, platform["gic_dist"])}, .gic_redist = {c_value(A, platform["gic_redist"])},',
                f'\t\t\t.uart0 = {c_value(A, platform["uart0"])},', '\t\t},', '\t\t.blocks = {']
        for key in SOC_SCHEMA['blocks']:
            if key in soc.get('blocks', {}):
                out.append(f'\t\t\t.{key} = {c_value(A, soc["blocks"][key])},')
        out.append('\t\t},')
        for section in TABLE_SECTIONS:
            if section in board:
                out.append(f'\t\t.{section} = {{')
                for key, (kind, _) in BOARD_SCHEMA[section].items():
                    if key in board[section]:
                        out.append(f'\t\t\t.{key} = {c_value(kind, board[section][key])},')
                out.append('\t\t},')
        out.append('\t},')
    if not boards:
        out.append('\t{ .name = NULL },      /* (no device package was there when this kernel was built) */')
    out += ['};', f'static const unsigned hakios_board_count = {len(boards)};', '']
    return '\n'.join(out)


def platform_text(name, board, soc):
    """The loader's header for one board: every value it is built with, as a constant (loader/mtkloader.c)."""
    out = [GENERATED, f'/* board {name}: {board["description"]}, {board["soc"]} */',
           '#ifndef HAKIOS_PLATFORM_H', '#define HAKIOS_PLATFORM_H', '',
           f'#define HAKIOS_BOARD_NAME               {c_string(name)}',
           f'#define HAKIOS_BOARD_DESCRIPTION        {c_string(board["description"])}',
           f'#define HAKIOS_SOC_NAME                 {c_string(board["soc"])}', '']

    def define(macro, value):
        out.append(f'#define {macro:35} {value}')

    for _, feature in feature_bits():
        if feature in LOADER_FEATURES:
            define(c_name(feature), '1' if feature in board['features'] else '0')
    out.append('')
    for section, prefix in HEADER_SECTIONS:
        schema, data = (SOC_SCHEMA, soc) if section in SOC_SCHEMA else (BOARD_SCHEMA, board)
        values = data.get(section, {})
        if not values:
            continue
        out.append(f'/* [{section}] of {"soc/" + board["soc"] + ".toml" if schema is SOC_SCHEMA else "the board file"} */')
        for key, (kind, _) in schema[section].items():
            if key not in values:
                continue
            macro = prefix + key.upper()
            if kind is LIST:
                define(macro, '{ ' + ', '.join(str(v) for v in values[key]) + ' }')
                define(macro + '_COUNT', str(len(values[key])))
            elif kind is ALIST:
                define(macro, '{ ' + ', '.join(c_value(A, v) for v in values[key]) + ' }')
            elif isinstance(kind, tuple):
                define(macro, c_string(values[key]))
            else:
                define(macro, c_value(kind, values[key]))
        out.append('')
    # the cores above the small ones begin here (the loader marks every core below as a small one)
    big = sorted(soc['platform']['big_cores'])
    define('HAKIOS_SOC_FIRST_BIG_CORE', str(big[0] if big else soc['platform']['cores']))
    out += ['', '#endif', '']
    return '\n'.join(out)


def generated(socs, boards):
    files = {KERNEL_TABLE: table_text(socs, boards)}
    for name, board in boards.items():
        if 'loader' in board:
            files[f'{name}/{LOADER_HEADER}'] = platform_text(name, board, socs[board['soc']])
    return files


def write_generated(out, socs, boards):
    """Write the files that differ; say which. A file whose contents are the same is not touched (make looks at times)."""
    written = []
    for name, text in generated(socs, boards).items():
        path = Path(out) / name
        if not path.is_file() or path.read_text() != text:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            written.append(str(shown(path)))
    return written


def phones(boards):
    """The boards with a loader: the phones."""
    return sorted(name for name, board in boards.items() if 'loader' in board)


def toml_value(kind, value):
    if isinstance(value, list):
        return '[' + ', '.join(str(v) for v in value) + ']'
    if isinstance(value, str):
        return c_string(value)
    return f'0x{value:08x}' if kind in (A, X) else str(value)


def soc_text(soc_name, found):
    """A new SoC file from what the tree said: every key with the node it came from."""
    out = [f'# {soc_name}: what every board with this chip shares, read by tools/boards.py new --dtb from one',
           "# phone's Android device tree. Check each value; fill in what the end of the file lists.",
           f'name = "{soc_name}"', 'description = "MediaTek ... (' + soc_name.upper() + ')"', '']
    for section in ('platform', 'blocks'):
        out.append(f'[{section}]')
        for key, (kind, _) in SOC_SCHEMA[section].items():
            if key in found['soc'][section]:
                line = f'{key} = {toml_value(kind, found["soc"][section][key])}'
                out.append(f'{line:36}# {found["sources"].get(f"{section}.{key}", "")}')
        out.append('')
    out += ['# Not found in the tree:'] + [f'#   {m}' for m in found['missing'] if m.startswith(('[blocks]', 'the', 'a UART', '/psci'))]
    out += ['# [cpufreq] (the big cluster\'s PLL) is never in a tree: the vendor\'s cpufreq table for the chip.', '']
    return '\n'.join(out)


def soc_differences(soc, found):
    """What an existing SoC file and the phone's tree disagree on, in words."""
    out = []
    for section in ('platform', 'blocks'):
        for key, value in found['soc'][section].items():
            have = soc.get(section, {}).get(key)
            if have is None:
                out.append(f'[{section}] {key}: the file has none, the tree says {value!r}')
            elif have != value and key not in ('io_base', 'io_size'):
                out.append(f'[{section}] {key}: the file says {have!r}, the tree {value!r} ({found["sources"].get(section + "." + key, "")})')
    return out


def new_board(name, soc_name, description=None, dtb=None):
    if not NAME.match(name):
        raise SystemExit(f'boards.py: {name!r}: lower-case letters, digits and dashes')
    target = DEVICES / name
    if target.exists():
        raise SystemExit(f'boards.py: {shown(target)} is there already')
    found = None
    if dtb:
        try:
            found = from_dtb(Path(dtb).read_bytes())
        except (OSError, ValueError) as error:
            raise SystemExit(f'boards.py: {dtb}: {error}')
    if not (SOCS / f'{soc_name}.toml').is_file():
        if not NAME.match(soc_name):
            raise SystemExit(f'boards.py: {soc_name!r}: lower-case letters, digits and dashes')
        if found:
            (SOCS / f'{soc_name}.toml').write_text(soc_text(soc_name, found))
            print(f'boards.py: soc/{soc_name}.toml written from the tree: check it, then fill in what its end lists')
        else:
            text = (DEVICES / '_template/soc.toml').read_text()
            (SOCS / f'{soc_name}.toml').write_text(text.replace('name = "mt0000"', f'name = "{soc_name}"'))
            print(f'boards.py: soc/{soc_name}.toml is new: every address in it is to be read from your phone')
    elif found:
        differences = soc_differences(minitoml.loads((SOCS / f'{soc_name}.toml').read_text()), found)
        print(f'boards.py: soc/{soc_name}.toml and the tree: ' + ('they agree' if not differences else
              'they DISAGREE -- another chip, or another revision of it:\n  ' + '\n  '.join(differences)))
    target.mkdir(parents=True)
    text = (DEVICES / '_template/board.toml').read_text()
    text = text.replace('name = "NAME"', f'name = "{name}"').replace('soc = "mt0000"', f'soc = "{soc_name}"')
    if description:
        text = text.replace('description = "Maker Model"', f'description = {c_string(description)}')
    if found:
        board = found['board']
        lines = text.split('\n')
        for i, line in enumerate(lines):
            if line.startswith('# core_order = ') and 'core_order' in board:
                lines[i] = f'# core_order = {toml_value(LIST, board["core_order"])}     # from the tree: {found["sources"]["loader.core_order"]}'
            elif line.startswith('# fb_base = ') and 'fb_base' in board:
                lines[i] = f'# fb_base = 0x{board["fb_base"]:08x}              # from the tree: {found["sources"]["display.fb_base"]}'
            elif line.startswith('# fb_size = ') and 'fb_size' in board:
                lines[i] = f'# fb_size = 0x{board["fb_size"]:x}'
        if 'ramoops_region' in board:
            address, size = board['ramoops_region']
            lines.insert(lines.index('# [ramoops]') + 1, f'#                                   (the tree: the region is 0x{address:08x} + 0x{size:x})')
        text = '\n'.join(lines)
    (target / 'board.toml').write_text(text)
    shutil.copy(DEVICES / '_template/README.md', target / 'README.md')
    if found and found['missing']:
        print('boards.py: not in the tree, to be read another way:\n  ' + '\n  '.join(found['missing']))
    print(f'boards.py: {shown(target)}/board.toml: fill in what is marked, in the order of docs/porting.md; '
          f'then tools/boards.py check')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--devices', action='append', default=[], metavar='DIR', help='look for device packages here as well')
    parser.add_argument('--soc-dir', action='append', default=[], metavar='DIR', help='look for SoC files here as well')
    parser.add_argument('--alone', action='store_true', help='look nowhere else: not in devices/ and soc/, not in HAKIOS_DEVICES')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    sub.add_parser('check')
    sub.add_parser('which')
    for command in ('show', 'status'):
        sub.add_parser(command).add_argument('board')
    get = sub.add_parser('get')
    get.add_argument('board')
    get.add_argument('key')
    gen = sub.add_parser('gen')
    gen.add_argument('--out', default=str(OUT))
    new = sub.add_parser('new')
    new.add_argument('name')
    new.add_argument('--soc', required=True)
    new.add_argument('--description')
    new.add_argument('--dtb', help="the phone's Android device tree (as root: cat /sys/firmware/fdt)")
    args = parser.parse_args()
    MORE_DEVICES.extend(Path(d) for d in args.devices)
    MORE_SOCS.extend(Path(d) for d in args.soc_dir)
    if args.alone:
        ALONE.append(True)
    else:
        MORE_DEVICES.extend(Path(d) for d in os.environ.get('HAKIOS_DEVICES', '').split(os.pathsep) if d)

    if args.command == 'new':
        new_board(args.name, args.soc, args.description, args.dtb)
        return 0
    if args.command == 'check':
        problems = Problems()
        socs, boards = load(problems)
        for problem in problems:
            print(f'boards.py: {problem}', file=sys.stderr)
        if not problems:
            print(f'boards: {len(boards)} board(s) and {len(socs)} SoC file(s), all consistent')
        return 1 if problems else 0
    socs, boards = load()
    if args.command == 'list':
        for name in sorted(boards):
            print(f'{name:14} {boards[name]["soc"]:10} {boards[name]["description"]}')
        if not boards:
            print('no device package is here: tools/device.py available lists the ones there are', file=sys.stderr)
    elif args.command == 'which':
        there = phones(boards)
        if len(there) != 1:
            raise SystemExit('boards.py: ' + ("no phone's package is here (tools/device.py add NAME brings one in)" if not there
                             else f'{len(there)} phones are here ({", ".join(there)}): say which with HAKIOS_BOARD=NAME'))
        print(there[0])
    elif args.command == 'gen':
        written = write_generated(args.out, socs, boards)
        print(f'boards: {len(boards)} board(s); ' + (f'wrote {", ".join(written)}' if written else f'nothing new in {shown(args.out)}'))
    else:
        if args.board not in boards:
            raise SystemExit(f'boards.py: no board "{args.board}" here (tools/boards.py list)')
        board = boards[args.board]
        soc = socs[board['soc']]
        if args.command == 'status':
            print(status_text(args.board, board, soc))
        elif args.command == 'get':
            section, _, key = args.key.rpartition('.')
            table = board.get(section, {}) if section else board
            if key not in table:
                return 1
            value = table[key]
            kind = BOARD_SCHEMA.get(section, {}).get(key, (D,))[0]
            print(' '.join(str(v) for v in value) if isinstance(value, list) else
                  ('true' if value else 'false') if isinstance(value, bool) else
                  hex(value) if kind in (A, X) and type(value) is int else value)
        else:
            for title, data in ((f'devices/{args.board}/board.toml', board), (f'soc/{board["soc"]}.toml', soc)):
                print(f'# {title}')
                for key, value in data.items():
                    if isinstance(value, dict):
                        print(f'[{key}]')
                        for k, v in value.items():
                            print(f'  {k} = {hex(v) if type(v) is int and v > 4096 else v!r}')
                    else:
                        print(f'{key} = {value!r}')
                print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
