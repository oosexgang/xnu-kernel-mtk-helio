<p align="center">
  <img src="Documentation/assets/banner.svg" alt="hakios" width="830">
</p>

<p align="center">
  Apple's open-source XNU kernel and the Darwin user space, on phones with MediaTek Helio chips.
</p>

<p align="center">
  <a href="Documentation/architecture.md">Architecture</a> · <a href="Documentation/porting.md">Porting</a> · <a href="Documentation/building.md">Building</a> · <a href="Documentation/drivers.md">Drivers</a> · <a href="https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices">Devices</a> · <a href="CONTRIBUTING.md">Contributing</a>
</p>

<p align="center">
  <a href="https://github.com/oosexgang/xnu-kernel-mtk-helio/actions/workflows/check.yml"><img alt="checks" src="https://github.com/oosexgang/xnu-kernel-mtk-helio/actions/workflows/check.yml/badge.svg"></a>
  <a href="Documentation/README.md"><img alt="documentation" src="https://img.shields.io/badge/docs-read-5b6143"></a>
  <a href="https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices"><img alt="device packages" src="https://img.shields.io/badge/devices-branch-5b6143"></a>
  <a href="https://t.me/oosexgangdevs"><img alt="Telegram" src="https://img.shields.io/badge/Telegram-oosexgangdevs-e0a23a?logo=telegram&logoColor=white"></a>
  <a href="kernel/UPSTREAM"><img alt="XNU release" src="https://img.shields.io/badge/XNU-12377.121.6-3a402e"></a>
</p>

<p align="center">
  English | <a href="README.ru.md">Русский</a>
</p>

# HakiOS · MediaTek Helio

HakiOS runs Apple's open-source XNU kernel and a Darwin user space on Android phones with MediaTek
Helio chips. Android stays the phone's system: the loader sits in front of Android's own kernel,
starts XNU once when Android asks for it, and the phone comes back to Android by itself, with the
kernel's log.

> Experimental. Needs an unlocked bootloader and root under Android.
> **Two partitions are written**: `boot_a` (your own rooted boot image with the loader in front of
> its kernel, every run) and a partition of your own for the system image (once an image). Slot `b`
> is never written. Check that the boot ROM's download mode can rewrite `boot_a` **before** the first
> flash: [the way back](Documentation/porting.md#what-the-phone-must-allow).

This branch is the project itself, for any phone: the kernel, the loader, the user space, the tools,
the description of each supported chip. It names no phone. A phone is a **device package**, a small
directory kept on the [`devices` branch](https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices),
and one command brings it into a checkout.

## What is here

- **One kernel for every board.** The board is found at boot; a driver starts only where the board's
  file lists it, and only with its boot argument, so a machine nobody described is never written to.
- **Boards are data.** A phone is one TOML file and its chip another. Addresses, memory ranges, the
  panel, the cell and the boot defaults live there, not in the drivers or the loader. A tool starts
  the phone's file from its Android device tree and says what can be turned on next.
- **Darwin 27.** The 27.2 release's own user space, as its maker ships it but for pointer
  authentication, which this CPU does not have and which is rewritten out of the code. It is built
  from your own IPSW on your machine (`darwin/`).
- **Metal on the Mali-G52.** A native Metal library over the kernel's Mali driver: CoreAnimation's
  shaders go from Apple's AIR to NIR and through Mesa's Bifrost compiler, and the stages the window
  server needs are compiled on the Mac beforehand ([Documentation/metal-glass.md](Documentation/metal-glass.md)).
- **Checks that need no phone.** The board files are validated, the drivers run on the host against
  models of the hardware, the loader is compiled for a made-up phone, the generated references are
  kept current, and the kernel sources are verified against Apple's release, on every push.
- **Pinned inputs.** Everything taken from elsewhere is in [`components.toml`](components.toml), at
  an exact version.

## How it boots

<p align="center">
  <img src="Documentation/assets/boot-chain.svg" alt="The boot chain: boot ROM, preloader and LK, boot_a, the loader, the kernel, Darwin user space" width="830">
</p>

How the pieces fit together: [Documentation/architecture.md](Documentation/architecture.md).

## Devices

The phones are device packages, kept apart from the code on the
[`devices` branch](https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices): its README lists
them with how far each is, and each package's own README and journal say what works on that phone,
what was verified on it and when, and what is still off. Chip support is shared and lives here, one
file a chip in [`arch/arm64/boot/dts/mediatek/`](arch/arm64/boot/dts/mediatek): MT6768 (Helio G85) so far.

## Quick start

```sh
git clone https://github.com/oosexgang/xnu-kernel-mtk-helio.git
cd xnu-kernel-mtk-helio
make check                        # everything that needs no phone

tools/device.py available         # the phones there are packages for
tools/device.py add x6826b        # bring one in (arch/arm64/boot/dts/x6826b)
tools/boards.py status x6826b     # what it has on, what it can turn on next
```

Then build and run: [Documentation/building.md](Documentation/building.md). For a phone that has no package yet:
[Documentation/porting.md](Documentation/porting.md).

## Start here

| You want to | Read |
| --- | --- |
| Understand how it fits together | [Documentation/architecture.md](Documentation/architecture.md) |
| Port it to your phone | [Documentation/porting.md](Documentation/porting.md) |
| Build the kernel, the images and run them | [Documentation/building.md](Documentation/building.md) |
| Go from a Mac to Darwin 27 on a phone (in Russian) | [Documentation/darwin27.ru.md](Documentation/darwin27.ru.md); in a page, [Documentation/quickstart.ru.md](Documentation/quickstart.ru.md) |
| Spend a day with the phone: the runs in order, and what success looks like (in Russian) | [Documentation/phone-test.ru.md](Documentation/phone-test.ru.md) |
| Know where everything is | [Documentation/layout.md](Documentation/layout.md) |
| Know what a driver, a feature or a boot argument does | [Documentation/drivers.md](Documentation/drivers.md), [Documentation/features.md](Documentation/features.md), [Documentation/boot-args.md](Documentation/boot-args.md) |
| Know where Metal on the Mali stands | [Documentation/metal-glass.md](Documentation/metal-glass.md) |
| Read how Darwin 27 came up, run by run | [Documentation/darwin27-boot.md](Documentation/darwin27-boot.md) |
| Write a driver for a part nobody has one for | [Documentation/writing-a-driver.md](Documentation/writing-a-driver.md) |
| Find out what a message in the log means | [Documentation/troubleshooting.md](Documentation/troubleshooting.md) |
| Look up a word | [Documentation/glossary.md](Documentation/glossary.md) |
| Know how boards and packages are described | [arch/arm64/boot/dts/README.md](arch/arm64/boot/dts/README.md) |
| Change something and send it back | [CONTRIBUTING.md](CONTRIBUTING.md) |
| Carry over a branch or a patch from an earlier layout | [Documentation/migrating.md](Documentation/migrating.md) |
| See what changed | [CHANGELOG.md](CHANGELOG.md) |

## Layout

Laid out as Linux's tree is; [Documentation/layout.md](Documentation/layout.md) has every directory, and which
directory of XNU's tree each kernel directory of ours is.

| Path | Contents |
| --- | --- |
| `arch/arm64/kernel/`, `arch/arm64/include/` | The platform: the GICv3, the other cores through PSCI, the return to the loader with the log, the profiler; the machine's header. |
| `arch/arm64/boot/` | `mtkloader`: what LK starts in place of a kernel, built for one board; the payload and boot image tools. |
| `arch/arm64/boot/dts/` | The board template and the packages you bring in (each a board file); `mediatek/`: **one file per chip**, where its blocks are, shared by every board with that chip. |
| `arch/arm64/configs/` | The boot arguments of each kind of run (`*_defconfig`: `make defconfigs`), and the kernel's build configuration (`xnu/`). |
| `drivers/` | The drivers: `board/` (`mtk_*` for MediaTek's blocks, `hakios_*` for what is the same on any phone) and `iokit/` (`HakiosMali`, the stand-ins for Apple's closed kernel extensions). |
| `fs/`, `crypto/`, `security/`, `include/` | APFS and the RAM file system; the kernel's crypto; code-signing policy; the headers user space shares with the kernel. |
| `kernel/` | Apple's pinned release, the changes to Apple's files (`patches/`), and the tools that assemble, check and build the tree. |
| `init/`, `lib/` | The first programs Darwin runs (`stage0`, `init`) and their small C runtime. |
| `darwin/` | What Darwin 27 needs: libraries inserted into a few of its processes, the Metal and Mali builds, the PAC rewriter, the image builders (`mtk27.sh` drives them); `darwin/system/` fetches and chooses the earlier system's files. |
| `userland/` | Our own user-space sources: the inserted libraries, the Metal device and the native Mali library, the audio plug-in, the run's tools and test programs; the job lists (`jobs/`), our launchd jobs (`launchd/`) and the tools of the user space (`tools/`: the shader front end, Mesa's patches, the stage cache, APFS). |
| `workbench/` | Host tests: the tools, and the kernel's drivers against models of the hardware, with a made-up phone and chip. |
| `tools/` | `boards.py` (board files), `device.py` (packages), `defconfig.py` (the runs' arguments), `bootargs.py` (the generated reference), lint and the publish check; `run/` (one run on the phone, the loader's probes), `log/` (what a run left, as pictures, sound and numbers). |
| `Documentation/` | Documentation ([its index](Documentation/README.md)). |
| `MAINTAINERS`, `components.toml` | Each part's files, tests and documents; everything taken from elsewhere, pinned to an exact version. |

## Porting in one paragraph

A phone is described by one board file and its chip by a SoC file. `tools/boards.py new NAME --soc
SOC --dtb fdt.dtb` starts the board file from the phone's Android device tree. The kernel starts a
driver only on a board that lists it, so a new phone begins with nothing but LK's framebuffer and gains
hardware one line at a time; `tools/boards.py status NAME` says what can be turned on next and what
each of the rest still needs ([Documentation/features.md](Documentation/features.md)). Where the blocks of the chip are
comes from the SoC file, not from the drivers' code. Parts nobody has a driver for yet (a touch
controller, a camera sensor) need one written, from an example that is built and tested with the rest
([Documentation/writing-a-driver.md](Documentation/writing-a-driver.md)). When it works, the package goes to the
`devices` branch. The guide: [Documentation/porting.md](Documentation/porting.md).

## What is not in this repository

Nothing of Apple's and nothing of a phone's vendor. XNU is fetched at build time from Apple's
open-source release. The system's files come from Apple's IPSW, on your machine; the SDK is yours to
install. The phone's firmware (touch controller, CONNSYS, sensor hub, camera tables) and its rooted
boot image come off your own phone. No picture of a screen is kept: the documents say in words what
the screen showed. Details: [Documentation/architecture.md](Documentation/architecture.md#what-is-not-in-this-repository),
[NOTICE.md](NOTICE.md).

## Where it came from

The port grew out of the working tree of the project's first phone, and out of
[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), branch `mtk`. For people with a
branch from there: [Documentation/migrating.md](Documentation/migrating.md) says where everything went, and
[CHANGELOG.md](CHANGELOG.md) what changed. The Qualcomm side of the project is
[xnu-kernel-qcom](https://github.com/oosexgang/xnu-kernel-qcom); the scripts of `darwin/` and the
shader front end came from there.

## Community

News and progress: [t.me/oosexgangdevs](https://t.me/oosexgangdevs). Questions, bugs and new phones:
the repository's issues and pull requests; [CONTRIBUTING.md](CONTRIBUTING.md) says where a change goes.

## Licences

See [NOTICE.md](NOTICE.md) and the source headers. Apple's files, SDKs, device firmware and built
images are local build inputs and outputs, never repository content; `tools/check-publishable.sh`
looks for them.

## Acknowledgements

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) and [xnu-kernel-qcom](https://github.com/oosexgang/xnu-kernel-qcom)
- [Mesa](https://mesa3d.org/): panfrost and the Bifrost compiler
- [mtkclient](https://github.com/bkerler/mtkclient): the way back through the boot ROM
- Apple's open-source XNU, and the Linux and Android drivers cited in individual source files

Apple, Darwin and Metal are trademarks of Apple Inc. This project is not affiliated with or endorsed
by Apple, MediaTek or Infinix; their names are used only to say what the code works with.
