// Compile the browser's identity calculation unchanged for the native client.
// export_name belongs to WebAssembly and is not needed by the native linker.
#define __attribute__(attribute)
#include "identity-keygen/rsa_keygen.c"
#undef __attribute__
