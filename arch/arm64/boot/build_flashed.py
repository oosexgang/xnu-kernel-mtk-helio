#!/usr/bin/env python3
"""
Pack build/boot_a.img: mtkloader, then Android's kernel, then the XNU payload, in the
rooted Android boot image (backup/boot_a_magisk.img by default; its ramdisk, dtb and cmdline
are kept). Booting it runs XNU once, then Android comes back with XNU's log in
/sys/fs/pstore/console-ramoops-0 (see mtkloader/mtkloader.c).

  build_flashed.py [android-boot.img]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

android = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'backup', 'boot_a_magisk.img')
if not os.path.isfile(android):
    sys.exit(f'build_flashed.py: no {android}: it is your phone\'s own rooted boot image '
             '(see Documentation/porting.md, "What the phone must allow"), never in the repository')
# HAKIOS_PAYLOAD=flash: no payload in the boot image; the loader reads it from the system partition's tail
# (tools/run/payload-flash.sh), and boot_a holds only the loader and Android
payload = [] if os.environ.get('HAKIOS_PAYLOAD') == 'flash' else [os.path.join(ROOT, 'build', 'payload.bin')]
subprocess.check_call([sys.executable, os.path.join(HERE, 'chainboot.py'), android,
                       os.path.join(ROOT, 'build', 'loader', 'mtkloader.bin'), os.path.join(ROOT, 'build', 'boot_a.img')] + payload)
