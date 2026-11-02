# Shader sources and embedding

API means application programming interface. D3D11 means Direct3D 11.

Each `../renderers/<api>/shaders/` directory contains the existing drawing
shaders for that API. D3D11 also contains the optional background and composition
compute shaders. This organization does not add effect acceleration to other APIs.

Edit `.hlsl`, `.glsl`, or `.in` source files, then run from the meek-ui root:

```sh
python backends/tools/embed_shaders.py
python backends/tools/embed_shaders.py --check
```

The generator resolves paths relative to itself, so it also works from another
working directory. `manifest.json` beside the shaders maps source files to C
declarations and generated header names. The C renderer includes these headers
from `shaders/generated/`; commit both sources and generated headers. Normal
builds need neither Python nor shader files installed beside the executable.
The `--check` command does not write files and fails when a header is stale.

Vulkan also checks in SPIR-V (Standard Portable Intermediate Representation)
bytecode. After changing a Vulkan shader, regenerate it with
`python backends/tools/compile_vulkan_shaders.py`, using `glslangValidator` on the
build machine. `embed_shaders.py --check` verifies the recorded source hashes
for those binary headers too. No runtime shader compiler is loaded by Vulkan.
The fixed-function OpenGL 2 renderer uses no shaders and needs no shader folder.

Templates ending in `.in` use `@C_MACRO@` to retain the existing shared C shader
snippets from `../common/_visual_contract.h` and the platform-selected GLSL
(OpenGL Shading Language) version in the GLES (OpenGL for Embedded Systems) renderer. A token on its own line replaces the whole line;
an inline token replaces only itself. C compilation expands the macro in the
generated string. This keeps the SDF (signed distance field) and antialiasing contract centralized and
preserves macOS's GLSL 410 versus GLES's GLSL 300 es selection.

Standalone and embedded hosts compile the same renderer and generated shaders.
The optional engine in `../renderers/d3d11/effects_d3d11.c` remains a separate
D3D11 device used synchronously on the caller's drawing thread. Windows hosts
can select `GUI_EFFECT_ACCELERATION__D3D11_COMPUTE`; CPU (central processing unit) effects remain the default.
Native effect ports for OpenGL/GLES, Vulkan, D3D9 and D3D12 remain future work.
