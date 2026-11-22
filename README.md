# HakiOS · MediaTek Helio

English | [Русский](README.ru.md)

[Architecture](docs/architecture.md) · [Porting](docs/porting.md) · [Building](docs/building.md) · [Drivers](docs/drivers.md) · [Devices](https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices) · [Contributing](CONTRIBUTING.md)

HakiOS runs Apple's open-source XNU kernel, with a Darwin user space on top of it, on Android phones
with MediaTek Helio chips. Android stays the phone's system: the loader sits in front of Android's
own kernel, starts XNU once when Android asks for it, and the phone comes back to Android by itself
with the kernel's log.

> Experimental. Needs an unlocked bootloader and root under Android.
> **Two partitions are written**: `boot_a` (the phone's rooted boot image with the loader in front of
> its kernel, `tools/run/xnu-cycle.sh`) and a partition of your own for the system image
> (`tools/run/system-flash.sh`). Slot `b` is never written. Check that the boot ROM's download mode can
> rewrite `boot_a` **before** the first flash: [the way back](docs/porting.md#what-the-phone-must-allow).

This branch is the project itself, for any phone: the kernel, the loader, the tools, the description
of each supported chip. It names no phone. A phone is a **device package**, a small directory kept on
the [`devices` branch](https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices), and one command
brings it into a checkout.

## What is here

- **One kernel for every board.** The board is found at boot; a driver starts only where the board's
  file lists it, so a machine nobody described is never written to.
- **Boards are data.** A phone is one TOML file and its chip another. Addresses, memory ranges, the
  panel, the cell and boot defaults live there, not in the drivers or the loader.
- **Device packages.** Phones are kept apart from the code. The kernel and the loader are built for
  the packages that are present.
- **Checks that need no phone.** The board files are validated, the tools are tested against a
  made-up phone, the loader is compiled for it, and the kernel sources are verified against Apple's
  release, on every push.
- **Pinned inputs.** Everything taken from elsewhere is in [`components.toml`](components.toml), at an
  exact version.

## How it boots

```
boot ROM -> preloader -> LK (the phone's own, untouched)
   LK loads boot_a and starts its "kernel": the loader, then Android's kernel, then the payload
      the loader: Android, unless one XNU boot was asked for (from Android, through pstore's pmsg zone)
         XNU, on LK's framebuffer, with the root read from a partition of the flash
            ... the run ends -> back into the loader -> the log into ramoops -> warm reset -> Android
```

How the pieces fit together: [docs/architecture.md](docs/architecture.md).

## Devices

| Package | Device | SoC | State |
| --- | --- | --- | --- |
| `x6826b` | Infinix Hot 20 | Helio G85 (MT6768) | Boots to the home screen; touch, GPU, Wi-Fi, Bluetooth scan, battery. |

What works on each phone, what was verified on it and when, is in the package's own README on the
[`devices` branch](https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices). SoC support is
shared and lives here, in `soc/`: MT6768 so far.

## Quick start

```sh
git clone https://github.com/oosexgang/xnu-kernel-mtk-helio.git
cd xnu-kernel-mtk-helio
make check                        # everything that needs no phone

tools/device.py available         # the phones there are packages for
tools/device.py add x6826b        # bring one in (devices/x6826b)
tools/boards.py status x6826b     # what it has on, what it can turn on next
```

Then build and run: [docs/building.md](docs/building.md). For a phone that has no package yet:
[docs/porting.md](docs/porting.md).

## Layout

| Path | Contents |
| --- | --- |
| `devices/` | The board template, and the packages you bring in. |
| `soc/` | **One file per chip**: where its blocks are, shared by every board with that chip. |
| `kernel/` | The kernel: Apple's pinned release, the files added to it (`overlay/`), the changes to Apple's files (`patches/`), and the tools that assemble, check and build it. |
| `loader/` | `mtkloader`: what LK starts in place of a kernel. Built for one board. |
| `system/` | Fetching the system image's files from their maker and choosing which go in. |
| `userland/` | Our own user-space sources: `init`, the libraries inserted into a few processes, the Metal device, the audio plug-in, test programs; `build.sh` makes the two images. |
| `workbench/` | Host tests of the tools, with a made-up phone and chip. |
| `tools/` | `boards.py` (board files), `device.py` (packages), the run cycle (`xnu-cycle.sh`, `system-flash.sh`), probes, image and log tools. |
| `components.toml` | Everything taken from elsewhere, pinned to an exact version. |
| `docs/` | Documentation. |

## What is not in this repository

Nothing of Apple's and nothing of a phone's vendor. XNU is fetched at build time from Apple's
open-source release. The system image's files are fetched by you, on your machine, from Apple's own
server (`darwin/system/fetch.sh`); the SDK is yours to install. The phone's firmware (touch controller,
CONNSYS, sensor hub) and its rooted boot image come off your own phone. No picture of a screen is
kept: the documents say in words what the screen showed.
Details: [docs/architecture.md](docs/architecture.md#what-is-not-in-this-repository), [NOTICE.md](NOTICE.md).

## Where it came from

The port grew out of a working tree called darwin-redwood, the first phone's, and out of
[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), branch `mtk`. For people with a
branch from there: [docs/migrating.md](docs/migrating.md) says where everything went, and
[CHANGELOG.md](CHANGELOG.md) what changed. The Qualcomm side of the project is
[xnu-kernel-qcom](https://github.com/oosexgang/xnu-kernel-qcom).

## Licences

See [NOTICE.md](NOTICE.md) and the source headers. Apple, iOS, iPhone and Metal are trademarks of Apple
Inc. This project is not affiliated with or endorsed by Apple, MediaTek or Infinix; their names are used
only to say what the code works with.
