# The kernel

Apple's `xnu-12377.121.6`, the 27 release's kernel, kept as three things:

| | |
| --- | --- |
| `UPSTREAM` | which release it is made from, by commit |
| `overlay/` | the files added, at their place in the XNU tree |
| `patches/` | the changes to Apple's own files, one patch a topic (`patches/series`, [patches/README.md](patches/README.md)) |

An *assembled tree* is a checkout of the release with the overlay copied in and the patches applied:
what is compiled, and where the kernel is worked on. It is a git repository of its own (tag `upstream`
is Apple's release, tag `assembled` what the scripts put there, branch `hakios` the work) and lives
next to this repository, in `../xnu-12377-hakios` unless `XNU_SRC` says otherwise.

## Building

What it needs is in [docs/building.md](../docs/building.md). In short:

```sh
kernel/assemble.sh ../xnu-12377-hakios   # Apple's release + overlay + patches
kernel/deps/setup.sh                     # what the build needs beside the tree: LLVM 19.1.7, mig 138, the iig headers of
                                         # macOS 26.5's SDK, sources, host tools, the SDK's overlay, two libraries
kernel/build.sh -j8 -k                   # -k is needed: one harmless error (lldbmacros) ends make with code 2
```

The result is `$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` (`BCM2837` is
the open configuration of XNU the port reuses, not a statement about the machine). `kernel/build.sh`
brings the tree up to date with the repository first and writes the board table into it for the
device packages that are present, so a kernel knows the boards it was built with.

Things to know:

- The path of the tree must have no spaces in it.
- After a change to kernel headers that the two outside libraries see (the pthread kext, firehose):
  `kernel/deps/setup.sh --rebuild-libs`.
- Apple's DriverKit generator (`iig`) is closed. `kernel/deps/iig-impl/OSObject.iig.cpp` stands in,
  generated from the linker's list of undefined symbols (`kernel/deps/gen-iig-stubs.py`).

## Two ways of working

- **A file of ours**: edit it in `kernel/overlay/`, run `kernel/build.sh` (which updates the tree).
- **In the tree**, the only way for Apple's own files: edit there, build, then
  `kernel/export.sh ../xnu-12377-hakios`. `git diff` in this repository then shows the work: new files
  under `overlay/`, changed topic patches. A changed file of Apple's that no line of `patches/series`
  claims stops the export: give it a topic there.

`kernel/syntax-check.sh --tree ../xnu-12377-hakios` (or `make syntax-check`) puts every C file of
the overlay through the machine's clang with `-fsyntax-only` and the format check, in about ten
seconds instead of a kernel build; `kernel/syntax-check.skip` lists the few files that need the real
build's generated headers.

`kernel/verify.sh` checks that the release assembles and that exporting the result gives back exactly
what is committed. `kernel/import-patch.sh` takes a patch written against a tree. `kernel/assemble.sh
--update` never runs over work in the tree that was not exported.

## Moving to another release

Change the three values of `UPSTREAM`, assemble (the patches that no longer apply say so), fix them
in the tree, export. The overlay and the series stay.
