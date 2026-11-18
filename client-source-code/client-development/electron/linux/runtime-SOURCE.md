# AppImage runtime

`runtime-x86_64`, `runtime-aarch64` and `runtime-armhf` are the AppImage type 2 runtime for x86-64, 64-bit ARM and
32-bit ARM processors. The runtime is the program at the start of every AppImage file, which mounts the appended
SquashFS image and starts its `AppRun`. `package_electron.py` writes the runtime of the target, then the SquashFS
image of the app folder, into `client-electron-amd64.AppImage`, `client-electron-arm64.AppImage` or
`client-electron-armhf.AppImage`. The files are kept in the repository so that a build does not download them.

Source: https://github.com/AppImage/type2-runtime/releases/tag/continuous (files `runtime-x86_64`, `runtime-aarch64` and `runtime-armhf`)
Build: continuous build of 2026-10-19, commit `fedfad51acc2860b2916a1eef32ba4a500ff31ef`
SHA-256 of runtime-x86_64: `617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46`
SHA-256 of runtime-aarch64: `9a72a744d221576c9347ec074243acc5c3c7568896b12714e0e82e9f0bf47900`
SHA-256 of runtime-armhf: `acc940e0b091453490861ca7341135c7cbe6182d4aa7336c67fa5f51b0e77fcc`
License: MIT, in `runtime-LICENSE.txt`
