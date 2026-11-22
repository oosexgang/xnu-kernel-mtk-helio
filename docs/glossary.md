# Glossary

The words of three worlds, one line each: this project's, MediaTek's and Android's, Apple's. Where a
word names a file or a tool, the path says where it is.

## This project

| Word | What it is |
| --- | --- |
| **Assembled tree** | Apple's kernel release with `kernel/overlay/` copied in and `kernel/patches/` applied: what is compiled (`kernel/assemble.sh`, `../xnu-12377-hakios`). |
| **Board file** | `NAME/board.toml` of a device package: what one phone has, where its memory and panel are, which features it may start. |
| **Boot argument** | A word on the kernel's command line (`mali=1`); every driver waits for its own. The list: [boot-args.md](boot-args.md). |
| **Device package** | A phone's directory (`board.toml`, README, journal), kept on the `devices` branch and brought in by `tools/device.py add`. |
| **Fake kernel** | `workbench/fakekernel/`: enough of XNU for a driver's own `.c` file to run as a host program. |
| **Feature** | A driver a board may start (`gpu-mali-g52`), listed in its board file; the kernel's `HAKIOS_HAS_*`. The list: [features.md](features.md). |
| **Job list** | `userland/jobs/rc.jobs*`: what our launchd job `com.hakios.demo` runs, a command a line; how a run moves the interface by itself. |
| **Job cache** | launchd's own list of the system's jobs; with `JOBCACHE=1` the small image carries it and `init` binds it over the system's, so jobs change without a new system image. |
| **Kept lines** | What the kernel keeps for the host (`kern.hakios.host_keep`, 64 KB, through the pmsg zone): lines a program says beginning with `!!`. They survive when the log's middle is lost. |
| **kmali** | `userland/src/mali/rwa/kmali.c`: the native library's GPU memory and submissions, over HakiosMali. |
| **Loader** | `mtkloader` (`loader/`): what LK starts in place of a kernel; it chooses Android or XNU, builds XNU's device tree, and takes the log back. |
| **Model** | A host program's stand-in for a block's registers (`workbench/pwrap_model.h`), behind a driver's `rd()` and `wr()`. |
| **Native library** | `libhakiosmali.dylib` (`userland/src/mali/rwa/`): `air_metal.h`'s contract served with the Mali's job descriptors written by us. |
| **Payload** | The kernel and the small image in one container ("XNUP", `tools/run/mkpayload.sh`), in the last 96 MiB of the system partition. |
| **Recipe** | One pipeline a process built, as the Metal device says it in a run's log (`rwr` lines); harvested into `userland/share/rwm-recipes27.txt`. |
| **Run** | One boot of XNU: `boot_a` written, one XNU boot asked for, the log read back (`tools/run/xnu-cycle.sh`). |
| **Small image** | `root26-sys.apfs`: our programs, libraries and job list, mounted on `/hakios` (`md1`). |
| **SoC file** | `soc/SOC.toml`: where the blocks of one chip are, shared by every board with it. |
| **Stage** | One compiled shader of a pipeline (a vertex, fragment or compute function with its constants). |
| **Stage cache** | Stages compiled on the Mac beforehand (`userland/tools/rwacache.sh`), loaded by the native library instead of compiled on the phone. |
| **stage0** | `/sbin/launchd` of the system image: ours; it mounts the small image and runs our `init`. |
| **System image** | The system's files as an APFS image in a partition of its own (`xnusys` on the first phone), the root. |
| **Variant** | One configuration of a generic pipeline (`fixed_frag_lph_cpf` with its configuration buffer), harvested into `rwm-variants27.txt`. |
| **Watcher** | The loader's code on a spare core: takes a snapshot and resets the board when a kernel never returns. |

## MediaTek and Android

| Word | What it is |
| --- | --- |
| **ATF, GenieZone** | The secure firmware below the kernel; on the first phone it left `ICC_CTLR_EL1.EOImode` set ([porting.md](porting.md), step 4). |
| **`boot_a`, slot `a`/`b`** | Android's A/B boot partitions. This port writes `boot_a` every run and never slot `b`. |
| **BROM** | The boot ROM. Its download mode (with mtkclient) can rewrite any partition: the way back from a broken `boot_a`. |
| **BTIF** | The serial interface between the application processor and CONNSYS's Bluetooth. |
| **CONNSYS** | The connectivity subsystem: its own CPU and firmware for Wi-Fi, Bluetooth and GPS, with the radio chip (an MT6631 on the first phone). |
| **eMMC, MSDC** | The flash, and the MediaTek SD/MMC controller in front of it (MSDC0). |
| **EMI** | The external memory interface; CONNSYS's and Wi-Fi's reserved memory are ranges of it. |
| **GPT** | The partition table. New partitions are cut with `tools/gpt-carve.py` into files, never by the tools onto the phone. |
| **LK** | Little Kernel, MediaTek's bootloader: it loads `boot_a` and starts its kernel, which here is our loader. |
| **Magisk** | The root this port needs under Android: `su` for the request, the log and the partition. |
| **MFG** | The GPU's power domains and clocks; the Mali sits in them. |
| **MMSYS, OVL** | The display subsystem, and its overlay engine whose layer 0 shows LK's framebuffer. |
| **MT6358** | The PMIC of the first phone: keys, the fuel gauge, the supplies. |
| **PMIC wrapper (PWRAP)** | The bridge through which the application processor reads and writes the PMIC. |
| **pmsg, ramoops, pstore** | Android's memory that survives a warm reset: the console zone carries the kernel's log back, the pmsg zone the request for a boot and the kept lines. |
| **Preloader** | MediaTek's first stage after the BROM; it loads LK. |
| **SCP, nanohub** | The sensor processor and its firmware's protocol; the accelerometer is behind it. |
| **SENINF, CAMSV** | The camera's receiver and the block that writes its frames to memory. |
| **TOPRGU** | The watchdog and reset unit. |
| **TouchComm** | The protocol of the first phone's touch controller (TD4160), on SPI; its firmware is uploaded by the loader. |
| **WMT, STP** | CONNSYS's control protocol and the transport its channels share (Wi-Fi, Bluetooth, GPS). |

## Apple

| Word | What it is |
| --- | --- |
| **AIR** | Apple's shader intermediate: LLVM bitcode with its own intrinsics, one module a function inside a `.metallib`. |
| **AKS, key bag** | AppleKeyStore, the service that guards keys; here a kernel stand-in (`aks_keys=1`, never with the 27 release). |
| **arm64e, PAC** | Apple's arm64 with pointer authentication, which this CPU lacks: `darwin/tools/pacstrip` rewrites it into plain instructions. |
| **backboardd** | The window server: CoreAnimation's render server, which draws every frame through Metal. |
| **CoreAnimation** | The compositing framework; its uber shaders (`fixed_frag_lph_cph`) draw nearly everything. |
| **Cryptex** | A separately signed piece of the system (the OS's, the App one), mounted beside the root. |
| **DriverKit, iig** | Apple's user-space drivers and the generator of their interfaces; the kernel's build needs iig's output. |
| **dyld shared cache** | The system's libraries prelinked into one file; rewritten as a whole for PAC. |
| **FairPlay** | Apple's content protection; the kernel has a stand-in client for it. |
| **IOKit, kext** | The kernel's driver framework and its extensions; Apple's closed ones are stood in for in `kernel/overlay/iokit/`. |
| **IOMobileFramebuffer, IOSurface** | The display service the window server swaps frames to; shared image memory between processes and the GPU. |
| **IPSW** | Apple's firmware archive of a release: disk images of the root and the cryptexes, the shared cache. |
| **launchd** | Process 1 of the system (`/sbin/launchd.system` here), started by our `init`. |
| **`.lproj`** | A bundle's per-language resources; a bundle whose development region's directory is missing shows string keys. |
| **Liquid Glass** | The 27 release's glass material: drawn over a blur of what is under it. |
| **Metal, `.metallib`** | Apple's GPU interface, and its shader library file. |
| **RunningBoard** | The process lifecycle service; its watchdog ends extensions that take too long. |
| **SpringBoard, PosterBoard** | The home screen and lock screen; the wallpaper's renderer. |
| **XNU, Darwin** | Apple's open-source kernel, and the operating system around it. |

## Mesa

| Word | What it is |
| --- | --- |
| **Bifrost** | The Mali-G52's architecture (v7), and Mesa's compiler for it. |
| **NIR** | Mesa's shader intermediate; the AIR front end writes it. |
| **panfrost** | Mesa's driver for Mali; this port uses its compiler and descriptors' layouts, and its Gallium driver on the first Metal path. |
