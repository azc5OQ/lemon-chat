# Bundled Windows formatter

`clang-format.exe` is the Windows x86-64 executable from [LLVM 23.1.5](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.5), the latest stable release when downloaded on 2026-11-07. It is used by the server and desktop Windows formatting scripts.

The executable was extracted from the official `clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst` archive. The downloaded archive matched the SHA-256 published in the GitHub release metadata:

```text
b2be33385ddc86e83511417a060de927ae8b09d1f3017cc3cdce5c73cd5029d7
```

The extracted executable has this SHA-256:

```text
9b8e2d41c3733c5a6b6d7f1dbefdf8ef1dadcb7093339533aa5cd696497f1fcc
```

The LLVM license is in `clang-format-LICENSE.txt`. See [source formatting](../docs/building.md#source-formatting) for the commands.
