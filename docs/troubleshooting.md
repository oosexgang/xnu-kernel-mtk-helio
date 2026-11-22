# Troubleshooting

What went wrong on the first phone, what it looked like, and what fixed it. A line here comes from a
run or a build that showed it; nothing is listed from guesswork. Most of what a run does is read
from its log (`XNU_LOG_OUT=...` of `tools/run/xnu-cycle.sh`), not from the screen.

## The board files and the tools

- **`tools/boards.py check` names a file and a key.** The key is missing, has the wrong type, or a
  feature lists a block the SoC file does not have. The message says which; `tools/boards.py status
  NAME` says what each feature waits for.
- **`kernel/syntax-check.sh` fails on a file of the overlay** with an error in Apple's headers that
  the file does not cause: the file needs the real build's generated headers. Add it to
  `kernel/syntax-check.skip`. A file on that list that compiles fails the check, so the list stays
  short.
- **`workbench/bootargs_test.py`: "run `make docs`".** A boot argument's read or its comment changed;
  `make docs` writes [boot-args.md](boot-args.md) again.
- **On a Mac, a script fails with "mapfile: command not found" or "wait: -n: invalid option".** macOS
  has bash 3.2. The scripts here do without both; a new one must too.

## Building

- **The boot image is too big for `boot_a` (32 MB).** The kernel, the loader and the payload were
  29.7 MB. With `HAKIOS_PAYLOAD=flash` the payload goes to the tail of the system partition, and the
  boot image is 23.9 MB ([building.md](building.md#the-payload-in-the-system-partition-instead-of-boot_a)).
  The Metal build needs this.
- **Mesa builds the wrong driver.** The default `DRIVERS` is `freedreno`, the Qualcomm port's GPU.
  Here it is `DRIVERS=softpipe,panfrost`.
- **A clean clone's image has no Wi-Fi.** The CONNSYS firmware is the phone's own and is never in
  the repository: `build.sh: fw/: 0 files ... absent`. Put it in
  `kernel/deps/build/connsys-fw/firmware` first ([building.md](building.md#wi-fi-bluetooth-and-metal)).

## At boot

- **Red text on the screen, then Android.** It is the loader's fault handler: `*** loader fault:
  vector ... ESR ... ELR ... FAR ...`, shown for 5 seconds, then the loader starts Android. Take a
  picture of the screen, or let Android come up and read the line from pstore
  (`/sys/fs/pstore/console-ramoops*`). Once, a reboot by hand cleared it and it did not come back; ELR
  says which instruction it was.
- **pstore has only "starting Android".** The loader did not see the request to start XNU, so Android
  booted. Run again.
- **`payload: ITS SUM IS NOT ITS HEADER'S: not used`.** The payload in the system partition's tail is
  not the one its header describes: a write was cut short, or the partition was written over.
  Write it again with `tools/run/payload-flash.sh`; it reads back what it wrote.
- **`payload: none in the tail of xnusys`.** The boot image was built with `HAKIOS_PAYLOAD=flash`
  and the payload was never written. `tools/run/xnu-cycle.sh` writes it when `HAKIOS_PAYLOAD=flash` is set
  for the run too.
- **launchd panics in the key bag step.** The image has the key store in it and the run did not
  pass `aks_keys=1`. The image and the boot arguments must agree: `tools/run/image.sh bluetooth` with
  the arguments [building.md](building.md#wi-fi-bluetooth-and-metal) gives.

## What the screen shows

- **Icons and text too big.** The run had no `fbscale=2 fbfit=3`. They are in the board file's
  `[boot] bootargs` now; a run with its own `XNU_ARGS` must include them.
- **No wallpaper.** `sb_wallpaper=0` was among the arguments: it turns the wallpaper off. Leave it
  out.
- **Pictures from a run.** `/hakios/bin/fbgrab -log NAME` in the run's job list puts a JPEG in the
  log; `python3 tools/log/fbjpg.py LOG OUT.jpg NAME` takes it out. The log keeps its last 256 KB, which
  holds about two pictures, so take them as the last thing a run does.

## Hardware

- **Bluetooth: Settings says "Unavailable".** `bluetoothd` exits at once when
  `/hakios/bin/tolog` is missing. `userland/build.sh` used to build it only with `METAL=1`; it
  builds it always now. Build the image again.
- **Wi-Fi: the radio does not read its firmware.** The image on the phone is a clean clone's,
  without CONNSYS firmware (see Building).
- **Keys or battery do nothing.** Both are off until asked: `buttons=2`, `mtkbatt=1`. The board
  file must list `keys-mt6358` and `battery-mt6358`. The log says which of the two is missing.
- **The battery percentage disagrees with Android's.** `mtkbatt` starts from the percentage Android's
  gauge left and follows the PMIC's charge counter. If that and the voltage are more than 30 points
  apart, it takes the voltage, and says so once in the log. `mtkbatt=3` takes the voltage alone.

## If the phone does not come back

A kernel that hangs costs a power cycle: the image boots Android unless it is asked for XNU, once. If
`boot_a` itself is broken, the way back is the boot ROM's download mode with mtkclient
([building.md](building.md#if-it-does-not-come-back)). Check that you can reach download mode before
the first flash.
