# Shaders in an embedded application

Embedded and standalone adapters use the same renderer and shader sources.
Embedded does not mean central processing unit (CPU) rendering: the adapter borrows
the host graphics device, and the renderer can submit shaders to its graphics
processing unit (GPU). Device/context recreation still requires rebuilding the
corresponding GPU shader objects.

The application currently includes generated shader headers in its executable or
library. It does not load shader files from the working directory. There are two
different kinds of embedded data:

| Renderer | Current contents | Offline compilation option |
| --- | --- | --- |
| Direct3D 9 | High-Level Shading Language (HLSL) source strings; compiled during initialization | Compile the existing Shader Model 3 profiles to bytecode and store aligned byte arrays. |
| Direct3D 11 | HLSL source strings, including layers and optional compute effects | Compile the existing Shader Model 5 profiles to bytecode arrays. |
| Direct3D 12 | HLSL source strings | Compile the existing Shader Model 5 profiles to bytecode arrays. |
| Vulkan | Already-compiled SPIR-V (Standard Portable Intermediate Representation) arrays | Already implemented by `backends/tools/compile_vulkan_shaders.py`. |
| OpenGL 3 / OpenGL ES 3 | OpenGL Shading Language (GLSL) source strings | Keep portable source as the fallback; program binaries can be a local driver-specific cache. |
| OpenGL 2 | Fixed-function presentation, without a programmable shader pipeline | No shader to precompile in the current implementation. |

`backends/tools/embed_shaders.py` embeds **source**, except that it also checks the
separately generated Vulkan bytecode for freshness. It does not precompile the
Direct3D renderers today.

The Direct3D compiler can emit byte arrays in C headers. This works with a plain
Clang or GCC application: shader compilation is a separate build step and does
not require compiling the application with Microsoft's C++ compiler. The desired
follow-up is a checked-in bytecode header and source/profile hash per shader, a
freshness check, and direct shader creation from those arrays. Cover every shader,
including text, images, layer composition and optional compute effects, before
removing the runtime compiler dependency. See
[Microsoft's shader compilation documentation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-part1).

Use `unsigned char`/byte arrays for Direct3D blobs with the required alignment;
Vulkan's shader-module input is an array of 32-bit words. Its compiled format is
portable within the capabilities requested by the shader, as described in
[Vulkan shader modules](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/02_Graphics_pipeline_basics/01_Shader_modules.html).
OpenGL program binaries can become incompatible after hardware or driver changes;
they cannot replace portable source for all supported machines. See the program
binary rules in the [OpenGL specification](https://registry.khronos.org/OpenGL/specs/gl/glspec43.compatibility.pdf).

Precompilation reduces startup work and deployment dependencies. It does not
remove per-frame CPU rasterization, GPU readbacks, uploads or full-screen blur.
Those costs need renderer/compositor changes independently of how shaders are
packaged.
