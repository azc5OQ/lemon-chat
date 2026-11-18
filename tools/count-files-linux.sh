#!/bin/sh
set -u

# sh mode for zsh
if [ -n "${ZSH_VERSION:-}" ]; then emulate sh; fi

# repository folder
cd "$(dirname "$0")/.." || exit 1

count() {
    n=$(($(find . -type f -name "*.$1" 2>/dev/null | wc -l)))
    printf '.%s file count %s\n' "$1" "$n"
}

for e in c h cpp hpp o obj dts dtsi S rst txt yaml rs sh csv py conf config gitignore asn1 js html css; do
    count "$e"
done

echo

no_ext=$(($(find . -type f ! -name "*.*" 2>/dev/null | wc -l)))
echo "files without extension count $no_ext"

all=$(($(find . -type f 2>/dev/null | wc -l)))
echo "all files count $all"
