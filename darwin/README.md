# Darwin user space

What the 27 release (27.2, build 24B119) needs to run on a MediaTek phone: small libraries inserted
into a few of its processes, the Metal plug-in and the native Mali library built for its arm64e
processes (`darwin/userland/`), the tools that make Apple's arm64e code runnable and build the two
images (`darwin/tools/`), and the script that drives them all, `mtk27.sh`. Darwin itself is not here:
it is unpacked on your machine from Apple's IPSW. `darwin/system/` does the same for the earlier
system (20F66), whose small root is still where the 27 release's small image starts.

Most of `darwin/userland/` and `darwin/tools/` came from
[xnu-kernel-qcom](https://github.com/oosexgang/xnu-kernel-qcom), whose scripts expect that team's
working tree, darwin-renoir, beside them. `mtk27.sh` makes that tree out of links into this
repository (`build/ios27/darwin-renoir`), so everything here builds from a clean checkout and an
unpacked IPSW. How to build and run: [docs/building.md](../docs/building.md); how it was found:
[docs/darwin27-boot.md](../docs/darwin27-boot.md).

## `mtk27.sh`

| Step | What it builds | Output |
| --- | --- | --- |
| `env` | the darwin-renoir and theos trees of links, `ldid`, `tools/pacstrip` | `build/ios27/` |
| `gl` | Mesa with panfrost for arm64e (`userland/tools/mk-mesa.sh MESA_ARCH=arm64e`), `libhakios-gl.dylib` and `gl_test` over it (`gl26.sh`) | `kernel/deps/build/mesa/out-arm64e/`, `build/ios27/obj/` |
| `libs` | our libraries for the system's processes (`inj26.sh`, at SpringBoard's addresses of the build `BUILD=24B119`) | `build/ios27/obj/` |
| `metal` | the Metal plug-in over that Mesa, and CoreAnimation's AIR shaders out of the release's own `default.metallib` (`metal27.sh`) | `build/ios27/obj/metal/` |
| `mali` | the native Mali library (`libhakiosmali.dylib`), `hakios-mali` and `mali-probe` (`mali27.sh`) | `build/ios27/obj/mali/` |
| `root` | the small image (`root26-sys.sh`) | `$REL/root26-sys.apfs`, `$REL/jobs26` |
| `system` | the system image (`mkimage26.py --prune`) | `$REL/system27-prune.apfs` |
| `all` | `gl`, `libs`, `metal`, `root`, `system` | |

`REL` is the unpacked IPSW (`rootfs/`, `cryptex/os/`, `cryptex/app/`, `dyld/`, `dyld-strip/`), by
default `build/ios27/ios27`. Every step needs `userland/build.sh` run once (its `root/` is the small
image's start), `kernel/deps/setup.sh`, and theos's SDK in `$THEOS`.

## `darwin/userland/`

| File | What it is | Inserted into / used by | Needs |
| --- | --- | --- | --- |
| `inj26.sh` | Builds our libraries for the system's processes as arm64e (`userland/src/inject*.c`, the audio plug-in), puts SpringBoard's addresses of the build in (`BUILD`), runs `pacstrip` and `ldid` over each. | `root26-sys.sh` | `mtk27.sh env`, theos |
| `gl26.sh` | `libhakios-gl.dylib` and `gl_test` over the arm64e Mesa. | the small image | `mtk27.sh gl` |
| `metal27.sh` | The Metal plug-in (`userland/src/hakiosmetal.m`, `userland/src/metal/`), `metal_props` (which names it on `IOAcceleratorES`), `psgrep`, and `QuartzCore-default.metallib`. | `root26-sys.sh` (`METAL27=DIR`) | `mtk27.sh gl`, the IPSW |
| `mali27.sh` | The native library (`userland/src/mali/rwa/`, `kmali.c`), `mali-probe`; Mesa's `genxml` made from its `v7.xml`. | `root26-sys.sh` (`MALI27=DIR`) | `kernel/deps/setup.sh` (the Mesa tree) |
| `root26-sys.sh` | The small image (`/hakios`): our root tree, the libraries, every Mach-O signed ad hoc, `inject.conf`, the job list, the stage cache; and the launchd jobs made from the system's own plists. | `mkpayload.sh` (`HAKIOS_RAMDISK=`), `mkimage26.py` (the jobs) | the steps above, the IPSW |
| `inject_backlight.c`, `inject_brightness.c`, `inject_poster.c`, `inject_statusbar.c`, `console_log.c` | The Qualcomm port's libraries: display mode and idle timer, the brightness slider, the first wallpaper, the status bar's shift, a program's standard error on the console. | SpringBoard, backboardd, PosterBoard, the programs of `CONSOLE_LOG` | `inj26.sh` |

`root26-sys.sh` takes, besides `REL`, `OBJ` and the outputs of the other steps:

| Variable | What it does |
| --- | --- |
| `RC_JOBS=FILE` | the run's job list (a file of `userland/jobs/`, by its path) |
| `RWA_CACHE_BIN=FILE` | the native library's stage cache (`userland/tools/rwacache.sh`) as `/hakios/share/rwa-cache.bin` |
| `JOBCACHE=1` | the launchd jobs also as launchd's job cache in the small image (`etc/xpc/launchd.plist`), which `init` binds over the system's: jobs change without a new system image |
| `ALLOW="WORD ..."` | jobs that `userland/mkjobs.py`'s list of denied words would leave out let through (`wifi WiFi` for Wi-Fi; `audio bluetooth` always) |
| `JOBS="LABEL ..."`, `NO_JOBS="LABEL ..."` | the named jobs replaced, or more of them left out |
| `CONSOLE_LOG="NAME ..."`, `LOGGING_DEBUG="SUBSYSTEM ..."` | those programs' standard error, or those subsystems' every level of `os_log`, on the console |
| `LPROJ="BUNDLE ..."`, `LPROJ=auto` | bundles of the system volume given an empty `en.lproj` through `init`'s overlays (only bundles without code of their own) |
| `PREFS_LOG=1`, `REDUCE_TRANSPARENCY=1` | Settings' Bluetooth lines kept for the host; Reduce Transparency already on |

A line of the small image's `etc/inject.conf` names a library and the executables that get it; the
kernel inserts it when one of them starts (`kern.exec_inject`). A library that replaces methods finds
them by name, checks their types, and leaves alone what is not as it expects.

## `darwin/tools/`

| File | What it does | Needs |
| --- | --- | --- |
| `pacstrip.c` | Rewrites every PAC instruction into its plain equivalent, in a copy of a dyld shared cache or of a Mach-O, and makes the code signature's page hashes again. Built by `mtk27.sh env`. | a C compiler; on a Mac, `macos/` |
| `fips26.py` | Puts the HMAC corecrypto computes over its rewritten code in place of the stored one, so that its self test at boot passes; then `pacstrip resign`. | `pacstrip` built; the rewritten cache |
| `mkimage26.py` | The system image: the chosen files of the IPSW, every arm64e Mach-O through `pacstrip`, the rewritten cache, the launchd jobs and job cache (`LAUNCHDAEMONS=DIR`), `/sbin/launchd` as stage0 and the system's own as `/sbin/launchd.system`. `--prune` leaves out what makes it too big for the partition (4.16 GB of 4.30: the last 96 MiB are the payload's); the empty `.lproj` directories of the kept bundles are made again. | the IPSW with `dyld-strip/`, `pacstrip`, `userland/tools/apfs/` |
| `macos/` | For a Mac: OpenSSL's three digests from CommonCrypto and no `copy_file_range` for `pacstrip.c`, and an `ldid` made of `codesign`. | macOS |

## `darwin/system/`

The earlier system, 20F66, fetched by you from Apple's server. What is fetched stays here, ignored by
git; `userland/build.sh` builds the small root (and the earlier system's own image) from it.

| File | What it does |
| --- | --- |
| `fetch.sh` | Fetches the two disk images out of the IPSW without the rest (`fetch-from-ipsw.py`), unpacks them with 7-Zip, and lays out `rootfs/`, the cryptexes, the shared cache and `var.skel`. `KEEP=1` keeps the disk images. |
| `fetch-from-ipsw.py` | One member of the IPSW, by range requests. |
| `select-system.py` | Which files of the root file system go into an image (also the 27 release's, through `mkimage26.py`). |
| `mkvarskel.py` | The skeleton of `/private/var` that `init` makes at every boot. |

## What depends on the phone

The libraries and the images are the same for every phone. What is not: the system partition's name
and size (the board file's `[loader] system_partition`, and the payload's 96 MiB at its end), and the
identity Darwin is told (`loader/identity.h`, the loader's). The SpringBoard addresses in `inj26.sh`
and the values in `fips26.py` belong to a Darwin build, not to a phone.

## Changes from the Qualcomm port

- `metal27.sh`, `mali27.sh` and `mtk27.sh` are new; `root26-sys.sh` puts their output into the small
  image (`METAL27=`, `MALI27=`, `RWA_CACHE_BIN=`), with an `etc/rc` that names the plug-in on
  `IOAcceleratorES` before launchd starts, and learned `JOBCACHE`, `ALLOW` and `LPROJ`.
- `inj26.sh` also reads `inject_sb.c`'s thread state through arm64e's accessors, and writes the
  boards' user-space table itself (no board here moves its status bar).
- `mkimage26.py --prune` drops six more frameworks' resources, so that the payload fits behind the
  image in the partition.
- Left out, because this port does not use them: the Qualcomm phone's boot tool, its RAM-budget
  scripts, Mesa with freedreno, and the first boots' probe image.
