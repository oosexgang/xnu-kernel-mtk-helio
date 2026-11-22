# The 27 release's user space

The small image and the system image of the 27 release (27.2, 24B119, from the iPhone12,1 IPSW) for the
X6826B. Also the libraries put into the system's processes, all arm64e. How to build and run them is in
[docs/release27.md](../docs/release27.md).

Most of this directory is a directory of the Qualcomm port,
[xnu-kernel-qcom](https://github.com/oosexgang/xnu-kernel-qcom). Its scripts expect that team's working
tree, darwin-renoir, beside them. [`mtk27.sh`](mtk27.sh) makes that tree out of this repository, so
everything here builds from a clean checkout and an extracted IPSW.

## Files

| File | What it is |
| --- | --- |
| `mtk27.sh` | Runs the steps below in order: `env`, `gl`, `libs`, `metal`, `root`, `system`, or `all`. |
| `userland/inj26.sh` | Our libraries for the system's processes, arm64e, at SpringBoard's addresses of the build (`BUILD=24B119`). |
| `userland/gl26.sh` | `libhakios-gl.dylib` and `gl_test` over the arm64e Mesa. |
| `userland/metal27.sh` | The Metal driver for the Mali (`userland/src/hakiosmetal.m`, `userland/src/metal/`), with Mesa's panfrost and QuartzCore's AIR shaders. |
| `userland/root26-sys.sh` | The small image (`/hakios`): our root tree, the libraries, every Mach-O signed ad hoc, `inject.conf`, and the launchd jobs made from the system's own plists. |
| `userland/inject_*.c`, `console_log.c` | The Qualcomm port's libraries: backlight, brightness, the first wallpaper, the status bar, standard error to the console. |
| `tools/mkimage26.py` | The system image: the files chosen, arm64e code made runnable (`pacstrip`), `/sbin/launchd` as stage0 and the system's own as `/sbin/launchd.system`. |
| `tools/pacstrip.c` | Rewrites arm64e code so that it runs on a processor without pointer authentication. Built by `mtk27.sh env`. |
| `tools/fips26.py` | Puts corecrypto's HMAC over its rewritten code in place of the stored one. |
| `tools/macos/` | For a Mac: OpenSSL's three digests from CommonCrypto and no `copy_file_range` for `pacstrip.c`, and an `ldid` made of `codesign`. |

## Changes from the Qualcomm port

- `metal27.sh` is new. `root26-sys.sh` puts its output in the small image (`METAL27=DIR`) and, with it,
  an `etc/rc` that names the plug-in on `IOAcceleratorES` before launchd starts.
- `inj26.sh` also reads `inject_sb.c`'s thread state through arm64e's accessors. It also writes the
  boards' user-space table, because this port's `tools/boards.py` makes none and no board here moves its
  status bar.
- `mkimage26.py --prune` drops six more frameworks' resources (WeatherUI, AvatarKit, PhotoAnalysis,
  TextRecognition, FRC, PassKitUI). That leaves the last 96 MiB of the X6826B's `xnusys` free for the
  payload: 4.16 GB of 4.30.
- The system's launchd is `/sbin/launchd.system`, the name the 16.5 image uses. This port's `etc/rc`
  already runs that name.
- Paths point into this repository, and `REL` names the extracted IPSW.
- Left out, because this phone does not use them: the Qualcomm phone's boot tool, its RAM-budget scripts,
  Mesa with freedreno, and the first boots' probe image.
