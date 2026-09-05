# why this directory exists

The embedded Node runtime (`jniLibs/<abi>/libnode.so`, from nodejs-mobile) is a native library -
Java cannot call it directly. This directory builds `liblemonchatnode.so`, the small JNI glue
between the two:

- `native-lib.cpp` - exposes `nativeStartNode()` to Java (`NodeRuntime.java`), which calls
  `node::Start()`. That call blocks for the life of the Node event loop, so Java runs it on its
  own thread. It also pumps Node's stdout/stderr into logcat under the tag `lemonchat-node` -
  without that pump every `console.log` from the bundle is invisible on a device.
- `CMakeLists.txt` - builds the glue and links it against the prebuilt `libnode.so`. Built with
  `-DANDROID_STL=c++_shared`, because libnode links the shared libc++ and the apk must ship
  `libc++_shared.so` next to it or `System.loadLibrary("node")` fails at runtime.
- `nodejs-mobile-include/` - the Node headers the glue compiles against. NOT committed (~600
  files); `nodejs-client/fetch-nodejs-mobile.py` downloads them together with `libnode.so`.
  Headers and .so must come from the SAME nodejs-mobile release - `node.h`'s `Start()` signature
  is the entire contract between them.

If Node ever needs to be removed from the app, delete this directory, the `jniLibs` libraries,
and the `externalNativeBuild` block in `app/build.gradle`.
