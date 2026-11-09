#!/bin/sh
# The shared cleaner uses only Python's standard library and Git.
set -eu
clean_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if command -v python3 >/dev/null 2>&1
then
    exec python3 "$clean_root/tools/clean.py" "$@"
fi
printf '%s\n' 'Python 3 is required to run the cleanup script. No files were removed.' >&2
exit 1
