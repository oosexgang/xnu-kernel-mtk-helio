#!/bin/bash
# Look for what must not be published, before every commit and push: binaries, firmware, pictures of a
# screen, a system image's files, home directories, a phone's serial number, keys, and names that went
# out of the project. It looks at what git tracks or would track (not at what .gitignore keeps out).
# Prints what it finds; exit status 1 if anything.
# (no pipefail: each check is "... | xargs grep ... | grep . && bad=1", and xargs says 123 whenever one file
# has no match, which with pipefail made every such check pass however much it printed)
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R" || exit 1
bad=0
say() { echo "$*"; bad=1; }
me=tools/check-publishable.sh
files=$(git ls-files --cached --others --exclude-standard)

while IFS= read -r f; do
	[ -f "$f" ] || continue
	case "$f" in
	# never, under any name: the touch controller's firmware as C, the loader's font (a rendering of a
	# system font), another phone's touch firmware, a phone's device tree, the camera's captured tables
	*/touchfw.h|*/font8x16.h|*focaltech_fw.h|*.dtb|*_jn1.h|*_rx.h|*/camjn1.h|*/camrx.h)
		say "vendor's or a system's own data: $f" ;;
	*.png|*.jpg|*.jpeg|*.heic|*.gif|*.mp4|*.mov) say "a picture or a video (none is kept: a screen shows somebody else's interface): $f" ;;
	*/rc.jobs.*|*/rc.*) ;;
	*.bin|*.mbn|*.elf|*.efi|*.fd|*.metallib|*.bc|*.air|*.ipsw|*.dmg|*.img|*.apfs|*_fw.h|*.dylib|*.ko|*.zst) say "firmware or a system file by its name: $f" ;;
	esac
	t=$(file -b "$f")
	case "$t" in
	*Mach-O*|*ELF*|*"PE32"*|*"Device Tree"*|LLVM*|*"compiled Java"*) say "binary: $f ($t)" ;;
	esac
done <<< "$files"

# found GREP-ARGUMENTS...: the tracked files that match, without this script
found() { echo "$files" | tr '\n' '\0' | xargs -0 grep -lI "$@" 2>/dev/null | grep -v "^$me\$"; }
found -e "/Users/[a-z]" -e "/home/[a-z][a-z0-9]*/" -e "/private/tmp/[a-z]*-[0-9]*/" | while read -r f; do echo "home path: $f"; done | grep . && bad=1
# an adb serial number of a real phone (the tools take it from HAKIOS_SERIAL, never from a file)
found -E "[0-9]{9}U[0-9]{6}|HAKIOS_SERIAL=[0-9A-Za-z]{8,}" | while read -r f; do echo "a phone's serial number: $f"; done | grep . && bad=1
found -E "(ghp_[A-Za-z0-9]{20}|github_pat_|AKIA[0-9A-Z]{16}|BEGIN (RSA|OPENSSH|EC) PRIVATE KEY)" | while read -r f; do echo "secret: $f"; done | grep . && bad=1
# e-mail addresses other than the author's (git config user.email)
mine=$(git config user.email)
echo "$files" | tr '\n' '\0' | xargs -0 grep -IoHE "[A-Za-z0-9._%+-]+@(gmail|qq|163|outlook|hotmail|icloud|me)\\.com" 2>/dev/null | grep -v ":$mine\$" | while read -r f; do echo "e-mail: $f"; done | grep . && bad=1
# names of the tools used to write this (the repository is the author's own)
found -i "cl[a]ude\\|anthrop[i]c" | while read -r f; do echo "tool name: $f"; done | grep . && bad=1
# embedded byte arrays (a firmware turned into a header) above 64 rows
for f in $(found -E "0x[0-9a-f]{2}, ?0x[0-9a-f]{2}, ?0x[0-9a-f]{2}, ?0x[0-9a-f]{2}, ?0x"); do
	n=$(grep -cE "0x[0-9a-f]{2}, ?0x[0-9a-f]{2}, ?0x[0-9a-f]{2}, ?0x[0-9a-f]{2}, ?0x" "$f")
	[ "$n" -gt 64 ] && say "byte array ($n rows): $f"
done
# The working name of the project's first phone: gone from code, paths, interfaces and documents, all of
# them (Documentation/migrating.md writes it <old>).
echo "$files" | grep -i "redw[o]od" | while read -r f; do echo "old name in a path: $f"; done | grep . && bad=1
found -i "redw[o]od" | while read -r f; do echo "old name: $f"; done | grep . && bad=1
# A symbolic link that leaves the repository (an absolute path, or up and out of it) points at one
# machine's files: never published (a link to a directory of firmware was, once)
git ls-files -s | awk '$1 == "120000" { print $4 }' | while read -r f; do
	t=$(readlink "$f")
	case "$t" in
	/*) echo "link out of the repository: $f -> $t" ;;
	*) (cd "$(dirname "$f")" && cd "$(dirname "$t")" 2>/dev/null && pwd -P) | grep -qv "^$R" && echo "link out of the repository: $f -> $t" ;;
	esac
done | grep . && bad=1
# The system image's trade names: only as the literal names of inputs and of what the system itself
# looks for, and in the notices. Everything else says "the system" and "the reference phone".
allowed='iPhoneOS[0-9.]*\.sdk|iPhone10,3|iPhone12,1|toolchain/linux/iphone|apple-ios|subsystem = .ios.|API_UNAVAILABLE\(ios\)|~iphone|/System/Volumes/iOS|chosen\[64\] = "iPhone"|trademarks? of Apple|товарные знаки'
echo "$files" | grep -vE "^(NOTICE\.md|CHANGELOG\.md|Documentation/migrating\.md|kernel/patches/.*\.patch)$" | tr '\n' '\0' |
	xargs -0 grep -nIwiE "ios|iphone" 2>/dev/null | grep -v "^$me:" | grep -vE "$allowed" |
	while read -r l; do echo "the system's trade name: ${l:0:150}"; done | grep . && bad=1
# ... and in the patches, on the lines they add (the lines around them are Apple's)
grep -n "^+" kernel/patches/*.patch 2>/dev/null | grep -wiE "ios|iphone" | grep -vE "$allowed" |
	while read -r l; do echo "the system's trade name: ${l:0:150}"; done | grep . && bad=1
[ $bad = 0 ] && echo "nothing found"
exit $bad
