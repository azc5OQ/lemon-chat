// backends/embedded/embedded_d3d11.c
//
// Direct3D 11 backend for drawing meek-ui inside a frame the host owns.
// It sits on top of the standalone d3d11 renderer's adopt mode (which
// already skips clear and present) and adds what an overlay cannot live
// without: the host's pipeline state is backed up before meek-ui draws
// and restored afterwards, releasing every reference the backup took.
// A game keeps rendering with exactly the state it had bound and never
// notices that another renderer ran in between; that is what made the
// Nuklear overlay backends this is modeled on reliable.
//
// What is captured: render targets and depth-stencil view, viewports,
// scissor rects, rasterizer / blend / depth-stencil states, the pixel
// shader's first shader resource and sampler, the vertex / pixel /
// geometry shaders with their class instances, the first vertex- and
// pixel-shader constant buffers, primitive topology, index buffer,
// the first vertex buffer, the input layout, and rendering predication. That is a superset of
// what the meek-ui renderer touches.

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "d3d11_renderer.h"
#include "embedded_d3d11.h"
#include "clib/stdlib.h"
#include "third_party/log.h"

#define EMBEDDED_D3D11_MAX_CLASS_INSTANCES 256
#define EMBEDDED_D3D11_MAX_VIEWPORTS D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
#define EMBEDDED_D3D11_MAX_RENDER_TARGETS D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT

// everything the host had bound when embedded_d3d11__begin_frame ran
typedef struct _embedded_d3d11_internal__backup_t
{
    UINT scissor_count;
    D3D11_RECT scissors[EMBEDDED_D3D11_MAX_VIEWPORTS];
    UINT viewport_count;
    D3D11_VIEWPORT viewports[EMBEDDED_D3D11_MAX_VIEWPORTS];
    ID3D11RenderTargetView* render_targets[EMBEDDED_D3D11_MAX_RENDER_TARGETS];
    ID3D11DepthStencilView* depth_stencil_view;
    ID3D11RasterizerState* rasterizer_state;
    ID3D11BlendState* blend_state;
    FLOAT blend_factor[4];
    UINT sample_mask;
    ID3D11DepthStencilState* depth_stencil_state;
    UINT stencil_ref;
    ID3D11ShaderResourceView* ps_shader_resource;
    ID3D11SamplerState* ps_sampler;
    ID3D11PixelShader* ps;
    ID3D11VertexShader* vs;
    ID3D11GeometryShader* gs;
    UINT ps_instance_count;
    UINT vs_instance_count;
    UINT gs_instance_count;
    ID3D11ClassInstance* ps_instances[EMBEDDED_D3D11_MAX_CLASS_INSTANCES];
    ID3D11ClassInstance* vs_instances[EMBEDDED_D3D11_MAX_CLASS_INSTANCES];
    ID3D11ClassInstance* gs_instances[EMBEDDED_D3D11_MAX_CLASS_INSTANCES];
    ID3D11HullShader* hs;
    ID3D11DomainShader* ds;
    UINT hs_instance_count;
    UINT ds_instance_count;
    ID3D11ClassInstance* hs_instances[EMBEDDED_D3D11_MAX_CLASS_INSTANCES];
    ID3D11ClassInstance* ds_instances[EMBEDDED_D3D11_MAX_CLASS_INSTANCES];
    ID3D11Buffer* vs_constant_buffer;
    ID3D11Buffer* ps_constant_buffer;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11Buffer* index_buffer;
    DXGI_FORMAT index_format;
    UINT index_offset;
    ID3D11Buffer* vertex_buffer;
    UINT vertex_stride;
    UINT vertex_offset;
    ID3D11InputLayout* input_layout;
    ID3D11Predicate* predicate;
    BOOL predicate_value;
} _embedded_d3d11_internal__backup_t;

static ID3D11Device* g_embedded_d3d11_device = NULL_POINTER;
static ID3D11DeviceContext* g_embedded_d3d11_context = NULL_POINTER;
static IDXGISwapChain* g_embedded_d3d11_swapchain = NULL_POINTER;
static boole g_embedded_d3d11_attached = FALSE;
static boole g_embedded_d3d11_in_frame = FALSE;
static _embedded_d3d11_internal__backup_t g_embedded_d3d11_backup;

static void _embedded_d3d11_internal__backup(ID3D11DeviceContext* context);
static void _embedded_d3d11_internal__restore(ID3D11DeviceContext* context);
static void _embedded_d3d11_internal__release_instances(ID3D11ClassInstance** instances, UINT count);

/**
 * @brief captures every piece of pipeline state meek-ui may touch; each Get call adds a reference that restore releases
 *
 * @param ID3D11DeviceContext* context -> the host's immediate context
 *
 * @return void
 */
static void _embedded_d3d11_internal__backup(ID3D11DeviceContext* context)
{
    _embedded_d3d11_internal__backup_t* b = &g_embedded_d3d11_backup;

    stdlib__memset(b, 0, sizeof(*b));

    b->scissor_count = EMBEDDED_D3D11_MAX_VIEWPORTS;
    b->viewport_count = EMBEDDED_D3D11_MAX_VIEWPORTS;
    b->ps_instance_count = EMBEDDED_D3D11_MAX_CLASS_INSTANCES;
    b->vs_instance_count = EMBEDDED_D3D11_MAX_CLASS_INSTANCES;
    b->gs_instance_count = EMBEDDED_D3D11_MAX_CLASS_INSTANCES;
    b->hs_instance_count = EMBEDDED_D3D11_MAX_CLASS_INSTANCES;
    b->ds_instance_count = EMBEDDED_D3D11_MAX_CLASS_INSTANCES;

    ID3D11DeviceContext_RSGetScissorRects(context, &b->scissor_count, b->scissors);
    ID3D11DeviceContext_RSGetViewports(context, &b->viewport_count, b->viewports);
    ID3D11DeviceContext_OMGetRenderTargets(context, EMBEDDED_D3D11_MAX_RENDER_TARGETS, b->render_targets, &b->depth_stencil_view);
    ID3D11DeviceContext_RSGetState(context, &b->rasterizer_state);
    ID3D11DeviceContext_OMGetBlendState(context, &b->blend_state, b->blend_factor, &b->sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(context, &b->depth_stencil_state, &b->stencil_ref);
    ID3D11DeviceContext_PSGetShaderResources(context, 0, 1, &b->ps_shader_resource);
    ID3D11DeviceContext_PSGetSamplers(context, 0, 1, &b->ps_sampler);
    ID3D11DeviceContext_PSGetShader(context, &b->ps, b->ps_instances, &b->ps_instance_count);
    ID3D11DeviceContext_VSGetShader(context, &b->vs, b->vs_instances, &b->vs_instance_count);
    ID3D11DeviceContext_GSGetShader(context, &b->gs, b->gs_instances, &b->gs_instance_count);
    ID3D11DeviceContext_HSGetShader(context, &b->hs, b->hs_instances, &b->hs_instance_count);
    ID3D11DeviceContext_DSGetShader(context, &b->ds, b->ds_instances, &b->ds_instance_count);
    ID3D11DeviceContext_VSGetConstantBuffers(context, 0, 1, &b->vs_constant_buffer);
    ID3D11DeviceContext_PSGetConstantBuffers(context, 0, 1, &b->ps_constant_buffer);
    ID3D11DeviceContext_IAGetPrimitiveTopology(context, &b->topology);
    ID3D11DeviceContext_IAGetIndexBuffer(context, &b->index_buffer, &b->index_format, &b->index_offset);
    ID3D11DeviceContext_IAGetVertexBuffers(context, 0, 1, &b->vertex_buffer, &b->vertex_stride, &b->vertex_offset);
    ID3D11DeviceContext_IAGetInputLayout(context, &b->input_layout);
    ID3D11DeviceContext_GetPredication(context, &b->predicate, &b->predicate_value);
}

/**
 * @brief releases the class-instance references a *GetShader call handed out
 *
 * @param ID3D11ClassInstance** instances -> the array filled by the Get call
 * @param UINT count -> how many entries the Get call reported
 *
 * @return void
 */
static void _embedded_d3d11_internal__release_instances(ID3D11ClassInstance** instances, UINT count)
{
    UINT i = 0;

    for (i = 0; i < count && i < EMBEDDED_D3D11_MAX_CLASS_INSTANCES; i++)
    {
        if (instances[i] != NULL_POINTER)
        {
            ID3D11ClassInstance_Release(instances[i]);
            instances[i] = NULL_POINTER;
        }
    }
}

/**
 * @brief puts the captured state back and releases every reference the backup took
 *
 * @param ID3D11DeviceContext* context -> the host's immediate context
 *
 * @attention order matters only in that shaders are rebound before their class instances are released; everything else is independent
 *
 * @return void
 */
static void _embedded_d3d11_internal__restore(ID3D11DeviceContext* context)
{
    _embedded_d3d11_internal__backup_t* b = &g_embedded_d3d11_backup;
    UINT i = 0;

    ID3D11DeviceContext_RSSetScissorRects(context, b->scissor_count, b->scissors);
    ID3D11DeviceContext_RSSetViewports(context, b->viewport_count, b->viewports);

    ID3D11DeviceContext_OMSetRenderTargets(context, EMBEDDED_D3D11_MAX_RENDER_TARGETS, b->render_targets, b->depth_stencil_view);

    for (i = 0; i < EMBEDDED_D3D11_MAX_RENDER_TARGETS; i++)
    {
        if (b->render_targets[i] != NULL_POINTER)
        {
            ID3D11RenderTargetView_Release(b->render_targets[i]);
        }
    }

    if (b->depth_stencil_view != NULL_POINTER)
    {
        ID3D11DepthStencilView_Release(b->depth_stencil_view);
    }

    ID3D11DeviceContext_RSSetState(context, b->rasterizer_state);

    if (b->rasterizer_state != NULL_POINTER)
    {
        ID3D11RasterizerState_Release(b->rasterizer_state);
    }

    ID3D11DeviceContext_OMSetBlendState(context, b->blend_state, b->blend_factor, b->sample_mask);

    if (b->blend_state != NULL_POINTER)
    {
        ID3D11BlendState_Release(b->blend_state);
    }

    ID3D11DeviceContext_OMSetDepthStencilState(context, b->depth_stencil_state, b->stencil_ref);

    if (b->depth_stencil_state != NULL_POINTER)
    {
        ID3D11DepthStencilState_Release(b->depth_stencil_state);
    }

    ID3D11DeviceContext_PSSetShaderResources(context, 0, 1, &b->ps_shader_resource);

    if (b->ps_shader_resource != NULL_POINTER)
    {
        ID3D11ShaderResourceView_Release(b->ps_shader_resource);
    }

    ID3D11DeviceContext_PSSetSamplers(context, 0, 1, &b->ps_sampler);

    if (b->ps_sampler != NULL_POINTER)
    {
        ID3D11SamplerState_Release(b->ps_sampler);
    }

    ID3D11DeviceContext_PSSetShader(context, b->ps, b->ps_instances, b->ps_instance_count);

    if (b->ps != NULL_POINTER)
    {
        ID3D11PixelShader_Release(b->ps);
    }

    _embedded_d3d11_internal__release_instances(b->ps_instances, b->ps_instance_count);

    ID3D11DeviceContext_VSSetShader(context, b->vs, b->vs_instances, b->vs_instance_count);

    if (b->vs != NULL_POINTER)
    {
        ID3D11VertexShader_Release(b->vs);
    }

    _embedded_d3d11_internal__release_instances(b->vs_instances, b->vs_instance_count);

    ID3D11DeviceContext_GSSetShader(context, b->gs, b->gs_instances, b->gs_instance_count);

    if (b->gs != NULL_POINTER)
    {
        ID3D11GeometryShader_Release(b->gs);
    }

    _embedded_d3d11_internal__release_instances(b->gs_instances, b->gs_instance_count);

    ID3D11DeviceContext_HSSetShader(context, b->hs, b->hs_instances, b->hs_instance_count);

    if (b->hs != NULL_POINTER)
    {
        ID3D11HullShader_Release(b->hs);
    }

    _embedded_d3d11_internal__release_instances(b->hs_instances, b->hs_instance_count);

    ID3D11DeviceContext_DSSetShader(context, b->ds, b->ds_instances, b->ds_instance_count);

    if (b->ds != NULL_POINTER)
    {
        ID3D11DomainShader_Release(b->ds);
    }

    _embedded_d3d11_internal__release_instances(b->ds_instances, b->ds_instance_count);

    ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &b->vs_constant_buffer);

    if (b->vs_constant_buffer != NULL_POINTER)
    {
        ID3D11Buffer_Release(b->vs_constant_buffer);
    }

    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &b->ps_constant_buffer);

    if (b->ps_constant_buffer != NULL_POINTER)
    {
        ID3D11Buffer_Release(b->ps_constant_buffer);
    }

    ID3D11DeviceContext_IASetPrimitiveTopology(context, b->topology);
    ID3D11DeviceContext_IASetIndexBuffer(context, b->index_buffer, b->index_format, b->index_offset);

    if (b->index_buffer != NULL_POINTER)
    {
        ID3D11Buffer_Release(b->index_buffer);
    }

    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &b->vertex_buffer, &b->vertex_stride, &b->vertex_offset);

    if (b->vertex_buffer != NULL_POINTER)
    {
        ID3D11Buffer_Release(b->vertex_buffer);
    }

    ID3D11DeviceContext_IASetInputLayout(context, b->input_layout);

    if (b->input_layout != NULL_POINTER)
    {
        ID3D11InputLayout_Release(b->input_layout);
    }

    // GetPredication preserves the comparison value even with a null predicate.
    ID3D11DeviceContext_SetPredication(context, b->predicate, b->predicate_value);
    if (b->predicate != NULL_POINTER)
    {
        ID3D11Predicate_Release(b->predicate);
    }

    stdlib__memset(b, 0, sizeof(*b));
}

/**
 * @brief adopts the host's device, context and swap chain; builds the renderer's pipelines and its render target view of back buffer 0
 *
 * @param ID3D11Device* device -> host-owned device; stays owned by the host
 * @param ID3D11DeviceContext* context -> host-owned immediate context
 * @param IDXGISwapChain* swapchain -> host-owned swap chain the overlay draws into
 *
 * @attention call memory_manager__init and widget_registry__bootstrap_builtins before, font__init after; the renderer takes no references on the three objects, so the host must keep them alive until embedded_d3d11__detach
 *
 * @return boole -> TRUE when attached
 */
boole embedded_d3d11__attach(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapchain)
{
    if (g_embedded_d3d11_attached == TRUE)
    {
        log_warn("%s", "embedded_d3d11__attach: already attached");
        return TRUE;
    }

    if (device == NULL_POINTER || context == NULL_POINTER || swapchain == NULL_POINTER)
    {
        log_error("%s", "embedded_d3d11__attach: null device, context or swapchain");
        return FALSE;
    }

    if (renderer__init_adopt(device, context, swapchain) == FALSE)
    {
        return FALSE;
    }

    g_embedded_d3d11_device = device;
    g_embedded_d3d11_context = context;
    g_embedded_d3d11_swapchain = swapchain;
    g_embedded_d3d11_attached = TRUE;
    g_embedded_d3d11_in_frame = FALSE;

    return TRUE;
}

/**
 * @brief tears the renderer down; releases only what the backend created, never the host's objects
 *
 * @return void
 */
void embedded_d3d11__detach(void)
{
    if (g_embedded_d3d11_attached == FALSE)
    {
        return;
    }

    if (g_embedded_d3d11_in_frame == TRUE)
    {
        embedded_d3d11__end_frame();
    }

    renderer__shutdown();

    g_embedded_d3d11_device = NULL_POINTER;
    g_embedded_d3d11_context = NULL_POINTER;
    g_embedded_d3d11_swapchain = NULL_POINTER;
    g_embedded_d3d11_attached = FALSE;
}

/**
 * @brief tells whether attach succeeded and detach has not run
 *
 * @return boole -> TRUE while attached
 */
boole embedded_d3d11__is_attached(void)
{
    return g_embedded_d3d11_attached;
}

/**
 * @brief drops the renderer's view of the back buffer; the host must call this before it resizes the swap chain, since ResizeBuffers fails while any view of the old back buffer exists
 *
 * @attention the next embedded_d3d11__begin_frame re-creates the view against the current back buffer
 *
 * @return void
 */
void embedded_d3d11__release_render_target(void)
{
    if (g_embedded_d3d11_attached == FALSE)
    {
        return;
    }

    renderer__d3d11_release_render_target();
}

/**
 * @brief backs up the host's pipeline state and starts a meek-ui frame on the host's back buffer without clearing it
 *
 * @param int64 width -> back buffer width in pixels
 * @param int64 height -> back buffer height in pixels
 *
 * @attention call after the host drew its own frame and before it presents; everything meek-ui emits until embedded_d3d11__end_frame lands on top of the host's pixels
 *
 * @return boole -> FALSE when not attached
 */
boole embedded_d3d11__begin_frame(int64 width, int64 height)
{
    gui_color_t no_clear = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (g_embedded_d3d11_attached == FALSE)
    {
        return FALSE;
    }

    if (g_embedded_d3d11_in_frame == TRUE)
    {
        log_warn("%s", "embedded_d3d11__begin_frame: frame already open, ending it first");
        embedded_d3d11__end_frame();
    }

    _embedded_d3d11_internal__backup(g_embedded_d3d11_context);

    // A host occlusion predicate applies to Draw, Copy, Update and Clear too.
    // Keep every UI operation unconditional until the host state is restored.
    ID3D11DeviceContext_SetPredication(g_embedded_d3d11_context, NULL_POINTER, FALSE);

    // stages meek-ui never uses stay unbound while it draws: a host geometry
    // shader would run on the overlay's triangles and bound tessellation
    // shaders would make its triangle-list draws invalid; restore rebinds them
    ID3D11DeviceContext_GSSetShader(g_embedded_d3d11_context, NULL_POINTER, NULL_POINTER, 0);
    ID3D11DeviceContext_HSSetShader(g_embedded_d3d11_context, NULL_POINTER, NULL_POINTER, 0);
    ID3D11DeviceContext_DSSetShader(g_embedded_d3d11_context, NULL_POINTER, NULL_POINTER, 0);

    renderer__begin_frame(width, height, no_clear);
    g_embedded_d3d11_in_frame = TRUE;

    return TRUE;
}

/**
 * @brief flushes meek-ui's pending draws and restores the host's pipeline state; the host presents afterwards
 *
 * @return void
 */
void embedded_d3d11__end_frame(void)
{
    if (g_embedded_d3d11_in_frame == FALSE)
    {
        return;
    }

    renderer__end_frame();
    _embedded_d3d11_internal__restore(g_embedded_d3d11_context);
    g_embedded_d3d11_in_frame = FALSE;
}
