# From a clone to the home screen

Every command between `git clone` and Darwin 27's home screen on a phone whose package is on the
`devices` branch (the first one: `x6826b`), in order, with what each one needs. It is done on an Intel
Mac with macOS 14, the Command Line Tools and no Homebrew; the kernel's build fetches its own LLVM.
For a phone that is not the first phone, read [porting.md](porting.md) first: this page is the last
step of that one. In Russian, step by step: [darwin27.ru.md](darwin27.ru.md).

## What you need before the first command

| | |
| --- | --- |
| The phone | bootloader unlocked, Android rooted (Magisk), on slot `a`, USB debugging allowed for this computer |
| A partition for the system | 4.3 GB on the first phone (`xnusys`, cut out of `userdata` once: `tools/gpt-carve.py`, step 8 of porting.md); its name is the board file's `[loader] system_partition`. The image takes all but its last 96 MiB, which are the payload's |
| `backup/boot_a_magisk.img` | **your own** rooted boot image, the one that is in `boot_a` when Android boots. The loader is packed into it. Never in the repository |
| `adb`, `fastboot` in `PATH` | Android's platform-tools |
| The SDK | `iPhoneOS16.5.sdk` in `$THEOS/sdks` (github.com/theos/sdks); the user space is built against it |
| 7-Zip 23 or newer | as `7zz` or `7z` in `PATH`: it reads APFS and the system's disk images |
| `python3` 3.9 or newer, meson, ninja, bison, flex | the tools; Mesa's builds |
| The 27.2 IPSW, unpacked | `rootfs/`, `cryptex/os/`, `cryptex/app/`, `dyld/` in one directory, `REL` (default `build/ios27/ios27`). How: [darwin27-boot.md](darwin27-boot.md#the-kernel-and-the-build) |
| The phone's firmware | for Wi-Fi and Bluetooth, CONNSYS's files in `kernel/deps/build/connsys-fw/firmware`; for the touch controller, `fw-touch/hdl_firmware.img`. The package's journals say which files; never in the repository |
| Disk and network | about 40 GB; Apple's XNU and the build's sources, LLVM 19.1.7, the earlier system's two disk images (7.4 GB, below), Mesa's release |

The path of the clone must have no spaces in it.

## The commands

```sh
export HAKIOS_SERIAL=<your phone's adb serial>     # only ever in the environment, never in a file
export REL=$PWD/build/ios27/ios27                  # the unpacked IPSW

make check                               # everything that needs no phone
tools/device.py add x6826b               # the phone's package: what the kernel's table and the loader are built from

# the kernel (about 4 minutes once its dependencies are there)
kernel/assemble.sh ../xnu-12377-hakios   # Apple's xnu-12377.121.6 + kernel/overlay + kernel/patches, next to the clone
kernel/deps/setup.sh                     # LLVM 19.1.7, mig 138, the iig headers of macOS 26.5's SDK, the build's sources
kernel/build.sh -j8 -k                   # -k: one harmless error (lldbmacros) ends make with code 2
tools/get-zig.sh                         # zig's ld.lld links the loader (not needed where ld.lld is installed)

# the earlier system's small root, where the 27 release's small image starts
darwin/system/fetch.sh                   # the 20F66 system's files from Apple's server (about 30 min at 4 MB/s)
userland/build.sh                        # our programs; userland/root

# the 27 release's images
darwin/mtk27.sh env                      # the link trees, ldid, darwin/tools/pacstrip
darwin/tools/pacstrip cache "$REL/dyld-strip" "$REL"/dyld/dyld_shared_cache_arm64e{,.[0-9]*}
python3 darwin/tools/fips26.py "$REL/dyld-strip" STORED COMPUTED   # the build's two values: the script's head says where
darwin/mtk27.sh all                      # Mesa for arm64e (10 to 20 min the first time), libraries, Metal, images
darwin/mtk27.sh mali                     # the native Mali library

# the stage cache: the window server's shaders compiled on this Mac
userland/tools/air/mk-host-mac-llvm.sh   # Mesa for this Mac, once
export MESA_HOST_BUILD=$PWD/build/mesa-host-mac-llvm/build
userland/tools/rwacache.sh build/rwa-cache.bin          # about a minute
RWA_CACHE_BIN=$PWD/build/rwa-cache.bin RC_JOBS=$PWD/userland/jobs/rc.jobs.nativeui darwin/mtk27.sh root

# the phone
HAKIOS_PAYLOAD=flash tools/run/system-flash.sh "$REL/system27-prune.apfs"   # once an image; Android must be up
HAKIOS_RAMDISK="$REL/root26-sys.apfs" tools/run/mkpayload.sh               # every run: the kernel + the small image
HAKIOS_PAYLOAD=flash XNU_SECS=300 XNU_LOG_OUT=logs/run1.log XNU_ARGS="cpus=7 touchscreen=2 buttons=2 mtkbatt=1 \
  mali=1 mtk_cpufreq=3 mali_mhz=950 metal=1 camtl=1 fb_queue=1 fb_flip=1 metal_profile=4 fbscale=2 fbfit=3 \
  launchd_unsecure_cache=1 mali_native=1 thid_should_crash=0" tools/run/xnu-cycle.sh
```

The last command builds the loader, packs it with Android's kernel into `build/boot_a.img`, writes the
payload into the tail of the system partition and reads it back, flashes `boot_a`, lets Android come
up, asks it for one XNU boot, and reads the log when Android is back. Only `boot_a` and the system
partition are written; slot `b` never. The phone goes back to Android by itself `XNU_SECS` after the
kernel starts.

With the radios and the accelerometer, add `connsys_step=17 connsys_wake=0 connsys_bt=2 scp_step=7`
to the arguments and `JOBCACHE=1 ALLOW="wifi WiFi"` to the `root` step (the network to join is
`fw-wifi/network`: its name, then its passphrase; never in the repository). The camera is `mtkcam=5`,
without CONNSYS. Never `aks_keys=1` with the 27 release: launchd reboots at once.

## What a good run looks like

In `logs/run1.log`, in this order:

- `boot-args: debug=0x8 serial=3 rd=md0 emmc_root=1 cpus=7 touchscreen=2 ...`
- `board: x6826b (...)`, and the features the board lists
- `Darwin Kernel Version 12377.121.6`
- our init's `=== 36 checks, 0 failed ===`
- every five seconds, the GPU's `mali: GPU busy N% of 5 s: ...`
- every 30 s, once the window server draws, the native library's lines `rwn backboardd: 30s: ...`:
  the stage cache's hits (`stage cache: N stages taken from it, M compiled`), the command buffers of
  each second, and `fragment jobs N checked, 0 failed`
- `=== end of XNU log ===`, `starting Android`

A long run loses the middle of its log: what is early and was not kept for the host is gone.

To see the screen without looking at the phone, a job list takes pictures (`fbgrab -keep NAME`) and
`python3 tools/log/fbjpg.py logs/run1.log shot.jpg NAME` makes them into files; `fbshot=SECONDS` is the
kernel's own picture of the panel (`tools/log/fbshot.py`). The log keeps its start and its last 256 KB;
lines a program says with `!!` go into what the kernel keeps for the host (64 KB) and survive the
middle.

## The earlier system (20F66)

The first generation of this port ran the 20F66 system, from the same repository; its pieces are still
here and its small root is the 27 release's starting point. Its own run:

```sh
userland/build.sh                        # also userland/system.apfs (2.4 GB), the earlier system's image
tools/run/system-flash.sh                # system.apfs into the partition
tools/run/mkpayload.sh                   # the payload with userland/root.apfs
XNU_LOG_OUT=logs/run1.log tools/run/xnu-cycle.sh   # the board file's [boot]
```

`tools/run/image.sh wifi|bluetooth|metal` makes its images for those runs; it was not run with the
12377 kernel of this branch.

## If it does not come back

A kernel that hangs costs a power cycle: the image boots Android unless asked for XNU, once. If
`boot_a` itself is broken, the way back is the boot ROM's download mode (mtkclient,
`mtk.py w boot_a backup/boot_a_magisk.img`); check that you can reach it before the first flash.
[troubleshooting.md](troubleshooting.md) has what a message in the log means.
