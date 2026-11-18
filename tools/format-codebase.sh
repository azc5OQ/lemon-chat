#!/bin/sh
# Formats the C, C++ and Java sources of server-source-code, client-source-code or both with clang-format and
# the own rules in .clang-format of this folder. It asks which part to format and offers to download
# clang-format when it is missing. Options of format_codebase.py can be added, for example --check.
# Before every commit, run format-before-commit, which restores the project rules.
if command -v python3 >/dev/null 2>&1
then
    exec python3 "$(dirname "$0")/format_codebase.py" "$@"
fi
echo 'Python 3 is required to format the sources. No files were changed.' >&2
exit 1
