# shellcheck shell=bash disable=SC2034  # sourced: its variables are used by the scripts that source it
# Shared by kernel/assemble.sh, export.sh and verify.sh (sourced, not run).
#
# The kernel is kept as three things:
#   UPSTREAM          which Apple release it is made from
#   the files HakiOS adds: the directories of the repository's top that OVERLAY_PLACES names, each one
#                     directory of the XNU tree, as Linux's tree keeps its architecture, drivers, crypto and file
#                     systems (drivers/board is bsd/dev, fs/apfs is bsd/miscfs/apfs, ...); and overlay/, the files
#                     that belong to none of them, at their place in the tree
#   patches/          the changes to Apple's own files, one patch a topic (patches/series)
# and an "assembled tree" is a checkout of the release with the overlay copied in and the patches
# applied: what is compiled, and where the kernel is worked on. It is a git repository of its own:
#   tag upstream      Apple's release
#   tag assembled     what the last assemble.sh put there (a commit on top of upstream)
#   branch hakios     the working branch
KERNEL_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OVERLAY=$KERNEL_DIR/overlay
REPO_DIR=$(cd "$KERNEL_DIR/.." && pwd)
# A directory of the repository and the directory of the tree its files are, each "REPOSITORY:TREE". Everything in
# it is the tree's but its README.md, which is the repository's (tools/bootargs.py reads this list as well).
OVERLAY_PLACES=(
	"arch/arm64/kernel:osfmk/arm64"
	"arch/arm64/include/pexpert:pexpert/pexpert/arm64"
	"arch/arm64/configs/xnu:config"
	"crypto:osfmk/minicrypto"
	"fs/apfs:bsd/miscfs/apfs"
	"fs/ramfs:bsd/miscfs/ramfs"
	"security/amfi:libkern/amfi"
	"drivers/board:bsd/dev"
	"drivers/iokit:iokit/Kernel"
	"include/board:bsd/sys"
	"include/external:EXTERNAL_HEADERS"
)

# overlay_files: every file the repository adds to the tree, one a line, "REPOSITORY-PATH<tab>TREE-PATH" (the first
# relative to the repository's top), kernel/overlay's first, then each place's
overlay_files() {
	local place dir prefix
	(cd "$OVERLAY" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) | awk '{ printf "kernel/overlay/%s\t%s\n", $0, $0 }'
	for place in "${OVERLAY_PLACES[@]}"; do
		dir=${place%%:*} prefix=${place#*:}
		[ -d "$REPO_DIR/$dir" ] || continue
		(cd "$REPO_DIR/$dir" && find . -type f ! -name README.md | sed 's|^\./||' | LC_ALL=C sort) |
			awk -v d="$dir" -v p="$prefix" '{ printf "%s/%s\t%s/%s\n", d, $0, p, $0 }'
	done
}

# overlay_view DIR: every file of the overlay at its path in the kernel's tree, as a link to the repository's file
# (DIR/bsd/dev/mtk_keys.c, DIR/bsd/sys/hakios_input.h, ...): what the host's tests compile the kernel's files in,
# with the same paths the kernel's build has
overlay_view() {
	local view=$1 file tree
	mkdir -p "$view"
	while IFS=$'\t' read -r file tree; do
		mkdir -p "$view/$(dirname "$tree")"
		ln -sfn "$REPO_DIR/$file" "$view/$tree"
	done < <(overlay_files)
}

# place_of TREE-PATH: the place of OVERLAY_PLACES a file of the tree belongs to, as "REPOSITORY:TREE"; nothing for
# a file that is kernel/overlay's
place_of() {
	local place
	for place in "${OVERLAY_PLACES[@]}"; do
		case "$1" in "${place#*:}"/*) echo "$place"; return 0 ;; esac
	done
	return 1
}
PATCHES=$KERNEL_DIR/patches
SERIES=$PATCHES/series
# shellcheck source=UPSTREAM
source "$KERNEL_DIR/UPSTREAM"

log() { echo "${0##*/}: $*"; }
die() { echo "${0##*/}: $*" >&2; exit 1; }

# Files the build makes inside the tree: never part of the kernel's sources. (The board table is made
# from the device packages that are there when the kernel is built: tools/boards.py gen.)
TREE_EXCLUDES=(/BUILD/ /bsd/dev/hakios_board_table.h '/bsd/dev/*_mtkcam_jn1.h' '/bsd/dev/*_mtkcam_rx.h' '/bsd/dev/*_camera_jn1.h' '/bsd/dev/*_camera_rx.h' '*.orig' '*.rej')

# tree_excludes DIR: tell the tree's git which files are the build's. Written anew each time, so that
# a tree made before the list grew learns of what was added.
tree_excludes() {
	mkdir -p "$1/.git/info"
	{
		echo "# kernel/lib.sh (TREE_EXCLUDES): what the build makes in this tree"
		printf '%s\n' "${TREE_EXCLUDES[@]}"
	} > "$1/.git/info/exclude"
}

# The patches of the series, in order.
series_patches() {
	awk '!/^[[:space:]]*(#|$)/ && !seen[$1]++ { print $1 }' "$SERIES"
}

# patch_for FILE: the patch a file of the tree belongs to -- the first line of the series with a
# pattern that matches it (a directory ending in /, a path, or a glob). Nothing if none does.
patch_for() {
	local file=$1 patch pattern
	while read -r patch patterns; do
		case "$patch" in ''|'#'*) continue ;; esac
		for pattern in $patterns; do
			[ "${pattern%/}" = "$pattern" ] || pattern="$pattern*"  # a directory: everything in it
			# shellcheck disable=SC2254  # the pattern is meant to be a glob
			case "$file" in
			$pattern) echo "$patch"; return 0 ;;
			esac
		done
	done < "$SERIES"
	return 1
}

# git on an assembled tree, with nothing of the user's configuration changing what a diff looks like
tree_git() {
	local tree=$1
	shift
	git -C "$tree" -c core.quotepath=false -c diff.noprefix=false -c diff.mnemonicPrefix=false \
		-c diff.algorithm=myers -c core.autocrlf=false -c user.name=hakios -c user.email=hakios@localhost "$@"
}

is_assembled_tree() {
	[ -d "$1/.git" ] && git -C "$1" rev-parse -q --verify refs/tags/upstream > /dev/null 2>&1
}
