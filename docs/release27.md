# The 27 release on the X6826B

Branch `ios27`: the kernel of the 27 release (xnu-12377.121.6) with this port's drivers, and its user space, 27.2
(24B119, from the iPhone12,1 IPSW). Built on a Mac (macOS 14, x86_64, Command Line Tools 15.3).

## What it does

| Run | What happened |
| --- | --- |
| 5 | The lock screen of 27.2 at about 180 s: date, battery percentage (`mtkbatt`), "Swipe up to open". |
| 6 | Touch: Settings opened and scrolled with a finger; `rwtouch` swipes reach SpringBoard. |
| 7 | With Metal (`metal=1 camtl=1`): the phone did not come back to Android and the log was empty. The image had the 16.5 build's Mesa, which is arm64 and cannot be loaded by arm64e processes. `darwin/mtk27.sh metal` now builds an arm64e one; not yet run on the phone. |

Without Metal everything is drawn by the CPU, which is slow (16.5 drew 10 frames a second that way; 16.5
with Metal drew up to 40).

## The kernel

```bash
kernel/deps/setup.sh        # once: also LLVM 19.1.7, the macOS 26.5 SDK's DriverKit headers, mig 138
kernel/build.sh -j8 -k
```

What 12377 needs beyond the 16.5 kernel's build, all in `kernel/deps/setup.sh` and `kernel/build.sh`:

- **LLVM 19.1.7** (`kernel/deps/llvm19`). The Command Line Tools' clang 15 does not compile 12377.
  `host/bin/xnu-clang` takes it for everything but arm64e, because upstream LLVM compiles arm64e to empty
  functions.
- **`ld64` without `-lto_library`.** LLVM 19's clang passes its libLTO to the linker, and that crashes
  the Command Line Tools' `ld`. Nothing here uses LTO.
- **mig of bootstrap_cmds-138**, built without autotools. The Command Line Tools' mig has no `-mach_msg2`.
- **iig headers from the macOS 26.5 SDK.** 12377's `.iig` files have methods only that release's
  output carries.
- **`DEFAULT_TARGET=build RC_DARWIN_KERNEL_VERSION=12377.121.6 PLUTIL=plutil`** for make.
- **The SDK copy `kernel/deps/sdk/iPhoneOS16.5.xnu-12377.121.6.sdk`.** libfirehose and libpthread are
  compiled against this release's headers, so the 16.5 kernel's copy stays as it was.

## User space

```bash
REL=/path/to/ios27 darwin/mtk27.sh all
```

`REL` is the extracted IPSW (`rootfs/`, `cryptex/os/`, `cryptex/app/`, `dyld/`, `dyld-strip/`). macOS
14 cannot mount 27's APFS, so the images are unpacked with `7zz x -snl -snld`, and `unzbm` fixes the
LZBITMAP files 7-Zip leaves. The `.aea` images are decrypted with blacktop's `ipsw fw aea`.
`darwin/tools/pacstrip cache` writes `dyld-strip/`, and `darwin/tools/fips26.py` patches it.

`darwin/mtk27.sh` makes the darwin-renoir tree that the Qualcomm port's scripts expect, out of this
repository. Then it builds:

- our libraries for the system's processes, arm64e (`inj26.sh`);
- Mesa with panfrost and the Metal driver, arm64e, plus QuartzCore's AIR shaders from the 27 release's own
  `default.metallib` (`metal27.sh`);
- the small image (`root26-sys.sh`);
- the system image (`mkimage26.py --prune`).

## A run

The system image (4.16 GB) leaves the last 96 MiB of `xnusys` free for the payload. With Mesa, the
payload no longer fits in `boot_a`.

```bash
HAKIOS_PAYLOAD=flash tools/run/system-flash.sh $REL/system27-prune.apfs
HAKIOS_RAMDISK=$REL/root26-sys.apfs tools/run/mkpayload.sh
HAKIOS_PAYLOAD=flash XNU_ARGS="cpus=4 touchscreen=2 buttons=2 mtkbatt=1 fbscale=2 fbfit=3 launchd_unsecure_cache=1 sb_wallpaper=0" tools/run/xnu-cycle.sh
```

Add `metal=1 camtl=1 fb_queue=1 fb_flip=1` for Metal.

`sb_wallpaper=0` is for the two wallpapers of run 5. On 16.5, PosterBoard drew nothing, so
`libhakios-sb` put a wallpaper of its own over the wallpaper window. On 27 PosterBoard draws itself, and
run 5 showed both at once. That `sb_wallpaper=0` leaves only PosterBoard's has not been seen in a picture
yet: run 6's were taken while Settings was open.

The kernel's boot arguments are `rd=md0 emmc_root=1`. The root is the system on `xnusys`, the small
image is `md1` on `/hakios`, and its `etc/rc` runs `/sbin/launchd.system`.
