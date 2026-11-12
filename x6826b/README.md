# x6826b: Infinix Hot 20 (X6826B)

| | |
| --- | --- |
| SoC | MediaTek Helio G85 (`soc/mt6768.toml`), Mali-G52 MC2 |
| Panel | 720 x 1640, video mode, behind OVL0; LK's framebuffer is 736 pixels a line |
| Storage | eMMC on MSDC0; the system image in a partition `xnusys` (4 GiB, cut off the end of `userdata`) |
| PMIC, radio | MT6358; CONNSYS with an MT6631 |
| Android | kernel 4.19.191, Magisk in `boot_a`; ramoops at `0x4d010000` |

Run numbers are those of the logs the work was done with (on the author's machine); the journals in
`journal/` tell each story run by run. The journals were written before the project's files and
interfaces were renamed and use the names of that time (`redwood_*`, `mtkloader/`, `ios165/`);
`docs/migrating.md` of the main branch maps them.

## What works

| What | Feature, boot argument | Seen |
| --- | --- | --- |
| Boot to the home screen, drawn by the CPU, four cores, the root read from `xnusys` | `emmc-msdc`, `emmc_root=1` | from an empty clone on 2026-11-10: `36 checks, 0 failed`, the home screen in the log's picture |
| Touch (OmniVision TD4160 on SPI2; the loader uploads its firmware) | `touch-td4160`, `touchscreen=2` | `touch: TD4160-… is up` in every run since |
| The GPU (Mali-G52), Mesa's panfrost, the Metal device over it | `gpu-mali-g52`, `mali=1` | `mali: came up to step 6 of 6` |
| Power and volume keys; the cores' clocks | `keys-mt6358`, `buttons=2` | |
| The accelerometer through the sensor hub; rotation | `scp-sensors`, `scp_step=N` | run 276 and after ([journal/sensors-x6826b.md](journal/sensors-x6826b.md)) |
| Wi-Fi: joins a network from Settings, Safari loads pages | `connsys-mt6631`, `connsys_step=17` | runs 457 and 467 ([journal/wifi-x6826b.md](journal/wifi-x6826b.md)) |
| Bluetooth: `bluetoothd` on the MT6631, apps see real BLE devices in a scan | `connsys-mt6631`, `connsys_bt=2` | run 711, runs 712 to 762 ([journal/bluetooth-x6826b.md](journal/bluetooth-x6826b.md)) |
| The battery's real percentage in the status bar | `battery-mt6358`, `mtkbatt=1` | runs 765 to 769: the same number as Android right after |
| The main camera (Samsung S5K JN1) powered, its identity `0x38E1` read, its register table replayed | the loader's probe (`tools/cam-probe.sh`) | 2026-11-07 ([journal/camera-x6826b.md](journal/camera-x6826b.md)) |

## What does not

- **A confirmed Bluetooth pairing, the profiles, audio.** Pairing reaches the confirmation request.
- **The charger chip (RT9471)** is not driven: on a computer's port the battery gives 0.2 to 0.45 A.
  **The battery's temperature** is not read.
- **Camera capture** into the system: the sensor streams, the receiver (SENINF, CAMSV) is being worked
  out.
- **Cellular, the speakers and microphones, vibration**: nobody has started.
- **Writing to the flash**: the kernel's eMMC driver only reads; `/private/var` is in RAM and is lost
  with the run.

## Where the values are from

`board.toml` names the source of each beside it. The device tree was read as root under the Android
above (`/sys/firmware/fdt`); ramoops' zones from that kernel's log on 2026-10-19; the framebuffer's
address by the loader's `fbdiag`; CONNSYS's two reserved ranges from the kernel's log at boot (they
are allocated, not fixed in the tree: read yours).

## Yours to bring

| File | From | For |
| --- | --- | --- |
| `backup/boot_a_magisk.img` | your own rooted `boot_a` | every run: the loader is packed into it |
| `fw-touch/hdl_firmware.img` | `/vendor/firmware` of your phone | touch |
| CONNSYS's firmware (`kernel/deps/build/connsys-fw/firmware/`) | `/vendor/firmware` ([journal/wifi-x6826b.md](journal/wifi-x6826b.md) lists the files) | Wi-Fi, Bluetooth |
| a capture of Android bringing the camera up | your phone ([journal/camera-x6826b.md](journal/camera-x6826b.md)) | the camera's register tables |

## The way back

`boot_a` starts Android unless asked for XNU once. If `boot_a` itself is broken: the boot ROM's
download mode (hold Power, Volume up and Volume down with the cable in, release Power, keep the
volume keys held) and mtkclient as root: `mtk.py w boot_a backup/boot_a_magisk.img`. This was needed
once, on 2026-10-19, after an image that always started XNU with no watchdog.
