# NAME: Maker Model

| | |
| --- | --- |
| SoC | (arch/arm64/boot/dts/mediatek/SOC.toml) |
| Panel | W x H, video or command mode |
| Storage | eMMC or UFS; the partition with the system image |
| Verified | the date and the commit of the main branch it was run with |

## What works

One line a driver, with the run it was seen in (the date, the log's line that proves it).

## What does not

What was tried and why it stopped; what nobody has tried.

## Where the values are from

Which Android build the device tree and the logs were read under, and anything a reader of
`board.toml` could not guess from its comments.

## Yours to bring

The files of the phone a run needs and the repository never has: the rooted boot image, firmware.
