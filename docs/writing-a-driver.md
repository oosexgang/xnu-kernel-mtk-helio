# Writing a driver

A driver here is one C file under `kernel/overlay/bsd/dev/`, built into the kernel. The example is
[examples/example_pwrkey.c](examples/example_pwrkey.c): it reads the power key from the MT6358 PMIC
and shows it as the sysctl `kern.hakios.example_pwrkey`. Its test,
[examples/example_pwrkey_test.c](examples/example_pwrkey_test.c), runs in `make check`. The example
is not built into the kernel.

## The rules, and why

- **Off until asked.** Nothing is touched unless a boot argument asks for it (`example_pwrkey=1`).
  A driver that writes to the wrong register of a phone can leave it unable to boot Android.
- **Only on a board that has the part.** The driver checks with
  `hakios_board_wants(HAKIOS_HAS_..., "arg=1")`. On a board whose file does not list the feature, the
  driver says so in the log and does nothing.
- **Addresses come from the board, never from the driver.** `hakios_board()->blocks.pwrap` is the
  SoC file's value (`soc/*.toml`). A constant in a driver works on one phone and writes to something
  else on the next one.
- **Every wait is bounded.** A register that never changes ends the wait with an answer such as "no
  answer". It does not hang the boot.
- **The PMIC wrapper is shared.** Take `mtk_pwrap_lock()` around every transaction:
  `mtk_keys.c`, `mtk_battery.c`, `mtk_camera.c` and `mtk_connsys.c` all use its WACS2 channel.
- **Say what was found.** One line in the log at start: what the driver read, and why it is stopping
  when it stops. A run nobody watches is read from that log.
- **The comment beside a boot argument's read is its documentation.** `tools/bootargs.py` puts it in
  [boot-args.md](boot-args.md). Write `name=N: what it does` next to the `PE_parse_boot_argn`.

## What a driver has at hand

| Need | Where |
| --- | --- |
| The board's addresses and features | `hakios_board.h`: `hakios_board()`, `hakios_board_wants()` |
| A block's registers | `ml_io_map(physical, size)`, then 32-bit loads and stores |
| The PMIC | the WACS2 sequence of `mtk_battery.c` (`pmic_read`, `pmic_write`) under `mtk_pwrap_lock()` |
| A button or a touch for the system | `hakios_touch_post()` (`sys/hakios_input.h`) |
| A value for programs | `SYSCTL_PROC` / `SYSCTL_INT` under `kern.hakios` |
| A thread of its own | `kernel_thread_start()`, then `thread_deallocate()` |
| Waiting | `delay_for_interval(N, NSEC_PER_MSEC)` in a thread; `delay(usec)` for short waits |

## Steps

1. **Read the part's registers under Android first**, as root, and write down what Android's driver
   writes (its source is in the vendor's kernel). [porting.md](porting.md) says where each kind of
   value is read.
2. **Add the feature**: a `HAKIOS_HAS_*` bit in `kernel/overlay/bsd/dev/hakios_board.h`, and in
   `tools/boards.py` what it needs (`FEATURE_BLOCKS`, `FEATURE_SECTIONS`) and its boot argument
   (`FEATURE_BOOTARGS`). Then list it in the board files that have the part.
   `tools/boards.py check` must pass.
3. **Write the driver**, starting from the example. Put register access behind the
   `HAKIOS_MMIO_TEST` seam (`rd`/`wr`), as the example, `mtk_keys.c` and `mtk_battery.c` do. The
   kernel's build compiles the same code either way.
4. **Write its test** next to the others in `workbench/`. Include the driver's `.c`, fake the kernel
   with `workbench/fakekernel`, and model the hardware (`workbench/pwrap_model.h` is the PMIC's). Write one
   scenario per behavior, including the failures: the part does not answer, the argument is not
   given, the board does not have the part. Add the test to the list in `workbench/run.sh`.
5. **Build it in.** Assemble the tree (`kernel/assemble.sh ../xnu-12377-hakios`), add the file to
   `bsd/conf/files` there (`bsd/dev/NAME.c standard`, as `mtk_keys.c` is), and call its init from
   `hakios_input_init()` in `hakios_input.c`. Then `kernel/export.sh` writes the change back into
   `kernel/patches/` and `kernel/overlay/`.
6. **Check:** `make syntax-check` (seconds), `make check`, then `make kernel` (`kernel/build.sh`).
7. **Run it on the phone** with its argument added: `XNU_ARGS="... example_pwrkey=1" tools/run/xnu-cycle.sh`
   ([building.md](building.md)). Read the result in the log, not from the screen.
8. `make docs` updates [boot-args.md](boot-args.md); [drivers.md](drivers.md) gets a line by hand.

## What the host test can and cannot show

It shows that the driver's logic does what it should with the registers the model gives it: the
sequence, the bounds, every failure path, and that nothing is touched when nothing was asked for.

It cannot show that the model is the hardware. A wrong register address, a missing clock, or a part
that needs a power rail the model does not have only shows up on the phone. A driver whose tests pass
has not been run until it has run on a phone; say which of the two it is when you commit it.
