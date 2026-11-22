# Documentation

In Russian, to start with: [quickstart.ru.md](quickstart.ru.md) (from a clone to a first run, in a page),
[phone-test.ru.md](phone-test.ru.md) (a day with the phone: the runs in order, and what success looks like in the
log), [whats-new.ru.md](whats-new.ru.md) (what changed lately, by who it matters to),
[darwin27.ru.md](darwin27.ru.md) (from a Mac to Darwin 27, step by step).

| Document | What it answers |
| --- | --- |
| [layout.md](layout.md) | Where everything is: each directory, and which directory of XNU's tree a kernel directory of ours is. |
| [architecture.md](architecture.md) | How the boot chain, the kernel, the loader and Darwin's user space fit together; what is data, what is a driver, what is the same for every device; what is not in the repository. |
| [porting.md](porting.md) | How to bring the project up on another MediaTek phone, step by step, with the check for each step. |
| [building.md](building.md) | From a clone to Darwin 27's home screen: every command, what each needs, what a good run's log shows. |
| [darwin27.ru.md](darwin27.ru.md) | In Russian: what the project has done, and from a Mac to Darwin 27 on a phone, step by step. |
| [quickstart.ru.md](quickstart.ru.md), [phone-test.ru.md](phone-test.ru.md), [whats-new.ru.md](whats-new.ru.md) | In Russian: the shortest way to a first run; the runs of a day with the phone, in order; what is new. |
| [drivers.md](drivers.md) | Every driver: its part, its feature and boot argument, how tied it is to one phone. |
| [chipsets.md](chipsets.md) | What a MediaTek chip gives the drivers, what is still one chip's in them, and how to bring another chip up. |
| [features.md](features.md) | Every feature a board can list: the blocks and sections it needs first, and what starts it. Generated. |
| [boot-args.md](boot-args.md) | Every boot argument the kernel reads, with the comment beside its read. Generated. |
| [writing-a-driver.md](writing-a-driver.md) | How a driver is written here: the rules and why, what a driver has at hand, the steps, with an example driver and its test that `make check` builds and runs. |
| [metal-glass.md](metal-glass.md) | Metal on the Mali: how a frame reaches the GPU, the stage cache, what was measured, what stands between the 27 release and its glass. |
| [darwin27-boot.md](darwin27-boot.md) | The lab notebook: how Darwin 27 was brought to its home screen and to an interface that moves, run by run, with the measurements. |
| [bluetooth-porting.md](bluetooth-porting.md), [camera-porting.md](camera-porting.md) | Bluetooth and the camera on another MediaTek phone: the skeleton driver and the operations a phone supplies. |
| [troubleshooting.md](troubleshooting.md) | What a message or a symptom means and what to do: the checks, the builds, the boot, the log, the screen, Metal, the system, the hardware. |
| [glossary.md](glossary.md) | The words of three worlds, one line each: this project's, MediaTek's and Android's, Apple's. |
| [migrating.md](migrating.md) | For existing branches and patches: where everything went, in this repository's layout of 22 November 2026 and from the first working tree. |

Next to the code:

| Document | |
| --- | --- |
| [../arch/arm64/boot/dts/README.md](../arch/arm64/boot/dts/README.md) | Device packages and board files. |
| [../arch/arm64/boot/dts/mediatek/README.md](../arch/arm64/boot/dts/mediatek/README.md) | SoC files: what one says, where its values come from. |
| [../workbench/README.md](../workbench/README.md) | What is tested without a phone, and how a driver runs without a kernel. |
| [../tools/README.md](../tools/README.md) | The tools, one line each: the project's, one run on the phone, what a run left. |
| [../kernel/README.md](../kernel/README.md) | How the kernel's sources are kept and worked on. |
| [../kernel/patches/README.md](../kernel/patches/README.md) | What each patch changes in Apple's files. |
| `../arch/arm64/configs/*_defconfig` | The boot arguments of each kind of run, one a line with what it does (`make defconfigs`). |
| [../arch/arm64/boot/README.md](../arch/arm64/boot/README.md) | The loader. |
| [../darwin/README.md](../darwin/README.md) | What Darwin 27 needs: each library, tool and build step, and what each needs. |
| [../userland/README.md](../userland/README.md) | Our user space: the programs, the inserted libraries, the Metal device, the job lists, the user space's tools. |
| [../userland/src/mali/README.md](../userland/src/mali/README.md) | The native Mali back end's bring-up: its parts, its probe's stages, its 37 capabilities. |
| [../userland/docs/air.md](../userland/docs/air.md), [air-report.md](../userland/docs/air-report.md) | The AIR to NIR shader front end, and its corpus report. |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Where a change goes, and the rules. |
| [../MAINTAINERS](../MAINTAINERS) | Each part: its files, its tests and its documents. |
| [../SECURITY.md](../SECURITY.md) | What must never be published, and how to report a problem. |
| [../components.toml](../components.toml) | Everything taken from elsewhere, at the exact version, with its licence. |

A phone's own story (what it did, run by run, and where the guides' facts were found) is its
package's `journal/` on the `devices` branch.
