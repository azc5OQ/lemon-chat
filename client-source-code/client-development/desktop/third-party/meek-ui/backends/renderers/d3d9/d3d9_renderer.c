// ============================================================================
// d3d9_renderer.c - Direct3D 9 backend.
// ============================================================================
// WHAT THIS FILE DOES (the elevator pitch):
// scene.c walks the widget tree and tells us "draw this colored rectangle
// here" by calling renderer__submit_rect(). this file packages those calls
// into work the GPU understands and ships them across using Direct3D 9 --
// microsoft's previous-generation graphics api that's still ubiquitous on
// Windows XP/Vista/7 and any GPU that predates D3D10's "feature levels".
// this backend is interchangeable with opengl3_renderer.c and
// d3d11_renderer.c -- exactly one is compiled into gui.dll at a time
// (selected via the GUI_RENDERER CMake cache variable). all three
// produce visually-identical output (see renderer.h "VISUAL CONTRACT").
// WHY KEEP D3D9 AROUND?
// it's the lowest-common-denominator api on Windows. works on:
// - Windows XP (last mainstream OS without D3D10+)
// - integrated graphics from before ~2008
// - virtual machines / remote desktop scenarios where modern api
// support is patchy
// - users who specifically install older drivers for stability
// if the toolkit ever needs to ship to a wider audience than "modern
// gaming GPUs", the D3D9 path keeps it accessible.
// D3D9 IN PLAIN C (not C++):
// like d3d11, d3d9 is a COM api with C++-friendly headers. unlike d3d11,
// d3d9 doesn't have a COBJMACROS option -- we have to write
// `device->lpVtbl->Method(device, args...)` everywhere. it's noisy but
// mechanical; the alternative (#define a per-method macro) would just be
// reinventing what d3d11 ships in its header.
// COM Release()/AddRef() rules apply: every pointer we get back must be
// Release()'d when we're done. shutdown() walks every pointer in the
// state struct and releases what's non-NULL.
// THE PIPELINE (very similar to d3d11's, with the differences below):
// 1. INPUT-ASSEMBLY: SetVertexDeclaration + SetStreamSource. d3d9
// pre-dates d3d10's "input layout" concept, so we describe the
// vertex format with a vertex declaration object instead.
// 2. VERTEX SHADER: shader model 3 (vs_3_0). compiled at runtime via
// D3DCompile (same compiler as d3d11). constants are set via
// SetVertexShaderConstantF (one float4 per call -- no constant
// buffer object).
// 3. RASTERIZER: state set via SetRenderState calls (no rasterizer
// state object). scissor enable lives here.
// 4. PIXEL SHADER: shader model 3 (ps_3_0). same SDF math as the other
// backends. samplers via SetSamplerState (per-stage).
// 5. OUTPUT-MERGER: alpha blending via D3DRS_SRCBLEND / DESTBLEND
// render state. no separate "blend state object" -- it's all a
// bag of flags.
// 6. PRESENT: IDirect3DDevice9::Present, but the frame must be
// bracketed by BeginScene / EndScene (a d3d9-ism that d3d10+
// removed).
// D3D9 VS D3D11 / OPENGL -- DIFFERENCES THAT MATTER FOR THIS CODE:
// 1. NO "CONTEXT" OBJECT. IDirect3DDevice9 does both what d3d11's
// Device and DeviceContext do.
// 2. VERTEX DECLARATION replaces d3d11's input layout / opengl's VAO.
// 3. CONSTANTS via SetVertexShaderConstantF (one float4 per slot),
// not via cbuffers.
// 4. RENDER STATE is a bag of flags set with SetRenderState; no
// "blend state object" / "rasterizer state object" the way d3d11
// has. To swap blend modes or scissor enable, you set individual
// flags.
// 5. PRESENT is IDirect3DDevice9::Present, and frames are bracketed
// by BeginScene / EndScene -- forgetting either silently drops
// every draw call. this caught us once.
// 6. PIXEL CENTER OFFSET. d3d9's rasterizer puts pixel centers at
// INTEGER coordinates, unlike d3d10+/gl which put them at
// HALF-INTEGER (n.5). to land on the same pixels, we subtract
// 0.5 from the vertex position in the vertex shader BEFORE the
// NDC conversion. visible in the VS source string below as
// `adj = i.pos - float2(0.5, 0.5)`. without this every rect
// drawn at integer coordinates would be off by half a pixel
// compared to the d3d11/opengl renderings.
// ref: docs.microsoft.com -- "directly mapping texels to pixels"
// 7. LOST DEVICE. on certain events (user alt-tab in fullscreen,
// some driver updates, display mode changes) the device becomes
// "lost" and all POOL_DEFAULT resources must be released, then
// Reset() called on the device, then resources recreated. Standalone
// frames poll TestCooperativeLevel and recover; adopting hosts call
// the device-lost/reset hooks around their own Reset.
// VISUAL CONTRACT:
// this backend must produce the same pixels as every other backend
// given the same input -- see renderer.h's "VISUAL CONTRACT" section.
// the SDF function, blend mode (note: SEPARATE alpha blend), smoothstep
// range, scissor semantics, vertex ordering, and the half-pixel offset
// are all specified; don't diverge from them.

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#define MEEK_RENDERER_BACKEND
#include "renderer.h"
#include "renderer_backend.h"
#include "gradient.h"
#include "d3d9_renderer.h"
#include "_visual_contract.h"
#include "_blur_cpu.h"
#include "_mip_cpu.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

#define _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(p)      \
    do                                                \
    {                                                 \
        if ((p) != NULL_POINTER)                              \
        {                                             \
            (p)->lpVtbl->Release(p);                  \
            (p) = NULL_POINTER;                               \
        }                                             \
    } while (0)

// ============================================================================
// VERTEX FORMAT
// ============================================================================
// identical to the other backends: 11 floats (pos, color, local, rect
// size, radius). in d3d9 the vertex declaration binds the offsets to
// HLSL semantics.

typedef struct _d3d9_renderer_internal__vertex_t
{
    float x;        // 0x0
    float y;        // 0x4
    float r;        // 0x8
    float g;        // 0xC
    float b;        // 0x10
    float a;        // 0x14
    float lx;       // 0x18
    float ly;       // 0x1C
    float rect_w;   // 0x20
    float rect_h;   // 0x24
    float radius;   // 0x28
} _d3d9_renderer_internal__vertex_t;

// text vertex: position + color + atlas UV. 6 per glyph quad.
typedef struct _d3d9_renderer_internal__text_vertex_t
{
    float x;   // 0x0
    float y;   // 0x4
    float r;   // 0x8
    float g;   // 0xC
    float b;   // 0x10
    float a;   // 0x14
    float u;   // 0x18
    float v;   // 0x1C
} _d3d9_renderer_internal__text_vertex_t;

#define _D3D9_RENDERER_INTERNAL__MAX_QUADS       16384
#define _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD  6

// text batch cap. matches the other backends (4096 glyphs) — sized for
// ARM/mobile-conscious working-set limits in spirit, even though this
// backend itself only runs on windows.
#define _D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096

// per-atlas runs. same pattern as the other backends: each run in the
// text VBO binds a different IDirect3DTexture9 before DrawPrimitive, so
// a frame can mix multiple fonts / sizes.
#define _D3D9_RENDERER_INTERNAL__MAX_TEXT_RUNS 64

typedef struct _d3d9_renderer_internal__text_run_t
{
    IDirect3DTexture9* atlas;        // 0x0
    int64              vert_start;   // 0x8
    int64              vert_count;   // 0x10
} _d3d9_renderer_internal__text_run_t;

// ============================================================================
// HLSL SHADERS (shader model 3)
// ============================================================================
// direct translation of the GLSL, with three d3d9-specific adjustments:
// - VS output position semantic is "POSITION" (SM3) instead of the
// d3d10+ "SV_POSITION".
// - PS output semantic is "COLOR" instead of "SV_TARGET".
// - vertex shader subtracts (0.5, 0.5) from input pixel coord before
// the NDC conversion, to account for d3d9's integer-pixel-center
// rasterization rule. WITHOUT this, every rect renders half a pixel
// off and looks blurry.
// - uniforms come from SetVertexShaderConstantF / SetPixelShaderConstantF,
// not cbuffers. vs constant c0 holds the viewport (float2 + 2 unused).

#include "shaders/generated/rect.vert.h"

#include "shaders/generated/rect.frag.h"

// TEXT SHADERS (HLSL SM3).
// - vertex shader does the same -0.5 pixel-center adjustment as the
// solid rect VS so glyph quads land on the same integer pixels as
// the rects they're drawn on top of.
// - pixel shader samples sampler s0. with D3DFMT_L8 the texel's .r is
// the luminance (our atlas coverage byte); .rrrr is the same value
// in all channels, so reading .r is correct.
#include "shaders/generated/text.vert.h"

// See opengl3_renderer.c text_fs_src for the full rationale on the
// pow(a, 1/2.2) step. Short version: we blend in sRGB space (the
// backbuffer is D3DFMT_A8R8G8B8, not gamma-corrected), which
// darkens partial-coverage edges. Reshaping the atlas coverage
// through the gamma curve produces edges that land at the
// perceptual midpoint instead of the sRGB-linear midpoint, so
// text stops looking washed-out on dark backgrounds.
#include "shaders/generated/text.frag.h"

// ============================================================================
// STATE
// ============================================================================

typedef struct _d3d9_renderer_internal__state_t
{
    HWND hwnd;                                                                             // 0x0

    IDirect3D9*               d3d9;                                                        // 0x8
    IDirect3DDevice9*         device;                                                      // 0x10

    // solid-rect pipeline.
    IDirect3DVertexShader9*      vs;                                                       // 0x18
    IDirect3DPixelShader9*       ps;                                                       // 0x20
    IDirect3DVertexDeclaration9* decl;                                                     // 0x28
    IDirect3DVertexBuffer9*      vbo;                                                      // 0x30

    _d3d9_renderer_internal__vertex_t* cpu_verts;                                            // 0x38
    int64                            vert_count;                                           // 0x40
    int64                            vert_cap;                                             // 0x48

    int64 viewport_w;                                                                      // 0x50
    int64 viewport_h;                                                                      // 0x58

    // last viewport dimensions we actually applied to the swap chain
    // via Device::Reset. begin_frame compares the incoming
    // viewport_w / viewport_h against these; if they differ, the
    // backbuffer is resized so content doesn't stretch. initialized
    // to 0/0 in renderer__init so the first frame's Reset kicks in
    // via the usual mismatch path.
    int64 last_applied_w;                                                                  // 0x60
    int64 last_applied_h;                                                                  // 0x68

    // stuck-lost flag: stops draws and Present attempts.
    boole device_lost;                                                                     // 0x70

    // text pipeline. atlas textures are D3DFMT_L8 in D3DPOOL_MANAGED
    // (survives device lost without explicit recovery).
    // per-atlas run batching: the frame's text verts are partitioned
    // into runs, each tagged with its atlas texture; end_frame calls
    // SetTexture + DrawPrimitive once per run.
    IDirect3DVertexShader9*      text_vs;                                                  // 0x78
    IDirect3DPixelShader9*       text_ps;                                                  // 0x80
    IDirect3DVertexDeclaration9* text_decl;                                                // 0x88
    IDirect3DVertexBuffer9*      text_vbo;                                                 // 0x90
    // borrowed; set by set_text_atlas.
    IDirect3DTexture9*           current_text_atlas;                                       // 0x98

    _d3d9_renderer_internal__text_vertex_t* text_cpu_verts;                                  // 0xA0
    int64                                 text_vert_count;                                 // 0xA8
    int64                                 text_vert_cap;                                   // 0xB0

    _d3d9_renderer_internal__text_run_t text_runs[_D3D9_RENDERER_INTERNAL__MAX_TEXT_RUNS];   // 0xB8
    int64                             text_run_count;                                      // 0x6B8

    // Image pipeline. Lazy-built on first renderer__submit_image.
    // Vertex format (pos + color + uv = 32 bytes) matches the text
    // pipeline -- same VS fits both. A separate vertex declaration
    // is needed because the image VS only declares 3 attributes,
    // not the 5 the text VS declares (no lx/ly/rect_w/rect_h/radius).
    IDirect3DVertexShader9*      image_vs;                                                 // 0x6C0
    IDirect3DPixelShader9*       image_ps;                                                 // 0x6C8
    IDirect3DVertexDeclaration9* image_decl;                                               // 0x6D0
    // 6-vert scratch.
    IDirect3DVertexBuffer9*      image_vbo;                                                // 0x6D8

    // Premultiplied layer pipeline and a small pool of render targets. CPU
    // layer uploads are managed textures; only the targets need reset hooks.
    IDirect3DVertexShader9* layer_vs;
    IDirect3DPixelShader9* layer_ps;
    IDirect3DVertexDeclaration9* layer_decl;
    struct { IDirect3DTexture9* texture; IDirect3DSurface9* surface;
             int w, h; uint64 used; } layer_targets[4];
    uint64 layer_counter;
    int layer_active;
    IDirect3DSurface9* layer_saved_targets[4];
    IDirect3DSurface9* layer_saved_depth;
    DWORD layer_target_count;
    D3DVIEWPORT9 layer_saved_viewport;
    int64 layer_saved_w, layer_saved_h, layer_saved_scissor_depth;
    gui_rect_t layer_saved_scissors[32];
    IDirect3DSurface9* layer_readback;
    D3DFORMAT layer_readback_format;
    UINT layer_readback_w, layer_readback_h;

    // Scissor stack. D3D9 toggles scissor via D3DRS_SCISSORTESTENABLE
    // and SetScissorRect. We keep the render state TRUE permanently
    // (set in init) so we don't have to flip it on every push/pop;
    // the rect itself is what changes. When the stack is empty we
    // set the rect to the full viewport (no-op clip).
    gui_rect_t scissor_stack[32];                                                            // 0x6E0
    int64    scissor_depth;                                                                // 0x8E0
    // adopted: the device belongs to the host (embedded / overlay use). No
    // Clear, BeginScene, EndScene, Present or Reset; textures go to the
    // managed pool so the host's own device reset cannot lose them.
    boole    adopted;                                                                      // 0x8E8
    // ex: the adopted device is a D3D9Ex one (Direct3DCreate9Ex /
    // CreateDeviceEx). It refuses D3DPOOL_MANAGED, so textures go to the
    // default pool with D3DUSAGE_DYNAMIC instead; on such a device they
    // survive the host's Reset, so nothing else changes.
    boole    ex;                                                                           // 0x8E9
} _d3d9_renderer_internal__state_t;

static _d3d9_renderer_internal__state_t _d3d9_renderer_internal__g;

// ============================================================================
// FORWARD DECLS
// ============================================================================

static boole _d3d9_renderer_internal__create_device(HWND hwnd);
static boole _d3d9_renderer_internal__build_pipeline(void);
static boole _d3d9_renderer_internal__build_text_pipeline(void);
static boole _d3d9_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob);
static void  _d3d9_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c);
static void  _d3d9_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color_t c);
static void  _d3d9_renderer_internal__flush_batches(void);
static void  _d3d9_renderer_internal__capture_if_requested(void);
static void _d3d9_renderer_internal__release_blur(void);
static void  _d3d9_renderer_internal__apply_scissor_top(void);
static void  _d3d9_renderer_internal__apply_render_state(void);
static boole _d3d9_renderer_internal__reset_swapchain(int64 w, int64 h);
static void _d3d9_renderer_internal__release_reset_resources(void);
static boole _d3d9_renderer_internal__restore_reset_resources(void);
static void _d3d9_renderer_internal__release_layers(void);
static D3DPOOL _d3d9_renderer_internal__texture_pool(DWORD* usage);

// IID_IDirect3DDevice9Ex, spelled out so that no dxguid library is needed
// to ask an adopted device whether it is a D3D9Ex one.
static const GUID _d3d9_renderer_internal__iid_device9ex = { 0xb18b10ce, 0x2649, 0x405a, { 0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a } };

// ============================================================================
// PUBLIC: renderer__init
// ============================================================================

/**
 * @brief bring up the graphics context and initialize the per-frame pipeline. Called once at startup AFTER the platform layer has created its window. The renderer owns whatever GPU objects it needs (GL context, D3D device+swapchain, shaders, vertex buffers) and stores them in its own file-static state.
 *
 * @param void* native_window -> Platform window handle. On Windows this is HWND (`struct HWND__*`). Backends cast as needed.
 *
 * @return boole -> TRUE on success; FALSE if context creation, shader compilation, or resource allocation failed.
 */
boole renderer__init(void* native_window)
{
    stdlib__memset(&_d3d9_renderer_internal__g, 0, sizeof(_d3d9_renderer_internal__g));

    _d3d9_renderer_internal__g.hwnd = (HWND)native_window;
    if (_d3d9_renderer_internal__g.hwnd == NULL_POINTER)
    {
        log_error("%s", "renderer__init got a NULL window handle");
        return FALSE;
    }

    RECT rc;
    GetClientRect(_d3d9_renderer_internal__g.hwnd, &rc);
    _d3d9_renderer_internal__g.viewport_w = rc.right - rc.left;
    _d3d9_renderer_internal__g.viewport_h = rc.bottom - rc.top;
    if (_d3d9_renderer_internal__g.viewport_w <= 0) _d3d9_renderer_internal__g.viewport_w = 1;
    if (_d3d9_renderer_internal__g.viewport_h <= 0) _d3d9_renderer_internal__g.viewport_h = 1;

    if (_d3d9_renderer_internal__create_device(_d3d9_renderer_internal__g.hwnd) == FALSE)
    {
        return FALSE;
    }
    if (_d3d9_renderer_internal__build_pipeline() == FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// PUBLIC: renderer__init_adopt_d3d9
// ============================================================================

/**
 * @brief initializes the renderer against a Direct3D 9 device the host owns; the renderer draws inside the host's own scene and never clears, presents or resets
 *
 * @param IDirect3DDevice9* device -> host-owned device; must outlive the renderer, no reference is taken
 *
 * @attention the viewport is read from the device at attach time and follows the size passed to renderer__begin_frame afterwards; the host calls renderer__d3d9_on_device_lost / _on_device_reset around its own Reset
 *
 * @return boole -> TRUE on success
 */
boole renderer__init_adopt_d3d9(IDirect3DDevice9* device)
{
    D3DVIEWPORT9 vp;
    IDirect3DDevice9Ex* ex = NULL_POINTER;

    stdlib__memset(&_d3d9_renderer_internal__g, 0, sizeof(_d3d9_renderer_internal__g));

    if (device == NULL_POINTER)
    {
        log_error("%s", "renderer__init_adopt_d3d9: null device");
        return FALSE;
    }

    _d3d9_renderer_internal__g.device  = device;
    _d3d9_renderer_internal__g.adopted = TRUE;
    _d3d9_renderer_internal__g.hwnd    = NULL_POINTER;
    _d3d9_renderer_internal__g.d3d9    = NULL_POINTER;

    // a D3D9Ex device answers the query; its textures cannot be managed
    if (SUCCEEDED(device->lpVtbl->QueryInterface(device, &_d3d9_renderer_internal__iid_device9ex, (void**)&ex)) && ex != NULL_POINTER)
    {
        ex->lpVtbl->Release(ex);
        _d3d9_renderer_internal__g.ex = TRUE;
        log_info("%s", "renderer__init_adopt_d3d9: D3D9Ex device, textures in the default pool");
    }

    stdlib__memset(&vp, 0, sizeof(vp));

    if (SUCCEEDED(device->lpVtbl->GetViewport(device, &vp)) && vp.Width > 0 && vp.Height > 0)
    {
        _d3d9_renderer_internal__g.viewport_w = (int64)vp.Width;
        _d3d9_renderer_internal__g.viewport_h = (int64)vp.Height;
    }
    else
    {
        _d3d9_renderer_internal__g.viewport_w = 1;
        _d3d9_renderer_internal__g.viewport_h = 1;
    }

    _d3d9_renderer_internal__g.last_applied_w = _d3d9_renderer_internal__g.viewport_w;
    _d3d9_renderer_internal__g.last_applied_h = _d3d9_renderer_internal__g.viewport_h;

    if (_d3d9_renderer_internal__build_pipeline() == FALSE)
    {
        _d3d9_renderer_internal__g.device = NULL_POINTER;
        renderer__shutdown();
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief the pool and usage every texture is created with
 *
 * @param DWORD* usage -> receives the usage flags: D3DUSAGE_DYNAMIC in the default pool, so the texture stays lockable for the upload
 *
 * @attention a plain D3D9 device gets D3DPOOL_MANAGED, which survives Reset and needs no lock flags; a D3D9Ex device refuses that pool and gets D3DPOOL_DEFAULT, where its textures survive Reset as well
 *
 * @return D3DPOOL -> the pool
 */
static D3DPOOL _d3d9_renderer_internal__texture_pool(DWORD* usage)
{
    if (_d3d9_renderer_internal__g.ex == TRUE)
    {
        *usage = D3DUSAGE_DYNAMIC;
        return D3DPOOL_DEFAULT;
    }
    *usage = 0;
    return D3DPOOL_MANAGED;
}

/**
 * @brief releases the default-pool objects before the host resets its device; adopted mode only
 *
 * @attention the image pipeline is dropped whole and rebuilt lazily on the next image draw; drawing stays off until renderer__d3d9_on_device_reset
 *
 * @return void
 */
void renderer__d3d9_on_device_lost(void)
{
    if (_d3d9_renderer_internal__g.adopted == FALSE)
    {
        return;
    }

    _d3d9_renderer_internal__release_reset_resources();
}

/**
 * @brief release every default-pool buffer before a standalone or host device reset
 * @return void
 */
static void _d3d9_renderer_internal__release_reset_resources(void)
{
    _d3d9_renderer_internal__release_layers();

    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_vs);
    _d3d9_renderer_internal__g.device_lost = TRUE;
}

/**
 * @brief recreates the default-pool vertex buffers after the host reset its device; adopted mode only
 *
 * @return boole -> TRUE when the buffers exist again and drawing may resume
 */
boole renderer__d3d9_on_device_reset(void)
{
    if (_d3d9_renderer_internal__g.adopted == FALSE)
    {
        return FALSE;
    }

    return _d3d9_renderer_internal__restore_reset_resources();
}

/**
 * @brief restore solid and text buffers after reset; rebuild images lazily on their next draw
 * @return boole -> TRUE when rendering can resume
 */
static boole _d3d9_renderer_internal__restore_reset_resources(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    UINT vbo_bytes = 0;
    UINT text_vbo_bytes = 0;
    HRESULT hr = 0;

    if (dev == NULL_POINTER)
    {
        return FALSE;
    }

    vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_QUADS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__vertex_t));
    hr = dev->lpVtbl->CreateVertexBuffer(dev, vbo_bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &_d3d9_renderer_internal__g.vbo, NULL_POINTER);

    if (FAILED(hr))
    {
        log_error("%s %08lX", "renderer__d3d9_on_device_reset: CreateVertexBuffer (solid) failed, hresult", (unsigned long)hr);
        return FALSE;
    }

    text_vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__text_vertex_t));
    hr = dev->lpVtbl->CreateVertexBuffer(dev, text_vbo_bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &_d3d9_renderer_internal__g.text_vbo, NULL_POINTER);

    if (FAILED(hr))
    {
        log_error("%s %08lX", "renderer__d3d9_on_device_reset: CreateVertexBuffer (text) failed, hresult", (unsigned long)hr);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.vbo);
        return FALSE;
    }

    _d3d9_renderer_internal__g.device_lost = FALSE;
    return TRUE;
}

// ============================================================================
// PUBLIC: renderer__shutdown
// ============================================================================

/**
 * @brief tear down the graphics context and free every GPU/CPU resource owned by the renderer. Symmetric with renderer__init; safe to call even if init failed partway.
 *
 * @return void
 */
void renderer__shutdown(void)
{
    _d3d9_renderer_internal__release_layers();
    // text pipeline first (atlas textures are owned by font.c; it
    // releases them via renderer__destroy_atlas before we get here).
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.text_vs);

    // Image pipeline. Textures from renderer__create_texture_rgba
    // are owned by callers (released via renderer__destroy_texture).
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.image_vs);
    // current_text_atlas is a borrowed pointer -- not ours to release.
    _d3d9_renderer_internal__g.current_text_atlas = NULL_POINTER;

    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.vbo);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.decl);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.vs);
    // an adopted device belongs to the host; forget it instead of releasing it
    if (_d3d9_renderer_internal__g.adopted == TRUE)
    {
        _d3d9_renderer_internal__g.device = NULL_POINTER;
    }
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.device);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(_d3d9_renderer_internal__g.d3d9);

    if (_d3d9_renderer_internal__g.cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_d3d9_renderer_internal__g.cpu_verts);
    }
    if (_d3d9_renderer_internal__g.text_cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_d3d9_renderer_internal__g.text_cpu_verts);
    }
    stdlib__memset(&_d3d9_renderer_internal__g, 0, sizeof(_d3d9_renderer_internal__g));
}

// ============================================================================
// PUBLIC: renderer__begin_frame
// ============================================================================

/**
 * @brief start a frame. Set the viewport, clear the back buffer to `clear`, reset the per-frame vertex batch.
 *
 * @param int64 viewport_w -> Viewport width in pixels.
 * @param int64 viewport_h -> Viewport height in pixels.
 * @param gui_color_t clear -> Back-buffer clear color.
 *
 * @return void
 */
void renderer__begin_frame(int64 viewport_w, int64 viewport_h, gui_color_t clear)
{
    HRESULT cooperative = D3D_OK;

    if (_d3d9_renderer_internal__g.device == NULL_POINTER)
    {
        return;
    }

    if (_d3d9_renderer_internal__g.device_lost == TRUE)
    {
        if (_d3d9_renderer_internal__g.adopted == TRUE)
        {
            return;
        }

        cooperative = _d3d9_renderer_internal__g.device->lpVtbl->TestCooperativeLevel(_d3d9_renderer_internal__g.device);
        if (cooperative != D3D_OK && cooperative != D3DERR_DEVICENOTRESET)
        {
            return;
        }
        if (_d3d9_renderer_internal__reset_swapchain(viewport_w, viewport_h) == FALSE)
        {
            return;
        }
    }

    // resize the swap chain's backbuffer if the host window changed
    // size since the last frame. without this, the backbuffer stays
    // at its original dimensions and d3d9's Present stretches /
    // letterboxes it onto the new client area -- visibly bad on a
    // window drag that grows or shrinks the client rect. full Reset
    // flow lives in _reset_swapchain below (releases POOL_DEFAULT
    // VBs, calls Device::Reset with new pp, recreates VBs, reapplies
    // render state). mismatch against last_applied_* triggers it.
    _d3d9_renderer_internal__g.viewport_w = viewport_w;
    _d3d9_renderer_internal__g.viewport_h = viewport_h;
    if (_d3d9_renderer_internal__g.adopted == TRUE)
    {
        // the host owns the swap chain; only the viewport follows the size
        _d3d9_renderer_internal__g.last_applied_w = viewport_w;
        _d3d9_renderer_internal__g.last_applied_h = viewport_h;
    }
    else if (viewport_w != _d3d9_renderer_internal__g.last_applied_w || viewport_h != _d3d9_renderer_internal__g.last_applied_h)
    {
        if (_d3d9_renderer_internal__reset_swapchain(viewport_w, viewport_h) == FALSE)
        {
            // reset_swapchain already logged + flagged device_lost;
            // nothing useful we can do here beyond bail.
            return;
        }
    }

    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    // d3d9's Clear takes an ARGB packed uint, not a float array.
    // convert the gui_color_t to 0xAARRGGBB.
    DWORD cr = (DWORD)(clear.r * 255.0f + 0.5f) & 0xFF;
    DWORD cg = (DWORD)(clear.g * 255.0f + 0.5f) & 0xFF;
    DWORD cb = (DWORD)(clear.b * 255.0f + 0.5f) & 0xFF;
    DWORD ca = (DWORD)(clear.a * 255.0f + 0.5f) & 0xFF;
    D3DCOLOR packed = (ca << 24) | (cr << 16) | (cg << 8) | cb;

    if (_d3d9_renderer_internal__g.adopted == FALSE)
    {
        dev->lpVtbl->Clear(dev, 0, NULL_POINTER, D3DCLEAR_TARGET, packed, 1.0f, 0);
    }

    // viewport. d3d9 viewports use top-left origin (y-down).
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width  = (DWORD)viewport_w;
    vp.Height = (DWORD)viewport_h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->lpVtbl->SetViewport(dev, &vp);
    // Scissor changes can submit the first batch before end_frame.
    float viewport_constants[4] = {(float)viewport_w, (float)viewport_h, 0, 0};
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, viewport_constants, 1);

    // d3d9 requires every frame's drawing to be bracketed by
    // BeginScene / EndScene. failing to do so silently drops draws.
    if (_d3d9_renderer_internal__g.adopted == FALSE)
    {
        dev->lpVtbl->BeginScene(dev);
    }
    else
    {
        // inside the host's scene: our render state is re-applied every frame, the host's state block restore undoes it afterwards
        _d3d9_renderer_internal__apply_render_state();
    }

    _d3d9_renderer_internal__g.vert_count      = 0;
    _d3d9_renderer_internal__g.text_vert_count = 0;
    _d3d9_renderer_internal__g.text_run_count  = 0;
    // current_text_atlas is intentionally preserved across frames; the
    // first font__draw call each frame rebinds it anyway. text_runs is
    // cleared per-frame (each run belongs to the frame that produced it).

    // Initial scissor: full viewport (no-op clip). Required since
    // D3DRS_SCISSORTESTENABLE is permanently TRUE -- without a sane
    // rect, the GPU clips to whatever leftover scissor was last set.
    _d3d9_renderer_internal__apply_scissor_top();
}

// ============================================================================
// PUBLIC: renderer__submit_rect
// ============================================================================

/**
 * @brief queue one rectangle for the current frame. Radius == 0 produces square corners; positive values are clamped to half the smaller side.
 *
 * @param gui_rect_t r -> Rectangle in top-left-origin pixel coords.
 * @param gui_color_t c -> Fill color (linear RGBA, 0..1).
 * @param float radius -> Corner radius in pixels.
 *
 * @return void
 */
void renderer__submit_rect(gui_rect_t r, gui_color_t c, float radius)
{
    if (_d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    if (_d3d9_renderer_internal__g.vert_count + _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD > _d3d9_renderer_internal__g.vert_cap)
    {
        return;
    }

    float max_r = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius < 0.0f) radius = 0.0f;
    if (radius > max_r) radius = max_r;

    float x0 = r.x;
    float y0 = r.y;
    float x1 = r.x + r.w;
    float y1 = r.y + r.h;

    _d3d9_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c);
    _d3d9_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
}

// ============================================================================
// PUBLIC: renderer__end_frame
// ============================================================================

// ============================================================================
// INTERNAL: flush_batches
// ============================================================================
// Draw whatever is currently buffered (solid rects + text glyphs), then
// reset the buffer counts so subsequent submissions start a fresh batch.
// Called from end_frame and from push/pop_scissor (any state change that
// can't be merged into a single DrawPrimitive forces a flush).

static void _d3d9_renderer_internal__flush_batches(void)
{
    if (_d3d9_renderer_internal__g.device == NULL_POINTER || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    // PASS 1: solid rects.
    if (_d3d9_renderer_internal__g.vert_count > 0)
    {
        void*  mapped = NULL_POINTER;
        UINT   bytes  = (UINT)(_d3d9_renderer_internal__g.vert_count * sizeof(_d3d9_renderer_internal__vertex_t));
        HRESULT hr = _d3d9_renderer_internal__g.vbo->lpVtbl->Lock(_d3d9_renderer_internal__g.vbo, 0, bytes, &mapped, D3DLOCK_DISCARD);
        if (SUCCEEDED(hr) && mapped != NULL_POINTER)
        {
            stdlib__memcpy(mapped, _d3d9_renderer_internal__g.cpu_verts, bytes);
            _d3d9_renderer_internal__g.vbo->lpVtbl->Unlock(_d3d9_renderer_internal__g.vbo);
        }

        dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.decl);
        dev->lpVtbl->SetStreamSource(dev, 0, _d3d9_renderer_internal__g.vbo, 0, (UINT)sizeof(_d3d9_renderer_internal__vertex_t));
        dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.vs);
        dev->lpVtbl->SetPixelShader(dev, _d3d9_renderer_internal__g.ps);

        UINT primitive_count = (UINT)stdlib__div64(_d3d9_renderer_internal__g.vert_count, 3);
        dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, primitive_count);

        _d3d9_renderer_internal__g.vert_count = 0;
    }

    // PASS 2: text. one DrawPrimitive per atlas-run (multi-font support).
    if (_d3d9_renderer_internal__g.text_vert_count > 0 && _d3d9_renderer_internal__g.text_run_count > 0)
    {
        void*  tmapped = NULL_POINTER;
        UINT   tbytes  = (UINT)(_d3d9_renderer_internal__g.text_vert_count * sizeof(_d3d9_renderer_internal__text_vertex_t));
        HRESULT thr = _d3d9_renderer_internal__g.text_vbo->lpVtbl->Lock(_d3d9_renderer_internal__g.text_vbo, 0, tbytes, &tmapped, D3DLOCK_DISCARD);
        if (SUCCEEDED(thr) && tmapped != NULL_POINTER)
        {
            stdlib__memcpy(tmapped, _d3d9_renderer_internal__g.text_cpu_verts, tbytes);
            _d3d9_renderer_internal__g.text_vbo->lpVtbl->Unlock(_d3d9_renderer_internal__g.text_vbo);
        }

        dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.text_decl);
        dev->lpVtbl->SetStreamSource(dev, 0, _d3d9_renderer_internal__g.text_vbo, 0, (UINT)sizeof(_d3d9_renderer_internal__text_vertex_t));
        dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.text_vs);
        dev->lpVtbl->SetPixelShader(dev, _d3d9_renderer_internal__g.text_ps);

        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
        dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

        for (int64 i = 0; i < _d3d9_renderer_internal__g.text_run_count; i++)
        {
            _d3d9_renderer_internal__text_run_t* run = &_d3d9_renderer_internal__g.text_runs[i];
            if (run->vert_count <= 0 || run->atlas == NULL_POINTER)
            {
                continue;
            }
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture9*)run->atlas);
            UINT prim_count = (UINT)stdlib__div64(run->vert_count, 3);
            dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, (UINT)run->vert_start, prim_count);
        }

        dev->lpVtbl->SetTexture(dev, 0, NULL_POINTER);

        _d3d9_renderer_internal__g.text_vert_count    = 0;
        _d3d9_renderer_internal__g.text_run_count     = 0;
        _d3d9_renderer_internal__g.current_text_atlas = NULL_POINTER;
    }
}

// ============================================================================
// PUBLIC: renderer__push_scissor / renderer__pop_scissor
// ============================================================================
// D3D9 toggles scissor via D3DRS_SCISSORTESTENABLE + SetScissorRect.
// We leave the render state TRUE permanently (set in init) so we don't
// have to flip it on every push/pop; the rect itself is what changes.
// When the stack becomes empty we set the rect to the full viewport
// (no-op clip).

static gui_rect_t _d3d9_renderer_internal__intersect(gui_rect_t a, gui_rect_t b)
{
    float x0  = (a.x > b.x) ? a.x : b.x;
    float y0  = (a.y > b.y) ? a.y : b.y;
    float x1a = a.x + a.w, x1b = b.x + b.w;
    float y1a = a.y + a.h, y1b = b.y + b.h;
    float x1 = (x1a < x1b) ? x1a : x1b;
    float y1 = (y1a < y1b) ? y1a : y1b;
    gui_rect_t r;
    r.x = x0;
    r.y = y0;
    r.w = (x1 > x0) ? (x1 - x0) : 0.0f;
    r.h = (y1 > y0) ? (y1 - y0) : 0.0f;
    return r;
}

static void _d3d9_renderer_internal__apply_scissor_top(void)
{
    if (_d3d9_renderer_internal__g.device == NULL_POINTER || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    RECT sr;
    if (_d3d9_renderer_internal__g.scissor_depth <= 0)
    {
        sr.left   = 0;
        sr.top    = 0;
        sr.right  = (LONG)_d3d9_renderer_internal__g.viewport_w;
        sr.bottom = (LONG)_d3d9_renderer_internal__g.viewport_h;
    }
    else
    {
        gui_rect_t r = _d3d9_renderer_internal__g.scissor_stack[0];
        for (int64 i = 1; i < _d3d9_renderer_internal__g.scissor_depth; i++)
        {
            r = _d3d9_renderer_internal__intersect(r, _d3d9_renderer_internal__g.scissor_stack[i]);
        }
        sr.left   = (LONG)r.x;
        sr.top    = (LONG)r.y;
        sr.right  = (LONG)(r.x + r.w);
        sr.bottom = (LONG)(r.y + r.h);
        if (sr.right  < sr.left)
        {
            sr.right  = sr.left;
        }
        if (sr.bottom < sr.top)
        {
            sr.bottom = sr.top;
        }
    }
    dev->lpVtbl->SetScissorRect(dev, &sr);
}

/**
 * @brief push a clip rect onto the scissor stack. Subsequent submissions are clipped to the intersection of every rect currently on the stack.
 *
 * @param gui_rect_t rect -> Clip region in pixel coords (top-left origin).
 *
 * @return void
 */
void renderer__push_scissor(gui_rect_t rect)
{
    _d3d9_renderer_internal__flush_batches();
    if (_d3d9_renderer_internal__g.scissor_depth >= (int64)(sizeof(_d3d9_renderer_internal__g.scissor_stack) / sizeof(_d3d9_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("%s", "renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    _d3d9_renderer_internal__g.scissor_stack[_d3d9_renderer_internal__g.scissor_depth++] = rect;
    _d3d9_renderer_internal__apply_scissor_top();
}

/**
 * @brief pop the most recent scissor rect off the stack. If the stack is now empty, scissor testing is disabled entirely (everything draws).
 *
 * @return void
 */
void renderer__pop_scissor(void)
{
    _d3d9_renderer_internal__flush_batches();
    if (_d3d9_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("%s", "renderer__pop_scissor: stack already empty");
        return;
    }
    _d3d9_renderer_internal__g.scissor_depth--;
    _d3d9_renderer_internal__apply_scissor_top();
}

/**
 * @brief capture the framebuffer region under `rect`, apply a separable Gaussian blur with standard deviation `sigma_px`, and paint the blurred result back over `rect`. Subsequent submissions draw on top of the blurred backdrop, as the caller intends (e.g. text rendered legibly above a frosted panel background).
 *
 * @param gui_rect_t rect -> Region to blur, in pixel coords.
 * @param float sigma_px -> Gaussian standard deviation in pixels. Values under 1.0 produce negligible blur; 4-16 is the "frosted glass" range.
 *
 * @return void
 */
void renderer__blur_region(gui_rect_t rect, float sigma_px)
{
    if (sigma_px <= 0.0f)
    {
        return;
    }
    if (rect.w <= 0.0f || rect.h <= 0.0f)
    {
        return;
    }

    // Drain pending batches so the backbuffer reflects everything
    // submitted before this blur call.
    _d3d9_renderer_internal__flush_batches();

    int vw = (int)_d3d9_renderer_internal__g.viewport_w;
    int vh = (int)_d3d9_renderer_internal__g.viewport_h;

    int x = (int)rect.x;
    int y = (int)rect.y;
    int w = (int)rect.w;
    int h = (int)rect.h;
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > vw)
    {
        w = vw - x;
    }
    if (y + h > vh)
    {
        h = vh - y;
    }
    if (w <= 0 || h <= 0)
    {
        return;
    }

    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    // D3D9 readback: GetRenderTarget gets the current RT (the
    // backbuffer surface). CreateOffscreenPlainSurface on
    // D3DPOOL_SYSTEMMEM is a CPU-mappable surface in the same
    // format. GetRenderTargetData copies between them. Then Lock
    // the system surface to read.
    IDirect3DSurface9* rt = NULL_POINTER;
    HRESULT hr = IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    if (FAILED(hr) || rt == NULL_POINTER)
    {
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    D3DSURFACE_DESC rtd;
    if (FAILED(IDirect3DSurface9_GetDesc(rt, &rtd)))
    {
        IDirect3DSurface9_Release(rt);
        return;
    }

    IDirect3DSurface9* sysmem = NULL_POINTER;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface( dev, rtd.Width, rtd.Height, rtd.Format, D3DPOOL_SYSTEMMEM, &sysmem, NULL_POINTER);
    if (FAILED(hr) || sysmem == NULL_POINTER)
    {
        IDirect3DSurface9_Release(rt);
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sysmem);
    IDirect3DSurface9_Release(rt);
    if (FAILED(hr))
    {
        IDirect3DSurface9_Release(sysmem);
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    RECT lock_rect;
    lock_rect.left   = x;
    lock_rect.top    = y;
    lock_rect.right  = x + w;
    lock_rect.bottom = y + h;
    D3DLOCKED_RECT lr;
    hr = IDirect3DSurface9_LockRect(sysmem, &lr, &lock_rect, D3DLOCK_READONLY);
    if (FAILED(hr))
    {
        IDirect3DSurface9_Release(sysmem);
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    size_t pxbytes = (size_t)w * (size_t)h * 4;
    ubyte* pixels = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE__RENDERER);
    ubyte* tmp    = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE__RENDERER);
    if (pixels == NULL_POINTER || tmp == NULL_POINTER)
    {
        if (pixels)
        {
            GUI_FREE(pixels);
        }
        if (tmp)
        {
            GUI_FREE(tmp);
        }
        IDirect3DSurface9_UnlockRect(sysmem);
        IDirect3DSurface9_Release(sysmem);
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    // D3D9 backbuffer is X8R8G8B8 / A8R8G8B8 -- BGRA in memory order
    // on little-endian. Swizzle to RGBA for the helper +
    // create_texture_rgba.
    for (int py = 0; py < h; py++)
    {
        ubyte* src = (ubyte*)lr.pBits + (size_t)py * (size_t)lr.Pitch;
        ubyte* dst = pixels           + (size_t)py * (size_t)w * 4;
        for (int px = 0; px < w; px++)
        {
            dst[px * 4 + 0] = src[px * 4 + 2]; // R <- B
            dst[px * 4 + 1] = src[px * 4 + 1]; // G
            dst[px * 4 + 2] = src[px * 4 + 0]; // B <- R
            dst[px * 4 + 3] = src[px * 4 + 3]; // A
        }
    }
    IDirect3DSurface9_UnlockRect(sysmem);
    IDirect3DSurface9_Release(sysmem);

    blur_cpu__apply_gaussian(pixels, tmp, w, h, sigma_px);

    void* tex = renderer__create_texture_rgba(pixels, w, h);

    gui_rect_t draw_rect;
    draw_rect.x = (float)x;
    draw_rect.y = (float)y;
    draw_rect.w = (float)w;
    draw_rect.h = (float)h;
    gui_color_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
    renderer__submit_image(draw_rect, tex, white);
    _d3d9_renderer_internal__flush_batches();
    renderer__destroy_texture(tex);

    GUI_FREE(tmp);
    GUI_FREE(pixels);
}

/**
 * @brief drain the pending rect + text batches mid-frame in the correct order (rects first, then text-on-top), WITHOUT changing scissor state or presenting. After this call the GPU has painted everything submitted so far, and subsequent submissions start a fresh batch that will draw ON TOP of what's already on the back buffer.
 *
 * @return void
 */
void renderer__flush_pending_draws(void)
{
    // Public thin wrapper over the internal flush so scene_render can
    // break the per-frame batch at z-index sibling boundaries. See
    // renderer.h for why this matters for text+z interleaving.
    if (_d3d9_renderer_internal__g.device == NULL_POINTER || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    _d3d9_renderer_internal__flush_batches();
}

/**
 * @brief finalize the frame: upload the batched verts, issue the draw, then present the back buffer to the window (swap buffers / swapchain Present). After this call returns, the frame is on screen.
 *
 * @return void
 */
void renderer__end_frame(void)
{
    if (_d3d9_renderer_internal__g.device == NULL_POINTER || _d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    // flush any remaining batched submissions.
    _d3d9_renderer_internal__flush_batches();

    // defensive scissor reset: if a widget pushed but failed to pop,
    // reset to the full viewport so the next frame starts clean.
    if (_d3d9_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("%s %lld %s", "renderer__end_frame: scissor stack non-empty (", (long long)_d3d9_renderer_internal__g.scissor_depth, "); resetting");
        _d3d9_renderer_internal__g.scissor_depth = 0;
        _d3d9_renderer_internal__apply_scissor_top();
    }

    // the host ends its own scene and presents
    if (_d3d9_renderer_internal__g.adopted == TRUE)
    {
        return;
    }

    dev->lpVtbl->EndScene(dev);

    _d3d9_renderer_internal__capture_if_requested();

    // Begin-frame checks cooperative level and resets a recoverable standalone device.
    HRESULT hr = dev->lpVtbl->Present(dev, NULL_POINTER, NULL_POINTER, NULL_POINTER, NULL_POINTER);
    if (hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET)
    {
        log_warn("%s", "device lost; waiting for recovery");
        _d3d9_renderer_internal__g.device_lost = TRUE;
    }
}

// ============================================================================
// INTERNAL: device creation
// ============================================================================

static boole _d3d9_renderer_internal__create_device(HWND hwnd)
{
    _d3d9_renderer_internal__g.d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (_d3d9_renderer_internal__g.d3d9 == NULL_POINTER)
    {
        log_error("%s", "Direct3DCreate9 failed");
        return FALSE;
    }

    D3DPRESENT_PARAMETERS pp;
    stdlib__memset(&pp, 0, sizeof(pp));
    pp.Windowed               = TRUE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth        = (UINT)_d3d9_renderer_internal__g.viewport_w;
    pp.BackBufferHeight       = (UINT)_d3d9_renderer_internal__g.viewport_h;
    pp.hDeviceWindow          = hwnd;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE; // vsync on.
    pp.EnableAutoDepthStencil = FALSE; // we don't use depth for 2d.

    HRESULT hr = _d3d9_renderer_internal__g.d3d9->lpVtbl->CreateDevice( _d3d9_renderer_internal__g.d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &_d3d9_renderer_internal__g.device );
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateDevice failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // seed last_applied_* to match the pp values we just created the
    // device with, so begin_frame's resize detection only trips when
    // the window actually changes size (not on the very first frame).
    _d3d9_renderer_internal__g.last_applied_w = _d3d9_renderer_internal__g.viewport_w;
    _d3d9_renderer_internal__g.last_applied_h = _d3d9_renderer_internal__g.viewport_h;

    _d3d9_renderer_internal__apply_render_state();

    return TRUE;
}

// apply the full render-state bag the VISUAL CONTRACT requires.
// factored out of create_device so reset_swapchain can re-apply it
// after IDirect3DDevice9::Reset (Reset wipes render state back to
// defaults).
static void _d3d9_renderer_internal__apply_render_state(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL_POINTER)
    {
        return;
    }

    // alpha blending matching the VISUAL CONTRACT:
    // - alpha blending: src=SRCALPHA, dst=INVSRCALPHA.
    // - no culling (we don't bother with winding order).
    // - fixed-function lighting off (irrelevant for shader path but
    // some drivers warn if you leave it on).
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOP,   D3DBLENDOP_ADD);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE,  D3DCULL_NONE);

    // Separate alpha blend so framebuffer alpha stays at 1.0 across
    // overlapping draws -- matches d3d11 and (post-fix) opengl3/gles3.
    // Without this, dst.a decays and DWM/compositor may treat the
    // window as semi-transparent during alpha animations.
    dev->lpVtbl->SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLENDALPHA,  D3DBLEND_ONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOPALPHA,   D3DBLENDOP_ADD);

    // Scissor: enable permanently. Each push/pop_scissor updates the
    // RECT itself via SetScissorRect; when the stack is empty, the
    // rect is set to the full viewport (no-op clip). Keeps the render
    // state stable across the frame -- no need to flip TRUE/FALSE on
    // each scissor change.
    dev->lpVtbl->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING,  FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE,   D3DZB_FALSE);
}

// resize the swap chain's backbuffer to (w, h) via Device::Reset.
// without this, a resized window stretches / letterboxes the old
// backbuffer onto the new client area -- what the user observed as
// "d3d9 stretches when resizing, opengl3/d3d11 don't".
// Every POOL_DEFAULT resource (solid, text and image vertex
// buffers) must be released first or Reset returns
// D3DERR_INVALIDCALL. render state also gets wiped and must be
// re-applied. POOL_MANAGED resources (our atlas textures) and the
// shaders + vertex declarations survive untouched.
static boole _d3d9_renderer_internal__reset_swapchain(int64 w, int64 h)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL_POINTER)
    {
        return FALSE;
    }
    if (w <= 0)
    {
        w = 1;
    }
    if (h <= 0)
    {
        h = 1;
    }

    // release POOL_DEFAULT resources. VERY IMPORTANT to do this BEFORE
    // Reset -- a single outstanding POOL_DEFAULT handle makes Reset
    // fail with D3DERR_INVALIDCALL and the device stays in a broken
    // state. we null the pointers so recreate can safely overwrite.
    _d3d9_renderer_internal__release_reset_resources();

    D3DPRESENT_PARAMETERS pp;
    stdlib__memset(&pp, 0, sizeof(pp));
    pp.Windowed               = TRUE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth        = (UINT)w;
    pp.BackBufferHeight       = (UINT)h;
    pp.hDeviceWindow          = _d3d9_renderer_internal__g.hwnd;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
    pp.EnableAutoDepthStencil = FALSE;

    HRESULT hr = dev->lpVtbl->Reset(dev, &pp);
    if (FAILED(hr))
    {
        // Leave drawing suspended; begin-frame retries when the device is recoverable.
        log_error("%s %08lX %s", "IDirect3DDevice9::Reset failed (0x", (unsigned long)hr, ")");
        _d3d9_renderer_internal__g.device_lost = TRUE;
        return FALSE;
    }

    if (_d3d9_renderer_internal__restore_reset_resources() == FALSE)
    {
        return FALSE;
    }

    // Reset clears render state, sampler state, and texture stage
    // state. re-apply the bag we rely on.
    _d3d9_renderer_internal__apply_render_state();

    _d3d9_renderer_internal__g.last_applied_w = w;
    _d3d9_renderer_internal__g.last_applied_h = h;
    return TRUE;
}

// ============================================================================
// INTERNAL: shader compilation
// ============================================================================

static boole _d3d9_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob)
{
    ID3DBlob* err_blob = NULL_POINTER;
    HRESULT   hr       = D3DCompile( src, stdlib__strlen(src), NULL_POINTER, NULL_POINTER, NULL_POINTER, entry, target, 0, 0, out_blob, &err_blob );
    if (FAILED(hr))
    {
        if (err_blob != NULL_POINTER)
        {
            log_error("%s %s %s %s", "shader compile error (", entry, "):\n", (char*)err_blob->lpVtbl->GetBufferPointer(err_blob));
            _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
        }
        else
        {
            log_error("%s %08lX %s", "D3DCompile failed (0x", (unsigned long)hr, ")");
        }
        return FALSE;
    }
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
    return TRUE;
}

// ============================================================================
// INTERNAL: pipeline setup
// ============================================================================

static boole _d3d9_renderer_internal__build_pipeline(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    // compile HLSL to shader model 3 bytecode.
    ID3DBlob* vs_blob = NULL_POINTER;
    ID3DBlob* ps_blob = NULL_POINTER;
    if (_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__vs_src, "vs_main", "vs_3_0", &vs_blob) == FALSE)
    {
        return FALSE;
    }
    if (_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__ps_src, "ps_main", "ps_3_0", &ps_blob) == FALSE)
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    // create shader objects from bytecode.
    HRESULT hr;
    hr = dev->lpVtbl->CreateVertexShader(dev, (const DWORD*)vs_blob->lpVtbl->GetBufferPointer(vs_blob), &_d3d9_renderer_internal__g.vs);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateVertexShader failed (0x", (unsigned long)hr, ")");
        goto fail;
    }
    hr = dev->lpVtbl->CreatePixelShader(dev, (const DWORD*)ps_blob->lpVtbl->GetBufferPointer(ps_blob), &_d3d9_renderer_internal__g.ps);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreatePixelShader failed (0x", (unsigned long)hr, ")");
        goto fail;
    }

    // vertex declaration. d3d9 matches elements to shader inputs by
    // semantic name + index, same as d3d11's input layout.
    // element stream=0 (the only stream we use), offsets in bytes,
    // type = FLOAT2/3/4/FLOAT1.
    D3DVERTEXELEMENT9 elems[] = {
        { 0,  0, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0,  8, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 24, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 32, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 40, D3DDECLTYPE_FLOAT1,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
        D3DDECL_END()
    };
    hr = dev->lpVtbl->CreateVertexDeclaration(dev, elems, &_d3d9_renderer_internal__g.decl);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateVertexDeclaration failed (0x", (unsigned long)hr, ")");
        goto fail;
    }

    // dynamic vertex buffer in POOL_DEFAULT. POOL_DEFAULT is the fast
    // GPU-memory pool but it's what gets lost on device lost.
    // POOL_MANAGED would survive Reset but is slower for dynamic data.
    UINT vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_QUADS* _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__vertex_t));
    hr = dev->lpVtbl->CreateVertexBuffer( dev, vbo_bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, /* FVF = 0 (we use a vertex declaration instead of fixed vertex format). */ D3DPOOL_DEFAULT, &_d3d9_renderer_internal__g.vbo, NULL_POINTER );
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateVertexBuffer failed (0x", (unsigned long)hr, ")");
        goto fail;
    }

    // cpu-side staging buffer.
    _d3d9_renderer_internal__g.vert_cap  = _D3D9_RENDERER_INTERNAL__MAX_QUADS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d9_renderer_internal__g.cpu_verts = (_d3d9_renderer_internal__vertex_t*)GUI_MALLOC_T((size_t)_d3d9_renderer_internal__g.vert_cap * sizeof(_d3d9_renderer_internal__vertex_t), MM_TYPE__RENDERER);
    if (_d3d9_renderer_internal__g.cpu_verts == NULL_POINTER)
    {
        log_error("%s", "out of memory for cpu vertex buffer");
        goto fail;
    }

    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    // text pipeline: separate shaders, VBO, vertex declaration.
    if (_d3d9_renderer_internal__build_text_pipeline() == FALSE)
    {
        return FALSE;
    }
    return TRUE;

fail:
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
    return FALSE;
}

// ============================================================================
// INTERNAL: text pipeline
// ============================================================================
// compiles the text VS+PS, builds a vertex declaration for the 3-attribute
// text vertex format, allocates a dynamic text VBO + cpu staging buffer.
// sampler state for the atlas is set per draw in end_frame (d3d9 doesn't
// have a "sampler state object" -- state is scalar per-stage).

static boole _d3d9_renderer_internal__build_text_pipeline(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;

    ID3DBlob* vs_blob = NULL_POINTER;
    ID3DBlob* ps_blob = NULL_POINTER;
    if (_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__text_vs_src, "vs_main", "vs_3_0", &vs_blob) == FALSE)
    {
        return FALSE;
    }
    if (_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__text_ps_src, "ps_main", "ps_3_0", &ps_blob) == FALSE)
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = dev->lpVtbl->CreateVertexShader(dev, (const DWORD*)vs_blob->lpVtbl->GetBufferPointer(vs_blob), &_d3d9_renderer_internal__g.text_vs);
    if (FAILED(hr))
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("%s %08lX %s", "CreateVertexShader (text) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }
    hr = dev->lpVtbl->CreatePixelShader(dev, (const DWORD*)ps_blob->lpVtbl->GetBufferPointer(ps_blob), &_d3d9_renderer_internal__g.text_ps);
    if (FAILED(hr))
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("%s %08lX %s", "CreatePixelShader (text) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    // text vertex declaration:
    // POSITION @ 0   (float2)
    // COLOR0   @ 8   (float4)
    // TEXCOORD0 @ 24 (float2)
    // matches the HLSL VSIn struct and the _d3d9_renderer_internal__text_vertex_t layout.
    D3DVERTEXELEMENT9 elems[] = {
        { 0,  0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0,  8, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    hr = dev->lpVtbl->CreateVertexDeclaration(dev, elems, &_d3d9_renderer_internal__g.text_decl);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateVertexDeclaration (text) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // dynamic text vertex buffer (POOL_DEFAULT, same tradeoff as the
    // solid VBO: fast but lost on device lost; recovery isn't wired up).
    UINT vbo_bytes = (UINT)(_D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS* _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d9_renderer_internal__text_vertex_t));
    hr = dev->lpVtbl->CreateVertexBuffer( dev, vbo_bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &_d3d9_renderer_internal__g.text_vbo, NULL_POINTER );
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateVertexBuffer (text) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // cpu-side text staging buffer.
    _d3d9_renderer_internal__g.text_vert_cap  = _D3D9_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d9_renderer_internal__g.text_cpu_verts = (_d3d9_renderer_internal__text_vertex_t*)GUI_MALLOC_T((size_t)_d3d9_renderer_internal__g.text_vert_cap * sizeof(_d3d9_renderer_internal__text_vertex_t), MM_TYPE__RENDERER);
    if (_d3d9_renderer_internal__g.text_cpu_verts == NULL_POINTER)
    {
        log_error("%s", "out of memory for cpu text vertex buffer");
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// INTERNAL: vertex append
// ============================================================================

static void _d3d9_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c)
{
    _d3d9_renderer_internal__vertex_t* v = &_d3d9_renderer_internal__g.cpu_verts[_d3d9_renderer_internal__g.vert_count++];
    v->x      = x;
    v->y      = y;
    v->r      = c.r;
    v->g      = c.g;
    v->b      = c.b;
    v->a      = c.a;
    v->lx     = lx;
    v->ly     = ly;
    v->rect_w = rw;
    v->rect_h = rh;
    v->radius = radius;
}

static void _d3d9_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color_t c)
{
    _d3d9_renderer_internal__text_vertex_t* tv = &_d3d9_renderer_internal__g.text_cpu_verts[_d3d9_renderer_internal__g.text_vert_count++];
    tv->x = x;
    tv->y = y;
    tv->r = c.r;
    tv->g = c.g;
    tv->b = c.b;
    tv->a = c.a;
    tv->u = u;
    tv->v = v;
}

// ============================================================================
// PUBLIC: text pipeline
// ============================================================================
// font.c produces an R8 atlas; d3d9 has no straight "one-channel 8-bit"
// format, but D3DFMT_L8 is functionally equivalent (luminance 8 -- the
// sampler returns .rrrr, and reading .r in the pixel shader gives us our
// coverage byte). we use D3DPOOL_MANAGED so the texture survives device
// lost without explicit recovery; the small upload cost is fine for
// one-time atlas creation.

/**
 * @brief upload an alpha-only bitmap to the GPU as a texture. Called by font.c when rasterizing a new (family, size) pair.
 *
 * @param const ubyte* pixels -> Row-major R8 bitmap (one byte per pixel, interpreted as alpha).
 * @param int width -> Bitmap width in pixels.
 * @param int height -> Bitmap height in pixels.
 *
 * @return void* -> Opaque backend-specific texture handle (GLuint / ID3D11ShaderResourceView* / IDirect3DTexture9*), or NULL on failure.
 */
void* renderer__create_atlas_r8(const ubyte* pixels, int width, int height)
{
    if (pixels == NULL_POINTER || width <= 0 || height <= 0 || _d3d9_renderer_internal__g.device == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    DWORD usage = 0;
    D3DPOOL pool = _d3d9_renderer_internal__texture_pool(&usage);

    IDirect3DTexture9* tex = NULL_POINTER;
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, (UINT)width, (UINT)height, 1, /* levels (no mips). */ usage, D3DFMT_L8, pool, &tex, NULL_POINTER);
    if (FAILED(hr) || tex == NULL_POINTER)
    {
        log_error("%s %08lX %s %d %s", "CreateTexture (atlas) failed (0x", (unsigned long)hr, ") in pool", (int)pool, "(1 managed, 0 default)");
        return NULL_POINTER;
    }

    // upload the bitmap. LockRect gives us a pointer + pitch; the driver
    // may have padded each row to a different stride than our source, so
    // we memcpy per row.
    D3DLOCKED_RECT lr;
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL_POINTER, 0);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "LockRect (atlas) failed (0x", (unsigned long)hr, ")");
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(tex);
        return NULL_POINTER;
    }

    ubyte*       dst     = (ubyte*)lr.pBits;
    const ubyte* src     = pixels;
    size_t       row_len = (size_t)width; // R8 = 1 byte per pixel.
    for (int y = 0; y < height; y++)
    {
        stdlib__memcpy(dst, src, row_len);
        dst += lr.Pitch;
        src += row_len;
    }
    tex->lpVtbl->UnlockRect(tex, 0);

    return (void*)tex;
}

/**
 * @brief destroy an atlas created by renderer__create_atlas_r8.
 *
 * @param void* atlas -> Handle returned by create_atlas_r8. NULL is a no-op.
 *
 * @return void
 */
void renderer__destroy_atlas(void* atlas)
{
    if (atlas == NULL_POINTER)
    {
        return;
    }
    IDirect3DTexture9* tex = (IDirect3DTexture9*)atlas;
    // clear our borrowed pointer if it matches, so the next frame's text
    // pass doesn't dereference a dangling texture.
    if (_d3d9_renderer_internal__g.current_text_atlas == tex)
    {
        _d3d9_renderer_internal__g.current_text_atlas = NULL_POINTER;
    }
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(tex);
}

/**
 * @brief bind the atlas to be sampled by subsequent renderer__submit_text_glyph calls (until either another atlas is bound or the frame ends). The text draw pass uses the last-bound atlas -- so all text in one frame that uses different fonts needs to be submitted in font-coherent runs.
 *
 * @param void* atlas -> Handle from create_atlas_r8.
 *
 * @return void
 */
void renderer__set_text_atlas(void* atlas)
{
    _d3d9_renderer_internal__g.current_text_atlas = (IDirect3DTexture9*)atlas;
}

/**
 * @brief queue one textured quad into the frame's text batch. Drawn after all solid rects when end_frame fires.
 *
 * @param gui_rect_t rect -> Quad position + size in pixels (top-left origin).
 * @param gui_rect_t uv -> UV rect into the atlas (0..1 coordinates; uv.w/h are sizes, not endpoints).
 * @param gui_color_t color -> Text color; per-pixel alpha = color.a * atlas_sample.r.
 *
 * @return void
 */
void renderer__submit_text_glyph(gui_rect_t rect, gui_rect_t uv, gui_color_t color)
{
    if (_d3d9_renderer_internal__g.device_lost)
    {
        return;
    }
    IDirect3DTexture9* atlas = _d3d9_renderer_internal__g.current_text_atlas;
    if (atlas == NULL_POINTER)
    {
        return;
    }
    if (_d3d9_renderer_internal__g.text_vert_cap < _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }
    boole new_run = (boole)(_d3d9_renderer_internal__g.text_run_count == 0 ||
        _d3d9_renderer_internal__g.text_runs[_d3d9_renderer_internal__g.text_run_count - 1].atlas != atlas);
    if (_d3d9_renderer_internal__g.text_vert_count + _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD > _d3d9_renderer_internal__g.text_vert_cap ||
        (new_run && _d3d9_renderer_internal__g.text_run_count >= _D3D9_RENDERER_INTERNAL__MAX_TEXT_RUNS))
    {
        // Keep the active scissor and draw order, then continue this string.
        // The flush resets the atlas, so restore it for subsequent glyphs.
        _d3d9_renderer_internal__flush_batches();
        _d3d9_renderer_internal__g.current_text_atlas = atlas;
    }

    // open or extend a run keyed by the current atlas texture.
    _d3d9_renderer_internal__text_run_t* run = NULL_POINTER;
    if (_d3d9_renderer_internal__g.text_run_count > 0)
    {
        run = &_d3d9_renderer_internal__g.text_runs[_d3d9_renderer_internal__g.text_run_count - 1];
        if (run->atlas != atlas)
        {
            run = NULL_POINTER;
        }
    }
    if (run == NULL_POINTER)
    {
        run = &_d3d9_renderer_internal__g.text_runs[_d3d9_renderer_internal__g.text_run_count++];
        run->atlas      = atlas;
        run->vert_start = _d3d9_renderer_internal__g.text_vert_count;
        run->vert_count = 0;
    }

    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    float u0 = uv.x;
    float v0 = uv.y;
    float u1 = uv.x + uv.w;
    float v1 = uv.y + uv.h;

    // same vertex ordering as solid rects (VISUAL CONTRACT).
    _d3d9_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _d3d9_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d9_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _d3d9_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d9_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _d3d9_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    run->vert_count += _D3D9_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

// ============================================================================
// IMAGE PIPELINE (RGBA8 textures via D3D9)
// ============================================================================
// Textures are IDirect3DTexture9 in D3DFMT_A8R8G8B8 (universally
// supported on D3D9 hardware), D3DPOOL_MANAGED so they survive a
// device reset without explicit recovery -- matches the text atlas
// pool choice. Upload via LockRect / memcpy-with-BGRA-swizzle /
// UnlockRect.
// Shader pair: VS is identical to the text pipeline's (pos + col +
// uv, viewport constant at c0 for screen-to-NDC). PS just does a
// full RGBA sample * color instead of the text path's .r * color.
// A dedicated vertex declaration declares only the 3 attributes
// the image VS uses (pos, color, uv) rather than the text pipeline's
// 5-attribute layout.

#include "shaders/generated/image.frag.h"

static boole _d3d9_renderer_internal__ensure_image_pipeline(void)
{
    if (_d3d9_renderer_internal__g.device_lost == TRUE)
    {
        return FALSE;
    }
    if (_d3d9_renderer_internal__g.image_ps != NULL_POINTER)
    {
        return TRUE;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL_POINTER)
    {
        return FALSE;
    }

    // Reuse the text VS source. Both pipelines share the same
    // screen-to-NDC transform + vertex format (pos + col + uv).
    ID3DBlob* vs_blob = NULL_POINTER;
    ID3DBlob* ps_blob = NULL_POINTER;
    if (_d3d9_renderer_internal__compile_shader(_d3d9_renderer_internal__text_vs_src, "vs_main", "vs_3_0", &vs_blob) == FALSE)
    {
        return FALSE;
    }
    if (_d3d9_renderer_internal__compile_shader(_D3D9_RENDERER_INTERNAL__IMAGE_PS_SRC, "ps_main", "ps_3_0", &ps_blob) == FALSE)
    {
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = dev->lpVtbl->CreateVertexShader(dev, (const DWORD*)vs_blob->lpVtbl->GetBufferPointer(vs_blob), &_d3d9_renderer_internal__g.image_vs);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image_vs CreateVertexShader failed (0x", (unsigned long)hr, ")");
        _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        return FALSE;
    }
    hr = dev->lpVtbl->CreatePixelShader(dev, (const DWORD*)ps_blob->lpVtbl->GetBufferPointer(ps_blob), &_d3d9_renderer_internal__g.image_ps);
    _D3D9_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image_ps CreatePixelShader failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    D3DVERTEXELEMENT9 elems[] = {
        { 0,  0, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0,  8, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 24, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    hr = dev->lpVtbl->CreateVertexDeclaration(dev, elems, &_d3d9_renderer_internal__g.image_decl);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image decl failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    UINT vb_bytes = (UINT)(6 * 8 * sizeof(float));
    hr = dev->lpVtbl->CreateVertexBuffer(dev, vb_bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &_d3d9_renderer_internal__g.image_vbo, NULL_POINTER);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image vbo failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief upload an RGBA8 image to the GPU. Bytes are consumed top-to-bottom, left-to-right, 4 bytes per pixel, no row padding (tightly packed). Caller retains ownership of the `rgba` buffer; the renderer copies the data into a texture and no longer references the source pointer after returning.
 *
 * @param const ubyte* rgba -> Tightly-packed RGBA8 pixel buffer, length = width*height*4.
 * @param int width -> Image width in pixels.
 * @param int height -> Image height in pixels.
 *
 * @return void* -> Opaque texture handle, or NULL if upload failed or the backend doesn't yet implement this pipeline.
 */
static void* _d3d9_renderer_internal__create_texture_rgba(const ubyte* rgba, int width, int height, boole generate_mips)
{
    if (rgba == NULL_POINTER || width <= 0 || height <= 0 || _d3d9_renderer_internal__g.device_lost == TRUE)
    {
        return NULL_POINTER;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    // the mip chain is built on the CPU by the shared helper so every
    // backend samples the same texels (see _mip_cpu.h). the automatic
    // chain (D3DUSAGE_AUTOGENMIPMAP) needed the default pool, whose
    // textures cannot be locked for the upload and are lost on a device
    // reset, which an adopting host may do at any time; so the levels go
    // into a managed texture one LockRect each: lockable, restored by
    // the runtime after a reset, trilinear once the image sampler's
    // MIPFILTER is LINEAR. the chain matters: photo-sized sources drawn
    // as 48 px tiles twinkle under plain bilinear minification.
    mip_cpu__chain_t chain;
    ubyte*           storage       = NULL_POINTER;
    size_t           storage_bytes = generate_mips == TRUE ? mip_cpu__storage_bytes(width, height) : 0;
    int              level         = 0;
    DWORD            usage         = 0;
    D3DPOOL          pool          = _d3d9_renderer_internal__texture_pool(&usage);

    if (storage_bytes > 0)
    {
        storage = (ubyte*)GUI_MALLOC_T(storage_bytes, MM_TYPE__RENDERER);
    }

    mip_cpu__build(&chain, rgba, width, height, storage);

    IDirect3DTexture9* tex = NULL_POINTER;
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, (UINT)width, (UINT)height, (UINT)chain.count, usage, D3DFMT_A8R8G8B8, pool, &tex, NULL_POINTER);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s %d %s %d %s %d", "image CreateTexture failed (0x", (unsigned long)hr, ") for", width, "x", height, "in pool", (int)pool);
        if (storage != NULL_POINTER)
        {
            GUI_FREE(storage);
        }
        return NULL_POINTER;
    }

    for (level = 0; level < chain.count; level++)
    {
        const mip_cpu__level_t* lv = &chain.levels[level];
        D3DLOCKED_RECT          locked;
        int                     y = 0;

        hr = tex->lpVtbl->LockRect(tex, (UINT)level, &locked, NULL_POINTER, 0);
        if (FAILED(hr))
        {
            log_error("%s %08lX %s %d", "image LockRect failed (0x", (unsigned long)hr, ") on level", level);
            tex->lpVtbl->Release(tex);
            if (storage != NULL_POINTER)
            {
                GUI_FREE(storage);
            }
            return NULL_POINTER;
        }

        // stb_image hands over R,G,B,A byte order; D3DFMT_A8R8G8B8 on a
        // little-endian machine is B,G,R,A in memory, so every pixel is
        // swizzled on the way in. once per image at upload, not per frame.
        for (y = 0; y < lv->h; y++)
        {
            const ubyte* src = lv->pixels + (size_t)y * (size_t)lv->w * 4u;
            ubyte*       dst = (ubyte*)locked.pBits + (size_t)y * (size_t)locked.Pitch;
            int          x   = 0;

            for (x = 0; x < lv->w; x++)
            {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
                src += 4;
                dst += 4;
            }
        }

        tex->lpVtbl->UnlockRect(tex, (UINT)level);
    }

    if (storage != NULL_POINTER)
    {
        GUI_FREE(storage);
    }

    return (void*)tex;
}

// Public image uploads retain mipmaps for images that can be drawn smaller.
void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _d3d9_renderer_internal__create_texture_rgba(rgba, width, height, TRUE);
}

// The compositor's final surface is displayed at one texel per viewport pixel.
// Its lower mip levels would be rebuilt and uploaded every frame but never used.
GUI_API void* renderer_backend__create_frame_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _d3d9_renderer_internal__create_texture_rgba(rgba, width, height, FALSE);
}

/**
 * @brief release a texture returned by renderer__create_texture_rgba. NULL is a no-op so widgets that never successfully uploaded can still call this from their destroy hook without a guard.
 *
 * @param void* tex -> Handle from create_texture_rgba.
 *
 * @return void
 */
void renderer__destroy_texture(void* tex)
{
    if (tex == NULL_POINTER)
    {
        return;
    }
    IDirect3DTexture9* t = (IDirect3DTexture9*)tex;
    t->lpVtbl->Release(t);
}

/**
 * @brief draw one image quad immediately. Flushes any pending solid-rect or text batch first so previously-submitted content stays underneath and subsequent submissions stay on top. Full-texture draw (UV 0..1 on both axes); no subregion support in this pass (crop via node bounds + scissor instead).
 *
 * @param gui_rect_t rect -> Destination rect in top-left-origin pixel coords.
 * @param void* tex -> Texture handle from create_texture_rgba. NULL = skip draw.
 * @param gui_color_t tint -> Multiplied against the sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 * @param float u0 -> left texture coordinate of the sub-rect to sample
 * @param float v0 -> top texture coordinate of the sub-rect to sample
 * @param float u1 -> right texture coordinate of the sub-rect to sample
 * @param float v1 -> bottom texture coordinate of the sub-rect to sample
 *
 * @return void
 */
void renderer__submit_image_uv(gui_rect_t rect, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    if (tex == NULL_POINTER)
    {
        return;
    }
    if (_d3d9_renderer_internal__ensure_image_pipeline() == FALSE)
    {
        return;
    }
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    if (dev == NULL_POINTER)
    {
        return;
    }

    // Flush pending rect / text batches so their Draws land under
    // this image in submit order.
    _d3d9_renderer_internal__flush_batches();

    // Viewport constant into c0. flush_batches normally does this
    // before each draw, but text / rect paths may not have run this
    // frame; ensure the VS gets its viewport either way.
    float cb[4] = {
        (float)_d3d9_renderer_internal__g.viewport_w,
        (float)_d3d9_renderer_internal__g.viewport_h,
        0.0f, 0.0f,
    };
    dev->lpVtbl->SetVertexShaderConstantF(dev, 0, cb, 1);

    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    // 6 verts, pos(2) col(4) uv(2) = 8 floats each.
    float verts[6 * 8] = {
        x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0,
        x1, y0, tint.r, tint.g, tint.b, tint.a, u1, v0,
        x0, y1, tint.r, tint.g, tint.b, tint.a, u0, v1,
        x1, y0, tint.r, tint.g, tint.b, tint.a, u1, v0,
        x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1,
        x0, y1, tint.r, tint.g, tint.b, tint.a, u0, v1,
    };

    void* mapped = NULL_POINTER;
    HRESULT hr = _d3d9_renderer_internal__g.image_vbo->lpVtbl->Lock(_d3d9_renderer_internal__g.image_vbo, 0, sizeof(verts), &mapped, D3DLOCK_DISCARD);
    if (FAILED(hr) || mapped == NULL_POINTER)
    {
        return;
    }
    stdlib__memcpy(mapped, verts, sizeof(verts));
    _d3d9_renderer_internal__g.image_vbo->lpVtbl->Unlock(_d3d9_renderer_internal__g.image_vbo);

    dev->lpVtbl->SetVertexDeclaration(dev, _d3d9_renderer_internal__g.image_decl);
    dev->lpVtbl->SetStreamSource(dev, 0, _d3d9_renderer_internal__g.image_vbo, 0, (UINT)(8 * sizeof(float)));
    dev->lpVtbl->SetVertexShader(dev, _d3d9_renderer_internal__g.image_vs);
    dev->lpVtbl->SetPixelShader(dev, _d3d9_renderer_internal__g.image_ps);

    // Sampler state on stage 0. D3D9 has no sampler-state objects,
    // so we set each knob individually. MIPFILTER=LINEAR completes
    // the trilinear triangle (MIN + MAG + MIP all LINEAR) so the
    // mipmap chain we built in create_texture_rgba actually gets
    // sampled -- without it MIPFILTER_NONE ignores the mips and we
    // get the same bilinear-only shimmer the pre-mipmap build had.
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev->lpVtbl->SetSamplerState(dev, 0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);

    dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
    dev->lpVtbl->DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 2);
    dev->lpVtbl->SetTexture(dev, 0, NULL_POINTER);
}

/**
 * @brief queue a textured quad covering rect with the whole texture; convenience wrapper over renderer__submit_image_uv with the full [0,1] range
 *
 * @param gui_rect_t rect -> destination rectangle in pixels
 * @param void* tex -> texture handle from renderer__create_texture_rgba
 * @param gui_color_t tint -> multiplied color
 *
 * @return void
 */
void renderer__submit_image(gui_rect_t rect, void* tex, gui_color_t tint)
{
    renderer__submit_image_uv(rect, tex, tint, 0.0f, 0.0f, 1.0f, 1.0f);
}

// Shadow: multi-layer concentric rects with quadratic falloff.
// Same approach as opengl3/gles3/d3d11 -- stacks of submit_rect calls
// with increasing size and decreasing alpha approximate a Gaussian
// feather without needing a separate blur shader.
/**
 * @brief queue a shadow splat for the current frame. Same rect convention as submit_rect. `blur` is the feather width in pixels. Caller supplies the final offset rect (rect.x already includes dx, rect.y includes dy) so the renderer doesn't have to know about the originating shape.
 *
 * @param gui_rect_t rect -> Shadow bounding rect (already offset).
 * @param gui_color_t c -> Shadow color (alpha controls strength).
 * @param float radius -> Same corner radius as the decorated shape.
 * @param float blur -> Soft-edge width in pixels. 0 = hard edge.
 *
 * @return void
 */
void renderer__submit_shadow(gui_rect_t rect, gui_color_t c, float radius, float blur)
{
    if (c.a <= 0.0f)
    {
        return;
    }
    int layers = (int)(blur * 0.4f) + 2;
    if (layers < 2)
    {
        layers = 2;
    }
    if (layers > 8)
    {
        layers = 8;
    }
    float step = blur / (float)layers;
    for (int i = 0; i < layers; i++)
    {
        float grow = step * (float)i;
        float t = (float)i / (float)(layers - 1);
        float falloff = (1.0f - t) * (1.0f - t);
        gui_color_t layer_c = c;
        layer_c.a = c.a * falloff / (float)layers;
        gui_rect_t lr;
        lr.x = rect.x - grow;
        lr.y = rect.y - grow;
        lr.w = rect.w + 2.0f * grow;
        lr.h = rect.h + 2.0f * grow;
        renderer__submit_rect(lr, layer_c, radius + grow);
    }
}

// Gradient: per-corner colors via the existing SDF-rect vertex format.
// The PS interpolates v_color across the triangle.
/**
 * @brief two-color linear gradient rect. Drop-in replacement for submit_rect when a gradient is requested. `direction` selects the axis via gui_gradient_dir_e.
 *
 * @param gui_rect_t r -> Rect in top-left-origin pixel coords.
 * @param gui_color_t from -> Color at the start of the axis.
 * @param gui_color_t to -> Color at the end of the axis.
 * @param int direction -> gui_gradient_dir_e value. Cast to int for ABI stability across the DLL boundary.
 * @param float radius -> Corner radius in pixels.
 *
 * @return void
 */
void renderer__submit_rect_gradient(gui_rect_t r, gui_color_t from, gui_color_t to, int direction, float radius)
{
    gui_gradient_t g;

    stdlib__null_memory(&g, sizeof(g));
    gradient__two_color(&g, from, to, (gui_gradient_dir_e)direction);
    renderer__submit_gradient(r, &g, 1.0f, 1.0f, radius);
}

/**
 * @brief a CSS gradient over a rect: tessellated by gradient__tessellate (shared by every renderer, so the pixels agree) and pushed through the rect pipeline like a solid rect, which applies the rounded-corner mask and the anti-aliased edge to the triangles.
 *
 * @param gui_rect_t r -> Rect in top-left-origin pixel coords.
 * @param const gui_gradient_t* g -> The gradient (linear or radial, with its stops).
 * @param float scale -> The UI scale, for pixel lengths inside the gradient.
 * @param float opacity -> Multiplies every stop's alpha.
 * @param float radius -> Corner radius in pixels.
 *
 * @return void
 */
GUI_API boole renderer_backend__presents_with_vsync(void)
{
    return (boole)(_d3d9_renderer_internal__g.adopted == FALSE && _d3d9_renderer_internal__g.device_lost == FALSE);
}

void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color)
{
    gui_vec2_t vertices[3] = {a, b, c};
    float left = stdlib__fminf(a.x, stdlib__fminf(b.x, c.x)) - 1;
    float top = stdlib__fminf(a.y, stdlib__fminf(b.y, c.y)) - 1;
    float width = stdlib__fmaxf(a.x, stdlib__fmaxf(b.x, c.x)) - left + 1;
    float height = stdlib__fmaxf(a.y, stdlib__fmaxf(b.y, c.y)) - top + 1;
    int i;
    if (_d3d9_renderer_internal__g.vert_count + 3 > _d3d9_renderer_internal__g.vert_cap) return;
    // Expand the rectangle mask past the geometry. Triangle rasterization
    // owns the edges; the existing colour pipeline needs no extra shader.
    for (i = 0; i < 3; i++)
        _d3d9_renderer_internal__push_vertex(vertices[i].x, vertices[i].y,
            vertices[i].x - left, vertices[i].y - top, width, height, 0, color);
}

void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    int64 count = 0;
    int64 i     = 0;
    float max_r = 0.0f;

    count = gradient__tessellate(g, r, scale, opacity, verts, GRADIENT_MAX_VERTS);

    if (count <= 0 || _d3d9_renderer_internal__g.vert_count + count > _d3d9_renderer_internal__g.vert_cap)
    {
        return;
    }

    max_r = (r.w < r.h ? r.w : r.h) * 0.5f;

    if (radius < 0.0f)
    {
        radius = 0.0f;
    }

    if (radius > max_r)
    {
        radius = max_r;
    }

    for (i = 0; i < count; i++)
    {
        _d3d9_renderer_internal__push_vertex(verts[i].x, verts[i].y, verts[i].x - r.x, verts[i].y - r.y, r.w, r.h, radius, verts[i].c);
    }
}

// ===== frame capture state (see renderer.h) ================================
static ubyte* _d3d9_renderer_internal__capture_out  = NULL_POINTER;
static int64  _d3d9_renderer_internal__capture_cap  = 0;
static boole  _d3d9_renderer_internal__capture_done = FALSE;

// The armed frame capture: GetRenderTargetData into a system-memory
// surface, then the X8R8G8B8 rows swizzled to RGBA while copying out.
// Same mechanics as renderer__blur_region's readback, for the whole
// buffer.
static void _d3d9_renderer_internal__capture_if_requested(void)
{
    IDirect3DDevice9* dev = _d3d9_renderer_internal__g.device;
    int64 w = (int64)_d3d9_renderer_internal__g.viewport_w;
    int64 h = (int64)_d3d9_renderer_internal__g.viewport_h;
    IDirect3DSurface9* rt = NULL_POINTER;
    IDirect3DSurface9* sysmem = NULL_POINTER;
    D3DSURFACE_DESC rtd;
    D3DLOCKED_RECT lr;
    HRESULT hr = 0;
    int64 x = 0;
    int64 y = 0;

    if (_d3d9_renderer_internal__capture_out == NULL_POINTER || dev == NULL_POINTER || w <= 0 || h <= 0 || _d3d9_renderer_internal__capture_cap < w * h * 4)
    {
        return;
    }

    hr = IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);

    if (FAILED(hr) || rt == NULL_POINTER)
    {
        _d3d9_renderer_internal__capture_out = NULL_POINTER;
        return;
    }

    if (FAILED(IDirect3DSurface9_GetDesc(rt, &rtd)) || FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, rtd.Width, rtd.Height, rtd.Format, D3DPOOL_SYSTEMMEM, &sysmem, NULL_POINTER)) || sysmem == NULL_POINTER)
    {
        IDirect3DSurface9_Release(rt);
        _d3d9_renderer_internal__capture_out = NULL_POINTER;
        return;
    }

    hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sysmem);
    IDirect3DSurface9_Release(rt);

    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(sysmem, &lr, NULL_POINTER, D3DLOCK_READONLY)))
    {
        for (y = 0; y < h && y < (int64)rtd.Height; y++)
        {
            const ubyte* src = (const ubyte*)lr.pBits + (size_t)y * (size_t)lr.Pitch;
            ubyte* dst = _d3d9_renderer_internal__capture_out + (size_t)y * (size_t)w * 4;

            for (x = 0; x < w && x < (int64)rtd.Width; x++)
            {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }

        IDirect3DSurface9_UnlockRect(sysmem);
        _d3d9_renderer_internal__capture_done = TRUE;
    }

    IDirect3DSurface9_Release(sysmem);
    _d3d9_renderer_internal__capture_out = NULL_POINTER;
}


/**
 * @brief size of the buffer renderer__request_capture needs: the back buffer's pixel size
 *
 * @param int64* out_w -> receives the width in pixels
 * @param int64* out_h -> receives the height in pixels
 *
 * @return boole -> FALSE before the renderer has a viewport
 */
boole renderer__capture_size(int64* out_w, int64* out_h)
{
    int64 w = (int64)_d3d9_renderer_internal__g.viewport_w;
    int64 h = (int64)_d3d9_renderer_internal__g.viewport_h;

    if (out_w != NULL_POINTER)
    {
        *out_w = w;
    }

    if (out_h != NULL_POINTER)
    {
        *out_h = h;
    }

    return (boole)(w > 0 && h > 0);
}

/**
 * @brief arms a copy of the next finished frame into out_rgba (top-down RGBA8, width * height * 4 bytes), taken at the next renderer__end_frame before the frame is presented
 *
 * @param ubyte* out_rgba -> destination, owned by the caller
 * @param int64 cap -> destination capacity in bytes
 *
 * @return boole -> FALSE when the buffer is too small or there is no viewport
 */
boole renderer__request_capture(ubyte* out_rgba, int64 cap)
{
    int64 w = 0;
    int64 h = 0;

    if (out_rgba == NULL_POINTER || renderer__capture_size(&w, &h) == FALSE || cap < w * h * 4)
    {
        return FALSE;
    }

    _d3d9_renderer_internal__capture_out  = out_rgba;
    _d3d9_renderer_internal__capture_cap  = cap;
    _d3d9_renderer_internal__capture_done = FALSE;

    return TRUE;
}

/**
 * @brief tells whether the armed capture has been copied out (it is, once renderer__end_frame ran after the request)
 *
 * @return boole -> TRUE after the copy
 */
boole renderer__capture_done(void)
{
    return _d3d9_renderer_internal__capture_done;
}

// Ordinary standalone scenes can use this renderer directly.
GUI_API boole renderer_backend__needs_compositor(void) { return FALSE; }

// Shared by standalone and embedded D3D9 builds.
#include "d3d9_layers.h"

#include "d3d9_blur.h"
