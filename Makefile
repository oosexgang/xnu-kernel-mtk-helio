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
	@echo "make boards         validate the device packages and soc/, write what the code is built with (build/boards)"
	@echo "make verify-kernel  Apple's release + overlay + patches assemble and export back unchanged (network)"
	@echo "make syntax-check   every overlay file through clang -fsyntax-only, in seconds (TREE=an assembled tree)"
	@echo "make docs           docs/boot-args.md and docs/features.md, generated (tools/bootargs.py, tools/boards.py)"
	@echo "make kernel         build the kernel (kernel/build.sh; docs/building.md says what it needs)"
	@echo "make loader         build the loader for the phone whose package is here (loader/build.sh)"

.PHONY: check
check: lint boards-check test

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
	loader/build.sh
