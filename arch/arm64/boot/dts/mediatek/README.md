# SoC files

One file per chip: where its blocks are. A SoC file is shared by every board with that chip, so it
lives here, with the code, and not in a phone's package.

| File | Chip | Used by |
| --- | --- | --- |
| `mt6768.toml` | MediaTek Helio G85 (MT6768; the G80 and G70 are the same chip) | the `x6826b` package |

## What a SoC file says

| Section | Contents | Needed for |
| --- | --- | --- |
| `[platform]` | The interrupt controller, the cores and how they are numbered, which are the big ones, the firmware's power interface, the UART, one window over the peripherals | Every board: the loader builds the kernel's device tree from it |
| `[blocks]` | The address of every peripheral block a driver maps | The loader and every driver: none carries an address of its own |
| `[cpufreq]` | The big cluster's PLL and the speed it can take at the voltage LK leaves | A board's `big_cores_clock = true` in `[loader]` |

Every key is explained in the file itself and in `arch/arm64/boot/dts/_template/soc.toml`.

## Where the values come from

The Android device tree of a phone with the chip, read on the phone as root
(`/sys/firmware/fdt`) and searched with `tools/fdtfind.py FILE WORD`: each key names the node it is
from. What is in no tree is said so in the key's comment.

`tools/boards.py check` validates every SoC file with the boards that use it, and
`tools/boards.py status NAME` says which block a feature of a board still waits for.

A SoC file goes to this branch; a phone's package goes to the `devices` branch.

## The other Helio chips

A SoC file is all the kernel needs to know of a chip; what draws on it is its GPU. The native Metal library
(`userland/src/mali/rwa`) names the chip a board has (`hw.board.soc`, this file's `name`) and its GPU at its start,
and refuses a GPU it does not serve with a sentence saying what is missing (`rwa_helio.c`):

| Chip | Helio | GPU | Native Metal library |
| --- | --- | --- | --- |
| `mt6768` | P65, G70, G80, G85, G88 | Mali-G52 MC2 (Bifrost v7) | served -- run on the X6826B (G85) |
| `mt6785` | G90, G90T, G95 | Mali-G76 MC4 (Bifrost v7) | served by the same descriptors; not run yet |
| `mt6771` | P60, P70 | Mali-G72 MP3 (Bifrost v6) | planned: the library built again with `PAN_ARCH=6` |
| `mt6763` | P23, P30 | Mali-G71 MP2 (Bifrost v6) | planned, as above |
| `mt6781` | G96 | Mali-G57 MC2 (Valhall v9) | planned: a backend of its own (genxml v9) |
| `mt6789` | G99 | Mali-G57 MC2 (Valhall v9) | planned, as above |
| `mt6797`, `mt6757`, `mt6755` | X20-X27, P20/P25, P10 | Mali-T880 / T860 (Midgard) | not planned |
| `mt6761`, `mt6762`, `mt6765`, `mt6779` | A22, P22/G25, P35/G35/G37, P90/P95 | PowerVR | no Mali: no native path |

The GPUs are the vendor's published ones; only `mt6768` has been run. To bring another chip up:

1. Its SoC file here, from the Android device tree of a phone with it (`arch/arm64/boot/dts/_template/soc.toml`, `tools/fdtfind.py`).
   `mali` (the GPU's registers) and `mfgcfg` (its clock gates) are the GPU's blocks.
2. Its GPU brought up by `bsd/dev/mtk_mali.c` (power domains, clocks): the G52's are MT6768's MFG domains, another
   chip's are in its Android kernel's `drivers/gpu/mediatek` and clock driver. The clock's range (`mali_mhz=`,
   `bsd/dev/mtk_keys.c` gpu_raise) is the G52's PLL on MT6768: another chip needs its own.
3. The library's table (`rwa_helio.c`): the chip, its GPU's product id (Panfrost's `pan_props.c`) and `run = true`
   once a phone has drawn with it. The tile buffer the library sizes its tiles by comes from Panfrost's model table;
   the tiler's levels from the GPU's own `TILER_FEATURES`.
