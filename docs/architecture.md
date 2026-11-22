# Architecture

How the pieces fit together, and where each one lives in the repository. Read this before changing
anything; [porting.md](porting.md) is the step-by-step guide for a new device.

## The boot chain

```
boot ROM -> preloader -> LK (the phone's own, untouched)
   LK loads boot_a as an Android boot image and starts its "kernel" as a Linux arm64 Image:
   MMU and caches off, x0 = LK's device tree
      the "kernel" is gzip of:  mtkloader | padding | Android's own Image | the payload (XNU, the small root)
         loader (loader/): Android, unless one XNU boot was asked for
           - built for one board: every address is that board's (hakios_platform.h)
           - builds the Apple-format device tree and boot_args, names the board in /chosen/darwin-board
           - gives the kernel LK's framebuffer; uploads the touch controller's firmware on the way
           - a watcher on a spare core takes a snapshot and resets the board if the kernel never returns
              kernel (kernel/: Apple's XNU + overlay + patches)
                - looks its board up in the generated table (hakios_board())
                - starts only the drivers the board lists and the boot-args ask for
                - the root is read from a partition of the eMMC; /private/var lives in RAM
                   user space: launchd -> backboardd -> SpringBoard -> apps
                     - small libraries of ours are inserted into a few processes (userland/src/inject*.c)
         ... the run ends (time, panic, five fingers held) -> back into the loader
             -> the log into Android's ramoops zones -> warm reset -> Android, which shows it in /sys/fs/pstore
```

**The default is Android.** XNU starts only when the loader finds the request `HAKIOS:BOOT-XNU` in
ramoops' pmsg zone, written from Android by `tools/run/boot-xnu.sh`, and takes it away: the boot after
XNU is Android again. A kernel that hangs costs a power cycle.

With `HAKIOS_PAYLOAD=flash` the payload is not in `boot_a` but in the last 96 MiB of the system
partition, and the loader reads it from there ([building.md](building.md)).

What this port does that a `fastboot boot` port does not, and what it costs: it **writes `boot_a`**
(every run: the loader is built with the run's flags and packed into your own rooted boot image) and
**a partition for the system image** (once per image). It needs root under Android for three things:
the request through `/dev/pmsg0`, the log out of `/sys/fs/pstore`, and `dd` into the partition. The
way back from a broken `boot_a` is the boot ROM's download mode (mtkclient); [porting.md](porting.md)
says how to check it first.

## Three layers, three kinds of change

| Layer | Where | Changes when |
| --- | --- | --- |
| **Device data** | A device package per phone (`NAME/board.toml`, kept on the `devices` branch), `soc/SOC.toml` on the main branch | A new phone, or a new SoC. No code. |
| **Drivers for parts** | `kernel/overlay/bsd/dev/`, `kernel/overlay/iokit/Kernel/`, the loader's part headers | A phone has a part nobody wrote a driver for yet (a touch controller, a camera sensor). |
| **The port itself** | `kernel/patches/`, the rest of `kernel/overlay/`, `loader/`, `userland/` | Apple's kernel or the system image changes, or the port learns something new. Same for every device. |

The aim is that a new phone on a known SoC is a change to the first row only, and a new SoC adds one
file to it. Where that is not yet true, [drivers.md](drivers.md) says so driver by driver.

## Device data

```
device packages (NAME/board.toml) ─┐
  from the devices branch, or any  │  tools/boards.py gen      build/boards/hakios_board_table.h      (the kernel's table)
  directory on the search path     ├─────────────────────►    build/boards/NAME/hakios_platform.h    (the loader's, one a board)
soc/mt6768.toml, soc/… ────────────┘  (run by the builds)
```

- The repository describes no phone. `devices/` on the main branch has a template
  (`devices/_template/`).
- A phone is a **device package**: a directory `NAME/` with its `board.toml`, its README and its
  journal. Packages are kept on the `devices` branch. `tools/device.py add x6826b` copies one into
  `devices/NAME/` of a checkout, which git ignores there.
- A **board file** says what the device has: its SoC, the drivers it may start (`features`), the
  memory the loader may use (`[loader]`), Android's ramoops zones (`[ramoops]`), the panel and LK's
  framebuffer (`[display]`), the cell (`[battery]`), CONNSYS's reserved memory (`[connsys]`), its
  default run (`[boot]`).
- A **SoC file** says where the blocks of the chip are: the interrupt controller and the cores
  (`[platform]`), every peripheral block a driver maps (`[blocks]`), the big cluster's PLL
  (`[cpufreq]`).
- The generated files are not committed. `kernel/build.sh` and `loader/build.sh` run
  `tools/boards.py gen` themselves.
- **The kernel** finds its row by `/chosen/darwin-board`. A driver asks
  `hakios_board_wants(HAKIOS_HAS_…)` before it starts and reads addresses from `hakios_board()`.
  A board that is not in the table has no feature: nothing of a phone is touched on a machine nobody
  described.
- **The loader** is not one binary for every board, and cannot be: LK starts it at an address that is
  the phone's, and it travels inside that phone's own boot image. It is compiled for one board, with
  that board's values as constants, and its part drivers (the touch firmware upload, the CONNSYS
  report) are compiled in only for a board that lists the feature.

Details and every key: [devices/README.md](../devices/README.md), `devices/_template/`.

## The kernel

| Path | Contents |
| --- | --- |
| `kernel/UPSTREAM` | The Apple release, pinned by commit. |
| `kernel/overlay/` | The files added, as ordinary sources at their place in the XNU tree. |
| `kernel/patches/` | The changes to Apple's own 77 files, as 7 patches by topic. |
| `kernel/assemble.sh`, `export.sh`, `verify.sh` | Make the tree that is compiled; take work back; check the round trip. |
| `kernel/build.sh`, `env.sh`, `deps/` | The build, on macOS and on Linux. |

What the overlay provides, in the order a boot meets it:

1. **Platform** (`osfmk/arm64/`): GICv3 interrupts, PSCI for the other cores, the return to the
   loader with the log.
2. **Stand-ins for Apple's closed kernel extensions** (`iokit/Kernel/`, `libkern/amfi/`,
   `osfmk/minicrypto/`): the platform expert, IOSurface, IOMobileFramebuffer, AppleKeyStore, the
   network stack's registry side, code-signing policy, crypto.
3. **File systems** (`bsd/miscfs/apfs/`, `ramfs/`): an APFS implementation that reads the system
   image, and the RAM file system for `/private/var`.
4. **Drivers** (`bsd/dev/`): `mtk_*` for MediaTek's blocks (eMMC, the PMIC's keys and gauge, Mali,
   CONNSYS, the sensor hub, the camera path), `hakios_*` for what is the same on any phone (touch
   input, the network interface, the Wi-Fi station's side, audio, the board table). Each does nothing
   until its boot argument asks.

More: [kernel/README.md](../kernel/README.md), [kernel/patches/README.md](../kernel/patches/README.md).

## The loader

`loader/mtkloader.c` is one program: LK's hand-over, the payload ("XNUP": kernel + small root), the
device tree it builds for XNU, the log's way into ramoops, the watcher. `msdc.h` reads the eMMC as LK
left it; `mtkdbg.h` is the screen and ramoops output; `mtkspi.h` and `tcm.h` are the touch
controller's bus and protocol; `mtkpwrap.h`, `mtkmali.h`, `mtkcam.h` and `mtkconnsys.h` are probes
that report and return to Android. It runs with the MMU off, so all memory is device memory: no SIMD,
no unaligned access, no compiler builtins. More: [loader/README.md](../loader/README.md).

## User space

The system image's own programs are used as their maker ships them. What is ours:

| Path | Contents |
| --- | --- |
| `userland/src/init.c`, `stage0.c` | Process 1 before launchd: mounts, checks, the list of libraries to insert. |
| `userland/src/inject*.c` | Libraries inserted into single processes by the kernel (`kern.exec_inject`). |
| `userland/src/metal/`, `hakiosmetal.m` | The Metal device over the Mali, with Mesa's panfrost underneath. |
| `userland/src/audio_plugin.c` | The audio output device. |
| `userland/build.sh` | Builds all of it and makes `root.apfs` (travels with the kernel) and `system.apfs` (the root, in its own partition). |
| `system/` | `fetch.sh` gets the system image's files from their maker; `select-system.py` chooses which go into the image. |

## What is not in this repository

Being honest about this is part of the architecture: these are the inputs a build or a boot needs
from elsewhere. Everything that is fetched is pinned, listed in one place: `components.toml`
(`tools/components.py` prints it, and the lint checks it against the scripts that fetch).

| What | Why not here | Where it comes from |
| --- | --- | --- |
| Apple's XNU release | It is Apple's; only the pin is kept. | `kernel/assemble.sh` fetches it. |
| The system image's files | Apple's files are never redistributed. | `darwin/system/fetch.sh`, on your machine, from Apple's server. |
| The SDK | Apple's. | `iPhoneOS16.5.sdk` of github.com/theos/sdks, into `$THEOS/sdks`. |
| The phone's firmware (touch, CONNSYS, sensor hub) and camera tables | The vendor's; taken from the same phone it runs on. | `adb pull` from its Android; the package's journal says which files. |
| The rooted boot image | Your phone's own. | `backup/boot_a_magisk.img`. |
| The loader's font | A rendering of a system font. | `loader/build.sh` makes it on macOS; elsewhere the loader's text on the panel is blank. |

## Not carried over from the working tree

The UEFI loader and the Mu-Silicium port, the QEMU scripts, the first `fastboot boot` image scripts
and everything of the Qualcomm phone: [migrating.md](migrating.md) lists them.
