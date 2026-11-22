# XNU on another MediaTek phone: the first boot

What the first phone (an MT6768 with an eMMC) taught, as an order of work for another MediaTek phone:
from a stock Android phone to the kernel's own log, all cores, the root on the flash and the home
screen. The later pieces have their own guides: [Bluetooth](bluetooth-porting.md),
[the camera](camera-porting.md). Read [architecture.md](architecture.md) first.

No second MediaTek phone has been ported with this text yet. It says what one phone needed, in the
order it was needed, and where each thing is in the tree. Expect a step it does not know about.

## What is yours and what is not

| The same on every phone (already written) | Yours to find and write |
| --- | --- |
| The kernel: Apple's xnu-12377.121.6 + `kernel/patches/` + `kernel/overlay/`, built as `DEVELOPMENT_ARM64_BCM2837` | Your phone's package: `devices/NAME/board.toml` (the table below) |
| GICv3, PSCI cores, the return to the loader, the platform expert | `soc/SOC.toml`, if your chip has none yet |
| The loader, the image format and its packer (`loader/chainboot.py`), the run cycle (`tools/run/xnu-cycle.sh`, `tools/run/boot-xnu.sh`) | The storage driver if yours is not an eMMC on MSDC0 left ready by LK |
| The user space side: `darwin/mtk27.sh` for the 27 release, `userland/build.sh` and `darwin/system/fetch.sh` for the earlier system, the inserted libraries | The touch controller, the GPU's power sequence, the keys, the radios, the camera: a driver each, with a feature |
| The log's road back through ramoops (`loader/mtkdbg.h`) | The addresses of the ramoops zones and of LK's framebuffer |

A phone with the same chip and the same parts is a package and nothing else. A phone with another
SoC of the family adds a SoC file and keeps the method. A phone that is not MediaTek keeps the kernel
and the user space side only: `mtkloader` assumes LK, MSDC, TOPRGU and OVL.

## What the phone must allow

1. **An unlocked bootloader** and a way to write one partition (`fastboot flash boot_a`).
2. **Root under Android** (the first phone has Magisk in `boot_a`). The run cycle asks for an XNU boot through `/dev/pmsg0`, reads the log from
   `/sys/fs/pstore` and writes the system image with `dd`: all three need `su`. `loader/build_flashed.py` packs the loader into **your own**
   rooted boot image, `backup/boot_a_magisk.img` by default; it is never in the repository.
3. **ramoops in Android's kernel**: `/sys/fs/pstore/console-ramoops-0` exists after a reboot. This is the only way out for a log: the first phone has no
   reachable serial port, and nothing else survives the reset back into Android.
4. **A way back that does not need the partition you are writing.** On an A/B phone recovery and fastbootd live inside `boot`, so a `boot_a` that
   hangs before LK's keys are read leaves no fastboot. On the first phone that happened once and the boot ROM's download mode with mtkclient
   (`mtk.py w boot_a <the rooted image>`) was the rescue. Check that your phone's boot ROM is reachable **before** the first flash, and keep the
   rooted image at hand. Slot B is never written by anything here.
5. **Space for the system image**: a partition of its own (4 GiB on the first phone, `xnusys`, carved off the end of `userdata`).

## The boot chain

```
boot ROM -> preloader -> LK (stock, untouched)
   LK loads boot_a as an Android boot image (version 2) and starts its "kernel" as a Linux arm64 Image:
   at 0x40080000, MMU and caches off, x0 = LK's device tree
      the "kernel" is gzip of:   mtkloader.bin | padding to CHAIN_OFFSET (0x40000) | Android's own Image | "XNUP" payload (XNU, small root)
         mtkloader: Android, unless one XNU boot was asked for
            XNU: Mach-O copied to free DRAM, a device tree in Apple's format built in memory, boot_args revision 2, LK's framebuffer
               ... the run ends (time, panic, checkpoint) -> back into the loader -> log into ramoops -> warm reset -> Android
```

- `loader/chainboot.py` packs the image; the arm64 header's `res2` is the length of Android's Image and `res3` the payload's offset
  (`loader/start.S`). Android's ramdisk, dtb and command line are kept as they were.
- The payload is `build/payload.bin`, made by `loader/mkpayload.py` as `tools/run/mkpayload.sh` calls it. The
  loader takes two of its entries: `kernel` and `ramdisk` (the small root, md1). `loader/build_flashed.py` then makes
  `build/boot_a.img`.
- **The default is Android.** XNU starts only when the loader finds the words `HAKIOS:BOOT-XNU` in ramoops' pmsg zone, written from
  Android by `tools/run/boot-xnu.sh`, and takes them away: the boot after XNU is Android again. A kernel that hangs costs a power cycle.
  (`-DMENU_DEFAULT_XNU=1` reverses that; an image that always started XNU, with no watchdog, is how the phone was trapped.)
- The loader runs with the MMU off, so all memory is device memory: no SIMD, no unaligned access, no compiler builtins
  (`-mgeneral-regs-only -mstrict-align -fno-builtin`, `loader/build.sh`). Code added to it must hold to that.
- A marker in DRAM (the board file's `marker`, three copies) says how the last run ended: none, run; `NXRN`, XNU was cut short, so copy its log out of DRAM
  and start Android; `NXDN`, XNU came back and its log is saved, start Android.

A boot from RAM alone (`fastboot boot` of such an image, nothing written) has not been made to work
with this loader: every run writes `boot_a`.

## Describe the board

```sh
adb shell su -c 'cat /sys/firmware/fdt' > fdt.dtb     # the live tree, with what LK added at boot
tools/boards.py new NAME --soc mt6768 --description "Maker Model" --dtb fdt.dtb
```

With `--dtb` the tool fills in what the tree says (tools/devicetree.py): for a chip that has no SoC
file it writes one, each key with the node it came from; for one that has, it says whether the phone's
tree agrees with it -- a disagreement means another chip or another revision. It lists what the tree
does not have. The rest of the board file is read from your phone, all under Android, as root, before
any code is run.
`tools/fdtfind.py <file.dtb> <word>` searches a device tree saved to a file
(`adb shell su -c 'cat /sys/firmware/fdt' > fdt.dtb`); the live one is under
`/sys/firmware/devicetree/base`. The example values are the first phone's and are named as such:
**none of them may be carried to another phone unread.**

| What | Key | The first phone's | Where to read it |
| --- | --- | --- | --- |
| Where LK loads the kernel | `[loader] kernel_load` | `0x40080000` | `/proc/iomem` ("Kernel code"), the boot image header |
| ramoops: console zone, its size, pmsg zone | `[ramoops]` | `0x4d09f000`, `0x40000`, `0x4d0e0000`, `0x10000` | the tree's `reserved-memory` pstore node and Android's kernel log: the region is cut into dump records, console, ftrace, pmsg in that order |
| A range of DRAM nothing else uses | `[loader] xnu_base`, `xnu_size` | `0xc0000000`, `0xfe000000` | `reserved-memory` and `/proc/iomem`: clear of LK, Android's Image and ramdisk, ATF, the TEE, the framebuffer, ramoops |
| A place the marker survives a warm reset | `[loader] marker`, `panic_log` | `0x80000000`, `0x80100000` | fbdiag's phase 2 (below) says whether DRAM is kept |
| Free DRAM for staging and tests | `[loader] chain_stub`, `scratch`, `test_buffer` | `0x4a000000`, `0x50000000`, `0xa0000000` | the same maps |
| LK's framebuffer, its pitch, the panel | `[display]` | `0x787b0000`, 2944 bytes (736 pixels: LK rounds 720 up to 32), 720 x 1640 | `reserved-memory`, then fbdiag, which reads the address the display engine scans |
| The processors | `[loader] core_order` | `[0, 7, 6, 1, 2, 3, 4, 5]`: the boot core, the two big ones, the other small ones | `/proc/cpuinfo` (CPU part), the tree's `cpus` |
| The name of the system partition | `[loader] system_partition` | `xnusys` | yours to choose |
| Where fbdiag keeps its marker and what it probes | `[fbdiag]` | the header of ramoops' ftrace zone | the ramoops layout |
| The phone's name to Android | `[boot] android_device` | | `adb shell getprop ro.product.device` |

For a chip without a SoC file, `soc/SOC.toml` takes the tree's `reg` of each block
(`devices/_template/soc.toml`, `soc/mt6768.toml` as the example): the interrupt controller, the UART,
the watchdog (`toprgu`), the display's overlay engine and mutex, the eMMC's controller. The big
cluster's PLL (`[cpufreq]`) comes from the vendor's cpufreq table for your chip; leave
`big_cores_clock` out of the board file until you have read it: it writes a PLL.

```sh
tools/boards.py check        # says which file and why, until everything is consistent
tools/boards.py status NAME  # what is on, what can be listed, what the rest waits for
```

Leave `features = []` for now, and add each as its step below is reached. Three constants are the
kernel's and not the phone's: the kernel's virtual base, the page size, the boot-args format.

The serial number the user space is told is a placeholder (`HAKIOS0000000000`). A real build passes
its own with `-DXNU_SERIAL=...`; it is never committed.

## The order of work

Each step has the check that proves it. Do not start a step whose predecessor's check has not been seen on the phone.

1. **Does code of ours run at all, and can it say so?** `loader/build_fbdiag.sh` packs `fbdiag` in front of Android's kernel. It writes to
   the screen and into the ramoops console, waits five seconds and starts Android. *Check:* `/sys/fs/pstore/console-ramoops-0` begins with
   `fbdiag: phase 1` and a line with the exception level, SCTLR, CPACR, MPIDR and CNTFRQ. If the file is Android's old log, the console zone's address
   is wrong. Six bars on the screen say whether the layer has per-pixel alpha (bar D, white with alpha 0, is invisible if it has). Phase 2, after a
   watchdog reset, says how much of DRAM was kept: that decides whether a log can be rescued from a kernel that hung.
2. **The loader's own log.** Build `mtkloader` with `-DMSDC_TEST=1` (the next step's test: it starts no kernel). *Check:* its lines in the same file. Now every later
   step has a voice: the rule is that code says what it is about to do **before** anything that can hang, so the last line of a log is the step
   that hung.
3. **The flash.** `tools/run/msdc-test.sh` (`-DMSDC_TEST=1`): the loader reads the GPT through `msdc.h`, in PIO mode, with the card as LK left it:
   nothing is initialised. *Check:* `eMMC read test` in the log with your partition names. On a UFS phone this step is a driver you write
   (`bsd/dev/hakios_ufs.c` is the Qualcomm phone's and is a place to start reading, not a fit).
4. **Timers and interrupts, before the kernel.** `-DTIMER_PROBE=1` sets the GIC up as XNU does and arms each timer. Two things stopped the first phone
   here, both silent:
   - `ICC_CTLR_EL1.EOImode` is left set by the firmware below (ATF/GenieZone). XNU writes only `ICC_EOIR1_EL1`, so with it set the first timer
     interrupt is also the last, and the kernel idles for ever "waiting for the root device". The loader clears it on the boot core
     (`gic_fix_eoimode`) and patches one instruction of the kernel's `gic_v3_irq_cpu_init` so that every other core clears it too.
   - XNU's timer is the virtual one (PPI 27). The loader prints CNTPCT against CNTVCT and the exception level it was started at.
   *Check:* the probe's lines name a timer that was taken.
5. **The kernel's first lines.** Build the kernel (`kernel/README.md`), make the payload, run `tools/run/xnu-cycle.sh` with `XNU_ARGS="cpus=1"`. There is
   no serial port, so the loader finds the kernel's console functions by symbol and points them at its own log buffer (`run_xnu`: the patches of
   `vmapple_uart`). If the kernel says nothing, `-DXNU_CHECKPOINT=n` makes it return at the n-th `LOADER_CHECKPOINT` of `osfmk/arm64/start.s` /
   `arm_init.c`, and the log says `(checkpoint)`: bisect with that. `-DXNU_VERBOSE=1` puts the text console on the panel; it is slow and the system cannot
   draw under it. *Check:* the kernel's version line in the log.
6. **Memory and the device tree.** The loader builds the tree the kernel reads (`mtkloader.c`, the device tree builder): the root's identity,
   `chosen` (`dram-base`, `dram-size`, `darwin-board`, `nvram-proxy-data`, the memory map with the RAM disks), `cpus` (`reg` = core << 8),
   `arm-io` with the GIC and the UART, `psci-method` = `smc`. *Check:* the kernel reaches `vm_page_bootstrap` and prints its page counts.
7. **The other cores.** Raise `cpus=` one at a time. `XNU_CORES` and `cpus=` must be the same number (`xnu-cycle.sh` keeps them so): the tree
   offers exactly that many, from the head of `core_order`, and the next one is the watcher's. A watcher on a core the kernel wants hangs every
   rendezvous. *Check:* each processor checks in, in the log.
8. **The root.** `darwin/system/fetch.sh` (your own download of the 20F66 system), `userland/build.sh` for `system.apfs`, the partition
   (`tools/gpt-carve.py` writes a new GPT **to files**; putting it on the phone is your step and the one that can lose `userdata`), then
   `tools/run/system-flash.sh`. With `emmc_root=1` the kernel reads the partition itself (`bsd/dev/mtk_emmc.c`; the measurements are in the
   eMMC journal of the first phone's package: holding the image in DRAM instead cost 2.5 GB wired and 203 s of every boot). For the 27 release the
   image is `darwin/mtk27.sh system`'s, written with `HAKIOS_PAYLOAD=flash` so that the partition's last 96 MiB stay the payload's
   ([building.md](building.md)). *Check:* `launchd` starts; then the home screen appears on
   LK's framebuffer, drawn by the CPU.
9. **The screen as the system wants it.** LK's layer has per-pixel alpha on and the kernel's console writes pixels with alpha 0: nothing is seen. The
   loader switches the alpha off and the other LK layers too. If your panel is command mode and not video mode, pixels written into the
   framebuffer do not appear by themselves: that was not met here.
10. **Touch.** Probe from the loader first (`tools/run/touch-probe.sh`, `-DTOUCH_PROBE=1`): a cycle is four minutes and no kernel is rebuilt. Then
    the kernel driver has one entry point, `hakios_touch_post(type, finger, x, y)`. The first phone's controller (TD4160, `mtk_tcm.c`) keeps its
    firmware in RAM, so the loader uploads it on the way (`tcm.h`); yours may not need that.
11. **The keys and the clocks** (`mtk_keys.c`, through the PMIC wrapper), **the GPU** (`tools/run/gpu-probe.sh`, then `mtk_mali.c`, with
    the native Mali library in user space, [metal-glass.md](metal-glass.md)), **the sensor hub** (`mtk_scp.c`; the sensors' journal of the
    first phone's package).
    `mtk_cpufreq=` and `mali_mhz=` change PLLs and, above some points, voltages: read your chip's table first.
12. **The radios, the camera, the battery**: Wi-Fi (CONNSYS from the phone's own firmware; the Wi-Fi journal of the first phone's package), [Bluetooth](bluetooth-porting.md),
    [the camera](camera-porting.md), `mtk_battery.c` (the PMIC's gauge).

## One run

The first time, from a new clone, there is more before these four: the kernel's sources and dependencies, zig for the loader, the files of the system, the
system image in its partition and your boot image in `backup/`. [building.md](building.md) has every command.

```
export HAKIOS_SERIAL=<your phone's adb serial>          # never in a file
kernel/build.sh -j8 -k                                   # the kernel (kernel/README.md for the first time)
./userland/build.sh                                     # the small root, userland/root.apfs (its head comment lists the switches)
tools/run/mkpayload.sh         # the payload: kernel + the small root
XNU_LOG_OUT=logs/run1.log tools/run/xnu-cycle.sh      # the board file's [boot]: 120 s, cpus=4 touchscreen=2 mali=1 fbscale=2 fbfit=3
```

`xnu-cycle.sh` builds the loader with the run's flags (anything after the script's name goes to the compiler, such as `-DXNU_CHECKPOINT=17`), packs
and flashes `boot_a` (and refuses if the current slot is not `a`), waits for Android, asks for one XNU boot, waits for Android again and reads the
log. The kernel returns by itself after `XNU_SECS` (`loader_return_after=`); a watcher on a spare core takes a snapshot and resets the board
`XNU_WATCH` seconds in if it has not. The log holds 256 KB of console and what the kernel kept for the host (`kern.hakios.host_keep`, through the
pmsg zone, 64 KB): a long run loses its beginning, so keep runs short and say less.

## The kernel's side

- A driver is a file `kernel/overlay/bsd/dev/` (`mtk_<name>.c` for a MediaTek block, `hakios_<name>.c` for what any phone has), a line in
  `bsd/conf/files` (in `kernel/patches/0001-build.patch`), and a call to its start function in `hakios_drivers.c`. There is no clock, pin, regulator or bus framework: a driver maps its registers and writes them, and says in its head comment
  which Linux source each layout was read from. GPL sources are read and named, never copied.
- **Every driver is off without its boot-arg** and starts only on a board that lists its feature ([features.md](features.md)). The first
  phone's: `emmc_root=1`, `touchscreen=2`, `buttons=2`, `mali=1`, `metal=1`, `connsys_step=17`, `scp_step=7`, `mtkcam=5`, `mtkbatt=1`.
  Give your phone's drivers their own values or names; do not reuse a value that maps another phone's registers.
- Drivers that share the PMIC wrapper take `mtk_pwrap_lock()` around every transaction.
- Two skeletons hold the phone-independent half of a subsystem and refuse to run until a table of operations is complete:
  `mtk_bt_skel.c`, `mtk_camera_skel.c`. A new subsystem is worth writing that way from the start.

## What stopped the first phone, in one list

| What was seen | What it was |
| --- | --- |
| Android's old log in pstore, nothing of ours | the wrong console zone address, or the loader not started (the image's header) |
| The kernel "waits for the root device" for ever | no timer interrupt after the first: EOImode (step 4) |
| A black panel while the log says the console draws | per-pixel alpha on LK's layer (step 9) |
| A 15 minute run on the Apple logo | the watcher on a core the kernel was offered (step 7) |
| Jetsam killing everything minutes in | the system image held in DRAM (step 8) |
| The boot image does not fit | the loader is built `-Os`; `boot_a` is 32 MB here and the kernel plus the small root must fit |
| `rootvp not authenticated`, or a reboot looking for a preboot volume | `rd=md0` must stay on the command line beside `emmc_root=1` (why: the eMMC journal of the first phone's package) |
| The phone boots only into a hung XNU | an image that always started XNU; the boot ROM was the way back |

## What is never in the tree

Nothing of Apple's, nothing of the vendor's (no firmware, no partition image, no device tree, no battery table), no serial number, no home path, no
network name. A port brings its own phone's files from its own phone.
