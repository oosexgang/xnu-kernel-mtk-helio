#!/usr/bin/env python3
"""Host tests of the tools: the TOML reader, the board files' checks, what `gen` writes.

They run against the stand-in phone of workbench/devices and the made-up chip of workbench/soc, whose addresses
are no real phone's, so a test cannot pass by a value being hard-wired somewhere.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import minitoml  # noqa: E402

BOARDS = [sys.executable, str(ROOT / 'tools/boards.py'), '--alone', '--devices', str(ROOT / 'workbench/devices'),
          '--soc-dir', str(ROOT / 'workbench/soc')]


def boards(*arguments, devices=None):
    command = list(BOARDS)
    if devices is not None:
        command[4] = str(devices)
    return subprocess.run(command + list(arguments), capture_output=True, text=True)


class TOML(unittest.TestCase):
    def test_every_file_of_the_repository(self):
        files = [p for p in ROOT.rglob('*.toml') if 'build' not in p.parts and '.git' not in p.parts]
        self.assertGreater(len(files), 3)
        for path in files:
            own = minitoml.loads_own(path.read_text())
            self.assertIsInstance(own, dict, path)
            try:
                import tomllib
            except ImportError:
                continue
            self.assertEqual(own, tomllib.loads(path.read_text()), path)

    def test_values(self):
        data = minitoml.loads_own('a = 0x10 # c\nb = "x\\ty"\nc = [1, 2,\n 3]\n[t]\nd = true\n[[r]]\ne = \'q\'\n[[r]]\ne = "w"\n')
        self.assertEqual(data, {'a': 16, 'b': 'x\ty', 'c': [1, 2, 3], 't': {'d': True}, 'r': [{'e': 'q'}, {'e': 'w'}]})

    def test_refuses_what_it_does_not_read(self):
        for text in ('a = 1.5\n', 'a = {b = 1}\n', 'a.b = 1\n', 'a = """x"""\n', 'a = 1\na = 2\n', 'a = [1\n'):
            with self.assertRaises(ValueError, msg=text):
                minitoml.loads_own(text)


class Boards(unittest.TestCase):
    def test_the_stand_in_is_consistent(self):
        done = boards('check')
        self.assertEqual(done.returncode, 0, done.stderr)

    def changed(self, old, new):
        """A copy of the stand-in's package with one piece of text replaced; the check's answer."""
        with tempfile.TemporaryDirectory() as directory:
            shutil.copytree(ROOT / 'workbench/devices/phone-a', Path(directory) / 'phone-a')
            path = Path(directory) / 'phone-a/board.toml'
            text = path.read_text()
            self.assertIn(old, text)
            path.write_text(text.replace(old, new))
            return boards('check', devices=directory)

    def test_refusals(self):
        for old, new, why in (
                ('"touch-td4160"]', '"touch-td4160", "no-such-part"]', 'knows no "no-such-part"'),
                ('[battery]\ndesign_uah = 4000000\n', '', 'needs [battery]'),
                ('core_order = [0, 6, 7, 1, 2, 3, 4, 5]', 'core_order = [0, 6, 7, 1, 2, 3, 4]', 'every core of the chip once'),
                ('core_order = [0, 6, 7, 1, 2, 3, 4, 5]', 'core_order = [6, 0, 7, 1, 2, 3, 4, 5]', 'begins with the core'),
                ('marker = 0x81000000', 'marker = 0xd0000000', 'overlap'),
                ('fb_pitch = 4352', 'fb_pitch = 1080', 'four a pixel'),
                ('soc = "mt0001"', 'soc = "mt9999"', 'there is no soc/mt9999.toml'),
                ('cpus = 4', 'cpus = 8', 'cpus: 1 to 7'),
                ('bootargs = ["touchscreen=2"]', 'bootargs = ["touch screen"]', 'not NAME or NAME=VALUE'),
                ('pmsg_size = 0x10000', 'pmsg_size = "big"', 'a number of 32 bits'),
                ('width = 1080', 'width = 1080\nwidht = 1', 'unknown key "widht"')):
            done = self.changed(old, new)
            self.assertNotEqual(done.returncode, 0, new)
            self.assertIn(why, done.stderr, new)

    def test_generated(self):
        with tempfile.TemporaryDirectory() as out:
            done = boards('gen', '--out', out)
            self.assertEqual(done.returncode, 0, done.stderr)
            table = (Path(out) / 'hakios_board_table.h').read_text()
            header = (Path(out) / 'phone-a/hakios_platform.h').read_text()
            again = boards('gen', '--out', out)
            self.assertIn('nothing new', again.stdout)
        self.assertIn('.name = "phone-a"', table)
        self.assertIn('.features = HAKIOS_HAS_EMMC_MSDC | HAKIOS_HAS_BATTERY_MT6358 | HAKIOS_HAS_KEYS_MT6358 | HAKIOS_HAS_TOUCH_TD4160,', table)
        self.assertIn('.system_partition = "sysa",', table)
        self.assertIn('.msdc0 = 0x11330000ULL,', table)            # (the made-up chip's, not the MT6768's 0x11230000)
        self.assertIn('.design_uah = 4000000u,', table)
        self.assertNotIn('.connsys', table)
        self.assertNotIn('x6826b', table)
        for line in ('#define HAKIOS_BOARD_NAME               "phone-a"', 'HAKIOS_LOADER_KERNEL_LOAD           0x41080000ULL',
                     'HAKIOS_LOADER_CORE_ORDER            { 0, 6, 7, 1, 2, 3, 4, 5 }', 'HAKIOS_HAS_TOUCH_TD4160             1',
                     'HAKIOS_HAS_CONNSYS_MT6631           0', 'HAKIOS_RAMOOPS_PMSG_SIZE            0x10000u',
                     'HAKIOS_DISPLAY_FB_PITCH             4352u', 'HAKIOS_SOC_FIRST_BIG_CORE           6'):
            self.assertIn(line, header)
        self.assertNotIn('HAKIOS_LOADER_BIG_CORES_CLOCK', header)

    def test_status_and_get(self):
        status = boards('status', 'phone-a').stdout
        self.assertIn('emmc-msdc          on', status)
        self.assertIn('connsys-mt6631     needs [connsys] in the board file', status)
        self.assertEqual(boards('get', 'phone-a', 'boot.android_device').stdout.strip(), 'stand-in-a')
        self.assertEqual(boards('get', 'phone-a', 'loader.kernel_load').stdout.strip(), '0x41080000')
        self.assertEqual(boards('which').stdout.strip(), 'phone-a')

    def test_the_template_explains_every_key(self):
        import boards as tool
        board = (ROOT / 'devices/_template/board.toml').read_text()
        for section, keys in tool.BOARD_SCHEMA.items():
            for key in keys:
                self.assertRegex(board, r'(?m)^#? ?%s = ' % key, f'devices/_template/board.toml does not explain {section or "top"}.{key}')
        soc = (ROOT / 'devices/_template/soc.toml').read_text()
        for section in ('platform', 'cpufreq'):
            for key in tool.SOC_SCHEMA[section]:
                self.assertRegex(soc, r'(?m)^#? ?%s = ' % key, f'devices/_template/soc.toml does not explain {section}.{key}')
        for key in tool.SOC_SCHEMA['blocks']:
            self.assertIn(key, soc)



def fdt(nodes):
    """A flattened device tree of {path: {property: bytes}}, paths in order, for the tests (no dtc needed)."""
    import struct
    strings, structure = b'', b''
    offsets = {}

    def name_offset(name):
        nonlocal strings
        if name not in offsets:
            offsets[name] = len(strings)
            strings += name.encode() + b'\0'
        return offsets[name]

    def pad(b):
        return b + b'\0' * (-len(b) % 4)
    open_path = []
    for path, props in list(nodes.items()) + [('/', None)]:
        parts = [] if path == '/' else path.strip('/').split('/')
        while open_path and open_path != parts[:len(open_path)]:
            structure += struct.pack('>I', 2)
            open_path.pop()
        if props is None:
            break
        for part in parts[len(open_path):]:
            structure += struct.pack('>I', 1) + pad(part.encode() + b'\0')
            open_path.append(part)
        for key, value in props.items():
            structure += struct.pack('>III', 3, len(value), name_offset(key)) + pad(value)
    structure = struct.pack('>I', 1) + pad(b'\0') + structure + struct.pack('>II', 2, 9)
    off_struct = 40 + 16
    off_strings = off_struct + len(structure)
    total = off_strings + len(strings)
    return struct.pack('>10I', 0xd00dfeed, total, off_struct, off_strings, 40, 17, 16, 0, len(strings), len(structure)) + \
        b'\0' * 16 + structure + strings


class FeaturesPage(unittest.TestCase):
    def test_the_committed_page_is_what_the_tool_writes(self):
        written = boards('features')
        self.assertEqual(written.returncode, 0, written.stderr)
        self.assertEqual((ROOT / 'docs/features.md').read_text(), written.stdout, 'run `make docs`')

    def test_every_feature_of_the_kernel_is_on_it(self):
        page = (ROOT / 'docs/features.md').read_text()
        header = (ROOT / 'drivers/board/hakios_board.h').read_text()
        import re
        for name in re.findall(r'HAKIOS_HAS_([A-Z0-9_]+)\s*=\s*1u', header):
            self.assertIn('`' + name.lower().replace('_', '-') + '`', page)


class DeviceTree(unittest.TestCase):
    def tree(self):
        import struct

        def reg(*pairs):
            return b''.join(struct.pack('>QQ', a, s) for a, s in pairs)
        nodes = {
            '/interrupt-controller': {'compatible': b'arm,gic-v3\0', 'reg': reg((0x0d000000, 0x40000), (0x0d040000, 0x200000))},
            '/serial@12002000': {'reg': reg((0x12002000, 0x1000))},
            '/psci': {'method': b'hvc\0'},
            '/mmc@12230000': {'reg': reg((0x12230000, 0x10000))},
            '/msdc@12230000': {'reg': reg((0x12230000, 0x10000))},
            '/msdc@12240000': {'reg': reg((0x12240000, 0x10000))},
            '/scp@11500000': {'reg': reg((0x11500000, 0x80000), (0x115c0000, 0x3000), (0x115c4000, 0x1000))},
            '/toprgu@11007000': {'reg': reg((0x11007000, 0x1000))},
            '/gpio@11005000': {'reg': reg((0x11005000, 0x1000)), 'status': b'disabled\0'},
            '/reserved-memory/pstore': {'reg': reg((0x5e010000, 0xe0000))},
        }
        for i in range(4):
            nodes[f'/cpus/cpu@{i}'] = {'reg': struct.pack('>I', i << 8), 'compatible': b'arm,cortex-a75\0' if i == 3 else b'arm,cortex-a55\0'}
        return fdt(nodes)

    def test_what_the_tree_says(self):
        import devicetree
        found = devicetree.from_dtb(self.tree())
        platform, blocks = found['soc']['platform'], found['soc']['blocks']
        self.assertEqual((platform['gic_dist'], platform['gic_redist'], platform['uart0']), (0x0d000000, 0x0d040000, 0x12002000))
        self.assertEqual((platform['cores'], platform['core_shift'], platform['big_cores'], platform['psci']), (4, 8, [3], 'hvc'))
        self.assertEqual(found['board']['core_order'], [0, 3, 1, 2])
        self.assertEqual((blocks['msdc0'], blocks['msdc1']), (0x12230000, 0x12240000))
        self.assertEqual((blocks['scp_sram'], blocks['scp_cfg'], blocks['scp_clkctrl']), (0x11500000, 0x115c0000, 0x115c4000))
        self.assertEqual(blocks['toprgu'], 0x11007000)
        self.assertNotIn('gpio', blocks)                            # (disabled: left out, not taken)
        self.assertEqual(found['board']['ramoops_region'], (0x5e010000, 0xe0000))
        self.assertTrue(any('gpio' in m for m in found['missing']))
        self.assertTrue(any('framebuffer' in m for m in found['missing']))

    def test_refuses_what_is_not_a_tree(self):
        import devicetree
        with self.assertRaises(ValueError):
            devicetree.from_dtb(b'\0' * 64)


if __name__ == '__main__':
    unittest.main()
