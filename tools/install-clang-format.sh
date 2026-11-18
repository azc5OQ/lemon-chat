#!/bin/sh
# Downloads the clang-format version that format_codebase.py uses into this folder, without formatting
# any file.
if command -v python3 >/dev/null 2>&1
then
    exec python3 "$(dirname "$0")/format_codebase.py" --install "$@"
fi
echo 'Python 3 is required to download clang-format. No files were changed.' >&2
exit 1
