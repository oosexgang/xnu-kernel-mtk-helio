#!/bin/sh
# The speakers' guard (bsd/dev/hakios_guard.c) on this machine: workbench/guard_measure.c built against it and run.
# The kernel's header is all it needs, and an empty sys/ioccom.h beside it (the ioctl numbers are not used).
set -e
cd "$(dirname "$0")/.."
SRC=kernel/overlay/bsd
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/sys"
cp $SRC/sys/hakios_audio.h "$tmp/sys/"
printf '#define _IOW(g, n, t) 0\n' > "$tmp/sys/ioccom.h"
cc -O2 -Wall -DKERNEL -I"$tmp" workbench/guard_measure.c $SRC/dev/hakios_guard.c -lm -o "$tmp/guard_test"
"$tmp/guard_test"
