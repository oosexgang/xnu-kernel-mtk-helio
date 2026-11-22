#!/bin/bash
# Take the kernel's sources out of an assembled tree, back into the repository: the files the tree has
# and Apple's release has not go back to their places (kernel/lib.sh's OVERLAY_PLACES: a new file of
# bsd/dev to drivers/board, ...; one of no place to kernel/overlay), the changes to Apple's files become
# the patches of kernel/patches/series. Run it after working in the tree; `git diff` in the repository then shows
# what the work was.
#
#   kernel/export.sh DIR            from the tree in DIR (made by kernel/assemble.sh)
#
#   --to OUT        write OUT/overlay, OUT/places/DIR and OUT/patches instead of the repository's (kernel/verify.sh)
#
# What the build leaves in the tree (BUILD/, the firmware stub) is not sources and is left out. A
# changed file of Apple's that no line of patches/series claims stops the export: give it a topic there.
# A patch written by somebody else against the tree (a .patch file) is taken over the same way: apply it
# in the tree (git apply), then export.
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

out=$KERNEL_DIR
while [ $# -gt 0 ]; do
	case "$1" in
	--to) out=${2:?--to needs a directory}; shift ;;
	-h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option $1 (see --help)" ;;
	*) break ;;
	esac
	shift
done
[ $# -eq 1 ] || die "usage: export.sh [--to OUT] DIR"
is_assembled_tree "$1" || die "$1 is not a tree made by assemble.sh"
D=$(cd "$1" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)

work=$(mktemp -d)
trap 'rm -rf "$work"; rm -f "$D/.git/hakios-export-index"' EXIT

tree_excludes "$D"
# The tree as it lies there (committed or not, new files included) in an index of its own.
export GIT_INDEX_FILE=$D/.git/hakios-export-index
rm -f "$GIT_INDEX_FILE"
tree_git "$D" read-tree upstream
tree_git "$D" add -A .

mkdir -p "$work/overlay" "$work/patches" "$work/lists"
unclaimed=
# (the files of each patch are kept as lists in files, one a patch: bash 3.2, which macOS has, knows no
# associative arrays)
while IFS=$'\t' read -r status file; do
	case "$status" in
	A)
		if place=$(place_of "$file"); then
			to=$work/places/${place%%:*}/${file#"${place#*:}"/}
		else
			to=$work/overlay/$file
		fi
		mkdir -p "$(dirname "$to")"
		cp -p "$D/$file" "$to"
		;;
	*)
		if patch=$(patch_for "$file"); then
			echo "$file" >> "$work/lists/$patch"
		else
			unclaimed+="  $file"$'\n'
		fi
		;;
	esac
done < <(tree_git "$D" diff --cached --no-renames --name-status upstream)
[ -z "$unclaimed" ] || die "changed files of Apple's that no line of kernel/patches/series claims:
$unclaimed"

for patch in $(series_patches); do
	[ -s "$work/lists/$patch" ] || continue
	files=()
	while IFS= read -r file; do files+=("$file"); done < "$work/lists/$patch"
	# (without the "index" lines: their hashes are abbreviated by how many objects a clone has)
	tree_git "$D" diff --cached --no-color --no-ext-diff --no-textconv --no-renames -U3 \
		--src-prefix=a/ --dst-prefix=b/ upstream -- "${files[@]}" |
		sed -E '/^index [0-9a-f]+\.\.[0-9a-f]+( [0-7]+)?$/d' > "$work/patches/$patch"
done
exported=$(tree_git "$D" write-tree)
unset GIT_INDEX_FILE

# In place of what was there; the series is the repository's and stays, and so does each place's README.md.
rm -rf "$out/overlay"
mv "$work/overlay" "$out/overlay"
for place in "${OVERLAY_PLACES[@]}"; do
	dir=${place%%:*}
	if [ "$out" = "$KERNEL_DIR" ]; then
		to=$REPO_DIR/$dir
		[ -d "$to" ] && find "$to" -type f ! -name README.md -delete
	else
		to=$out/places/$dir
		rm -rf "$to"
	fi
	if [ -d "$work/places/$dir" ]; then
		mkdir -p "$to"
		cp -pR "$work/places/$dir/." "$to/"
	fi
	[ -d "$to" ] && find "$to" -mindepth 1 -type d -empty -delete
done
mkdir -p "$out/patches"
rm -f "$out"/patches/*.patch
for patch in "$work"/patches/*.patch; do
	[ -e "$patch" ] && mv "$patch" "$out/patches/"
done
if [ "$out" = "$KERNEL_DIR" ]; then
	# The tree is now what the repository says: noted in the tree, so that the next update knows the work
	# was taken over (and touches nothing for it).
	commit=$(tree_git "$D" commit-tree "$exported" -p upstream -m "$UPSTREAM_TAG + HakiOS (exported to the repository)")
	git -C "$D" symbolic-ref HEAD refs/heads/hakios
	git -C "$D" update-ref refs/heads/hakios "$commit"
	git -C "$D" read-tree "$commit"
	git -C "$D" tag -f assembled "$commit" > /dev/null
else
	cp "$SERIES" "$out/patches/series"
fi
log "$( (find "$out/overlay" -type f; [ ! -d "$work/places" ] || find "$work/places" -type f) | wc -l) files added to the tree, $(find "$out/patches" -name '*.patch' | wc -l) patches in $out"
