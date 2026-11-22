# Notice

## Licences

- `kernel/patches/` and the files of `kernel/overlay/` derived from Apple's XNU are under the
  **Apple Public Source License 2.0**, as XNU is (`LICENSES/APSL-2.0.txt`).
- `kernel/overlay/bsd/miscfs/apfs/libdeflate/` is libdeflate, under the MIT licence (its `COPYING`).
- Everything else is under the **MIT licence** (`LICENSES/MIT.txt`), unless a file's header says
  otherwise.
- `darwin/` comes from [xnu-kernel-qcom](https://github.com/oosexgang/xnu-kernel-qcom) (MIT), with the
  changes this port needed; `darwin/README.md` says which.
- Drivers name the Linux sources a register layout was read from. Those sources were read, not
  copied.

## What is not here, and why

- **Apple's files.** The kernel's own sources are fetched from Apple's open-source release at build
  time. The files of the system image (build 20F66 for iPhone10,3) are downloaded by the user, on the
  user's machine, from Apple's update server (`darwin/system/fetch.sh` names the file); the SDK
  (`iPhoneOS16.5.sdk`) is installed by the user. None of them is redistributed here, and using them
  is subject to Apple's licences, which are the user's to accept.
- **A phone's vendor's files.** Firmware, device trees, boot images and calibration tables stay on
  the phone they came from or on its owner's machine.
- **Pictures of a screen.** They would show somebody else's interface.

`tools/check-publishable.sh` looks for all of these before every commit.

## Trademarks and affiliation

Apple, iOS, iPhone, Darwin and Metal are trademarks of Apple Inc. MediaTek and Helio are trademarks
of MediaTek Inc. This is an independent research project, not affiliated with or endorsed by Apple,
MediaTek, Infinix or any phone maker; their names are used only to describe what the code works
with. If you believe a file here infringes anyone's rights, open an issue and it will be removed.

## Origin

The port grew out of the working tree darwin-redwood (the project's first phone, a Qualcomm one) and
of hakios-xnu-kernel's branch `mtk`. Names of that tree survive only in `docs/migrating.md` and
`CHANGELOG.md`, which say what each became.
