# From a clone to the home screen

Every command between `git clone` and the system's home screen on a phone whose package is on the `devices` branch (the first one: `x6826b`), in order, with what each one needs and how long it took.
It was done so on 11 and 12 November 2026: a new clone of this repository in an empty directory, on an Intel Mac with macOS 14.0, Apple clang 15.0.0,
Command Line Tools 15.3, no Homebrew. Whatever stopped that clone is fixed in the scripts or said here. On Linux the steps are the same and the
packages are those of [kernel/README.md](../kernel/README.md); that side was not run again for this page.

For a phone that is not the first phone, read [porting.md](porting.md) first: this page is the last step of that one.

## What you need before the first command

| | |
| --- | --- |
| The phone | bootloader unlocked, Android rooted (Magisk), on slot `a`, USB debugging allowed for this computer |
| A partition `xnusys` | 4 GB, for the system image. The first phone's was cut out of `userdata` once (`tools/gpt-carve.py`, step 8 of porting.md). Without it there is no root to boot |
| `backup/boot_a_magisk.img` | **your own** rooted boot image, the one that is in `boot_a` when Android boots. The loader is packed into it. Never in the repository |
| `adb`, `fastboot` in `PATH` | Android's platform-tools |
| A compiler | macOS: the Command Line Tools. Linux: see kernel/README.md |
| The system image 20F66 SDK | `~/theos/sdks/iPhoneOS16.5.sdk` (or `$THEOS/sdks`), from github.com/theos/sdks; the command is in kernel/README.md |
| 7-Zip 23 or newer | as `7z` or `7zz` in `PATH`. It reads APFS disk images. 7-zip.org's own macOS build is `7zz`; of its two archives `-mac` is for Apple silicon and `-mac-x64` for Intel |
| `python3` | 3.9 was enough; no modules beyond the standard ones |
| Disk | 20 GB: 1 GB kernel sources and build, 8 GB while the system is fetched (5.6 GB after), 2.3 GB each for the image and the tree it is made of |
| Network | 7.4 GB from Apple's CDN (the two disk images of the reference phone's the system image 20F66), 47 MB of zig, the sources `kernel/assemble.sh ../xnu-8796-hakios` and `kernel/deps/setup.sh` clone |

The path of the clone must have no spaces in it.

## The commands

```sh
export HAKIOS_SERIAL=<your phone's adb serial>     # only ever in the environment, never in a file

make check                               # 16 s    everything that needs no phone
tools/device.py add x6826b               #         the phone's package: what the kernel's table and the loader are built from
kernel/assemble.sh ../xnu-8796-hakios    #         Apple's xnu-8796.121.2 + kernel/overlay + kernel/patches, next to the clone
kernel/deps/setup.sh                     # 40 s    what the kernel's build needs: sources, host tools, the SDK's overlay, two libraries
kernel/build.sh -j8 -k                   # 2 min 48 s the kernel. -k is needed: one harmless error (lldbmacros) ends make with code 2
tools/get-zig.sh                         #         zig 0.13.0 into build/zig: its ld.lld links the loader (not needed where ld.lld is installed)
darwin/system/fetch.sh                          # ~30 min at 4 MB/s: the files of the system image (20F66) from Apple's server; a broken download is taken up again
userland/build.sh                        # 4 min   our programs and libraries, userland/root.apfs (4 MB) and userland/system.apfs (2.4 GB)
tools/run/mkpayload.sh                       # 1 s     build/payload.bin = the kernel + the small root
mkdir -p backup fw-touch && cp <your rooted boot image> backup/boot_a_magisk.img && cp <the touch firmware> fw-touch/hdl_firmware.img
tools/run/system-flash.sh                    # ~2 min  system.apfs into xnusys (Android must be up; 2.4 GB free on /data for the way there)
XNU_LOG_OUT=logs/run1.log tools/run/xnu-cycle.sh   # 5 min   the board file's [boot]: 120 s, cpus=4 touchscreen=2 mali=1 fbscale=2 fbfit=3
```

The times are those of a clone of this repository built on 12 November 2026 on the Intel Mac named above;
where none is given, the step was run but its time was not kept.

The last command builds the loader, packs it with Android's kernel and the payload into `build/boot_a.img` (30 MB; `boot_a` holds 32),
flashes `boot_a`, lets Android come up, asks it for one XNU boot, and reads the log when Android is back. The phone shows the home screen about
100 seconds after the kernel starts and goes back to Android by itself `XNU_SECS` after. Only `boot_a` and `xnusys` are written; slot `b` never.

## What a good run looks like

In `logs/run1.log`, in this order:

- `boot-args: debug=0x8 serial=3 rd=md0 emmc_root=1 cpus=4 touchscreen=2 mali=1 ...`
- `touch: TD4160-... is up` (the loader gave the touch controller its firmware)
- `Darwin Kernel Version 22.5.0: <the time you built it>`
- `mali: came up to step 6 of 6`
- `load_init_program: attempting to load /sbin/launchd`, then `=== 36 checks, 0 failed ===` from our init
- every five seconds, once SpringBoard draws: `MiniIOMobileFramebuffer: 51 frames in 5012 ms (10 a second)` and
  `this frame on average red 118 green 151 blue 148; 100% of it not black`
- `=== end of XNU log ===`, `starting Android`

Ten frames a second is the CPU drawing: this build has no Mesa in it. The GPU path is `GL=1` (and `METAL=1`) of `userland/build.sh`, which needs
`userland/tools/mk-mesa.sh` and for that meson and ninja; it was not part of this test.

To see the screen without looking at the phone, add `fbshot=20` to `XNU_ARGS` and give the run ten seconds more (`XNU_SECS=130`): the kernel
prints a small picture of the frame into the log and `python3 tools/log/fbshot.py logs/run1.log shot.png` makes a PNG of it. `fbshot=N` counts the
five-second frame reports, N/5 of them, and those start only when SpringBoard draws (about 85 s in): `fbshot=20` is a picture at about 125 s.
The log keeps its last 256 KB, so a picture wider than the default 60 pixels, or one taken long before the end, falls out of it.

## What is not in a run made this way

- No Wi-Fi, Bluetooth, sensors, camera or sound: their firmware is your phone's own and is not fetched (`build.sh: fw/: 0 files ... absent`),
  and each wants its boot-args and jobs; see that subject's journal in [README.md](README.md).
- The five test programs that want an entitlement are unsigned without `ldid` (`build.sh: no ldid: ...`). `LDID=$PWD/userland/tools/ldid-codesign.sh ./userland/build.sh` signs
  them with macOS's `codesign`.
- `lsd` fills the log with `denied attempt to escape CPU monitor`, twenty lines a second. It is harmless and it is why the log's beginning is lost.

## The payload in the system partition instead of boot_a

`boot_a` holds Android's own kernel and ramdisk besides ours; on the first phone it is 32 MB, and with
the kernel and the small root in it 2 MB were left. With `HAKIOS_PAYLOAD=flash` the boot image carries
only the loader in front of Android (23.9 MB on the first phone), and the payload goes into the last
96 MiB of the system partition, written and read back by `tools/run/payload-flash.sh` (the run cycle calls
it). The loader reads it from there (1.6 s for 19 MB), checks its sum and starts it; a missing or
damaged one sends the phone to Android, as a boot image without a payload always did.

```sh
HAKIOS_PAYLOAD=flash XNU_LOG_OUT=logs/run1.log tools/run/xnu-cycle.sh
```

`tools/run/system-flash.sh` refuses a system image that would reach those 96 MiB. A loader built before
this change cannot read the tail: the first such run needs the new loader in `boot_a`, which every run
of `xnu-cycle.sh` writes anyway.

## Wi-Fi, Bluetooth and Metal

Each needs more in the images than the plain boot, and the phone's CONNSYS firmware in
`kernel/deps/build/connsys-fw/firmware` (the files are listed in the package's Wi-Fi journal; never in the
repository). `tools/run/image.sh` makes both images and the payload; the system image changes, so write it
before the run.

```sh
tools/run/image.sh wifi && tools/run/system-flash.sh       # and fw-wifi/network: your network's name, then its passphrase
XNU_ARGS="cpus=4 touchscreen=2 buttons=2 mali=1 fbscale=2 fbfit=3 scp_step=7 connsys_step=17 connsys_wake=0 mtkbatt=1" \
  XNU_SECS=210 tools/run/xnu-cycle.sh

tools/run/image.sh bluetooth && tools/run/system-flash.sh  # the same arguments and connsys_bt=2 aks_keys=1, XNU_SECS=300
QC_METALLIB=<QuartzCore's shaders> tools/run/image.sh metal && tools/run/system-flash.sh   # and metal=1 fb_queue=1 fb_flip=1
```

What was seen on 12 November 2026 from this repository: Wi-Fi joined the network given (WPA2, its keys
in) and Settings showed it ticked; the status bar had the battery's percentage; `bt_test` saw 19 BLE
devices through CoreBluetooth; with Metal the plug-in `HakiosMetalDevice` was on `IOAcceleratorES` and
CoreAnimation drew through the GPU, up to 17 frames a second with the payload in `boot_a` (`35 checks,
4 failed`: the files of init's four tests that the cut image leaves out) and up to 40 with the payload in
the system partition and nothing cut (`36 checks, 0 failed`). The pictures come from the job lists'
`fbgrab -log NAME`: `python3 tools/log/fbjpg.py LOG OUT.jpg NAME`.

## If it does not come back

A kernel that hangs costs a power cycle: the image boots Android unless asked for XNU, once. If `boot_a` itself is broken, the way back is the boot
ROM's download mode (mtkclient, `mtk.py w boot_a backup/boot_a_magisk.img`); check that you can reach it before the first flash.
