# What each feature needs

A board lists the drivers it may start in `features` of its `board.toml`. Each needs to be told where
things are first: blocks of its chip's SoC file (`[blocks]` of `arch/arm64/boot/dts/mediatek/SOC.toml`, shared by every phone
with that chip) and, for some, a section of the board file. `tools/boards.py check` refuses a board
that lists a feature without them, and `tools/boards.py status NAME` says the same for one phone.

This page is written by `tools/boards.py features --write` (`make docs`) from the tables the checker
uses and the comments of `drivers/board/hakios_board.h`; a test keeps the three the same.
What each key means is in `arch/arm64/boot/dts/_template/`; the order to turn features on in is
[porting.md](porting.md)'s.

| Feature | What it is | SoC file `[blocks]` | Board file | Started with |
|---|---|---|---|---|
| `emmc-msdc` | the root on an eMMC behind MSDC0, as LK left the card (emmc_root=1) | `msdc0` | `[loader]` | emmc_root=1: the root on the flash, read by the kernel |
| `keys-mt6358` | power and volume keys: the keypad block and the MT6358's key detector through the PMIC wrapper; the cores' and the GPU's clocks beside them (buttons=2) | `keypad`, `pwrap`, `apmixed`, `topckgen` | nothing | buttons=2; mtk_cpufreq=1 looks at the clocks, =2 raises them; mali_mhz=N |
| `gpu-mali-g52` | a Mali-G52 in MFG's power domains (mali=1) | `topckgen`, `infracfg_ao`, `spm`, `mfgcfg`, `mali` | nothing | mali=1 |
| `scp-sensors` | the sensor hub (SCP) and its accelerometer (scp_step=N) | `scp_sram`, `scp_cfg`, `scp_clkctrl`, `topckgen`, `infracfg_ao` | nothing | scp_step=N: 1 only reads; 7 keeps the hub and its accelerometer running |
| `battery-mt6358` | the MT6358's fuel gauge and BATADC; the cell is the board's [battery] (mtkbatt=1) | `pwrap` | `[battery]` | mtkbatt=1 |
| `display-ovl-flip` | frames shown by pointing OVL0's layer 0 at a second picture inside LK's framebuffer (MiniIOMobileFramebuffer.cpp, fb_flip=1) | `disp_ovl0` | `[display]` | fb_flip=1 |
| `touch-td4160` | an OmniVision TD4160 (TouchComm) on SPI2, its pins bit-banged (touchscreen=2) | `spi2`, `gpio` | `[display]` | touchscreen=2 |
| `connsys-mt6631` | CONNSYS with an MT6631: Wi-Fi and Bluetooth (connsys_step=N) | `spm`, `pwrap`, `gpio`, `toprgu`, `infracfg_ao`, `btif`, `ap_dma_btif_tx`, `conn`, `conn_mcu`, `conn_hif_on`, `conn_top_misc_off`, `conn_wbg`, `conn_misc_on` | `[connsys]` | connsys_step=N: 1 and 2 only read, 16 makes the radio en0, 17 also joins fw/network; connsys_bt=2 for Bluetooth |
| `camera-s5kjn1` | a Samsung S5K JN1 on CSI through SENINF and CAMSV (mtkcam=1) | `pwrap`, `gpio`, `topckgen`, `infracfg_ao`, `spm`, `smi_common`, `mmsys`, `camsys`, `apmixed`, `mipi_rx`, `seninf`, `camsv1`, `smi_larb3`, `i2c4` | nothing | mtkcam=1 makes one capture; mtkcam=5 serves frames on /dev/hakios-camera |

`touch-td4160`, `connsys-mt6631`, `camera-s5kjn1` are drivers of one part, wired the way one phone has it: list one only if your phone has that
part on the same pins and bus ([drivers.md](drivers.md)). The loader has a part of its own for
`touch-td4160`, `connsys-mt6631`, `gpu-mali-g52`, `camera-s5kjn1`, compiled in only for a board that lists the feature.

A feature listed is not a feature started: every driver still waits for its boot argument, and its
first level only reads and logs.
