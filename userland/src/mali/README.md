# hakios-mali

The beginning of a Metal back end for the X6826B's Mali-G52 (Bifrost, architecture v7) that writes the GPU's job
descriptors itself: no Gallium, no Panfrost driver, no Mesa at run time. It talks to the kernel's own GPU service,
HakiosMali (`kernel/overlay/iokit/Kernel/HakiosMali.cpp`, brought up by `bsd/dev/mtk_mali.c` with boot-arg `mali=1`),
in the requests of Linux's panfrost DRM driver: buffers made, given a GPU address and mapped, job chains submitted to
the fragment slot or the other, waits by the job's number. It follows the method of `metal/` in xnu-kernel-qcom, which
drives the Adreno the same way: every descriptor checked on this machine against a fake GPU, every refusal a sentence
that names what is wrong, and a probe that measures on the phone what the fake cannot.

## What there is

| Part | What it does | Checked by |
|---|---|---|
| `v7.h`, `v7.c` | The descriptors: Job Header, Write Value job, Fragment job, Framebuffer (Parameters, Padding), Render Target, Tiler Context and Heap, the sample positions, the tile size and the clear colour. Packed and unpacked by Mesa's own `genxml/v7_pack.h`, which the build makes from Mesa's `v7.xml` with Mesa's `gen_pack.py`: no field position is written by hand. What each one is filled with is what `pan_desc.c` (`GENX(pan_emit_fbd)`, `pan_prepare_rt`, `GENX(pan_select_tile_size)`), `pan_jm.c` (`GENX(jm_emit_fragment_job)`, `jm_emit_tiler_desc`), `pan_clear.c` and `pan_samples.c` write for the same frame. The readers refuse a bit outside every field with the generated unpacker's own sentence. | `tests/host_test.cpp`: every word of the 64x64 RGBA8 frame against literals worked out by hand from `v7.xml`, field by field |
| `mali.hpp`, `gpu.cpp` | `Gpu` and `Bo`: parameters, buffers (create, GPU address, mapping, close), submissions with the buffers they use, waits (HakiosMali's request 0x7d), the names of the GPU's exception codes | the fake GPU; on the phone, `mali-probe` stages `buffer` and `write-value` |
| `iokit_transport.cpp` | the only part that knows IOKit: HakiosMali's user client, its client memory per handle | the phone |
| `pass.hpp`, `pass.cpp` | `Device` (the GPU checked to be a v7 model of `pan_props.c`'s table -- the phone's is 0x7402, G52 r1; the sample positions; the tiler heap), `ClearPass` (one colour target, load action Clear, nothing drawn: one Fragment job over a Framebuffer Descriptor and its Render Target, the frame Panfrost submits for a clear-only batch -- on v7 without a Tiler Context, which `jm_emit_tiler_desc` makes only for a draw; optionally with one), `WriteValue` | the fake GPU, which carries the frame out; the refusals by sentence; on the phone, `mali-probe` |
| `fake_gpu.hpp`, `fake_gpu.cpp` | HakiosMali's requests answered in the process and the job chains run as strictly as the Mali takes them: every job through Mesa's unpackers, a job outside every buffer or on the wrong slot, a framebuffer pointer whose tag disagrees with its descriptor, tiles past the frame (TILE_RANGE_FAULT), a target's rows outside its buffer, an effective tile size or colour buffer allocation the hardware does not take -- each stops the chain with its sentence and the Exception Status the GPU would write. It carries out Write Value and the cleared frame (only for targets with Clean Pixel Write Enable: a tile nothing was drawn into is "clean", and the GPU does not write it back otherwise) | itself, by `host_test`'s fifteen mutation twins: an accepted frame changed in one place in GPU memory, each refused with its sentence and its code |
| `probe.cpp` | `mali-probe`: the stages below against HakiosMali (or `--fake`), each a line `mali-probe: STAGE PASS|FAIL ...` on the console and the same with `!!` through `kern.hakios.say`, kept for the host | the phone |

| `compiler.h`, `compiler.c` | The compiler inside the driver: the library's programs (`tri`, `color`: a varying and a pushed uniform, `tex`: a texture at a varying) and kernels (`fill`) built as NIR and compiled in the process by Mesa's Bifrost compiler (`bifrost_preprocess_nir`, `GENX(pan_shader_compile)`, no IDVS), with what Panfrost prepares for them: Renderer States (`pan_shader_prepare_rsd`; the fragment shader's merged as `panfrost_emit_frag_shader` merges it, without depth, with Metal's Less and writes, and for a blend that reads the destination), Blend descriptors (`panfrost_emit_blend`: off and source-over, RGBA8 and BGRA8), the words each stage pushes, the varyings to link. The viewport transform's scale and offset are system values in uniform buffer 1, pushed as Panfrost pushes its own | on the phone, stage `compiler`: compiled there, word for word the Mac's |
| `program.h`, `shaders_v7.h`, `tools/bishaders.c`, `tools/mk-shaders.sh` | The same programs compiled on the Mac (`mk-shaders.sh MESA_HOST_BUILD=...`) and written out, for a phone build without the compiler and for the comparison | the stages below |
| `draw.hpp`, `draw.cpp` | `DrawPass`: any program into a target of any size, RGBA8 or BGRA8, as Panfrost submits a batch on v7: one vertex and tiler job pair per draw (`makeMany`: up to 16, chained as `pan_jc_add_job` chains them, all into one Tiler Context), then the Fragment job; attributes (`panfrost_emit_vertex_data`), varyings linked (`panfrost_emit_varying_descs`), pushed words and the uniform buffer table (`panfrost_emit_const_buf`, `panfrost_emit_ubo`), a texture and sampler (`GENX(panfrost_new_texture)`, `panfrost_create_sampler_state`), 16-bit indices (`jm_emit_primitive`), blending, the depth test in the tile buffer. `ComputePass`: one dispatch as `GENX(jm_launch_grid)` submits it (`panfrost_pack_work_groups_compute`) | the fake GPU checks every job and address; on the phone, the draw and compute stages |
| `compiler.c` `hm_compile_air_kernel`, `tools/airkernels.c` | Apple's compute functions: a function of a `.metallib` through the AIR frontend (`userland/tools/mesa/air`), its device buffers made global memory at addresses pushed from uniform buffer 15, the grid's sizes system values there too, threadgroup memory as workgroup storage, then Bifrost | stage `apple-kernels` |
| `metal.hpp`, `metal.cpp` | Metal's command model over the passes: `Texture`, `RenderPipeline`, `ComputePipeline`, `CommandBuffer` with render command encoders and dispatches, commit in order, `waitUntilCompleted`, Metal's statuses (an Error naming the first job not DONE); `Gpu::surfaceBo`: an IOSurface's memory as a target (request 0x7e), a drawable's shape | stages `metal-api`, `iosurface` |

Not yet, refused by name where it would be asked for: the `MTLDevice` plug-in itself, Apple's vertex and fragment
functions and its kernels' textures and samplers, stencil, depth written to memory, MSAA, mipmaps, AFBC and tiled
images, formats other than RGBA8Unorm and BGRA8Unorm, sRGB, several colour targets, load action Load, compute with
thread storage (spilling), presenting a drawable.

## What the phone said

Runs 500 to 502, the 27 release with `mali=1 metal=1`, `mali-probe` as launchd's first job (`userland/jobs/rc.jobs.maliprobe`):

- The GPU is a **Mali-G52 r1**: product 0x7402 (not 0x7212, which run 500's probe asked for and refused by name), r1p0,
  two shader cores, `TILER_FEATURES` 0x209 (2^9-byte bins, **two** hierarchy levels), `THREAD_TLS_ALLOC` 0x400.
- `clear64`, the first milestone: one Fragment job of ours cleared the 64x64 RGBA8 target, 4096 of 4096 pixels
  `33 66 99 cc`, every canary byte after it kept, the job DONE (runs 501 and 502).
- Also as the fake GPU says: Write Value, a frame with a Tiler Context, 48x20 at an offset with padded rows (nothing
  of the padding written), BGRA8, the panel's 750x1624 (about 1 ms), no write without Clean Pixel Write Enable;
  a 64x64 clear and its wait take about 90 us.
- Where the fake GPU was wrong, and was changed: the Fragment job's tile bounds do **not** limit what is written (a job
  over tiles (1,1)-(2,2) wrote the whole frame); the framebuffer's Bound Min/Max does, to the pixel, also off the tile
  grid ((5,7)-(40,50)).
- `triangle` (run 502), the first draw: vertex, tiler and fragment jobs DONE, 420 of 420 pixels more than a pixel
  inside red, 3304 of 3304 more than a pixel outside the black clear, the red where the viewport puts it (wide at row
  20, the apex at row 48), the vertex job's position for vertex 0 (16, 16, 0.5, 1).

- Runs 507 to 509: `varyings` (red, green and blue interpolated across the triangle), `uniform` (times a pushed tint),
  `blend` (source-over) all with a largest difference of 0; `texture` 3136 of 3136 pixels the texel their uv names;
  `compute` 64 of 64 words; `depth` the near triangle's colour where two overlap, though drawn first; targets of
  160x96 and the panel's 750x1624 (150210 of 150210 pixels within 1/255), BGRA8, an indexed quad; four draws in one pass
  with the tile buffer's depth kept across them; and `compiler`: the programs compiled on the phone, word for word the
  Mac's, in 33 ms, and drawn with.
- Runs 510 to 512: `metal-api` (one command buffer of Metal's model: a textured quad and a source-over triangle into a
  BGRA8 texture, an empty pass, a dispatch, Completed in 152 us, every result right), `iosurface` (a draw into an
  IOSurface made with IOSurfaceCreate, read back through IOSurface's own lock), `shared` (256 of 256 words read across
  a barrier from threadgroup memory), and Apple's own kernels: 16 of the 36 compute functions of the 27 release's
  PortraitFilters compiled on the phone through the AIR frontend (`hm_compile_air_kernel`), and three of them
  dispatched, each writing exactly what its NIR says. 36 of 36 stages.
- Run 513: `blit` (the `copy` kernel: 200 of 200 words, the canary after them kept), `lines` (two lines of width 1:
  58 of 58 pixels judged right, 64 drawn in all, 32 a line, nothing else) and `points` (four of size 1: exactly their
  four pixels). 39 of 39 stages; **capabilities 23 of 37 measured (62%)**.
- Run 515: `texture-linear` (bilinear exactly: largest difference 0), `render-to-texture` (4096 of 4096), `stencil`
  (420 of 420 inside the marked triangle, nothing outside), `viewport-scissor`, `formats` (R8 and RG8 targets and
  textures), `mipmaps` (each level where its LOD puts it, and the LOD clamp), `blit-texture` (777 of 777 texels, nothing
  outside touched), `generate-mipmaps` (every texel of five levels the 2x2 average of the level before within 1), and
  **`apple-render`: Apple's own render pipeline, ARKitCore's `resample_v` and `resample_f`, compiled on the phone from
  the `.metallib` by the AIR frontend and Bifrost in 3.4 ms and drawn right, every pixel**. `cull` showed the hardware's
  winding is the one with row 0 at the top (here NDC y -1 is row 0, so the API's flag is turned over: draw.cpp), and
  `instanced` was right but judged with a threshold the four small triangles do not reach (160 pixels); both changed.
  48 of 50 stages; **capabilities 31 of 37 measured (83%)**.
- Run 516: `cull` with the winding turned over (none: both, back+CCW: A, back+CW: B, front+CCW: B), `instanced`
  (four instances of a triangle through the NPOT divisor, three of a line through the POT one), `vertex-descriptor`
  (Float2 and UChar4Normalized out of one 12-byte interleaved buffer), `argument-buffer` (a kernel following the GPU
  addresses held in a buffer, as Metal 3's argument buffers hold them: no function of the release's libraries has an
  argument buffer the AIR frontend reports, `airrender --argbufs`), `msaa` (four samples resolved on writeback: 64
  edge pixels in exact quarters, 16 + 32 + 16, none with one sample), and `apple-render` again. 53 of 53 stages;
  **capabilities 35 of 37 measured (94%)**. Left: presenting to the panel through the window server, and the
  `MTLDevice` plug-in of Metal.framework itself.

## Capabilities: how far along

A fixed list of 37 things a Metal backend for this GPU has to do, the same list in `probe.cpp` (`capabilities()`).
One counts only when every `mali-probe` stage named for it said PASS on the phone in the same run; the probe prints
`capabilities N of 37 measured` at the end. A count, not a weight: the last ones (Apple's render functions, the
`MTLDevice` plug-in, presenting) are the largest pieces of work.

| # | Capability | Stages |
|---|---|---|
| 1 | the GPU opened and identified | `open` `params` `device` |
| 2 | GPU buffers: create, map, address | `buffer` |
| 3 | job submission and completion | `write-value` |
| 4 | render pass: load Clear, store | `clear64` `clear64-tiler` `clear64-noclean` |
| 5 | targets of any size, to 750x1624 | `clear-48x20` `clear-750x1624` `varyings-160x96` `varyings-750x1624` |
| 6 | BGRA8 targets | `clear64-bgra` `varyings-bgra` |
| 7 | vertex and fragment shaders: a draw | `triangle` |
| 8 | vertex attributes and varyings | `varyings` |
| 9 | uniform buffers (setVertexBytes) | `uniform` |
| 10 | blending, source over | `blend` |
| 11 | indexed draws, 16-bit | `indexed` |
| 12 | several draws in a pass | `multidraw` |
| 13 | textures sampled, nearest | `texture` |
| 14 | compute dispatch | `compute` |
| 15 | threadgroup memory and barriers | `shared` |
| 16 | depth test and write | `depth` `depth-off` |
| 17 | the shader compiler on the device | `compiler` `compiler-draw` |
| 18 | command buffers, encoders, statuses | `metal-api` |
| 19 | an IOSurface as a drawable | `iosurface` |
| 20 | Apple metallib compute kernels | `apple-image_calcminmax0` `apple-MetalFaceMask_InitMinMax` `apple-MetalFaceMask_InitSpanTable` |
| 21 | blit: buffer to buffer copy | `blit` (the `copy` kernel) |
| 22 | primitive: lines | `lines` |
| 23 | primitive: points | `points` |
| 24 | textures sampled, linear | `texture-linear` |
| 25 | mipmaps | `mipmaps` |
| 26 | more pixel formats: R8, RG8 | `formats` |
| 27 | render to a texture, then sample it | `render-to-texture` |
| 28 | multisampling | `msaa` |
| 29 | stencil | `stencil` |
| 30 | scissor and viewport from the API | `viewport-scissor` |
| 31 | culling and winding | `cull` |
| 32 | instanced draws | `instanced` |
| 33 | vertex descriptors and argument buffers | `vertex-descriptor` `argument-buffer` |
| 34 | Apple vertex and fragment functions (AIR) | `apple-render` (ARKitCore's `resample_v` + `resample_f`) |
| 35 | blit: textures, generateMipmaps | `blit-texture` `generate-mipmaps` |
| 36 | presenting to the panel (CAMetalLayer) | -- |
| 37 | Metal.framework's MTLDevice on this backend | -- |

## mali-probe's stages

1. `open`: HakiosMali's user client (waits up to 60 s for the service: mtk_mali.c starts it once the GPU is up).
2. `params`: the GPU's product (0x7212 for the G52), revision, shader cores, TILER_FEATURES (hierarchy levels), TLS.
3. `device`: the same checked, the sample positions uploaded.
4. `buffer`: a 64 KB buffer, its GPU address below 4 GB, every word written and read back through the mapping.
5. `write-value`: the smallest job, a 32-bit write, and the job's Exception Status DONE (1).
6. `clear64` **(the first milestone)**: one Fragment job clears a 64x64 RGBA8Unorm target to (0.2, 0.4, 0.6, 0.8),
   which is 33 66 99 cc in memory; every pixel compared, every other byte of the buffer a canary that must stay.
7. `clear64-tiler`: the same frame with a Tiler Context and heap, as a frame with draws has them.
8. `clear-48x20`: a target whose size is not a whole number of tiles, at an offset, with row padding (the GPU writes
   up to the frame's width and height only, and nothing of the padding).
9. `clear64-bgra`: BGRA8Unorm, the render target's swizzle (B first in memory).
10. `clear64-tiles`: a Fragment job over tiles (1,1)-(2,2) only: the whole frame is written all the same.
11. `clear64-fbbounds`, `clear64-both`, `clear64-fbbounds-odd`: the framebuffer's Bound Min/Max, alone, with the
    job's tiles, and off the tile grid: what is written is exactly those pixels.
12. `clear-750x1624`: a frame of the panel's size.
13. `clear64-noclean`: Clean Pixel Write Enable off on a frame without primitives: nothing written.
14. `triangle`: the first draw (`DrawPass` with the `tri` program), judged pixel by pixel away from the edges; the target is also printed
    as `mali-picture` lines (`tools/probe-picture.py LOG OUT.png triangle`).
15. `timing`: twenty clears of 64x64, a submission and its wait each.

The expected pixel is computed in the probe from the colour alone (round to nearest even of c x 255, as `pan_clear.c`
rounds), not by the library.

## Building and running

On this machine (macOS or Linux), against the fake GPU:

```sh
make -C userland/src/mali test MESA=$PWD/kernel/deps/build/mesa/mesa-25.0.7
```

For the phone (arm64e, the 27 release), then the small image with the probe and a job list that runs it:

```sh
darwin/mtk27.sh mali                        # darwin/userland/mali27.sh: build/ios27/obj/mali/mali-probe
RC_JOBS=userland/jobs/rc.jobs.maliprobe darwin/mtk27.sh root
```

`root26-sys.sh` puts `mali-probe` into `/hakios/bin` when `MALI27` names its directory (`mtk27.sh root` does so when
the `mali` step has run). Booted with `mali=1`, the job list runs it as soon as HakiosMali is there and again 90 s
later with `--dump` (every frame's descriptors printed by Mesa's printers). Its verdicts are in the run's log, and
the `!!mali-probe:` lines at its end, in what the kernel kept for the host.
