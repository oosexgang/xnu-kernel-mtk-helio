# Drivers

Every driver of `drivers/board/` and `iokit/Kernel/`: what it is for, what starts it, and
how tied to one phone it still is. A driver starts only with its boot argument, and a MediaTek driver
only on a board whose file lists its feature.

## MediaTek's blocks

| Driver | Feature | Boot argument | What comes from data | What is still in the code |
| --- | --- | --- | --- | --- |
| `mtk_emmc.c`: the root on the eMMC, read through MSDC0 as LK left the card | `emmc-msdc` | `emmc_root=1` | the controller's address, the partition's name | reads by descriptor DMA (checked against PIO at mount, PIO when it fails); it never writes |
| `mtk_keys.c`: power and volume keys (keypad block, the MT6358's detector through the PMIC wrapper); the cores' and the GPU's clocks | `keys-mt6358` | `buttons=2`; `mtk_cpufreq=1` looks, `=2` raises the A75s to 2000 MHz, `=3` the A55s to 1700 too; `mali_mhz=N` (950 the fastest used) | the four blocks' addresses | the MT6768's frequency table and PLL registers; which key is which |
| `mtk_mali.c`, `HakiosMali.cpp`: the Mali-G52 woken step by step, then kept on for programs: buffers, submissions to the two job slots, an IOSurface's memory as a target (the requests of Linux's panfrost DRM driver, which the native library and Gallium's panfrost both speak) | `gpu-mali-g52` | `mali=1` | the five blocks' addresses | MFG's power domain sequence of the MT6768; the MT6358 rail's voltage |
| `mtk_scp.c`: the sensor hub and its accelerometer, whose orientation reaches backboardd | `scp-sensors` | `scp_step=N` (1 only reads, 7 keeps the hub running) | the blocks' addresses | the firmware's layout as this chip's LK loads it |
| `mtk_battery.c`: the MT6358's fuel gauge, BATADC, the charger's presence | `battery-mt6358` | `mtkbatt=1` | the PMIC wrapper's address, the cell's charge | the 4.45 V cell's voltage curve; no charger chip is driven, no temperature is read |
| `mtk_connsys.c`, `mtk_wifi.c`, `mtk_btsk.c`: CONNSYS with an MT6631, Wi-Fi and Bluetooth from the phone's own firmware | `connsys-mt6631` | `connsys_step=N` (1 and 2 only read; 16 the radio as `en0`, 17 also joins `fw/network`), `connsys_wake=0`, `connsys_bt=2` (the controller for bluetoothd), `bt_dump=0` | every block's address, CONNSYS's reserved memory | the MT6631's bring-up sequence, pins and PMIC supplies as the first phone wires them |
| `mtk_tcm.c`: a TouchComm controller (OmniVision TD4160) on SPI2, bit-banged | `touch-td4160` | `touchscreen=2` | SPI2's and the GPIO block's addresses, the panel's size | the pins (25 to 28, interrupt on 1) |
| `mtk_camera.c`: a Samsung S5K JN1 through SENINF and CAMSV: one capture, or a stream served on `/dev/hakios-camera` | `camera-s5kjn1` | `mtkcam=1` (one capture), `mtkcam=5` (frames to user space); never with CONNSYS (a shared PMIC channel) | nothing yet | every address, rail and pin; the register tables are made from your own phone's capture |
| `mtk_bt_skel.c`, `mtk_camera_skel.c`: the phone-independent half of Bluetooth and of the camera, each waiting for a table of operations | none | `mtkbt_skel=1`, `mtkcam_skel=1` | by construction | compiled, not run on a phone |

`MiniIOMobileFramebuffer.cpp`'s flip of OVL0's layer between two pictures in LK's framebuffer is the
feature `display-ovl-flip` (`fb_flip=1`); the overlay engine's address and the framebuffer's place are
data.

Two addresses of the MT6768 remain in platform code as fall-backs for a device tree that has none:
the interrupt controller's in `osfmk/arm64/gic_v3_irq.c` and the UART's in
`iokit/Kernel/GenericARMPE.cpp`. The loader always passes the interrupt controller in the tree it
builds.

## The same on any phone

| Driver | What it is |
| --- | --- |
| `hakios_board.c` | The board's row of the table, `hw.board.*` |
| `hakios_input.c` | `/dev/hakios-touch`: what a touch driver posts and backboardd's side reads; the five-finger way out |
| `hakios_net.c`, `hakios_sta.c`, `hakios_apple80211.c`, `hakios_wpa.c`, `hakios_wifi_given.c` | `en0` through the kernel's network KPI, the Wi-Fi station's side, Apple80211's requests answered, the WPA2 handshake, the network given in a file |
| `hakios_audio.c` | `/dev/hakios-audio`: where the audio plug-in writes what is played; a recording of it |
| `hakios_battery.c`, `HakiosBattery.cpp` | `kern.hakios.battery_*` and the power source powerd finds, fed by a gauge driver |
| `HakiosWLAN.cpp`, `HakiosBluetooth.cpp` | The registry's side of Wi-Fi and Bluetooth |
| `hakios_image.c`, `hakios_sparse.c`, `hakios_apfs_check.c` | A file of a RAM disk by path; a sparse image as a block device; a walk of a whole volume |
| `hakios_guard.c` | A speaker guard (limiter); nothing on a MediaTek phone feeds it yet |
| `hakios_fbgrab.c`, `hakios_ustack.c` | The picture of the screen into the log (`fbshot=`); user stacks for diagnostics |
| `hakios_virtio_*.c` | An emulator's devices; not started on a phone |
| `Mini*.cpp`, `GenericARMPE.cpp` | Stand-ins for the closed kernel extensions |

## The loader's parts

| Header | Feature | What it does |
| --- | --- | --- |
| `tcm.h`, `mtkspi.h` | `touch-td4160` | uploads the controller's firmware before the kernel starts |
| `mtkconnsys.h` | `connsys-mt6631` | prints what Android's tree says of CONNSYS |
| `mtkmali.h`, `mtkpwrap.h` | probe (`-DGPU_PROBE=1`) | wakes the GPU and asks it what it is |
| `mtkcam.h` | probe (`-DCAM_PROBE=1`) | powers the camera and reads the sensor's identity |
