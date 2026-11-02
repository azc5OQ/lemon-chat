// backends/embedded/embedded_d3d12.c
//
// Direct3D 12 backend for drawing meek-ui inside a frame the host owns.
// The standalone d3d12 renderer's adopt mode uses the host's device, its
// direct command queue and its swap chain, but keeps its own command
// allocators, command lists, descriptor heaps, fence and pipeline state
// objects. Every meek-ui frame is one command list executed on the host's
// queue after the host's own lists and before the host presents.
//
// There is no state backup here on purpose: in Direct3D 12 the pipeline
// state lives in command lists and PSOs, and meek-ui never records into
// the host's lists. The two things the host must respect are the back
// buffer state (PRESENT when begin_frame runs, PRESENT again after
// end_frame) and swap chain resizes, which must release the views first.

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "d3d12_renderer.h"
#include "embedded_d3d12.h"
#include "third_party/log.h"

static boole g_embedded_d3d12_attached = FALSE;
static boole g_embedded_d3d12_in_frame = FALSE;

/**
 * @brief adopts the host's device, queue and swap chain and builds the renderer's own command objects and pipelines
 *
 * @param ID3D12Device* device -> host-owned device, borrowed
 * @param ID3D12CommandQueue* queue -> host-owned direct queue the overlay's command lists are executed on, borrowed
 * @param IDXGISwapChain* swapchain -> host-owned swap chain (any DXGI version that upgrades to IDXGISwapChain3)
 *
 * @attention call memory_manager__init and widget_registry__bootstrap_builtins before, font__init after
 *
 * @return boole -> TRUE when attached
 */
boole embedded_d3d12__attach(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain* swapchain)
{
    if (g_embedded_d3d12_attached == TRUE)
    {
        log_warn("%s", "embedded_d3d12__attach: already attached");
        return TRUE;
    }

    if (device == NULL_POINTER || queue == NULL_POINTER || swapchain == NULL_POINTER)
    {
        log_error("%s", "embedded_d3d12__attach: null device, queue or swapchain");
        return FALSE;
    }

    if (renderer__init_adopt_d3d12(device, queue, swapchain) == FALSE)
    {
        return FALSE;
    }

    g_embedded_d3d12_attached = TRUE;
    g_embedded_d3d12_in_frame = FALSE;

    return TRUE;
}

/**
 * @brief waits for the renderer's outstanding work and releases everything it created; the host's objects are untouched
 *
 * @return void
 */
void embedded_d3d12__detach(void)
{
    if (g_embedded_d3d12_attached == FALSE)
    {
        return;
    }

    if (g_embedded_d3d12_in_frame == TRUE)
    {
        embedded_d3d12__end_frame();
    }

    renderer__shutdown();
    g_embedded_d3d12_attached = FALSE;
}

/**
 * @brief tells whether attach succeeded and detach has not run
 *
 * @return boole -> TRUE while attached
 */
boole embedded_d3d12__is_attached(void)
{
    return g_embedded_d3d12_attached;
}

/**
 * @brief drops the renderer's references to the swap chain buffers; the host calls this before ResizeBuffers, the views are rebuilt on the next begin_frame
 *
 * @return void
 */
void embedded_d3d12__release_render_targets(void)
{
    if (g_embedded_d3d12_attached == FALSE)
    {
        return;
    }

    renderer__d3d12_release_render_targets();
}

/**
 * @brief starts a meek-ui frame on the current back buffer without clearing it
 *
 * @param int64 width -> back buffer width in pixels
 * @param int64 height -> back buffer height in pixels
 *
 * @attention the back buffer must be in the PRESENT state, which is the case after the host's own command lists transitioned it for presentation
 *
 * @return boole -> FALSE when not attached
 */
boole embedded_d3d12__begin_frame(int64 width, int64 height)
{
    gui_color_t no_clear = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (g_embedded_d3d12_attached == FALSE)
    {
        return FALSE;
    }

    if (g_embedded_d3d12_in_frame == TRUE)
    {
        log_warn("%s", "embedded_d3d12__begin_frame: frame already open, ending it first");
        embedded_d3d12__end_frame();
    }

    renderer__begin_frame(width, height, no_clear);
    g_embedded_d3d12_in_frame = TRUE;

    return TRUE;
}

/**
 * @brief closes the frame's command list, executes it on the host's queue and leaves the back buffer in the PRESENT state
 *
 * @return void
 */
void embedded_d3d12__end_frame(void)
{
    if (g_embedded_d3d12_in_frame == FALSE)
    {
        return;
    }

    renderer__end_frame();
    g_embedded_d3d12_in_frame = FALSE;
}
