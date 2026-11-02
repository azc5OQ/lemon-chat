# Embedded renderers

An embedded renderer draws meek-ui inside an application that already owns a window and graphics device. It wraps the implementation under `../renderers/`; it is not a second copy of the renderer or shaders. The host chooses when to draw, forwards input and presents. The adapters contain no injection or function-hooking code.

UI (user interface) drawing stays on the calling thread. Keep borrowed devices, contexts and queues alive until detach. Adapters never clear the host image, create a host window or present a frame. They release only their own resources.

## Available adapters

API means application programming interface. CPU means central processing unit; GPU means graphics processing unit. OpenGL is the Open Graphics Library; OpenGL ES (Embedded Systems) has its own adapter.

| Adapter | Host requirements | Drawing and host state |
|---|---|---|
| `embedded_d3d9` | Windows, Direct3D 9 device; draw between host `BeginScene` and `EndScene` | State block plus explicit transforms and viewport restoration. Device loss/reset callbacks are required. |
| `embedded_d3d11` | Windows, Direct3D 11 device, immediate context, swapchain | Saves and restores shader, resource, input, rasterizer, output and predication state. Drawing is unconditional during the UI frame. Release its render-target view before host resize. |
| `embedded_d3d12` | Windows, Direct3D 12 device, graphics queue, swapchain | Private command lists on the host queue. Requires the back buffer in `PRESENT` state. Release overlay views before resize. |
| `embedded_opengl2` | Windows or Linux, desktop OpenGL 2.0 or newer compatibility context | CPU painting and a fixed-function texture quad. No OpenGL 3 functions or shaders. Saves legacy matrices, client/server attributes, buffers and applicable extension state. |
| `embedded_opengl3` | Windows or Linux, desktop OpenGL 3.3 or newer | Shared modern renderer; saves program, vertex array, buffers, pixel stores, texture/sampler, framebuffers and raster/sample state. Host coverage/masks and alpha-to-one are disabled for UI drawing and restored afterward. |
| `embedded_gles3` | Current OpenGL ES 3.0 or newer context; Linux example supplied | CPU painting through the shared GLES renderer; preserves the GLES state it changes. No desktop OpenGL loader. |
| `embedded_vulkan` | Windows or Linux, physical device, logical device, graphics queue and color target | CPU painting through the shared Vulkan renderer; private command buffers. Host command-buffer bindings are untouched. |

OpenGL 2 and OpenGL 3 are different contracts. The old Windows context constructor can expose a newer compatibility implementation, but does not guarantee OpenGL 3.3. The OpenGL 3 adapter checks the actual version. OpenGL 2 rejects core-only and OpenGL ES contexts.

The OpenGL adapters target the window's default framebuffer. Keep the same context current on the calling thread. A null desktop loader uses WGL (Windows Graphics Library) on Windows or GLX (OpenGL Extension to the X Window System) on Linux. EGL (the Khronos native platform interface) hosts supply an `eglGetProcAddress` wrapper to `attach_with_loader`. GLES links its functions directly. End active transform feedback before GLES attachment or drawing; an unpaused operation is rejected without changing host state.

Linux GLES is tested. Android can use the same context-adoption design, but no Android device test is claimed here. Windows GLES requires host-provided headers and an implementation such as ANGLE (Almost Native Graphics Layer Engine), plus the host's EGL setup. There is no standalone Windows EGL platform or tested ANGLE host example yet.

## Integrating into your project with Clang

To compile meek-ui's `.c` files directly into your own executable or plugin DLL, see [direct source integration](direct-source-integration.md). It includes a commented CMake recipe, manual source selection, compile definitions and per-backend dependencies, with no separate meek-ui archive.

Include the header for the host's graphics API: `embedded_d3d9.h`, `embedded_d3d11.h`, `embedded_d3d12.h`, `embedded_opengl2.h`, `embedded_opengl3.h`, `embedded_gles3.h` or `embedded_vulkan.h`. For ordinary interface work include `scene.h`; for direct shapes include `renderer.h`. Add `font.h`, `parser_xml.h`, `parser_style.h`, `animator.h` or `hot_reload.h` when using those functions. Windows input forwarding is declared separately in `embedded_win32_input.h`.

You can compile the core, one renderer and its embedded adapter directly into your executable or plugin. Alternatively, put those same sources into a static library and link it. The normal top-level `meek_ui` target includes standalone window/platform integration, so use `meek_ui_embedded_backend_sources` for this path. The adapter does not discover the host API or install hooks for you. Select one renderer per library build; the generic renderer functions share names and global state.

This CMake fragment builds a Direct3D 11 static library. Add it to your own C project and change only the path to this checkout:

```cmake
# Source selection and public include paths are maintained by meek-ui.
include("path/to/meek-ui/cmake/meek_ui.cmake")

# Core: widgets, layout, styles, input, images, fonts and CPU composition.
# Backend: Direct3D 11 renderer, host-device adapter and file/input helpers.
meek_ui_core_sources(overlay_core)
meek_ui_embedded_backend_sources(overlay_backend d3d11)
meek_ui_renderer_link_libs(overlay_graphics_libraries d3d11)

add_library(meek_overlay STATIC ${overlay_core} ${overlay_backend})
target_compile_features(meek_overlay PUBLIC c_std_11)
target_include_directories(meek_overlay PUBLIC ${MEEK_UI_INCLUDE_DIRS})

# PUBLIC definitions also reach projects linking this CMake target: their
# headers must use the same static-library and allocation-tracking contract.
target_compile_definitions(meek_overlay PUBLIC
    MEEK_UI_STATIC
    MEEK_UI_HOST_OWNS_MAIN
    GUI_TRACK_ALLOCATIONS
)

# Windows system declarations and libraries used by this backend.
target_compile_definitions(meek_overlay PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_link_libraries(meek_overlay PRIVATE
    user32 gdi32 ${overlay_graphics_libraries}
)

# After declaring your application's target elsewhere:
# target_link_libraries(my_application PRIVATE meek_overlay)
```

Configure using ordinary Clang and your existing target/toolchain environment:

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build --target meek_overlay
```

The output extension follows Clang's selected target and archive tools: a Windows target compatible with MSVC (Microsoft Visual C++) commonly uses `.lib`, while a MinGW (Minimalist GNU for Windows) target commonly uses `.a`. MSVC here identifies the binary/toolchain environment; the compiler command above is ordinary `clang`. Build the library and application for the same architecture, target and runtime. Keep any cross-compilation toolchain options you already use.

Linking the CMake target carries its include paths, definitions and required system libraries into your application. If you copy only the archive into another build, also supply those include paths, definitions and libraries yourself. Keep the dependent public headers together; the selected adapter header includes other meek-ui headers.

With those include paths configured, a Direct3D 11 integration starts with:

```c
#include "embedded_d3d11.h"       /* Adopt the host device/context/swapchain. */
#include "embedded_win32_input.h" /* Optional Windows message forwarding. */
#include "scene.h"               /* Nodes, layout and input. */
#include "renderer.h"            /* Direct lines, triangles and other shapes. */
```

Initialize once, attach to the host objects, draw between the adapter's frame calls, forward input, and detach before those objects are destroyed. The following sections explain the actual per-backend calls, presentation hooks and resize/reset handling. The complete [Direct3D 11 example](../../examples/embedded_d3d11/main.c) shows a host that owns its window and graphics objects; replacing that host with your own application does not change the adapter contract. Linux OpenGL/GLES and Vulkan builds need their corresponding system libraries; their example `CMakeLists.txt` files show those dependencies.

## Host sequence

For Windows message consumption, subclass installation/removal and the Linux event-dispatch equivalents, see [input routing](input-routing.md). The Windows helper preserves click ownership; the current Linux example translator is not yet an equivalent input filter.

For a smaller menu or several movable panels inside the host target, see [panels, title bars and dragging](panels-and-dragging.md). It separates supported layout/input behavior from the title-bar drag feature that still needs implementation.

If the host has a memory manager of its own, hand meek-ui its functions first (see [memory manager wiring](memory-manager-wiring.md)). Then initialize the memory manager and widget registry, and attach the adapter. For OpenGL, keep resource creation, hot reload, layout, drawing and resource destruction inside `begin_frame`/`end_frame`. Uploads must not inherit host pixel-transfer state or leave texture bindings changed.

```c
host_services__set(&services);   /* optional: the host's memory manager, log sink, file reader */
memory_manager__init();          /* the memory mode is fixed here */
widget_registry__bootstrap_builtins();
if (!embedded_opengl2__attach()) { /* handle unsupported context */ }

embedded_opengl2__begin_frame(width, height);
font__init();
font__register_dir("fonts");
hot_reload__watch_style("overlay.style");
hot_reload__watch_ui("overlay.ui");
embedded_opengl2__end_frame();

/* After host drawing, before host presentation: */
scene__begin_frame_time(now_ms);
if (embedded_opengl2__begin_frame(width, height))
{
    hot_reload__tick();
    scene__resolve_styles();
    animator__tick();
    scene__layout(width, height);
    scene__emit_draws();
    embedded_opengl2__end_frame();
}

/* While the host context is still current: */
embedded_opengl2__begin_frame(width, height);
hot_reload__shutdown();
font__shutdown();
embedded_opengl2__end_frame();
embedded_opengl2__detach();
memory_manager__shutdown();      /* meek-ui's last call; the host's manager may shut down after it */
```

The examples contain full error handling and cleanup. Register named handlers with `scene__register_handler`; there is no host export lookup. On Windows, `embedded_win32__on_window_event` forwards input and reports whether meek-ui consumed it. Other hosts forward events through the scene input functions. Input translation is separate from context adoption.

Direct3D 9: call `on_device_lost` before `Reset`, then `on_device_reset` after success. Direct3D 11/12: call `release_render_target` / `release_render_targets` before `ResizeBuffers`; the next frame rebuilds views. OpenGL: supply the current drawable dimensions each frame.

## Hook integration: which function, and what to call inside it

A hook intercepts a host function, does the overlay work at that boundary, then calls the original function. The following recipes describe that caller-side code. Hook installation, finding the intended window and keeping original function pointers are the integrator's responsibility; none of those operations happens inside `embedded_*__attach`.

The back buffer is the image being prepared for display. A swapchain is the set of images the host rotates through when presenting frames. DXGI means DirectX Graphics Infrastructure, the Windows interface used for Direct3D 11/12 swapchains.

| Backend | Draw hook: insert overlay **before** calling the original | Other boundaries to track |
|---|---|---|
| Direct3D 9 | Prefer `IDirect3DDevice9::EndScene`; alternatively device/swapchain `Present` with a valid caller-managed scene pair | `IDirect3DDevice9::Reset`; device replacement/destruction. The 9Ex methods are `PresentEx` / `ResetEx`. |
| Direct3D 11 | `IDXGISwapChain::Present`, or `IDXGISwapChain1::Present1` when the host uses it | `ResizeBuffers`, or `IDXGISwapChain3::ResizeBuffers1`; device/swapchain replacement/destruction. |
| Direct3D 12 | `IDXGISwapChain::Present`, or `IDXGISwapChain1::Present1` | Swapchain creation to identify its direct command queue; `ResizeBuffers` / `ResizeBuffers1`; device/queue/swapchain lifetime. |
| Desktop OpenGL 2 | Windows `SwapBuffers`; Linux `glXSwapBuffers`; EGL hosts `eglSwapBuffers` | Context and drawable creation, binding, resize and destruction. |
| Desktop OpenGL 3 | Windows `SwapBuffers`; Linux `glXSwapBuffers`; EGL hosts `eglSwapBuffers` | The same boundaries as OpenGL 2, with a desktop OpenGL 3.3+ context. |
| OpenGL ES 3 | `eglSwapBuffers` in an EGL host | EGL context/surface creation, binding, resize and destruction. |
| Vulkan | `vkQueuePresentKHR`, after host rendering has been submitted | Device/queue discovery, `vkCreateSwapchainKHR`, `vkGetSwapchainImagesKHR`, image views, `vkDestroySwapchainKHR`, and synchronization described below. |

These are the ordinary presentation paths. A host using an extension path, such as `wglSwapLayerBuffers` or `eglSwapBuffersWithDamageKHR` / `eglSwapBuffersWithDamageEXT`, needs that actual entry point intercepted too. Draw once per selected presentation; do not draw twice when one intercepted entry point calls another. Damage rectangles describe the part of an image that changed; their handling also has to include the overlay.

### Conventions used in the recipes

The C fragments are hook-body recipes, not a complete hook library. Each backend is an alternative build. `original_*` names stand for saved, unhooked host function pointers with the original calling convention and arguments. `width`, `height` and `now_ms` are caller-supplied target dimensions and monotonic time in milliseconds. The caller has already selected the intended device, window/surface and swapchain, checked that the target is usable and nonzero in size, and prevented nested overlay calls. Other host objects simply pass through to the original function.

Initialize memory, widgets, fonts and the UI/style as in [Host sequence](#host-sequence), once per overlay lifetime. Attach once after the correct graphics objects exist. Do not initialize, attach, detach or load all files on every presentation. The adapters and scene currently use global state: these recipes cover one active overlay target, not independent simultaneous scenes on every swapchain.

To keep the hook bodies short, this **caller-defined** helper contains the scene work already shown above. It is not a library entry point:

```c
static void overlay__draw(int64 width, int64 height, int64 now_ms)
{
    scene__begin_frame_time(now_ms);
    hot_reload__tick();
    scene__resolve_styles();
    animator__tick();
    scene__layout(width, height);
    scene__emit_draws();
}
```

Run this helper between the chosen adapter's `begin_frame` and `end_frame`. Forward the original presentation even when attachment or `begin_frame` fails. Handle initialization, submission and device-loss errors in the integration; do not keep drawing with invalid borrowed objects.

### Direct3D 9: `EndScene` or `Present`, plus `Reset`

Attach with `embedded_d3d9__attach(device)` on the host's graphics thread. In the selected device's `EndScene` hook:

The shader-based backend requires vertex/pixel shader model 3.0. The adapter
sets the complete state used by its triangle passes each frame: rasterization,
depth/stencil/alpha testing, blend operations, color masks, multisample mask,
texture-coordinate wrapping, stream frequencies, and sampler defaults.
`D3DSBT_ALL` restores host shaders, declarations, streams, textures, constants,
render/sampler state; transforms and viewport also have explicit backups when
the device supports their getters. Pure devices rely on the state block.
Render targets, depth surface and mixed-device software vertex processing are
saved separately. Extra render targets and depth are detached during UI drawing
and restored afterward. A lost device rejects frames until reset succeeds.

The injectable test in `tests/example_overlay` discovered a runtime where each
device has a private vtable: modifying a dummy device's vtable does not hook the
host. Its optional MinHook EndScene discovery obtains the live device before
patching that device's Present/Reset slots. This discovery code is confined to
the test DLL, not the library adapter. See that test's README for reproduction.

```c
/* The host has successfully called BeginScene and finished its drawing. */
if (embedded_d3d9__begin_frame(width, height))
{
    overlay__draw(width, height, now_ms);
    embedded_d3d9__end_frame();
}
return original_EndScene(device);
```

The adapter does not call `BeginScene` or `EndScene`. Drawing belongs inside the host's existing pair, so putting this block after the original `EndScene`, or directly in `Present` with no active scene, is incorrect. Prefer the final host scene that produces the displayed image. If several scenes or swapchains are involved, track the chosen presentation instead of treating every `EndScene` as a new displayed frame. See the [Direct3D 9 scene contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-endscene).

This adapter draws into the currently bound render target; it does not select the host's final back buffer. Verify the target at the hook point. If it is an intermediate image, choose a later integration point or explicitly save, bind and restore the intended color/depth targets in caller code. Obtain dimensions from that target's `GetDesc` (`GetRenderTarget` or the selected swapchain's `GetBackBuffer`), and release temporary references.

Alternatively, hook `IDirect3DDevice9::Present`. This requires a hook point outside an active host scene and the intended back buffer already bound (or explicitly bound and restored by the caller). The hook establishes its own scene pair; the adapter still does not do that for you:

```c
/* Alternative to drawing in EndScene; use one draw path per frame.
   Device-interface C macros require COBJMACROS before the system headers. */
if (SUCCEEDED(IDirect3DDevice9_BeginScene(device)))
{
    if (embedded_d3d9__begin_frame(width, height))
    {
        overlay__draw(width, height, now_ms);
        embedded_d3d9__end_frame();
    }
    /* Saved original: do not re-enter an installed overlay EndScene hook. */
    original_EndScene(device);
}
return original_Present(device, source_rect, destination_rect,
                        override_window, dirty_region);
```

Call the matching `EndScene` only if `BeginScene` succeeded, and forward `Present` even if overlay drawing is skipped. This extra scene pair is why the existing host `EndScene` is the preferred recipe; Microsoft's guidance favors one scene pair per presented frame. Do not render in both hooks. `IDirect3DSwapChain9::Present` is another presentation entry point: use its associated device for the scene/adapter calls, then forward the swapchain method's own arguments, including `flags`. Direct3D 9Ex uses `IDirect3DDevice9Ex::PresentEx`, with its additional flags. Source/destination rectangles and an override window also affect which target is displayed and how mouse coordinates must be mapped.

In the matching `Reset` hook, between overlay frames:

```c
overlay_ready = FALSE;
embedded_d3d9__on_device_lost();
HRESULT result = original_Reset(device, parameters);
if (SUCCEEDED(result))
{
    /* Keep drawing disabled if this returns FALSE. */
    overlay_ready = embedded_d3d9__on_device_reset();
    /* Re-query actual target dimensions and update input mapping. */
}
return result;
```

Here `overlay_ready` is caller-owned state; skip overlay frames while it is false. A Direct3D 9Ex device (`Direct3DCreate9Ex` / `CreateDeviceEx`, which refuses `D3DPOOL_MANAGED`) is recognised at attach time and gets its textures in the default pool; `examples/embedded_d3d9_min --d3d9ex` and `--d3d9ex --reset` validate that path, including a `Reset` with those textures alive. A host using 9Ex still has to route its `ResetEx` / `PresentEx` through the same lost / reset calls. Shut down overlay resources and call `embedded_d3d9__detach()` before the borrowed device's lifetime ends.

### Direct3D 11: hook `Present` and `ResizeBuffers`

Direct3D 11 has no `BeginScene`, `EndScene` or Direct3D 9-style device `Reset` method. Use the swapchain presentation and resize methods instead. A lost/removed device needs host device recreation, not just a buffer resize; release the old overlay resources, detach, and attach/rebuild resources against the replacement objects.

From the selected swapchain, obtain its `ID3D11Device` with `GetDevice`, then its immediate context with `GetImmediateContext`. Keep those objects alive and call `embedded_d3d11__attach(device, context, swapchain)` once. Query the actual back-buffer texture using `GetBuffer` and `GetDesc`; release the temporary reference after reading its size.

In the selected swapchain's `Present` hook, after host drawing:

```c
if (!(flags & DXGI_PRESENT_TEST) &&
    embedded_d3d11__begin_frame(width, height))
{
    overlay__draw(width, height, now_ms);
    embedded_d3d11__end_frame();
}
return original_Present(swapchain, sync_interval, flags);
```

`DXGI_PRESENT_TEST` checks presentation status without displaying a frame. The adapter binds its view of the swapchain image and restores the host state it changes. A `Present1` hook uses the same draw block, then calls `original_Present1(swapchain, sync_interval, flags, present_parameters)`.

In the selected swapchain's `ResizeBuffers` hook, with no overlay frame open:

```c
embedded_d3d11__release_render_target();
HRESULT result = original_ResizeBuffers(swapchain, count, width,
                                       height, format, flags);
/* On success, re-query the actual buffer size, then update input mapping.
   The next begin_frame rebuilds the overlay's render-target view. */
return result;
```

Use the same release-before-original order for `ResizeBuffers1`, preserving its extra arguments. The caller and host must also release their own old buffer references and bindings; the adapter only releases its own. Width/height arguments of zero are requests for DXGI to choose the dimensions, not the dimensions to pass to layout. See [ResizeBuffers reference rules](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers).

For `Present1`, dirty rectangles and scroll metadata must describe the overlay changes as well as the host changes. Updating just the rectangles is insufficient if the underlying pixels were not repainted: moving or hiding an overlay needs the old area restored too. These recipes assume the host redraws the complete background each frame; partial redraw integration is caller work. See [Present1's update-region contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiswapchain1-present1). Shut down resources and call `embedded_d3d11__detach()` before device/swapchain replacement or destruction.

### Direct3D 12: hook `Present`, retain the correct command queue

Direct3D 12 also has no `BeginScene`, `EndScene` or device `Reset` method. `ID3D12GraphicsCommandList::Reset` and `ID3D12CommandAllocator::Reset` recycle command-recording storage; they are not window-resize or device-reset hooks. Leave the host's command lists and allocators alone. Device removal requires host recreation followed by overlay reattachment and resource setup.

The presentation hook resembles Direct3D 11, but attachment additionally requires the host's **direct command queue**. Capture the queue associated with the selected swapchain at `IDXGIFactory::CreateSwapChain` or the applicable `IDXGIFactory2::CreateSwapChainForHwnd`, `CreateSwapChainForCoreWindow` or `CreateSwapChainForComposition`. In Direct3D 12 their `pDevice` argument is a direct command queue. `swapchain->GetDevice` gives a device, not that queue. Merely observing an arbitrary `ExecuteCommandLists` call does not establish which queue belongs to this swapchain. See [Direct3D 12 swapchains](https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains).

Call `embedded_d3d12__attach(device, direct_queue, swapchain)` once. At the hook point, host rendering must already be submitted and ordered before overlay work on that queue, and the current back buffer must be in `D3D12_RESOURCE_STATE_PRESENT`:

```c
if (!(flags & DXGI_PRESENT_TEST) &&
    embedded_d3d12__begin_frame(width, height))
{
    overlay__draw(width, height, now_ms);
    embedded_d3d12__end_frame();
}
return original_Present(swapchain, sync_interval, flags);
```

The adapter selects the current swapchain buffer and submits its own command list on the adopted queue, leaving the image in `PRESENT` state. Do not record overlay commands into the host's command list or close/reset it. Work from another queue must already have the required ordering before this point. The `Present1` and partial-redraw rules above also apply here.

In `ResizeBuffers` / `ResizeBuffers1`, call `embedded_d3d12__release_render_targets()` **before** the original method. It waits for the adapter's outstanding work and drops its buffer references; the host still handles its own outstanding work and references. After success, read the new buffer resource's `GetDesc`, update dimensions/mapping, and let the next `begin_frame` rebuild views. If the swapchain's associated queue changes, reattach to the new queue. Call `embedded_d3d12__detach()` before borrowed objects are destroyed.

### OpenGL 2: hook the window-system swap, not an OpenGL draw call

For a Windows host, the usual hook is `SwapBuffers(HDC dc)`. HDC means handle to a device context. `wglSwapBuffers` is not the ordinary Windows entry-point name. Check `wglGetCurrentContext()` and `wglGetCurrentDC()` to identify the intended current context and drawable. Use `WindowFromDC` when the drawable is a window and its identity is needed. Attach with `embedded_opengl2__attach()` while that context is current, then initialize resources inside a successful adapter frame as shown in [Host sequence](#host-sequence).

The drawing part of the Windows hook is:

```c
/* In the chosen window's SwapBuffers hook, with its context current. */
if (embedded_opengl2__begin_frame(width, height))
{
    overlay__draw(width, height, now_ms);
    embedded_opengl2__end_frame();
}
return original_SwapBuffers(dc);
```

That is the complete per-frame ordering: host draws, hook enters, overlay begins/draws/ends, original `SwapBuffers` runs. Do not call `glClear`, create a second context, call the original swap from the draw helper, or swap twice. The adapter handles the legacy matrix, texture and other drawing state it changes. It targets the default window framebuffer, even if the host left an offscreen framebuffer bound. `glBegin`, `glEnd`, `glDrawArrays` and `glFlush` are not reliable whole-frame boundaries and are not the hooks used by this recipe.

For Linux GLX, use `glXSwapBuffers(Display *display, GLXDrawable drawable)`. Verify `glXGetCurrentContext()` / `glXGetCurrentDrawable()`, call the same `embedded_opengl2__begin_frame` / draw / `end_frame` block, then `original_glXSwapBuffers(display, drawable)`; this function returns `void`. Default attachment uses the GLX loader.

For desktop OpenGL through EGL, use `eglSwapBuffers(EGLDisplay display, EGLSurface surface)`. Verify `eglGetCurrentContext()` and `eglGetCurrentSurface(EGL_DRAW)`. Attach with `embedded_opengl2__attach_with_loader(resolver)`, where `resolver` is the caller's `meek_opengl_get_proc_fn` wrapper around `eglGetProcAddress`. Call the same frame block, then `return original_eglSwapBuffers(display, surface)`. Select this adapter only for a desktop compatibility context, not an OpenGL ES context.

No adapter reset is needed for a window resize with the same context. Supply the new drawable size to both `begin_frame` and layout. For context lifetime, observe the host's binding/destruction path: WGL `wglMakeCurrent` / `wglDeleteContext`, GLX `glXMakeCurrent` or `glXMakeContextCurrent` / `glXDestroyContext`, or EGL `eglMakeCurrent` / `eglDestroyContext` and surface destruction. Release fonts/UI resources inside a final adapter frame and call `embedded_opengl2__detach()` **while the original context is still valid and current**, before it is unbound for the last time or destroyed. A destruction hook may run with no suitable context current, so it can be too late to start cleanup there. A new context requires fresh renderer attachment and resource setup.

### OpenGL 3: the same swap hooks, the modern adapter calls

Use the same Windows `SwapBuffers`, Linux `glXSwapBuffers` or desktop-EGL `eglSwapBuffers` boundaries and context checks as OpenGL 2. Attach once with `embedded_opengl3__attach()` for WGL/GLX, or `embedded_opengl3__attach_with_loader(resolver)` for EGL. The current context must actually provide desktop OpenGL 3.3 or newer.

```c
/* Windows SwapBuffers hook body, after selecting the matching context. */
if (embedded_opengl3__begin_frame(width, height))
{
    overlay__draw(width, height, now_ms);
    embedded_opengl3__end_frame();
}
return original_SwapBuffers(dc);
```

In GLX use that frame block before `original_glXSwapBuffers(display, drawable)`; in EGL use it before `return original_eglSwapBuffers(display, surface)`. Resize and cleanup ordering are the OpenGL 2 sequence with `embedded_opengl3__detach()`. Do not mix OpenGL 2 attachment with OpenGL 3 frame functions. The modern adapter restores its shader, vertex-array, framebuffer and other changed state; the caller still supplies the correct context and drawable dimensions.

### OpenGL ES 3: hook `eglSwapBuffers`

With the intended OpenGL ES 3.0+ context current on its draw surface, call `embedded_gles3__attach()` once and initialize resources inside an adapter frame. There is no desktop OpenGL function loader in this adapter.

```c
/* In the matching EGL surface's swap hook. */
if (embedded_gles3__begin_frame(width, height))
{
    overlay__draw(width, height, now_ms);
    embedded_gles3__end_frame();
}
return original_eglSwapBuffers(display, surface);
```

Read the draw surface's dimensions with `eglQuerySurface(display, surface, EGL_WIDTH, &w)` and `EGL_HEIGHT`; a native window resize can precede the actual surface resize. Handle `eglSwapBuffersWithDamageKHR` / `EXT` if used, with damage covering the overlay too. Before final unbinding or `eglDestroyContext` / `eglDestroySurface`, release overlay resources with the context and surface valid, then call `embedded_gles3__detach()`. On context loss, stop drawing and rebuild for the replacement context; these adapters do not automatically recover an externally lost context. The [EGL specification](https://registry.khronos.org/EGL/specs/eglspec.1.5.pdf) describes surface-size queries and resize behavior. Platform validation limits remain those listed under [Available adapters](#available-adapters).

### Vulkan: hook `vkQueuePresentKHR` with swapchain and wait tracking

Presentation alone does not provide everything needed for attachment. Retain the physical/logical device association from `vkCreateDevice`, queue handles and families from `vkGetDeviceQueue` / `vkGetDeviceQueue2`, and the selected swapchain's format, extent, usage and sharing mode from `vkCreateSwapchainKHR`. Get its images through `vkGetSwapchainImagesKHR`, and provide suitable image views for them. If caller code creates those views, caller code must destroy them after overlay work finishes. The adapter does not create or own them.

Call `embedded_vulkan__attach(&config)` once, where `config` contains the physical device, logical device, graphics queue, its family and the target color format. Inside `vkQueuePresentKHR`, `pPresentInfo->pImageIndices[i]` selects an already acquired image of `pSwapchains[i]`; do not acquire it again. `vkAcquireNextImageKHR` / `vkAcquireNextImage2KHR` hooks can track acquisition in a broader integration, but the present request itself supplies the image index needed here.

The following recipe deliberately covers **one swapchain per present call**, ordinary presentable images, and a graphics queue that already owns the image (or concurrent sharing). `image`, `view`, dimensions and `usage` come from the tracked swapchain and presented index. The host has finished submitting its image work. Its present waits express the dependencies the overlay must wait for:

```c
meek_vulkan_frame_t frame = {0};
frame.image = image;
frame.view = view;
frame.width = width;
frame.height = height;
frame.initial_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
frame.final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
frame.wait_count = present_info->waitSemaphoreCount;
frame.wait_semaphores = present_info->pWaitSemaphores;
frame.usage = usage;

if (!embedded_vulkan__begin_frame(&frame))
{
    /* No submission: forward the original waits unchanged. */
    return original_vkQueuePresentKHR(present_queue, present_info);
}
overlay__draw(width, height, now_ms);
if (!embedded_vulkan__end_frame())
{
    /* Host-defined error recovery: waits may already have been consumed.
       Do not blindly retry presentation with the original waits. */
    return overlay__handle_vulkan_submission_failure();
}

VkPresentInfoKHR forwarded = *present_info;
forwarded.waitSemaphoreCount = 0;
forwarded.pWaitSemaphores = NULL;
return original_vkQueuePresentKHR(present_queue, &forwarded);
```

`overlay__handle_vulkan_submission_failure()` is a caller-defined placeholder returning `VkResult`, not an adapter API. On success, `end_frame` has consumed the supplied binary semaphores and waited for overlay completion, so the copied present request must not wait on them again. Preserve the caller's remaining presentation fields and per-swapchain result storage. See [Vulkan present parameters](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentInfoKHR.html).

Do not apply this snippet independently to several swapchains sharing one present wait list: that would consume the same waits repeatedly. Multi-swapchain batching, incremental presentation regions, shared-present layouts, rotated surfaces, and exclusive graphics-to-present queue-family transfers need additional caller integration. The adapter does not supply those policies. In particular, a queue that can present but cannot draw is not sufficient for `config.queue`.

Track `vkCreateSwapchainKHR` / `vkDestroySwapchainKHR`, including successful replacement through `oldSwapchain`. A same-format replacement supplies new image/view handles and extent on the next frame. A format or adopted device/queue change requires detach/reattach. Do not destroy old views/images while the overlay or host still uses them. Shut down overlay resources and call `embedded_vulkan__detach()` before `vkDestroyDevice` ends the borrowed device's lifetime. `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` are handled by the host's swapchain recreation path; the overlay does not recreate the host swapchain. See [ownership and synchronization](#vulkan-ownership-and-synchronization) for the full adapter contract.

### Resolution and mouse coordinates at the hook boundary

Refresh the actual target size after successful reset/resize or surface replacement, and whenever the host changes its drawable size. A Windows `WM_SIZE` notification is a useful signal; the render target remains the source of truth. Use Direct3D buffer descriptions, GLX drawable geometry, or EGL surface queries as appropriate. WGL window drawables use their client extent in the correct Windows coordinate space. Do not infer full-window dimensions from `GL_VIEWPORT`: the host may have left a small offscreen-pass viewport set. Do not use a game's internal low-resolution image size when drawing the overlay onto its final upscaled back buffer.

Pass the same target width/height to the adapter and `scene__layout`. Update the client-to-target mapping at the same boundary so input is not tested against stale layout dimensions. A minimized, unavailable or zero-size target skips overlay drawing while the original host call continues normally. There is no automatic resize detector in the embedded adapters, and `scene__on_resize` is currently a stub rather than a replacement for layout.

On Windows, forward messages from the selected window procedure, often called `WndProc`. For matching one-to-one client/target pixels on the same thread, `embedded_win32__on_window_event(hwnd, msg, wparam, lparam)` forwards mouse movement, the left button and vertical wheel; wheel positions are converted from screen to client coordinates. Its return value tells the caller whether the helper considers the message consumed. Forward unconsumed messages to the original window procedure using `CallWindowProc`. Its current keyboard/character gate checks `scene__has_char_redirect()`, not focused-widget state; it is not a complete automatic keyboard-capture policy.

```c
/* In the selected WndProc hook; overlay_ready is caller-owned state.
   Use this direct route only on the scene thread with matching pixels. */
if (overlay_ready &&
    embedded_win32__on_window_event(hwnd, msg, wparam, lparam))
{
    return 0;
}
return CallWindowProc(original_WndProc, hwnd, msg, wparam, lparam);
```

The helper does **not** map stretched or offset client coordinates into render-target pixels. In those hosts, translate input yourself and call `scene__on_mouse_move`, `scene__on_mouse_button` and `scene__on_mouse_wheel` with target-space coordinates. For an unrotated target displayed in a client rectangle `(left, top, content_width, content_height)`:

```c
/* All client values use the same coordinate units; extents are positive. */
double target_x = (client_x - left) * (double)width  / content_width;
double target_y = (client_y - top)  * (double)height / content_height;
```

For example, the center `(640, 360)` of a 1280 x 720 client rectangle maps to `(960, 540)` on a 1920 x 1080 target. The scene input API takes integer pixels with a top-left origin; do not flip the mouse vertically just because the graphics backend is OpenGL. Ignore outside-content hits when not dragging, but preserve captured drag/release delivery outside that rectangle. A rotated presentation requires the corresponding inverse rotation as well. The current adapters do not discover these presentation transforms.

DPI (dots per inch) affects how Windows reports coordinates and how large UI should appear. Handle `WM_DPICHANGED` when applicable to the host, and keep pointer positions and the client rectangle in the same DPI-awareness coordinate space. `scene__set_scale` scales logical style dimensions into target pixels; input is already in target pixels and must not be divided by that scale a second time. Respect the host's existing DPI policy rather than changing process-wide awareness from the overlay. See [Windows DPI coordinate behavior](https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows).

Input and rendering must use the same scene thread. The Windows helper calls the scene synchronously; it contains no event queue or synchronization. If `WndProc` runs on another thread than the presentation hook, transfer events to the drawing thread using the host's event mechanism, then map and dispatch them there. Preserve event order and update layout before hit testing after a resize. Do not call the scene concurrently from both hooks. The helper cancels interrupted presses on capture loss, cancel-mode and focus loss; host raw/relative input, hidden or locked cursors and broader focus/consumption policy still need caller handling. The X11 input helper in the examples assumes matching coordinates and forwards events unconditionally; it is not an automatic Linux injection/input filter. See [input routing](input-routing.md) for the implemented ownership rules and platform limits.

## Vulkan ownership and synchronization

`meek_vulkan_config_t` describes borrowed device and queue handles. `meek_vulkan_frame_t` supplies the image, view, size, actual initial and desired final layouts, usage flags, and binary wait semaphores. The target must be single-sample, usable as a color attachment and already owned by the graphics queue family, or use concurrent sharing. Capture also requires `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`.

Targets can use RGBA (red, green, blue, alpha) or BGRA (blue, green, red, alpha) byte formats, with UNORM (unsigned normalized) or sRGB (standard red, green, blue) encoding. For an sRGB target, the source is decoded before linear-light blending. Translucent overlaps can therefore differ from the default byte-oriented UNORM comparison contract.

The adapter loads the existing target, draws over it, then transitions to the requested layout. `end_frame` consumes supplied semaphores and waits for its own submission's fence. The host can then present without waiting on those semaphores again. Do not reuse a consumed binary wait twice.

This synchronous first implementation keeps private resources and borrowed image views safe at return. Normal embedded cleanup does not use whole-device idle waits. The host serializes access to the borrowed queue. The adapter never changes or submits host command buffers, or owns the host device, queue, swapchain, images or views. A same-format resize supplies new handles; changing format requires detach and reattach.

## Build, effects and diagnostics

`meek_ui_embedded_backend_sources(OUT renderer)` in `cmake/meek_ui.cmake` selects the renderer, adapter, null atlas and filesystem support. Windows also includes input translation. Compile with the core sources and define `MEEK_UI_STATIC` and `MEEK_UI_HOST_OWNS_MAIN`. Do not add a standalone platform entry point or loop. Example CMake files show the system libraries and includes.

CPU effects are the default. Enable supported native GPU effects before attachment; on Windows this also enables the existing separate Direct3D 11 compute engine:

```c
renderer__set_effect_acceleration(GUI_EFFECT_ACCELERATION__GPU);
renderer__set_debug_flags(GUI_RENDERER_DEBUG__FPS);
```

FPS means frames per second. The yellow counter uses the shared renderer API, without font files or widgets. Hosts with their own argument parser call `renderer__parse_launch_flags(&argc, argv)` for `--fps` / `--no-fps`; the examples already do. See [diagnostics](../../docs/renderer-diagnostics.md). The compute engine uses a separate device and remains optional even when presentation uses shaders. See [backends](../README.md).

Native backdrop blur is implemented by the shared Direct3D 9, Direct3D 11 and OpenGL 3 renderers. The embedded adapters use it too. See [supported targets, fallback and tests](../../docs/gpu-backdrop-blur.md).

## Verification

`tests/visual/embedded_ports_check.py` checks cards, public primitives, resize, detach/reattach, and all 22 Motion Lab replay captures. OpenGL hosts seed deliberately unusual state, including pixel buffers, and verify restoration. Linux covers strict OpenGL 2.0, desktop GLX/EGL, strict GLES 3.0 and Vulkan with validation. Windows covers OpenGL 2 and Vulkan, with a separate sRGB run. CPU Motion Lab images are compared within the same platform, dimensions and clock.

The established Direct3D and Windows OpenGL 3 hosts also use `renderer_lifecycle_check.py` and `embedded_primitive_check.py`. These are real host-owned context/device tests, not injection into a third-party application. Android and macOS device checks remain separate work; Metal is currently a [design](../../docs/metal-design.md).

## What the static library needs from its host

A static `meek_ui` resolves on its own except for three things it cannot
implement: a heap, somewhere to put log lines, and reading a file. A host
hands those over once, before any other library call, through
`meek_host_services_t` (`src/host_services.h`), installed with
`host_services__set`. Every slot has its own typedef, so a function with
the wrong signature is a compile error rather than a crash inside the
library.

| Slot | Signature | Required | What it is for |
|---|---|---|---|
| `allocate` | `void* (nuint size, uint type)` | with the next two | `&memory_manager__alloc` of the module initialised first |
| `allocate_zeroed` | `void* (nuint count, nuint size, uint type)` | no | `&memory_manager__calloc`; without it meek-ui zeroes after `allocate` |
| `reallocate` | `void* (void* ptr, nuint new_size)` | with `allocate` | `&memory_manager__realloc` |
| `release` | `void (void* ptr)` | with `allocate` | `&memory_manager__free` |
| `owner` | `const void*` | no | set by `memory_manager__export_services`; leave NULL when filling by hand |
| `write_log` | `void (meek_log_level_e level, const char* file, int line, const char* message)` | no | where finished log lines go |
| `read_file` | `boole (const char* path, ubyte** out_bytes, int64* out_size)` | no | reading a whole file |
| `free_file` | `void (ubyte* bytes)` | with `read_file` | releasing what `read_file` returned |

The heap slots are described in [memory-manager-wiring.md](memory-manager-wiring.md).
The log sink receives each line already formatted, without a trailing
newline, and must not call back into the library. On Windows the file
slots are rarely needed: `fs_win32.c` reads through the Win32 file API
directly, with no C runtime involved.

**Standalone linking.** Use these definitions consistently on the core and embedded backend:

| Switch | Effect |
|---|---|
| `MEEK_UI_STATIC` | Direct/static integration |
| `MEEK_UI_HOST_OWNS_MAIN` | Entry point belongs to the host |
| `MEEK_UI_HOST_SERVICES_FALLBACK=0` | Explicit host logging and filesystem services; disables unsupported image-decoder hosted paths. The heap is governed by the memory manager's own switch below |
| `MEMORY_MANAGER__NO_COMMON_RUNTIME` (presence) | No C runtime: the memory manager makes no `malloc`-family or direct critical-section call. On Windows the copy that owns memory receives `HeapCreate`, `HeapAlloc`, `HeapReAlloc`, `HeapFree`, `HeapDestroy` and the critical-section functions through `memory_manager__init_win32` ([memory-manager-wiring.md](memory-manager-wiring.md), section 4.4). A copy that only delegates needs none of them |
| `NDEBUG` | Disable assertion dependencies in third-party code |
| `STDLIB_DEFINE_MEMORY_HELPERS=1` | Supply compiler-generated memory entry points using the local stdlib |
| `STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS=1` | Enable local Windows stack probes on supported compiler/CPU combinations |

Math is always local; there is no math backend switch. Configuration and numeric
contracts are at the top of `src/clib/stdlib.h`. `stdlib__getenv` returns NULL in
this standalone implementation; process-environment overrides are unavailable.

The implementation has no separate runtime adapters or external assembly files.
`STDLIB_ENABLE_ASSEMBLY_OPTIMIZATIONS` is numeric: 0 (default) disables handwritten
assembly and 1 enables supported paths, including local Windows stack probes
for Clang i386/AMD64/ARM64 (excluding ARM64EC) and MSVC i386. All stdlib switches are documented in
its header. Clang standalone compilation should use `-ffreestanding`
(`/clang:-ffreestanding` with clang-cl); compiler-generated support dependencies
still need an object/link audit for the selected target.

`python tests/freestanding/check.py --compiler clang-cl --arch x86 --assembly 1` compiles every
core and embedded D3D9 object and attempts `/NODEFAULTLIB /OPT:NOREF` linking.
It records linker diagnostics in `build/freestanding-clang-cl-x86-audit/link.log`.
If linking succeeds, it checks imports and executes local stdlib checks. It also
accepts `--compiler msvc` and `--arch x64`. The standalone suites enable
`STDLIB_DEFINE_MEMORY_HELPERS=1`; unresolved symbols remain failures. The full
Clang x86/x64 audit passes with assembly enabled and no CRT imports.
`tests\freestanding_symbols.bat` invokes the same check.

The D3D9 renderer compiles shaders and therefore needs `d3dcompiler.lib` alongside
its normal Windows import libraries. NanoSVG takes input bytes and explicit
allocation hooks; all of its primitive operations use clib directly.

See [stdlib and allocation notes](../../docs/stdlib-and-memory.md) for the test
matrix, configuration and performance measurements.

## Memory manager wiring

A host that already has a memory manager supplies pointers to its
allocation functions (malloc, realloc and free shaped, optionally calloc)
through `host_services__set`, before `memory_manager__init`. meek-ui's
memory manager then forwards every allocation to those functions and
stores no allocation data of its own.

[memory-manager-wiring.md](memory-manager-wiring.md) has the exact types
and signatures, the requirements on the functions (including the calling
convention on 32-bit x86), the order of calls at startup and shutdown, code
for the three cases (host manager in the same module, in another module,
or another copy of meek-ui's memory manager), how to check the result, and
a table of symptoms and their causes.

## Shader packaging

See [shader packaging](shader-packaging.md) for the current embedded source/bytecode formats, offline Direct3D compilation, and the distinction between startup compilation and per-frame rendering costs.

D3D9 now also supports layer rendering through its shared renderer. Its layer shaders, pooled targets and texture updates are included in an embedded build automatically. Keep calling `embedded_d3d9__on_device_lost` before the host resets its device and `embedded_d3d9__on_device_reset` after success. Both color/depth target bindings and the parent scissor stack are restored after rendering a layer. Layer backdrop readback currently requires a non-multisampled A8R8G8B8 or X8R8G8B8 host target; multisampled/HDR host backdrops remain unsupported. The adapter never ends the host scene to perform a resolve.

The optional GPU-effects flag also enables eligible live navigation page layers
in D3D9, D3D11 and OpenGL 3. The page still repaints its animated children; the
backend moves/fades the resulting texture. No additional hook or rendering
thread is required. Hosts can prepare retained-page fonts after resolving styles
with `font__prewarm_tree(nav, scene__scale())`, on the drawing thread while the
graphics context is attached. See [navigation performance and CPU fallback](../../docs/navigation.md#rendering-transitions-efficiently).
