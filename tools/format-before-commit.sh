#!/bin/sh
# Run this before every commit. It formats the C, C++, Java and JavaScript files that differ from the last commit
# with the project rules of before-commit.clang-format (clang-format 23.1.5) and before-commit.jsbeautifyrc
# (js-beautify 2.0.3), so that the commit keeps the project style and contains no formatting changes. Add the
# changed files again with git add afterwards. --check only lists the files that would change. See
# docs/committing.md.
if command -v python3 >/dev/null 2>&1
then
    exec python3 "$(dirname "$0")/format_codebase.py" --before-commit "$@"
fi
echo 'Python 3 is required to format the sources. No files were changed.' >&2
exit 1
