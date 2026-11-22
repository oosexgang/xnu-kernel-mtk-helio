# Metal on another MediaTek phone

Darwin 27 draws its interface through Metal, and Metal here is a stack of four parts. Three of them are the same on
every phone; one is the chip's, and is a table. This page says what each part needs, what a new phone has to give,
and how to see that each step worked. The words are in [glossary.md](glossary.md); where Metal stands on the first
phone is [metal-glass.md](metal-glass.md).

| Part | Where | Per phone? |
| --- | --- | --- |
| The GPU's platform: its clock, MFG's power domains, the bus protection, its clock gate | `drivers/board/mtk_mfg.h` | **The chip's**: a table, `struct mtk_mfg`, found by the SoC file's name |
| The Mali itself: reset, power of its cores, the MMU, the job slots, the requests of panfrost's interface | `drivers/board/mtk_mali.c`, `drivers/iokit/HakiosMali.cpp` | No: any Mali with a job manager (Bifrost, Valhall up to the G78) |
| The library that writes the GPU's work | `userland/src/mali/rwa/` (native), Mesa's panfrost (`libhakiosair`) | No: chosen by the GPU, below |
| The Metal device CoreAnimation talks to | `userland/src/hakiosmetal.m`, `userland/src/metal/` | No |

## Which GPU, and what draws it

The kernel reads the GPU's id at its start and says it in the log (`mali: GPU_ID ... (Mali-G52 r1p0, mt6768)`). What
happens next depends on its architecture:

| GPU | Helio | Kernel | What draws Metal |
| --- | --- | --- | --- |
| Mali-G52, G76 (Bifrost v7) | P65, G70, G80, G85, G88; G90, G95 | driven | The native library (`mali_native=1`), the fastest: written for this GPU |
| Mali-G71, G72 (Bifrost v6) | P23, P30; P60, P70 | driven, not run yet | Mesa's panfrost: the native library refuses the GPU and the Metal device takes the Gallium library in its place, by itself |
| Mali-G57 (Valhall v9, with a job manager) | G96, G99 | driven, not run yet | Mesa's panfrost, as above |
| Mali-T860, T880 (Midgard) | P10, P20, P25, X20 to X27 | **refused**: its MMU takes only the older table format, and this driver writes AArch64 tables | nothing yet |
| Mali-G310 and after (a command stream frontend) | | **refused**: firmware of its own and another way to submit | nothing yet |
| PowerVR | A22, P22, P35, G25, G35, G37, P90, P95 | no driver | nothing: Mesa has no driver for it, and its software rasterizer cannot draw into the window server's surfaces |

"Not run yet" means: compiled, and the same code as on the G52, but no phone with that GPU has been run. The first
run of one is a [phone_first](../arch/arm64/configs/phone_first_defconfig) of its own (step 3 below).

The fallback was run on the first phone with `rwn_refuse=1` (run 673): the system up on Mesa's panfrost, the job
list to its end, 22618 GPU job chains and none failed; slower than the native library. It is said in the log, as a
kept line: `rwm backboardd: the native Mali library does not serve
this GPU (...): Mesa's panfrost in its place`. To see it on a phone whose GPU the native library does serve, add
`rwn_refuse=1` to the run's arguments.

## Bringing a new chip's GPU up

1. **The SoC file's blocks.** `topckgen`, `infracfg_ao`, `spm`, `mfgcfg` and `mali` in
   `arch/arm64/boot/dts/mediatek/SOC.toml`, from the phone's Android device tree (`tools/boards.py new --dtb`, or by
   hand with `tools/fdtfind.py`), and the feature `gpu-mali-g52` in the board file (the feature is named after the
   first GPU it drove; it means "a Mali in MFG").
2. **The chip's table in `drivers/board/mtk_mfg.h`.** From the chip's Android kernel:
   - `mux_*`: topckgen's `CLK_CFG_n_CLR` and `_SET` of `mfg_sel` (its clock driver, `clk-mt67xx.c`), and a source that
     runs without a PLL of its own (a syspll divider);
   - `domains`: MFG's power domains in their order, each with its `PWR_CON` offset in the SPM, its bit in
     `PWR_STATUS`, its SRAM bits, and the bus protection released after it (its scpsys or spm driver's MFG entries,
     the `infracfg` `..._PROTECTEN_CLR` masks);
   - `pwr_status`, `pwr_status_2nd`: the SPM's two status registers;
   - `cg_*`: `MFG_CG_CLR` and BG3D's bit in mfgcfg.

   A chip whose GPU does not come up by these steps (a rail to raise first, a GPU with a microcontroller in front)
   gives `power_on`, a function, instead. `workbench/mfg_power_test.c` checks every table's shape; add the new chip
   to it where it can say more.
3. **A kernel run.** (The loader's GPU probe, `tools/run/gpu-probe.sh`, does the same steps with nothing of the kernel
   around them and returns to Android with what it read, which is the safer first look; but its numbers in
   `arch/arm64/boot/mtkmali.h` are still the MT6768's written out, so for another chip they are changed there too.)
   Then a kernel run with
   `XNU_DEFCONFIG=phone_first XNU_ARGS="cpus=1 mali=1"`: `sysctl kern.hakios.mali` (and the log) says how far it
   came:

   | `kern.hakios.mali` | Means |
   | --- | --- |
   | 0 | Not started: `mali=1` missing, the board does not list the feature, or the chip has no table (the log says which) |
   | 1 | The clock was set, and a power domain did not come up (the log names it and PWR_STATUS) |
   | 3 | Powered and gated on; the GPU then did not reset, or is one this driver refuses (named in the log) |
   | 4 | Its cores powered: `mali: GPU_ID ...: all powered` |
   | 6 | A job ran through the GPU's own page tables: ready for programs |

4. **Darwin 27 on it**: `XNU_DEFCONFIG=phone_darwin27`, with the board's own arguments where they differ. Leave out
   `mtk_cpufreq=` and `mali_mhz=` until the chip has its own clock tables: on a chip that is not the MT6768 both are
   refused, and said so, rather than written with the MT6768's numbers.
5. **The stage cache** ([building.md](building.md)) is compiled for the GPU it was built for; on another GPU leave
   `RWA_CACHE_BIN` out of the `root` step, and the window server's stages are compiled on the phone (about 60 ms
   each with the native library).

## What is still one chip's

- The GPU's clock above 364 MHz (`mali_mhz=`, `drivers/board/mtk_keys.c` `gpu_raise`): the MT6768's GPUPLL and the
  MT6358's rails. Another chip's clock is its gpufreq driver's table.
- The native library's tile sizes come from Panfrost's model table and the tiler's levels from the GPU's own
  registers; nothing in it names a chip. It was measured on the G52 only.
