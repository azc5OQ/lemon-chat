// The configured minimp3 frame decoder does not use malloc or other stdlib functions.
// Keep its stdlib include freestanding; the wrapper owns the WebAssembly heap.
