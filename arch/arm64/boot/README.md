# The loader

`mtkloader` is what the phone's LK starts in place of a kernel. It is packed in front of Android's
own kernel in your rooted boot image (`chainboot.py`), so each boot it chooses: Android, or, when
Android asked for it once, XNU.

| File | Contents |
| --- | --- |
| `start.S`, `link.ld` | The arm64 Image header LK looks for; the entry, exception vectors, the secondary core's entry |
| `mtkloader.c` | The choice, the payload, the device tree and boot_args, the log's way into ramoops, the watcher, the probes |
| `mtkdbg.h` | Output with the MMU off: ramoops' console zone and text on LK's framebuffer |
| `msdc.h` | The eMMC as LK left it: the GPT and blocks, in PIO mode |
| `mtkspi.h`, `tcm.h` | The touch controller's bus and its protocol (feature `touch-td4160`): the firmware upload |
| `mtkpwrap.h`, `mtkmali.h`, `mtkcam.h`, `mtkconnsys.h` | Probes: the PMIC, the GPU, the camera, what Android's tree says of CONNSYS. They report and go back to Android |
| `fbdiag.c`, `fbdiag_start.S`, `build_fbdiag.sh` | The first thing a port runs: does our code run, and can it say so |
| `identity.h`, `mkidentity.py` | The device-tree identity the user space is shown |
| `mkpayload.py`, `chainboot.py`, `build_flashed.py` | The payload ("XNUP"), the boot image |

## Built for one board

```sh
tools/device.py add x6826b      # the board's package
arch/arm64/boot/build.sh                 # build/loader/mtkloader.bin; extra arguments go to the compiler (-DXNU_CHECKPOINT=17)
```

Every address is the board's or its chip's, from `build/boards/NAME/hakios_platform.h`, which
`tools/boards.py gen` writes from the package and `arch/arm64/boot/dts/mediatek/`. With more than one phone's package present,
say which: `HAKIOS_BOARD=NAME`. A part of the loader that belongs to one feature is compiled in only
for a board that lists it.

Three inputs are yours and are made in `build/loader/` when missing, never committed:

- `touchfw.h`, from your phone's touch firmware image (`fw-touch/hdl_firmware.img`, `TOUCH_FW=FILE`).
  Without it the loader carries none and says so at boot.
- `font8x16.h`, the text on the panel: rendered from a system font on macOS; blank elsewhere (the log
  in ramoops is whole either way).
- `camjn1.h`, `camrx.h`, the camera's register tables from your own capture (`tools/camtable.py`,
  `tools/camrx.py`), only for the camera probe.

## Rules of the house

The loader runs with the MMU off, so all memory is device memory: no SIMD, no unaligned access, no
compiler builtins (`-mgeneral-regs-only -mstrict-align -fno-builtin`). It says what it is about to do
before anything that can hang. It is built `-Os`: the boot partition is small.

The compile-time switches (`-DMSDC_TEST=1`, `-DTIMER_PROBE=1`, `-DTOUCH_PROBE=1`, `-DGPU_PROBE=1`,
`-DCAM_PROBE=1`, `-DXNU_CHECKPOINT=n`, `-DXNU_VERBOSE=1`, `-DXNU_SERIAL=...`) are explained at the head
of `mtkloader.c`; [docs/porting.md](../../../docs/porting.md) says when each is used.
