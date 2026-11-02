// backends/embedded/embedded_d3d9.c
//
// Direct3D 9 backend for drawing meek-ui inside a frame the host owns.
// The host calls begin_frame / end_frame between its own BeginScene and
// EndScene (an EndScene or Present hook in an overlay); the standalone
// d3d9 renderer's adopt mode does the drawing and never clears, begins,
// ends, presents or resets.
//
// State backup: one state block created with D3DSBT_ALL is kept for the
// life of the attachment. Capture at the start of the frame records every
// render state, sampler state, texture, stream, declaration, shader and
// constant the host had set; Apply at the end puts them all back. The
// world, view and projection transforms and the viewport are read and
// written separately on top of that, because the documentation is silent
// on whether a state block brings transforms back and overlay work has
// shown drivers where it does not. The state block lives in the default
// pool, so it is dropped before the host resets its device and recreated
// on the first frame afterwards.
//
// Render states the host may have changed that the standalone renderer
// never touches (it starts from a fresh device) are forced to what the
// renderer's pipelines assume at the start of every frame: solid fill, no
// stencil, no alpha test, no fog, no clip planes, all color channels, no
// sRGB write, no depth write.

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "d3d9_renderer.h"
#include "embedded_d3d9.h"
#include "third_party/log.h"

static IDirect3DDevice9* g_embedded_d3d9_device = NULL_POINTER;
static IDirect3DStateBlock9* g_embedded_d3d9_state_block = NULL_POINTER;
static D3DMATRIX g_embedded_d3d9_world;
static D3DMATRIX g_embedded_d3d9_view;
static D3DMATRIX g_embedded_d3d9_projection;
static D3DVIEWPORT9 g_embedded_d3d9_viewport;
static boole g_embedded_d3d9_transforms_valid = FALSE;
static boole g_embedded_d3d9_viewport_valid = FALSE;
static boole g_embedded_d3d9_attached = FALSE;
static boole g_embedded_d3d9_in_frame = FALSE;
static boole g_embedded_d3d9_lost = FALSE;
static UINT g_embedded_d3d9_streams = 1;
static UINT g_embedded_d3d9_targets = 1;
static IDirect3DSurface9* g_embedded_d3d9_saved_targets[4];
static IDirect3DSurface9* g_embedded_d3d9_saved_depth;
static boole g_embedded_d3d9_mixed_vp;
static BOOL g_embedded_d3d9_saved_software_vp;

static boole _embedded_d3d9_internal__ensure_state_block(void);
static void _embedded_d3d9_internal__apply_frame_state(void);

/**
 * @brief creates the state block on first use and after a device reset dropped it
 *
 * @return boole -> TRUE when a state block exists
 */
static boole _embedded_d3d9_internal__ensure_state_block(void)
{
    HRESULT hr = 0;

    if (g_embedded_d3d9_state_block != NULL_POINTER)
    {
        return TRUE;
    }

    hr = IDirect3DDevice9_CreateStateBlock(g_embedded_d3d9_device, D3DSBT_ALL, &g_embedded_d3d9_state_block);

    if (FAILED(hr) || g_embedded_d3d9_state_block == NULL_POINTER)
    {
        log_error("%s %08lX", "embedded_d3d9: CreateStateBlock failed, hresult", (unsigned long)hr);
        g_embedded_d3d9_state_block = NULL_POINTER;
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief forces the render states the renderer's pipelines assume but never set, because a game may have left any of them changed
 *
 * @return void
 */
static void _embedded_d3d9_internal__apply_frame_state(void)
{
    IDirect3DDevice9* dev = g_embedded_d3d9_device;
    UINT stream;
    int coord;

    // A state block saves the host but does not establish our pipeline.
    // Reset every state consumed by our shader-based triangle passes rather
    // than relying on the defaults of a freshly created standalone device.
    for (stream = 0; stream < g_embedded_d3d9_streams; ++stream)
        IDirect3DDevice9_SetStreamSourceFreq(dev, stream, 1);
    for (coord = 0; coord < 16; ++coord)
        IDirect3DDevice9_SetRenderState(dev, (D3DRENDERSTATETYPE)((coord < 8 ? D3DRS_WRAP0 : D3DRS_WRAP8) + (coord % 8)), 0);

    IDirect3DDevice9_SetRenderState(dev, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPING, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DEPTHBIAS, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SLOPESCALEDEPTHBIAS, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_MULTISAMPLEMASK, 0xffffffffu);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    // The UI writes COLOR0 only. Do not damage a host's additional targets.
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE1, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE2, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE3, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRGBWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    // flat shading also applies to the colour outputs of a vertex shader;
    // a game that draws its own geometry flat would leave every vertex
    // colour gradient of the UI flat as well
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, 0);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXANISOTROPY, 1);
    // a game's sharpening bias or skipped top mip levels on sampler 0
    // would blur or sharpen the UI's image mips; the glyph atlas has one
    // level and is not affected either way
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPMAPLODBIAS, 0);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, 0);
}

/**
 * @brief adopts the host's Direct3D 9 device and builds the renderer's pipelines on it
 *
 * @param IDirect3DDevice9* device -> host-owned device; no reference is taken, it must outlive embedded_d3d9__detach
 *
 * @attention call memory_manager__init and widget_registry__bootstrap_builtins before, font__init after
 *
 * @return boole -> TRUE when attached
 */
boole embedded_d3d9__attach(IDirect3DDevice9* device)
{
    D3DCAPS9 caps;
    D3DDEVICE_CREATION_PARAMETERS creation;
    IDirect3DStateBlock9* initial_state = NULL_POINTER;
    boole initialized;
    if (g_embedded_d3d9_attached == TRUE)
    {
        log_warn("%s", "embedded_d3d9__attach: already attached");
        return TRUE;
    }

    if (device == NULL_POINTER)
    {
        log_error("%s", "embedded_d3d9__attach: null device");
        return FALSE;
    }

    if (FAILED(IDirect3DDevice9_GetDeviceCaps(device, &caps))) return FALSE;
    if (FAILED(IDirect3DDevice9_GetCreationParameters(device, &creation))) return FALSE;
    g_embedded_d3d9_mixed_vp = (creation.BehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0;
    // This backend uses vs_3_0/ps_3_0. Report an unsupported device instead
    // of attempting to render with missing shaders.
    if (caps.VertexShaderVersion < D3DVS_VERSION(3, 0) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0))
    {
        log_error("%s", "embedded_d3d9 requires shader model 3.0");
        return FALSE;
    }
    if (FAILED(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &initial_state))) return FALSE;
    g_embedded_d3d9_saved_software_vp = IDirect3DDevice9_GetSoftwareVertexProcessing(device);
    if (g_embedded_d3d9_mixed_vp) IDirect3DDevice9_SetSoftwareVertexProcessing(device, FALSE);
    initialized = renderer__init_adopt_d3d9(device);
    IDirect3DStateBlock9_Apply(initial_state);
    if (g_embedded_d3d9_mixed_vp) IDirect3DDevice9_SetSoftwareVertexProcessing(device, g_embedded_d3d9_saved_software_vp);
    IDirect3DStateBlock9_Release(initial_state);
    if (initialized == FALSE)
    {
        return FALSE;
    }

    g_embedded_d3d9_device = device;
    g_embedded_d3d9_state_block = NULL_POINTER;
    g_embedded_d3d9_attached = TRUE;
    g_embedded_d3d9_in_frame = FALSE;
    g_embedded_d3d9_lost = FALSE;
    g_embedded_d3d9_streams = caps.MaxStreams;
    g_embedded_d3d9_targets = caps.NumSimultaneousRTs < 4 ? caps.NumSimultaneousRTs : 4;

    return TRUE;
}

/**
 * @brief tears the renderer down and releases the state block; the host's device is untouched
 *
 * @return void
 */
void embedded_d3d9__detach(void)
{
    if (g_embedded_d3d9_attached == FALSE)
    {
        return;
    }

    if (g_embedded_d3d9_in_frame == TRUE)
    {
        embedded_d3d9__end_frame();
    }

    if (g_embedded_d3d9_state_block != NULL_POINTER)
    {
        IDirect3DStateBlock9_Release(g_embedded_d3d9_state_block);
        g_embedded_d3d9_state_block = NULL_POINTER;
    }

    renderer__shutdown();

    g_embedded_d3d9_device = NULL_POINTER;
    g_embedded_d3d9_attached = FALSE;
}

/**
 * @brief tells whether attach succeeded and detach has not run
 *
 * @return boole -> TRUE while attached
 */
boole embedded_d3d9__is_attached(void)
{
    return g_embedded_d3d9_attached;
}

/**
 * @brief releases the state block and the renderer's default-pool objects; the host calls this before it resets its device
 *
 * @return void
 */
void embedded_d3d9__on_device_lost(void)
{
    if (g_embedded_d3d9_attached == FALSE)
    {
        return;
    }

    if (g_embedded_d3d9_in_frame) embedded_d3d9__end_frame();
    g_embedded_d3d9_lost = TRUE;
    if (g_embedded_d3d9_state_block != NULL_POINTER)
    {
        IDirect3DStateBlock9_Release(g_embedded_d3d9_state_block);
        g_embedded_d3d9_state_block = NULL_POINTER;
    }

    renderer__d3d9_on_device_lost();
}

/**
 * @brief recreates the renderer's default-pool objects; the host calls this after a successful Reset
 *
 * @return boole -> TRUE when drawing may resume
 */
boole embedded_d3d9__on_device_reset(void)
{
    if (g_embedded_d3d9_attached == FALSE)
    {
        return FALSE;
    }

    g_embedded_d3d9_lost = !renderer__d3d9_on_device_reset();
    return !g_embedded_d3d9_lost;
}

/**
 * @brief captures the host's pipeline state and starts a meek-ui frame inside the host's scene
 *
 * @param int64 width -> back buffer width in pixels
 * @param int64 height -> back buffer height in pixels
 *
 * @attention must run between the host's BeginScene and EndScene
 *
 * @return boole -> FALSE when not attached or when the state could not be captured
 */
boole embedded_d3d9__begin_frame(int64 width, int64 height)
{
    gui_color_t no_clear = { 0.0f, 0.0f, 0.0f, 0.0f };
    IDirect3DDevice9* dev = g_embedded_d3d9_device;
    HRESULT hr = 0;
    UINT target;

    if (g_embedded_d3d9_attached == FALSE || g_embedded_d3d9_lost || width <= 0 || height <= 0)
    {
        return FALSE;
    }

    if (g_embedded_d3d9_in_frame == TRUE)
    {
        log_warn("%s", "embedded_d3d9__begin_frame: frame already open, ending it first");
        embedded_d3d9__end_frame();
    }

    if (_embedded_d3d9_internal__ensure_state_block() == FALSE)
    {
        return FALSE;
    }

    hr = IDirect3DStateBlock9_Capture(g_embedded_d3d9_state_block);

    if (FAILED(hr))
    {
        log_error("%s %08lX", "embedded_d3d9__begin_frame: state block Capture failed, hresult", (unsigned long)hr);
        return FALSE;
    }

    // transforms and viewport on top of the state block; a pure device refuses the Get calls, then the block alone has to do
    g_embedded_d3d9_transforms_valid = FALSE;

    if (SUCCEEDED(IDirect3DDevice9_GetTransform(dev, D3DTS_WORLD, &g_embedded_d3d9_world)) && SUCCEEDED(IDirect3DDevice9_GetTransform(dev, D3DTS_VIEW, &g_embedded_d3d9_view)) && SUCCEEDED(IDirect3DDevice9_GetTransform(dev, D3DTS_PROJECTION, &g_embedded_d3d9_projection)))
    {
        g_embedded_d3d9_transforms_valid = TRUE;
    }

    g_embedded_d3d9_viewport_valid = SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &g_embedded_d3d9_viewport)) ? TRUE : FALSE;

    // Render targets/depth surfaces and software VP are not in D3DSBT_ALL.
    // Disable extra outputs while the UI's layers temporarily bind RT0.
    // Otherwise a deferred renderer's MRTs can invalidate draws or receive
    // unwanted writes. Keep references only for this begin/end pair.
    if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &g_embedded_d3d9_saved_targets[0]))) return FALSE;
    for (target = 1; target < g_embedded_d3d9_targets; ++target) {
        g_embedded_d3d9_saved_targets[target] = NULL_POINTER;
        IDirect3DDevice9_GetRenderTarget(dev, target, &g_embedded_d3d9_saved_targets[target]);
        IDirect3DDevice9_SetRenderTarget(dev, target, NULL_POINTER);
    }
    g_embedded_d3d9_saved_depth = NULL_POINTER;
    IDirect3DDevice9_GetDepthStencilSurface(dev, &g_embedded_d3d9_saved_depth);
    IDirect3DDevice9_SetDepthStencilSurface(dev, NULL_POINTER);
    g_embedded_d3d9_saved_software_vp = IDirect3DDevice9_GetSoftwareVertexProcessing(dev);
    if (g_embedded_d3d9_mixed_vp) IDirect3DDevice9_SetSoftwareVertexProcessing(dev, FALSE);

    _embedded_d3d9_internal__apply_frame_state();
    renderer__begin_frame(width, height, no_clear);
    g_embedded_d3d9_in_frame = TRUE;

    return TRUE;
}

/**
 * @brief flushes meek-ui's pending draws and puts the host's state block, transforms and viewport back
 *
 * @return void
 */
void embedded_d3d9__end_frame(void)
{
    IDirect3DDevice9* dev = g_embedded_d3d9_device;
    UINT target;

    if (g_embedded_d3d9_in_frame == FALSE)
    {
        return;
    }

    renderer__end_frame();

    // Bind surfaces first: SetRenderTarget changes the viewport implicitly.
    for (target = 0; target < g_embedded_d3d9_targets; ++target) {
        IDirect3DDevice9_SetRenderTarget(dev, target, g_embedded_d3d9_saved_targets[target]);
        if (g_embedded_d3d9_saved_targets[target]) IDirect3DSurface9_Release(g_embedded_d3d9_saved_targets[target]);
        g_embedded_d3d9_saved_targets[target] = NULL_POINTER;
    }
    IDirect3DDevice9_SetDepthStencilSurface(dev, g_embedded_d3d9_saved_depth);
    if (g_embedded_d3d9_saved_depth) IDirect3DSurface9_Release(g_embedded_d3d9_saved_depth);
    g_embedded_d3d9_saved_depth = NULL_POINTER;
    if (g_embedded_d3d9_mixed_vp) IDirect3DDevice9_SetSoftwareVertexProcessing(dev, g_embedded_d3d9_saved_software_vp);

    if (g_embedded_d3d9_state_block != NULL_POINTER)
    {
        IDirect3DStateBlock9_Apply(g_embedded_d3d9_state_block);
    }

    if (g_embedded_d3d9_transforms_valid == TRUE)
    {
        IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &g_embedded_d3d9_world);
        IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &g_embedded_d3d9_view);
        IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &g_embedded_d3d9_projection);
    }

    if (g_embedded_d3d9_viewport_valid == TRUE)
    {
        IDirect3DDevice9_SetViewport(dev, &g_embedded_d3d9_viewport);
    }

    g_embedded_d3d9_in_frame = FALSE;
}
