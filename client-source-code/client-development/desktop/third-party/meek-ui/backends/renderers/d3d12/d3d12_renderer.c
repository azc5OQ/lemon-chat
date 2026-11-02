// ============================================================================
// d3d12_renderer.c - Direct3D 12 backend.
// ============================================================================
// Fifth renderer.h implementation. Status: SCAFFOLD (solid-rect +
// scissor working, text + image + gradient + shadow stubbed).
// Matches the completeness level of d3d11 + d3d9 + vulkan scaffolds.
// Platform: Windows only (D3D12 is Win32). The platform_win32 layer
// hands its HWND to renderer__init; we create a DXGI swap chain
// against it.
// VISUAL CONTRACT (from renderer.h): same SDF rounded-box formula,
// smoothstep range, scissor semantics, separate-alpha blend, top-
// left-origin pixel coordinate space as every other backend.
// DEPENDENCIES:
// d3d12.lib d3d12.dll       the core API.
// dxgi.lib dxgi1_4+.dll     swapchain + factory.
// d3dcompiler.lib           runtime HLSL compile, same as d3d11.
// dxguid.lib                IID_* constants for the COM-style
// ID3D12xxx interfaces.
// LIFECYCLE (renderer__init):
// 1. D3D12CreateDevice(D3D_FEATURE_LEVEL_11_0)
// 2. create direct command queue
// 3. CreateDXGIFactory -> CreateSwapChainForHwnd
// 4. descriptor heaps: RTV (for swapchain buffers)
// 5. command allocator + command list (one per frame-in-flight)
// 6. fence + event for CPU/GPU sync
// 7. root signature (one root constant for viewport)
// 8. rect PSO: HLSL via D3DCompile, IA layout, blend, raster
// 9. upload vertex buffer (committed resource in UPLOAD heap,
// persistently mapped)
// FRAME LOOP (begin_frame -> submit_rect* -> end_frame):
// - wait fence for this frame slot
// - reset allocator + list
// - barrier backbuffer PRESENT -> RENDER_TARGET
// - OMSetRenderTargets, ClearRenderTargetView, SetPipelineState,
// SetGraphicsRootSignature, RSSetViewports, RSSetScissorRects,
// IASetVertexBuffers, SetGraphicsRoot32BitConstants, DrawInstanced
// - barrier RENDER_TARGET -> PRESENT
// - Close + ExecuteCommandLists
// - Present(1,0) (vsync)
// - Signal fence, advance frame slot

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "types.h"
#include "gui.h"
#define MEEK_RENDERER_BACKEND
#include "renderer.h"
#include "renderer_backend.h"
#include "gradient.h"
#include "d3d12_renderer.h"
#include "_visual_contract.h"
#include "_blur_cpu.h"
#include "_mip_cpu.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// C-mode COM helpers. D3D12 headers only expose the method-table
// form in C (no operator overloading); the `vtbl_call(p, M, ...)`
// macro keeps call sites readable. Same pattern the d3d11_renderer
// + d3d9_renderer use.
#define VCALL(obj, method, ...) ((obj)->lpVtbl->method((obj), __VA_ARGS__))
#define VCALL0(obj, method)     ((obj)->lpVtbl->method((obj)))

// ============================================================================
// constants
// ============================================================================

// MAX_QUADS is sized for a WHOLE FRAME's worth of rects, not a
// mid-frame flush batch -- we can't safely reuse lower offsets in
// the persistent-mapped UPLOAD buffer after a flush has recorded a
// DrawInstanced against them. 8192 rect quads = 48K verts = ~2.1 MB
// per FIF slot; easily fits even heavy UIs. Same reasoning for
// MAX_TEXT_QUADS: 16384 glyph quads covers large text pages.
#define _D3D12_RENDERER_INTERNAL__MAX_QUADS         16384
#define _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD    6
#define _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT  2
#define _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT 2
// an adopted swap chain may have more buffers than our own; the view arrays are sized for the maximum
#define _D3D12_RENDERER_INTERNAL__MAX_SWAP_BUFFERS 4
#define _D3D12_RENDERER_INTERNAL__MAX_SCISSORS      16
#define _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS    16384
#define _D3D12_RENDERER_INTERNAL__MAX_TEXT_RUNS     64
// Shader-visible CBV_SRV_UAV heap capacity. Every loaded atlas +
// every uploaded image texture reserves one slot. Widget_image's
// cache caps at 64; font.c creates at most a few atlases per frame.
// 256 leaves generous headroom.
#define _D3D12_RENDERER_INTERNAL__MAX_SRVS          256

// Per-slot deferred-destroy ring for tex_entry pointers whose draws
// were recorded into the per-frame command list and so must outlive
// the frame -- blur_region creates one per call. Sized for many
// blur regions per frame with safety margin.
#define _D3D12_RENDERER_INTERNAL__MAX_PENDING_FREES 32

// ============================================================================
// vertex layout -- matches every other backend
// ============================================================================

typedef struct _d3d12_renderer_internal__vertex_t
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
} _d3d12_renderer_internal__vertex_t;

// Text + image vertex: pos + color + uv = 8 floats. Same layout as
// the d3d11 text/image pipelines. Both use the SAME vertex shader
// (view->NDC + passthrough color/uv) but different pixel shaders
// because R8 atlas sampling and RGBA texture sampling want different
// math.
typedef struct _d3d12_renderer_internal__tx_vertex_t
{
    float x;   // 0x0
    float y;   // 0x4
    float r;   // 0x8
    float g;   // 0xC
    float b;   // 0x10
    float a;   // 0x14
    float u;   // 0x18
    float v;   // 0x1C
} _d3d12_renderer_internal__tx_vertex_t;

typedef struct _d3d12_renderer_internal__scissor_entry_t
{
    D3D12_RECT rect;
    boole      active;
} _d3d12_renderer_internal__scissor_entry_t;

// Atlas / texture record. One per loaded atlas or image. Holds the
// GPU texture + its shader-visible SRV slot. The void* handle we
// hand back to font.c / widget_image.c is a pointer to this struct.
typedef struct _d3d12_renderer_internal__tex_entry_t
{
    ID3D12Resource*              resource;
    int                          srv_slot; // index into srv_heap
    D3D12_GPU_DESCRIPTOR_HANDLE  srv_gpu; // cached so glyph path skips offset-math
    int                          width;
    int                          height;
    boole                        in_use;
} _d3d12_renderer_internal__tex_entry_t;

// One run of text quads sharing an atlas. end_frame binds the run's
// atlas SRV once, then draws the run's vertex range with a single
// DrawInstanced. Scene typically issues a small number of runs per
// frame (one per font size / family combination).
typedef struct _d3d12_renderer_internal__text_run_t
{
    _d3d12_renderer_internal__tex_entry_t* atlas;         // 0x0
    int64                                 vert_start;   // 0x8
    int64                                 vert_count;   // 0x10
} _d3d12_renderer_internal__text_run_t;

// ============================================================================
// state
// ============================================================================

typedef struct _d3d12_renderer_internal__state_t
{
    HWND hwnd;

    ID3D12Device*               device;
    ID3D12CommandQueue*         queue;
    IDXGISwapChain3*            swapchain;

    // RTV descriptor heap (one per swap buffer)
    ID3D12DescriptorHeap*       rtv_heap;
    UINT                        rtv_stride;
    ID3D12Resource*             swap_buffers[_D3D12_RENDERER_INTERNAL__MAX_SWAP_BUFFERS];
    D3D12_CPU_DESCRIPTOR_HANDLE swap_rtvs   [_D3D12_RENDERER_INTERNAL__MAX_SWAP_BUFFERS];

    // per-frame command recording
    ID3D12CommandAllocator*     allocators[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    ID3D12GraphicsCommandList*  cmd_lists[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];

    // fence + event
    ID3D12Fence*                fence;
    UINT64                      fence_values[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    UINT64                      next_fence_value;
    HANDLE                      fence_event;

    // rect pipeline
    ID3D12RootSignature*        root_sig;
    ID3D12PipelineState*        rect_pso;

    // Rect vertex buffer (one per frame-in-flight). Append-only
    // within a frame: CPU writes at offset rect_vert_count, flush
    // draws [rect_draw_cursor..rect_vert_count) then advances
    // rect_draw_cursor to rect_vert_count. We do NOT reset
    // rect_vert_count mid-frame because the command list may have
    // recorded a DrawInstanced that will read those verts later,
    // and persistent-mapped UPLOAD memory means CPU writes to the
    // same offsets would stomp the still-pending draw's data. Both
    // cursors reset to 0 at begin_frame (previous frame's work has
    // completed per the fence wait).
    ID3D12Resource*             rect_vbo[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    _d3d12_renderer_internal__vertex_t* rect_vbo_ptr[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    int64                       rect_vert_count;
    int64                       rect_draw_cursor;

    // current frame slot / backbuffer index
    UINT                        frame_slot; // 0..FRAMES_IN_FLIGHT-1
    UINT                        backbuffer_index; // 0..SWAP_BUFFER_COUNT-1

    // viewport
    int64                       viewport_w;
    int64                       viewport_h;
    gui_color_t                   clear_color;
    boole                       frame_active;

    // scissor
    _d3d12_renderer_internal__scissor_entry_t scissors[_D3D12_RENDERER_INTERNAL__MAX_SCISSORS];
    int64                       scissor_depth;

    // text + image: shared shader-visible SRV heap + pool of tex entries
    ID3D12DescriptorHeap*       srv_heap;
    UINT                        srv_stride;
    _d3d12_renderer_internal__tex_entry_t tex_pool[_D3D12_RENDERER_INTERNAL__MAX_SRVS];

    // text + image: shared root signature (+ static sampler) and pipelines
    ID3D12RootSignature*        tx_root_sig;
    ID3D12PipelineState*        text_pso;
    ID3D12PipelineState*        image_pso;
    boole                       tx_built; // lazy-built on first atlas/image

    // Text per-frame VBO. Same append-only discipline as the rect
    // VBO: text_vert_count is where NEW glyph verts get written;
    // flush_text draws runs [text_first_undrawn_run..text_run_count)
    // then advances text_first_undrawn_run. Don't reset either
    // counter mid-frame for the same CPU-writes-stomp-recorded-GPU-reads
    // reason described on the rect VBO above.
    ID3D12Resource*             text_vbo[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    _d3d12_renderer_internal__tx_vertex_t* text_vbo_ptr[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    int64                       text_vert_count;
    _d3d12_renderer_internal__text_run_t text_runs[_D3D12_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int                         text_run_count;
    int                         text_first_undrawn_run;
    _d3d12_renderer_internal__tex_entry_t* current_text_atlas;

    // Image per-frame VBO. Image draws aren't batched into runs --
    // each submit_image call flushes rects + text first, writes 6
    // verts into this buffer, and issues its own draw. So we only
    // need ONE quad's worth of verts in this buffer per frame, but
    // since calls are sequential we reuse the 6-vertex slot for
    // each one's bounds update (the vertex position for one call
    // stays valid long enough for its own draw). Simplest: size it
    // for max N queued images per frame.
    ID3D12Resource*             image_vbo[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    _d3d12_renderer_internal__tx_vertex_t* image_vbo_ptr[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    int64                       image_vert_count; // reset each frame

    // Per-slot deferred-destroy ring. blur_region creates a transient
    // tex_entry, queues an image draw against it, and must NOT free
    // it inline -- the per-frame cmd list still references the SRV
    // and hasn't been submitted yet. Instead we stash the entry in
    // this slot's ring; when begin_frame returns to this slot N
    // frames later and waits on its fence, we know all those draws
    // are GPU-complete and the entries are safe to release.
    _d3d12_renderer_internal__tex_entry_t* pending_frees[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT][_D3D12_RENDERER_INTERNAL__MAX_PENDING_FREES];
    int                         pending_free_count[_D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT];
    // number of buffers in the swap chain: 2 for our own, whatever the host made for an adopted one
    UINT                        swap_buffer_count;
    // adopted: device, queue and swap chain belong to the host; no clear, no present, device and queue are not released
    boole                       adopted;
    // rtvs_dirty: the host resized its swap chain, rebuild the views before the next frame
    boole                       rtvs_dirty;
} _d3d12_renderer_internal__state_t;

static _d3d12_renderer_internal__state_t _d3d12_renderer_internal__g;
static boole _d3d12_renderer_internal__present_paced;

// ============================================================================
// HLSL shaders
// ============================================================================
// HLSL 5.1 (D3D12 requires at least 5.0; 5.1 is fine everywhere).
// Same math as the opengl3 / gles3 shaders, SM5.1 flavored.
// Viewport delivered via root 32-bit constants (cheaper than a
// CBV for 8 bytes; same role push constants play in the Vulkan
// backend).

#include "shaders/generated/rect.h"

// Text + image share one VS (pos + color + uv -> NDC + passthrough).
// Different pixel shaders: text samples R8 with gamma-correct
// reshape; image samples RGBA and multiplies by vertex color for
// tint. Root signature reserves slot 0 for viewport constants,
// slot 1 for a 1-SRV descriptor table, with one static sampler
// at s0 (linear, clamp).
#include "shaders/generated/text_image.h"

// ============================================================================
// forward decls
// ============================================================================

static boole _d3d12_renderer_internal__create_device_and_queue(void);
static boole _d3d12_renderer_internal__create_swapchain(HWND hwnd);
static boole _d3d12_renderer_internal__create_rtvs(void);
static boole _d3d12_renderer_internal__create_command_objects(void);
static boole _d3d12_renderer_internal__create_fence(void);
static boole _d3d12_renderer_internal__create_root_signature(void);
static boole _d3d12_renderer_internal__create_rect_pso(void);
static boole _d3d12_renderer_internal__create_rect_vertex_buffers(void);
static boole _d3d12_renderer_internal__create_srv_heap(void);
static boole _d3d12_renderer_internal__create_tx_root_and_pipelines(void);
static boole _d3d12_renderer_internal__create_tx_vertex_buffers(void);
static int   _d3d12_renderer_internal__alloc_srv_slot(void);
static void  _d3d12_renderer_internal__free_srv_slot(int slot);
static _d3d12_renderer_internal__tex_entry_t* _d3d12_renderer_internal__alloc_tex_entry(void);
static void  _d3d12_renderer_internal__release_tex_entry(_d3d12_renderer_internal__tex_entry_t* e);
static boole _d3d12_renderer_internal__upload_texture(_d3d12_renderer_internal__tex_entry_t* e, const mip_cpu__level_t* levels, int level_count, DXGI_FORMAT fmt, int bytes_per_pixel);
static void  _d3d12_renderer_internal__wait_for_fence(UINT64 value);
static void  _d3d12_renderer_internal__wait_for_gpu(void);
static void  _d3d12_renderer_internal__flush_rects(void);
static void  _d3d12_renderer_internal__capture_record(ID3D12GraphicsCommandList* list);
static void  _d3d12_renderer_internal__capture_finish(void);
static void  _d3d12_renderer_internal__flush_text(void);
static void  _d3d12_renderer_internal__bind_rect_pipeline(ID3D12GraphicsCommandList* list);
static void  _d3d12_renderer_internal__schedule_pending_free(_d3d12_renderer_internal__tex_entry_t* e);
static void  _d3d12_renderer_internal__drain_pending_frees(int slot);
static void  _d3d12_renderer_internal__rebind_frame_after_drain(void);

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
    _d3d12_renderer_internal__present_paced = FALSE;
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    stdlib__memset(s, 0, sizeof(*s));

    if (native_window == NULL_POINTER)
    {
        log_error("%s", "d3d12_renderer: NULL_POINTER native_window");
        return FALSE;
    }
    s->hwnd = (HWND)native_window;

    if (_d3d12_renderer_internal__create_device_and_queue() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_swapchain(s->hwnd) == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_rtvs() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_command_objects() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_fence() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_root_signature() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_rect_pso() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_rect_vertex_buffers() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_srv_heap() == FALSE)
    {
        return FALSE;
    }
    if (_d3d12_renderer_internal__create_tx_vertex_buffers() == FALSE)
    {
        return FALSE;
    }
    // Text + image root sig + pipelines are lazy-built on first use so
    // a PoC that never submits text (unit-test style) doesn't pay for
    // them. Flag is reset here so shutdown can tell if we need to
    // release them.
    s->tx_built = FALSE;

    s->backbuffer_index = VCALL0(s->swapchain, GetCurrentBackBufferIndex);

    log_info("%s %d %s %lld %s %lld %s", "d3d12_renderer: up (", (int)s->swap_buffer_count, "buffers,", (long long)s->viewport_w, "x", (long long)s->viewport_h, ")");
    return TRUE;
}

// ============================================================================
// PUBLIC: renderer__init_adopt_d3d12
// ============================================================================

/**
 * @brief initializes the renderer against a Direct3D 12 device, command queue and swap chain the host owns; meek-ui records its draws into its own command lists and executes them on the host's queue before the host presents
 *
 * @param ID3D12Device* device -> host-owned device, borrowed
 * @param ID3D12CommandQueue* queue -> host-owned direct command queue, borrowed
 * @param IDXGISwapChain* swapchain -> host-owned swap chain; IDXGISwapChain3 is queried from it and that one reference is released on shutdown
 *
 * @attention the back buffer must be in the PRESENT state when renderer__begin_frame runs, which is the case right before the host's Present; the host calls renderer__d3d12_release_render_targets before it resizes the swap chain
 *
 * @return boole -> TRUE on success
 */
boole renderer__init_adopt_d3d12(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapchain)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    DXGI_SWAP_CHAIN_DESC desc;
    HRESULT hr = 0;

    stdlib__memset(s, 0, sizeof(*s));

    if (device == NULL_POINTER || queue == NULL_POINTER || swapchain == NULL_POINTER)
    {
        log_error("%s", "renderer__init_adopt_d3d12: null device, queue or swapchain");
        return FALSE;
    }

    s->device  = device;
    s->queue   = queue;
    s->adopted = TRUE;

    hr = VCALL(swapchain, QueryInterface, &IID_IDXGISwapChain3, (void**)&s->swapchain);

    if (FAILED(hr) || s->swapchain == NULL_POINTER)
    {
        log_error("%s %08x", "renderer__init_adopt_d3d12: the swap chain does not expose IDXGISwapChain3: 0x", (unsigned)hr);
        stdlib__memset(s, 0, sizeof(*s));
        return FALSE;
    }

    stdlib__memset(&desc, 0, sizeof(desc));
    hr = VCALL(swapchain, GetDesc, &desc);

    if (FAILED(hr) || desc.BufferCount == 0 || desc.BufferCount > _D3D12_RENDERER_INTERNAL__MAX_SWAP_BUFFERS)
    {
        log_error("%s %u", "renderer__init_adopt_d3d12: unsupported swap chain buffer count", (unsigned)desc.BufferCount);
        VCALL0(s->swapchain, Release);
        stdlib__memset(s, 0, sizeof(*s));
        return FALSE;
    }

    s->swap_buffer_count = desc.BufferCount;
    s->viewport_w = (int64)desc.BufferDesc.Width;
    s->viewport_h = (int64)desc.BufferDesc.Height;

    if (s->viewport_w <= 0)
    {
        s->viewport_w = 1;
    }

    if (s->viewport_h <= 0)
    {
        s->viewport_h = 1;
    }

    if (_d3d12_renderer_internal__create_rtvs() == FALSE || _d3d12_renderer_internal__create_command_objects() == FALSE || _d3d12_renderer_internal__create_fence() == FALSE || _d3d12_renderer_internal__create_root_signature() == FALSE || _d3d12_renderer_internal__create_rect_pso() == FALSE || _d3d12_renderer_internal__create_rect_vertex_buffers() == FALSE || _d3d12_renderer_internal__create_srv_heap() == FALSE || _d3d12_renderer_internal__create_tx_vertex_buffers() == FALSE)
    {
        renderer__shutdown();
        return FALSE;
    }

    s->tx_built = FALSE;
    s->backbuffer_index = VCALL0(s->swapchain, GetCurrentBackBufferIndex);
    log_info("%s %d %s %lld %s %lld %s", "d3d12_renderer: adopted the host swap chain (", (int)s->swap_buffer_count, "buffers,", (long long)s->viewport_w, "x", (long long)s->viewport_h, ")");

    return TRUE;
}

/**
 * @brief drops every reference to the swap chain's buffers so the host can resize it; the views are rebuilt on the next renderer__begin_frame
 *
 * @attention waits for the GPU first, since in-flight command lists may still reference the buffers
 *
 * @return void
 */
void renderer__d3d12_release_render_targets(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    int i = 0;

    if (s->swapchain == NULL_POINTER)
    {
        return;
    }

    if (s->queue != NULL_POINTER && s->fence != NULL_POINTER)
    {
        _d3d12_renderer_internal__wait_for_gpu();
    }

    for (i = 0; i < _D3D12_RENDERER_INTERNAL__MAX_SWAP_BUFFERS; i++)
    {
        if (s->swap_buffers[i] != NULL_POINTER)
        {
            VCALL0(s->swap_buffers[i], Release);
            s->swap_buffers[i] = NULL_POINTER;
        }
    }

    if (s->rtv_heap != NULL_POINTER)
    {
        VCALL0(s->rtv_heap, Release);
        s->rtv_heap = NULL_POINTER;
    }

    s->rtvs_dirty = TRUE;
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
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    if (s->queue != NULL_POINTER && s->fence != NULL_POINTER)
    {
        // Make sure the GPU is done touching everything before we
        // start releasing. D3D12 resources are refcounted but
        // releasing an in-use resource blows up the debug layer.
        _d3d12_renderer_internal__wait_for_gpu();
    }

    // Drain any deferred-destroy entries that didn't see a fence
    // wait yet. wait_for_gpu above guarantees the GPU is idle, so
    // releasing now is safe -- belt-and-braces in case host code
    // tears down mid-frame.
    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        _d3d12_renderer_internal__drain_pending_frees(i);
    }

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        if (s->rect_vbo_ptr[i] != NULL_POINTER && s->rect_vbo[i] != NULL_POINTER)
        {
            VCALL(s->rect_vbo[i], Unmap, 0, NULL_POINTER);
            s->rect_vbo_ptr[i] = NULL_POINTER;
        }
        if (s->rect_vbo[i]    != NULL_POINTER)
        {
            VCALL0(s->rect_vbo[i], Release);
            s->rect_vbo[i]    = NULL_POINTER;
        }
        if (s->text_vbo_ptr[i] != NULL_POINTER && s->text_vbo[i] != NULL_POINTER)
        {
            VCALL(s->text_vbo[i], Unmap, 0, NULL_POINTER);
            s->text_vbo_ptr[i] = NULL_POINTER;
        }
        if (s->text_vbo[i]    != NULL_POINTER)
        {
            VCALL0(s->text_vbo[i], Release);
            s->text_vbo[i]    = NULL_POINTER;
        }
        if (s->image_vbo_ptr[i] != NULL_POINTER && s->image_vbo[i] != NULL_POINTER)
        {
            VCALL(s->image_vbo[i], Unmap, 0, NULL_POINTER);
            s->image_vbo_ptr[i] = NULL_POINTER;
        }
        if (s->image_vbo[i]   != NULL_POINTER)
        {
            VCALL0(s->image_vbo[i], Release);
            s->image_vbo[i]   = NULL_POINTER;
        }
        if (s->cmd_lists[i]   != NULL_POINTER)
        {
            VCALL0(s->cmd_lists[i], Release);
            s->cmd_lists[i]   = NULL_POINTER;
        }
        if (s->allocators[i]  != NULL_POINTER)
        {
            VCALL0(s->allocators[i], Release);
            s->allocators[i]  = NULL_POINTER;
        }
    }

    // Release any still-live atlas / image textures. font__shutdown and
    // widget_image__cache_shutdown run BEFORE renderer__shutdown per
    // the platform contract, so the pool should normally be empty
    // here -- this loop is defense against host code that didn't
    // play by the rules.
    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__MAX_SRVS; i++)
    {
        _d3d12_renderer_internal__tex_entry_t* e = &s->tex_pool[i];
        if (e->in_use == FALSE)
        {
            continue;
        }
        if (e->resource != NULL_POINTER)
        {
            VCALL0(e->resource, Release);
            e->resource = NULL_POINTER;
        }
        e->in_use = FALSE;
    }

    if (s->text_pso    != NULL_POINTER)
    {
        VCALL0(s->text_pso, Release);
        s->text_pso    = NULL_POINTER;
    }
    if (s->image_pso   != NULL_POINTER)
    {
        VCALL0(s->image_pso, Release);
        s->image_pso   = NULL_POINTER;
    }
    if (s->tx_root_sig != NULL_POINTER)
    {
        VCALL0(s->tx_root_sig, Release);
        s->tx_root_sig = NULL_POINTER;
    }
    if (s->srv_heap    != NULL_POINTER)
    {
        VCALL0(s->srv_heap, Release);
        s->srv_heap    = NULL_POINTER;
    }

    if (s->rect_pso   != NULL_POINTER)
    {
        VCALL0(s->rect_pso, Release);
        s->rect_pso   = NULL_POINTER;
    }
    if (s->root_sig   != NULL_POINTER)
    {
        VCALL0(s->root_sig, Release);
        s->root_sig   = NULL_POINTER;
    }

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__MAX_SWAP_BUFFERS; i++)
    {
        if (s->swap_buffers[i] != NULL_POINTER)
        {
            VCALL0(s->swap_buffers[i], Release);
            s->swap_buffers[i] = NULL_POINTER;
        }
    }

    if (s->rtv_heap   != NULL_POINTER)
    {
        VCALL0(s->rtv_heap, Release);
        s->rtv_heap   = NULL_POINTER;
    }
    if (s->swapchain  != NULL_POINTER)
    {
        VCALL0(s->swapchain, Release);
        s->swapchain  = NULL_POINTER;
    }
    // the swap chain reference above is ours in both modes (QueryInterface); queue and device are borrowed when adopted
    if (s->adopted == TRUE)
    {
        s->queue  = NULL_POINTER;
        s->device = NULL_POINTER;
    }
    if (s->queue      != NULL_POINTER)
    {
        VCALL0(s->queue, Release);
        s->queue      = NULL_POINTER;
    }
    if (s->device     != NULL_POINTER)
    {
        VCALL0(s->device, Release);
        s->device     = NULL_POINTER;
    }
    if (s->fence      != NULL_POINTER)
    {
        VCALL0(s->fence, Release);
        s->fence      = NULL_POINTER;
    }
    if (s->fence_event != NULL_POINTER)
    {
        CloseHandle(s->fence_event);
        s->fence_event = NULL_POINTER;
    }

    stdlib__memset(s, 0, sizeof(*s));
}

// ============================================================================
// PUBLIC: begin_frame / submit_rect / end_frame
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
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    DXGI_SWAP_CHAIN_DESC description;
    HRESULT result = S_OK;

    stdlib__memset(&description, 0, sizeof(description));
    s->frame_active = FALSE;
    if (s->swapchain == NULL_POINTER || viewport_w <= 0 || viewport_h <= 0)
    {
        return;
    }
    if (s->adopted == FALSE && (viewport_w != s->viewport_w || viewport_h != s->viewport_h))
    {
        result = VCALL(s->swapchain, GetDesc, &description);
        if (FAILED(result))
        {
            return;
        }
        renderer__d3d12_release_render_targets();
        result = VCALL(s->swapchain, ResizeBuffers, 0, (UINT)viewport_w, (UINT)viewport_h, DXGI_FORMAT_UNKNOWN, description.Flags);
        if (FAILED(result))
        {
            log_error("%s %08lX", "d3d12 swap chain resize failed, hresult", (unsigned long)result);
            return;
        }
    }

    s->viewport_w             = viewport_w;
    s->viewport_h             = viewport_h;
    s->clear_color            = clear;
    s->rect_vert_count        = 0;
    s->rect_draw_cursor       = 0;
    s->text_vert_count        = 0;
    s->text_run_count         = 0;
    s->text_first_undrawn_run = 0;
    s->image_vert_count       = 0;

    // Wait for the GPU to be done with the slot we're about to
    // reuse. fence_values[slot] was set in end_frame when that
    // slot's cmd list was submitted; we block here if the GPU
    // hasn't reached it yet.
    UINT slot = s->frame_slot;

    // the host resized its swap chain since the last frame: views first
    if (s->rtvs_dirty == TRUE)
    {
        if (_d3d12_renderer_internal__create_rtvs() == FALSE)
        {
            return;
        }
        s->rtvs_dirty = FALSE;
    }

    _d3d12_renderer_internal__wait_for_fence(s->fence_values[slot]);

    // Slot's previous frame is GPU-complete: any tex_entry queued by
    // last cycle's blur_region into this slot's pending-free ring is
    // now safe to release. Drain before recording the new frame.
    _d3d12_renderer_internal__drain_pending_frees((int)slot);

    ID3D12CommandAllocator*    alloc = s->allocators[slot];
    ID3D12GraphicsCommandList* list  = s->cmd_lists[slot];

    // Reset allocator + list before recording new commands for
    // this frame. Both must be idle (GPU done) before Reset.
    VCALL0(alloc, Reset);
    VCALL(list, Reset, alloc, s->rect_pso);

    s->backbuffer_index = VCALL0(s->swapchain, GetCurrentBackBufferIndex);

    // Transition: PRESENT -> RENDER_TARGET for the current backbuffer.
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = s->swap_buffers[s->backbuffer_index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    VCALL(list, ResourceBarrier, 1, &barrier);

    // Bind the current backbuffer RTV and clear it.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->swap_rtvs[s->backbuffer_index];
    VCALL(list, OMSetRenderTargets, 1, &rtv, FALSE, NULL_POINTER);

    float clear_col[4] = { clear.r, clear.g, clear.b, clear.a };

    // the host's frame is already in the back buffer when adopted
    if (s->adopted == FALSE)
    {
        VCALL(list, ClearRenderTargetView, rtv, clear_col, 0, NULL_POINTER);
    }

    // Static pipeline state for this frame: root sig + rect PSO
    // bound (already bound via Reset(pso)), viewport + default
    // scissor set to the full backbuffer.
    VCALL(list, SetGraphicsRootSignature, s->root_sig);

    D3D12_VIEWPORT vp = { 0 };
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = (float)viewport_w;
    vp.Height   = (float)viewport_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    VCALL(list, RSSetViewports, 1, &vp);

    D3D12_RECT sc = { 0 };
    sc.left   = 0;
    sc.top    = 0;
    sc.right  = (LONG)viewport_w;
    sc.bottom = (LONG)viewport_h;
    VCALL(list, RSSetScissorRects, 1, &sc);

    // Push the viewport into the root 32-bit constants for the VS.
    float pc[4] = { (float)viewport_w, (float)viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);

    s->frame_active = TRUE;
}

// Push one vertex into the rect VBO. Factored out so submit_rect and
// submit_rect_gradient can share it -- the only thing they disagree on
// is whether all six corners get the same color or four distinct ones.
static void _d3d12_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    _d3d12_renderer_internal__vertex_t* v = &s->rect_vbo_ptr[s->frame_slot][s->rect_vert_count];
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
    s->rect_vert_count++;
}

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
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    // Buffer-full check: drop the rect and log once per frame. We
    // can't flush-and-reuse because the UPLOAD buffer is persistent-
    // mapped and the already-recorded DrawInstanced commands would
    // re-read the lower offsets as we overwrite them. Bump
    // MAX_QUADS if a real UI hits this.
    if (s->rect_vert_count + _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD > _D3D12_RENDERER_INTERNAL__MAX_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    float half_min = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius > half_min)
    {
        radius = half_min;
    }
    if (radius < 0.0f)
    {
        radius = 0.0f;
    }

    float x0 = r.x,         y0 = r.y;
    float x1 = r.x + r.w,   y1 = r.y + r.h;

    // Two triangles: (tl, tr, bl) + (tr, br, bl). Vertex ordering
    // matches every other backend's per-VISUAL-CONTRACT layout.
    _d3d12_renderer_internal__push_vertex(x0, y0, 0.0f, 0.0f, r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x1, y1, r.w,  r.h,  r.w, r.h, radius, c);
    _d3d12_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
}

static void _d3d12_renderer_internal__flush_rects(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }

    int64 pending = s->rect_vert_count - s->rect_draw_cursor;
    if (pending <= 0)
    {
        return;
    }

    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    // the text flush leaves the text root signature and PSO bound, and a
    // paint-order flush mid-frame (scene_render's z boundaries) sends more
    // rects after it; without rebinding, those rects went through the
    // text pipeline and never showed (every positioned box of the
    // 22-position scene was missing on d3d12)
    _d3d12_renderer_internal__bind_rect_pipeline(list);

    // Draw the pending slice [rect_draw_cursor .. rect_vert_count).
    // VBV points at the start of the buffer; StartVertexLocation in
    // DrawInstanced offsets the draw inside it. Critically, we do
    // NOT reset rect_vert_count here -- the command list still
    // references these verts by offset, so subsequent CPU writes
    // must go to a HIGHER offset to avoid stomping. Cursor advances
    // to vert_count; next flush will pick up from there.
    D3D12_VERTEX_BUFFER_VIEW vbv = { 0 };
    vbv.BufferLocation = VCALL0(s->rect_vbo[s->frame_slot], GetGPUVirtualAddress);
    vbv.SizeInBytes    = (UINT)(sizeof(_d3d12_renderer_internal__vertex_t) * s->rect_vert_count);
    vbv.StrideInBytes  = sizeof(_d3d12_renderer_internal__vertex_t);
    VCALL(list, IASetVertexBuffers, 0, 1, &vbv);
    VCALL(list, IASetPrimitiveTopology, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    VCALL(list, DrawInstanced, (UINT)pending, 1, (UINT)s->rect_draw_cursor, 0);

    s->rect_draw_cursor = s->rect_vert_count;
}

/**
 * @brief drain the pending rect + text batches mid-frame in the correct order (rects first, then text-on-top), WITHOUT changing scissor state or presenting. After this call the GPU has painted everything submitted so far, and subsequent submissions start a fresh batch that will draw ON TOP of what's already on the back buffer.
 *
 * @return void
 */
void renderer__flush_pending_draws(void)
{
    // Public flush so scene_render can break the per-frame batch at
    // z-index sibling boundaries. See renderer.h for the full reason.
    // Rects first, text second -- same ordering as end_frame.
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    _d3d12_renderer_internal__flush_rects();
    _d3d12_renderer_internal__flush_text();
}

/**
 * @brief finalize the frame: upload the batched verts, issue the draw, then present the back buffer to the window (swap buffers / swapchain Present). After this call returns, the frame is on screen.
 *
 * @return void
 */
void renderer__end_frame(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }

    _d3d12_renderer_internal__flush_rects();
    _d3d12_renderer_internal__flush_text();

    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    // an armed capture copies the finished frame into a readback buffer
    // here, still in the RENDER_TARGET state; it is mapped after the
    // fence below
    _d3d12_renderer_internal__capture_record(list);

    // Transition: RENDER_TARGET -> PRESENT before Close.
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = s->swap_buffers[s->backbuffer_index];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    VCALL(list, ResourceBarrier, 1, &barrier);

    VCALL0(list, Close);

    ID3D12CommandList* submit_lists[1] = { (ID3D12CommandList*)list };
    VCALL(s->queue, ExecuteCommandLists, 1, submit_lists);

    // Present -- interval 1 = vsync, same semantics as d3d11 /
    // eglSwapBuffers / SwapBuffers.
    if (s->adopted == FALSE)
    {
        HRESULT pr = VCALL(s->swapchain, Present, 1, 0);
        _d3d12_renderer_internal__present_paced = (boole)(pr == S_OK);
        if (FAILED(pr))
        {
            log_warn("%s %08x", "IDXGISwapChain3::Present failed: 0x", (unsigned)pr);
        }
    }

    // Signal the fence after present so the next wait_for_fence
    // for this slot (on begin_frame in two frames' time) can
    // guarantee the GPU has completed all its work.
    s->next_fence_value++;
    VCALL(s->queue, Signal, s->fence, s->next_fence_value);
    s->fence_values[s->frame_slot] = s->next_fence_value;

    _d3d12_renderer_internal__capture_finish();

    s->frame_active = FALSE;
    s->frame_slot   = (s->frame_slot + 1) % _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT;
}

// ===== frame capture state (see renderer.h) ================================
static ubyte* _d3d12_renderer_internal__capture_out  = NULL_POINTER;
static int64  _d3d12_renderer_internal__capture_cap  = 0;
static boole  _d3d12_renderer_internal__capture_done = FALSE;

// The armed frame capture, first half: the back buffer copied into a
// READBACK-heap buffer (256-byte aligned rows) while the command list is
// still open, with the RENDER_TARGET / COPY_SOURCE transitions around the
// copy. Same mechanics as renderer__blur_region's readback.
static ID3D12Resource* _d3d12_renderer_internal__capture_readback = NULL_POINTER;
static UINT            _d3d12_renderer_internal__capture_pitch    = 0;

static void _d3d12_renderer_internal__capture_record(ID3D12GraphicsCommandList* list)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    int64 w = (int64)s->viewport_w;
    int64 h = (int64)s->viewport_h;
    UINT   row_pitch = 0;
    UINT64 buffer_sz = 0;
    D3D12_HEAP_PROPERTIES hp = { 0 };
    D3D12_RESOURCE_DESC bd = { 0 };
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    D3D12_TEXTURE_COPY_LOCATION src_loc = { 0 };
    D3D12_TEXTURE_COPY_LOCATION dst_loc = { 0 };
    ID3D12Resource* backbuffer = NULL_POINTER;
    HRESULT hr = 0;

    if (_d3d12_renderer_internal__capture_out == NULL_POINTER || w <= 0 || h <= 0 || _d3d12_renderer_internal__capture_cap < w * h * 4)
    {
        return;
    }

    row_pitch = ((UINT)w * 4u + 255u) & ~255u;
    buffer_sz = (UINT64)row_pitch * (UINT64)h;

    hp.Type             = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = buffer_sz;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    hr = VCALL(s->device, CreateCommittedResource, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL_POINTER, &IID_ID3D12Resource, (void**)&_d3d12_renderer_internal__capture_readback);

    if (FAILED(hr) || _d3d12_renderer_internal__capture_readback == NULL_POINTER)
    {
        _d3d12_renderer_internal__capture_readback = NULL_POINTER;
        _d3d12_renderer_internal__capture_out = NULL_POINTER;
        return;
    }

    _d3d12_renderer_internal__capture_pitch = row_pitch;
    backbuffer = s->swap_buffers[s->backbuffer_index];

    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    VCALL(list, ResourceBarrier, 1, &barrier);

    src_loc.pResource        = backbuffer;
    src_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    dst_loc.pResource                          = _d3d12_renderer_internal__capture_readback;
    dst_loc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint.Offset             = 0;
    dst_loc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst_loc.PlacedFootprint.Footprint.Width    = (UINT)w;
    dst_loc.PlacedFootprint.Footprint.Height   = (UINT)h;
    dst_loc.PlacedFootprint.Footprint.Depth    = 1;
    dst_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;
    VCALL(list, CopyTextureRegion, &dst_loc, 0, 0, 0, &src_loc, NULL_POINTER);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    VCALL(list, ResourceBarrier, 1, &barrier);
}

// Second half, after the frame's fence was signaled: wait for it, map the
// readback buffer, copy the rows out, release it.
static void _d3d12_renderer_internal__capture_finish(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    int64 w = (int64)s->viewport_w;
    int64 h = (int64)s->viewport_h;
    D3D12_RANGE read_range  = { 0, 0 };
    D3D12_RANGE write_range = { 0, 0 };
    void* mapped = NULL_POINTER;
    int64 y = 0;

    if (_d3d12_renderer_internal__capture_readback == NULL_POINTER)
    {
        return;
    }

    _d3d12_renderer_internal__wait_for_fence(s->next_fence_value);
    read_range.End = (SIZE_T)_d3d12_renderer_internal__capture_pitch * (SIZE_T)h;

    if (_d3d12_renderer_internal__capture_out != NULL_POINTER && SUCCEEDED(VCALL(_d3d12_renderer_internal__capture_readback, Map, 0, &read_range, &mapped)) && mapped != NULL_POINTER)
    {
        for (y = 0; y < h; y++)
        {
            stdlib__memcpy(_d3d12_renderer_internal__capture_out + (size_t)y * (size_t)w * 4, (const ubyte*)mapped + (size_t)y * (size_t)_d3d12_renderer_internal__capture_pitch, (size_t)w * 4);
        }

        VCALL(_d3d12_renderer_internal__capture_readback, Unmap, 0, &write_range);
        _d3d12_renderer_internal__capture_done = TRUE;
    }

    VCALL0(_d3d12_renderer_internal__capture_readback, Release);
    _d3d12_renderer_internal__capture_readback = NULL_POINTER;
    _d3d12_renderer_internal__capture_out = NULL_POINTER;
}

// ============================================================================
// text / image pipelines -- atlas upload, glyph submit, texture draw
// ============================================================================

/**
 * @brief upload an alpha-only bitmap to the GPU as a texture. Called by font.c when rasterizing a new (family, size) pair.
 *
 * @param const ubyte* pixels -> Row-major R8 bitmap (one byte per pixel, interpreted as alpha).
 * @param width Bitmap width in pixels.
 * @param height Bitmap height in pixels.
 *
 * @return void* -> Opaque backend-specific texture handle (GLuint / ID3D11ShaderResourceView* / IDirect3DTexture9*), or NULL on failure.
 */
void* renderer__create_atlas_r8(const ubyte* pixels, int w, int h)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (pixels == NULL_POINTER || w <= 0 || h <= 0)
    {
        return NULL_POINTER;
    }
    // Text + image root sig / pipelines are built lazily on the first
    // atlas or texture request. Doing it here (rather than in init)
    // means a PoC that never uses text avoids the D3DCompile cost.
    if (s->tx_built == FALSE)
    {
        if (_d3d12_renderer_internal__create_tx_root_and_pipelines() == FALSE)
        {
            return NULL_POINTER;
        }
        s->tx_built = TRUE;
    }
    _d3d12_renderer_internal__tex_entry_t* e = _d3d12_renderer_internal__alloc_tex_entry();
    if (e == NULL_POINTER)
    {
        log_error("%s", "d3d12 atlas: pool full");
        return NULL_POINTER;
    }
    mip_cpu__level_t atlas_level;
    atlas_level.pixels = pixels;
    atlas_level.w      = w;
    atlas_level.h      = h;
    if (_d3d12_renderer_internal__upload_texture(e, &atlas_level, 1, DXGI_FORMAT_R8_UNORM, 1) == FALSE)
    {
        _d3d12_renderer_internal__release_tex_entry(e);
        return NULL_POINTER;
    }
    return (void*)e;
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
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (atlas == NULL_POINTER)
    {
        return;
    }
    _d3d12_renderer_internal__tex_entry_t* e = (_d3d12_renderer_internal__tex_entry_t*)atlas;
    // Wait for any in-flight frame that might still reference this
    // atlas via a recorded command list. Textures in this renderer
    // live for the whole process under normal use (font__shutdown is
    // the one caller), so blocking here is cheap -- better than
    // risking a use-after-free mid-flight.
    _d3d12_renderer_internal__wait_for_gpu();
    if (s->current_text_atlas == e)
    {
        s->current_text_atlas = NULL_POINTER;
    }
    _d3d12_renderer_internal__release_tex_entry(e);
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
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    s->current_text_atlas = (_d3d12_renderer_internal__tex_entry_t*)atlas;
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
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    if (s->current_text_atlas == NULL_POINTER)
    {
        return;
    }
    if (s->text_vert_count + _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD > _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    // Open or extend a run keyed by the current atlas. end_frame walks
    // the run list, binds each run's SRV once, draws that run's vert
    // range. Matches d3d11's text-run batching.
    // Critical: we may only extend the LAST run if it has NOT been
    // flushed yet. Mid-frame flushes (push_scissor, submit_image)
    // advance text_first_undrawn_run past already-drawn runs; if we
    // extended one of those, the new glyphs would live inside its
    // recorded [vert_start..vert_start+vert_count) range but no
    // subsequent DrawInstanced would cover them -- first_undrawn is
    // already past this run, so flush_text wouldn't re-draw it.
    // Bug surfaced as "popup text invisible" because the popup paints
    // after the main tree's scroll pop_scissor flushed text, sharing
    // the same atlas with the main tree.
    _d3d12_renderer_internal__text_run_t* run = NULL_POINTER;
    if (s->text_run_count > s->text_first_undrawn_run)
    {
        run = &s->text_runs[s->text_run_count - 1];
        if (run->atlas != s->current_text_atlas)
        {
            run = NULL_POINTER;
        }
    }
    if (run == NULL_POINTER)
    {
        if (s->text_run_count >= _D3D12_RENDERER_INTERNAL__MAX_TEXT_RUNS)
        {
            return;
        }
        run = &s->text_runs[s->text_run_count++];
        run->atlas      = s->current_text_atlas;
        run->vert_start = s->text_vert_count;
        run->vert_count = 0;
    }

    float x0 = rect.x, y0 = rect.y;
    float x1 = rect.x + rect.w, y1 = rect.y + rect.h;
    float u0 = uv.x, v0 = uv.y;
    float u1 = uv.x + uv.w, v1 = uv.y + uv.h;

    _d3d12_renderer_internal__tx_vertex_t* v = &s->text_vbo_ptr[s->frame_slot][s->text_vert_count];
    #define _PUSH_TX(IDX, X, Y, U, V) do { \
        v[IDX].x = (X); v[IDX].y = (Y); \
        v[IDX].r = color.r; v[IDX].g = color.g; v[IDX].b = color.b; v[IDX].a = color.a; \
        v[IDX].u = (U); v[IDX].v = (V); \
    } while (0)
    _PUSH_TX(0, x0, y0, u0, v0);
    _PUSH_TX(1, x1, y0, u1, v0);
    _PUSH_TX(2, x0, y1, u0, v1);
    _PUSH_TX(3, x1, y0, u1, v0);
    _PUSH_TX(4, x1, y1, u1, v1);
    _PUSH_TX(5, x0, y1, u0, v1);
    #undef _PUSH_TX

    s->text_vert_count += _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD;
    run->vert_count    += _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD;
}

/**
 * @brief upload an RGBA8 image to the GPU. Bytes are consumed top-to-bottom, left-to-right, 4 bytes per pixel, no row padding (tightly packed). Caller retains ownership of the `rgba` buffer; the renderer copies the data into a texture and no longer references the source pointer after returning.
 *
 * @param const ubyte* rgba -> Tightly-packed RGBA8 pixel buffer, length = width*height*4.
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 *
 * @return void* -> Opaque texture handle, or NULL if upload failed or the backend doesn't yet implement this pipeline.
 */
static void* _d3d12_renderer_internal__create_texture_rgba(const ubyte* rgba, int w, int h, boole generate_mips)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (rgba == NULL_POINTER || w <= 0 || h <= 0)
    {
        return NULL_POINTER;
    }
    if (s->tx_built == FALSE)
    {
        if (_d3d12_renderer_internal__create_tx_root_and_pipelines() == FALSE)
        {
            return NULL_POINTER;
        }
        s->tx_built = TRUE;
    }
    _d3d12_renderer_internal__tex_entry_t* e = _d3d12_renderer_internal__alloc_tex_entry();
    if (e == NULL_POINTER)
    {
        log_error("%s", "d3d12 image: pool full");
        return NULL_POINTER;
    }
    // the mip chain is built on the CPU by the shared helper so every
    // backend samples the same texels (see _mip_cpu.h); this backend had
    // no chain at all before, and photo-sized sources drawn as 48 px
    // tiles twinkle under plain bilinear minification.
    mip_cpu__chain_t chain;
    ubyte*           storage       = NULL_POINTER;
    size_t           storage_bytes = generate_mips == TRUE ? mip_cpu__storage_bytes(w, h) : 0;
    boole            uploaded      = FALSE;

    if (storage_bytes > 0)
    {
        storage = (ubyte*)GUI_MALLOC_T(storage_bytes, MM_TYPE__RENDERER);
    }

    mip_cpu__build(&chain, rgba, w, h, storage);
    uploaded = _d3d12_renderer_internal__upload_texture(e, chain.levels, chain.count, DXGI_FORMAT_R8G8B8A8_UNORM, 4);

    if (storage != NULL_POINTER)
    {
        GUI_FREE(storage);
    }

    if (uploaded == FALSE)
    {
        _d3d12_renderer_internal__release_tex_entry(e);
        return NULL_POINTER;
    }
    return (void*)e;
}

// Public image uploads retain mipmaps for images that can be drawn smaller.
void* renderer__create_texture_rgba(const ubyte* rgba, int w, int h)
{
    return _d3d12_renderer_internal__create_texture_rgba(rgba, w, h, TRUE);
}

// The compositor's final surface is displayed at one texel per viewport pixel.
// Its lower mip levels would be rebuilt and uploaded every frame but never used.
GUI_API void* renderer_backend__create_frame_texture_rgba(const ubyte* rgba, int width, int height)
{
    return _d3d12_renderer_internal__create_texture_rgba(rgba, width, height, FALSE);
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
    _d3d12_renderer_internal__tex_entry_t* e = (_d3d12_renderer_internal__tex_entry_t*)tex;
    _d3d12_renderer_internal__wait_for_gpu();
    _d3d12_renderer_internal__release_tex_entry(e);
}

/**
 * @brief draw one image quad immediately. Flushes any pending solid-rect or text batch first so previously-submitted content stays underneath and subsequent submissions stay on top. Full-texture draw (UV 0..1 on both axes); no subregion support in this pass (crop via node bounds + scissor instead).
 *
 * @param rect Destination rect in top-left-origin pixel coords.
 * @param void* tex -> Texture handle from create_texture_rgba. NULL = skip draw.
 * @param gui_color_t tint -> Multiplied against the sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 * @param float u0 -> left texture coordinate of the sub-rect to sample
 * @param float v0 -> top texture coordinate of the sub-rect to sample
 * @param float u1 -> right texture coordinate of the sub-rect to sample
 * @param float v1 -> bottom texture coordinate of the sub-rect to sample
 *
 * @return void
 */
void renderer__submit_image_uv(gui_rect_t r, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    if (tex == NULL_POINTER)
    {
        // Placeholder: let scene's fitted-image fallback handle it.
        // Caller passes a tinted gradient rect in that case.
        return;
    }
    _d3d12_renderer_internal__tex_entry_t* e = (_d3d12_renderer_internal__tex_entry_t*)tex;

    // Flush pending rects + text first so this image draws on top
    // of anything queued before it but under anything queued after.
    // Matches the submit-order = render-order invariant.
    _d3d12_renderer_internal__flush_rects();
    _d3d12_renderer_internal__flush_text();

    // Write 6 verts into the per-frame image VBO.
    if (s->image_vert_count + _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD > _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }
    int64 vstart = s->image_vert_count;
    _d3d12_renderer_internal__tx_vertex_t* v = &s->image_vbo_ptr[s->frame_slot][vstart];
    float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    #define _PUSH_IMG(IDX, X, Y, U, V) do { \
        v[IDX].x = (X); v[IDX].y = (Y); \
        v[IDX].r = tint.r; v[IDX].g = tint.g; v[IDX].b = tint.b; v[IDX].a = tint.a; \
        v[IDX].u = (U); v[IDX].v = (V); \
    } while (0)
    _PUSH_IMG(0, x0, y0, u0, v0);
    _PUSH_IMG(1, x1, y0, u1, v0);
    _PUSH_IMG(2, x0, y1, u0, v1);
    _PUSH_IMG(3, x1, y0, u1, v0);
    _PUSH_IMG(4, x1, y1, u1, v1);
    _PUSH_IMG(5, x0, y1, u0, v1);
    #undef _PUSH_IMG
    s->image_vert_count += _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD;

    // Bind image pipeline + root sig + descriptor heap + SRV slot,
    // push viewport, issue draw for these 6 verts. Leaves state
    // such that the next flush_rects re-binds the rect pipeline.
    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    VCALL(list, SetGraphicsRootSignature, s->tx_root_sig);
    ID3D12DescriptorHeap* heaps[1] = { s->srv_heap };
    VCALL(list, SetDescriptorHeaps, 1, heaps);
    VCALL(list, SetGraphicsRootDescriptorTable, 1, e->srv_gpu);

    float pc[4] = { (float)s->viewport_w, (float)s->viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);

    VCALL(list, SetPipelineState, s->image_pso);

    D3D12_VERTEX_BUFFER_VIEW vbv = { 0 };
    vbv.BufferLocation = VCALL0(s->image_vbo[s->frame_slot], GetGPUVirtualAddress);
    vbv.BufferLocation += (UINT64)(sizeof(_d3d12_renderer_internal__tx_vertex_t) * vstart);
    vbv.SizeInBytes    = (UINT)(sizeof(_d3d12_renderer_internal__tx_vertex_t) * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD);
    vbv.StrideInBytes  = sizeof(_d3d12_renderer_internal__tx_vertex_t);
    VCALL(list, IASetVertexBuffers, 0, 1, &vbv);
    VCALL(list, IASetPrimitiveTopology, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    VCALL(list, DrawInstanced, _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD, 1, 0, 0);

    // Re-bind the rect root sig + PSO so subsequent submit_rect calls
    // don't need to do any work besides appending verts. Mirrors how
    // the d3d11 path leaves image state such that the next rect
    // flush rebinds cleanly via flush_batches.
    _d3d12_renderer_internal__bind_rect_pipeline(list);
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

// Shadow: concentric layered rects with quadratic falloff. The rect
// pipeline's per-fragment SDF AA handles the soft edge; no special
// shader needed. Layer count scales with blur radius. Same algorithm
// as d3d11 / opengl3 / gles3.
/**
 * @brief queue a shadow splat for the current frame. Same rect convention as submit_rect. `blur` is the feather width in pixels. Caller supplies the final offset rect (rect.x already includes dx, rect.y includes dy) so the renderer doesn't have to know about the originating shape.
 *
 * @param rect Shadow bounding rect (already offset).
 * @param gui_color_t c -> Shadow color (alpha controls strength).
 * @param float radius -> Same corner radius as the decorated shape.
 * @param float blur -> Soft-edge width in pixels. 0 = hard edge.
 *
 * @return void
 */
void renderer__submit_shadow(gui_rect_t r, gui_color_t c, float radius, float blur)
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
        float grow    = step * (float)i;
        float t       = (float)i / (float)(layers - 1);
        float falloff = (1.0f - t) * (1.0f - t);
        gui_color_t layer_c = c;
        layer_c.a = c.a * falloff / (float)layers;
        gui_rect_t lr;
        lr.x = r.x - grow;
        lr.y = r.y - grow;
        lr.w = r.w + 2.0f * grow;
        lr.h = r.h + 2.0f * grow;
        renderer__submit_rect(lr, layer_c, radius + grow);
    }
}

// Gradient: per-corner color on the existing SDF-rect pipeline. The
// VS passes `color` through and the PS interpolates across the triangle,
// so two-color lerps cost nothing extra on the GPU -- just different
// corner colors fed through push_vertex. Direction maps onto which
// corners get `from` vs `to`:
// VERTICAL       from at top row, to at bottom row.
// HORIZONTAL     from at left column, to at right column.
// DIAGONAL_TL    from at top-left corner, to at bottom-right; the
// other two corners get the midpoint so the interp
// follows the correct axis rather than a triangle-
// induced diagonal.
// DIAGONAL_TR    mirror of the above.
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
    return (boole)(_d3d12_renderer_internal__g.adopted == FALSE && _d3d12_renderer_internal__present_paced);
}

void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color)
{
    gui_vec2_t vertices[3] = {a, b, c};
    float left = stdlib__fminf(a.x, stdlib__fminf(b.x, c.x)) - 1;
    float top = stdlib__fminf(a.y, stdlib__fminf(b.y, c.y)) - 1;
    float width = stdlib__fmaxf(a.x, stdlib__fmaxf(b.x, c.x)) - left + 1;
    float height = stdlib__fmaxf(a.y, stdlib__fmaxf(b.y, c.y)) - top + 1;
    int i;
    if (_d3d12_renderer_internal__g.frame_active == FALSE || _d3d12_renderer_internal__g.rect_vert_count + 3 > _D3D12_RENDERER_INTERNAL__MAX_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD) return;
    // Expand the rectangle mask past the geometry. Triangle rasterization
    // owns the edges; the existing colour pipeline needs no extra shader.
    for (i = 0; i < 3; i++)
        _d3d12_renderer_internal__push_vertex(vertices[i].x, vertices[i].y,
            vertices[i].x - left, vertices[i].y - top, width, height, 0, color);
}

void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    int64 count = 0;
    int64 i     = 0;
    float max_r = 0.0f;

    if (s->frame_active == FALSE)
    {
        return;
    }

    count = gradient__tessellate(g, r, scale, opacity, verts, GRADIENT_MAX_VERTS);

    if (count <= 0 || s->rect_vert_count + count > _D3D12_RENDERER_INTERNAL__MAX_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD)
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
        _d3d12_renderer_internal__push_vertex(verts[i].x, verts[i].y, verts[i].x - r.x, verts[i].y - r.y, r.w, r.h, radius, verts[i].c);
    }
}

// ============================================================================
// scissor
// ============================================================================

/**
 * @brief push a clip rect onto the scissor stack. Subsequent submissions are clipped to the intersection of every rect currently on the stack.
 *
 * @param gui_rect_t r -> Clip region in pixel coords (top-left origin).
 *
 * @return void
 */
void renderer__push_scissor(gui_rect_t r)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->scissor_depth >= _D3D12_RENDERER_INTERNAL__MAX_SCISSORS)
    {
        return;
    }

    // Flush both rect and text batches before the scissor state
    // change so anything already queued draws under the OLD scissor.
    // flush_text restores text state; we re-bind rect afterwards so
    // subsequent submit_rect flushes don't see stale tx pipeline.
    _d3d12_renderer_internal__flush_rects();
    if (s->text_run_count > 0)
    {
        _d3d12_renderer_internal__flush_text();
        if (s->frame_active)
        {
            _d3d12_renderer_internal__bind_rect_pipeline(s->cmd_lists[s->frame_slot]);
        }
    }

    LONG x0 = (LONG)r.x;
    LONG y0 = (LONG)r.y;
    LONG x1 = (LONG)(r.x + r.w);
    LONG y1 = (LONG)(r.y + r.h);
    if (x0 < 0)
    {
        x0 = 0;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (x1 > (LONG)s->viewport_w)
    {
        x1 = (LONG)s->viewport_w;
    }
    if (y1 > (LONG)s->viewport_h)
    {
        y1 = (LONG)s->viewport_h;
    }

    if (s->scissor_depth > 0)
    {
        D3D12_RECT prev = s->scissors[s->scissor_depth - 1].rect;
        if (x0 < prev.left)
        {
            x0 = prev.left;
        }
        if (y0 < prev.top)
        {
            y0 = prev.top;
        }
        if (x1 > prev.right)
        {
            x1 = prev.right;
        }
        if (y1 > prev.bottom)
        {
            y1 = prev.bottom;
        }
    }
    if (x1 < x0)
    {
        x1 = x0;
    }
    if (y1 < y0)
    {
        y1 = y0;
    }

    D3D12_RECT sc = { x0, y0, x1, y1 };
    s->scissors[s->scissor_depth].rect   = sc;
    s->scissors[s->scissor_depth].active = TRUE;
    s->scissor_depth++;

    if (s->frame_active)
    {
        VCALL(s->cmd_lists[s->frame_slot], RSSetScissorRects, 1, &sc);
    }
}

/**
 * @brief pop the most recent scissor rect off the stack. If the stack is now empty, scissor testing is disabled entirely (everything draws).
 *
 * @return void
 */
void renderer__pop_scissor(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->scissor_depth <= 0)
    {
        return;
    }

    // Mirror push_scissor: both batches flush before the scissor
    // pop so queued draws land under the inner scissor.
    _d3d12_renderer_internal__flush_rects();
    if (s->text_run_count > 0)
    {
        _d3d12_renderer_internal__flush_text();
        if (s->frame_active)
        {
            _d3d12_renderer_internal__bind_rect_pipeline(s->cmd_lists[s->frame_slot]);
        }
    }

    s->scissor_depth--;

    if (s->frame_active == FALSE)
    {
        return;
    }

    D3D12_RECT sc;
    if (s->scissor_depth > 0)
    {
        sc = s->scissors[s->scissor_depth - 1].rect;
    }
    else
    {
        sc.left   = 0;
        sc.top    = 0;
        sc.right  = (LONG)s->viewport_w;
        sc.bottom = (LONG)s->viewport_h;
    }
    VCALL(s->cmd_lists[s->frame_slot], RSSetScissorRects, 1, &sc);
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

    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }

    // Bake everything queued so far into the per-frame command list:
    // the readback below copies pixels straight off the backbuffer,
    // so anything still sitting in submit_rect / submit_text / submit_
    // image batches needs to be drawn first. Same semantic as the
    // glReadPixels flush in opengl3 and the d3d11 flush_batches
    // pre-readback step.
    _d3d12_renderer_internal__flush_rects();
    _d3d12_renderer_internal__flush_text();

    int vw = (int)s->viewport_w;
    int vh = (int)s->viewport_h;

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

    // Allocate a READBACK-heap buffer to receive the pixel copy.
    // Row pitch must be 256-byte aligned (D3D12_TEXTURE_DATA_PITCH_
    // ALIGNMENT) -- the placed-footprint copy uses that as the
    // declared stride and Map gives back a buffer with the same
    // padding. Buffer is one-shot: created here, released after the
    // CPU-side memcpy.
    UINT   row_pitch  = ((UINT)w * 4u + 255u) & ~255u;
    UINT64 buffer_sz  = (UINT64)row_pitch * (UINT64)h;

    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type             = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC bd = { 0 };
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = buffer_sz;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* readback = NULL_POINTER;
    HRESULT hr = VCALL(s->device, CreateCommittedResource, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL_POINTER, &IID_ID3D12Resource, (void**)&readback);
    if (FAILED(hr) || readback == NULL_POINTER)
    {
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];
    ID3D12Resource* backbuffer = s->swap_buffers[s->backbuffer_index];

    // Backbuffer is currently RENDER_TARGET (begin_frame established
    // that). Transition to COPY_SOURCE for the read, copy the rect
    // to the readback buffer, then transition straight back to
    // RENDER_TARGET so the post-blur image draw can resume rendering
    // without another barrier.
    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    VCALL(list, ResourceBarrier, 1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION src_loc = { 0 };
    src_loc.pResource        = backbuffer;
    src_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst_loc = { 0 };
    dst_loc.pResource                          = readback;
    dst_loc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint.Offset             = 0;
    dst_loc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst_loc.PlacedFootprint.Footprint.Width    = (UINT)w;
    dst_loc.PlacedFootprint.Footprint.Height   = (UINT)h;
    dst_loc.PlacedFootprint.Footprint.Depth    = 1;
    dst_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;

    D3D12_BOX src_box = { 0 };
    src_box.left   = (UINT)x;
    src_box.top    = (UINT)y;
    src_box.front  = 0;
    src_box.right  = (UINT)(x + w);
    src_box.bottom = (UINT)(y + h);
    src_box.back   = 1;

    VCALL(list, CopyTextureRegion, &dst_loc, 0, 0, 0, &src_loc, &src_box);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    VCALL(list, ResourceBarrier, 1, &barrier);

    // Close + submit the per-frame cmd list mid-frame so the GPU
    // actually executes the rect/text/copy sequence. Block on a
    // fence so the readback buffer's contents are stable before we
    // Map. After this, allocator + cmd list are idle and safe to
    // Reset for the post-blur draws.
    VCALL0(list, Close);
    ID3D12CommandList* submit_lists[1] = { (ID3D12CommandList*)list };
    VCALL(s->queue, ExecuteCommandLists, 1, submit_lists);

    s->next_fence_value++;
    VCALL(s->queue, Signal, s->fence, s->next_fence_value);
    _d3d12_renderer_internal__wait_for_fence(s->next_fence_value);

    // Map + memcpy out of the readback buffer. RowPitch may exceed
    // w*4 due to the 256-byte alignment, so copy row-by-row. Format
    // is R8G8B8A8_UNORM (matches the swapchain), no channel swizzle
    // needed -- the blur helper expects RGBA in memory order.
    D3D12_RANGE read_range  = { 0, (SIZE_T)buffer_sz };
    D3D12_RANGE write_range = { 0, 0 };
    void* mapped = NULL_POINTER;
    hr = VCALL(readback, Map, 0, &read_range, &mapped);
    if (FAILED(hr) || mapped == NULL_POINTER)
    {
        VCALL0(readback, Release);
        // Cmd list is closed but end_frame still needs it open. Run
        // the same rebind helper used on the success path so the
        // frame can finish.
        _d3d12_renderer_internal__rebind_frame_after_drain();
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    size_t pxbytes = (size_t)w * (size_t)h * 4;
    ubyte* pixels  = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE__RENDERER);
    ubyte* tmp     = (ubyte*)GUI_MALLOC_T(pxbytes, MM_TYPE__RENDERER);
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
        VCALL(readback, Unmap, 0, &write_range);
        VCALL0(readback, Release);
        _d3d12_renderer_internal__rebind_frame_after_drain();
        gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
        renderer__submit_rect(rect, dim, 0.0f);
        return;
    }

    for (int py = 0; py < h; py++)
    {
        ubyte* src_p = (ubyte*)mapped + (size_t)py * (size_t)row_pitch;
        ubyte* dst_p = pixels         + (size_t)py * (size_t)w * 4;
        stdlib__memcpy(dst_p, src_p, (size_t)w * 4);
    }

    VCALL(readback, Unmap, 0, &write_range);
    VCALL0(readback, Release);

    // CPU separable Gaussian. D3D12 has top-left origin like d3d11,
    // so no row flip needed (only opengl3/gles3 readback is upside-
    // down).
    blur_cpu__apply_gaussian(pixels, tmp, w, h, sigma_px);

    // Reopen the per-frame cmd list and re-establish ALL state that
    // begin_frame originally set. After Reset the list is empty; the
    // submit_image path that follows depends on root sig + viewport
    // + scissor + RTV + root constants being bound. Forgetting
    // OMSetRenderTargets is the silent-no-pixels failure mode this
    // function previously hit.
    _d3d12_renderer_internal__rebind_frame_after_drain();

    // Build the SRV-backed transient texture from the blurred pixels.
    // create_texture_rgba uses its own one-shot upload allocator + cmd
    // list internally and signals + waits on a fresh fence value, so
    // it doesn't disturb the per-frame cmd list state we just rebuilt.
    void* tex = renderer__create_texture_rgba(pixels, w, h);

    if (tex != NULL_POINTER)
    {
        gui_rect_t draw_rect;
        draw_rect.x = (float)x;
        draw_rect.y = (float)y;
        draw_rect.w = (float)w;
        draw_rect.h = (float)h;
        gui_color_t white = { 1.0f, 1.0f, 1.0f, 1.0f };
        renderer__submit_image(draw_rect, tex, white);

        // Defer release: submit_image recorded a draw referencing
        // this entry's SRV slot, but the cmd list won't actually
        // execute until end_frame. Inline destroy_texture would
        // wait_for_gpu (drains nothing because cmd list is still
        // open) and then release the resource out from under the
        // pending draw. Instead, queue on the slot's pending-free
        // ring; begin_frame N+FRAMES_IN_FLIGHT drains it after the
        // fence wait proves the GPU is done.
        _d3d12_renderer_internal__schedule_pending_free((_d3d12_renderer_internal__tex_entry_t*)tex);
    }

    GUI_FREE(tmp);
    GUI_FREE(pixels);
}

// ============================================================================
// IMPL: device / swap / RTVs / command / fence / root sig / PSO / VBO
// ============================================================================

static boole _d3d12_renderer_internal__create_device_and_queue(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

#if !defined(NDEBUG)
    // Debug layer: catches invalid API usage (wrong resource states,
    // unbound descriptors, mismatched root signatures, etc.) and
    // logs to OutputDebugString. Only available when the Windows
    // Graphics Tools feature is installed. Silently continues if
    // not available (release builds, or Windows SKUs without the
    // Graphics Tools optional feature).
    {
        ID3D12Debug* dbg = NULL_POINTER;
        HRESULT hr_dbg = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbg);
        if (SUCCEEDED(hr_dbg) && dbg != NULL_POINTER)
        {
            VCALL0(dbg, EnableDebugLayer);
            VCALL0(dbg, Release);
            log_info("%s", "d3d12 debug layer ON");
        }
    }
#endif

    // Default adapter (NULL) at feature level 11.0. D3D12 picks
    // the best available driver.
    HRESULT hr = D3D12CreateDevice(NULL_POINTER, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&s->device);
    if (FAILED(hr))
    {
        log_error("%s %08x", "D3D12CreateDevice failed: 0x", (unsigned)hr);
        return FALSE;
    }

#if !defined(NDEBUG)
    // Route the debug layer's messages into log_warn so validation
    // errors show up in the same console the host app uses for all
    // other logging, not just the IDE's Debug Output window. Also
    // enables "break on error" in the InfoQueue so the debugger
    // stops on the offending call (useful under VS; harmless when
    // running standalone).
    {
        ID3D12InfoQueue* iq = NULL_POINTER;
        HRESULT hr_iq = VCALL(s->device, QueryInterface, &IID_ID3D12InfoQueue, (void**)&iq);
        if (SUCCEEDED(hr_iq) && iq != NULL_POINTER)
        {
            VCALL(iq, SetBreakOnSeverity, D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);
            VCALL(iq, SetBreakOnSeverity, D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            VCALL0(iq, Release);
            log_info("%s", "d3d12 info queue: break-on-error armed");
        }
    }
#endif

    D3D12_COMMAND_QUEUE_DESC cqd = { 0 };
    cqd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    cqd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = VCALL(s->device, CreateCommandQueue, &cqd, &IID_ID3D12CommandQueue, (void**)&s->queue);
    if (FAILED(hr))
    {
        log_error("%s %08x", "ID3D12Device::CreateCommandQueue failed: 0x", (unsigned)hr);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_swapchain(HWND hwnd)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    IDXGIFactory4* factory = NULL_POINTER;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr))
    {
        log_error("%s %08x", "CreateDXGIFactory1 failed: 0x", (unsigned)hr);
        return FALSE;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0)
    {
        w = 800;
    }
    if (h <= 0)
    {
        h = 600;
    }
    s->viewport_w = w;
    s->viewport_h = h;

    DXGI_SWAP_CHAIN_DESC1 scd = { 0 };
    scd.Width       = (UINT)w;
    scd.Height      = (UINT)h;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferCount = _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT;
    s->swap_buffer_count = _D3D12_RENDERER_INTERNAL__SWAP_BUFFER_COUNT;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;
    scd.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;

    IDXGISwapChain1* sc1 = NULL_POINTER;
    hr = VCALL(factory, CreateSwapChainForHwnd, (IUnknown*)s->queue, hwnd, &scd, NULL_POINTER, NULL_POINTER, &sc1);
    if (FAILED(hr))
    {
        log_error("%s %08x", "CreateSwapChainForHwnd failed: 0x", (unsigned)hr);
        VCALL0(factory, Release);
        return FALSE;
    }

    // Disable DXGI's alt-enter fullscreen -- we handle window
    // state ourselves.
    VCALL(factory, MakeWindowAssociation, hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Upgrade to IDXGISwapChain3 for GetCurrentBackBufferIndex.
    hr = VCALL(sc1, QueryInterface, &IID_IDXGISwapChain3, (void**)&s->swapchain);
    VCALL0(sc1, Release);
    VCALL0(factory, Release);
    if (FAILED(hr))
    {
        log_error("%s %08x", "QueryInterface IDXGISwapChain3 failed: 0x", (unsigned)hr);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_rtvs(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    D3D12_DESCRIPTOR_HEAP_DESC hd = { 0 };
    hd.NumDescriptors = _D3D12_RENDERER_INTERNAL__MAX_SWAP_BUFFERS;
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HRESULT hr = VCALL(s->device, CreateDescriptorHeap, &hd, &IID_ID3D12DescriptorHeap, (void**)&s->rtv_heap);
    if (FAILED(hr))
    {
        log_error("%s %08x", "CreateDescriptorHeap (RTV) failed: 0x", (unsigned)hr);
        return FALSE;
    }

    s->rtv_stride = VCALL(s->device, GetDescriptorHandleIncrementSize, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    VCALL(s->rtv_heap, GetCPUDescriptorHandleForHeapStart, &handle);

    for (int i = 0; i < (int)s->swap_buffer_count; i++)
    {
        hr = VCALL(s->swapchain, GetBuffer, (UINT)i, &IID_ID3D12Resource, (void**)&s->swap_buffers[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "IDXGISwapChain3::GetBuffer[", i, "] failed: 0x", (unsigned)hr);
            return FALSE;
        }
        VCALL(s->device, CreateRenderTargetView, s->swap_buffers[i], NULL_POINTER, handle);
        s->swap_rtvs[i] = handle;
        handle.ptr += s->rtv_stride;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_command_objects(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        HRESULT hr = VCALL(s->device, CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&s->allocators[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "CreateCommandAllocator[", i, "] failed: 0x", (unsigned)hr);
            return FALSE;
        }

        hr = VCALL(s->device, CreateCommandList, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->allocators[i], NULL_POINTER, &IID_ID3D12GraphicsCommandList, (void**)&s->cmd_lists[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "CreateCommandList[", i, "] failed: 0x", (unsigned)hr);
            return FALSE;
        }
        // Lists are created in the recording state; close them
        // so begin_frame's Reset call is symmetric.
        VCALL0(s->cmd_lists[i], Close);
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_fence(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    HRESULT hr = VCALL(s->device, CreateFence, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s->fence);
    if (FAILED(hr))
    {
        log_error("%s %08x", "ID3D12Device::CreateFence failed: 0x", (unsigned)hr);
        return FALSE;
    }
    s->next_fence_value = 0;
    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        s->fence_values[i] = 0;
    }

    s->fence_event = CreateEventW(NULL_POINTER, FALSE, FALSE, NULL_POINTER);
    if (s->fence_event == NULL_POINTER)
    {
        log_error("%s %lu", "CreateEventW failed:", (unsigned long)GetLastError());
        return FALSE;
    }
    return TRUE;
}

static void _d3d12_renderer_internal__wait_for_fence(UINT64 value)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    UINT64 completed = VCALL0(s->fence, GetCompletedValue);
    if (completed < value)
    {
        VCALL(s->fence, SetEventOnCompletion, value, s->fence_event);
        WaitForSingleObject(s->fence_event, INFINITE);
    }
}

static void _d3d12_renderer_internal__wait_for_gpu(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    s->next_fence_value++;
    VCALL(s->queue, Signal, s->fence, s->next_fence_value);
    _d3d12_renderer_internal__wait_for_fence(s->next_fence_value);
}

static boole _d3d12_renderer_internal__create_root_signature(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    // One root parameter: 4 32-bit constants (viewport.x,
    // viewport.y, _pad.x, _pad.y). Mapped to b0 in the HLSL as
    // `cbuffer RootCB`. Cheaper than a CBV for 16 bytes.
    D3D12_ROOT_PARAMETER rp = { 0 };
    rp.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 4;
    rp.Constants.ShaderRegister = 0;
    rp.Constants.RegisterSpace  = 0;
    rp.ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsd = { 0 };
    rsd.NumParameters = 1;
    rsd.pParameters   = &rp;
    rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = NULL_POINTER;
    ID3DBlob* err = NULL_POINTER;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        if (err != NULL_POINTER)
        {
            log_error("%s %s", "D3D12SerializeRootSignature:", (const char*)VCALL0(err, GetBufferPointer));
            VCALL0(err, Release);
        }
        return FALSE;
    }
    if (err != NULL_POINTER)
    {
        VCALL0(err, Release);
    }

    hr = VCALL(s->device, CreateRootSignature, 0, VCALL0(sig, GetBufferPointer), VCALL0(sig, GetBufferSize), &IID_ID3D12RootSignature, (void**)&s->root_sig);
    VCALL0(sig, Release);

    if (FAILED(hr))
    {
        log_error("%s %08x", "ID3D12Device::CreateRootSignature failed: 0x", (unsigned)hr);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_rect_pso(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    ID3DBlob* vs = NULL_POINTER;
    ID3DBlob* ps = NULL_POINTER;
    ID3DBlob* err = NULL_POINTER;

    HRESULT hr = D3DCompile( _D3D12_RENDERER_INTERNAL__HLSL_SRC, stdlib__strlen(_D3D12_RENDERER_INTERNAL__HLSL_SRC), "rect.hlsl", NULL_POINTER, NULL_POINTER, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr))
    {
        if (err != NULL_POINTER)
        {
            log_error("%s %s", "D3DCompile VS:", (const char*)VCALL0(err, GetBufferPointer));
            VCALL0(err, Release);
        }
        return FALSE;
    }
    if (err != NULL_POINTER)
    {
        VCALL0(err, Release);
        err = NULL_POINTER;
    }

    hr = D3DCompile( _D3D12_RENDERER_INTERNAL__HLSL_SRC, stdlib__strlen(_D3D12_RENDERER_INTERNAL__HLSL_SRC), "rect.hlsl", NULL_POINTER, NULL_POINTER, "PSMain", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr))
    {
        if (err != NULL_POINTER)
        {
            log_error("%s %s", "D3DCompile PS:", (const char*)VCALL0(err, GetBufferPointer));
            VCALL0(err, Release);
        }
        VCALL0(vs, Release);
        return FALSE;
    }
    if (err != NULL_POINTER)
    {
        VCALL0(err, Release);
    }

    // IA layout: same 11-float vertex as every other backend,
    // split into five named elements matching the HLSL struct.
    // DXGI uses *_FLOAT for IEEE-float formats (no "S" prefix --
    // that's the Vulkan VK_FORMAT_*_SFLOAT spelling). Got crossed
    // initially because both APIs ship side-by-side in this repo.
    D3D12_INPUT_ELEMENT_DESC ied[5] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__vertex_t, x),      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(_d3d12_renderer_internal__vertex_t, r),      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__vertex_t, lx),     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__vertex_t, rect_w), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, offsetof(_d3d12_renderer_internal__vertex_t, radius), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = { 0 };
    pso.pRootSignature                     = s->root_sig;
    pso.VS.pShaderBytecode                 = VCALL0(vs, GetBufferPointer);
    pso.VS.BytecodeLength                  = VCALL0(vs, GetBufferSize);
    pso.PS.pShaderBytecode                 = VCALL0(ps, GetBufferPointer);
    pso.PS.BytecodeLength                  = VCALL0(ps, GetBufferSize);
    pso.InputLayout.pInputElementDescs     = ied;
    pso.InputLayout.NumElements            = 5;
    pso.PrimitiveTopologyType              = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets                   = 1;
    pso.RTVFormats[0]                      = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat                          = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count                   = 1;
    pso.SampleMask                         = UINT_MAX;

    // Rasterizer: no cull, no depth, no scissor default-disabled (we enable dynamically).
    pso.RasterizerState.FillMode           = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode           = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable    = TRUE;

    // Depth-stencil: disabled. We don't use a DS view.
    pso.DepthStencilState.DepthEnable      = FALSE;
    pso.DepthStencilState.StencilEnable    = FALSE;

    // Blend: separate-alpha per the visual contract.
    pso.BlendState.AlphaToCoverageEnable   = FALSE;
    pso.BlendState.IndependentBlendEnable  = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable            = TRUE;
    pso.BlendState.RenderTarget[0].LogicOpEnable          = FALSE;
    pso.BlendState.RenderTarget[0].SrcBlend               = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend              = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp                = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha          = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha         = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha           = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr2 = VCALL(s->device, CreateGraphicsPipelineState, &pso, &IID_ID3D12PipelineState, (void**)&s->rect_pso);
    VCALL0(vs, Release);
    VCALL0(ps, Release);

    if (FAILED(hr2))
    {
        log_error("%s %08x", "CreateGraphicsPipelineState (rect) failed: 0x", (unsigned)hr2);
        return FALSE;
    }
    return TRUE;
}

static boole _d3d12_renderer_internal__create_rect_vertex_buffers(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    UINT64 size = (UINT64)(sizeof(_d3d12_renderer_internal__vertex_t) * _D3D12_RENDERER_INTERNAL__MAX_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD);

    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    hp.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask     = 1;
    hp.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension           = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment           = 0;
    rd.Width               = size;
    rd.Height              = 1;
    rd.DepthOrArraySize    = 1;
    rd.MipLevels           = 1;
    rd.Format              = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count    = 1;
    rd.Layout              = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags               = D3D12_RESOURCE_FLAG_NONE;

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        HRESULT hr = VCALL(s->device, CreateCommittedResource, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL_POINTER, &IID_ID3D12Resource, (void**)&s->rect_vbo[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "CreateCommittedResource (rect vbo", i, ") failed: 0x", (unsigned)hr);
            return FALSE;
        }

        // Persistent map -- same strategy as the Vulkan backend.
        // UPLOAD heap is CPU-write-combined, fast enough for a
        // 192 KB buffer streaming per frame.
        D3D12_RANGE read_range = { 0, 0 };
        hr = VCALL(s->rect_vbo[i], Map, 0, &read_range, (void**)&s->rect_vbo_ptr[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "Map (rect vbo", i, ") failed: 0x", (unsigned)hr);
            return FALSE;
        }
    }
    return TRUE;
}

// ============================================================================
// text + image support
// ============================================================================

static boole _d3d12_renderer_internal__create_srv_heap(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    // Shader-visible CBV_SRV_UAV heap. `SHADER_VISIBLE` is required so
    // `SetGraphicsRootDescriptorTable` can reference slots in it.
    // Capacity = _MAX_SRVS is sized for worst-case atlases + images.
    D3D12_DESCRIPTOR_HEAP_DESC hd = { 0 };
    hd.NumDescriptors = _D3D12_RENDERER_INTERNAL__MAX_SRVS;
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = VCALL(s->device, CreateDescriptorHeap, &hd, &IID_ID3D12DescriptorHeap, (void**)&s->srv_heap);
    if (FAILED(hr))
    {
        log_error("%s %08x", "CreateDescriptorHeap (SRV) failed: 0x", (unsigned)hr);
        return FALSE;
    }
    s->srv_stride = VCALL(s->device, GetDescriptorHandleIncrementSize, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return TRUE;
}

// Text + image VBOs. Upload heap, persistent mapped. Streaming write
// from the CPU each frame, GPU reads once per draw. Same pattern as
// the rect VBO.
static boole _d3d12_renderer_internal__create_tx_vertex_buffers(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    D3D12_HEAP_PROPERTIES hp = { 0 };
    hp.Type             = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    UINT64 size = (UINT64)(sizeof(_d3d12_renderer_internal__tx_vertex_t) * _D3D12_RENDERER_INTERNAL__MAX_TEXT_QUADS * _D3D12_RENDERER_INTERNAL__VERTS_PER_QUAD);

    D3D12_RESOURCE_DESC rd = { 0 };
    rd.Dimension         = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width             = size;
    rd.Height            = 1;
    rd.DepthOrArraySize  = 1;
    rd.MipLevels         = 1;
    rd.Format            = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count  = 1;
    rd.Layout            = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags             = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RANGE read_range = { 0, 0 };

    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT; i++)
    {
        HRESULT hr = VCALL(s->device, CreateCommittedResource, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL_POINTER, &IID_ID3D12Resource, (void**)&s->text_vbo[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "CreateCommittedResource (text vbo", i, ") 0x", (unsigned)hr);
            return FALSE;
        }
        hr = VCALL(s->text_vbo[i], Map, 0, &read_range, (void**)&s->text_vbo_ptr[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "Map text vbo", i, ": 0x", (unsigned)hr);
            return FALSE;
        }

        hr = VCALL(s->device, CreateCommittedResource, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL_POINTER, &IID_ID3D12Resource, (void**)&s->image_vbo[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "CreateCommittedResource (image vbo", i, ") 0x", (unsigned)hr);
            return FALSE;
        }
        hr = VCALL(s->image_vbo[i], Map, 0, &read_range, (void**)&s->image_vbo_ptr[i]);
        if (FAILED(hr))
        {
            log_error("%s %d %s %08x", "Map image vbo", i, ": 0x", (unsigned)hr);
            return FALSE;
        }
    }
    return TRUE;
}

// Build the text + image root signature (shared) and compile both
// pixel shaders against one shared VS. Root sig layout:
// slot 0: 4x 32-bit constants (viewport xy + pad)
// slot 1: 1-SRV descriptor table  (shader register t0)
// static sampler at s0: LINEAR min/mag, CLAMP uv -- matches every
// other backend's atlas + image sampling.
static boole _d3d12_renderer_internal__create_tx_root_and_pipelines(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;

    // Descriptor range: 1 SRV starting at t0. The shader reads the
    // current table's first descriptor.
    D3D12_DESCRIPTOR_RANGE range = { 0 };
    range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors     = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace      = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rp[2] = { 0 };
    rp[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.RegisterSpace  = 0;
    rp[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges   = &range;
    rp[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC ss = { 0 };
    ss.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxAnisotropy  = 1;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.BorderColor    = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    ss.MinLOD         = 0;
    ss.MaxLOD         = D3D12_FLOAT32_MAX;
    ss.ShaderRegister = 0;
    ss.RegisterSpace  = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = { 0 };
    rsd.NumParameters     = 2;
    rsd.pParameters       = rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers   = &ss;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig = NULL_POINTER;
    ID3DBlob* err = NULL_POINTER;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr))
    {
        if (err != NULL_POINTER)
        {
            log_error("%s %s", "tx root sig serialize:", (const char*)VCALL0(err, GetBufferPointer));
            VCALL0(err, Release);
        }
        return FALSE;
    }
    if (err != NULL_POINTER)
    {
        VCALL0(err, Release);
    }

    hr = VCALL(s->device, CreateRootSignature, 0, VCALL0(sig, GetBufferPointer), VCALL0(sig, GetBufferSize), &IID_ID3D12RootSignature, (void**)&s->tx_root_sig);
    VCALL0(sig, Release);
    if (FAILED(hr))
    {
        log_error("%s %08x", "tx CreateRootSignature failed: 0x", (unsigned)hr);
        return FALSE;
    }

    // Compile VS + both PSes from the shared TX HLSL source.
    ID3DBlob* vs = NULL_POINTER;
    ID3DBlob* ps_text = NULL_POINTER;
    ID3DBlob* ps_img  = NULL_POINTER;

    hr = D3DCompile(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC, stdlib__strlen(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC), "tx.hlsl", NULL_POINTER, NULL_POINTER, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { if (err != NULL_POINTER)
    {
        log_error("%s %s", "tx VS:", (const char*)VCALL0(err, GetBufferPointer));
        VCALL0(err, Release);
        } return FALSE;
    }
    if (err != NULL_POINTER)
    {
        VCALL0(err, Release);
        err = NULL_POINTER;
    }

    hr = D3DCompile(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC, stdlib__strlen(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC), "tx.hlsl", NULL_POINTER, NULL_POINTER, "PSText", "ps_5_0", 0, 0, &ps_text, &err);
    if (FAILED(hr)) { if (err != NULL_POINTER)
    {
        log_error("%s %s", "tx PSText:", (const char*)VCALL0(err, GetBufferPointer));
        VCALL0(err, Release);
        } VCALL0(vs, Release); return FALSE;
    }
    if (err != NULL_POINTER)
    {
        VCALL0(err, Release);
        err = NULL_POINTER;
    }

    hr = D3DCompile(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC, stdlib__strlen(_D3D12_RENDERER_INTERNAL__TX_HLSL_SRC), "tx.hlsl", NULL_POINTER, NULL_POINTER, "PSImage", "ps_5_0", 0, 0, &ps_img, &err);
    if (FAILED(hr)) { if (err != NULL_POINTER)
    {
        log_error("%s %s", "tx PSImage:", (const char*)VCALL0(err, GetBufferPointer));
        VCALL0(err, Release);
        } VCALL0(vs, Release); VCALL0(ps_text, Release); return FALSE;
    }
    if (err != NULL_POINTER)
    {
        VCALL0(err, Release);
    }

    // IA layout: 2 floats pos + 4 floats color + 2 floats uv.
    D3D12_INPUT_ELEMENT_DESC ied[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__tx_vertex_t, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(_d3d12_renderer_internal__tx_vertex_t, r), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(_d3d12_renderer_internal__tx_vertex_t, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = { 0 };
    pso.pRootSignature                    = s->tx_root_sig;
    pso.VS.pShaderBytecode                = VCALL0(vs, GetBufferPointer);
    pso.VS.BytecodeLength                 = VCALL0(vs, GetBufferSize);
    pso.InputLayout.pInputElementDescs    = ied;
    pso.InputLayout.NumElements           = 3;
    pso.PrimitiveTopologyType             = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets                  = 1;
    pso.RTVFormats[0]                     = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat                         = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count                  = 1;
    pso.SampleMask                        = UINT_MAX;

    pso.RasterizerState.FillMode          = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode          = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable   = TRUE;

    pso.DepthStencilState.DepthEnable     = FALSE;
    pso.DepthStencilState.StencilEnable   = FALSE;

    // Separate-alpha blend per the visual contract (same as the rect pipeline).
    pso.BlendState.AlphaToCoverageEnable  = FALSE;
    pso.BlendState.IndependentBlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].BlendEnable            = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend               = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend              = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp                = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha          = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha         = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha           = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask  = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.PS.pShaderBytecode = VCALL0(ps_text, GetBufferPointer);
    pso.PS.BytecodeLength  = VCALL0(ps_text, GetBufferSize);
    hr = VCALL(s->device, CreateGraphicsPipelineState, &pso, &IID_ID3D12PipelineState, (void**)&s->text_pso);
    if (FAILED(hr))
    {
        log_error("%s %08x", "text PSO failed: 0x", (unsigned)hr);
        VCALL0(vs, Release);
        VCALL0(ps_text, Release);
        VCALL0(ps_img, Release);
        return FALSE;
    }

    pso.PS.pShaderBytecode = VCALL0(ps_img, GetBufferPointer);
    pso.PS.BytecodeLength  = VCALL0(ps_img, GetBufferSize);
    hr = VCALL(s->device, CreateGraphicsPipelineState, &pso, &IID_ID3D12PipelineState, (void**)&s->image_pso);
    VCALL0(vs, Release);
    VCALL0(ps_text, Release);
    VCALL0(ps_img, Release);
    if (FAILED(hr))
    {
        log_error("%s %08x", "image PSO failed: 0x", (unsigned)hr);
        return FALSE;
    }

    return TRUE;
}

// Next-free-slot allocator over the fixed tex_pool. O(n) scan; pool
// is bounded at MAX_SRVS (256) so this is cheap. Frees put the slot
// back in the pool by flipping in_use.
static int _d3d12_renderer_internal__alloc_srv_slot(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    for (int i = 0; i < _D3D12_RENDERER_INTERNAL__MAX_SRVS; i++)
    {
        if (s->tex_pool[i].in_use == FALSE)
        {
            return i;
        }
    }
    return -1;
}

static void _d3d12_renderer_internal__free_srv_slot(int slot)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (slot < 0 || slot >= _D3D12_RENDERER_INTERNAL__MAX_SRVS)
    {
        return;
    }
    s->tex_pool[slot].in_use = FALSE;
}

static _d3d12_renderer_internal__tex_entry_t* _d3d12_renderer_internal__alloc_tex_entry(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    int slot = _d3d12_renderer_internal__alloc_srv_slot();
    if (slot < 0)
    {
        return NULL_POINTER;
    }
    _d3d12_renderer_internal__tex_entry_t* e = &s->tex_pool[slot];
    stdlib__memset(e, 0, sizeof(*e));
    e->in_use   = TRUE;
    e->srv_slot = slot;
    return e;
}

static void _d3d12_renderer_internal__release_tex_entry(_d3d12_renderer_internal__tex_entry_t* e)
{
    if (e == NULL_POINTER)
    {
        return;
    }
    if (e->resource != NULL_POINTER)
    {
        VCALL0(e->resource, Release);
        e->resource = NULL_POINTER;
    }
    _d3d12_renderer_internal__free_srv_slot(e->srv_slot);
}

// Stash a tex_entry on the current frame slot's pending-free ring.
// blur_region is the only caller: it builds a transient backbuffer-
// snapshot texture, queues an image draw against it in the per-
// frame command list, and can NOT release inline because the cmd
// list still references the SRV until end_frame closes + executes
// it. Drain runs in begin_frame after that slot's fence wait, when
// the previous cycle's draws are guaranteed GPU-complete.
// If the ring is full (host code blurring far more regions per
// frame than expected), the entry leaks until shutdown's tex_pool
// sweep frees it -- inline release isn't safe because submit_image
// has already recorded a draw against this SRV into the per-frame
// cmd list, and that cmd list hasn't been submitted yet.
// wait_for_gpu would block on submitted work but wouldn't drain
// the pending recording, so a release here would race the eventual
// draw against a freed resource. With 32 slots × FIF=2 = 64 in-
// flight blur textures the cap is generous; hitting it indicates
// host code does an unreasonable number of blurred regions per
// frame and should be sized up rather than worked around.
static void _d3d12_renderer_internal__schedule_pending_free(_d3d12_renderer_internal__tex_entry_t* e)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (e == NULL_POINTER)
    {
        return;
    }
    int slot = (int)s->frame_slot;
    if (s->pending_free_count[slot] >= _D3D12_RENDERER_INTERNAL__MAX_PENDING_FREES)
    {
        log_warn("%s", "d3d12 blur: pending-free ring full, leaking SRV slot until shutdown (raise MAX_PENDING_FREES)");
        return;
    }
    s->pending_frees[slot][s->pending_free_count[slot]++] = e;
}

static void _d3d12_renderer_internal__drain_pending_frees(int slot)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (slot < 0 || slot >= _D3D12_RENDERER_INTERNAL__FRAMES_IN_FLIGHT)
    {
        return;
    }
    for (int i = 0; i < s->pending_free_count[slot]; i++)
    {
        _d3d12_renderer_internal__release_tex_entry(s->pending_frees[slot][i]);
        s->pending_frees[slot][i] = NULL_POINTER;
    }
    s->pending_free_count[slot] = 0;
}

// One-shot synchronous upload helper. Atlas and image loads are rare
// (once per font / image file) so the simple "record + submit + block"
// path beats a queue + batching design. Steps:
// 1. Create DEFAULT-heap texture (GPU-only) for the final resource.
// 2. Create UPLOAD-heap staging buffer sized by GetCopyableFootprints
// (accounts for the 256-byte row-pitch alignment requirement).
// 3. Map the upload buffer and copy pixels row-by-row (respecting
// RowPitch, which may exceed `w * bytes_per_pixel`).
// 4. Use a dedicated one-shot allocator + command list to record
// the CopyTextureRegion + transition to PIXEL_SHADER_RESOURCE.
// 5. Execute, signal the fence, block until GPU done.
// 6. Create the SRV in the shader-visible heap at the entry's slot.
// 7. Release staging buffer + command objects.
// levels[0] is the top level; further levels are the mip chain, one
// subresource each (images bring the chain from _mip_cpu.h, the text
// atlas has one level).
static boole _d3d12_renderer_internal__upload_texture(_d3d12_renderer_internal__tex_entry_t* e, const mip_cpu__level_t* levels, int level_count, DXGI_FORMAT fmt, int bytes_per_pixel)
{
    _d3d12_renderer_internal__state_t* s  = &_d3d12_renderer_internal__g;
    int                                w  = levels[0].w;
    int                                h  = levels[0].h;
    int                                lv = 0;

    if (level_count < 1 || level_count > MIP_CPU__MAX_LEVELS)
    {
        log_error("%s %d", "tex upload: bad level count", level_count);
        return FALSE;
    }

    D3D12_HEAP_PROPERTIES hp_default = { 0 };
    hp_default.Type             = D3D12_HEAP_TYPE_DEFAULT;
    hp_default.CreationNodeMask = 1;
    hp_default.VisibleNodeMask  = 1;

    D3D12_HEAP_PROPERTIES hp_upload = { 0 };
    hp_upload.Type             = D3D12_HEAP_TYPE_UPLOAD;
    hp_upload.CreationNodeMask = 1;
    hp_upload.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC td = { 0 };
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = (UINT64)w;
    td.Height           = (UINT)h;
    td.DepthOrArraySize = 1;
    td.MipLevels        = (UINT16)level_count;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = VCALL(s->device, CreateCommittedResource, &hp_default, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL_POINTER, &IID_ID3D12Resource, (void**)&e->resource);
    if (FAILED(hr))
    {
        log_error("%s %08x", "tex CreateCommittedResource (default) failed: 0x", (unsigned)hr);
        return FALSE;
    }

    // Query footprint to learn the UPLOAD buffer size + RowPitch the
    // driver expects. RowPitch is rounded up to 256 bytes per D3D12's
    // ReadRange requirement (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT).
    // one footprint per level; the offsets place the levels back to
    // back in the upload buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps[MIP_CPU__MAX_LEVELS];
    UINT   num_rows[MIP_CPU__MAX_LEVELS];
    UINT64 row_bytes[MIP_CPU__MAX_LEVELS];
    UINT64 total_bytes = 0;
    stdlib__memset(fps, 0, sizeof(fps));
    VCALL(s->device, GetCopyableFootprints, &td, 0, (UINT)level_count, 0, fps, num_rows, row_bytes, &total_bytes);

    D3D12_RESOURCE_DESC bd = { 0 };
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total_bytes;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* upload = NULL_POINTER;
    hr = VCALL(s->device, CreateCommittedResource, &hp_upload, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL_POINTER, &IID_ID3D12Resource, (void**)&upload);
    if (FAILED(hr))
    {
        log_error("%s %08x", "tex upload CreateCommittedResource failed: 0x", (unsigned)hr);
        VCALL0(e->resource, Release);
        e->resource = NULL_POINTER;
        return FALSE;
    }

    // Map + row-by-row copy. Row padding (fp.Footprint.RowPitch) may
    // exceed `w * bpp` on some GPUs; copy each logical row into its
    // aligned slot. Upload buffer's offset into its own allocation
    // is fp.Offset, but for a freshly-allocated buffer it's 0.
    D3D12_RANGE read_range = { 0, 0 };
    ubyte* mapped = NULL_POINTER;
    hr = VCALL(upload, Map, 0, &read_range, (void**)&mapped);
    if (FAILED(hr))
    {
        log_error("%s %08x", "tex upload Map failed: 0x", (unsigned)hr);
        VCALL0(upload, Release);
        VCALL0(e->resource, Release);
        e->resource = NULL_POINTER;
        return FALSE;
    }
    for (lv = 0; lv < level_count; lv++)
    {
        UINT row_bpp = (UINT)levels[lv].w * (UINT)bytes_per_pixel;
        UINT row     = 0;
        for (row = 0; row < num_rows[lv]; row++)
        {
            stdlib__memcpy(mapped + fps[lv].Offset + (size_t)row * fps[lv].Footprint.RowPitch, levels[lv].pixels + (size_t)row * row_bpp, row_bpp);
        }
    }
    VCALL(upload, Unmap, 0, NULL_POINTER);

    // One-shot command list. Create dedicated allocator + list so we
    // don't interfere with the per-frame command objects. Record the
    // copy + transition + close + execute + fence + block.
    ID3D12CommandAllocator*    upload_alloc = NULL_POINTER;
    ID3D12GraphicsCommandList* upload_list  = NULL_POINTER;
    hr = VCALL(s->device, CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&upload_alloc);
    if (FAILED(hr))
    {
        log_error("%s %08x", "tex upload CreateCommandAllocator: 0x", (unsigned)hr);
        VCALL0(upload, Release);
        VCALL0(e->resource, Release);
        e->resource = NULL_POINTER;
        return FALSE;
    }
    hr = VCALL(s->device, CreateCommandList, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, upload_alloc, NULL_POINTER, &IID_ID3D12GraphicsCommandList, (void**)&upload_list);
    if (FAILED(hr))
    {
        log_error("%s %08x", "tex upload CreateCommandList: 0x", (unsigned)hr);
        VCALL0(upload_alloc, Release);
        VCALL0(upload, Release);
        VCALL0(e->resource, Release);
        e->resource = NULL_POINTER;
        return FALSE;
    }

    for (lv = 0; lv < level_count; lv++)
    {
        D3D12_TEXTURE_COPY_LOCATION src = { 0 };
        src.pResource       = upload;
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fps[lv];

        D3D12_TEXTURE_COPY_LOCATION dst = { 0 };
        dst.pResource        = e->resource;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = (UINT)lv;

        VCALL(upload_list, CopyTextureRegion, &dst, 0, 0, 0, &src, NULL_POINTER);
    }

    D3D12_RESOURCE_BARRIER barrier = { 0 };
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = e->resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    VCALL(upload_list, ResourceBarrier, 1, &barrier);

    VCALL0(upload_list, Close);

    ID3D12CommandList* submit_lists[1] = { (ID3D12CommandList*)upload_list };
    VCALL(s->queue, ExecuteCommandLists, 1, submit_lists);

    // Block until the copy completes. Use the same fence as the main
    // frame loop -- ticks monotonically, no conflict.
    s->next_fence_value++;
    VCALL(s->queue, Signal, s->fence, s->next_fence_value);
    _d3d12_renderer_internal__wait_for_fence(s->next_fence_value);

    VCALL0(upload_list, Release);
    VCALL0(upload_alloc, Release);
    VCALL0(upload, Release);

    // Create the SRV in the heap slot the entry already reserved.
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { 0 };
    sv.Format                    = fmt;
    sv.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels       = (UINT)level_count;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    VCALL(s->srv_heap, GetCPUDescriptorHandleForHeapStart, &cpu);
    cpu.ptr += (SIZE_T)e->srv_slot * s->srv_stride;
    VCALL(s->device, CreateShaderResourceView, e->resource, &sv, cpu);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    VCALL(s->srv_heap, GetGPUDescriptorHandleForHeapStart, &gpu);
    gpu.ptr += (UINT64)e->srv_slot * s->srv_stride;
    e->srv_gpu = gpu;

    e->width  = w;
    e->height = h;
    return TRUE;
}

// Draw queued text runs. Called from end_frame after the rect batch
// flush. Binds the text pipeline + descriptor heap once, then walks
// the runs array -- each run gets its own SRV binding + draw call.
static void _d3d12_renderer_internal__flush_text(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    if (s->text_first_undrawn_run >= s->text_run_count)
    {
        return;
    }

    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    VCALL(list, SetGraphicsRootSignature, s->tx_root_sig);
    ID3D12DescriptorHeap* heaps[1] = { s->srv_heap };
    VCALL(list, SetDescriptorHeaps, 1, heaps);
    VCALL(list, SetPipelineState, s->text_pso);

    float pc[4] = { (float)s->viewport_w, (float)s->viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);

    // Append-only buffer: each run already records its own
    // vert_start offset into text_vbo, so a single VBV over the
    // whole buffer lets each run's DrawInstanced pick its slice
    // via StartVertexLocation. Don't reset vert_count or run_count
    // here -- same reason as flush_rects; subsequent glyph writes
    // must go to HIGHER offsets to avoid stomping these still-
    // pending draws.
    D3D12_VERTEX_BUFFER_VIEW vbv = { 0 };
    vbv.BufferLocation = VCALL0(s->text_vbo[s->frame_slot], GetGPUVirtualAddress);
    vbv.SizeInBytes    = (UINT)(sizeof(_d3d12_renderer_internal__tx_vertex_t) * s->text_vert_count);
    vbv.StrideInBytes  = sizeof(_d3d12_renderer_internal__tx_vertex_t);
    VCALL(list, IASetVertexBuffers, 0, 1, &vbv);
    VCALL(list, IASetPrimitiveTopology, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (int i = s->text_first_undrawn_run; i < s->text_run_count; i++)
    {
        _d3d12_renderer_internal__text_run_t* run = &s->text_runs[i];
        if (run->atlas == NULL_POINTER || run->vert_count <= 0)
        {
            continue;
        }
        VCALL(list, SetGraphicsRootDescriptorTable, 1, run->atlas->srv_gpu);
        VCALL(list, DrawInstanced, (UINT)run->vert_count, 1, (UINT)run->vert_start, 0);
    }

    s->text_first_undrawn_run = s->text_run_count;
}

// Restore rect pipeline + root sig binding. Called after a text or
// image draw swapped in the tx root sig; subsequent submit_rect
// flushes expect the rect pipeline active.
static void _d3d12_renderer_internal__bind_rect_pipeline(ID3D12GraphicsCommandList* list)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    VCALL(list, SetGraphicsRootSignature, s->root_sig);
    VCALL(list, SetPipelineState, s->rect_pso);
    float pc[4] = { (float)s->viewport_w, (float)s->viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);
}

// Re-establish the full per-frame-recording state that begin_frame
// sets up: allocator + cmd list Reset, root sig, viewport, scissor
// (honoring the active scissor stack depth), RTV, root constants.
// blur_region uses this after closing + executing + waiting on the
// cmd list mid-frame to drain the readback copy: the cmd list comes
// back empty + state-less and end_frame's path needs the same
// invariants begin_frame established. Critically: omitting
// OMSetRenderTargets here is the silent-no-pixels failure mode
// (DrawInstanced with no RTV draws nothing) that this code is
// careful to avoid.
static void _d3d12_renderer_internal__rebind_frame_after_drain(void)
{
    _d3d12_renderer_internal__state_t* s = &_d3d12_renderer_internal__g;
    ID3D12GraphicsCommandList* list = s->cmd_lists[s->frame_slot];

    VCALL0(s->allocators[s->frame_slot], Reset);
    VCALL(list, Reset, s->allocators[s->frame_slot], s->rect_pso);

    VCALL(list, SetGraphicsRootSignature, s->root_sig);

    D3D12_VIEWPORT vp = { 0 };
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = (float)s->viewport_w;
    vp.Height   = (float)s->viewport_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    VCALL(list, RSSetViewports, 1, &vp);

    D3D12_RECT sc = { 0 };
    if (s->scissor_depth > 0)
    {
        sc = s->scissors[s->scissor_depth - 1].rect;
    }
    else
    {
        sc.left   = 0;
        sc.top    = 0;
        sc.right  = (LONG)s->viewport_w;
        sc.bottom = (LONG)s->viewport_h;
    }
    VCALL(list, RSSetScissorRects, 1, &sc);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->swap_rtvs[s->backbuffer_index];
    VCALL(list, OMSetRenderTargets, 1, &rtv, FALSE, NULL_POINTER);

    float pc[4] = { (float)s->viewport_w, (float)s->viewport_h, 0.0f, 0.0f };
    VCALL(list, SetGraphicsRoot32BitConstants, 0, 4, pc, 0);
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
    int64 w = (int64)_d3d12_renderer_internal__g.viewport_w;
    int64 h = (int64)_d3d12_renderer_internal__g.viewport_h;

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

    _d3d12_renderer_internal__capture_out  = out_rgba;
    _d3d12_renderer_internal__capture_cap  = cap;
    _d3d12_renderer_internal__capture_done = FALSE;

    return TRUE;
}

/**
 * @brief tells whether the armed capture has been copied out (it is, once renderer__end_frame ran after the request)
 *
 * @return boole -> TRUE after the copy
 */
boole renderer__capture_done(void)
{
    return _d3d12_renderer_internal__capture_done;
}

// Ordinary standalone scenes can use this renderer directly.
GUI_API boole renderer_backend__needs_compositor(void) { return FALSE; }

// Layer mode is not implemented for this backend yet; the whole-frame
// compositor draws its extended scenes (renderer_backend.h).
GUI_API boole renderer_backend__supports_layers(void)
{
    return FALSE;
}

GUI_API void* renderer_backend__create_layer_texture(const ubyte* premultiplied_rgba, int width, int height)
{
    (void)premultiplied_rgba;
    (void)width;
    (void)height;
    return NULL_POINTER;
}

GUI_API void renderer_backend__submit_layer(void* texture, const float* corners, float opacity, int blend_mode)
{
    (void)texture;
    (void)corners;
    (void)opacity;
    (void)blend_mode;
}

GUI_API boole renderer_backend__update_layer_texture(void* texture, const ubyte* premultiplied_rgba, int width, int height)
{
    (void)texture;
    (void)premultiplied_rgba;
    (void)width;
    (void)height;
    return FALSE;
}

GUI_API boole renderer_backend__read_region(int x, int y, int w, int h, ubyte* out_rgba)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)out_rgba;
    return FALSE;
}

GUI_API boole renderer_backend__begin_layer(int width, int height)
{
    (void)width;
    (void)height;
    return FALSE;
}

GUI_API void* renderer_backend__end_layer(void)
{
    return NULL_POINTER;
}

GUI_API void renderer_backend__submit_tiles(gui_rect_t box, float radius, gui_rect_t tile, int columns, int rows, const gui_gradient_t* gradient, gui_color_t color, float scale, float opacity)
{
    (void)box;
    (void)radius;
    (void)tile;
    (void)columns;
    (void)rows;
    (void)gradient;
    (void)color;
    (void)scale;
    (void)opacity;
}

GUI_API boole renderer_backend__blur_layer(int x, int y, int w, int h, int radius) { (void)x; (void)y; (void)w; (void)h; (void)radius; return FALSE; }
