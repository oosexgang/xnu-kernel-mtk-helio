# Troubleshooting

What went wrong on the first phone, what it looked like, and what fixed it. A line here comes from a
run or a build that showed it; nothing is listed from guesswork. Most of what a run does is read
from its log (`XNU_LOG_OUT=...` of `tools/run/xnu-cycle.sh`), not from the screen. Run numbers and
commits point into [darwin27-boot.md](darwin27-boot.md) and the history.

## The checks and the tools

- **`tools/boards.py check` names a file and a key.** The key is missing, has the wrong type, or a
  feature lists a block the SoC file does not have. The message says which; `tools/boards.py status
  NAME` says what each feature waits for.
- **`make check`: "run `make docs`".** A boot argument's read or its comment changed, or a feature's
  table: `make docs` writes [boot-args.md](boot-args.md) and [features.md](features.md) again.
- **`tools/check-publishable.sh` says "link out of the repository".** A symbolic link whose target
  is an absolute path, or one that climbs out of the checkout, was added: it names one machine's
  files. Copy what is needed, or leave it ignored (`/fw-*` is).
- **`kernel/syntax-check.sh` fails on a file of the overlay** with an error in Apple's headers that
  the file does not cause: the file needs the real build's generated headers. Add it to
  `kernel/syntax-check.skip`. A file on that list that compiles fails the check, so the list stays
  short.
- **On a Mac, a script fails with "mapfile: command not found" or "wait: -n: invalid option".** macOS
  has bash 3.2. The scripts here do without both; a new one must too.
- **A command typed in zsh with `$CC` or `$DEFS` in it fails to find a compiler or a flag.** zsh does
  not split a variable into words. Run such a line in `bash -c '...'`; the scripts are bash.

## Building

- **`mtk27: no userland/root (run userland/build.sh once)`.** The 27 release's small image starts from
  the earlier system's: `darwin/system/fetch.sh`, then `userland/build.sh`.
- **`darwin/mtk27.sh gl` takes 15 minutes again.** A change to any patch of `userland/tools/mesa/`
  makes `mk-mesa.sh` unpack Mesa anew and build it whole; a change to the AIR front end only copies
  it in and rebuilds what changed.
- **`userland/tools/rwacache.sh`: "MESA_HOST_BUILD: a host Mesa build directory with panfrost".** The
  stage cache is compiled by Mesa built for this Mac: `userland/tools/air/mk-host-mac-llvm.sh` once,
  then `MESA_HOST_BUILD=$PWD/build/mesa-host-mac-llvm/build`.
- **rwacheck or rwagen says "stopped the host's compiler".** The Mac's Mesa is built with its
  assertions, the phone's is not: an assertion here is a real compiler bug the phone would miscompile
  silently (`4e56618` was one). One is known and harmless: `path_stroke_curve` with NIR's validation on.
- **The boot image is too big for `boot_a` (32 MB).** With `HAKIOS_PAYLOAD=flash` the payload goes
  to the tail of the system partition and the boot image carries only the loader in front of
  Android (23.9 MB on the first phone). Every 27 release run needs it.
- **Mesa builds the wrong driver.** The default `DRIVERS` is `freedreno`, the Qualcomm port's GPU.
  Here it is `DRIVERS=softpipe,panfrost` (`darwin/mtk27.sh gl` passes it).
- **A clean clone's image has no Wi-Fi.** The CONNSYS firmware is the phone's own and is never in
  the repository. Put it in `kernel/deps/build/connsys-fw/firmware` first.

## At boot

- **Red text on the screen, then Android.** It is the loader's fault handler: `*** loader fault:
  vector ... ESR ... ELR ... FAR ...`, shown for 5 seconds, then the loader starts Android. Take a
  picture of the screen, or let Android come up and read the line from pstore
  (`/sys/fs/pstore/console-ramoops*`). ELR says which instruction it was.
- **pstore has only "starting Android".** The loader did not see the request to start XNU, so Android
  booted. Run again.
- **`payload: ITS SUM IS NOT ITS HEADER'S: not used`.** The payload in the system partition's tail is
  not the one its header describes: a write was cut short, or the partition was written over.
  Write it again with `tools/run/payload-flash.sh`; it reads back what it wrote.
- **`payload: none in the tail of xnusys`.** The boot image was built with `HAKIOS_PAYLOAD=flash`
  and the payload was never written. `tools/run/xnu-cycle.sh` writes it when `HAKIOS_PAYLOAD=flash` is
  set for the run too.
- **The 27 release: launchd reboots the phone at once.** `aks_keys=1` was among the arguments (run
  644; nvram says "keybagd"). Leave it out with the 27 release.
- **The earlier system: launchd panics in the key bag step.** The image has the key store in it and the
  run did not pass `aks_keys=1`. The image and the boot arguments must agree.
- **Panic "Unaligned kernel data abort" in `memmove` from `wf_tx`.** 12377 maps a write-combined range
  outside the kernel's own memory as device memory unless it is in the non-cacheable window; Wi-Fi's
  DMA memory now is (`b957376`, run 645).
- **Panic in `OSKext::foreachKextInBacktrace`.** The loaded kexts' summary was NULL when telemetry
  walked a backtrace; patch `0004` checks it (run 646).
- **The camera and the radios together.** They share a PMIC channel: run `mtkcam=5` without
  `connsys_step=`, and the radios without the camera.

## The log

- **A long run lost its middle.** The log keeps its start and its last 256 KB. What matters later is
  said as a kept line (`!!` at the start of a program's line, `hakios_host_keep_add` in the kernel):
  those come back whole, up to 64 KB.
- **Pictures from a job list are missing or cut.** The kept lines are full: the 64 KB went to other
  kept lines first (HCI dumps of a scan: `bt_dump=0`; a process table: `cputop` without `keep`). Take
  pictures smaller (`fbgrab -keep NAME 150 0.4`) and fewer.
- **Pictures from a run.** `/hakios/bin/fbgrab -keep NAME WIDTH QUALITY` in the job list keeps a JPEG
  for the host; `python3 tools/log/fbjpg.py LOG OUT.jpg NAME` takes it out.

## What the screen shows

- **Icons and text too big.** The run had no `fbscale=2 fbfit=3`. They are in the board file's
  `[boot] bootargs`; a run with its own `XNU_ARGS` must include them.
- **Swipes at 17 frames a second, the GPU 60% busy.** The live wallpaper: with `sb_wallpaper=0` among
  the arguments PosterBoard draws its rainbow as vector paths every frame. Without it, 25 to 29.
- **The home screen redraws ten times a second while nothing moves.** The Clock icon's second hand
  (stopped by `inject_sb.c` since `9551f48`). CoreAnimation repaints the whole panel for any change
  here, so anything that animates costs a full frame.
- **Symbols in a menu drawn as black squares.** Textures and buffers made while another thread held
  them (`7e14793`); fixed by `11b8607` (run 643). If they come back, compare with a run of the
  library before your change: one picture is not a regression, two runs are.
- **The blur behind a menu or a folder is flat green and white.** Open; it is old (the same with the
  run 608 library).

## Metal and the GPU

- **An opening stutters and `stage cache: ... M compiled` grows.** Stages not in the cache were
  compiled on the phone. `rwn_keylog=1` names each with its key; harvest the run's recipes
  (`userland/tools/rwm-recipes.py`, `rwm-variants.py`) and build the cache again. Since `a135b47` a
  miss costs about 60 ms.
- **`pipeline compiling, ms: AIR to NIR ...` in the seconds.** Before `a135b47` the colour program's
  opcodes were not known while translating and every compile carried the whole interpreter; a
  front end older than that commit is the cause.
- **`fragment jobs N checked, M failed`, or a GPU fault "into a freed buffer".** A surface's memory
  was closed while a job still used it: imported surfaces are now closed under the library's lock
  and taken back from the closing list when imported again (`f798295`).
- **The phone runs out of memory, frames stop for seconds.** Free memory is about 30 MB with 1.4 GB
  available: a larger buffer cache (128 MB, run 618) starves the system. Keep the library's caches as
  they are.
- **A compute dispatch is refused.** One with dynamic threadgroup memory needs workgroup storage;
  refused by name where the library cannot give it (`89c23a5`, then `7aa4bec`).

## The system

- **Settings' Bluetooth pane shows keys like `PLACARD_SUBTITLE`, no switch; Calendar's menu says
  `ADD_EVENT`.** The system image lost every empty `.lproj` directory, and bundles whose development
  region is English then find no strings. `mkimage26.py` makes them again (`88416c3`): a new system
  image. Overlays from the small image cannot stand in for bundles with code: SpringBoard died (run 655).
- **Wi-Fi's daemon does not run.** Its job is limited to a device tree with a `wlan` node, and the
  job list's denials leave it out: `ALLOW="wifi WiFi"` for the `root` step, which also lifts the limit.
- **Bluetooth: Settings says "Unavailable" (earlier system).** `bluetoothd` exits at once when
  `/hakios/bin/tolog` is missing; every image has it now.
- **audiomxd spins at 90% with launchd at 40%, then crashes.** It looks for airplayd's endpoint
  manager, whose job is left out; a stand-in job answers it (`com.hakios.nullairplay`, with
  `JOBCACHE=1`).
- **Two cores busy for minutes after the first unlock.** Widget extensions reload their timelines;
  StocksWidget is killed by RunningBoard's watchdog and launched again, in a loop.
- **Photos stops at start.** The image has no `default.metallib` for PhotosUICore.

## Hardware

- **Keys or battery do nothing.** Both are off until asked: `buttons=2`, `mtkbatt=1`. The board
  file must list `keys-mt6358` and `battery-mt6358`. The log says which of the two is missing.
- **The battery percentage disagrees with Android's.** `mtkbatt` starts from the percentage Android's
  gauge left and follows the PMIC's charge counter. If that and the voltage are more than 30 points
  apart, it takes the voltage, and says so once in the log. `mtkbatt=3` takes the voltage alone.
- **Wi-Fi: the radio does not read its firmware.** The image is a clean clone's, without CONNSYS
  firmware (see Building).

## If the phone does not come back

A kernel that hangs costs a power cycle: the image boots Android unless it is asked for XNU, once. If
`boot_a` itself is broken, the way back is the boot ROM's download mode with mtkclient
([building.md](building.md#if-it-does-not-come-back)). Check that you can reach download mode before
the first flash.
