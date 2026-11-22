#!/bin/bash
# The checks of form, for every file the repository tracks:
#   - every shell script parses (bash -n), every Python script parses
#   - the scripts of kernel/, loader/, workbench/, tools/ and userland/tools/ pass shellcheck, where the machine has it
#   - components.toml pins what the fetching scripts pin (tools/components.py)
#   - nothing that must not be published is there (tools/check-publishable.sh)
# `make lint` runs this; it changes nothing.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
failed=0

while IFS= read -r script; do
	bash -n "$script" || failed=1
done < <(git ls-files '*.sh')

while IFS= read -r script; do
	python3 -c 'import ast, sys; ast.parse(open(sys.argv[1]).read(), sys.argv[1])' "$script" || failed=1
done < <(git ls-files '*.py')

if command -v shellcheck > /dev/null; then
	shellcheck -S error kernel/*.sh loader/*.sh workbench/*.sh tools/*.sh tools/run/*.sh userland/tools/*.sh || failed=1
else
	echo "lint: no shellcheck on this machine: only the syntax was checked"
fi

tools/components.py check || failed=1

tools/check-publishable.sh > /dev/null || { tools/check-publishable.sh; failed=1; }

[ $failed = 0 ] && echo "lint: ok"
exit $failed
