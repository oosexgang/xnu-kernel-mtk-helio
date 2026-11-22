# What can be run from a checkout. `make` alone lists it.
#
# Everything under "check" needs no phone, no system image and no kernel toolchain: a C compiler,
# Python 3.9 or newer and git.

.DEFAULT_GOAL := help
SHELL := /bin/bash

.PHONY: help
help:
	@echo "make check          lint + boards + test: everything that needs no phone (what CI runs)"
	@echo "make device DEVICE=NAME   bring a phone's package in from the devices branch (tools/device.py add)"
	@echo "make test           host tests: the tools, the drivers against models, the loader for a made-up phone (workbench/run.sh)"
	@echo "make lint           syntax of every shell and Python script, shellcheck, nothing unpublishable"
	@echo "make boards         validate the device packages and arch/arm64/boot/dts/mediatek/, write what the code is built with (build/boards)"
	@echo "make verify-kernel  Apple's release + overlay + patches assemble and export back unchanged (network)"
	@echo "make syntax-check   every overlay file through clang -fsyntax-only, in seconds (TREE=an assembled tree)"
	@echo "make defconfigs     the boot arguments of each kind of run (arch/arm64/configs: tools/defconfig.py --list)"
	@echo "make docs           Documentation/boot-args.md and Documentation/features.md, generated (tools/bootargs.py, tools/boards.py)"
	@echo "make kernel         build the kernel (kernel/build.sh; Documentation/building.md says what it needs)"
	@echo "make loader         build the loader for the phone whose package is here (arch/arm64/boot/build.sh)"

.PHONY: check
check: lint boards-check defconfig-check test

.PHONY: test
test:
	workbench/run.sh

.PHONY: lint
lint:
	tools/lint.sh

.PHONY: boards
boards:
	tools/boards.py check
	tools/boards.py gen

.PHONY: docs
docs:
	tools/bootargs.py --write
	tools/boards.py features --write

.PHONY: syntax-check
TREE ?= ../xnu-12377-hakios
syntax-check:
	kernel/syntax-check.sh --tree $(TREE)

.PHONY: defconfigs defconfig-check
defconfigs:
	tools/defconfig.py --list
defconfig-check:
	tools/defconfig.py --check

.PHONY: boards-check
boards-check:
	tools/boards.py check

.PHONY: device
device:
	tools/device.py add $(DEVICE)

.PHONY: verify-kernel
verify-kernel:
	kernel/verify.sh

.PHONY: kernel
kernel:
	kernel/build.sh -j8 -k

.PHONY: loader
loader:
	arch/arm64/boot/build.sh
