#!/bin/sh
# Builds client.html and the Electron app for macOS, client-electron-amd64.dmg and client-electron-arm64.dmg in
# buildresult. It runs the client build in ../browser with --electron, which takes the same options, for example
# --minified. It needs the Xcode command line tools, installed with xcode-select --install.
if [ "$(uname -s)" != Darwin ]
then
    echo 'build-mac.sh runs only on macOS. Use build-windows.bat on Windows and build-linux.sh on Linux.' >&2
    exit 1
fi
exec sh "$(dirname "$0")/../browser/build-mac.sh" --electron "$@"
