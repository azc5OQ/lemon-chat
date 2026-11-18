#!/bin/sh
# Builds client.html and the Electron app for Linux, client-electron-amd64.AppImage, client-electron-arm64.AppImage
# and client-electron-armhf.AppImage in buildresult. It runs the client build in ../browser with --electron, which
# takes the same options, for example --minified. The AppImage files need mksquashfs from the squashfs-tools
# package, on Debian and Ubuntu installed with sudo apt install squashfs-tools.
if [ "$(uname -s)" != Linux ]
then
    echo 'build-linux.sh runs only on Linux. Use build-windows.bat on Windows and build-mac.sh on macOS.' >&2
    exit 1
fi
exec sh "$(dirname "$0")/../browser/build-linux.sh" --electron "$@"
