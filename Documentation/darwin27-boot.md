# Darwin 27 on the first phone: the lab notebook

How the 27 release's user space (27.2, build 24B119) was brought to its home screen and to an interface
that moves, on the first phone (MT6768, a Mali-G52), run by run: what stood in the way, in the order it
was met, what fixed it, and the measurements. The guides say how to do it; this page says how it was
found. Commit hashes are this repository's; a run is one boot of XNU, numbered on the build machine.

## The kernel and the build

The kernel is Apple's xnu-12377.121.6, the release's own, with this port's drivers. What 12377 needs
beyond the earlier kernel's build is all in `kernel/deps/setup.sh` and `kernel/build.sh`:

- **LLVM 19.1.7** (`kernel/deps/llvm19`): the Command Line Tools' clang 15 does not compile 12377.
  `host/bin/xnu-clang` takes it for everything but arm64e, because upstream LLVM compiles arm64e to
  empty functions.
- **`ld64` without `-lto_library`.** LLVM 19's clang passes its libLTO to the linker, and that crashes
  the Command Line Tools' `ld`. Nothing here uses LTO.
- **mig of bootstrap_cmds-138**, built without autotools: the Command Line Tools' mig has no `-mach_msg2`.
- **iig headers of the macOS 26.5 SDK**: 12377's `.iig` files have methods only that release's output
  carries.
- **`DEFAULT_TARGET=build RC_DARWIN_KERNEL_VERSION=12377.121.6 PLUTIL=plutil`** for make, and an SDK
  copy of its own (`kernel/deps/sdk/...xnu-12377.121.6.sdk`) in which libfirehose and libpthread are
  compiled against this release's headers.

The user space comes from the release's IPSW, unpacked on the Mac: macOS 14 cannot mount 27's APFS, so
the images are unpacked with `7zz x -snl -snld` and `unzbm` repairs the LZBITMAP files 7-Zip leaves; the
`.aea` images are decrypted first. `darwin/tools/pacstrip cache` rewrites the shared cache's pointer
authentication into plain instructions (`dyld-strip/`), and `darwin/tools/fips26.py` gives corecrypto
the HMAC of its rewritten code. `darwin/mtk27.sh` then builds the libraries, Mesa, the Metal plug-in,
the native Mali library, the small image and the system image ([building.md](building.md)).

The system image (4.16 GB, pruned) is written once into `xnusys`; it leaves the partition's last
96 MiB to the payload (the kernel and the small image), which no longer fits in `boot_a` once Mesa is
in it. The kernel's arguments carry `rd=md0 emmc_root=1`: the root is the system on `xnusys`, the small
image is `md1` on `/hakios`, and its `etc/rc` runs `/sbin/launchd.system`.

## From the lock screen to the Mali (runs 5 to 230)

| Runs | What happened | What it took |
| --- | --- | --- |
| 5 | The lock screen of 27.2 at about 180 s: the date, the battery's percentage, "swipe up". | `emmc_root=1` in IOFindBSDRoot again; the small image as `md1`; the framebuffer reading the 26 and 27 releases' swap request (its fields 0x48 further on); the panel cleared with aligned stores, not `bzero`, on device memory. |
| 6 | Touch: Settings opened and scrolled by a finger; `rwtouch` gestures reach SpringBoard. | — |
| 7 | With Metal the phone did not come back. | The image had the earlier release's arm64 Mesa, which arm64e processes cannot load: `mtk27.sh gl` builds an arm64e one. |
| 30 to 57 | The window server alive with Metal through Gallium's panfrost, the whole run; the wallpaper stripes and the dock. | HakiosMali's buffers made as memory of no task, wired by `prepare()`; a command buffer waits while more than eight are alive (jetsam ended backboardd at 1.7 GB otherwise); textures made over the caller's bytes. |
| 182 to 204 | The home screen drawn on the Mali: icons, their names, the dock, the glass (`5c1d6bd`). | QuartzCore's vertex shader `fixed_vert_lph_spc` has two `[[point_size]]` outputs; a known branch decided while translating kept the vertex stage from writing both as zero. |
| 205 to 230 | The whole home screen, no job chain failed (`49da41f`); pipelines and generic variants built ahead of the frame (`00e3adb`). | A sampler in every slot a shader declares (the failed fragment chains were descriptors of zeros, `77b07a2`); backboardd's memory from 2.3 GB to 0.47 (`ddb89cb`). |

## The native library (runs 500 to 569)

The Gallium path drew, but slowly and with a driver nobody here controlled. From run 500 the GPU's job
descriptors are written by this port's own library ([metal-glass.md](metal-glass.md)).

| Runs | What happened |
| --- | --- |
| 500 to 516 | `mali-probe` on the phone: the GPU is a Mali-G52 r1 (0x7402, two shader cores, two tiler levels). The first fragment job of ours cleared a 64x64 target (501), the first triangle (502), then varyings, uniforms, blending, textures, compute, depth, the Metal command model, IOSurfaces, Apple's own compute and render functions: 35 of 37 capabilities measured (`userland/src/mali/README.md`). |
| 517 to 531 | `air_metal.h`'s contract served by the native library (`bea180b`); the home screen drawn by it, no chain failed (531). |
| 532 to 549 | Descriptors built in cached memory and copied at submit (GPU memory is write-combined: reading it costs about 3 µs a KB); uniform copies cut to what is read; the tiler's levels 32 and 1024 instead of Panfrost's 512 and 1024, since this GPU has two: the full pass halved. Swipes about 17 frames a second (549). |
| 550 to 569 | Settings froze for 20 s on pipeline builds: variants built off the draw, compiles in parallel. Blank widgets and glass: the two point-size outputs slid Panfrost's varying layout (`c26f18f`). Attachments the pass does not care about are cleared, not loaded (`cbe6a9e`). |

## Speed (runs 570 to 626)

| Runs | Finding | Change |
| --- | --- | --- |
| 570 to 583 | The live wallpaper (PosterBoard's rainbow, vector paths rasterized every frame) was about 60% of the GPU's time; it is there when `sb_wallpaper=0` is among the arguments. | Runs without it: a full pass about 7 ms, swipes 25 to 29 frames a second (583). 950 MHz gave 12% more. |
| 574 | A GPU stall: a compute dispatch with dynamic threadgroup memory ran without it. | Such dispatches refused (`89c23a5`) until supported (`7aa4bec`, run 609). |
| 584 to 596 | Openings at 1 to 4 frames: pipelines compiled on the phone, 1 to 8 s each. | The stage cache, compiled on the Mac (`cbf4dcc`); the generic pipelines' variants into it (`8aa5da0`); a Mesa miscompile of halfword swizzles found by it (`4e56618`); recipes harvested again after their notification had fallen into the log's lost middle. |
| 595 to 599 | The CPU, not the GPU: widget extensions at 80 to 96% each after the first unlock; the eMMC driver reading 6 to 25 MB per opening by PIO at 11 MB/s. | `cpus=7`; eMMC by DMA (`8eb61e9`: 4 MB in 54 ms against 255). |
| 603 to 608 | Slow command buffers were one blit each: mipmaps built byte by byte from write-combined memory; and a throttle from run 52 sleeping in `-commandBuffer`. | Mipmaps from cached memory (`40a9f65`), then on the GPU (run 615); the throttle at 32 (`2d2820e`): openings 10 to 37 command buffers a second. |
| 609 to 613 | The idle home screen redrew everything ten times a second. | It was the Clock icon's second hand (`9551f48`); CoreAnimation repaints the whole screen for any change here. |
| 614 to 626 | Lock waits in the library; the kernel's buffer churn; the kernel's queue of 64 jobs full during an opening. | Buffers closed outside the lock (`f461c93`), a pool of 256 KB chunks (`096b5b6`), imported surfaces kept a second (`2d9bf0a`). An opening's first frames are GPU-bound: 40 to 50 ms of full-screen passes with several targets. |

## The glass, the drivers, the compiles (runs 627 to 669)

| Runs | What happened |
| --- | --- |
| 627 to 643 | Sessions driven by hand: shade 39 to 48 frames a second, Control Center 31 to 45, swipes 25 to 40, an opening 11 to 33. The blur behind a context menu is flat green (old: the same with the run 608 library). The menu's symbols drawn as black squares were new, found by bisecting to `7e14793` (textures created outside the library's lock) and fixed by `11b8607`. |
| 644 to 652 | The earlier release's drivers brought to 27: Wi-Fi joins the given network, DHCP, DNS, HTTP, and Settings lists networks; Bluetooth's controller, bluetoothd on the kernel's transport, CoreBluetooth powered and scanning; the accelerometer turns the screen; camera frames on `/dev/hakios-camera`. On the way: a panic for an unaligned copy into Wi-Fi's DMA memory, which 12377 maps as device memory unless it is in the non-cacheable window (`b957376`); a NULL dereference in OSKext's backtrace walk (patch `0004`); `aks_keys=1` makes launchd reboot at once; the camera and CONNSYS share a PMIC channel and are not run together. |
| 653 to 665 | Settings' Bluetooth pane showed string keys: the system image had lost every empty `.lproj` directory, and 411 bundles name English as their region (`88416c3` makes them; it needs a new system image). Overlays over bundles with code broke SpringBoard and were limited to bundles without. StocksWidget is killed by RunningBoard's watchdog and relaunched, in a loop. Harvested recipes cut on-phone compiles in an opening window from 3.2 s to 0.2 s. |
| 666 to 669 | The compiles themselves: the uber shader's colour program read its opcodes from bytes the static initializer wrote, so each of a dozen calls translated the whole switch. Folded now (`a135b47`): all 464 recipes on the Mac in 10 s instead of 146, a stage on the phone in about 60 ms; run 669, with no stage cache at all, compiled all 2084 stages on the phone and drew the same screens. |

## A run as it is made now

```sh
# once an image
HAKIOS_PAYLOAD=flash tools/run/system-flash.sh "$REL/system27-prune.apfs"
# every run
HAKIOS_RAMDISK="$REL/root26-sys.apfs" tools/run/mkpayload.sh
HAKIOS_PAYLOAD=flash XNU_SECS=560 XNU_ARGS="cpus=7 touchscreen=2 buttons=2 mtkbatt=1 mali=1 mtk_cpufreq=3 \
  mali_mhz=950 metal=1 camtl=1 fb_queue=1 fb_flip=1 metal_profile=4 fbscale=2 fbfit=3 launchd_unsecure_cache=1 \
  mali_native=1 thid_should_crash=0" tools/run/xnu-cycle.sh
```

With the drivers: `connsys_step=17 connsys_wake=0 connsys_bt=2 scp_step=7` (Wi-Fi, Bluetooth, the
accelerometer), or `mtkcam=5` without them (the camera). Never `aks_keys=1` with the 27 release.

## What does not work yet

- The blur behind menus and folders (flat green); an opening's first frames are bound by the GPU.
- Settings' Bluetooth pane until a system image with the empty `.lproj` directories is written; a scan's
  results do not reach an application that is not in the foreground.
- Camera.app (the capture stack of the earlier release is not ported); `/private/var` is not kept
  between runs (the eMMC driver only reads), so every boot rebuilds the icon and application caches.
- Photos stops at start: the image has no `default.metallib` for PhotosUICore.
