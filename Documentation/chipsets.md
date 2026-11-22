# Chipsets: what a MediaTek chip gives the drivers

A chip is described once, in its SoC file (`arch/arm64/boot/dts/mediatek/SOC.toml`): where its interrupt
controller, cores and peripheral blocks are, and the big cluster's PLL. Every board with that chip shares it; a
board's own file (`arch/arm64/boot/dts/NAME/board.toml`) says which features it has and where its memory, panel and
cell are. The drivers take every address from these two files through `hakios_board()` in the kernel and
`hakios_platform.h` in the loader, and start only on a board that lists their feature
([features.md](features.md)).

One chip has a SoC file and has run: the **MT6768** (Helio G85 on the first phone; the G80 and G70 are the same
chip): two Cortex-A75s and six Cortex-A55s, a GICv3, PSCI through `smc`, a Mali-G52 MC2, the MT6358 PMIC behind the
PMIC wrapper, CONNSYS with an MT6631. The other Helio chips, their GPUs and what the native Metal library says of
each are in [the SoC files' README](../arch/arm64/boot/dts/mediatek/README.md#the-other-helio-chips).

## What each driver takes from the chip, and what is still one chip's

What comes from data moves to another chip with its SoC file. What is still in the code has to be checked, and
changed where the new chip differs, before the driver is started there.

| Driver | Feature | From the SoC file (`[blocks]`) | Still the MT6768's (or the first phone's) in the code |
| --- | --- | --- | --- |
| The eMMC (`drivers/board/mtk_emmc.c`) | `emmc-msdc` | `msdc0` | Nothing of the chip: MSDC as LK leaves it, read by descriptor DMA, never written |
| The keys and the clocks (`mtk_keys.c`) | `keys-mt6358` | `keypad`, `pwrap`, `apmixed`, `topckgen` | The MT6768's frequency table and PLL registers, which key is which; the PMIC is an MT6358 |
| The GPU (`mtk_mali.c`, `drivers/iokit/HakiosMali.cpp`) | `gpu-mali-g52` | `topckgen`, `infracfg_ao`, `spm`, `mfgcfg`, `mali` | MFG's power domain sequence of the MT6768, the MT6358 rail's voltage, the clock's range (`mali_mhz=`) |
| The sensor hub (`mtk_scp.c`) | `scp-sensors` | `scp_sram`, `scp_cfg`, `scp_clkctrl`, `topckgen`, `infracfg_ao` | The firmware's layout as this chip's LK loads it |
| The battery (`mtk_battery.c`) | `battery-mt6358` | `pwrap` | The MT6358's gauge; the cell's voltage curve (the board's `[battery]` gives its charge) |
| The display's flip (`drivers/iokit/MiniIOMobileFramebuffer.cpp`) | `display-ovl-flip` | `disp_ovl0` | OVL0's layer 0 as LK programs it |
| CONNSYS: Wi-Fi, Bluetooth (`mtk_connsys.c`, `mtk_wifi.c`, `mtk_btsk.c`) | `connsys-mt6631` | `spm`, `pwrap`, `gpio`, `toprgu`, `infracfg_ao`, `btif`, `ap_dma_btif_tx`, `conn*` | The MT6631's bring-up sequence, its pins and PMIC supplies as the first phone wires them |
| The touch screen (`mtk_tcm.c`) | `touch-td4160` | `spi2`, `gpio` | A part, not the chip: the TD4160 on the first phone's pins |
| The camera (`mtk_camera.c`) | `camera-s5kjn1` | `seninf`, `camsv1`, `mipi_rx`, `camsys`, `smi_*`, `mmsys`, `i2c4`, … | A part, not the chip: every rail and pin; the register tables are made from your own phone's capture |

Two addresses of the MT6768 remain in platform code as fall-backs for a device tree that has none: the interrupt
controller's in `arch/arm64/kernel/gic_v3_irq.c` and the UART's in `drivers/iokit/GenericARMPE.cpp`. The loader
always passes the interrupt controller in the tree it builds.

## Another chip

1. **Its SoC file**, from the Android device tree of a phone with it: `tools/boards.py new NAME --soc SOC --dtb
   FILE` writes one when the chip has none, every key naming the node it is from; `tools/fdtfind.py` finds the
   rest. `arch/arm64/boot/dts/_template/soc.toml` explains each key.
2. **Its first runs** with nothing on: `XNU_DEFCONFIG=phone_first` (one core), then the board's features one at a
   time, each first at the level that only reads ([porting.md](porting.md)).
3. **What is still one chip's** in the table above, checked against the chip's Android kernel before the driver
   writes anything: the PMIC (an MT6357 or MT6359 is not an MT6358), the GPU's power domains and clock, CONNSYS's
   radio chip.
4. **The GPU's generation** for Metal: the native library writes Bifrost's descriptors (Mali-G52 and G76); another
   generation is refused with a sentence that says so (`userland/src/mali/rwa/rwa_helio.c`).

A SoC file goes to the main branch; a phone's package to the `devices` branch.
