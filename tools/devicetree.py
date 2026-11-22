"""What a MediaTek phone's Android device tree says of its chip and of the board, for tools/boards.py new.

  from devicetree import from_dtb
  found = from_dtb(open('fdt.dtb', 'rb').read())

`found` has 'soc' (the sections [platform] and [blocks] of a SoC file), 'board' (what of a board file
the tree can say: the cores' order; the ramoops zones and LK's framebuffer where the tree has them),
'sources' (for each key, the node it was read from, as a comment for the file) and 'missing' (what was
looked for and not found, in words). Nothing is guessed: a key whose node is not there is left out.

Take the tree from the running phone, where LK's own nodes have been added (as root under Android:
`cat /sys/firmware/fdt`). The tree inside a boot image lacks the memory LK reserves at boot (ramoops,
the framebuffer); the chip's blocks are in both.
"""
import struct

FDT_MAGIC = 0xd00dfeed
FDT_BEGIN, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def parse(data):
    """[(path, {property: bytes})] in the order of the tree."""
    magic, _, off_struct, off_strings = struct.unpack_from('>4I', data, 0)
    if magic != FDT_MAGIC:
        raise ValueError('not a flattened device tree (no magic 0xd00dfeed)')
    at, path, out = off_struct, [], []
    while True:
        tag, = struct.unpack_from('>I', data, at)
        at += 4
        if tag == FDT_BEGIN:
            end = data.index(b'\0', at)
            path.append(data[at:end].decode('utf-8', 'replace'))
            out.append(('/'.join(path) or '/', {}))
            at = (end + 4) & ~3
        elif tag == FDT_END_NODE:
            path.pop()
        elif tag == FDT_PROP:
            length, name_off = struct.unpack_from('>II', data, at)
            at += 8
            name = data[off_strings + name_off:data.index(b'\0', off_strings + name_off)].decode()
            out[-1][1][name] = data[at:at + length]
            at = (at + length + 3) & ~3
        elif tag == FDT_NOP:
            continue
        elif tag == FDT_END:
            return out
        else:
            raise ValueError(f'a bad tag {tag} in the tree')


def strings(value):
    return [s.decode('utf-8', 'replace') for s in value.split(b'\0') if s]


def regs(value):
    """reg as (address, size) pairs, for #address-cells = #size-cells = 2 (what MediaTek's trees use)."""
    if len(value) % 16:
        return []
    return [struct.unpack_from('>QQ', value, i) for i in range(0, len(value), 16)]


# key of [blocks] -> (node name at the top of the tree, before its @; which reg entry)
BLOCKS = {
    'topckgen': ('topckgen', 0), 'infracfg_ao': ('infracfg_ao', 0), 'gpio': ('gpio', 0), 'spm': ('sleep', 0),
    'toprgu': ('toprgu', 0), 'apmixed': ('apmixed', 0), 'pwrap': ('pwrap', 0), 'keypad': ('kp', 0),
    'scp_sram': ('scp', 0), 'scp_cfg': ('scp', 1), 'scp_clkctrl': ('scp', 2),
    'ap_dma_btif_tx': ('btif', 1), 'btif': ('btif', 0), 'i2c2': ('i2c2', 0), 'i2c4': ('i2c4', 0), 'spi2': ('spi2', 0),
    'msdc0': ('mmc', 0), 'msdc1': ('msdc', 0), 'mipi_rx': ('mipi_rx_ana_csi0a', 0), 'mfgcfg': ('mfg_cfg', 0),
    'mali': ('mali', 0), 'mmsys': ('mmsys_config', 0), 'disp_mutex0': ('disp_mutex0', 0), 'smi_common': ('smi_common', 0),
    'disp_ovl0': ('disp_ovl0', 0), 'conn': ('wifi', 0), 'conn_mcu': ('consys', 0), 'conn_hif_on': ('consys', 4),
    'conn_top_misc_off': ('consys', 5), 'camsys': ('camsys', 0), 'smi_larb3': ('smi_larb3', 0), 'seninf': ('seninf1', 0),
    'camsv0': ('camsv1', 0), 'camsv1': ('camsv2', 0),
}
# blocks that are in no node of their own (a window of another's): said so, never filled in
NOT_IN_TREE = ('conn_wbg', 'conn_misc_on')


def from_dtb(data):
    nodes = parse(data)
    top = {}
    for path, props in nodes:
        parts = path.strip('/').split('/')
        if len(parts) == 1 and parts[0]:
            base = parts[0].split('@')[0]
            if 'reg' in props and props.get('status', b'okay\0').rstrip(b'\0') in (b'okay', b'ok'):
                top.setdefault(base, (path, props))
    soc = {'platform': {}, 'blocks': {}}
    board = {}
    sources, missing = {}, []

    # the interrupt controller: the node called interrupt-controller, or the one compatible with a GICv3
    gic = top.get('interrupt-controller')
    if gic is None:
        gic = next(((p, v) for p, v in nodes if b'arm,gic-v3' in v.get('compatible', b'')), None)
    if gic and len(regs(gic[1].get('reg', b''))) >= 2:
        (dist, _), (redist, _) = regs(gic[1]['reg'])[:2]
        soc['platform']['gic_dist'], soc['platform']['gic_redist'] = dist, redist
        sources['platform.gic_dist'] = f'{gic[0]}: reg, first entry'
        sources['platform.gic_redist'] = f'{gic[0]}: reg, second entry'
    else:
        missing.append('the interrupt controller (a node compatible with arm,gic-v3, two reg entries)')
    serial = next(((p, v) for p, v in nodes if p.count('/') == 1 and p.startswith('/serial@') and 'reg' in v), None)
    if serial:
        soc['platform']['uart0'] = regs(serial[1]['reg'])[0][0]
        sources['platform.uart0'] = f'{serial[0]}: the first UART'
    else:
        missing.append('a UART (/serial@...)')

    # the cores: their MPIDR numbering, and which are the big ones (the compatible that is not the most
    # common one, or the higher capacity-dmips-mhz where the tree has it)
    cpus = [(p, v) for p, v in nodes if p.startswith('/cpus/cpu@') and p.count('/') == 2]
    if cpus:
        mpidr = [struct.unpack('>I', v['reg'][-4:])[0] for _, v in cpus if 'reg' in v]
        soc['platform']['cores'] = len(cpus)
        shift = 8 if all(m & 0xff == 0 for m in mpidr) and any(mpidr) else 0
        soc['platform']['core_shift'] = shift
        sources['platform.cores'] = '/cpus: the cpu@ nodes'
        sources['platform.core_shift'] = "/cpus: their reg is 0x000, 0x100 ...: MPIDR's second affinity field" if shift else \
            '/cpus: their reg counts up from 0'
        kinds = [strings(v.get('compatible', b''))[:1] for _, v in cpus]
        caps = [struct.unpack('>I', v['capacity-dmips-mhz'])[0] if len(v.get('capacity-dmips-mhz', b'')) == 4 else None
                for _, v in cpus]
        if all(c is not None for c in caps) and len(set(caps)) > 1:
            big = [i for i, c in enumerate(caps) if c == max(caps)]
            sources['platform.big_cores'] = '/cpus: the cores of the highest capacity-dmips-mhz'
        else:
            common = max(set(map(tuple, kinds)), key=lambda k: kinds.count(list(k)))
            big = [i for i, k in enumerate(kinds) if tuple(k) != common]
            sources['platform.big_cores'] = '/cpus: the cores whose compatible is not the most common one (' + \
                ', '.join(sorted({k[0] for k in kinds if k})) + ')'
        soc['platform']['big_cores'] = big
        small = [i for i in range(len(cpus)) if i not in big]
        board['core_order'] = [0] + sorted((i for i in big if i != 0), reverse=True) + [i for i in small if i != 0]
        sources['loader.core_order'] = 'the boot core, the big cores, then the other small ones (from /cpus)'
    else:
        missing.append('the cores (/cpus/cpu@...)')
    psci = next((v for p, v in nodes if p == '/psci'), None)
    if psci and 'method' in psci:
        soc['platform']['psci'] = strings(psci['method'])[0]
        sources['platform.psci'] = '/psci: method'
    else:
        missing.append('/psci with a method')
    debug = top.get('cpu_dbgapb')
    if debug:
        soc['platform']['debug_core0'] = regs(debug[1]['reg'])[0][0]
        sources['platform.debug_core0'] = f'{debug[0]}: core 0\'s debug registers'

    for key, (name, index) in BLOCKS.items():
        node = top.get(name)
        entries = regs(node[1]['reg']) if node else []
        if len(entries) > index:
            soc['blocks'][key] = entries[index][0]
            sources[f'blocks.{key}'] = f'{node[0]}' + (f', reg {index}' if index else '')
        else:
            missing.append(f'[blocks] {key} (a node {name}@... with {index + 1} reg entries)')
    # the SD card's controller: a msdc@ node other than the eMMC's (the tree has one msdc@ for each, and
    # the eMMC's also as mmc@)
    if 'msdc0' in soc['blocks']:
        other = [(p, regs(v['reg'])[0][0]) for p, v in nodes if p.count('/') == 1 and p.startswith('/msdc@') and regs(v.get('reg', b''))
                 and regs(v['reg'])[0][0] != soc['blocks']['msdc0']]
        if other:
            soc['blocks']['msdc1'], sources['blocks.msdc1'] = other[0][1], f'{other[0][0]} (the controller that is not the eMMC\'s)'
        else:
            soc['blocks'].pop('msdc1', None)
            sources.pop('blocks.msdc1', None)
            missing.append('[blocks] msdc1 (a second msdc@ node)')
    for key in NOT_IN_TREE:
        missing.append(f'[blocks] {key}: in no node of the tree; read it from the vendor\'s driver')

    # the window over the peripherals: from the interrupt controller to the end of the last block, rounded
    addresses = [a for a in list(soc['blocks'].values()) + [soc['platform'].get('gic_dist')] if a]
    if addresses:
        low = min(addresses) & ~0xffffff
        high = (max(addresses) + 0x1000000) & ~0xffffff
        soc['platform']['io_base'], soc['platform']['io_size'] = low, high - low
        sources['platform.io_base'] = 'the lowest block found, rounded down to 16 MB'
        sources['platform.io_size'] = 'to past the highest block found, rounded up to 16 MB'

    # what LK adds to the live tree: ramoops' region and the framebuffer
    for path, props in nodes:
        if not path.startswith('/reserved-memory/') or 'reg' not in props:
            continue
        name = path.rsplit('/', 1)[1].lower()
        entries = regs(props['reg'])
        if not entries:
            continue
        address, size = entries[0]
        if 'pstore' in name or 'ramoops' in name or b'ramoops' in props.get('compatible', b''):
            board['ramoops_region'] = (address, size)
            sources['ramoops.region'] = f'{path}: reg (the console zone follows the dump records: read their size in Android\'s log)'
        elif 'framebuffer' in name:
            board['fb_base'], board['fb_size'] = address, size
            sources['display.fb_base'] = f'{path}: reg'
    if 'ramoops_region' not in board:
        missing.append('ramoops\' region (a reserved-memory node with "pstore" or "ramoops" in it: take the live tree)')
    if 'fb_base' not in board:
        missing.append('LK\'s framebuffer (reserved-memory "framebuffer": take the live tree, or arch/arm64/boot/fbdiag.c says it)')
    return {'soc': soc, 'board': board, 'sources': sources, 'missing': missing}
