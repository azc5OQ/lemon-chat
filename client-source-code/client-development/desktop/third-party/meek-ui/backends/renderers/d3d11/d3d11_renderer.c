// ============================================================================
// d3d11_renderer.c - Direct3D 11 backend.
// ============================================================================
// WHAT THIS FILE DOES (the elevator pitch):
// scene.c walks the widget tree and tells us "draw this colored rectangle
// here" by calling renderer__submit_rect(). this file packages those calls
// into work the GPU understands and ships it across to the graphics card
// using the Direct3D 11 api -- microsoft's "modern" graphics api on
// Windows (replaced by D3D12 conceptually, but D3D11 ships on every
// Windows 7+ box and is dramatically simpler to use).
// this backend is interchangeable with opengl3_renderer.c and
// d3d9_renderer.c -- exactly one is compiled into gui.dll at a time
// (selected via the GUI_RENDERER CMake cache variable). all three
// produce visually-identical output (see renderer.h "VISUAL CONTRACT").
// D3D11 IN PLAIN C (not C++):
// d3d11 is a COM api and the headers are C++-friendly by default. we
// define COBJMACROS before including <d3d11.h> which enables the C-style
// macros like ID3D11Device_CreateBuffer(device, ...) instead of
// device->lpVtbl->CreateBuffer(device, ...). the rest of the code looks
// mostly like regular C.
// every COM pointer we get back must be Release()'d when we're done. we
// use the _SAFE_RELEASE macro below to null-check + release in one go,
// so the shutdown path is resilient to init bailing halfway.
// THE PIPELINE (the rocket-stage analogy):
// 1. INPUT-ASSEMBLY (IA): tell D3D where the vertex bytes are and what
// shape they have (vertex format, primitive topology). like loading
// raw materials into the rocket -- "here's the data, here's its
// schema".
// 2. VERTEX SHADER (VS): a tiny program that runs ONCE PER VERTEX. our
// VS reads pixel coords from each vertex, converts to D3D's NDC
// (-1..1 with y-up), and passes through color + the SDF "varyings"
// that the pixel shader needs. running this on the GPU instead of
// doing the math CPU-side avoids constant CPU-GPU round trips.
// 3. RASTERIZER (RS): the GPU's hardware fixed function. takes the
// transformed vertices, figures out which pixels are inside the
// triangles, and runs the pixel shader once per pixel. ScissorEnable
// lives here -- when on, pixels outside the scissor RECT are
// discarded before the pixel shader runs.
// 4. PIXEL SHADER (PS): runs ONCE PER PIXEL inside our triangles. ours
// computes the SDF (signed distance function) for a rounded box,
// antialiases the edge with a 1-pixel smoothstep, and writes the
// final color (or `discard`s if outside the rounded shape -- that's
// what makes the rounded corners actually rounded instead of
// square-with-a-shaded-corner).
// 5. OUTPUT-MERGER (OM): the final stage. takes the pixel shader's
// output and blends it with what's already in the render target
// (this is where alpha blending happens). then writes the result
// to the back buffer.
// 6. PRESENT: hand the back buffer to the OS to display on screen.
// the back buffer becomes the front buffer; the previous front
// becomes the new back. this is the "swap" in "swap chain".
// WHY BATCHING?
// each call from CPU to GPU costs hundreds of microseconds of overhead --
// driver bookkeeping, command queue latency. drawing 100 rects with 100
// separate draw calls is dramatically slower than gathering them into a
// list of 600 vertices and issuing ONE draw call:
// begin_frame:    Map the back buffer for clearing, reset our
// cpu-side vertex list.
// submit_rect *N: append 6 vertices to the list per rectangle.
// end_frame:      flush_batches uploads the WHOLE vertex list to
// the GPU vertex buffer (one Map+memcpy+Unmap),
// sets pipeline state once, issues ONE Draw, then
// Present.
// the per-batch flush only fragments when state changes mid-frame
// (push_scissor or set_text_atlas during the recursion). each flush
// draws what's accumulated so far before changing GPU state.
// RESIZE HANDLING:
// unlike opengl (where the swapchain auto-tracks the window), d3d11
// requires an explicit call to IDXGISwapChain::ResizeBuffers plus
// recreating the render-target view after a window resize. we detect
// size changes at the start of each frame by comparing the viewport
// passed to begin_frame against what we saw last frame.
// VISUAL CONTRACT:
// this backend must produce the same pixels as every other backend
// given the same input -- see renderer.h's "VISUAL CONTRACT" section.
// the SDF function, blend mode, smoothstep range, scissor semantics,
// and radius clamping are all specified; don't diverge from them.

#define COBJMACROS
#include "effects_profile.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "gui.h"
#define MEEK_RENDERER_BACKEND
#include "renderer.h"
#include "renderer_backend.h"
#include "gradient.h"
#include "d3d11_renderer.h"
#include "_visual_contract.h"
#include "clib/stdlib.h"
#include "_blur_cpu.h"
#include "_mip_cpu.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"

// convenience: release a COM ptr and null it out. the "(IUnknown*)" cast
// is to satisfy strict compilers; IUnknown_Release works on any COM iface.
#define _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(p)           \
    do                                                      \
    {                                                       \
        if ((p) != NULL_POINTER)                                    \
        {                                                   \
            IUnknown_Release((IUnknown*)(p));               \
            (p) = NULL_POINTER;                                     \
        }                                                   \
    } while (0)

// ============================================================================
// VERTEX FORMAT
// ============================================================================
// identical to the OpenGL backend's vertex. the 11 floats map to the same
// shader inputs (POSITION, COLOR, TEXCOORD0/1/2 for local/rect_size/radius).

typedef struct _d3d11_renderer_internal__vertex_t
{
    // pixel position, top-left origin.
    float x;        // 0x0
    float y;        // 0x4
    // rgba, linear 0..1.
    float r;        // 0x8
    float g;        // 0xC
    float b;        // 0x10
    float a;        // 0x14
    // local position inside the rect, 0..rect_w/h.
    float lx;       // 0x18
    float ly;       // 0x1C
    // rect size, passed through as varying for the fragment SDF.
    float rect_w;   // 0x20
    float rect_h;   // 0x24
    // corner radius in pixels.
    float radius;   // 0x28
} _d3d11_renderer_internal__vertex_t;

// text vertex: simpler layout (position + color + uv). one per corner of
// each glyph quad. 6 per glyph.
typedef struct _d3d11_renderer_internal__text_vertex_t
{
    // pixel position, top-left origin.
    float x;
    float y;
    float r, g, b, a; // text color (atlas supplies alpha/coverage).
    // UV in 0..1 into the atlas texture.
    float u;
    float v;
} _d3d11_renderer_internal__text_vertex_t;

#define _D3D11_RENDERER_INTERNAL__MAX_QUADS       16384
#define _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD  6

// text batch cap. matches the opengl3 backend (4096 glyphs) for the same
// ARM/mobile-conscious reason -- small enough to stay under embedded-GPU
// working-set limits.
#define _D3D11_RENDERER_INTERNAL__MAX_TEXT_GLYPHS 4096

// Text runs: one per consecutive atlas SRV in a batch. A full run or
// glyph buffer is drained before accepting another glyph; these are
// bounded working buffers, not limits on text submitted in a frame.
// Each run maps to one Draw call with its SRV bound to PS register t0.
#define _D3D11_RENDERER_INTERNAL__MAX_TEXT_RUNS 64

typedef struct _d3d11_renderer_internal__text_run_t
{
    // SRV bound for this run's draw.
    ID3D11ShaderResourceView* atlas;        // 0x0
    // offset into text_cpu_verts.
    int64                     vert_start;   // 0x8
    // verts in this run.
    int64                     vert_count;   // 0x10
} _d3d11_renderer_internal__text_run_t;

// ============================================================================
// CONSTANT BUFFER LAYOUT
// ============================================================================
// d3d11 constant buffers are 16-byte-aligned. we only need a float2 but
// pad to float4 (16 bytes).

typedef struct _d3d11_renderer_internal__cb_globals_t
{
    float viewport_w;   // 0x0
    float viewport_h;   // 0x4
    float _padding_0;   // 0x8
    float _padding_1;   // 0xC
} _d3d11_renderer_internal__cb_globals_t;

// ============================================================================
// HLSL SHADER SOURCES
// ============================================================================
// direct translation of the GLSL shaders in opengl3_renderer.c. differences:
// - HLSL uses "float2/float3/float4" instead of "vec2/vec3/vec4".
// - semantics (POSITION, COLOR, TEXCOORDn, SV_POSITION, SV_TARGET) link
// shader inputs/outputs to the input-layout slots we create below.
// - the NDC conversion is identical -- d3d11 NDC is y-up with top-left
// origin (same as opengl after our vertex shader's y-flip), so the
// same math produces the same result.

#include "shaders/generated/rect.vert.h"

#include "shaders/generated/rect.frag.h"

// TEXT SHADERS (HLSL SM5). direct port of the GL text shaders.
// - single-channel atlas texture bound to t0.
// - LINEAR-filtering sampler bound to s0.
// - same pixel -> NDC conversion as the solid vs.
// - output color = vertex_color.rgb + (vertex_color.a * atlas.r) alpha.
#include "shaders/generated/text.vert.h"

// See opengl3_renderer.c text_fs_src for the full rationale on the
// pow(a, 1/2.2) step. Short version: we blend in sRGB space (the
// swap chain is DXGI_FORMAT_R8G8B8A8_UNORM, not _SRGB), which
// darkens partial-coverage edges. Reshaping the atlas coverage
// through the gamma curve produces edges that land at the
// perceptual midpoint instead of the sRGB-linear midpoint, so
// text stops looking washed-out on dark backgrounds.
#include "shaders/generated/text.frag.h"

// ============================================================================
// STATE
// ============================================================================

typedef struct _d3d11_renderer_internal__state_t
{
    HWND hwnd;                                                                               // 0x0

    // core objects
    ID3D11Device*           device;                                                          // 0x8
    ID3D11DeviceContext*    context;                                                         // 0x10
    IDXGISwapChain*         swapchain;                                                       // 0x18
    ID3D11RenderTargetView* rtv;                                                             // 0x20

    // adoption mode: TRUE when renderer__init_adopt received a caller-owned device/context/swapchain
    // shutdown then releases none of the three, begin_frame skips its clear, end_frame skips Present
    boole adopted;                                                                           // 0x28

    // solid-rect pipeline
    ID3D11VertexShader* vs;                                                                  // 0x30
    ID3D11PixelShader*  ps;                                                                  // 0x38
    ID3D11InputLayout*  input_layout;                                                        // 0x40
    // dynamic vertex buffer
    ID3D11Buffer*       vbo;                                                                 // 0x48
    // per-frame globals (viewport)
    ID3D11Buffer*       cbuffer;                                                             // 0x50
    ID3D11BlendState*   blend_state;                                                         // 0x58

    // cpu side
    _d3d11_renderer_internal__vertex_t* cpu_verts;                                             // 0x60
    int64                             vert_count;                                            // 0x68
    int64                             vert_cap;                                              // 0x70

    int64 viewport_w;                                                                        // 0x78
    int64 viewport_h;                                                                        // 0x80

    // text pipeline. mirrors the solid-rect set but has its own shaders,
    // input layout, and VBO. also owns a sampler state for the atlas and
    // a cached "currently bound atlas" SRV pointer.
    // per-atlas run batching: multiple fonts/sizes per frame land in
    // the same VBO as a sequence of runs, each tagged with its SRV.
    // end_frame binds each SRV in turn and issues one Draw per run.
    ID3D11VertexShader*       text_vs;                                                       // 0x88
    ID3D11PixelShader*        text_ps;                                                       // 0x90
    ID3D11InputLayout*        text_input_layout;                                             // 0x98
    ID3D11Buffer*             text_vbo;                                                      // 0xA0
    ID3D11SamplerState*       text_sampler;                                                  // 0xA8
    // not owned; set by renderer__set_text_atlas.
    ID3D11ShaderResourceView* current_text_atlas;                                            // 0xB0

    _d3d11_renderer_internal__text_vertex_t* text_cpu_verts;                                   // 0xB8
    int64                                  text_vert_count;                                  // 0xC0
    int64                                  text_vert_cap;                                    // 0xC8

    _d3d11_renderer_internal__text_run_t text_runs[_D3D11_RENDERER_INTERNAL__MAX_TEXT_RUNS];   // 0xD0
    int64                              text_run_count;                                       // 0x6D0

    // Image pipeline. Lazy-built on first renderer__submit_image call
    // so scenes that don't use <image> / background-image don't pay
    // for the shader compile + VBO alloc. Reuses the text sampler
    // (same LINEAR / CLAMP knobs) and shares the same vertex layout
    // (pos + color + uv = 32 bytes), just a separate PS that samples
    // the texture's full RGBA instead of text's alpha-only .r channel.
    ID3D11VertexShader* image_vs;                                                            // 0x6D8
    ID3D11PixelShader*  image_ps;                                                            // 0x6E0
    ID3D11InputLayout*  image_input_layout;                                                  // 0x6E8
    // 6-vert scratch, reused per submit.
    ID3D11Buffer*       image_vbo;                                                           // 0x6F0

    // Scissor stack + rasterizer state. The rasterizer state has
    // ScissorEnable=TRUE so the GPU clips to whatever rect was last
    // set via RSSetScissorRects. When the stack is empty the scissor
    // rect is set to the full viewport (which is a no-op clip),
    // so we don't have to swap rasterizer states on every push/pop.
    ID3D11RasterizerState* rasterizer_state;                                                 // 0x6F8
    gui_rect_t               scissor_stack[32];                                                // 0x700
    int64                  scissor_depth;                                                    // 0x900

    // Layer pipeline (renderer_backend.h, layer mode): a homogeneous quad
    // (x * w, y * w, w) + uv, a premultiplied texel times the opacity in
    // b1, three blend states (normal premultiplied, screen, replace) and a
    // point sampler for integer-aligned identity quads. Lazy-built.
    ID3D11VertexShader* blur_vs;
    ID3D11PixelShader* blur_ps;
    ID3D11Buffer* blur_constants;
    ID3D11Texture2D* blur_texture[2];
    ID3D11RenderTargetView* blur_rtv[2];
    ID3D11ShaderResourceView* blur_srv[2];
    int blur_w;
    int blur_h;
    DXGI_FORMAT blur_format;
    ID3D11VertexShader* layer_vs;
    ID3D11PixelShader*  layer_ps;
    ID3D11InputLayout*  layer_input_layout;
    ID3D11Buffer*       layer_vbo;
    ID3D11BlendState*   layer_blend_normal;
    ID3D11BlendState*   layer_blend_screen;
    ID3D11BlendState*   layer_blend_replace;
    ID3D11BlendState*   layer_blend_multiply;
    ID3D11SamplerState* layer_point_sampler;
    // one CPU-readable staging texture for read_region, grown to the largest region
    ID3D11Texture2D*    readback_staging;
    int                 readback_w;
    int                 readback_h;

    // render-to-layer pool (renderer_backend__begin_layer): a few render
    // targets reused by size, the one being drawn, and the frame state to
    // restore at end_layer
    struct
    {
        ID3D11Texture2D*          texture;
        ID3D11RenderTargetView*   rtv;
        ID3D11ShaderResourceView* srv;
        int                       w;
        int                       h;
        uint64                    used;
    } layer_targets[4];
    int                 layer_active; // slot + 1 while drawing a layer, else 0
    int64               layer_saved_viewport_w;
    int64               layer_saved_viewport_h;
    int64               layer_saved_scissor_depth;
    gui_rect_t layer_saved_scissors[32];
    uint64              layer_counter;
} _d3d11_renderer_internal__state_t;

static _d3d11_renderer_internal__state_t _d3d11_renderer_internal__g;
static boole _d3d11_renderer_internal__present_paced;

// ============================================================================
// FORWARD DECLS
// ============================================================================

static boole    _d3d11_renderer_internal__create_device_and_swapchain(HWND hwnd);
static boole    _d3d11_renderer_internal__create_rtv_from_backbuffer(void);
static boole    _d3d11_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob);
static boole    _d3d11_renderer_internal__build_pipeline(void);
static boole    _d3d11_renderer_internal__build_text_pipeline(void);
static boole    _d3d11_renderer_internal__handle_resize(int64 new_w, int64 new_h);
static void     _d3d11_renderer_internal__upload_constant_buffer(void);
static void     _d3d11_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c);
static void     _d3d11_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color_t c);
static void     _d3d11_renderer_internal__flush_batches(void);
// renderer_backend__submit_tiles statistics, logged at shutdown
static uint64 _d3d11_renderer_internal__tile_vertices = 0;
static uint64 _d3d11_renderer_internal__tile_flushes = 0;
static void _d3d11_renderer_internal__release_blur(void);
static boole    _d3d11_renderer_internal__ensure_layer_pipeline(void);
static boole    _d3d11_renderer_internal__layer_blend_state(D3D11_BLEND source_color, D3D11_BLEND destination_color, D3D11_BLEND source_alpha, D3D11_BLEND destination_alpha, ID3D11BlendState** out);
static void     _d3d11_renderer_internal__capture_if_requested(void);
static void     _d3d11_renderer_internal__apply_scissor_top(void);

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
    _d3d11_renderer_internal__present_paced = FALSE;
    stdlib__memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));

    _d3d11_renderer_internal__g.hwnd = (HWND)native_window;
    if (_d3d11_renderer_internal__g.hwnd == NULL_POINTER)
    {
        log_error("%s", "renderer__init got a NULL window handle");
        return FALSE;
    }

    // look up the initial client-area size from the window. d3d needs to
    // know it up front to size the back buffer. resize is handled later.
    RECT rc;
    GetClientRect(_d3d11_renderer_internal__g.hwnd, &rc);
    _d3d11_renderer_internal__g.viewport_w = rc.right - rc.left;
    _d3d11_renderer_internal__g.viewport_h = rc.bottom - rc.top;
    if (_d3d11_renderer_internal__g.viewport_w <= 0) _d3d11_renderer_internal__g.viewport_w = 1;
    if (_d3d11_renderer_internal__g.viewport_h <= 0) _d3d11_renderer_internal__g.viewport_h = 1;

    if (_d3d11_renderer_internal__create_device_and_swapchain(_d3d11_renderer_internal__g.hwnd) == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__create_rtv_from_backbuffer() == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__build_pipeline() == FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// PUBLIC: renderer__init_adopt
// ============================================================================

/**
 * @brief initializes the renderer against a pre-existing caller-owned d3d11 device and swap chain
 *
 * @param ID3D11Device* device -> caller-owned d3d11 device; must outlive the renderer
 * @param ID3D11DeviceContext* context -> caller-owned immediate context; must outlive the renderer
 * @param IDXGISwapChain* swapchain -> caller-owned swap chain; must outlive the renderer
 *
 * @attention used by hosts where another renderer owns presentation (Lemonade UWP: Win2D owns the swap chain and Presents once per composited frame); the adopter keeps its references, so shutdown releases none of the three objects
 *
 * @return boole -> TRUE on success; on failure the renderer state is left zeroed and renderer__shutdown remains safe to call
 */
boole renderer__init_adopt(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain)
{
    DXGI_SWAP_CHAIN_DESC desc;
    HRESULT hr = 0;

    stdlib__memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));

    if (device == NULL_POINTER || context == NULL_POINTER || swapchain == NULL_POINTER)
    {
        log_error("%s", "renderer__init_adopt: null device, context or swapchain");
        return FALSE;
    }

    _d3d11_renderer_internal__g.device    = device;
    _d3d11_renderer_internal__g.context   = context;
    _d3d11_renderer_internal__g.swapchain = swapchain;
    _d3d11_renderer_internal__g.adopted   = TRUE;

    // no HWND in the composition path
    _d3d11_renderer_internal__g.hwnd = NULL_POINTER;

    // pull the back-buffer dimensions from the swap chain so the viewport math matches what the adopter renders into
    stdlib__memset(&desc, 0, sizeof(desc));

    hr = IDXGISwapChain_GetDesc(swapchain, &desc);
    if (FAILED(hr))
    {
        log_error("%s %lu", "renderer__init_adopt: GetDesc failed, hresult", (unsigned long)hr);
        stdlib__memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));
        return FALSE;
    }

    _d3d11_renderer_internal__g.viewport_w = (int64)desc.BufferDesc.Width;
    _d3d11_renderer_internal__g.viewport_h = (int64)desc.BufferDesc.Height;

    if (_d3d11_renderer_internal__g.viewport_w <= 0)
    {
        _d3d11_renderer_internal__g.viewport_w = 1;
    }

    if (_d3d11_renderer_internal__g.viewport_h <= 0)
    {
        _d3d11_renderer_internal__g.viewport_h = 1;
    }

    if (_d3d11_renderer_internal__create_rtv_from_backbuffer() == FALSE)
    {
        stdlib__memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));
        return FALSE;
    }

    if (_d3d11_renderer_internal__build_pipeline() == FALSE)
    {
        stdlib__memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief drops the render target view of the back buffer so a host that owns the swap chain can resize it; the next renderer__begin_frame re-creates the view against the current back buffer
 *
 * @attention adopted mode only; the standalone renderer resizes its own swap chain inside renderer__begin_frame
 *
 * @return void
 */
void renderer__d3d11_release_render_target(void)
{
    if (_d3d11_renderer_internal__g.context == NULL_POINTER)
    {
        return;
    }

    ID3D11DeviceContext_OMSetRenderTargets(_d3d11_renderer_internal__g.context, 0, NULL_POINTER, NULL_POINTER);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.rtv);
    _d3d11_renderer_internal__g.viewport_w = 0;
    _d3d11_renderer_internal__g.viewport_h = 0;
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
    if (_d3d11_renderer_internal__tile_vertices > 0)
    {
        log_info("%s %llu %s %llu", "tiles: vertices", (unsigned long long)_d3d11_renderer_internal__tile_vertices, "flushes", (unsigned long long)_d3d11_renderer_internal__tile_flushes);
    }
    // text pipeline. atlas textures/SRVs are owned by font.c (destroyed
    // via renderer__destroy_atlas before we get here).
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_sampler);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_vbo);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_ps);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.text_vs);

    // Image pipeline. texture SRVs are owned by callers
    // (widget_image via user_data, image cache globally); caller
    // releases via renderer__destroy_texture.
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_vbo);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_ps);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.image_vs);

    // Layer pipeline. Layer textures are owned by the compositor's cache and
    // released through renderer__destroy_texture before this.
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.readback_staging);
    _d3d11_renderer_internal__g.readback_w = 0;
    _d3d11_renderer_internal__g.readback_h = 0;
    for (int slot = 0; slot < 4; slot++)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].srv);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].rtv);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].texture);
    }
    _d3d11_renderer_internal__release_blur();
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_point_sampler);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_blend_multiply);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_blend_replace);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_blend_screen);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_blend_normal);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_vbo);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_ps);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_vs);
    // current_text_atlas is a borrowed pointer (not ours to release).
    _d3d11_renderer_internal__g.current_text_atlas = NULL_POINTER;

    // solid-rect pipeline.
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.rasterizer_state);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.blend_state);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.cbuffer);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.vbo);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.ps);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.vs);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.rtv);

    // in adoption mode device/context/swapchain are borrowed; the adopter still holds and releases the original references, releasing here would underrun their refcount
    if (_d3d11_renderer_internal__g.adopted == FALSE)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.swapchain);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.context);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.device);
    }
    else
    {
        _d3d11_renderer_internal__g.swapchain = NULL_POINTER;
        _d3d11_renderer_internal__g.context   = NULL_POINTER;
        _d3d11_renderer_internal__g.device    = NULL_POINTER;
    }

    if (_d3d11_renderer_internal__g.cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_d3d11_renderer_internal__g.cpu_verts);
    }
    if (_d3d11_renderer_internal__g.text_cpu_verts != NULL_POINTER)
    {
        GUI_FREE(_d3d11_renderer_internal__g.text_cpu_verts);
    }
    stdlib__memset(&_d3d11_renderer_internal__g, 0, sizeof(_d3d11_renderer_internal__g));
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
    // handle window resize. if the platform says the viewport is now a
    // different size than our swapchain was created for, we have to
    // ResizeBuffers the swapchain and recreate the RTV against the new
    // back buffer.
    if (viewport_w != _d3d11_renderer_internal__g.viewport_w || viewport_h != _d3d11_renderer_internal__g.viewport_h)
    {
        _d3d11_renderer_internal__handle_resize(viewport_w, viewport_h);
    }

    _d3d11_renderer_internal__g.vert_count      = 0;
    _d3d11_renderer_internal__g.text_vert_count = 0;
    _d3d11_renderer_internal__g.text_run_count  = 0;
    // current_text_atlas is intentionally preserved across frames; the
    // first font__draw of each frame rebinds it anyway. runs reset per
    // frame (each run belongs to the frame that produced it).

    // bind the render target, clear it. OMSetRenderTargets sets where
    // pixel-shader output lands; ClearRenderTargetView fills that
    // target with the clear color.
    ID3D11DeviceContext_OMSetRenderTargets(_d3d11_renderer_internal__g.context, 1, &_d3d11_renderer_internal__g.rtv, NULL_POINTER);

    // in adoption mode the back buffer already holds the adopter's draw (Win2D); skip the clear and composite on top
    if (_d3d11_renderer_internal__g.adopted == FALSE)
    {
        FLOAT clear_rgba[4] = { clear.r, clear.g, clear.b, clear.a };
        ID3D11DeviceContext_ClearRenderTargetView(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rtv, clear_rgba);
    }

    // set the viewport (the area of the RTV we'll draw into). d3d11
    // viewports use top-left origin with y-down, same as our pixel coords.
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = (FLOAT)viewport_w;
    vp.Height   = (FLOAT)viewport_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(_d3d11_renderer_internal__g.context, 1, &vp);

    // Initial scissor: full viewport. The rasterizer state has
    // ScissorEnable=TRUE permanently (so we don't have to swap states
    // on every push/pop), which means there ALWAYS needs to be a sane
    // scissor rect bound. Each push/pop updates it; here we set the
    // no-op (full viewport) starting state so the first batch isn't
    // accidentally clipped to nothing.
    _d3d11_renderer_internal__apply_scissor_top();
    // Clipping/images may flush before end_frame, including the first frame.
    _d3d11_renderer_internal__upload_constant_buffer();
}

// ============================================================================
// PUBLIC: renderer__submit_rect
// ============================================================================
// identical cpu-side logic to opengl3_renderer. vertex layout + clamp +
// triangle winding are all part of the VISUAL CONTRACT.

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
    if (_d3d11_renderer_internal__g.vert_count + _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD > _d3d11_renderer_internal__g.vert_cap)
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

    _d3d11_renderer_internal__push_vertex(x0, y0,  0.0f, 0.0f, r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x1, y0,  r.w,  0.0f, r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x1, y1,  r.w,  r.h,  r.w, r.h, radius, c);
    _d3d11_renderer_internal__push_vertex(x0, y1,  0.0f, r.h,  r.w, r.h, radius, c);
}

// ============================================================================
// PUBLIC: renderer__end_frame
// ============================================================================
// 1. upload this frame's vertices to the dynamic vertex buffer.
// 2. upload viewport size to the constant buffer.
// 3. set the pipeline state (shaders, input layout, VB, CB, blend, topology).
// 4. Draw.
// 5. Present.
// Map uses D3D11_MAP_WRITE_DISCARD which tells the driver "throw away the
// old contents of this buffer, give me fresh gpu memory to write into". the
// driver can reuse the old memory or allocate new -- either way, it
// avoids a GPU/CPU sync and is the standard pattern for per-frame buffers.

// ============================================================================
// INTERNAL: flush_batches
// ============================================================================
// Draw whatever is currently buffered (solid rects + text glyphs), then
// reset the buffer counts so subsequent submissions start a fresh batch.
// Called from end_frame and from push/pop_scissor (any state change that
// can't be merged into a single Draw forces a flush).

static void _d3d11_renderer_internal__flush_batches(void)
{
    // PASS 1: solid rects.
    if (_d3d11_renderer_internal__g.vert_count > 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.vbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            size_t bytes = (size_t)_d3d11_renderer_internal__g.vert_count * sizeof(_d3d11_renderer_internal__vertex_t);
            stdlib__memcpy(mapped.pData, _d3d11_renderer_internal__g.cpu_verts, bytes);
            ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.vbo, 0);
        }

        ID3D11DeviceContext_IASetPrimitiveTopology(_d3d11_renderer_internal__g.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.input_layout);

        UINT stride = (UINT)sizeof(_d3d11_renderer_internal__vertex_t);
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.vbo, &stride, &offset);

        ID3D11DeviceContext_VSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.vs, NULL_POINTER, 0);
        ID3D11DeviceContext_PSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.ps, NULL_POINTER, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.cbuffer);

        FLOAT blend_factor[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_OMSetBlendState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.blend_state, blend_factor, 0xFFFFFFFF);
        ID3D11DeviceContext_RSSetState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rasterizer_state);

        ID3D11DeviceContext_Draw(_d3d11_renderer_internal__g.context, (UINT)_d3d11_renderer_internal__g.vert_count, 0);

        _d3d11_renderer_internal__g.vert_count = 0;
    }

    // PASS 2: text. one Draw per atlas-run (multi-font support).
    if (_d3d11_renderer_internal__g.text_vert_count > 0 && _d3d11_renderer_internal__g.text_run_count > 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.text_vbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            size_t bytes = (size_t)_d3d11_renderer_internal__g.text_vert_count * sizeof(_d3d11_renderer_internal__text_vertex_t);
            stdlib__memcpy(mapped.pData, _d3d11_renderer_internal__g.text_cpu_verts, bytes);
            ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.text_vbo, 0);
        }

        ID3D11DeviceContext_IASetPrimitiveTopology(_d3d11_renderer_internal__g.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetInputLayout(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.text_input_layout);

        UINT tstride = (UINT)sizeof(_d3d11_renderer_internal__text_vertex_t);
        UINT toffset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.text_vbo, &tstride, &toffset);

        ID3D11DeviceContext_VSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.text_vs, NULL_POINTER, 0);
        ID3D11DeviceContext_PSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.text_ps, NULL_POINTER, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.cbuffer);
        ID3D11DeviceContext_PSSetSamplers(_d3d11_renderer_internal__g.context, 0, 1, &_d3d11_renderer_internal__g.text_sampler);

        FLOAT blend_factor[4] = { 0, 0, 0, 0 };
        ID3D11DeviceContext_OMSetBlendState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.blend_state, blend_factor, 0xFFFFFFFF);
        ID3D11DeviceContext_RSSetState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rasterizer_state);

        for (int64 i = 0; i < _d3d11_renderer_internal__g.text_run_count; i++)
        {
            _d3d11_renderer_internal__text_run_t* run = &_d3d11_renderer_internal__g.text_runs[i];
            if (run->vert_count <= 0 || run->atlas == NULL_POINTER)
            {
                continue;
            }
            ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &run->atlas);
            ID3D11DeviceContext_Draw(_d3d11_renderer_internal__g.context, (UINT)run->vert_count, (UINT)run->vert_start);
        }

        ID3D11ShaderResourceView* null_srv = NULL_POINTER;
        ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &null_srv);

        _d3d11_renderer_internal__g.text_vert_count    = 0;
        _d3d11_renderer_internal__g.text_run_count     = 0;
        _d3d11_renderer_internal__g.current_text_atlas = NULL_POINTER;
    }
}

// ============================================================================
// PUBLIC: renderer__push_scissor / renderer__pop_scissor
// ============================================================================
// Bracket a region of submissions whose draws are clipped to the given
// rect. Each push/pop forces a flush of whatever is currently batched,
// then changes the GPU scissor rect via RSSetScissorRects. The
// rasterizer state already has ScissorEnable=TRUE (set in init), so the
// rect is always honored.
// Nesting takes the INTERSECTION of all stacked rects -- a scrollable
// child div inside a scrollable parent div correctly clips to the
// smaller of the two regions.
// When the stack becomes empty (or when no scissor was ever pushed),
// we still call RSSetScissorRects with the full viewport rect so the
// "always-on" rasterizer state has a sensible no-op clip.

static gui_rect_t _d3d11_renderer_internal__intersect(gui_rect_t a, gui_rect_t b)
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

static void _d3d11_renderer_internal__apply_scissor_top(void)
{
    D3D11_RECT sr;
    if (_d3d11_renderer_internal__g.scissor_depth <= 0)
    {
        // empty stack: scissor covers the full viewport so the
        // "always-on" rasterizer state behaves as if disabled.
        sr.left   = 0;
        sr.top    = 0;
        sr.right  = (LONG)_d3d11_renderer_internal__g.viewport_w;
        sr.bottom = (LONG)_d3d11_renderer_internal__g.viewport_h;
    }
    else
    {
        gui_rect_t r = _d3d11_renderer_internal__g.scissor_stack[0];
        for (int64 i = 1; i < _d3d11_renderer_internal__g.scissor_depth; i++)
        {
            r = _d3d11_renderer_internal__intersect(r, _d3d11_renderer_internal__g.scissor_stack[i]);
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
    ID3D11DeviceContext_RSSetScissorRects(_d3d11_renderer_internal__g.context, 1, &sr);
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
    _d3d11_renderer_internal__flush_batches();
    if (_d3d11_renderer_internal__g.scissor_depth >= (int64)(sizeof(_d3d11_renderer_internal__g.scissor_stack) / sizeof(_d3d11_renderer_internal__g.scissor_stack[0])))
    {
        log_warn("%s", "renderer__push_scissor: stack overflow, ignoring push");
        return;
    }
    _d3d11_renderer_internal__g.scissor_stack[_d3d11_renderer_internal__g.scissor_depth++] = rect;
    _d3d11_renderer_internal__apply_scissor_top();
}

/**
 * @brief pop the most recent scissor rect off the stack. If the stack is now empty, scissor testing is disabled entirely (everything draws).
 *
 * @return void
 */
void renderer__pop_scissor(void)
{
    _d3d11_renderer_internal__flush_batches();
    if (_d3d11_renderer_internal__g.scissor_depth <= 0)
    {
        log_warn("%s", "renderer__pop_scissor: stack already empty");
        return;
    }
    _d3d11_renderer_internal__g.scissor_depth--;
    _d3d11_renderer_internal__apply_scissor_top();
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
    // submitted before this blur call (otherwise we'd copy stale
    // pixels). Same semantic as the GL flush_batches before
    // glReadPixels.
    _d3d11_renderer_internal__flush_batches();

    int vw = (int)_d3d11_renderer_internal__g.viewport_w;
    int vh = (int)_d3d11_renderer_internal__g.viewport_h;

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

    // Pull the backbuffer as a source texture. D3D11 has no
    // glReadPixels; readback is staging-buffer + CopySubresourceRegion
    // + Map. The staging texture is CPU-readable, USAGE_STAGING.
    // Format = the swapchain's RGBA8 (B8G8R8A8 in DXGI ordering, which
    // we swizzle on upload below). Lifecycle: created here, released
    // before this call returns.
    ID3D11Texture2D* backbuffer = NULL_POINTER;
    HRESULT hr = IDXGISwapChain_GetBuffer( _d3d11_renderer_internal__g.swapchain, 0, &IID_ID3D11Texture2D, (void**)&backbuffer);
    if (FAILED(hr) || backbuffer == NULL_POINTER)
    {
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    D3D11_TEXTURE2D_DESC sd = { 0 };
    sd.Width              = (UINT)w;
    sd.Height             = (UINT)h;
    sd.MipLevels          = 1;
    sd.ArraySize          = 1;
    // Match the swapchain's format (R8G8B8A8_UNORM, set in
    // create_device_and_swapchain). CopySubresourceRegion requires
    // identical formats between source + dest; using B8G8R8A8 here
    // would silently produce all-zero pixels in the staging map.
    sd.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage              = D3D11_USAGE_STAGING;
    sd.BindFlags          = 0;
    sd.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags          = 0;

    ID3D11Texture2D* staging = NULL_POINTER;
    hr = ID3D11Device_CreateTexture2D( _d3d11_renderer_internal__g.device, &sd, NULL_POINTER, &staging);
    if (FAILED(hr) || staging == NULL_POINTER)
    {
        ID3D11Texture2D_Release(backbuffer);
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    D3D11_BOX box;
    box.left   = (UINT)x;
    box.top    = (UINT)y;
    box.front  = 0;
    box.right  = (UINT)(x + w);
    box.bottom = (UINT)(y + h);
    box.back   = 1;
    ID3D11DeviceContext_CopySubresourceRegion( _d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0, 0, 0, 0, (ID3D11Resource*)backbuffer, 0, &box);

    D3D11_MAPPED_SUBRESOURCE m = { 0 };
    hr = ID3D11DeviceContext_Map( _d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr) || m.pData == NULL_POINTER)
    {
        ID3D11Texture2D_Release(staging);
        ID3D11Texture2D_Release(backbuffer);
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
        ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0);
        ID3D11Texture2D_Release(staging);
        ID3D11Texture2D_Release(backbuffer);
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    // Copy row-by-row honouring the staging texture's RowPitch
    // (D3D11 may pad rows to 256-byte alignment so we can't assume
    // tightly packed). Format is RGBA8 (matches swapchain), so no
    // channel swizzle is needed.
    for (int py = 0; py < h; py++)
    {
        ubyte* src = (ubyte*)m.pData + (size_t)py * (size_t)m.RowPitch;
        ubyte* dst = pixels          + (size_t)py * (size_t)w * 4;
        stdlib__memcpy(dst, src, (size_t)w * 4);
    }
    ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0);
    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(backbuffer);

    // D3D11 has top-left origin, so no row flip needed.
    blur_cpu__apply_gaussian(pixels, tmp, w, h, sigma_px);

    void* tex = renderer__create_texture_rgba(pixels, w, h);

    gui_rect_t draw_rect;
    draw_rect.x = (float)x;
    draw_rect.y = (float)y;
    draw_rect.w = (float)w;
    draw_rect.h = (float)h;
    gui_color_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
    renderer__submit_image(draw_rect, tex, white);
    _d3d11_renderer_internal__flush_batches();
    renderer__destroy_texture(tex);

    GUI_FREE(tmp);
    GUI_FREE(pixels);
}

// ============================================================================
// PUBLIC: renderer__flush_pending_draws / renderer__end_frame
// ============================================================================

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
    _d3d11_renderer_internal__flush_batches();
}

/**
 * @brief finalize the frame: upload the batched verts, issue the draw, then present the back buffer to the window (swap buffers / swapchain Present). After this call returns, the frame is on screen.
 *
 * @return void
 */
void renderer__end_frame(void)
{
    // flush any remaining batched submissions.
    _d3d11_renderer_internal__flush_batches();

    // defensive scissor reset: if a widget pushed but failed to pop,
    // reset to the full viewport so the next frame starts clean.
    if (_d3d11_renderer_internal__g.scissor_depth != 0)
    {
        log_warn("%s %lld %s", "renderer__end_frame: scissor stack non-empty (", (long long)_d3d11_renderer_internal__g.scissor_depth, "); resetting");
        _d3d11_renderer_internal__g.scissor_depth = 0;
        _d3d11_renderer_internal__apply_scissor_top();
    }

    _d3d11_renderer_internal__capture_if_requested();

    // present. 1 = vsync (wait for next vblank). use 0 to uncap the
    // frame rate if you want unlimited fps.
    // in adoption mode the adopter owns the swap chain and Presents once for the whole composited frame
    if (_d3d11_renderer_internal__g.adopted == FALSE)
    {
        EFFECTS_PROFILE_BEGIN(present_profile);
        HRESULT result = IDXGISwapChain_Present(_d3d11_renderer_internal__g.swapchain, 1, 0);
        // Occluded/lost surfaces may return immediately without waiting for
        // vblank. Let the platform provide pacing until presentation resumes.
        _d3d11_renderer_internal__present_paced = (boole)(result == S_OK);
        EFFECTS_PROFILE_STEP(present_profile, EFFECTS_PROFILE_PRESENT);
    }
}

// ===== frame capture state (see renderer.h) ================================
static ubyte* _d3d11_renderer_internal__capture_out  = NULL_POINTER;
static int64  _d3d11_renderer_internal__capture_cap  = 0;
static boole  _d3d11_renderer_internal__capture_done = FALSE;

// The armed frame capture: the back buffer copied to a staging texture
// and mapped, rows copied out honoring the row pitch. Same mechanics as
// renderer__blur_region's readback, for the whole buffer.
static void _d3d11_renderer_internal__capture_if_requested(void)
{
    int64 w = (int64)_d3d11_renderer_internal__g.viewport_w;
    int64 h = (int64)_d3d11_renderer_internal__g.viewport_h;
    ID3D11Texture2D* backbuffer = NULL_POINTER;
    ID3D11Texture2D* staging = NULL_POINTER;
    D3D11_TEXTURE2D_DESC sd;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = 0;
    int64 y = 0;

    if (_d3d11_renderer_internal__capture_out == NULL_POINTER || w <= 0 || h <= 0 || _d3d11_renderer_internal__capture_cap < w * h * 4)
    {
        return;
    }

    hr = IDXGISwapChain_GetBuffer(_d3d11_renderer_internal__g.swapchain, 0, &IID_ID3D11Texture2D, (void**)&backbuffer);

    if (FAILED(hr) || backbuffer == NULL_POINTER)
    {
        _d3d11_renderer_internal__capture_out = NULL_POINTER;
        return;
    }

    ID3D11Texture2D_GetDesc(backbuffer, &sd);
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;

    hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &sd, NULL_POINTER, &staging);

    if (FAILED(hr) || staging == NULL_POINTER)
    {
        ID3D11Texture2D_Release(backbuffer);
        _d3d11_renderer_internal__capture_out = NULL_POINTER;
        return;
    }

    ID3D11DeviceContext_CopyResource(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, (ID3D11Resource*)backbuffer);
    stdlib__memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &mapped);

    if (SUCCEEDED(hr))
    {
        for (y = 0; y < h && y < (int64)sd.Height; y++)
        {
            stdlib__memcpy(_d3d11_renderer_internal__capture_out + (size_t)y * (size_t)w * 4, (const ubyte*)mapped.pData + (size_t)y * (size_t)mapped.RowPitch, (size_t)w * 4);
        }

        ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0);
        _d3d11_renderer_internal__capture_done = TRUE;
    }

    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(backbuffer);
    _d3d11_renderer_internal__capture_out = NULL_POINTER;
}

// ============================================================================
// INTERNAL: device + swapchain creation
// ============================================================================

static boole _d3d11_renderer_internal__create_device_and_swapchain(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC scd;
    stdlib__memset(&scd, 0, sizeof(scd));
    scd.BufferDesc.Width                   = (UINT)_d3d11_renderer_internal__g.viewport_w;
    scd.BufferDesc.Height                  = (UINT)_d3d11_renderer_internal__g.viewport_h;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count                   = 1;
    scd.SampleDesc.Quality                 = 0;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount                        = 2;
    scd.OutputWindow                       = hwnd;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags                              = 0;

    // feature level array: ask for 11.0 first, fall back to 10.x if the
    // driver says "not supported". D3D11CreateDeviceAndSwapChain tries
    // them in order and returns the highest it could create.
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT flags = 0;
    // in a dev build we'd also OR D3D11_CREATE_DEVICE_DEBUG so that the
    // d3d11 debug layer prints validation messages to the debugger. that
    // requires the Windows SDK graphics tools installed on the machine.

    D3D_FEATURE_LEVEL got_fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain( NULL_POINTER, /* adapter: default */ D3D_DRIVER_TYPE_HARDWARE, NULL_POINTER, /* software rasterizer: not used */ flags, feature_levels, (UINT)(sizeof(feature_levels) / sizeof(feature_levels[0])), D3D11_SDK_VERSION, &scd, &_d3d11_renderer_internal__g.swapchain, &_d3d11_renderer_internal__g.device, &got_fl, &_d3d11_renderer_internal__g.context );
    if (FAILED(hr))
    {
        // Windows versions/drivers without flip-discard retain the original
        // presentation path. Adopted devices never enter this function.
        if (_d3d11_renderer_internal__g.context) ID3D11DeviceContext_Release(_d3d11_renderer_internal__g.context);
        if (_d3d11_renderer_internal__g.device) ID3D11Device_Release(_d3d11_renderer_internal__g.device);
        if (_d3d11_renderer_internal__g.swapchain) IDXGISwapChain_Release(_d3d11_renderer_internal__g.swapchain);
        _d3d11_renderer_internal__g.context = NULL_POINTER;
        _d3d11_renderer_internal__g.device = NULL_POINTER;
        _d3d11_renderer_internal__g.swapchain = NULL_POINTER;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(NULL_POINTER, D3D_DRIVER_TYPE_HARDWARE, NULL_POINTER, flags,
            feature_levels, (UINT)(sizeof(feature_levels) / sizeof(feature_levels[0])), D3D11_SDK_VERSION,
            &scd, &_d3d11_renderer_internal__g.swapchain, &_d3d11_renderer_internal__g.device, &got_fl,
            &_d3d11_renderer_internal__g.context);
    }
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "D3D11CreateDeviceAndSwapChain failed (HRESULT 0x", (unsigned long)hr, ")");
        return FALSE;
    }

    log_info("D3D11 presentation: %s", scd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip-discard" : "legacy discard");
    return TRUE;
}

// get the current back buffer from the swapchain, wrap it in a
// render-target view, and release our local ref on the texture (the
// RTV holds its own). called once at init and again after every resize.
static boole _d3d11_renderer_internal__create_rtv_from_backbuffer(void)
{
    ID3D11Texture2D* back_buffer = NULL_POINTER;
    HRESULT hr = IDXGISwapChain_GetBuffer(_d3d11_renderer_internal__g.swapchain, 0, &IID_ID3D11Texture2D, (void**)&back_buffer);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "IDXGISwapChain::GetBuffer failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    hr = ID3D11Device_CreateRenderTargetView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)back_buffer, NULL_POINTER, &_d3d11_renderer_internal__g.rtv);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(back_buffer);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateRenderTargetView failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }
    return TRUE;
}

// ============================================================================
// INTERNAL: shader compilation
// ============================================================================
// compile an HLSL source string to bytecode via D3DCompile. `target` is
// the shader profile: "vs_5_0" for vertex shaders, "ps_5_0" for pixel
// shaders (the "5_0" is the shader model, available on feature level 11).
// errors come back in a separate ID3DBlob whose contents are a printable
// compile log. we log it and return FALSE.

static boole _d3d11_renderer_internal__compile_shader(char* src, char* entry, char* target, ID3DBlob** out_blob)
{
    ID3DBlob* err_blob = NULL_POINTER;
    HRESULT   hr       = D3DCompile( src, stdlib__strlen(src), NULL_POINTER, NULL_POINTER, NULL_POINTER, entry, target, 0, 0, out_blob, &err_blob );
    if (FAILED(hr))
    {
        if (err_blob != NULL_POINTER)
        {
            log_error("%s %s %s %s", "shader compile error (", entry, "):\n", (char*)ID3D10Blob_GetBufferPointer(err_blob));
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
        }
        else
        {
            log_error("%s %08lX %s", "D3DCompile failed (0x", (unsigned long)hr, ")");
        }
        return FALSE;
    }
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(err_blob);
    return TRUE;
}

// ============================================================================
// INTERNAL: pipeline setup
// ============================================================================

static boole _d3d11_renderer_internal__build_pipeline(void)
{
    // compile the two shader stages.
    ID3DBlob* vs_blob = NULL_POINTER;
    ID3DBlob* ps_blob = NULL_POINTER;

    if (_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__vs_src, "vs_main", "vs_5_0", &vs_blob) == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__ps_src, "ps_main", "ps_5_0", &ps_blob) == FALSE)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    // create shader objects from the bytecode blobs.
    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL_POINTER, &_d3d11_renderer_internal__g.vs);
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("%s %08lX %s", "CreateVertexShader failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }
    hr = ID3D11Device_CreatePixelShader( _d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL_POINTER, &_d3d11_renderer_internal__g.ps );
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("%s %08lX %s", "CreatePixelShader failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // input layout: the d3d11 equivalent of opengl's VAO attribute pointers.
    // semantics (POSITION, COLOR, TEXCOORD0/1/2) must match the shader's
    // input struct (VSIn). AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT
    // lets d3d compute the stride automatically, but we spell out the
    // offsets for clarity since they have to match our C struct layout.
    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = ID3D11Device_CreateInputLayout(_d3d11_renderer_internal__g.device, layout_desc, (UINT)(sizeof(layout_desc) / sizeof(layout_desc[0])), ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &_d3d11_renderer_internal__g.input_layout);

    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateInputLayout failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // dynamic vertex buffer, sized for the max batch.
    D3D11_BUFFER_DESC vbd;
    stdlib__memset(&vbd, 0, sizeof(vbd));
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth      = (UINT)(_D3D11_RENDERER_INTERNAL__MAX_QUADS* _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d11_renderer_internal__vertex_t));
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &vbd, NULL_POINTER, &_d3d11_renderer_internal__g.vbo);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateBuffer (vbo) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // constant buffer for the globals struct.
    D3D11_BUFFER_DESC cbd;
    stdlib__memset(&cbd, 0, sizeof(cbd));
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth      = (UINT)sizeof(_d3d11_renderer_internal__cb_globals_t);
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &cbd, NULL_POINTER, &_d3d11_renderer_internal__g.cbuffer);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateBuffer (cbuffer) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // blend state matching opengl's SRC_ALPHA/INV_SRC_ALPHA (= the
    // "classic" alpha blend). this is part of the VISUAL CONTRACT.
    D3D11_BLEND_DESC bd;
    stdlib__memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = ID3D11Device_CreateBlendState(_d3d11_renderer_internal__g.device, &bd, &_d3d11_renderer_internal__g.blend_state);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateBlendState failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // Rasterizer state. Two non-default knobs:
    // CullMode = NONE         the toolkit's quads are 2D (no winding
    // convention enforced); disable culling
    // so triangles draw regardless of order.
    // ScissorEnable = TRUE    enable the GPU scissor test always; the
    // scissor RECT is updated as widgets push/
    // pop. When the scissor stack is empty
    // we set the rect to cover the full
    // viewport, so it acts as a no-op clip.
    D3D11_RASTERIZER_DESC rd;
    stdlib__memset(&rd, 0, sizeof(rd));
    rd.FillMode              = D3D11_FILL_SOLID;
    rd.CullMode              = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable       = TRUE;
    rd.ScissorEnable         = TRUE;
    rd.MultisampleEnable     = FALSE;
    rd.AntialiasedLineEnable = FALSE;

    hr = ID3D11Device_CreateRasterizerState(_d3d11_renderer_internal__g.device, &rd, &_d3d11_renderer_internal__g.rasterizer_state);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateRasterizerState failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // cpu-side vertex staging buffer.
    _d3d11_renderer_internal__g.vert_cap  = _D3D11_RENDERER_INTERNAL__MAX_QUADS * _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d11_renderer_internal__g.cpu_verts = (_d3d11_renderer_internal__vertex_t*)GUI_MALLOC_T((size_t)_d3d11_renderer_internal__g.vert_cap * sizeof(_d3d11_renderer_internal__vertex_t), MM_TYPE__RENDERER);
    if (_d3d11_renderer_internal__g.cpu_verts == NULL_POINTER)
    {
        log_error("%s", "out of memory for cpu vertex buffer");
        return FALSE;
    }

    // text pipeline: separate shaders + input layout + VBO + sampler.
    if (_d3d11_renderer_internal__build_text_pipeline() == FALSE)
    {
        return FALSE;
    }

    return TRUE;
}

// build the text-specific half of the D3D11 pipeline: text VS/PS compile,
// text input layout, dynamic text vertex buffer, and a LINEAR/CLAMP
// sampler state bound at draw time.
static boole _d3d11_renderer_internal__build_text_pipeline(void)
{
    ID3DBlob* vs_blob = NULL_POINTER;
    ID3DBlob* ps_blob = NULL_POINTER;

    if (_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__text_vs_src, "vs_main", "vs_5_0", &vs_blob) == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__text_ps_src, "ps_main", "ps_5_0", &ps_blob) == FALSE)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL_POINTER, &_d3d11_renderer_internal__g.text_vs);
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("%s %08lX %s", "CreateVertexShader (text) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }
    hr = ID3D11Device_CreatePixelShader( _d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL_POINTER, &_d3d11_renderer_internal__g.text_ps );
    if (FAILED(hr))
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        log_error("%s %08lX %s", "CreatePixelShader (text) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // text input layout: 3 attributes, matching the text_vertex struct.
    // POSITION @ 0    (float2)
    // COLOR    @ 8    (float4)
    // TEXCOORD0 @ 24  (float2)
    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = ID3D11Device_CreateInputLayout(_d3d11_renderer_internal__g.device, layout_desc, (UINT)(sizeof(layout_desc) / sizeof(layout_desc[0])), ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &_d3d11_renderer_internal__g.text_input_layout);

    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);

    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateInputLayout (text) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // dynamic text vertex buffer. sized for MAX_TEXT_GLYPHS quads.
    D3D11_BUFFER_DESC vbd;
    stdlib__memset(&vbd, 0, sizeof(vbd));
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth      = (UINT)(_D3D11_RENDERER_INTERNAL__MAX_TEXT_GLYPHS* _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD * sizeof(_d3d11_renderer_internal__text_vertex_t));
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &vbd, NULL_POINTER, &_d3d11_renderer_internal__g.text_vbo);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateBuffer (text vbo) failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // sampler: LINEAR filter + CLAMP address on both axes. mirrors the
    // GL backend's glTexParameteri settings, so the visual output
    // matches across backends.
    D3D11_SAMPLER_DESC sd;
    stdlib__memset(&sd, 0, sizeof(sd));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0.0f;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = ID3D11Device_CreateSamplerState(_d3d11_renderer_internal__g.device, &sd, &_d3d11_renderer_internal__g.text_sampler);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateSamplerState failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // cpu-side text staging buffer.
    _d3d11_renderer_internal__g.text_vert_cap  = _D3D11_RENDERER_INTERNAL__MAX_TEXT_GLYPHS * _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD;
    _d3d11_renderer_internal__g.text_cpu_verts = (_d3d11_renderer_internal__text_vertex_t*)GUI_MALLOC_T((size_t)_d3d11_renderer_internal__g.text_vert_cap * sizeof(_d3d11_renderer_internal__text_vertex_t), MM_TYPE__RENDERER);
    if (_d3d11_renderer_internal__g.text_cpu_verts == NULL_POINTER)
    {
        log_error("%s", "out of memory for cpu text vertex buffer");
        return FALSE;
    }

    return TRUE;
}

// ============================================================================
// INTERNAL: resize
// ============================================================================
// when the window changes size, the swapchain's back buffer is still
// the old size. we need to:
// 1. release our RTV (it holds a ref to the old back buffer).
// 2. call IDXGISwapChain::ResizeBuffers to reallocate the back buffer.
// 3. get the new back buffer and create a fresh RTV against it.

static boole _d3d11_renderer_internal__handle_resize(int64 new_w, int64 new_h)
{
    if (new_w <= 0) new_w = 1;
    if (new_h <= 0) new_h = 1;

    _d3d11_renderer_internal__g.viewport_w = new_w;
    _d3d11_renderer_internal__g.viewport_h = new_h;

    // unbind and release the old RTV before ResizeBuffers (d3d11
    // requires no outstanding references to the back buffer).
    ID3D11DeviceContext_OMSetRenderTargets(_d3d11_renderer_internal__g.context, 0, NULL_POINTER, NULL_POINTER);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.rtv);

    // BufferCount=0 means "keep the same as before", Format=UNKNOWN
    // means "keep the same format". passing 0 for Flags = no changes.
    // in adoption mode the swap chain is owned externally and the adopter has already resized its buffers; only the RTV is rebuilt against the now-current back buffer
    if (_d3d11_renderer_internal__g.adopted == FALSE)
    {
        HRESULT hr = IDXGISwapChain_ResizeBuffers(_d3d11_renderer_internal__g.swapchain, 0, (UINT)new_w, (UINT)new_h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr))
        {
            log_error("%s %lu", "IDXGISwapChain::ResizeBuffers failed, hresult", (unsigned long)hr);
            return FALSE;
        }
    }

    return _d3d11_renderer_internal__create_rtv_from_backbuffer();
}

// ============================================================================
// INTERNAL: constant buffer upload
// ============================================================================

static void _d3d11_renderer_internal__upload_constant_buffer(void)
{
    _d3d11_renderer_internal__cb_globals_t cb;
    cb.viewport_w = (float)_d3d11_renderer_internal__g.viewport_w;
    cb.viewport_h = (float)_d3d11_renderer_internal__g.viewport_h;
    cb._padding_0 = 0.0f;
    cb._padding_1 = 0.0f;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        stdlib__memcpy(mapped.pData, &cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.cbuffer, 0);
    }
}

// ============================================================================
// INTERNAL: vertex append
// ============================================================================

static void _d3d11_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c)
{
    _d3d11_renderer_internal__vertex_t* v = &_d3d11_renderer_internal__g.cpu_verts[_d3d11_renderer_internal__g.vert_count++];
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

static void _d3d11_renderer_internal__push_text_vertex(float x, float y, float u, float v, gui_color_t c)
{
    _d3d11_renderer_internal__text_vertex_t* tv = &_d3d11_renderer_internal__g.text_cpu_verts[_d3d11_renderer_internal__g.text_vert_count++];
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
// font.c rasterizes the atlas into an R8 bitmap and hands us the bytes.
// we wrap those in an immutable ID3D11Texture2D and an SRV, which we own
// until renderer__destroy_atlas releases them. the opaque void* handle
// we hand back to font.c is the SRV pointer -- that's what ends up bound
// to pixel-shader register t0 during the text pass. the underlying
// Texture2D is held implicitly via the SRV's internal ref, so we release
// our own ref on the texture right after creating the view.

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
    if (pixels == NULL_POINTER || width <= 0 || height <= 0 || _d3d11_renderer_internal__g.device == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    // immutable texture: we never update it after creation (if we need
    // a new atlas size we just create a new one).
    D3D11_TEXTURE2D_DESC td;
    stdlib__memset(&td, 0, sizeof(td));
    td.Width              = (UINT)width;
    td.Height             = (UINT)height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count   = 1;
    td.SampleDesc.Quality = 0;
    td.Usage              = D3D11_USAGE_IMMUTABLE;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd;
    stdlib__memset(&srd, 0, sizeof(srd));
    srd.pSysMem     = pixels;
    srd.SysMemPitch = (UINT)width; // R8 = 1 byte per pixel, so pitch = width.

    ID3D11Texture2D* tex = NULL_POINTER;
    HRESULT hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &td, &srd, &tex);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateTexture2D (atlas) failed (0x", (unsigned long)hr, ")");
        return NULL_POINTER;
    }

    // wrap the texture in a shader resource view, which is what the PS
    // samples from. NULL desc = the whole texture as a 2D SRV.
    ID3D11ShaderResourceView* srv = NULL_POINTER;
    hr = ID3D11Device_CreateShaderResourceView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)tex, NULL_POINTER, &srv);
    // release our own ref on the texture. the SRV holds the last ref;
    // when the SRV is released in renderer__destroy_atlas, the texture
    // goes with it.
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(tex);

    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "CreateShaderResourceView (atlas) failed (0x", (unsigned long)hr, ")");
        return NULL_POINTER;
    }

    return (void*)srv;
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
    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)atlas;
    // if it's the currently-bound atlas, clear our cached borrow so we
    // don't feed a dangling SRV to the next frame's text pass.
    if (_d3d11_renderer_internal__g.current_text_atlas == srv)
    {
        _d3d11_renderer_internal__g.current_text_atlas = NULL_POINTER;
    }
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(srv);
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
    _d3d11_renderer_internal__g.current_text_atlas = (ID3D11ShaderResourceView*)atlas;
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
    ID3D11ShaderResourceView* atlas = _d3d11_renderer_internal__g.current_text_atlas;
    if (atlas == NULL_POINTER)
    {
        return;
    }
    if (_d3d11_renderer_internal__g.text_vert_cap < _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    boole new_run = (boole)(_d3d11_renderer_internal__g.text_run_count == 0 ||
        _d3d11_renderer_internal__g.text_runs[_d3d11_renderer_internal__g.text_run_count - 1].atlas != atlas);
    if (_d3d11_renderer_internal__g.text_vert_count + _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD > _d3d11_renderer_internal__g.text_vert_cap ||
        (new_run && _d3d11_renderer_internal__g.text_run_count >= _D3D11_RENDERER_INTERNAL__MAX_TEXT_RUNS))
    {
        /* Preserve clipping and pending rect/text order. The drain clears
           current_text_atlas, but the caller may set it once for an entire
           string, including subsequent glyphs after this automatic flush. */
        _d3d11_renderer_internal__flush_batches();
        _d3d11_renderer_internal__g.current_text_atlas = atlas;
    }

    // open or extend a run keyed by the current atlas SRV. when the
    // atlas changes between submissions, this function starts a new
    // run so end_frame can bind the right SRV per Draw.
    _d3d11_renderer_internal__text_run_t* run = NULL_POINTER;
    if (_d3d11_renderer_internal__g.text_run_count > 0)
    {
        run = &_d3d11_renderer_internal__g.text_runs[_d3d11_renderer_internal__g.text_run_count - 1];
        if (run->atlas != atlas)
        {
            run = NULL_POINTER;
        }
    }
    if (run == NULL_POINTER)
    {
        run = &_d3d11_renderer_internal__g.text_runs[_d3d11_renderer_internal__g.text_run_count++];
        run->atlas      = atlas;
        run->vert_start = _d3d11_renderer_internal__g.text_vert_count;
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
    _d3d11_renderer_internal__push_text_vertex(x0, y0, u0, v0, color);
    _d3d11_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d11_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    _d3d11_renderer_internal__push_text_vertex(x1, y0, u1, v0, color);
    _d3d11_renderer_internal__push_text_vertex(x1, y1, u1, v1, color);
    _d3d11_renderer_internal__push_text_vertex(x0, y1, u0, v1, color);
    run->vert_count += _D3D11_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

// ============================================================================
// IMAGE PIPELINE (RGBA8 textures via D3D11)
// ============================================================================
// Mirrors the text pipeline's shape: same vertex format (pos + color
// + uv = 32 bytes), same sampler (reused from the text pipeline),
// different PS. Lazy-built on first submit_image.
// Texture creation: ID3D11Device::CreateTexture2D with
// DXGI_FORMAT_R8G8B8A8_UNORM and USAGE_IMMUTABLE. One
// D3D11_SUBRESOURCE_DATA fill with the caller's RGBA bytes at
// SysMemPitch = width*4 (no padding). Wrap an
// ID3D11ShaderResourceView around it and hand that out as the
// opaque texture handle -- the SRV keeps the Texture2D alive, so
// callers never need to know about the texture object directly.

#include "shaders/generated/image.frag.h"

static boole _d3d11_renderer_internal__ensure_image_pipeline(void)
{
    if (_d3d11_renderer_internal__g.image_ps != NULL_POINTER)
    {
        return TRUE;
    }

    // VS: reuse the text vertex shader source verbatim -- both take
    // pos + color + uv, both emit SV_POSITION via the u_viewport
    // cbuffer. No reason to duplicate the compile.
    ID3DBlob* vs_blob = NULL_POINTER;
    ID3DBlob* ps_blob = NULL_POINTER;
    if (_d3d11_renderer_internal__compile_shader(_d3d11_renderer_internal__text_vs_src, "vs_main", "vs_5_0", &vs_blob) == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__compile_shader((char*)_D3D11_RENDERER_INTERNAL__IMAGE_PS_SRC, "ps_main", "ps_5_0", &ps_blob) == FALSE)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }

    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL_POINTER, &_d3d11_renderer_internal__g.image_vs);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image_vs CreateVertexShader failed (0x", (unsigned long)hr, ")");
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        return FALSE;
    }
    hr = ID3D11Device_CreatePixelShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL_POINTER, &_d3d11_renderer_internal__g.image_ps);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image_ps CreatePixelShader failed (0x", (unsigned long)hr, ")");
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
        return FALSE;
    }

    D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = ID3D11Device_CreateInputLayout(_d3d11_renderer_internal__g.device, layout_desc, (UINT)(sizeof(layout_desc) / sizeof(layout_desc[0])), ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &_d3d11_renderer_internal__g.image_input_layout);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image input layout failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }

    // 6-vertex dynamic VBO. Same D3D11_USAGE_DYNAMIC + MAP_WRITE_DISCARD
    // pattern the other pipelines use so each submit_image starts from
    // a fresh CPU-writable buffer on the driver's ring.
    D3D11_BUFFER_DESC vb_desc;
    stdlib__memset(&vb_desc, 0, sizeof(vb_desc));
    vb_desc.Usage          = D3D11_USAGE_DYNAMIC;
    vb_desc.ByteWidth      = (UINT)(6 * 8 * sizeof(float));
    vb_desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &vb_desc, NULL_POINTER, &_d3d11_renderer_internal__g.image_vbo);
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
static void* _d3d11_renderer_internal__create_texture_rgba(const ubyte* rgba, int width, int height, boole generate_mips)
{
    if (rgba == NULL_POINTER || width <= 0 || height <= 0)
    {
        return NULL_POINTER;
    }
    if (_d3d11_renderer_internal__g.device == NULL_POINTER)
    {
        return NULL_POINTER;
    }

    // the mip chain is built on the CPU by the shared helper so every
    // backend samples the same texels (see _mip_cpu.h); the driver's
    // GenerateMips filtered differently from the other APIs. the levels
    // go in as initial data, one subresource each, into an immutable
    // texture. the chain matters: photo-sized sources drawn as 48 px
    // tiles twinkle under plain bilinear minification.
    mip_cpu__chain_t       chain;
    ubyte*                 storage       = NULL_POINTER;
    size_t                 storage_bytes = generate_mips == TRUE ? mip_cpu__storage_bytes(width, height) : 0;
    D3D11_SUBRESOURCE_DATA init[MIP_CPU__MAX_LEVELS];
    int                    level         = 0;

    if (storage_bytes > 0)
    {
        storage = (ubyte*)GUI_MALLOC_T(storage_bytes, MM_TYPE__RENDERER);
    }

    mip_cpu__build(&chain, rgba, width, height, storage);
    stdlib__memset(init, 0, sizeof(init));

    for (level = 0; level < chain.count; level++)
    {
        init[level].pSysMem     = chain.levels[level].pixels;
        init[level].SysMemPitch = (UINT)chain.levels[level].w * 4u;
    }

    D3D11_TEXTURE2D_DESC td;
    stdlib__memset(&td, 0, sizeof(td));
    td.Width            = (UINT)width;
    td.Height           = (UINT)height;
    td.MipLevels        = (UINT)chain.count;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* tex2d = NULL_POINTER;
    HRESULT hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &td, init, &tex2d);

    if (storage != NULL_POINTER)
    {
        GUI_FREE(storage);
    }

    if (FAILED(hr))
    {
        log_error("%s %08lX %s %d %s %d", "image CreateTexture2D failed (0x", (unsigned long)hr, ") for", width, "x", height);
        return NULL_POINTER;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    stdlib__memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels       = (UINT)chain.count;

    ID3D11ShaderResourceView* srv = NULL_POINTER;
    hr = ID3D11Device_CreateShaderResourceView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)tex2d, &srv_desc, &srv);
    ID3D11Texture2D_Release(tex2d);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "image CreateShaderResourceView failed (0x", (unsigned long)hr, ")");
        return NULL_POINTER;
    }

    return (void*)srv;
}

// Public image uploads retain mipmaps for images that can be drawn smaller.
void* renderer__create_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _d3d11_renderer_internal__create_texture_rgba(rgba, width, height, TRUE);
}

// The compositor's final surface is displayed at one texel per viewport pixel.
// Its lower mip levels would be rebuilt and uploaded every frame but never used.
GUI_API void* renderer_backend__create_frame_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _d3d11_renderer_internal__create_texture_rgba(rgba, width, height, FALSE);
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
    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)tex;
    ID3D11ShaderResourceView_Release(srv);
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
    float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (tex == NULL_POINTER)
    {
        return;
    }
    if (_d3d11_renderer_internal__ensure_image_pipeline() == FALSE)
    {
        return;
    }

    // Flush any pending rect / text batches FIRST so their draw
    // calls land underneath this image (correct submit-order =
    // render-order). scissor push/pop would do the same but this
    // is the direct way.
    _d3d11_renderer_internal__flush_batches();

    // cbuffer may not have been uploaded yet for this frame if the
    // tree submitted nothing else before the image. Upload now
    // (cheap; DYNAMIC + MAP_WRITE_DISCARD, a few bytes).
    _d3d11_renderer_internal__upload_constant_buffer();

    float x0 = rect.x;
    float y0 = rect.y;
    float x1 = rect.x + rect.w;
    float y1 = rect.y + rect.h;

    // 6 verts: two tris covering the quad. Layout: pos(2) col(4) uv(2)
    // = 8 floats per vertex.
    float verts[6 * 8] = {
        x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0,
        x1, y0, tint.r, tint.g, tint.b, tint.a, u1, v0,
        x0, y1, tint.r, tint.g, tint.b, tint.a, u0, v1,
        x1, y0, tint.r, tint.g, tint.b, tint.a, u1, v0,
        x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1,
        x0, y1, tint.r, tint.g, tint.b, tint.a, u0, v1,
    };

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.image_vbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        return;
    }
    stdlib__memcpy(mapped.pData, verts, sizeof(verts));
    ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.image_vbo, 0);

    // Pipeline state: image shaders + our input layout + the shared
    // text sampler + the caller's SRV at slot 0.
    // Images can be the frame's first draw, including transparent embedded overlays.
    ID3D11DeviceContext_OMSetBlendState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.blend_state, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_RSSetState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rasterizer_state);
    ID3D11DeviceContext_IASetPrimitiveTopology(_d3d11_renderer_internal__g.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.image_input_layout);

    UINT stride = (UINT)(8 * sizeof(float));
    UINT offset = 0;
    ID3D11Buffer* vbo_ptr = _d3d11_renderer_internal__g.image_vbo;
    ID3D11DeviceContext_IASetVertexBuffers(_d3d11_renderer_internal__g.context, 0, 1, &vbo_ptr, &stride, &offset);

    ID3D11DeviceContext_VSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.image_vs, NULL_POINTER, 0);
    ID3D11DeviceContext_PSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.image_ps, NULL_POINTER, 0);

    // cbuffer (viewport) was written by flush_batches' rect pass; it
    // holds the current frame's viewport. Bind it to b0 for the VS.
    ID3D11Buffer* cb = _d3d11_renderer_internal__g.cbuffer;
    ID3D11DeviceContext_VSSetConstantBuffers(_d3d11_renderer_internal__g.context, 0, 1, &cb);

    ID3D11SamplerState* samp = _d3d11_renderer_internal__g.text_sampler;
    ID3D11DeviceContext_PSSetSamplers(_d3d11_renderer_internal__g.context, 0, 1, &samp);

    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)tex;
    ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &srv);

    ID3D11DeviceContext_Draw(_d3d11_renderer_internal__g.context, 6, 0);

    // Leave SRV bound to t0 through end of frame; flush_batches on
    // next rect/text draw will overwrite what it needs. No explicit
    // clear of the SRV slot required.
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
// Matches the opengl3/gles3 implementation -- uses submit_rect, which
// this backend already supports, so the same stacking trick works.
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

// Gradient: per-corner color via the existing SDF-rect vertex format.
// The fragment shader interpolates v_color across the triangle, so
// two-color lerps cost nothing extra on the GPU side -- just different
// corner colors fed to the same push_vertex call that solid-rect uses.
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
    return (boole)(_d3d11_renderer_internal__g.adopted == FALSE && _d3d11_renderer_internal__present_paced);
}

void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color)
{
    gui_vec2_t vertices[3] = {a, b, c};
    float left = stdlib__fminf(a.x, stdlib__fminf(b.x, c.x)) - 1;
    float top = stdlib__fminf(a.y, stdlib__fminf(b.y, c.y)) - 1;
    float width = stdlib__fmaxf(a.x, stdlib__fmaxf(b.x, c.x)) - left + 1;
    float height = stdlib__fmaxf(a.y, stdlib__fmaxf(b.y, c.y)) - top + 1;
    int i;
    if (_d3d11_renderer_internal__g.vert_count + 3 > _d3d11_renderer_internal__g.vert_cap) return;
    // Expand the rectangle mask past the geometry. Triangle rasterization
    // owns the edges; the existing colour pipeline needs no extra shader.
    for (i = 0; i < 3; i++)
        _d3d11_renderer_internal__push_vertex(vertices[i].x, vertices[i].y,
            vertices[i].x - left, vertices[i].y - top, width, height, 0, color);
}

void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    int64 count = 0;
    int64 i     = 0;
    float max_r = 0.0f;

    count = gradient__tessellate(g, r, scale, opacity, verts, GRADIENT_MAX_VERTS);

    if (count <= 0 || _d3d11_renderer_internal__g.vert_count + count > _d3d11_renderer_internal__g.vert_cap)
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
        _d3d11_renderer_internal__push_vertex(verts[i].x, verts[i].y, verts[i].x - r.x, verts[i].y - r.y, r.w, r.h, radius, verts[i].c);
    }
}

/**
 * @brief draw a background layer as a grid of tiles clipped to a rounded box (renderer_backend.h)
 * @param gui_rect_t box -> the node's box in device pixels: the rounded-box mask every tile is clipped to
 * @param float radius -> corner radius of the box
 * @param gui_rect_t tile -> the first tile; the grid steps by its size to the right and downwards
 * @param int columns -> tiles per row
 * @param int rows -> tile rows
 * @param const gui_gradient_t* gradient -> the tile's gradient, or NULL for a solid tile
 * @param gui_color_t color -> the solid tile color
 * @param float scale -> logical scale for the gradient's pixel stops
 * @param float opacity -> multiplies every vertex alpha
 * @return void
 */
GUI_API void renderer_backend__submit_tiles(gui_rect_t box, float radius, gui_rect_t tile, int columns, int rows, const gui_gradient_t* gradient, gui_color_t color, float scale, float opacity)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    int64 count = 0;
    int64 i = 0;
    int column = 0;
    int row = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    float max_r = (box.w < box.h ? box.w : box.h) * 0.5f;

    if (columns <= 0 || rows <= 0 || tile.w <= 0.0f || tile.h <= 0.0f)
    {
        return;
    }
    if (radius < 0.0f)
    {
        radius = 0.0f;
    }
    if (radius > max_r)
    {
        radius = max_r;
    }
    if (gradient != NULL_POINTER)
    {
        count = gradient__tessellate(gradient, tile, scale, opacity, verts, GRADIENT_MAX_VERTS);
    }
    else
    {
        // the two triangles of renderer__submit_rect
        color.a *= opacity;
        for (i = 0; i < 6; i++)
        {
            verts[i].x = tile.x + ((i == 1 || i == 3 || i == 4) ? tile.w : 0.0f);
            verts[i].y = tile.y + ((i == 2 || i == 4 || i == 5) ? tile.h : 0.0f);
            verts[i].c = color;
        }
        count = 6;
    }
    if (count <= 0)
    {
        return;
    }
    for (row = 0; row < rows; row++)
    {
        dy = (float)row * tile.h;
        for (column = 0; column < columns; column++)
        {
            dx = (float)column * tile.w;
            // a full batch is drawn before the next tile; the flush keeps
            // the submit order (every pending rect and glyph goes first)
            if (_d3d11_renderer_internal__g.vert_count + count > _d3d11_renderer_internal__g.vert_cap)
            {
                _d3d11_renderer_internal__flush_batches();
                _d3d11_renderer_internal__tile_flushes++;
            }
            if (_d3d11_renderer_internal__g.vert_count + count > _d3d11_renderer_internal__g.vert_cap)
            {
                return;
            }
            _d3d11_renderer_internal__tile_vertices += (uint64)count;
            for (i = 0; i < count; i++)
            {
                _d3d11_renderer_internal__push_vertex(verts[i].x + dx, verts[i].y + dy, verts[i].x + dx - box.x, verts[i].y + dy - box.y, box.w, box.h, radius, verts[i].c);
            }
        }
    }
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
    int64 w = (int64)_d3d11_renderer_internal__g.viewport_w;
    int64 h = (int64)_d3d11_renderer_internal__g.viewport_h;

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

    _d3d11_renderer_internal__capture_out  = out_rgba;
    _d3d11_renderer_internal__capture_cap  = cap;
    _d3d11_renderer_internal__capture_done = FALSE;

    return TRUE;
}

/**
 * @brief tells whether the armed capture has been copied out (it is, once renderer__end_frame ran after the request)
 *
 * @return boole -> TRUE after the copy
 */
boole renderer__capture_done(void)
{
    return _d3d11_renderer_internal__capture_done;
}

// Ordinary standalone scenes can use this renderer directly.
GUI_API boole renderer_backend__needs_compositor(void) { return FALSE; }

// ============================================================================
// LAYER MODE (renderer_backend.h, src/compositor.c, session/LAYERS-DESIGN.md)
// ============================================================================

#include "shaders/generated/layer.vert.h"
#include "shaders/generated/layer.frag.h"

/**
 * @brief create one of the layer blend states
 * @param D3D11_BLEND source_color -> color source factor
 * @param D3D11_BLEND destination_color -> color destination factor
 * @param D3D11_BLEND source_alpha -> alpha source factor
 * @param D3D11_BLEND destination_alpha -> alpha destination factor
 * @param ID3D11BlendState** out -> created state
 * @return boole -> TRUE on success
 */
static boole _d3d11_renderer_internal__layer_blend_state(D3D11_BLEND source_color, D3D11_BLEND destination_color, D3D11_BLEND source_alpha, D3D11_BLEND destination_alpha, ID3D11BlendState** out)
{
    D3D11_BLEND_DESC bd;
    HRESULT hr = 0;

    stdlib__memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = source_color;
    bd.RenderTarget[0].DestBlend             = destination_color;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = source_alpha;
    bd.RenderTarget[0].DestBlendAlpha        = destination_alpha;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(_d3d11_renderer_internal__g.device, &bd, out);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "layer CreateBlendState failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }
    return TRUE;
}

/**
 * @brief build the layer pipeline on first use
 * @return boole -> TRUE when the pipeline is ready
 */
static boole _d3d11_renderer_internal__ensure_layer_pipeline(void)
{
    ID3DBlob* vs_blob = NULL_POINTER;
    ID3DBlob* ps_blob = NULL_POINTER;
    D3D11_INPUT_ELEMENT_DESC layout_desc[3];
    D3D11_BUFFER_DESC vb_desc;
    D3D11_SAMPLER_DESC sd;
    HRESULT hr = 0;

    if (_d3d11_renderer_internal__g.layer_ps != NULL_POINTER)
    {
        return TRUE;
    }
    if (_d3d11_renderer_internal__g.device == NULL_POINTER)
    {
        return FALSE;
    }
    stdlib__memset(layout_desc, 0, sizeof(layout_desc));
    stdlib__memset(&vb_desc, 0, sizeof(vb_desc));
    stdlib__memset(&sd, 0, sizeof(sd));
    if (_d3d11_renderer_internal__compile_shader((char*)_D3D11_RENDERER_INTERNAL__LAYER_VS_SRC, "vs_main", "vs_5_0", &vs_blob) == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__compile_shader((char*)_D3D11_RENDERER_INTERNAL__LAYER_PS_SRC, "ps_main", "ps_5_0", &ps_blob) == FALSE)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
        return FALSE;
    }
    hr = ID3D11Device_CreateVertexShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL_POINTER, &_d3d11_renderer_internal__g.layer_vs);
    if (SUCCEEDED(hr))
    {
        hr = ID3D11Device_CreatePixelShader(_d3d11_renderer_internal__g.device, ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob), NULL_POINTER, &_d3d11_renderer_internal__g.layer_ps);
    }
    if (SUCCEEDED(hr))
    {
        layout_desc[0].SemanticName = "POSITION";
        layout_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
        layout_desc[0].AlignedByteOffset = 0;
        layout_desc[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        layout_desc[1].SemanticName = "TEXCOORD";
        layout_desc[1].Format = DXGI_FORMAT_R32G32_FLOAT;
        layout_desc[1].AlignedByteOffset = 12;
        layout_desc[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        layout_desc[2].SemanticName = "TEXCOORD";
        layout_desc[2].SemanticIndex = 1;
        layout_desc[2].Format = DXGI_FORMAT_R32_FLOAT;
        layout_desc[2].AlignedByteOffset = 20;
        layout_desc[2].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        hr = ID3D11Device_CreateInputLayout(_d3d11_renderer_internal__g.device, layout_desc, 3, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), &_d3d11_renderer_internal__g.layer_input_layout);
    }
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(vs_blob);
    _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(ps_blob);
    if (SUCCEEDED(hr))
    {
        vb_desc.Usage          = D3D11_USAGE_DYNAMIC;
        vb_desc.ByteWidth      = (UINT)(6 * 6 * sizeof(float));
        vb_desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(_d3d11_renderer_internal__g.device, &vb_desc, NULL_POINTER, &_d3d11_renderer_internal__g.layer_vbo);
    }
    if (SUCCEEDED(hr))
    {
        // integer-aligned identity quads fetch exact texels, as the CPU
        // compose does for an integer translation
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(_d3d11_renderer_internal__g.device, &sd, &_d3d11_renderer_internal__g.layer_point_sampler);
    }
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "layer pipeline failed (0x", (unsigned long)hr, ")");
        return FALSE;
    }
    // premultiplied source over: d = s + d * (1 - a); screen: d = s + d * (1 - s); replace: d = s
    if (_d3d11_renderer_internal__layer_blend_state(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, &_d3d11_renderer_internal__g.layer_blend_normal) == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__layer_blend_state(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, &_d3d11_renderer_internal__g.layer_blend_screen) == FALSE)
    {
        return FALSE;
    }
    if (_d3d11_renderer_internal__layer_blend_state(D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, &_d3d11_renderer_internal__g.layer_blend_replace) == FALSE)
    {
        return FALSE;
    }
    // multiply over an opaque backdrop: d = s * d + d * (1 - a)
    if (_d3d11_renderer_internal__layer_blend_state(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, &_d3d11_renderer_internal__g.layer_blend_multiply) == FALSE)
    {
        return FALSE;
    }
    return TRUE;
}

GUI_API boole renderer_backend__supports_layers(void)
{
    return (boole)(_d3d11_renderer_internal__g.device != NULL_POINTER);
}

/**
 * @brief a single-level premultiplied RGBA8 texture that UpdateSubresource can refresh (DEFAULT usage)
 * @param const ubyte* premultiplied_rgba -> tightly packed pixels, or NULL for an uninitialized texture
 * @param int width -> width
 * @param int height -> height
 * @return void* -> shader resource view, released with renderer__destroy_texture
 */
GUI_API void* renderer_backend__create_layer_texture(const ubyte* premultiplied_rgba, int width, int height)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D11Texture2D* tex2d = NULL_POINTER;
    ID3D11ShaderResourceView* srv = NULL_POINTER;
    HRESULT hr = 0;

    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || _d3d11_renderer_internal__g.device == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    stdlib__memset(&td, 0, sizeof(td));
    stdlib__memset(&init, 0, sizeof(init));
    stdlib__memset(&srv_desc, 0, sizeof(srv_desc));
    td.Width            = (UINT)width;
    td.Height           = (UINT)height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem     = premultiplied_rgba;
    init.SysMemPitch = (UINT)width * 4u;
    hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &td, (premultiplied_rgba != NULL_POINTER) ? &init : NULL_POINTER, &tex2d);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s %d %s %d", "layer CreateTexture2D failed (0x", (unsigned long)hr, ") for", width, "x", height);
        return NULL_POINTER;
    }
    srv_desc.Format                    = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    hr = ID3D11Device_CreateShaderResourceView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)tex2d, &srv_desc, &srv);
    ID3D11Texture2D_Release(tex2d);
    if (FAILED(hr))
    {
        log_error("%s %08lX %s", "layer CreateShaderResourceView failed (0x", (unsigned long)hr, ")");
        return NULL_POINTER;
    }
    return (void*)srv;
}

/**
 * @brief refresh a layer texture of the same size in place
 * @param void* texture -> from renderer_backend__create_layer_texture
 * @param const ubyte* premultiplied_rgba -> tightly packed pixels
 * @param int width -> must equal the texture width
 * @param int height -> must equal the texture height
 * @return boole -> FALSE when the size differs (the caller then recreates)
 */
GUI_API boole renderer_backend__update_layer_texture(void* texture, const ubyte* premultiplied_rgba, int width, int height)
{
    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)texture;
    ID3D11Resource* resource = NULL_POINTER;
    ID3D11Texture2D* tex2d = NULL_POINTER;
    D3D11_TEXTURE2D_DESC td;
    boole result = FALSE;

    if (srv == NULL_POINTER || premultiplied_rgba == NULL_POINTER || width <= 0 || height <= 0)
    {
        return FALSE;
    }
    stdlib__memset(&td, 0, sizeof(td));
    ID3D11ShaderResourceView_GetResource(srv, &resource);
    if (resource == NULL_POINTER)
    {
        return FALSE;
    }
    if (SUCCEEDED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void**)&tex2d)) && tex2d != NULL_POINTER)
    {
        ID3D11Texture2D_GetDesc(tex2d, &td);
        if (td.Width == (UINT)width && td.Height == (UINT)height && td.Usage == D3D11_USAGE_DEFAULT)
        {
            EFFECTS_PROFILE_BEGIN(upload_profile);
            ID3D11DeviceContext_UpdateSubresource(_d3d11_renderer_internal__g.context, (ID3D11Resource*)tex2d, 0, NULL_POINTER, premultiplied_rgba, (UINT)width * 4u, 0);
            EFFECTS_PROFILE_STEP(upload_profile, EFFECTS_PROFILE_LAYER_UPLOAD);
            result = TRUE;
        }
        ID3D11Texture2D_Release(tex2d);
    }
    ID3D11Resource_Release(resource);
    return result;
}

/**
 * @brief draw a premultiplied layer texture over a homogeneous quad (renderer_backend.h)
 * @param void* texture -> layer texture
 * @param const float* corners -> 4 x (x * w, y * w, w): top-left, top-right, bottom-right, bottom-left
 * @param float opacity -> multiplies all four channels
 * @param int blend_mode -> 0 normal, 1 screen, 2 replace
 * @return void
 */
GUI_API void renderer_backend__submit_layer(void* texture, const float* corners, float opacity, int blend_mode)
{
    float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float verts[6 * 6];
    static const int order[6] = {0, 1, 3, 1, 2, 3};
    static const float uvs[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11ShaderResourceView* srv = (ID3D11ShaderResourceView*)texture;
    ID3D11Resource* resource = NULL_POINTER;
    ID3D11Texture2D* tex2d = NULL_POINTER;
    D3D11_TEXTURE2D_DESC td;
    ID3D11BlendState* blend = NULL_POINTER;
    ID3D11SamplerState* sampler = NULL_POINTER;
    ID3D11Buffer* vbo_ptr = NULL_POINTER;
    ID3D11Buffer* cb = NULL_POINTER;
    UINT stride = (UINT)(6 * sizeof(float));
    UINT offset = 0;
    boole aligned = FALSE;
    HRESULT hr = 0;
    int i = 0;

    stdlib__memset(&mapped, 0, sizeof(mapped));
    stdlib__memset(&td, 0, sizeof(td));
    stdlib__memset(verts, 0, sizeof(verts));
    if (srv == NULL_POINTER || corners == NULL_POINTER || _d3d11_renderer_internal__ensure_layer_pipeline() == FALSE)
    {
        return;
    }
    // draws submitted before this layer land underneath it
    _d3d11_renderer_internal__flush_batches();
    _d3d11_renderer_internal__upload_constant_buffer();
    for (i = 0; i < 6; i++)
    {
        verts[i * 6 + 0] = corners[order[i] * 3 + 0];
        verts[i * 6 + 1] = corners[order[i] * 3 + 1];
        verts[i * 6 + 2] = corners[order[i] * 3 + 2];
        verts[i * 6 + 3] = uvs[order[i]][0];
        verts[i * 6 + 4] = uvs[order[i]][1];
        verts[i * 6 + 5] = opacity;
    }
    hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.layer_vbo, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        return;
    }
    stdlib__memcpy(mapped.pData, verts, sizeof(verts));
    ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)_d3d11_renderer_internal__g.layer_vbo, 0);
    // an integer-aligned identity quad of the texture's size samples texel
    // centers exactly, so point sampling reproduces the CPU's aligned fetch
    ID3D11ShaderResourceView_GetResource(srv, &resource);
    if (resource != NULL_POINTER)
    {
        if (SUCCEEDED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void**)&tex2d)) && tex2d != NULL_POINTER)
        {
            ID3D11Texture2D_GetDesc(tex2d, &td);
            ID3D11Texture2D_Release(tex2d);
        }
        ID3D11Resource_Release(resource);
    }
    aligned = (boole)(corners[2] == 1.0f && corners[5] == 1.0f && corners[8] == 1.0f && corners[11] == 1.0f && corners[0] == (float)(int)corners[0] && corners[1] == (float)(int)corners[1] && corners[3] - corners[0] == (float)td.Width && corners[7] - corners[1] == (float)td.Height && corners[4] == corners[1] && corners[6] == corners[3] && corners[9] == corners[0] && corners[10] == corners[7]);
    sampler = (aligned == TRUE) ? _d3d11_renderer_internal__g.layer_point_sampler : _d3d11_renderer_internal__g.text_sampler;
    blend = (blend_mode == 1) ? _d3d11_renderer_internal__g.layer_blend_screen : ((blend_mode == 2) ? _d3d11_renderer_internal__g.layer_blend_replace : ((blend_mode == 3) ? _d3d11_renderer_internal__g.layer_blend_multiply : _d3d11_renderer_internal__g.layer_blend_normal));
    ID3D11DeviceContext_OMSetBlendState(_d3d11_renderer_internal__g.context, blend, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_RSSetState(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.rasterizer_state);
    ID3D11DeviceContext_IASetPrimitiveTopology(_d3d11_renderer_internal__g.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.layer_input_layout);
    vbo_ptr = _d3d11_renderer_internal__g.layer_vbo;
    ID3D11DeviceContext_IASetVertexBuffers(_d3d11_renderer_internal__g.context, 0, 1, &vbo_ptr, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.layer_vs, NULL_POINTER, 0);
    ID3D11DeviceContext_PSSetShader(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.layer_ps, NULL_POINTER, 0);
    cb = _d3d11_renderer_internal__g.cbuffer;
    ID3D11DeviceContext_VSSetConstantBuffers(_d3d11_renderer_internal__g.context, 0, 1, &cb);
    ID3D11DeviceContext_PSSetSamplers(_d3d11_renderer_internal__g.context, 0, 1, &sampler);
    ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &srv);
    ID3D11DeviceContext_Draw(_d3d11_renderer_internal__g.context, 6, 0);
    srv = NULL_POINTER;
    ID3D11DeviceContext_PSSetShaderResources(_d3d11_renderer_internal__g.context, 0, 1, &srv);
}

/**
 * @brief draw a layer's content into a pooled render target (renderer_backend.h)
 * @param int width -> layer width in device pixels
 * @param int height -> layer height in device pixels
 * @return boole -> FALSE when no target is available (the caller rasterizes on the CPU)
 */
GUI_API boole renderer_backend__begin_layer(int width, int height)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_VIEWPORT vp;
    FLOAT clear_rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    HRESULT hr = 0;
    int slot = -1;
    int i = 0;

    stdlib__memset(&td, 0, sizeof(td));
    stdlib__memset(&vp, 0, sizeof(vp));
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || _d3d11_renderer_internal__g.device == NULL_POINTER || _d3d11_renderer_internal__g.layer_active != 0)
    {
        return FALSE;
    }
    // a target of the same size, else the least recently used slot
    for (i = 0; i < 4; i++)
    {
        if (_d3d11_renderer_internal__g.layer_targets[i].texture != NULL_POINTER && _d3d11_renderer_internal__g.layer_targets[i].w == width && _d3d11_renderer_internal__g.layer_targets[i].h == height)
        {
            slot = i;
            break;
        }
        if (slot < 0 || _d3d11_renderer_internal__g.layer_targets[i].used < _d3d11_renderer_internal__g.layer_targets[slot].used)
        {
            slot = i;
        }
    }
    if (_d3d11_renderer_internal__g.layer_targets[slot].texture == NULL_POINTER || _d3d11_renderer_internal__g.layer_targets[slot].w != width || _d3d11_renderer_internal__g.layer_targets[slot].h != height)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].srv);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].rtv);
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].texture);
        td.Width            = (UINT)width;
        td.Height           = (UINT)height;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &td, NULL_POINTER, &_d3d11_renderer_internal__g.layer_targets[slot].texture);
        if (SUCCEEDED(hr))
        {
            hr = ID3D11Device_CreateRenderTargetView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)_d3d11_renderer_internal__g.layer_targets[slot].texture, NULL_POINTER, &_d3d11_renderer_internal__g.layer_targets[slot].rtv);
        }
        if (SUCCEEDED(hr))
        {
            hr = ID3D11Device_CreateShaderResourceView(_d3d11_renderer_internal__g.device, (ID3D11Resource*)_d3d11_renderer_internal__g.layer_targets[slot].texture, NULL_POINTER, &_d3d11_renderer_internal__g.layer_targets[slot].srv);
        }
        if (FAILED(hr))
        {
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].srv);
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].rtv);
            _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.layer_targets[slot].texture);
            log_error("%s %08lX %s", "layer render target failed (0x", (unsigned long)hr, ")");
            return FALSE;
        }
        _d3d11_renderer_internal__g.layer_targets[slot].w = width;
        _d3d11_renderer_internal__g.layer_targets[slot].h = height;
    }
    // draws submitted before the layer belong to the frame
    _d3d11_renderer_internal__flush_batches();
    _d3d11_renderer_internal__g.layer_targets[slot].used = ++_d3d11_renderer_internal__g.layer_counter;
    _d3d11_renderer_internal__g.layer_active = slot + 1;
    _d3d11_renderer_internal__g.layer_saved_viewport_w = _d3d11_renderer_internal__g.viewport_w;
    _d3d11_renderer_internal__g.layer_saved_viewport_h = _d3d11_renderer_internal__g.viewport_h;
    _d3d11_renderer_internal__g.layer_saved_scissor_depth = _d3d11_renderer_internal__g.scissor_depth;
    stdlib__memcpy(_d3d11_renderer_internal__g.layer_saved_scissors, _d3d11_renderer_internal__g.scissor_stack, sizeof(_d3d11_renderer_internal__g.scissor_stack));
    // the vertex shaders map pixels through the viewport size in the
    // constant buffer: inside the layer that size is the layer's
    _d3d11_renderer_internal__g.viewport_w = width;
    _d3d11_renderer_internal__g.viewport_h = height;
    _d3d11_renderer_internal__g.scissor_depth = 0;
    // the constant buffer is uploaded once per frame; the layer needs its own size in it
    _d3d11_renderer_internal__upload_constant_buffer();
    ID3D11DeviceContext_OMSetRenderTargets(_d3d11_renderer_internal__g.context, 1, &_d3d11_renderer_internal__g.layer_targets[slot].rtv, NULL_POINTER);
    ID3D11DeviceContext_ClearRenderTargetView(_d3d11_renderer_internal__g.context, _d3d11_renderer_internal__g.layer_targets[slot].rtv, clear_rgba);
    vp.Width    = (FLOAT)width;
    vp.Height   = (FLOAT)height;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(_d3d11_renderer_internal__g.context, 1, &vp);
    _d3d11_renderer_internal__apply_scissor_top();
    return TRUE;
}

/**
 * @brief finish the layer: back to the frame's target, viewport and scissor
 * @return void* -> the layer's shader resource view (owned by the pool)
 */
GUI_API void* renderer_backend__end_layer(void)
{
    D3D11_VIEWPORT vp;
    int slot = _d3d11_renderer_internal__g.layer_active - 1;

    stdlib__memset(&vp, 0, sizeof(vp));
    if (slot < 0)
    {
        return NULL_POINTER;
    }
    _d3d11_renderer_internal__flush_batches();
    _d3d11_renderer_internal__g.layer_active = 0;
    _d3d11_renderer_internal__g.viewport_w = _d3d11_renderer_internal__g.layer_saved_viewport_w;
    _d3d11_renderer_internal__g.viewport_h = _d3d11_renderer_internal__g.layer_saved_viewport_h;
    _d3d11_renderer_internal__g.scissor_depth = _d3d11_renderer_internal__g.layer_saved_scissor_depth;
    stdlib__memcpy(_d3d11_renderer_internal__g.scissor_stack, _d3d11_renderer_internal__g.layer_saved_scissors, sizeof(_d3d11_renderer_internal__g.scissor_stack));
    _d3d11_renderer_internal__upload_constant_buffer();
    ID3D11DeviceContext_OMSetRenderTargets(_d3d11_renderer_internal__g.context, 1, &_d3d11_renderer_internal__g.rtv, NULL_POINTER);
    vp.Width    = (FLOAT)_d3d11_renderer_internal__g.viewport_w;
    vp.Height   = (FLOAT)_d3d11_renderer_internal__g.viewport_h;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(_d3d11_renderer_internal__g.context, 1, &vp);
    _d3d11_renderer_internal__apply_scissor_top();
    return (void*)_d3d11_renderer_internal__g.layer_targets[slot].srv;
}

/**
 * @brief read a back-buffer region (premultiplied by construction of the blend states), top-left origin
 * @param int x -> left, device pixels
 * @param int y -> top
 * @param int w -> width
 * @param int h -> height
 * @param ubyte* out_rgba -> w * h * 4 bytes
 * @return boole -> FALSE when the region leaves the viewport or the copy fails
 */
GUI_API boole renderer_backend__read_region(int x, int y, int w, int h, ubyte* out_rgba)
{
    ID3D11Texture2D* backbuffer = NULL_POINTER;
    ID3D11Texture2D* staging = NULL_POINTER;
    D3D11_TEXTURE2D_DESC sd;
    D3D11_BOX box;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = 0;
    ubyte swap = 0;
    int row = 0;

    stdlib__memset(&sd, 0, sizeof(sd));
    stdlib__memset(&box, 0, sizeof(box));
    stdlib__memset(&mapped, 0, sizeof(mapped));
    if (out_rgba == NULL_POINTER || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > (int)_d3d11_renderer_internal__g.viewport_w || y + h > (int)_d3d11_renderer_internal__g.viewport_h || _d3d11_renderer_internal__g.swapchain == NULL_POINTER)
    {
        return FALSE;
    }
    EFFECTS_PROFILE_BEGIN(readback_profile);
    _d3d11_renderer_internal__flush_batches();
    hr = IDXGISwapChain_GetBuffer(_d3d11_renderer_internal__g.swapchain, 0, &IID_ID3D11Texture2D, (void**)&backbuffer);
    if (FAILED(hr) || backbuffer == NULL_POINTER)
    {
        return FALSE;
    }
    ID3D11Texture2D_GetDesc(backbuffer, &sd);
    // the staging texture is kept and grown; a region copies into its corner
    if (_d3d11_renderer_internal__g.readback_staging == NULL_POINTER || _d3d11_renderer_internal__g.readback_w < w || _d3d11_renderer_internal__g.readback_h < h)
    {
        _D3D11_RENDERER_INTERNAL__SAFE_RELEASE(_d3d11_renderer_internal__g.readback_staging);
        sd.Width          = (UINT)((w > _d3d11_renderer_internal__g.readback_w) ? w : _d3d11_renderer_internal__g.readback_w);
        sd.Height         = (UINT)((h > _d3d11_renderer_internal__g.readback_h) ? h : _d3d11_renderer_internal__g.readback_h);
        sd.MipLevels      = 1;
        sd.ArraySize      = 1;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.BindFlags      = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags      = 0;
        hr = ID3D11Device_CreateTexture2D(_d3d11_renderer_internal__g.device, &sd, NULL_POINTER, &_d3d11_renderer_internal__g.readback_staging);
        if (FAILED(hr) || _d3d11_renderer_internal__g.readback_staging == NULL_POINTER)
        {
            _d3d11_renderer_internal__g.readback_staging = NULL_POINTER;
            ID3D11Texture2D_Release(backbuffer);
            return FALSE;
        }
        _d3d11_renderer_internal__g.readback_w = (int)sd.Width;
        _d3d11_renderer_internal__g.readback_h = (int)sd.Height;
    }
    staging = _d3d11_renderer_internal__g.readback_staging;
    box.left   = (UINT)x;
    box.top    = (UINT)y;
    box.right  = (UINT)(x + w);
    box.bottom = (UINT)(y + h);
    box.back   = 1;
    ID3D11DeviceContext_CopySubresourceRegion(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0, 0, 0, 0, (ID3D11Resource*)backbuffer, 0, &box);
    EFFECTS_PROFILE_STEP(readback_profile, EFFECTS_PROFILE_READBACK_COPY);
    hr = ID3D11DeviceContext_Map(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &mapped);
    EFFECTS_PROFILE_STEP(readback_profile, EFFECTS_PROFILE_READBACK_MAP);
    if (SUCCEEDED(hr) && mapped.pData != NULL_POINTER)
    {
        for (row = 0; row < h; row++)
        {
            stdlib__memcpy(out_rgba + (size_t)row * (size_t)w * 4, (const ubyte*)mapped.pData + (size_t)row * (size_t)mapped.RowPitch, (size_t)w * 4);
        }
        // an adopted swap chain may be BGRA: the compositor works in RGBA
        if (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        {
            for (row = 0; row < w * h; row++)
            {
                swap = out_rgba[row * 4];
                out_rgba[row * 4] = out_rgba[row * 4 + 2];
                out_rgba[row * 4 + 2] = swap;
            }
        }
        ID3D11DeviceContext_Unmap(_d3d11_renderer_internal__g.context, (ID3D11Resource*)staging, 0);
    }
    ID3D11Texture2D_Release(backbuffer);
    return (boole)SUCCEEDED(hr);
}

#include "d3d11_blur.h"
