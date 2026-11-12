# Device packages

One directory a phone: `NAME/board.toml` (what the phone has and where), `NAME/README.md` (what
works, what was verified and when, where the values are from) and, where there is one, `NAME/journal/`
(the phone's own story, run by run). Nothing of the code is here: it is on the
[main branch](https://github.com/oosexgang/xnu-kernel-mtk-helio), which names no phone.

| Package | Device | SoC |
| --- | --- | --- |
| [`x6826b`](x6826b/README.md) | Infinix Hot 20 | MediaTek Helio G85 (MT6768) |

In a checkout of the main branch:

```sh
tools/device.py available        # what is here
tools/device.py add x6826b       # copy a package into devices/ and check it against the code
```

A new phone: `tools/boards.py new NAME --soc SOC` on the main branch, the guide `docs/porting.md`,
then `tools/device.py export NAME DIR` into a checkout of this branch.

Nothing of a phone's vendor is kept here either: no firmware, no device tree, no boot image, no
picture of a screen. A value read from a phone (an address, a range) is a fact, with its source named
beside it.
