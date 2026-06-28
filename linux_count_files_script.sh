#!/usr/bin/env bash
set -u

exts=(c h cpp hpp o obj dts dtsi S rst txt yaml rs sh csv py conf config gitignore asn1 js html css)

count() {
    local ext="$1"
    local n
    n=$(find . -type f -name "*.${ext}" 2>/dev/null | wc -l)
    printf '.%s file count %s\n' "$ext" "$n"
}

for e in "${exts[@]}"; do
    count "$e"
done

echo

no_ext=$(find . -type f ! -name "*.*" 2>/dev/null | wc -l)
echo "files without extension count $no_ext"

all=$(find . -type f 2>/dev/null | wc -l)
echo "all files count $all"