# Devices

A device is described by data, in two places:

| What | Where | What it says |
| --- | --- | --- |
| A **device package** | `NAME/board.toml`, with the phone's README and journal beside it | What the device has: its SoC, the drivers it may start (`features`), the memory the loader may use (`[loader]`), Android's ramoops zones (`[ramoops]`), the panel and LK's framebuffer (`[display]`), the first diagnostic's probes (`[fbdiag]`), the cell (`[battery]`), CONNSYS's reserved memory (`[connsys]`), its default run (`[boot]`). |
| A **SoC file** | `arch/arm64/boot/dts/mediatek/SOC.toml` | Where the blocks of the chip are, once for every board with that chip: the interrupt controller, the cores and the UART (`[platform]`), every peripheral block (`[blocks]`), the big cluster's PLL (`[cpufreq]`). |

`arch/arm64/boot/dts/_template/board.toml` and `arch/arm64/boot/dts/_template/soc.toml` explain every key;
`arch/arm64/boot/dts/_template/README.md` is the form of a package's README.

## Where packages are kept

This branch describes no phone. It has the templates and the SoC files, which are shared. Phones are
packages on the repository's
[`devices` branch](https://github.com/oosexgang/xnu-kernel-mtk-helio/tree/devices): one directory
each, with nothing of the code.

```sh
tools/device.py available               # the packages the devices branch has
tools/device.py add x6826b              # bring one into this checkout: arch/arm64/boot/dts/x6826b
tools/device.py list                    # what is here, and where each is from
tools/device.py remove x6826b           # take it out again
tools/device.py add mine --from ../my-packages      # from a directory, or another branch or commit
tools/device.py export mine ../hakios-devices       # a package made here, to a checkout of the devices branch
```

A package that was brought in lies in `arch/arm64/boot/dts/NAME/`, which git ignores here. Packages are also found
in any directory named by `tools/boards.py --devices DIR` or the environment variable `HAKIOS_DEVICES`
(written as `PATH` is):

```sh
git worktree add ../hakios-devices devices
export HAKIOS_DEVICES=$PWD/../hakios-devices
```

## What is made of them

```sh
tools/boards.py list                    # the boards that are here
tools/boards.py show x6826b             # one board with its SoC's values
tools/boards.py status x6826b           # what it has on, what it could turn on, what the rest waits for
tools/boards.py check                   # validate everything; says which file and why
tools/boards.py new NAME --soc SOC      # start a new board from the template
```

`tools/boards.py gen` writes into `build/boards/`, for the boards that are there:

- `hakios_board_table.h`, the kernel's. At boot the kernel looks up the name the loader passed in
  `/chosen/darwin-board`. Drivers ask `hakios_board()` for addresses and
  `hakios_board_wants(HAKIOS_HAS_...)` before they start (`drivers/board/hakios_board.h`
  lists the features). A board that is not in the table gets no feature.
- `NAME/hakios_platform.h`, the loader's, for each board with a `[loader]` section: every value as a
  constant. The loader is built for one board.

Neither is kept in the repository. The builds make them (`kernel/build.sh`, `arch/arm64/boot/build.sh`), so
what is built knows exactly the boards whose packages were present: bring the package in, then build.
The tests make their own, with a stand-in phone (`workbench/devices`, `workbench/soc`).

`sysctl hw.board` shows what the running kernel found: `name`, `soc`, `description`, `features`.

## Adding a kind of data

When a driver needs something of a device that no section has yet, it becomes a key, not a constant:

1. **The kernel's struct**: a field in `struct hakios_board` (`drivers/board/hakios_board.h`).
2. **The schema**: the key and its kind in `SOC_SCHEMA` or `BOARD_SCHEMA` of `tools/boards.py`; if a
   feature cannot work without it, in `FEATURE_BLOCKS` or `FEATURE_SECTIONS`.
3. **The table**: a board's section goes into the kernel's table if it is in `TABLE_SECTIONS`, and
   into the loader's header if it is in `HEADER_SECTIONS`.
4. **The files**: the key explained in `arch/arm64/boot/dts/_template/` (a test fails while it is not), the value
   in the packages that have it, with a comment naming where on the phone it was read.
5. **The driver** reads `hakios_board()->…`.
6. **Tests**: `workbench/tools_test.py` for what the tool takes, refuses and writes.
