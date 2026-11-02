# Backends

API means application programming interface; OS means operating system.
CPU means central processing unit; GPU means graphics processing unit.
HLSL is High-Level Shading Language and GLSL is OpenGL Shading Language.

- `renderers/<api>/`: one graphics API implementation shared by standalone and
  embedded applications. API-specific atlases and effects live beside it.
- `renderers/<api>/shaders/`: editable HLSL/GLSL sources and `.in` templates;
  `generated/` contains their checked-in C headers.
- `platform/<os>/`: OS window, event-loop, input and filesystem integration.
  The Windows executable entry point is `platform/win32/main_entry_win32.c`.
- `embedded/`: adapters for host-owned graphics devices, host state preservation
  and embedded Win32 input translation. See [embedded/README.md](embedded/README.md).
- `common/`: CPU blur/mipmap helpers, the visual contract, the null atlas,
  profiling declarations and shared filesystem/startup helpers.
- `tools/`: shader embedding and freshness checks. See [tools/README.md](tools/README.md).

Standalone applications select a renderer and platform. Embedded applications
select that same renderer, an embedded adapter and their OS filesystem adapter;
the host owns the window and frame loop. There is no separate `standalone/`
source directory and no duplicate embedded copy of a renderer or its shaders.

Use `cmake/meek_ui.cmake` to select sources and include directories. Public header names and C entry points remain unchanged.

OpenGL 2 uses the portable CPU painter and a fixed-function texture quad, so it
has no shader folder. The desktop OpenGL 3 renderer requires version 3.3 or later.
Both have separate embedded adapters and Linux examples. Vulkan also has an
embedded adapter for Windows and Linux. OpenGL ES (Embedded Systems) 3 has a
separate context-adoption adapter, tested with Linux EGL (the Khronos native
platform interface). See the embedded documentation for host requirements.

`renderers/<api>/shaders/` contains each API's drawing and effect shaders. Regenerate
their checked-in C strings with `python backends/tools/embed_shaders.py` after edits;
applications never load shaders relative to their working directory. Vulkan's
SPIR-V (Standard Portable Intermediate Representation) bytecode is also checked
in: use `compile_vulkan_shaders.py` when changing its shader sources. A shader
compiler is needed during regeneration, not when launching the application.

The optional Windows compute engine in `renderers/d3d11/effects_d3d11.c` accelerates
large transformed/blended CPU effect surfaces and layered gradient backgrounds. It is independent of the
presentation renderer and owns its own hardware D3D11 device/context. All work
is submitted by the drawing thread. Embedded adapters retain their existing
host-state contract, and D3D9 resets do not invalidate this separate device.
This is an incremental path with upload/readback overhead, not a fully GPU
resident CSS (Cascading Style Sheets) compositor.

Layer mode (`docs/rendering-layers.md`, 2026-09-19) is the step toward that
compositor: a backend that implements `renderer_backend__supports_layers`,
`create_layer_texture`, `update_layer_texture`, `submit_layer`, `read_region`,
`begin_layer`, `end_layer` and `submit_tiles` (D3D11 and OpenGL 3 today) draws
extended scenes itself, with effect groups as cached textured quads or
backend-rendered layers and the tiled, offset and repeating gradient
backgrounds of plain nodes as grids of tessellated tiles clipped to the
rounded box; the others return FALSE from `supports_layers` and keep the
whole-frame CPU compositor. Porting a backend means one homogeneous-quad
pipeline modeled on its image pipeline, a back-buffer region read-back, a
pooled render target and the tile loop over its rect pipeline.

Select it before `platform__init` or an embedded `attach` call:

```c
renderer__set_effect_acceleration(GUI_EFFECT_ACCELERATION__D3D11_COMPUTE);
```

CPU effects are the default. The engine loads Direct3D 11/compiler DLLs (dynamic-link libraries) from System32
only after explicit selection and a suitable effect is encountered. It requires
a hardware feature-level 11.0 device; it does not select the WARP CPU renderer.
Small/oversized surfaces and failures use the portable CPU path. Shutdown frees
the engine and restores the default selection; select again before reattaching.
Other operating systems currently retain CPU effects. CMake includes the engine
in every Windows standalone/embedded configuration without new import libraries.

Background compute uses premultiplied ramps and rounded coverage. Conic layers
reuse exact CPU coordinate fields; their first paint and coordinate-cache misses
stay on CPU. Discontinuous/repeating linear or radial ramps also stay on CPU
because shader division can otherwise move sharp boundaries. The engine does
not mark CPU background caches valid after a GPU-only paint. Source textures
are reused for stacked conic coordinate fields, within the existing size bound.

Build and runtime verification, including WSL setup and commands:
[backend layout checks](../tests/visual/backend-layout.md).

D3D9, D3D11 and OpenGL 3 implement GPU layer composition, render-to-layer groups and tiled backgrounds. Their embedded adapters compile these same paths. D3D12, Vulkan and GLES3 still use the CPU compositor for extended effects. OpenGL 2 intentionally uses fixed-function presentation.

## Native backdrop filtering

The optional GPU-effects setting now supports readback-free frame backdrop blur
in D3D9, D3D11 and OpenGL 3, shared with embedded rendering. See
[activation, supported targets and regression tests](../docs/gpu-backdrop-blur.md).
Measure total frame time including presentation, because asynchronous GPU work
can shift waiting from the painting stage into the presentation stage.
