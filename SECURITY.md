# Security

This project runs Darwin on an Android phone it was not made for, booted by hand for one run at a time: it is for
development, not for a phone anyone depends on. Much of what keeps Apple's own devices safe is not here -- a secure
enclave and its keys, signed code enforced end to end, a measured boot chain -- and stand-ins take its place
(`security/amfi/`, `drivers/iokit/MiniAppleKeyStore.cpp`, `drivers/iokit/MiniFairPlay.cpp`,
`kernel/overlay/bsd/kern/kern_minimac.c`), which allow what Darwin needs to start. Do not keep anything on a phone
running it that you would not show to whoever holds it.

What a run keeps is in memory: `/private/var` is a file system in RAM (`fs/ramfs/`), and the kernel's eMMC driver
only reads (`drivers/board/mtk_emmc.c`). Nothing written during a run outlives it. The key store's stand-in has no
secret to wrap keys with; with the 27 release it is not asked for at all (`aks_keys=1` must not be passed).

## Reporting

A way to run code in the kernel from an application, to read another process's memory, to make a tool write to a
phone anywhere but where it says below, or to get a secret of yours (the network's passphrase, a serial number) into
the repository or into an image meant to be shared: please report it privately, through the repository's "Report a
vulnerability" (GitHub's private advisories), not in an issue. Say how to reproduce it and what it reaches.

What is not a vulnerability here: that the stand-ins grant what Apple's policies would refuse (they do, on purpose,
and say so in their heads), or that a phone running this can be read by whoever has it in hand.

## What the tools write to a phone

- `boot_a`, every run (`tools/run/xnu-cycle.sh`): the loader packed into your own rooted boot image, to the phone of
  `HAKIOS_SERIAL` only, not when its Android says it is another phone than the board's (`[boot] android_device`),
  and only while its current slot is `a`. Slot `b` is never written.
- The partition for the system image (`xnusys` on the first phone): the image once (`tools/run/system-flash.sh`),
  the payload into its last 96 MiB every run (`tools/run/payload-flash.sh`, which reads back what it wrote).
- Nothing else. A new partition table is written by `tools/gpt-carve.py` **into a file**; putting it on a phone is
  your step, and so is the way back from a broken `boot_a` (the boot ROM's download mode, before the first flash).

## What is never published

`tools/check-publishable.sh`, part of `make check`, looks for most of these in what git tracks, and `.gitignore`
keeps `fw-*/`, `backup/` and `logs/` out of it; the rule is yours to keep in anything you send:

- Apple's files (anything of an IPSW or a system image, SDK headers, the kernel built), and a phone maker's firmware,
  boot images, device trees and the camera's register tables made from a capture.
- The network to join: `fw-wifi/network`, its name on the first line and its passphrase on the second. It goes into
  the small image you build for your own phone (`root/fw/network`, mode 0600); `SHARE=1` of `userland/build.sh`
  leaves it out of an image meant for someone else. Never paste it into an issue or a log you share.
- A phone's serial number (`HAKIOS_SERIAL` lives in the environment, never in a file), home-directory paths, keys,
  and a symbolic link out of the repository.
