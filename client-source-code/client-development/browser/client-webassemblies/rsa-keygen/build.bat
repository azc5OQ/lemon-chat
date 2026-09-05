@echo off
rem builds rsa_keygen.wasm with plain llvm (no emscripten needed): freestanding c,
rem no libc, exports declared in the source with export_name attributes.
rem -ffp-contract=off because the seeded random clone depends on exact ieee doubles

set CLANG="C:\Program Files\LLVM\bin\clang.exe"

if not exist build mkdir build

%CLANG% --target=wasm32 -O2 -ffp-contract=off -nostdlib ^
    -Wl,--no-entry -Wl,--export-dynamic ^
    -o build\rsa_keygen.wasm src\rsa_keygen.c

if errorlevel 1 (
    echo build failed
    exit /b 1
)

echo built build\rsa_keygen.wasm
