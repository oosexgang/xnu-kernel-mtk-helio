# Tools

The project's own tools: the board files and device packages, the generated references, the checks,
and what a port reads off a phone. One run on the phone is `run/`; what a run left is turned into
pictures, sound and numbers by `log/`. The user space's tools (the shader front end, Mesa, the stage
cache, APFS, packages) are in `userland/tools/`; the Darwin images' in `darwin/tools/`.

Each script says at its head what it does and what it needs. The ones that touch a phone take its
serial number from `HAKIOS_SERIAL`, never from a file, check the phone's identity against the board
file's `[boot] android_device`, and stop when the phone is not on slot `a`.

## The project

| Tool | What it does |
| --- | --- |
| `boards.py` | Board and SoC files: `check`, `status NAME` (what is on, what can be turned on next), `new NAME --soc SOC --dtb FILE` (a board file started from the phone's device tree), `gen` (the kernel's table and the loader's header), `features --write` (`docs/features.md`) |
| `device.py` | Device packages: `available`, `add NAME` (from the `devices` branch, or `--from SOURCE`), `remove NAME`, `list`, `export NAME DIR` (back to a checkout of the `devices` branch) |
| `bootargs.py` | `docs/boot-args.md` from the kernel's sources (`--write`; `make docs`) |
| `components.py` | `components.toml` against the scripts that fetch each component (`check`, in `make lint`) |
| `lint.sh` | The checks of form: every shell and Python script parses, `shellcheck` where it is installed, the components, the publish check |
| `check-publishable.sh` | What must never be published: binaries, firmware, pictures, home paths, serial numbers, keys, old names, links out of the repository |
| `minitoml.py` | The TOML reader for a Python without `tomllib` |
| `get-zig.sh`, `mk-libdeflate.sh` | zig's linker for the loader (`build/zig`); libdeflate's decompressor into the kernel tree for the APFS driver |

## Reading a phone

| Tool | What it does |
| --- | --- |
| `devicetree.py`, `fdtfind.py` | A flattened device tree read (what `boards.py new --dtb` takes from it); the nodes that match a word |
| `gpt-carve.py`, `userdata-carve.py` | A partition of this port's own cut off the end of another (the system image's), as a new GPT written to a file and checked here: never to the phone by these tools |
| `android/` | Small programs run under Android as root |
| `mktouchfw.py`, `mkfont8x16.swift` | The loader's local inputs: the touch controller's firmware as C, from your phone's image; the panel's font, rendered from a system font on macOS |
| `camtable.py`, `camrx.py` | The camera sensor's and receiver's register tables, from a capture of your own phone's Android bringing the camera up |

## `run/`: one run on the phone

| Tool | What it does |
| --- | --- |
| `xnu-cycle.sh` | A run: the loader built with the run's flags, packed in front of your rooted boot image's kernel, `boot_a` written, one XNU boot asked for, the log read back when Android is up (`XNU_ARGS`, `XNU_SECS`, `XNU_LOG_OUT`; `HAKIOS_PAYLOAD=flash` for the payload in the system partition) |
| `mkpayload.sh` | `build/payload.bin`: the kernel and the small image (`HAKIOS_RAMDISK=`) |
| `payload-flash.sh` | The payload into the last 96 MiB of the system partition, written and read back (the run cycle calls it) |
| `system-flash.sh`, `mksparse.py` | A system image into its partition, straight from the host, in sparse pieces fastboot takes |
| `boot-xnu.sh` | Asks the loader for one XNU boot from Android (`HAKIOS:BOOT-XNU` in pmsg) and reboots |
| `image.sh` | The images of a run with more than the plain boot of the earlier system (`wifi`, `bluetooth`, `metal`) |
| `msdc-test.sh`, `touch-probe.sh`, `gpu-probe.sh`, `cam-probe.sh` | The loader's probes ([docs/porting.md](../docs/porting.md)): each a flash, one boot and a report; read only |

## `log/`: what a run left

| Tool | What it does |
| --- | --- |
| `fbshot.py`, `fbkept.py`, `fbjpg.py`, `fbframes.py`, `fbrecv.py` | Pictures of the panel: from the log (`fbshot=`), from the host's kept lines (`fbshot_keep`), from a job list's `fbgrab -keep NAME`, from the dump records (`fbframes=`), sent over the network |
| `frames.py`, `latency.py` | How evenly a run's frames came and what held up the slow ones; how far behind a finger the screen is |
| `recording.py`, `sound.py` | A video of a screen recording (`rec_secs=`); a WAV of a sound recording (`rec_sound=`) |
| `texshot.py`, `passpipes.py` | Textures read back by the Metal device (`rwm_texshot=`); the frame log of `rwm_passlog` with each pass's pipelines |
| `camraw.py`, `camthumb.py` | Pictures of the frames the camera driver left in memory, and of the thumbnail it logs |
