#!/bin/sh
# Builds the native Android app on macOS with tools/build.sh, which build-linux.sh also runs.
if [ "$(uname -s)" != Darwin ]
then
    echo 'build-mac.sh runs only on macOS. Use build-windows.bat on Windows and build-linux.sh on Linux.' >&2
    exit 1
fi
exec sh "$(dirname "$0")/tools/build.sh" "$@"
