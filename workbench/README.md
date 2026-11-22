# Workbench

What is tested without a phone, and how a driver runs without a kernel. `make check` runs it all
(`workbench/run.sh` is its `test` part); CI runs the same on every push. It needs a C compiler, Python
3.9 or newer and git: no phone, no system image, no kernel toolchain.

## What runs

| Test | What it holds |
| --- | --- |
| `tools_test.py` | The TOML reader against every TOML file of the repository; the board files' checks and refusals; what `tools/boards.py gen` writes; `status` and `get`; that the template explains every key; the device tree reader; that `docs/features.md` is what `boards.py features` writes |
| `bootargs_test.py` | An argument is found by its read and described by the comment that names it; `docs/boot-args.md` is what `tools/bootargs.py` writes (`make docs` after a change) |
| The loader | `loader/mtkloader.c` compiled for the stand-in phone (syntax and types, `-Werror`), so that a key the loader uses and the generator does not write is found here |
| `input_queue_test.c` | `/dev/hakios-touch`: what the touch and key drivers post and what the system reads |
| `station_test.c`, `apple80211_test.c` | The Wi-Fi station's side, and the system's Wi-Fi requests answered as wifid sends them |
| `sparse_driver_test.c` | A sparse image served as a disk |
| `guard_test.c` | The speakers' guard: power under each limit, no sample over the peak, ordinary sound untouched |
| `audio_device_test.c` | `/dev/hakios-audio`: what is played reaches the driver that plays it |
| `battery_driver_test.c` | The MT6358's fuel gauge against a model of the PMIC wrapper and the PMIC |
| `keys_driver_test.c` | The power and volume keys against a model of the keypad and the PMIC |
| `example_pwrkey_test` | The example driver of [docs/writing-a-driver.md](../docs/writing-a-driver.md), from `docs/examples/` |

`guard-measure.sh` (with `guard_measure.c`) is not a test but a measurement: the speakers' guard's
output, second by second, for a sustained tone at the amplifiers' levels, for music-like noise and for
the cooling after it.

## How a driver runs on the host

A driver's test `#include`s the driver's own `.c` file, so the code under test is the code the kernel
builds, not a copy:

- `fakekernel/` is enough of XNU's interfaces for a driver to compile and run as an ordinary program:
  locks, threads, timers, sleeps, memory, the log, `PE_parse_boot_argn` (each test sets the boot
  arguments it wants), and the board table (`hakios_board()`).
- Where a driver talks to hardware, its registers are a model behind `rd()` and `wr()` of
  `hakios_mmio.h` (built with `HAKIOS_MMIO_TEST`): `pwrap_model.h` is the MT6358 behind MediaTek's PMIC
  wrapper, which the keys and the gauge drivers read through.
- `devices/phone-a/` and `soc/mt0001.toml` are a made-up phone and chip: every address is no real
  phone's, so a test cannot pass by a value being hard-wired somewhere.

A test lists its scenarios when run without one, and `run.sh` runs each in its own process.

## Adding a test

Write `NAME_test.c` here, include the driver's `.c`, fake what the kernel gives it, model the
hardware it touches, and add `NAME_test` to the list in `run.sh`. What a host test cannot show (the
real hardware's timing, what a register does that the model does not) is said in
[docs/writing-a-driver.md](../docs/writing-a-driver.md#what-the-host-test-can-and-cannot-show).
