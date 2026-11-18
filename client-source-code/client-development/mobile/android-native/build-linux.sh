#!/bin/sh
# Builds the native Android app on Linux with tools/build.sh, which build-mac.sh also runs.
if [ "$(uname -s)" != Linux ]
then
    echo 'build-linux.sh runs only on Linux. Use build-windows.bat on Windows and build-mac.sh on macOS.' >&2
    exit 1
fi
exec sh "$(dirname "$0")/tools/build.sh" "$@"
