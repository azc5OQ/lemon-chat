# why this directory exists

The embedded Node runtime (`libnode.so`, from nodejs-mobile) is a native library.
Java cannot call it directly. This directory builds `liblemonchatnode.so`, the small JNI glue
between the two:

- `native-lib.cpp` - exposes `nativeStartNode()` to Java (`NodeRuntime.java`), which calls
  `node::Start()`. That call blocks for the life of the Node event loop, so Java runs it on its
  own thread. It also pumps Node's stdout/stderr into logcat under the tag `lemonchat-node` -
  without that pump every `console.log` from the bundle is invisible on a device.
- `CMakeLists.txt` - builds the glue and links it against the prebuilt `libnode.so`. Built with
  `-DANDROID_STL=c++_shared`, because libnode links the shared libc++ and the apk must ship
  `libc++_shared.so` next to it or `System.loadLibrary("node")` fails at runtime.
- The headers and libraries come from one verified version directory under `app/build/generated/node-runtime/`. Gradle passes its path as `NODEJS_MOBILE_ROOT`. The old adjacent `nodejs-mobile-include/` and `src/main/jniLibs` directories are not used.
- Node 24 headers require C++20. The JNI adapter uses 16 KiB ELF alignment and the same NDK revision as the runtime release. The packaged C++ library must remain available beside Node.

`prepareNodeRuntime` runs before native configuration and checks every cached file against the pinned release. It prepares matching headers and libraries together, so an interrupted download cannot leave a mixed runtime. Standard Android packaging strips non-runtime symbols while keeping exported functions and allocated runtime sections. The APK verification tool checks those sections against the verified SDK.
