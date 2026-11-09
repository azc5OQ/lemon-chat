#!/bin/sh
# Bootstrap only Node here. No system installation or shell-profile changes.
set -eu
browser_directory=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tools_directory="$browser_directory/.build-tools"
case "$(uname -s)" in
    Linux) platform=linux ;;
    Darwin) platform=darwin ;;
    *) echo 'Use build.bat on Windows, or install Node.js and run node build.cjs.' >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64) architecture=x64 ;;
    aarch64|arm64) architecture=arm64 ;;
    *) echo 'Install Node.js manually on this CPU, then run node build.cjs.' >&2; exit 1 ;;
esac
package=$(awk -v key="$platform-$architecture" '$1 == key { print }' "$browser_directory/tools/node-downloads.txt")
checksum=$(printf '%s\n' "$package" | awk '{print $2}')
url=$(printf '%s\n' "$package" | awk '{print $3}')
folder=$(printf '%s\n' "$package" | awk '{print $4}')
cached="$tools_directory/$folder/bin/node"
node_program=''
for candidate in node "$cached"
do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -e 'process.exit(parseInt(process.versions.node) >= 20 ? 0 : 1)' 2>/dev/null
    then
        node_program=$candidate
        break
    fi
done
if [ -z "$node_program" ]
then
    download=no
    policy=ask
    for argument in "$@"
    do
        case "$argument" in
            --download-tools) policy=yes ;;
            --no-download-tools) policy=no ;;
        esac
    done
    if [ "$policy" = yes ]
    then
        download=yes
    elif [ "$policy" = ask ] && [ -t 0 ]
    then
        printf 'Node.js is needed to run the builder. Download portable Node 24.14.0 into browser/.build-tools? [y/N]: '
        read -r answer
        case "$answer" in
            y|Y|yes|YES) download=yes ;;
        esac
    fi
    if [ "$download" != yes ]
    then
        echo 'Install Node.js 20 or newer from https://nodejs.org/ or use --download-tools.' >&2
        exit 1
    fi
    mkdir -p "$tools_directory"
    archive="$tools_directory/${url##*/}"
    curl --fail --location --retry 2 --output "$archive" "$url"
    if command -v sha256sum >/dev/null 2>&1
    then
        actual=$(sha256sum "$archive" | awk '{print $1}')
    else
        actual=$(shasum -a 256 "$archive" | awk '{print $1}')
    fi
    if [ "$actual" != "$checksum" ]
    then
        rm -f -- "$archive"
        echo 'Node.js checksum mismatch; nothing was installed.' >&2
        exit 1
    fi
    tar -xzf "$archive" -C "$tools_directory"
    rm -f -- "$archive"
    node_program=$cached
fi
if [ "$node_program" = "$cached" ]
then
    PATH="${cached%/*}:$PATH"
    export PATH
fi
exec "$node_program" "$browser_directory/build.cjs" "$@"
