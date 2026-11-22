# User space

Our own programs and libraries for the Darwin user space, and the tools they are made with. The
system's own programs are used as their maker ships them; these are what is around them: process 1
before launchd, the libraries inserted into a few of the system's processes, the Metal device and the
native Mali library, the audio plug-in, test programs, and the job lists a run follows.

`build.sh` builds all of it for the earlier system (20F66) and makes its two images: `root.apfs`, the
small image that travels with the kernel, and `system.apfs`, that system's own image. Its `root/` is
also where the 27 release's small image starts: `darwin/mtk27.sh` builds the same sources for the 27
release's arm64e processes ([darwin/README.md](../darwin/README.md)).

## What is here

| Path | What it is |
| --- | --- |
| `../init/` | The first programs, which `build.sh` compiles: `stage0.c`, `/sbin/launchd` of the system image, mounts the small image on `/hakios` and runs our `init.c`: mounts, checks, the data volume made from its skeleton, the overlays and the job cache bound over the system's, the libraries to insert (`etc/inject.conf`), then `etc/rc`; `child.c`, `var_store.c` |
| `src/inject*.c`, `src/tweaks/` | Libraries the kernel inserts into single processes (`kern.exec_inject`): SpringBoard, backboardd, media, applications, key chain, tweaks. Each checks a method's type before it replaces it |
| `src/hakiosmetal.m`, `src/metal/` | The Metal device: `HakiosMetal.bundle`, an `MTLDevice` over `air_metal.h`'s contract ([Documentation/metal-glass.md](../Documentation/metal-glass.md)) |
| `src/mali/` | The native Mali library: `rwa/` (the contract served with our own job descriptors), `kmali.c` (GPU memory and submissions), `mali-probe` and its host test ([src/mali/README.md](src/mali/README.md)) |
| `src/eagl.c`, `src/gl_*.c`, `src/hakios_gl.h` | OpenGL ES for the earlier system's CoreAnimation, over Mesa |
| `src/audio_plugin.c`, `src/camera_*.c` | The audio output device; the camera's device and image pipeline |
| `src/rwtouch.c`, `src/fbgrab.m`, `src/cputop.c`, `src/psgrep.c`, `src/tolog.c`, `src/svclook.c` | The run's own tools on the phone: gestures, pictures of the panel, the processors' use, processes and their stacks, a program's output into the log, a Mach service looked up |
| `src/*_test.c`, `src/*_test.m` | Test programs, each run by a job list |
| `../lib/libmini/` | A C runtime of our own (system call wrappers, strings, a small printf), for the first programs, which run without the system's libraries |
| `jobs/` | The job lists: what `com.hakios.demo` runs, one command a line (`rc.jobs`, the default, and the lists of particular runs) |
| `launchd/` | Our launchd jobs (`com.hakios.demo`, `.varstore`, `.wifi`, `.nullcomm`) |
| `rc.launchd`, `rc.launchd.*`, `rc.system` | `etc/rc`: what `init` runs; `rc.launchd` hands process 1 to the system's launchd |
| `share/` | `rwm-recipes27.txt`, `rwm-variants27.txt`: the pipelines the window server builds, harvested from runs, for the stage cache |
| `var/` | Preferences of the data volume a run starts with |
| `mkjobs.py`, `jobinfo.py`, `mkeagl.py` | The system's launchd jobs made from its own plists; what a job is; the GL entry points |
| `tools/` | The user space's tools, below |
| `docs/` | The shader front end ([air.md](docs/air.md), [air-report.md](docs/air-report.md)) and the earlier system's Metal analysis ([metal.md](docs/metal.md), in Russian) |

## `tools/`

| Tool | What it does |
| --- | --- |
| `mesa/` | Mesa's patches for this port, the AIR front end (`mesa/air/`, copied into Mesa's tree as `src/compiler/air`) and the DRM shim |
| `mk-mesa.sh` | Mesa built for the phone (`MESA_ARCH=arm64e` for the 27 release) |
| `air/` | The front end's checks on the Mac: the reader against `llvm-dis`, the corpus report, the CPU reference, the 27 release's replays |
| `rwacache.sh`, `rwm-recipes.py`, `rwm-variants.py` | The stage cache: recipes and variants harvested from a run's log, every one compiled on the Mac into `build/rwa-cache.bin` |
| `metallib.py`, `qc-metallib.py`, `qc-simlib.py` | A `.metallib` taken apart; QuartzCore's shaders in AIR made from another build's library |
| `render-ca-wallpaper.m` | A layered wallpaper rendered on the Mac |
| `apfs/` | The APFS image maker (`mkapfs.py`) and readers, with their tests |
| `dsc/`, `cache-name.py` | The shared cache read on the Mac; which image of it an address is in |
| `font_trim.py` | The two system fonts too large for an image cut down |
| `mk-jb.sh`, `jb-preinstall.py`, `ldid-codesign.sh` | The package manager's prefix (fetched into `jb/`, never here); packages put in as installed; `ldid` made of `codesign` |

## Building

```sh
userland/build.sh                                   # the earlier system's images; userland/root for the 27 release's
RC_JOBS=rc.jobs.unlock userland/build.sh            # a job list by its name in jobs/ (or by a path)
darwin/mtk27.sh mali && darwin/mtk27.sh root        # the 27 release's native library and small image
make -C userland/src/mali test MESA=$PWD/kernel/deps/build/mesa/mesa-25.0.7   # the Mali library against a fake GPU
```

`build.sh`'s head lists its switches (`GL=1`, `METAL=1`, `SYSTEM=1`, `RC=`, `RC_JOBS=`, ...). What it
builds is ignored by git (`obj/`, `root/`, the images).
