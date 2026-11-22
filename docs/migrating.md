# Moving work from the old layout

For people with a branch, a patch or a habit from branch `x6826b` of the working tree, or from branch
`mtk` of hakios-xnu-kernel, which had the same tree. This repository has a history of its own: nothing
is rebased onto it, work is carried over.

## Where things went

<!-- paths of the old layout -->

| Before | Now |
| --- | --- |
| `xnu/<old>.patch`, `xnu/apply.sh`, `xnu/export.sh` | `kernel/patches/` (seven patches by topic, `patches/series`), `kernel/assemble.sh`, `kernel/export.sh`, `kernel/verify.sh` |
| `xnu/files/` | `kernel/overlay/` |
| `xnu-src/xnu-8796.121.2` (the tree, inside the repository's directory) | `../xnu-8796-hakios`, next to the repository, made by `kernel/assemble.sh` (or `$XNU_SRC`) |
| `xnu-build.sh`, `xnu-env.sh` | `kernel/build.sh`, `kernel/env.sh` |
| `xnu-deps/` | `kernel/deps/` |
| `xnu/README.md` | `kernel/README.md` |
| `mtkloader/` | `loader/`; its output in `build/loader/` |
| `mtkloader/touchfw.h`, `mtkloader/font8x16.h` | Not in the repository. `loader/build.sh` makes them in `build/loader/` from your phone's firmware file (`fw-touch/hdl_firmware.img`) and, on macOS, from a system font |
| `mtkloader/camjn1.h`, `camrx.h` | `build/loader/camjn1.h`, `camrx.h` (`tools/camtable.py`, `tools/camrx.py`) |
| `tools/chainboot.py`, `xnuloader/mkpayload.py`, `xnuloader/identity.h`, `mkidentity.py` | `loader/chainboot.py`, `loader/mkpayload.py`, `loader/identity.h`, `loader/mkidentity.py` |
| `XL_RAMDISK=userland/root.apfs ./xl_x6826b build` | `tools/run/mkpayload.sh` (the payload is the kernel and the small root; the UEFI loader is no longer packed into it) |
| `build-payload.bin`, `xnu-flashed-slot_a.img` | `build/payload.bin`, `build/boot_a.img` |
| `xnu-deps/host/zig` | `build/zig` (`tools/get-zig.sh`) |
| `ios165/` | `system/` (the scripts and, ignored by git, what they fetch) |
| `userland/rc.ios`, `rc.jobs.ios`, `/sbin/launchd.ios` in the image | `userland/rc.system`, `rc.jobs.system`, `/sbin/launchd.system` |
| `docs/mtk-porting.md`, `docs/from-a-clone.md` | `docs/porting.md`, `docs/building.md` |
| `docs/bluetooth-mtk-porting.md`, `docs/camera-mtk-porting.md` | `docs/bluetooth-porting.md`, `docs/camera-porting.md` |
| `docs/*x6826b*.md`, `docs/touch-handover.md` (the phone's journals) | The package `x6826b` on the `devices` branch: `x6826b/journal/` |
| Constants in `mtkloader/*.h` and in the drivers (`LK_FB_BASE`, `RAMOOPS_*`, `MARKER_PA`, `core_order`, `SYSTEM_PART`, every `*_BASE` and `*_PHYS`) | `soc/mt6768.toml` and the package's `board.toml`; `tools/boards.py gen` makes `hakios_platform.h` for the loader and `hakios_board_table.h` for the kernel |
| `runkit/` (the author's local kit) | Still local and not here; a plain run's defaults are the board file's `[boot]` |

<!-- end -->

## What was not carried over

| What | Why | Where it still is |
| --- | --- | --- |
| The Qualcomm phone's drivers: Adreno and `<Old>GPU`, the audio DSP, QMI and WLFW, the WCN6750 and ath11k's layer (`wifi/lx`), the amplifiers, microphones, flashlight, haptic motor, Goodix touch, PMIC keys, the UEFI battery, EPSS, the cameras, UFS | Another chip family | [xnu-kernel-qcom](https://github.com/oosexgang/xnu-kernel-qcom) |
| `uefi/`, `xnuloader/loader.c`, `prebuilt/` | MediaTek phones start the loader from LK, not from a UEFI; no binaries are kept | xnu-kernel-qcom, hakios-uefi |
| `qemu/`, `xl`, `mkxnuimg.py` | The emulator's path goes through the UEFI loader | hakios-xnu-kernel |
| `xl_x6826b boot`, `mkxnuimg_x6826b.py` (a one-off image for `fastboot boot`) | Needed the vendor's device tree as a file in the repository | branch `mtk` |
| `mtkloader/diag.c`, `loop.S` | The first experiments; `fbdiag.c` is what a port runs | branch `mtk` |
| `tools/`: `mk-ath11k.sh`, `mk-wlfw.sh`, `qmi-test/`, `wpa-test/`, `ufs-*.py`, `darwin-write.py`, `flash-fingerprint.sh`, `buttons-*.sh`, `camsensor.py`, `new-workspace.sh`, `kernel-delta.sh`, `update-prebuilt.sh` | Tools of the Qualcomm phone or of the old layout | branch `mtk` |
| Pictures in `docs/` | They show another system's interface | nowhere in the new repositories |

Comments in the kernel, the loader and the tools still name files of the list above in about 130 places
(`hakios_adreno.c`, `hakios_ufs.c`, `xnuloader`, `HakiosGPU`): they are where a design was taken
from, and those files are in xnu-kernel-qcom under their names there, some still with the old prefix.

`userland/src/var_store.c` came over but has nothing to write to: it kept `/private/var` in a
partition through the Qualcomm phone's storage driver. On a MediaTek phone the kernel's eMMC driver
only reads. `userland/build.sh` still has the branches that packed the Qualcomm phone's firmware
(`fw-gpu`, `fw-audio`, `fw-haptic`, `fw-camera`, `ADSP=1`, `FLASHBOOT=1`); they do nothing where those
directories are absent.

## Names

The working name of the first phone is gone from code, paths, interfaces and documents. Below it is
written `<old>`, in the case the name had: `<old>` in lower case, `<Old>` capitalised, `<OLD>` in upper
case. Where it was a prefix, the name now has `hakios`, or `mtk` where the code is MediaTek's. Each pair
below changed on both of its sides at once; a program or an image built before the change does not work
with a kernel built after it.

| Was | Is |
| --- | --- |
| `/dev/<old>-touch`, `-audio`, `-camera`, … | `/dev/hakios-touch`, `-audio`, `-camera`, … |
| `kern.<old>.*`, `kern.<old>_switch_root` | `kern.hakios.*`, `kern.hakios_switch_root` |
| IOKit classes `<Old>Mali`, `<Old>MaliUserClient`, `<Old>Battery`, `<Old>WLAN`, `<Old>Bluetooth` | `HakiosMali`, `HakiosMaliUserClient`, `HakiosBattery`, `HakiosWLAN`, `HakiosBluetooth` |
| `<Old>MetalDevice` and the other `<Old>Metal*` classes, `<Old>Metal.bundle`, `<Old>Audio.driver` | `HakiosMetal*`, `HakiosMetal.bundle`, `HakiosAudio.driver` |
| `/<old>/bin`, `/<old>/lib`, `/usr/lib/<old>` in the images; the volume `<old>` | `/hakios/bin`, `/hakios/lib`, `/usr/lib/hakios`; the volume `hakios` |
| `lib<old>.dylib`, `lib<old>-sb`, `-media`, `-gl`, `-app`, `-tweaks`, `lib<old>air` | `libhakios.dylib`, `libhakios-sb`, … `libhakiosair` |
| launchd jobs `com.<old>.demo`, `.varstore`, `.wifi`, `.nullcomm` | `com.hakios.demo`, … |
| The request in pmsg: `DARWIN-<OLD>:BOOT-XNU`, `:GPU-PROBE`, `:CAM-PROBE` | `HAKIOS:BOOT-XNU`, `HAKIOS:GPU-PROBE`, `HAKIOS:CAM-PROBE` |
| `<OLD>_*` macros and header guards, `<old>_*()` functions | `HAKIOS_*`, `hakios_*()`; `MTK_*`, `mtk_*()` where the code is MediaTek's |
| make variable `<OLD>_OBJECTS` | `HAKIOS_OBJECTS` |
| `userland/src/<old>metal.m`, `<old>_*.h`, `userland/tools/mesa/<old>-drm/`, `panfrost-<old>-perf.patch` | `hakiosmetal.m`, `hakios_*.h`, `userland/tools/mesa/hakios-drm/`, `panfrost-hakios-perf.patch` |

Kernel files (`kernel/overlay/bsd/dev/`, headers in `bsd/sys/`):

| Was `<old>_…` | Is |
| --- | --- |
| `mtkcam`, `mtkcam_skel` | `mtk_camera`, `mtk_camera_skel` |
| `mtkkeys`, `mtkbatt`, `mtkwifi`, `mtkbt_skel` | `mtk_keys`, `mtk_battery`, `mtk_wifi`, `mtk_bt_skel` |
| `connsys`, `mali`, `emmc`, `btsk`, `scp`, `tcm` | `mtk_connsys`, `mtk_mali`, `mtk_emmc`, `mtk_btsk`, `mtk_scp`, `mtk_tcm` |
| everything else (`input`, `net`, `audio`, `battery`, `sta`, `wpa`, `apple80211`, `image`, `sparse`, `guard`, `fbgrab`, `ustack`, `apfs_check`, `virtio_*`, `wifi_given`) | `hakios_…` with the same ending |

What did not change, because something not ours looks for it: the names of Apple's services the
kernel stands in for (`AppleKeyStore`, `IOMobileFramebuffer`, `IOSurface`, …), `MACHINE=BCM2837` and
`mach.development.bcm2837` (an XNU configuration's name), entitlement keys. Boot arguments kept their
names, including the short `rwm_*` of the Metal device.

## Carrying a patch over

A patch against the old kernel tree applies to an assembled tree once its names are the new ones:
replace the names by the tables above (in the patch's text and its file names), then

```sh
kernel/import-patch.sh my-change.patch
git status                                          # new files under kernel/overlay, changed topic patches
```

## What behaves differently

- **A MediaTek driver needs its feature in the board file.** `touchscreen=2`, `buttons=2`, `mali=1`,
  `connsys_step=`, `scp_step=`, `mtkcam=1`, `mtkbatt=1`, `emmc_root=1` and `fb_flip=1` now also need
  the matching feature in `features`. The package `x6826b` lists all of them. Asked for on a board
  that does not, a driver prints which line is missing and touches nothing.
- **A kernel knows the boards it was built with.** Bring the package in first
  (`tools/device.py add x6826b`); `hw.board.*` says what the running kernel found.
- **The loader is built for a named board** (`HAKIOS_BOARD=NAME`, or the one phone whose package is
  here) and its output is in `build/loader/`.
- **The run cycle checks the phone's identity** (`[boot] android_device`) besides the serial number,
  the slot and the payload, before it writes `boot_a`.
- **An image and a kernel must be of the same side of the rename**: `tools/run/system-flash.sh` again
  after moving to this repository.
