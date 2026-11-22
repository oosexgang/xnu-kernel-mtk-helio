# Contributing

## Before you send anything

```sh
make check
```

It must pass. It runs the lint (every script parses, `shellcheck` where it is installed, the pinned
components, the publish check), validates the board and SoC files, checks that the generated
references (`docs/boot-args.md`, `docs/features.md`) are current, and runs the host tests of
`workbench/`. CI runs the same, plus `kernel/verify.sh`. After a change to a driver,
`kernel/syntax-check.sh --tree DIR` (or `make syntax-check`) says in seconds whether it still compiles,
without the kernel toolchain.

## Where a change goes

| The change | Goes in | Then |
| --- | --- | --- |
| A new phone, or a value of an existing one | Its device package on the `devices` branch: `NAME/board.toml`, with its README and journal. Not `arch/arm64/boot/dts/NAME` on the main branch, which git ignores there. | `tools/boards.py check`, with the package in `arch/arm64/boot/dts/` of a checkout of the main branch (`tools/device.py add NAME --from DIR` copies it there and checks it) |
| A new SoC, or a value of an existing one | `arch/arm64/boot/dts/mediatek/SOC.toml` | `tools/boards.py check` |
| A new feature | `HAKIOS_HAS_*` in `drivers/board/hakios_board.h`, its needs in `tools/boards.py` | `make docs` (`docs/features.md`) |
| A driver, or anything else HakiOS adds to the kernel | `kernel/overlay/` (a driver: `bsd/dev/`, a line in `bsd/conf/files`, its start in `hakios_drivers.c`) | `kernel/build.sh`, and its test in `workbench/` |
| A change to one of Apple's files | An assembled tree, then `kernel/export.sh` | Never edit `kernel/patches/*.patch` by hand: `kernel/verify.sh` rejects it. |
| A boot argument | A comment naming it as `name=…` beside its read | `make docs` |
| The loader | `arch/arm64/boot/` | `arch/arm64/boot/build.sh`; addresses come from the board files, not from code. |
| A library inserted into a process, a test program, the Metal device, the Mali library | `userland/src/` | `darwin/mtk27.sh libs`, `metal` or `mali`, then `root` |
| What a run does | `userland/jobs/` | `RC_JOBS=` of the `root` step |
| A build step of the 27 release's images | `darwin/` | `darwin/mtk27.sh STEP` |
| A tool | `tools/` (the project's), `tools/run/` (a run on the phone), `tools/log/` (what a run left), `userland/tools/` (the user space's) | its own head comment, and `tools/README.md` or `userland/README.md` |

[docs/architecture.md](docs/architecture.md) explains the split; [kernel/README.md](kernel/README.md)
has the two ways of working on the kernel. If you have a branch or a patch from before the layout of
22 November 2026: [docs/migrating.md](docs/migrating.md).

## Rules that keep the project portable

- **No device knowledge in device-independent code.** An address, a pin or a range that differs
  between phones belongs in the board or SoC file, and reaches the code through `hakios_board()` in
  the kernel and `hakios_platform.h` in the loader.
- **A driver for one part checks its feature.** After its boot argument, before it touches anything:
  `if (!hakios_board_wants(HAKIOS_HAS_…, "name"))`, which says which line is missing.
- **Off by default, read before write.** A new driver does nothing without its boot argument, and its
  first level only reads and logs.
- **Check what you replace.** A library that swaps a method of Darwin checks the method's type encoding
  first and leaves it alone, saying so, when it is not what it expects.
- **The loader says what it is about to do before anything that can hang**, so the last line of a
  log is the step that hung. It runs with the MMU off: no SIMD, no unaligned access, no builtins.
- **Test what can run without the phone.** Arithmetic and state machines go where the host tests can
  compile them; a driver's register traffic can be tested against a model (`workbench/`).
- **What two drivers need is written once**: the registers' access (`hakios_mmio.h`), the board
  (`hakios_board.h`), the PMIC wrapper's lock. [docs/writing-a-driver.md](docs/writing-a-driver.md)
  lists what a driver has at hand, and has an example driver to start from.
- **Say what was verified.** "Compiles", "passes the host tests" and "seen working on the phone" are
  three different statements. Commit messages and documents say which one applies, with the run.
- **Names.** `hakios_*` for what is the same on any phone, `mtk_*` for MediaTek's blocks, a chip's
  name only where the code is that chip's. No phone's name in a file's name.

## What must never be committed

`tools/check-publishable.sh` (part of `make check`) looks for these, but the rule is yours to keep:

- Apple's files: anything from an IPSW or a system image, SDK headers, shaders, the kernel itself.
- Firmware and binaries of the phone's maker: touch, CONNSYS, sensor hub firmware, device trees, boot
  images, the camera's register tables made from a capture.
- Pictures of a screen, built output, home-directory paths, a phone's serial number, a network's name
  or passphrase, keys, a symbolic link out of the repository.

A value read from a vendor's device tree or driver (an address, a range) is a fact and is fine, with
its source named in a comment.

## Commits

- One change per commit, with a body that says what changed and why.
- Name the paths. Say how it was verified, and what was not: a change that was run on a phone says
  which run and what the log showed.
- Regenerate before committing: `make docs`, `kernel/export.sh` if you worked in a tree. The board
  tables are not committed: the builds make them from the packages that are present.

## Style

C in the kernel follows the files around it: tabs, the comment above the code explains *why*, a
driver starts with a comment that says what the hardware is and where each number comes from. Shell
scripts run under bash 3.2 (macOS still has it) and pass `shellcheck`. Python needs nothing outside
the standard library and runs on 3.9.
