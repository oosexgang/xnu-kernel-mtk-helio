# Metal on the Mali, and the glass of Darwin 27

What exists for Metal on the Mali-G52 of the first chip (MT6768), how the window server's frames reach
the GPU, what has been measured, and what still stands between the 27 release and its glass drawn at
the panel's rate. Run by run, the story is [darwin27-boot.md](darwin27-boot.md); words are in
[glossary.md](glossary.md).

## The path of a frame

```
backboardd (CoreAnimation, the window server)
  └─ Metal.framework, which finds our device by the two properties the kernel publishes (metal=1)
       └─ HakiosMetal.bundle: userland/src/hakiosmetal.m + userland/src/metal/   the MTLDevice, its encoders
            └─ air_metal.h's contract (rwa_*), served by one of two libraries:
                 ├─ libhakiosmali.dylib (mali_native=1): userland/src/mali/rwa/, our own job descriptors
                 │    └─ kmali.c ── HakiosMali's user client ── kernel: drivers/iokit/HakiosMali.cpp, drivers/board/mtk_mali.c
                 └─ libhakiosair.dylib: Mesa's Gallium and panfrost (the first path, kept for comparison)
```

- **The device.** `hakiosmetal.m` is the plug-in Metal loads: an `MTLDevice` with its command queues,
  buffers, textures, pipelines and encoders, answering what CoreAnimation and a few other system
  processes ask. It knows nothing of the GPU: everything goes through `air_metal.h`'s functions.
- **The shaders.** A pipeline's functions are Apple's own, from the 27 release's `.metallib` files.
  The AIR front end (`userland/tools/mesa/air/`, [userland/docs/air.md](../userland/docs/air.md))
  translates their bitcode to NIR, and Mesa's Bifrost compiler makes Mali code of it. The function
  constants and the generic pipelines' configuration are folded while translating, so a stage is
  compiled for exactly one combination.
- **The native library.** `rwa_mali.c` writes the GPU's job descriptors itself, as Panfrost would for
  the same work: a batch of draws into the same targets is one tiler context, one framebuffer
  descriptor, one fragment job; a target that is not cleared is loaded into the tile buffer first by
  a frame shader. `rwa_shader.c` compiles and caches stages; `kmali.c` owns GPU memory and the
  submissions to the kernel.
- **The kernel.** `mtk_mali.c` powers the GPU through MFG's domains and clocks it (`mali_mhz=`);
  `HakiosMali.cpp` gives user space buffers, submissions to the two job slots, and the memory of an
  IOSurface as a target.

## The stage cache

Compiling one of CoreAnimation's uber shaders took one to eight seconds on the phone, and a window
server that compiles in the middle of an animation drops it. So the stages are compiled on the Mac
beforehand, with the same front end and the same Bifrost compiler, and the image carries them:

| Step | What does it |
| --- | --- |
| A run says which pipelines its processes built (`rwr` lines, the recipes) and which configurations of the generic pipelines it used | the plug-in, `notify com.hakios.rwm_recipes`; boot-arg `rwm_capture_gen=1` |
| The recipes and variants are harvested into `userland/share/rwm-recipes27.txt` and `rwm-variants27.txt` | `userland/tools/rwm-recipes.py`, `rwm-variants.py` |
| Every recipe and variant is compiled on the Mac into `build/rwa-cache.bin` | `userland/tools/rwacache.sh` (rwacheck, rwagen over the library built for the Mac) |
| The image takes the cache as `/hakios/share/rwa-cache.bin`; the library looks a stage up by its key before compiling | `RWA_CACHE_BIN=` of `darwin/mtk27.sh root` |

A key is the GPU, the bitcode, the function, its constants, the configuration and the part of the
pipeline's shape a compile reads. `rwn_keylog=1` names, on the phone, every stage that was not in the
cache. Since the front end folds what the static initializer stores (`a135b47`), a stage the cache
misses costs about 60 ms on the phone instead of a second or more, and a run with no cache at all
draws the same screens.

## What has been measured

| | Seen |
| --- | --- |
| The home screen | Drawn whole by the native library, no job chain failed (run 531). Without the live wallpaper (`sb_wallpaper=0` left out of the arguments): a full pass about 7 ms on the GPU, page swipes 25 to 29 frames a second (run 583). |
| Notification shade, Control Center | 25 to 36 command buffers a second (runs 588 to 596). |
| Opening an application | The first frames are 40 to 50 ms of full-screen passes with several targets: bound by the GPU. Compiles no longer stall it (runs 666 to 669). |
| Memory | Free memory is tight (about 30 MB free, 1.4 GB available); the library's caches are kept small on purpose. |
| Faults | None since `89c23a5` (a compute dispatch with dynamic threadgroup memory was refused instead of run without it). |

How it is measured: every 30 s the library says, in lines that begin `rwn backboardd: 30s:`, what the
GPU was busy with (`GPU busy N%`), the command buffers of each second, the pipelines made, the stage
cache's hits and misses, the compile's three phases in milliseconds, and the passes by kind with
their vertex, tiler and fragment time. `tools/log/frames.py` turns a run's log into how evenly frames
came; `rwm_slowcb=MS` names every command buffer slower than that.

## What is still open

- **The glass.** The 27 release's glass (the dock, Search, folders, menus) is drawn over a blur the
  window server makes of the screen beneath. It is drawn, but the blur behind a menu comes out flat
  green and white instead of the colours under it: not explained yet.
- **Openings are GPU-bound.** CoreAnimation renders an opening's offscreen passes into RGBA16Float
  targets, several at once; the full-screen multi-target passes are most of the time. Next: why the
  float targets (bandwidth), the blur passes, and which passes could be smaller.
- **The whole screen every time.** CoreAnimation repaints the whole panel for any change here (no
  damaged regions reach it), so even a clock's second hand cost a full frame (the Clock icon's hand
  is stopped for that, `9551f48`).
- **CPU starvation.** Widget extensions on the home screen (Stocks, Weather) can hold one or two cores
  for minutes; RunningBoard's watchdog kills and relaunches one of them in a loop.

## The boot arguments

The kernel's (`Documentation/boot-args.md`): `mali=1` (the GPU), `mali_mhz=N` (its clock; 950 is the fastest
used), `metal=1` (the device published to Metal), `camtl=1` (the hooks that let CoreAnimation take a
Metal device), `fb_queue=1 fb_flip=1` (frames shown by flipping OVL0 between two pictures).

The library's, read from the same boot arguments in user space:

| Argument | What it does |
| --- | --- |
| `mali_native=1` | the native library instead of Gallium's panfrost |
| `rwn_keylog=1` | every stage compiled on the phone named with its key (what the cache missed) |
| `rwn_profile=1`, `2`, `3` | pass and draw times by kind, more detail with each level |
| `rwn_smalllog=0` | the small passes' lines left out of the log |
| `rwn_texprobe=0` | no texture probes (they read GPU memory) |
| `rwn_keepfps=1` | the per-second counts kept for the host, as kept lines (`!!`) |
| `rwn_refuse=1` | the native library refuses the GPU, and the Metal device takes Mesa's panfrost in its place: the fallback of a Mali the library does not serve, seen on one it does ([metal-porting.md](metal-porting.md)) |
| `rwn_cache=0` | the stage cache not used |
| `rwn_gpumips=0` | mipmaps made by the processor, not the GPU |
| `rwm_slowcb=MS` | a command buffer slower than MS named, with what it did |
| `rwm_capture_gen=1` | the generic pipelines' configurations said, for `rwm-variants.py` |
| `rwm_recipes=1` | recipes to the host's kept lines as well (they are big) |
| `rwm_cblimit=N` | how many command buffers may be alive before a new one waits (32) |

## How to try it

```sh
darwin/mtk27.sh mali                         # the plug-in's native library and hakios-mali
userland/tools/rwacache.sh build/rwa-cache.bin
RWA_CACHE_BIN=$PWD/build/rwa-cache.bin RC_JOBS=$PWD/userland/jobs/rc.jobs.nativeui darwin/mtk27.sh root
```

and a run with `mali=1 metal=1 camtl=1 fb_queue=1 fb_flip=1 mali_native=1 mali_mhz=950` among its
arguments ([building.md](building.md)). The job lists `userland/jobs/rc.jobs.native*` move the
interface by `rwtouch` gestures and take pictures with `fbgrab`, so a run says what it showed without
anybody at the phone.
