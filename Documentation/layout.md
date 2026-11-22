# Where everything is

The tree is laid out as Linux's (and Android's kernel) tree is. The kernel's files of ours are kept in the
directories below, and `kernel/lib.sh` (`OVERLAY_PLACES`) says which directory of the kernel's tree each is:
`kernel/assemble.sh` puts them there, `kernel/export.sh` takes a new file of the tree back to its place, and
`overlay_view` of the same file lays them out at their paths in the tree for the host's tests (`workbench/run.sh`).
A file keeps its name and its contents wherever it is kept: the tree assembled from this layout is the one the
earlier layout assembled, to its git hash.

| Directory | In the kernel's tree | What |
| --- | --- | --- |
| `arch/arm64/boot/` | | The loader: `mtkloader.c`, what LK starts in place of Android's kernel, built for one board; the tools that pack the payload (`mkpayload.py`) and the boot image (`chainboot.py`, `build_flashed.py`); `fbdiag.c`, what a port runs first. [README](../arch/arm64/boot/README.md). |
| `arch/arm64/boot/dts/` | | The boards: the template (`_template/`) and a phone's package once it is brought in from the `devices` branch (`tools/device.py add NAME`: `NAME/board.toml`, which git ignores here); `mediatek/`, one file a chip (`mt6768.toml`): where its blocks are. `devices/` and `soc/` at the top, where they were, are still looked in. [README](../arch/arm64/boot/dts/README.md). |
| `arch/arm64/kernel/` | `osfmk/arm64/` | The GICv3, the other processors through PSCI, idle, the arm64e instructions this core lacks, the way back to the loader with the log, the profiler. |
| `arch/arm64/include/pexpert/` | `pexpert/pexpert/arm64/` | The machine's header (`BCM2837.h`: the XNU configuration this port builds is that one's). |
| `arch/arm64/configs/` | `xnu/`: `config/` | `*_defconfig`: the boot arguments of each kind of run (`tools/defconfig.py --list`; `XNU_DEFCONFIG=NAME` of `tools/run/xnu-cycle.sh`). `xnu/`: the kernel's build configuration. |
| `drivers/board/`, `drivers/iokit/` | `bsd/dev/`, `iokit/Kernel/` | The drivers: `mtk_*` for MediaTek's blocks, `hakios_*` for what is the same on any phone, the board table ([drivers.md](drivers.md)); `HakiosMali` and the stand-ins for Apple's closed kernel extensions. |
| `include/board/`, `include/external/` | `bsd/sys/`, `EXTERNAL_HEADERS/` | The headers user space shares with the kernel (`hakios_input.h`, `hakios_audio.h`, ...); the headers of Apple's closed parts. |
| `fs/apfs/`, `fs/ramfs/` | `bsd/miscfs/apfs/`, `ramfs/` | Our APFS, which reads the system image, and the file system in memory that holds `/private/var`. |
| `crypto/` | `osfmk/minicrypto/` | The kernel's cryptography provider in place of corecrypto. |
| `security/amfi/` | `libkern/amfi/` | The stand-in for AMFI: entitlements and code-signing policy. |
| `kernel/overlay/` | the same paths | The two files that belong to none of those. |
| `init/` | | The first programs Darwin runs: `stage0` (the system image's `/sbin/launchd`, which mounts the small image on `/hakios`), `init` (process 1 until the system's launchd), `child`, `var_store`. `userland/build.sh` compiles them. |
| `lib/libmini/` | | The small C runtime they are linked with, for programs that run without the system's libraries. |
| `kernel/` | | Apple's release as it is pinned (`UPSTREAM`), the topic patches to its files (`patches/`), the two files of ours that belong nowhere else (`overlay/`), and the tools that assemble, export, check and build the tree (`assemble.sh`, `export.sh`, `verify.sh`, `syntax-check.sh`, `build.sh`). [kernel/README.md](../kernel/README.md). |
| `darwin/` | | What the 27 release needs: the libraries inserted into a few of its processes, the Metal and Mali builds, the PAC rewriter, the image builders (`mtk27.sh` drives them); `darwin/system/` fetches and chooses the earlier system's files. [darwin/README.md](../darwin/README.md). |
| `userland/` | | Our user-space sources: the inserted libraries, the Metal device and the native Mali library, the audio plug-in, the run's tools and test programs; the job lists (`jobs/`), our launchd jobs (`launchd/`), the tools of the user space (`tools/`: the shader front end, Mesa's patches, the stage cache, APFS). [userland/README.md](../userland/README.md). |
| `tools/` | | What the builds and the checks run, and what a person asks a question of: boards, packages, defconfigs, the generated references; `run/` (one run on the phone), `log/` (what a run left). [tools/README.md](../tools/README.md). |
| `workbench/` | | The tests that need no phone: the tools, and the kernel's drivers against models of their hardware, with a made-up phone and chip. [workbench/README.md](../workbench/README.md). |
| `Documentation/`, `LICENSES/` | | Everything else to read ([README.md](README.md) lists it); the licences ([NOTICE.md](../NOTICE.md) says which file has which). |

At the top, beside them: `Makefile` (`make` lists what it does), `MAINTAINERS` (each part's files, its tests and
its documents), `CONTRIBUTING.md`, `SECURITY.md`, `NOTICE.md`, `CHANGELOG.md`, `components.toml` (everything taken
from elsewhere, pinned), `.clang-format` and `.editorconfig` (the style, for an editor), `.github/` (the check every
push runs, and the issue forms).

Made here and never committed (`.gitignore`): `build/` (the boards' tables, the loader, the payload, the boot image,
the images of the 27 release), `logs/` (what the runs left), `backup/` (your phone's rooted boot image), `fw-*/` (the
phone's own firmware, and `fw-wifi/network`: the network to join), and a phone's package in
`arch/arm64/boot/dts/NAME/` once it is brought in.

## Why this way

- **A file is where a reader of Linux looks for it.** A driver is in `drivers/`, a file system in `fs/`, what is the
  machine's in `arch/arm64/`, a board in `arch/arm64/boot/dts/`. The directory of XNU's tree a file goes to is said
  once, in `OVERLAY_PLACES`, and nowhere else: scripts and tests ask `kernel/lib.sh` (`overlay_files`,
  `overlay_view`, `place_of`), Python reads the same list (`tools/bootargs.py`).
- **Paths in the tree do not change.** Boot arguments, the generated reference ([boot-args.md](boot-args.md)), the
  kernel's build lists (`conf/files` in `kernel/patches/`) and the syntax check name a file by its path in the tree
  (`bsd/dev/mtk_keys.c`), which is what the compiler and a panic's backtrace say.
- **Boards stay data.** A board is a TOML file, not a device tree source: the loader is compiled with its values
  and the kernel's table is generated from it (`tools/boards.py gen`); it lies where Linux keeps a board's `.dts`
  only so that it is found there.
- **A run is a defconfig.** The boot arguments of a kind of run are a file of `arch/arm64/configs/`, one argument a
  line with what it does, not a line in a script or a document ([building.md](building.md)).
