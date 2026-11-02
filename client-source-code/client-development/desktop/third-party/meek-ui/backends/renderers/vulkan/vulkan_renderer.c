// Vulkan renderer shared by standalone Windows and embedded Windows/Linux.
// The embedded adapter borrows a device, graphics queue and color target;
// it records and submits private command buffers, never presents or owns the
// host swapchain. Public painting uses the portable compositor, followed by
// an RGBA texture draw. Shader modules come from checked-in SPIR-V headers.
// Target readback is fenced and returns top-down RGBA8. See embedded_vulkan.h
// for image-layout, semaphore, format and lifetime requirements.

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <vulkan/vulkan.h>

#if defined(_WIN32)
  #define VK_USE_PLATFORM_WIN32_KHR 1
  #include <vulkan/vulkan_win32.h>
#elif defined(__linux__) && !defined(__ANDROID__)
  // On Linux we support both X11 and Wayland surfaces. Which one
  // is actually used depends on what the platform layer hands us
  // as `native_window`. For the current X11 backend we only need
  // VK_KHR_xlib_surface; Wayland lands when the Wayland platform
  // does.
  #define VK_USE_PLATFORM_XLIB_KHR 1
  #include <X11/Xlib.h>
  #include <vulkan/vulkan_xlib.h>
#elif defined(__ANDROID__)
  #define VK_USE_PLATFORM_ANDROID_KHR 1
  #include <vulkan/vulkan_android.h>
#endif

// Checked-in shader bytecode: no runtime shader compiler dependency.
#include "vulkan_renderer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "types.h"
#include "gui.h"
#define MEEK_RENDERER_BACKEND
#include "renderer.h"
#include "renderer_backend.h"
#include "gradient.h"
#include "_visual_contract.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"
#include "clib/stdlib.h"

// ============================================================================
// constants
// ============================================================================

// Rect buffer sizes are bumped vs the d3d12 defaults because
// persistent-mapped UPLOAD buffers on Vulkan (host-visible + coherent)
// have the same mid-frame-flush stomp hazard: once a vkCmdDraw has
// been recorded against a vertex offset, subsequent CPU writes to
// those offsets pollute the still-pending draw. Fix is the same
// append-only cursor pattern used by d3d12. Buffers are sized for a
// whole frame's worth of verts at worst case.
#define _VULKAN_RENDERER_INTERNAL__MAX_QUADS         16384
#define _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD    6
#define _VULKAN_RENDERER_INTERNAL__MAX_FRAMES        2
#define _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG 8
#define _VULKAN_RENDERER_INTERNAL__MAX_SCISSORS      16
// Text pipeline limits: up to 16384 glyph quads (98K verts, ~3.1 MB
// per frame-in-flight) and 64 distinct text runs (one per atlas
// binding change). Matches the d3d12 profile.
#define _VULKAN_RENDERER_INTERNAL__MAX_TEXT_QUADS    16384
#define _VULKAN_RENDERER_INTERNAL__MAX_TEXT_RUNS     64
// Max concurrent atlas / image textures. font.c creates an atlas per
// (family, size); widget_image caches image textures. 128 covers
// reasonable asset libraries.
#define _VULKAN_RENDERER_INTERNAL__MAX_ATLASES       128

// ============================================================================
// vertex / state types
// ============================================================================

typedef struct _vulkan_renderer_internal__vertex_t
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
} _vulkan_renderer_internal__vertex_t;

// Text + image vertex: pos + color + uv = 8 floats. Smaller than the
// rect vertex (no SDF parameters needed — text samples an atlas, image
// samples an RGBA texture). Same layout the d3d12 text/image pipelines
// use so the visual contract stays uniform.
typedef struct _vulkan_renderer_internal__tx_vertex_t
{
    float x;   // 0x0
    float y;   // 0x4
    float r;   // 0x8
    float g;   // 0xC
    float b;   // 0x10
    float a;   // 0x14
    float u;   // 0x18
    float v;   // 0x1C
} _vulkan_renderer_internal__tx_vertex_t;

// Atlas / image record. One per R8 font atlas or RGBA image texture.
// The void* handle passed back to callers (font.c, widget_image.c)
// is a pointer to this struct.
typedef struct _vulkan_renderer_internal__atlas_entry_t
{
    VkImage         image;      // 0x0
    VkDeviceMemory  memory;     // 0x8
    VkImageView     view;       // 0x10
    // binds `image` at set=0, binding=0.
    VkDescriptorSet desc_set;   // 0x18
    int             width;      // 0x20
    int             height;     // 0x24
    boole           in_use;     // 0x28
    boole           pending_release;
} _vulkan_renderer_internal__atlas_entry_t;

// One run of text quads sharing an atlas. end_frame / flush_text binds
// the atlas's descriptor set once, then issues a single vkCmdDraw
// covering all the run's verts.
typedef struct _vulkan_renderer_internal__text_run_t
{
    _vulkan_renderer_internal__atlas_entry_t* atlas;        // 0x0
    int64                                   vert_start;   // 0x8
    int64                                   vert_count;   // 0x10
} _vulkan_renderer_internal__text_run_t;

typedef struct _vulkan_renderer_internal__scissor_entry_t
{
    VkRect2D rect;
    boole    active;
} _vulkan_renderer_internal__scissor_entry_t;

// ============================================================================
// global state
// ============================================================================
// Mirrors opengl3_renderer's _g layout: one struct to rule them
// all, flushed on shutdown.

typedef struct _vulkan_renderer_internal__state_t
{
    boole adopted;
    boole frame_ok;
    meek_vulkan_frame_t host_frame;
    VkSemaphore host_waits[16];
    VkBuffer capture_buffer;
    VkDeviceMemory capture_memory;
    VkDeviceSize capture_capacity;
    ubyte* capture_out;
    boole capture_done;
    // native window pass-through
    void* native_window;

    // instance / device / queue
    VkInstance       instance;
    VkPhysicalDevice phys_device;
    VkDevice         device;
    uint32_t         queue_family;
    VkQueue          queue;

    // surface + swapchain
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkFormat         swap_format;
    VkImageUsageFlags swap_usage;
    VkExtent2D       swap_extent;
    uint32_t         swap_image_count;
    VkImage          swap_images  [_VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG];
    VkImageView      swap_views   [_VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG];
    VkFramebuffer    framebuffers [_VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG];

    // render pass
    VkRenderPass     render_pass;

    // per-frame command buffers + sync
    VkCommandPool    command_pool;
    VkCommandBuffer  cmd_bufs    [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkSemaphore      sem_acquire [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkSemaphore      sem_present [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkFence          in_flight   [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    uint32_t         frame_index; // 0..MAX_FRAMES-1
    uint32_t         swap_image_index; // result of vkAcquireNextImageKHR

    // rect pipeline
    VkDescriptorSetLayout rect_dsl; // unused today (no textures) but reserved
    VkPipelineLayout      rect_pipeline_layout;
    VkPipeline            rect_pipeline;
    VkShaderModule        rect_vs;
    VkShaderModule        rect_fs;

    // Rect vertex buffer (host-visible, persistently mapped). Append-
    // only within a frame; `rect_draw_cursor` advances on flush so
    // mid-frame scissor flushes don't stomp the still-pending draw
    // (same discipline as d3d12).
    VkBuffer              rect_vbo   [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkDeviceMemory        rect_vbo_mem[_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    _vulkan_renderer_internal__vertex_t* rect_vbo_ptr[_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    int64                 rect_vert_count;
    int64                 rect_draw_cursor;

    // Text + image shared plumbing. Lazy-built on first atlas / image
    // request so a PoC that doesn't use text doesn't pay the shader-
    // compile cost.
    VkSampler                 tx_sampler; // linear, clamp.
    VkDescriptorSetLayout     tx_dsl; // 1 combined-image-sampler at set=0, binding=0.
    VkDescriptorPool          tx_descriptor_pool; // pool for per-atlas descriptor sets.
    VkPipelineLayout          text_pipeline_layout;
    VkPipeline                text_pipeline; // R8 sample + gamma reshape.
    VkPipelineLayout          image_pipeline_layout;
    VkPipeline                image_pipeline; // RGBA sample + tint.
    VkShaderModule            tx_vs; // shared VS.
    VkShaderModule            text_fs;
    VkShaderModule            image_fs;
    boole                     tx_built; // set true after first successful build.

    // Text per-frame VBO + runs + current atlas. Append-only like rects.
    VkBuffer              text_vbo     [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    VkDeviceMemory        text_vbo_mem [_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    _vulkan_renderer_internal__tx_vertex_t* text_vbo_ptr[_VULKAN_RENDERER_INTERNAL__MAX_FRAMES];
    int64                     text_vert_count;
    _vulkan_renderer_internal__text_run_t text_runs[_VULKAN_RENDERER_INTERNAL__MAX_TEXT_RUNS];
    int                       text_run_count;
    int                       text_first_undrawn_run;
    _vulkan_renderer_internal__atlas_entry_t* current_text_atlas;

    // Atlas / image pool. Fixed-size; allocated lazily in
    // create_atlas_r8 / create_texture_rgba and released in destroy.
    _vulkan_renderer_internal__atlas_entry_t atlas_pool[_VULKAN_RENDERER_INTERNAL__MAX_ATLASES];

    // frame state
    int64                 viewport_w;
    int64                 viewport_h;
    gui_color_t             clear_color;
    boole                 frame_active;

    // scissor stack
    _vulkan_renderer_internal__scissor_entry_t scissors[_VULKAN_RENDERER_INTERNAL__MAX_SCISSORS];
    int64                 scissor_depth;
} _vulkan_renderer_internal__state_t;

static _vulkan_renderer_internal__state_t _vulkan_renderer_internal__g;

// ============================================================================
// shader sources (GLSL 450)
// ============================================================================
// Same math as every other backend. GLSL 450 maps directly to
// Vulkan-flavored GLSL (push constants, explicit binding numbers,
// gl_VertexIndex instead of gl_VertexID). The SDF formula and AA
// band are identical to gles3_renderer.c.

#include "shaders/generated/rect.vert.spv.h"

#include "shaders/generated/rect.frag.spv.h"

// Text + image shared VS. Takes pos + color + uv, emits NDC + passthrough.
// Same shape as the rect VS but no SDF attributes.
#include "shaders/generated/text_image.vert.spv.h"

// Text FS: sample single-channel atlas, gamma-reshape, alpha-multiply
// with vertex color. Matches the d3d12 / d3d11 / opengl3 text shaders.
#include "shaders/generated/text.frag.spv.h"

// Image FS: sample RGBA texture and multiply by vertex tint.
#include "shaders/generated/image.frag.spv.h"

// ============================================================================
// forward decls
// ============================================================================

static boole _vulkan_renderer_internal__create_instance(void);
static void _vulkan_renderer_internal__record_capture(VkCommandBuffer cb);
static void _vulkan_renderer_internal__finish_capture(void);
static boole _vulkan_renderer_internal__create_surface(void* native_window);
static boole _vulkan_renderer_internal__pick_physical_device(void);
static boole _vulkan_renderer_internal__create_device(void);
static boole _vulkan_renderer_internal__create_swapchain(void);
static void  _vulkan_renderer_internal__destroy_swapchain(void);
static boole _vulkan_renderer_internal__create_render_pass(void);
static boole _vulkan_renderer_internal__create_framebuffers(void);
static boole _vulkan_renderer_internal__create_command_pool_and_buffers(void);
static boole _vulkan_renderer_internal__create_sync_primitives(void);
static boole _vulkan_renderer_internal__compile_shader(const uint32_t* code, size_t bytes, const char* tag, VkShaderModule* out);
static boole _vulkan_renderer_internal__create_rect_pipeline(void);
static boole _vulkan_renderer_internal__create_rect_vertex_buffers(void);
static uint32_t _vulkan_renderer_internal__find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
static void  _vulkan_renderer_internal__flush_rects(void);
// text + image pipeline setup (lazy, on first atlas or texture request):
static boole _vulkan_renderer_internal__ensure_tx_plumbing(void);
static boole _vulkan_renderer_internal__create_tx_sampler(void);
static boole _vulkan_renderer_internal__create_tx_descriptor_layout(void);
static boole _vulkan_renderer_internal__create_tx_descriptor_pool(void);
static boole _vulkan_renderer_internal__create_text_pipeline(void);
static boole _vulkan_renderer_internal__create_image_pipeline(void);
static boole _vulkan_renderer_internal__create_tx_vertex_buffers(void);
// atlas pool + upload:
static _vulkan_renderer_internal__atlas_entry_t* _vulkan_renderer_internal__alloc_atlas_slot(void);
static void _vulkan_renderer_internal__retire_atlases(void);
static void  _vulkan_renderer_internal__release_atlas(_vulkan_renderer_internal__atlas_entry_t* a);
static boole _vulkan_renderer_internal__upload_atlas(_vulkan_renderer_internal__atlas_entry_t* a, const ubyte* pixels, int w, int h, VkFormat format, int bytes_per_pixel);
static void  _vulkan_renderer_internal__flush_text(void);

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
    stdlib__memset(&_vulkan_renderer_internal__g, 0, sizeof(_vulkan_renderer_internal__g));
    _vulkan_renderer_internal__g.native_window = native_window;

    if (_vulkan_renderer_internal__create_instance() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_surface(native_window) == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__pick_physical_device() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_device() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_swapchain() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_render_pass() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_framebuffers() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_command_pool_and_buffers() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_sync_primitives() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_rect_pipeline() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_rect_vertex_buffers() == FALSE)
    {
        return FALSE;
    }

    log_info("%s %u %s %u %s %u %s %d %s", "vulkan_renderer: up (", (unsigned)_vulkan_renderer_internal__g.swap_image_count, "swapchain images,", (unsigned)_vulkan_renderer_internal__g.swap_extent.width, "x", (unsigned)_vulkan_renderer_internal__g.swap_extent.height, ", format=", (int)_vulkan_renderer_internal__g.swap_format, ")");
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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    if (s->device != VK_NULL_HANDLE)
    {
        if (!s->adopted) vkDeviceWaitIdle(s->device);
    }
    if (s->capture_buffer) vkDestroyBuffer(s->device,s->capture_buffer,NULL_POINTER);
    if (s->capture_memory) vkFreeMemory(s->device,s->capture_memory,NULL_POINTER);

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (s->rect_vbo_ptr[i] != NULL_POINTER)
        {
            vkUnmapMemory(s->device, s->rect_vbo_mem[i]);
            s->rect_vbo_ptr[i] = NULL_POINTER;
        }
        if (s->rect_vbo[i]     != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(s->device, s->rect_vbo[i], NULL_POINTER);
        }
        if (s->rect_vbo_mem[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(s->device, s->rect_vbo_mem[i], NULL_POINTER);
        }
    }

    // Text / image plumbing. Atlas pool entries own a VkImage + memory
    // + view + descriptor set; walk in_use==TRUE slots and free each.
    // Descriptor sets come from tx_descriptor_pool which is destroyed
    // right after, so the set handles don't need explicit vkFree --
    // they die with the pool. View + image + memory must be explicit.
    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_ATLASES; i++)
    {
        _vulkan_renderer_internal__atlas_entry_t* a = &s->atlas_pool[i];
        if (a->in_use == FALSE)
        {
            continue;
        }
        if (a->view   != VK_NULL_HANDLE)
        {
            vkDestroyImageView(s->device, a->view,   NULL_POINTER);
        }
        if (a->image  != VK_NULL_HANDLE)
        {
            vkDestroyImage    (s->device, a->image,  NULL_POINTER);
        }
        if (a->memory != VK_NULL_HANDLE)
        {
            vkFreeMemory      (s->device, a->memory, NULL_POINTER);
        }
    }

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (s->text_vbo_ptr[i] != NULL_POINTER)
        {
            vkUnmapMemory(s->device, s->text_vbo_mem[i]);
            s->text_vbo_ptr[i] = NULL_POINTER;
        }
        if (s->text_vbo[i]     != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(s->device, s->text_vbo[i], NULL_POINTER);
        }
        if (s->text_vbo_mem[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(s->device, s->text_vbo_mem[i], NULL_POINTER);
        }
    }

    if (s->text_pipeline         != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(s->device, s->text_pipeline, NULL_POINTER);
    }
    if (s->text_pipeline_layout  != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(s->device, s->text_pipeline_layout, NULL_POINTER);
    }
    if (s->image_pipeline        != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(s->device, s->image_pipeline, NULL_POINTER);
    }
    if (s->image_pipeline_layout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(s->device, s->image_pipeline_layout, NULL_POINTER);
    }
    if (s->tx_descriptor_pool    != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(s->device, s->tx_descriptor_pool, NULL_POINTER);
    }
    if (s->tx_dsl                != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(s->device, s->tx_dsl, NULL_POINTER);
    }
    if (s->tx_sampler            != VK_NULL_HANDLE)
    {
        vkDestroySampler(s->device, s->tx_sampler, NULL_POINTER);
    }
    if (s->tx_vs                 != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(s->device, s->tx_vs, NULL_POINTER);
    }
    if (s->text_fs               != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(s->device, s->text_fs, NULL_POINTER);
    }
    if (s->image_fs              != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(s->device, s->image_fs, NULL_POINTER);
    }

    if (s->rect_pipeline         != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(s->device, s->rect_pipeline, NULL_POINTER);
    }
    if (s->rect_pipeline_layout  != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(s->device, s->rect_pipeline_layout, NULL_POINTER);
    }
    if (s->rect_dsl              != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(s->device, s->rect_dsl, NULL_POINTER);
    }
    if (s->rect_vs               != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(s->device, s->rect_vs, NULL_POINTER);
    }
    if (s->rect_fs               != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(s->device, s->rect_fs, NULL_POINTER);
    }

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (s->sem_acquire[i] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(s->device, s->sem_acquire[i], NULL_POINTER);
        }
        if (s->sem_present[i] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(s->device, s->sem_present[i], NULL_POINTER);
        }
        if (s->in_flight[i]   != VK_NULL_HANDLE)
        {
            vkDestroyFence(s->device, s->in_flight[i], NULL_POINTER);
        }
    }

    if (s->command_pool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(s->device, s->command_pool, NULL_POINTER);
    }

    _vulkan_renderer_internal__destroy_swapchain();

    if (s->render_pass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(s->device, s->render_pass, NULL_POINTER);
    }

    if (!s->adopted && s->device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(s->device, NULL_POINTER);
        s->device = VK_NULL_HANDLE;
    }

    if (!s->adopted && s->surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(s->instance, s->surface, NULL_POINTER);
        s->surface = VK_NULL_HANDLE;
    }

    if (!s->adopted && s->instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(s->instance, NULL_POINTER);
        s->instance = VK_NULL_HANDLE;
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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    s->viewport_w             = viewport_w;
    s->viewport_h             = viewport_h;
    s->clear_color            = clear;
    s->rect_vert_count        = 0;
    s->rect_draw_cursor       = 0;
    s->text_vert_count        = 0;
    s->text_run_count         = 0;
    s->text_first_undrawn_run = 0;

    // Wait for previous frame using this slot to finish.
    s->frame_active = FALSE;
    s->frame_ok = FALSE;
    s->scissor_depth = 0;
    if (vkWaitForFences(s->device, 1, &s->in_flight[s->frame_index], VK_TRUE, UINT64_MAX) != VK_SUCCESS) return;

    if (!s->adopted)
    {
    // Acquire the next swapchain image.
    VkResult ar = vkAcquireNextImageKHR( s->device, s->swapchain, UINT64_MAX, s->sem_acquire[s->frame_index], VK_NULL_HANDLE, &s->swap_image_index);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // Swapchain got invalidated (window resized, display
        // reconnected). Recreating it mid-begin_frame would mean
        // rebuilding framebuffers + pipelines too; for this
        // scaffold we skip the frame and let the next tick try
        // again. Host platform typically re-layouts on resize so
        // the stale image isn't visible.
        log_warn("%s", "vkAcquireNextImageKHR: OUT_OF_DATE; skipping frame (swapchain recreate not yet wired)");
        s->frame_active = FALSE;
        return;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR)
    {
        log_error("%s %d", "vkAcquireNextImageKHR failed:", (int)ar);
        s->frame_active = FALSE;
        return;
    }



    }
    // Record command buffer for this frame. vkResetCommandBuffer
    // keeps the pool happy (command buffers are transient and
    // re-recorded every frame).
    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    if (s->adopted)
    {
        VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = s->host_frame.initial_layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = s->host_frame.image;
        barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,0,NULL_POINTER,0,NULL_POINTER,1,&barrier);
    }

    // One render pass wraps the entire frame's draws. Clear op
    // is baked into the first attachment load -- same semantic
    // as glClear / D3D11 ClearRenderTargetView. Convert gui_color_t
    // (linear 0..1) directly -- no sRGB conversion; we render to
    // a UNORM (non-sRGB) swapchain.
    VkClearValue clear_val;
    clear_val.color.float32[0] = clear.r;
    clear_val.color.float32[1] = clear.g;
    clear_val.color.float32[2] = clear.b;
    clear_val.color.float32[3] = clear.a;

    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass             = s->render_pass;
    rpbi.framebuffer            = s->framebuffers[s->swap_image_index];
    rpbi.renderArea.offset.x    = 0;
    rpbi.renderArea.offset.y    = 0;
    rpbi.renderArea.extent      = s->swap_extent;
    rpbi.clearValueCount        = 1;
    rpbi.pClearValues           = &clear_val;
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport + scissor (pipeline is created with these
    // states as DYNAMIC so we can update per-frame without
    // rebuilding the PSO on resize).
    VkViewport vp = { 0 };
    vp.x        = 0;
    vp.y        = 0;
    vp.width    = (float)s->swap_extent.width;
    vp.height   = (float)s->swap_extent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    VkRect2D sc = { { 0, 0 }, s->swap_extent };
    vkCmdSetScissor(cb, 0, 1, &sc);

    s->frame_active = TRUE;
    s->frame_ok = TRUE;
}

// Push one vertex into the rect VBO. Factored out so submit_rect and
// submit_rect_gradient share it; the only difference is whether all
// six corners share the same color or four distinct ones.
static void _vulkan_renderer_internal__push_vertex(float x, float y, float lx, float ly, float rw, float rh, float radius, gui_color_t c)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    _vulkan_renderer_internal__vertex_t* v = &s->rect_vbo_ptr[s->frame_index][s->rect_vert_count];
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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    _vulkan_renderer_internal__flush_text();
    // Buffer-full drop: the append-only cursor means a mid-frame
    // flush can't free space. MAX_QUADS (8192) is sized for a whole
    // frame; if a real UI hits the cap, bump the constant.
    if (s->rect_vert_count + _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD > _VULKAN_RENDERER_INTERNAL__MAX_QUADS * _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD)
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

    // Two triangles: (tl, tr, bl) + (tr, br, bl). Matches every other
    // backend's winding per the VISUAL CONTRACT.
    _vulkan_renderer_internal__push_vertex(x0, y0, 0.0f, 0.0f, r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x1, y0, r.w,  0.0f, r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x1, y1, r.w,  r.h,  r.w, r.h, radius, c);
    _vulkan_renderer_internal__push_vertex(x0, y1, 0.0f, r.h,  r.w, r.h, radius, c);
}

static void _vulkan_renderer_internal__flush_rects(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->rect_vert_count <= 0)
    {
        return;
    }
    if (s->frame_active == FALSE)
    {
        return;
    }

    int64 pending = s->rect_vert_count - s->rect_draw_cursor;
    if (pending <= 0)
    {
        return;
    }

    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->rect_pipeline);

    VkBuffer     vbs[1] = { s->rect_vbo[s->frame_index] };
    VkDeviceSize ofs[1] = { 0 };
    vkCmdBindVertexBuffers(cb, 0, 1, vbs, ofs);

    // Push constants deliver the viewport (2 floats) to the VS
    // for NDC conversion. Cheaper than a UBO for 8 bytes.
    float viewport[2];
    viewport[0] = (float)s->viewport_w;
    viewport[1] = (float)s->viewport_h;
    vkCmdPushConstants(cb, s->rect_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(viewport), viewport);

    // Draw the append-only slice [rect_draw_cursor .. rect_vert_count).
    // DO NOT reset rect_vert_count here -- the command buffer still
    // references these verts by offset, and subsequent CPU writes
    // must go to HIGHER offsets to avoid stomping the pending draw.
    // Same hazard the d3d12 backend hit earlier; same fix.
    vkCmdDraw(cb, (uint32_t)pending, 1, (uint32_t)s->rect_draw_cursor, 0);

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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    _vulkan_renderer_internal__flush_rects();
    _vulkan_renderer_internal__flush_text();
}

/**
 * @brief finalize the frame: upload the batched verts, issue the draw, then present the back buffer to the window (swap buffers / swapchain Present). After this call returns, the frame is on screen.
 *
 * @return void
 */
void renderer__end_frame(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        // begin_frame bailed out (acquire failure). Advance frame
        // index so the next tick uses the other slot.
        s->frame_index = (s->frame_index + 1) % _VULKAN_RENDERER_INTERNAL__MAX_FRAMES;
        return;
    }

    // Flush rects first, text on top. Same ordering every backend
    // uses so text always draws OVER rect bg (matches submit-order
    // when the widget tree emits "bg rect, then text label").
    _vulkan_renderer_internal__flush_rects();
    _vulkan_renderer_internal__flush_text();

    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];
    vkCmdEndRenderPass(cb);
    _vulkan_renderer_internal__record_capture(cb);
    if (s->adopted)
    {
        VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = s->host_frame.final_layout;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = s->host_frame.image;
        barrier.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,0,NULL_POINTER,0,NULL_POINTER,1,&barrier);
        VkResult result = vkEndCommandBuffer(cb);
        VkPipelineStageFlags stages[16];
        for (uint32_t i=0;i<s->host_frame.wait_count;i++) stages[i]=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit= { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount=s->host_frame.wait_count; submit.pWaitSemaphores=s->host_waits;
        submit.pWaitDstStageMask=stages; submit.commandBufferCount=1; submit.pCommandBuffers=&cb;
        if(result==VK_SUCCESS) result=vkResetFences(s->device,1,&s->in_flight[s->frame_index]);
        if(result==VK_SUCCESS) result=vkQueueSubmit(s->queue,1,&submit,s->in_flight[s->frame_index]);
        if(result==VK_SUCCESS) result=vkWaitForFences(s->device,1,&s->in_flight[s->frame_index],VK_TRUE,UINT64_MAX);
        s->frame_ok=result==VK_SUCCESS; s->frame_active=FALSE;
        if(!s->frame_ok) log_error("embedded Vulkan submit failed: %d",(int)result);
        if(s->frame_ok) { _vulkan_renderer_internal__finish_capture(); _vulkan_renderer_internal__retire_atlases(); }
        vkDestroyFramebuffer(s->device,s->framebuffers[0],NULL_POINTER);
        s->framebuffers[0]=VK_NULL_HANDLE;
        s->frame_index=(s->frame_index+1)%_VULKAN_RENDERER_INTERNAL__MAX_FRAMES;
        return;
    }
    vkEndCommandBuffer(cb);

    // Submit: wait for the image to be acquired, signal the
    // "render done" semaphore when all color writes land.
    VkPipelineStageFlags wait_stages[1] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &s->sem_acquire[s->frame_index];
    si.pWaitDstStageMask    = wait_stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &s->sem_present[s->frame_index];
    vkResetFences(s->device, 1, &s->in_flight[s->frame_index]);
    s->frame_ok = vkQueueSubmit(s->queue, 1, &si, s->in_flight[s->frame_index]) == VK_SUCCESS;
    if (!s->frame_ok) {s->frame_active=FALSE;return;}
    if (s->capture_out && vkWaitForFences(s->device,1,&s->in_flight[s->frame_index],VK_TRUE,UINT64_MAX)==VK_SUCCESS)
        _vulkan_renderer_internal__finish_capture();

    // Present after submission. FIFO present mode pins this to
    // vsync, same timing pacing as other backends.
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &s->sem_present[s->frame_index];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &s->swapchain;
    pi.pImageIndices      = &s->swap_image_index;
    VkResult pr = vkQueuePresentKHR(s->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
    {
        // Swapchain became invalid during / after the present.
        // Scaffold-level handling: log and continue; the next
        // begin_frame's acquire will also fail and skip a frame
        // until host platform rebuilds the swapchain. A full
        // impl would recreate here.
        log_warn("%s", "vkQueuePresentKHR: OUT_OF_DATE / SUBOPTIMAL; swapchain rebuild pending");
    }
    else if (pr != VK_SUCCESS)
    {
        log_error("%s %d", "vkQueuePresentKHR failed:", (int)pr);
    }

    _vulkan_renderer_internal__retire_atlases();
    s->frame_active = FALSE;
    s->frame_index  = (s->frame_index + 1) % _VULKAN_RENDERER_INTERNAL__MAX_FRAMES;
}

// ============================================================================
// text / image transport and private drawing helpers
// ============================================================================
// Public effects use the portable compositor; these resources also provide
// the final full-frame image upload.

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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (pixels == NULL_POINTER || width <= 0 || height <= 0)
    {
        return NULL_POINTER;
    }
    // Lazy-build the shared text plumbing (sampler, DSL, descriptor
    // pool, pipeline, VBO) on the first atlas request. That way a
    // PoC that never submits text doesn't pay for it.
    if (s->tx_built == FALSE)
    {
        if (_vulkan_renderer_internal__ensure_tx_plumbing() == FALSE)
        {
            return NULL_POINTER;
        }
        s->tx_built = TRUE;
    }
    _vulkan_renderer_internal__atlas_entry_t* a = _vulkan_renderer_internal__alloc_atlas_slot();
    if (a == NULL_POINTER)
    {
        log_error("%s", "vulkan atlas: pool full");
        return NULL_POINTER;
    }
    if (_vulkan_renderer_internal__upload_atlas(a, pixels, width, height, VK_FORMAT_R8_UNORM, 1) == FALSE)
    {
        _vulkan_renderer_internal__release_atlas(a);
        return NULL_POINTER;
    }
    return (void*)a;
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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (atlas == NULL_POINTER)
    {
        return;
    }
    _vulkan_renderer_internal__atlas_entry_t* a = (_vulkan_renderer_internal__atlas_entry_t*)atlas;
    // A recorded draw may still reference this descriptor even before submit.
    // Defer deletion until end_frame has submitted and completed those draws.
    if (s->frame_active) { a->pending_release = TRUE; return; }
    if (!s->adopted) vkDeviceWaitIdle(s->device);
    if (s->current_text_atlas == a)
    {
        s->current_text_atlas = NULL_POINTER;
    }
    _vulkan_renderer_internal__release_atlas(a);
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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    s->current_text_atlas = (_vulkan_renderer_internal__atlas_entry_t*)atlas;
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
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    if (s->current_text_atlas == NULL_POINTER)
    {
        return;
    }
    if (s->text_vert_count + _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD > _VULKAN_RENDERER_INTERNAL__MAX_TEXT_QUADS * _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD)
    {
        return;
    }

    _vulkan_renderer_internal__flush_rects();
    // Open or extend the last run. Identical rule to d3d12: only
    // extend the tail if that run has NOT been flushed yet -- mid-
    // frame flushes (push_scissor) advance first_undrawn_run past
    // already-drawn runs, and extending one of those would slot new
    // glyphs into a vert range nothing re-draws.
    _vulkan_renderer_internal__text_run_t* run = NULL_POINTER;
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
        if (s->text_run_count >= _VULKAN_RENDERER_INTERNAL__MAX_TEXT_RUNS)
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

    _vulkan_renderer_internal__tx_vertex_t* v = &s->text_vbo_ptr[s->frame_index][s->text_vert_count];
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

    s->text_vert_count += _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;
    run->vert_count    += _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;
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
void* renderer__create_texture_rgba(const ubyte* rgba, int w, int h)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (!rgba || w <= 0 || h <= 0 || w > 8192 || h > 8192 || !s->device) return NULL_POINTER;
    if (!s->tx_built) { if (!_vulkan_renderer_internal__ensure_tx_plumbing()) return NULL_POINTER; s->tx_built = TRUE; }
    _vulkan_renderer_internal__atlas_entry_t* a = _vulkan_renderer_internal__alloc_atlas_slot();
    if (!a) return NULL_POINTER;
    if (!_vulkan_renderer_internal__upload_atlas(a, rgba, w, h, VK_FORMAT_R8G8B8A8_UNORM, 4))
    { _vulkan_renderer_internal__release_atlas(a); return NULL_POINTER; }
    return a;
}
// The frame surface needs the image pipeline too; it remains unsupported here.
GUI_API void* renderer_backend__create_frame_texture_rgba(const ubyte* rgba, int width, int height)
{
    return renderer__create_texture_rgba(rgba, width, height);
}

void renderer__destroy_texture(void* tex) { renderer__destroy_atlas(tex); }
/**
 * @brief draw one image quad immediately. Flushes any pending solid-rect or text batch first so previously-submitted content stays underneath and subsequent submissions stay on top. Full-texture draw (UV 0..1 on both axes); no subregion support in this pass (crop via node bounds + scissor instead).
 *
 * @param rect Destination rect in top-left-origin pixel coords.
 * @param void* tex -> Texture handle from create_texture_rgba. NULL = skip draw.
 * @param gui_color_t tint -> Multiplied against the sampled RGBA. Pass (1,1,1,1) for "draw as-is".
 *
 * @return void
 */
void renderer__submit_image(gui_rect_t r, void* tex, gui_color_t tint)
{
    renderer__submit_image_uv(r,tex,tint,0,0,1,1);
}

/**
 * @brief uV-aware image submit; this backend has no image pipeline yet, so the sub-rect is ignored and the tinted-rect placeholder of renderer__submit_image is drawn instead
 *
 * @param gui_rect_t r -> destination rectangle in pixels
 * @param void* tex -> texture handle; ignored
 * @param gui_color_t tint -> multiplied color
 * @param float u0 -> ignored
 * @param float v0 -> ignored
 * @param float u1 -> ignored
 * @param float v1 -> ignored
 *
 * @return void
 */
void renderer__submit_image_uv(gui_rect_t r, void* tex, gui_color_t tint, float u0, float v0, float u1, float v1)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    _vulkan_renderer_internal__atlas_entry_t* a = tex;
    if (!s->frame_active || !a || !a->in_use || !s->image_pipeline) return;
    _vulkan_renderer_internal__flush_rects();
    _vulkan_renderer_internal__flush_text();
    if (s->text_vert_count + 6 > _VULKAN_RENDERER_INTERNAL__MAX_TEXT_QUADS * 6) return;
    _vulkan_renderer_internal__tx_vertex_t* v = &s->text_vbo_ptr[s->frame_index][s->text_vert_count];
    const int corner[6] = {0,1,2,1,3,2};
    for (int i=0;i<6;i++) {
        v[i] = (_vulkan_renderer_internal__tx_vertex_t){r.x + ((corner[i]&1)?r.w:0), r.y + ((corner[i]&2)?r.h:0),
            tint.r,tint.g,tint.b,tint.a,(corner[i]&1)?u1:u0,(corner[i]&2)?v1:v0};
    }
    VkCommandBuffer cb=s->cmd_bufs[s->frame_index];
    VkDeviceSize offset=0; float viewport[2]={(float)s->viewport_w,(float)s->viewport_h};
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->image_pipeline);
    vkCmdBindVertexBuffers(cb,0,1,&s->text_vbo[s->frame_index],&offset);
    vkCmdPushConstants(cb,s->image_pipeline_layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(viewport),viewport);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,s->image_pipeline_layout,0,1,&a->desc_set,0,NULL_POINTER);
    vkCmdDraw(cb,6,1,(uint32_t)s->text_vert_count,0);
    s->text_vert_count+=6;
}

// Shadow: concentric layered rects with quadratic falloff. The rect
// pipeline's per-fragment SDF AA produces the soft edge; no separate
// shader needed. Same algorithm as d3d11 / d3d12 / opengl3 / gles3.
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

// Gradient: per-corner color over the existing SDF-rect pipeline.
// The fragment shader interpolates `col` across the triangle, so
// two-color lerps cost nothing extra -- just different corner colors
// fed to push_vertex. Direction maps onto corners the same way every
// other backend does it (see the comment in d3d11 / opengl3).
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
    return FALSE;
}

void renderer__submit_triangle(gui_vec2_t a, gui_vec2_t b, gui_vec2_t c, gui_color_t color)
{
    _vulkan_renderer_internal__flush_text();
    gui_vec2_t vertices[3] = {a, b, c};
    float left = stdlib__fminf(a.x, stdlib__fminf(b.x, c.x)) - 1;
    float top = stdlib__fminf(a.y, stdlib__fminf(b.y, c.y)) - 1;
    float width = stdlib__fmaxf(a.x, stdlib__fmaxf(b.x, c.x)) - left + 1;
    float height = stdlib__fmaxf(a.y, stdlib__fmaxf(b.y, c.y)) - top + 1;
    int i;
    if (_vulkan_renderer_internal__g.frame_active == FALSE || _vulkan_renderer_internal__g.rect_vert_count + 3 > _VULKAN_RENDERER_INTERNAL__MAX_QUADS * _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD) return;
    // Expand the rectangle mask past the geometry. Triangle rasterization
    // owns the edges; the existing colour pipeline needs no extra shader.
    for (i = 0; i < 3; i++)
        _vulkan_renderer_internal__push_vertex(vertices[i].x, vertices[i].y,
            vertices[i].x - left, vertices[i].y - top, width, height, 0, color);
}

void renderer__submit_gradient(gui_rect_t r, const gui_gradient_t* g, float scale, float opacity, float radius)
{
    static gradient_vertex_t verts[GRADIENT_MAX_VERTS];
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    int64 count = 0;
    int64 i     = 0;
    float max_r = 0.0f;

    if (s->frame_active == FALSE)
    {
        return;
    }

    count = gradient__tessellate(g, r, scale, opacity, verts, GRADIENT_MAX_VERTS);

    if (count <= 0 || s->rect_vert_count + count > _VULKAN_RENDERER_INTERNAL__MAX_QUADS * _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD)
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
        _vulkan_renderer_internal__push_vertex(verts[i].x, verts[i].y, verts[i].x - r.x, verts[i].y - r.y, r.w, r.h, radius, verts[i].c);
    }
}

// ============================================================================
// scissor
// ============================================================================
// Intersection stack + flush-on-change, same pattern as every
// other backend. Vulkan's vkCmdSetScissor is dynamic state on our
// pipeline, so pushing a new rect is cheap (no PSO rebuild).

/**
 * @brief push a clip rect onto the scissor stack. Subsequent submissions are clipped to the intersection of every rect currently on the stack.
 *
 * @param gui_rect_t r -> Clip region in pixel coords (top-left origin).
 *
 * @return void
 */
void renderer__push_scissor(gui_rect_t r)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->scissor_depth >= _VULKAN_RENDERER_INTERNAL__MAX_SCISSORS)
    {
        return;
    }

    // Flush BOTH rects and text before the scissor change so queued
    // draws land under the OLD clip. Text runs are only defined once
    // tx plumbing exists (lazy-built on first atlas upload); guard
    // accordingly.
    _vulkan_renderer_internal__flush_rects();
    if (s->tx_built && s->text_run_count > s->text_first_undrawn_run)
    {
        _vulkan_renderer_internal__flush_text();
    }

    // Clamp input rect to [0, viewport] and intersect with the
    // currently effective scissor (if any) to produce the new
    // effective rect.
    int32_t x0 = (int32_t)r.x;
    int32_t y0 = (int32_t)r.y;
    int32_t x1 = (int32_t)(r.x + r.w);
    int32_t y1 = (int32_t)(r.y + r.h);
    if (x0 < 0)
    {
        x0 = 0;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (x1 > (int32_t)s->swap_extent.width)
    {
        x1 = (int32_t)s->swap_extent.width;
    }
    if (y1 > (int32_t)s->swap_extent.height)
    {
        y1 = (int32_t)s->swap_extent.height;
    }

    if (s->scissor_depth > 0)
    {
        VkRect2D prev = s->scissors[s->scissor_depth - 1].rect;
        int32_t px0 = prev.offset.x;
        int32_t py0 = prev.offset.y;
        int32_t px1 = prev.offset.x + (int32_t)prev.extent.width;
        int32_t py1 = prev.offset.y + (int32_t)prev.extent.height;
        if (x0 < px0)
        {
            x0 = px0;
        }
        if (y0 < py0)
        {
            y0 = py0;
        }
        if (x1 > px1)
        {
            x1 = px1;
        }
        if (y1 > py1)
        {
            y1 = py1;
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

    VkRect2D sc;
    sc.offset.x      = x0;
    sc.offset.y      = y0;
    sc.extent.width  = (uint32_t)(x1 - x0);
    sc.extent.height = (uint32_t)(y1 - y0);

    s->scissors[s->scissor_depth].rect   = sc;
    s->scissors[s->scissor_depth].active = TRUE;
    s->scissor_depth++;

    if (s->frame_active)
    {
        vkCmdSetScissor(s->cmd_bufs[s->frame_index], 0, 1, &sc);
    }
}

/**
 * @brief pop the most recent scissor rect off the stack. If the stack is now empty, scissor testing is disabled entirely (everything draws).
 *
 * @return void
 */
void renderer__pop_scissor(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->scissor_depth <= 0)
    {
        return;
    }

    // Same dual-flush as push_scissor: queued draws land under the
    // inner (tighter) clip before we pop back out.
    _vulkan_renderer_internal__flush_rects();
    if (s->tx_built && s->text_run_count > s->text_first_undrawn_run)
    {
        _vulkan_renderer_internal__flush_text();
    }

    s->scissor_depth--;

    if (s->frame_active == FALSE)
    {
        return;
    }

    VkRect2D sc;
    if (s->scissor_depth > 0)
    {
        sc = s->scissors[s->scissor_depth - 1].rect;
    }
    else
    {
        sc.offset.x      = 0;
        sc.offset.y      = 0;
        sc.extent.width  = s->swap_extent.width;
        sc.extent.height = s->swap_extent.height;
    }
    vkCmdSetScissor(s->cmd_bufs[s->frame_index], 0, 1, &sc);
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
    // Real Gaussian backdrop blur on Vulkan is deferred. Readback
    // path: vkCmdCopyImageToBuffer from the swapchain image into a
    // host-visible VkBuffer, with explicit pipeline barriers
    // moving the swapchain image through PRESENT_SRC -> TRANSFER_SRC
    // -> PRESENT_SRC, plus a vkQueueSubmit + vkWaitForFences for
    // synchronous CPU-side mapping. Same _blur_cpu helper that
    // opengl3 / gles3 / d3d11 / d3d9 use would slot in once the
    // readback lands.
    // Placeholder darken-rect for now -- consistent with the rest
    // of vulkan_renderer's image / text pipelines which are also
    // stubbed pending Vulkan-SDK-local verification.
    (void)sigma_px;
    gui_color_t dim = { 0.0f, 0.0f, 0.0f, 0.15f };
    renderer__submit_rect(rect, dim, 0.0f);
}

// ============================================================================
// IMPL: init helpers
// ============================================================================
// From here down is straightforward Vulkan boilerplate: instance,
// surface, device pick, swapchain, render pass, framebuffers,
// command buffers, sync primitives, pipeline, vertex buffer.
// Every step is the minimum viable -- no validation layers enabled
// by default, no MSAA, no depth buffer. The shipped shape is
// "simplest possible Vulkan that renders one triangle" extended
// with double-buffered frame-in-flight + a real VBO.

static boole _vulkan_renderer_internal__create_instance(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName   = "gui";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "novyworkbench-gui";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_1;

    // Minimal extension list: VK_KHR_surface + one platform-
    // specific surface extension. Validation layers intentionally
    // omitted; add -DGUI_VULKAN_VALIDATION to flip them on in a
    // future pass.
    const char* exts[3] = { 0 };
    uint32_t    n_ext   = 0;

    exts[n_ext++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    exts[n_ext++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    exts[n_ext++] = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    exts[n_ext++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#endif

    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = n_ext;
    ici.ppEnabledExtensionNames = exts;

    VkResult r = vkCreateInstance(&ici, NULL_POINTER, &s->instance);
    if (r != VK_SUCCESS)
    {
        log_error("%s %d", "vkCreateInstance failed:", (int)r);
        return FALSE;
    }
    return TRUE;
}

// Cross-platform surface dispatch table for native_window. The
// platform layer packs whichever native handle pair the active
// VK_KHR_*_surface extension expects:
// Win32:    native_window = HWND. HINSTANCE comes from GetModuleHandleW.
// X11:      native_window points at this struct so we get both
// (Display*, Window) atomically; the platform_linux_x11
// backend builds it on init and hands the address to
// renderer__init.
// Android:  native_window = ANativeWindow*.
// Other backends (DRM, Wayland, macOS) don't go through Vulkan
// today; if/when they do they add a branch here.
typedef struct vulkan_native_window_x11_t
{
    void*        display; // Display* cast to void* to avoid the X header in the renderer
    unsigned long window; // Window (XID, ulong on every libX11 build we target)
} vulkan_native_window_x11_t;

static boole _vulkan_renderer_internal__create_surface(void* native_window)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    if (native_window == NULL_POINTER)
    {
        log_error("%s", "vulkan_renderer: native_window is NULL_POINTER");
        return FALSE;
    }

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(NULL_POINTER);
    sci.hwnd      = (HWND)native_window;
    VkResult r = vkCreateWin32SurfaceKHR(s->instance, &sci, NULL_POINTER, &s->surface);
    if (r != VK_SUCCESS)
    {
        log_error("%s %d", "vkCreateWin32SurfaceKHR failed:", (int)r);
        return FALSE;
    }
    return TRUE;

#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    // X11 platform packs (Display*, Window) into a small struct
    // because vkCreateXlibSurfaceKHR needs both. The Linux X11
    // platform layer constructs one on init and passes its address
    // through renderer__init's `native_window` parameter.
    vulkan_native_window_x11_t* nw = (vulkan_native_window_x11_t*)native_window;
    VkXlibSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    sci.dpy    = (Display*)nw->display;
    sci.window = (Window)nw->window;
    VkResult r = vkCreateXlibSurfaceKHR(s->instance, &sci, NULL_POINTER, &s->surface);
    if (r != VK_SUCCESS)
    {
        log_error("%s %d", "vkCreateXlibSurfaceKHR failed:", (int)r);
        return FALSE;
    }
    return TRUE;

#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
    // Android passes ANativeWindow* directly. The Android platform
    // layer's APP_CMD_INIT_WINDOW handler hands us app->window.
    VkAndroidSurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    sci.window = (ANativeWindow*)native_window;
    VkResult r = vkCreateAndroidSurfaceKHR(s->instance, &sci, NULL_POINTER, &s->surface);
    if (r != VK_SUCCESS)
    {
        log_error("%s %d", "vkCreateAndroidSurfaceKHR failed:", (int)r);
        return FALSE;
    }
    return TRUE;

#else
    (void)native_window;
    log_error("%s", "vulkan_renderer: no surface extension compiled in for this platform");
    return FALSE;
#endif
}

static boole _vulkan_renderer_internal__pick_physical_device(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s->instance, &count, NULL_POINTER);
    if (count == 0)
    {
        log_error("%s", "no VkPhysicalDevices available");
        return FALSE;
    }
    if (count > 8)
    {
        count = 8;
    }
    VkPhysicalDevice devs[8];
    vkEnumeratePhysicalDevices(s->instance, &count, devs);

    // Walk the list, pick the first device that has a queue family
    // which supports both graphics AND present-to-our-surface.
    // Integrated GPU vs discrete preference is intentionally left
    // out for this scaffold.
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL_POINTER);
        if (qn == 0)
        {
            continue;
        }
        if (qn > 16)
        {
            qn = 16;
        }
        VkQueueFamilyProperties qfp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qfp);
        for (uint32_t q = 0; q < qn; q++)
        {
            if ((qfp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
            {
                continue;
            }
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, s->surface, &present);
            if (present == VK_FALSE)
            {
                continue;
            }
            s->phys_device  = devs[i];
            s->queue_family = q;
            return TRUE;
        }
    }
    log_error("%s", "no VkPhysicalDevice with graphics+present queue on this surface");
    return FALSE;
}

static boole _vulkan_renderer_internal__create_device(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s->queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;

    const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    VkResult r = vkCreateDevice(s->phys_device, &dci, NULL_POINTER, &s->device);
    if (r != VK_SUCCESS)
    {
        log_error("%s %d", "vkCreateDevice failed:", (int)r);
        return FALSE;
    }
    vkGetDeviceQueue(s->device, s->queue_family, 0, &s->queue);
    return TRUE;
}

static boole _vulkan_renderer_internal__create_swapchain(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->phys_device, s->surface, &caps);

    // Pick an 8-bit UNORM BGRA/RGBA format. If the surface's
    // preferred is UNDEFINED, BGRA8 UNORM is the safe default.
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys_device, s->surface, &fn, NULL_POINTER);
    if (fn == 0)
    {
        log_error("%s", "no surface formats");
        return FALSE;
    }
    if (fn > 16)
    {
        fn = 16;
    }
    VkSurfaceFormatKHR formats[16];
    vkGetPhysicalDeviceSurfaceFormatsKHR(s->phys_device, s->surface, &fn, formats);

    VkSurfaceFormatKHR pick = formats[0];
    for (uint32_t i = 0; i < fn; i++)
    {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            pick = formats[i];
            break;
        }
    }

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount)
    {
        want = caps.maxImageCount;
    }
    if (want > _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG)
    {
        want = _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == (uint32_t)-1)
    {
        // Surface is "size-follows-swapchain" (rare). Pick a
        // reasonable default; the host will soon set a real size
        // via the platform resize path.
        extent.width  = 800;
        extent.height = 600;
    }

    VkSwapchainCreateInfoKHR scci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    scci.surface          = s->surface;
    scci.minImageCount    = want;
    scci.imageFormat      = pick.format;
    scci.imageColorSpace  = pick.colorSpace;
    scci.imageExtent      = extent;
    scci.imageArrayLayers = 1;
    scci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    s->swap_usage         = scci.imageUsage;
    scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scci.preTransform     = caps.currentTransform;
    scci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scci.presentMode      = VK_PRESENT_MODE_FIFO_KHR; // vsync
    scci.clipped          = VK_TRUE;

    VkResult r = vkCreateSwapchainKHR(s->device, &scci, NULL_POINTER, &s->swapchain);
    if (r != VK_SUCCESS)
    {
        log_error("%s %d", "vkCreateSwapchainKHR failed:", (int)r);
        return FALSE;
    }
    s->swap_format = pick.format;
    s->swap_extent = extent;

    // Retrieve image handles + create views for each.
    uint32_t got = 0;
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &got, NULL_POINTER);
    if (got > _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG)
    {
        got = _VULKAN_RENDERER_INTERNAL__MAX_SWAPCHAIN_IMG;
    }
    vkGetSwapchainImagesKHR(s->device, s->swapchain, &got, s->swap_images);
    s->swap_image_count = got;

    for (uint32_t i = 0; i < got; i++)
    {
        VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivci.image    = s->swap_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format   = s->swap_format;
        ivci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel   = 0;
        ivci.subresourceRange.levelCount     = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount     = 1;
        VkResult rr = vkCreateImageView(s->device, &ivci, NULL_POINTER, &s->swap_views[i]);
        if (rr != VK_SUCCESS)
        {
            log_error("%s %u %s %d", "vkCreateImageView[", (unsigned)i, "] failed:", (int)rr);
            return FALSE;
        }
    }
    return TRUE;
}

static void _vulkan_renderer_internal__destroy_swapchain(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    for (uint32_t i = 0; i < s->swap_image_count; i++)
    {
        if (s->framebuffers[i] != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(s->device, s->framebuffers[i], NULL_POINTER);
            s->framebuffers[i] = VK_NULL_HANDLE;
        }
        if (s->swap_views[i] != VK_NULL_HANDLE)
        {
            vkDestroyImageView(s->device, s->swap_views[i], NULL_POINTER);
            s->swap_views[i] = VK_NULL_HANDLE;
        }
    }
    if (s->swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(s->device, s->swapchain, NULL_POINTER);
        s->swapchain = VK_NULL_HANDLE;
    }
    s->swap_image_count = 0;
}

static boole _vulkan_renderer_internal__create_render_pass(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkAttachmentDescription attach = { 0 };
    attach.format         = s->swap_format;
    attach.samples        = VK_SAMPLE_COUNT_1_BIT;
    attach.loadOp         = s->adopted ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attach.initialLayout  = s->adopted ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attach.finalLayout    = s->adopted ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref = { 0 };
    ref.attachment = 0;
    ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub = { 0 };
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    VkSubpassDependency dep = { 0 };
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &attach;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies   = &dep;

    VkResult r = vkCreateRenderPass(s->device, &rpci, NULL_POINTER, &s->render_pass);
    if (r != VK_SUCCESS)
    {
        log_error("%s %d", "vkCreateRenderPass failed:", (int)r);
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_framebuffers(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    for (uint32_t i = 0; i < s->swap_image_count; i++)
    {
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = s->render_pass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &s->swap_views[i];
        fci.width           = s->swap_extent.width;
        fci.height          = s->swap_extent.height;
        fci.layers          = 1;
        VkResult r = vkCreateFramebuffer(s->device, &fci, NULL_POINTER, &s->framebuffers[i]);
        if (r != VK_SUCCESS)
        {
            log_error("%s %u %s %d", "vkCreateFramebuffer[", (unsigned)i, "] failed:", (int)r);
            return FALSE;
        }
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_command_pool_and_buffers(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = s->queue_family;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(s->device, &cpci, NULL_POINTER, &s->command_pool) != VK_SUCCESS)
    {
        log_error("%s", "vkCreateCommandPool failed");
        return FALSE;
    }

    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = s->command_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = _VULKAN_RENDERER_INTERNAL__MAX_FRAMES;
    if (vkAllocateCommandBuffers(s->device, &cbai, s->cmd_bufs) != VK_SUCCESS)
    {
        log_error("%s", "vkAllocateCommandBuffers failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_sync_primitives(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first frame doesn't wait on a never-signalled fence

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        if (vkCreateSemaphore(s->device, &sci, NULL_POINTER, &s->sem_acquire[i]) != VK_SUCCESS || vkCreateSemaphore(s->device, &sci, NULL_POINTER, &s->sem_present[i]) != VK_SUCCESS || vkCreateFence(s->device, &fci, NULL_POINTER, &s->in_flight[i]) != VK_SUCCESS)
        {
            log_error("%s %d %s", "sync primitive create failed (frame", i, ")");
            return FALSE;
        }
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__compile_shader(const uint32_t* code, size_t bytes, const char* tag, VkShaderModule* out)
{
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = bytes; info.pCode = code;
    VkResult result = vkCreateShaderModule(_vulkan_renderer_internal__g.device, &info, NULL_POINTER, out);
    if (result != VK_SUCCESS) log_error("Vulkan shader %s failed: %d", tag, (int)result);
    return result == VK_SUCCESS;
}

static boole _vulkan_renderer_internal__create_rect_pipeline(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    if (_vulkan_renderer_internal__compile_shader(_VULKAN_RENDERER_INTERNAL__RECT_VS_SPV, sizeof(_VULKAN_RENDERER_INTERNAL__RECT_VS_SPV), "rect.vs", &s->rect_vs) == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__compile_shader(_VULKAN_RENDERER_INTERNAL__RECT_FS_SPV, sizeof(_VULKAN_RENDERER_INTERNAL__RECT_FS_SPV), "rect.fs", &s->rect_fs) == FALSE)
    {
        return FALSE;
    }

    // Vertex input: same 11-float layout gles3 uses, split into
    // five attributes matching the shader.
    VkVertexInputBindingDescription vbind = { 0 };
    vbind.binding   = 0;
    vbind.stride    = sizeof(_vulkan_renderer_internal__vertex_t);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[5] = { 0 };
    vattr[0].location = 0; vattr[0].binding = 0; vattr[0].format = VK_FORMAT_R32G32_SFLOAT;       vattr[0].offset = offsetof(_vulkan_renderer_internal__vertex_t, x);
    vattr[1].location = 1; vattr[1].binding = 0; vattr[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; vattr[1].offset = offsetof(_vulkan_renderer_internal__vertex_t, r);
    vattr[2].location = 2; vattr[2].binding = 0; vattr[2].format = VK_FORMAT_R32G32_SFLOAT;       vattr[2].offset = offsetof(_vulkan_renderer_internal__vertex_t, lx);
    vattr[3].location = 3; vattr[3].binding = 0; vattr[3].format = VK_FORMAT_R32G32_SFLOAT;       vattr[3].offset = offsetof(_vulkan_renderer_internal__vertex_t, rect_w);
    vattr[4].location = 4; vattr[4].binding = 0; vattr[4].format = VK_FORMAT_R32_SFLOAT;          vattr[4].offset = offsetof(_vulkan_renderer_internal__vertex_t, radius);

    VkPipelineVertexInputStateCreateInfo visci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    visci.vertexBindingDescriptionCount   = 1;
    visci.pVertexBindingDescriptions      = &vbind;
    visci.vertexAttributeDescriptionCount = 5;
    visci.pVertexAttributeDescriptions    = vattr;

    VkPipelineInputAssemblyStateCreateInfo iasci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport + scissor so we don't bake screen size into
    // the PSO. Rebuilt at framebuffer resize without touching the
    // pipeline object.
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsci.dynamicStateCount = 2;
    dsci.pDynamicStates    = dyn;

    VkPipelineViewportStateCreateInfo vsci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vsci.viewportCount = 1;
    vsci.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rsci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsci.polygonMode = VK_POLYGON_MODE_FILL;
    rsci.cullMode    = VK_CULL_MODE_NONE;
    rsci.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsci.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo msci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Separate-alpha blend -- same contract as every other
    // backend: RGB uses SRC_ALPHA/ONE_MINUS_SRC_ALPHA, alpha uses
    // ONE/ONE_MINUS_SRC_ALPHA so the framebuffer stays opaque.
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbci.attachmentCount = 1;
    cbci.pAttachments    = &cba;

    // Push constants deliver the viewport size to the VS. One 8-
    // byte push range suffices.
    VkPushConstantRange pcr = { 0 };
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(s->device, &plci, NULL_POINTER, &s->rect_pipeline_layout) != VK_SUCCESS)
    {
        log_error("%s", "vkCreatePipelineLayout failed");
        return FALSE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = { { 0 } };
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->rect_vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = s->rect_fs;
    stages[1].pName  = "main";
    uint32_t srgb = (s->swap_format == VK_FORMAT_R8G8B8A8_SRGB || s->swap_format == VK_FORMAT_B8G8R8A8_SRGB);
    VkSpecializationMapEntry color_entry = {0, 0, sizeof(srgb)};
    VkSpecializationInfo color_info = {1, &color_entry, sizeof(srgb), &srgb};
    stages[1].pSpecializationInfo = &color_info;

    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState      = &vsci;
    gpci.pRasterizationState = &rsci;
    gpci.pMultisampleState   = &msci;
    gpci.pColorBlendState    = &cbci;
    gpci.pDynamicState       = &dsci;
    gpci.layout              = s->rect_pipeline_layout;
    gpci.renderPass          = s->render_pass;
    gpci.subpass             = 0;

    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gpci, NULL_POINTER, &s->rect_pipeline) != VK_SUCCESS)
    {
        log_error("%s", "vkCreateGraphicsPipelines (rect) failed");
        return FALSE;
    }
    return TRUE;
}

static uint32_t _vulkan_renderer_internal__find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s->phys_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    {
        if ((type_filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static boole _vulkan_renderer_internal__create_rect_vertex_buffers(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkDeviceSize size = sizeof(_vulkan_renderer_internal__vertex_t) *
                        _VULKAN_RENDERER_INTERNAL__MAX_QUADS *
                        _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = size;
        bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s->device, &bci, NULL_POINTER, &s->rect_vbo[i]) != VK_SUCCESS)
        {
            log_error("%s %d %s", "vkCreateBuffer (rect vbo", i, ") failed");
            return FALSE;
        }

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(s->device, s->rect_vbo[i], &mr);

        // Host-visible + coherent: vertex writes land in memory
        // without an explicit flush. Fine for a 192 KB buffer
        // at 60 fps on modern GPUs.
        uint32_t mt = _vulkan_renderer_internal__find_memory_type( mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX)
        {
            log_error("%s", "no host-visible+coherent memory type for rect vbo");
            return FALSE;
        }

        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = mt;
        if (vkAllocateMemory(s->device, &mai, NULL_POINTER, &s->rect_vbo_mem[i]) != VK_SUCCESS)
        {
            log_error("%s %d %s", "vkAllocateMemory (rect vbo", i, ") failed");
            return FALSE;
        }
        vkBindBufferMemory(s->device, s->rect_vbo[i], s->rect_vbo_mem[i], 0);

        // Persistent map so submit_rect can stamp vertices without
        // re-mapping every frame.
        if (vkMapMemory(s->device, s->rect_vbo_mem[i], 0, size, 0, (void**)&s->rect_vbo_ptr[i]) != VK_SUCCESS)
        {
            log_error("%s %d %s", "vkMapMemory (rect vbo", i, ") failed");
            return FALSE;
        }
    }
    return TRUE;
}

// ============================================================================
// text + image plumbing (lazy, built on first atlas / image request)
// ============================================================================

static boole _vulkan_renderer_internal__ensure_tx_plumbing(void)
{
    if (_vulkan_renderer_internal__create_tx_sampler() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_tx_descriptor_layout() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_tx_descriptor_pool() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_text_pipeline() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_tx_vertex_buffers() == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__create_image_pipeline() == FALSE) return FALSE;
    // image_pipeline is left unbuilt for now. The image FS compiles
    // fine but the submit path hasn't been wired; the header comment
    // on renderer__create_texture_rgba explains. Mechanical follow-up
    // once the text path is proven.
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_sampler(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter        = VK_FILTER_LINEAR;
    sci.minFilter        = VK_FILTER_LINEAR;
    sci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy    = 1.0f;
    sci.compareEnable    = VK_FALSE;
    sci.compareOp        = VK_COMPARE_OP_NEVER;
    sci.mipLodBias       = 0.0f;
    sci.minLod           = 0.0f;
    sci.maxLod           = 0.0f;
    sci.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    if (vkCreateSampler(s->device, &sci, NULL_POINTER, &s->tx_sampler) != VK_SUCCESS)
    {
        log_error("%s", "vkCreateSampler (text) failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_descriptor_layout(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    // One binding: combined image sampler at set=0, binding=0,
    // used by the fragment shader (`layout(set=0, binding=0)
    // uniform sampler2D u_atlas`). Matches the text FS shader source
    // at the top of this file.
    VkDescriptorSetLayoutBinding b = { 0 };
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    b.pImmutableSamplers = NULL_POINTER;

    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(s->device, &dslci, NULL_POINTER, &s->tx_dsl) != VK_SUCCESS)
    {
        log_error("%s", "vkCreateDescriptorSetLayout (tx) failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_descriptor_pool(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    // One pool, sized for MAX_ATLASES combined-image-samplers + the
    // same number of sets. Descriptor sets are allocated on-demand
    // in upload_atlas.
    VkDescriptorPoolSize ps = { 0 };
    ps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = _VULKAN_RENDERER_INTERNAL__MAX_ATLASES;

    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets       = _VULKAN_RENDERER_INTERNAL__MAX_ATLASES;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(s->device, &pci, NULL_POINTER, &s->tx_descriptor_pool) != VK_SUCCESS)
    {
        log_error("%s", "vkCreateDescriptorPool (tx) failed");
        return FALSE;
    }
    return TRUE;
}

static boole _vulkan_renderer_internal__create_tx_pipeline(boole image)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkPipeline* pipeline = image ? &s->image_pipeline : &s->text_pipeline;
    VkPipelineLayout* pipeline_layout = image ? &s->image_pipeline_layout : &s->text_pipeline_layout;
    VkShaderModule* fragment = image ? &s->image_fs : &s->text_fs;
    if (!s->tx_vs && _vulkan_renderer_internal__compile_shader( _VULKAN_RENDERER_INTERNAL__TX_VS_SPV, sizeof(_VULKAN_RENDERER_INTERNAL__TX_VS_SPV), "tx.vert", &s->tx_vs) == FALSE)
    {
        return FALSE;
    }
    if (_vulkan_renderer_internal__compile_shader( image ? _VULKAN_RENDERER_INTERNAL__IMAGE_FS_SPV : _VULKAN_RENDERER_INTERNAL__TEXT_FS_SPV, image ? sizeof(_VULKAN_RENDERER_INTERNAL__IMAGE_FS_SPV) : sizeof(_VULKAN_RENDERER_INTERNAL__TEXT_FS_SPV), "text.frag", &*fragment) == FALSE)
    {
        return FALSE;
    }

    // Pipeline layout: the shared tx DSL + same push-constant range
    // the rect pipeline uses (viewport, 8 bytes vec2 at VS stage).
    VkPushConstantRange pcr = { 0 };
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset     = 0;
    pcr.size       = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &s->tx_dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(s->device, &plci, NULL_POINTER, &*pipeline_layout) != VK_SUCCESS)
    {
        log_error("%s", "vkCreatePipelineLayout (text) failed");
        return FALSE;
    }

    // Vertex input: tx_vertex struct -- pos (vec2) + color (vec4) + uv (vec2).
    VkVertexInputBindingDescription vib = { 0 };
    vib.binding   = 0;
    vib.stride    = sizeof(_vulkan_renderer_internal__tx_vertex_t);
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription via[3] = { 0 };
    via[0].location = 0;
    via[0].binding  = 0;
    via[0].format   = VK_FORMAT_R32G32_SFLOAT;
    via[0].offset   = offsetof(_vulkan_renderer_internal__tx_vertex_t, x);
    via[1].location = 1;
    via[1].binding  = 0;
    via[1].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    via[1].offset   = offsetof(_vulkan_renderer_internal__tx_vertex_t, r);
    via[2].location = 2;
    via[2].binding  = 0;
    via[2].format   = VK_FORMAT_R32G32_SFLOAT;
    via[2].offset   = offsetof(_vulkan_renderer_internal__tx_vertex_t, u);

    VkPipelineVertexInputStateCreateInfo visci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    visci.vertexBindingDescriptionCount   = 1;
    visci.pVertexBindingDescriptions      = &vib;
    visci.vertexAttributeDescriptionCount = 3;
    visci.pVertexAttributeDescriptions    = via;

    VkPipelineInputAssemblyStateCreateInfo iasci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Dynamic viewport + scissor (same as rect pipeline) so no re-
    // create on resize.
    VkPipelineViewportStateCreateInfo vpsci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpsci.viewportCount = 1;
    vpsci.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rsci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsci.polygonMode = VK_POLYGON_MODE_FILL;
    rsci.cullMode    = VK_CULL_MODE_NONE;
    rsci.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsci.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo msci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Separate-alpha blend -- same as every other backend per
    // VISUAL CONTRACT in renderer.h.
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.blendEnable         = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cbsci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbsci.attachmentCount = 1;
    cbsci.pAttachments    = &cba;

    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsci.dynamicStateCount = 2;
    dsci.pDynamicStates    = dyn_states;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s->tx_vs;
    stages[0].pName  = "main";
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = *fragment;
    stages[1].pName  = "main";
    uint32_t srgb = (s->swap_format == VK_FORMAT_R8G8B8A8_SRGB || s->swap_format == VK_FORMAT_B8G8R8A8_SRGB);
    VkSpecializationMapEntry color_entry = {0, 0, sizeof(srgb)};
    VkSpecializationInfo color_info = {1, &color_entry, sizeof(srgb), &srgb};
    stages[1].pSpecializationInfo = &color_info;

    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState      = &vpsci;
    gpci.pRasterizationState = &rsci;
    gpci.pMultisampleState   = &msci;
    gpci.pColorBlendState    = &cbsci;
    gpci.pDynamicState       = &dsci;
    gpci.layout              = *pipeline_layout;
    gpci.renderPass          = s->render_pass;
    gpci.subpass             = 0;
    if (vkCreateGraphicsPipelines(s->device, VK_NULL_HANDLE, 1, &gpci, NULL_POINTER, &*pipeline) != VK_SUCCESS)
    {
        log_error("%s", "vkCreateGraphicsPipelines (text) failed");
        return FALSE;
    }
    return TRUE;
}
static boole _vulkan_renderer_internal__create_text_pipeline(void) { return _vulkan_renderer_internal__create_tx_pipeline(FALSE); }
static boole _vulkan_renderer_internal__create_image_pipeline(void) { return _vulkan_renderer_internal__create_tx_pipeline(TRUE); }


static boole _vulkan_renderer_internal__create_tx_vertex_buffers(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;

    VkDeviceSize size = (VkDeviceSize)sizeof(_vulkan_renderer_internal__tx_vertex_t) *
                        _VULKAN_RENDERER_INTERNAL__MAX_TEXT_QUADS *
                        _VULKAN_RENDERER_INTERNAL__VERTS_PER_QUAD;

    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_FRAMES; i++)
    {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = size;
        bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s->device, &bci, NULL_POINTER, &s->text_vbo[i]) != VK_SUCCESS)
        {
            log_error("%s %d %s", "vkCreateBuffer (text vbo", i, ") failed");
            return FALSE;
        }

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(s->device, s->text_vbo[i], &req);

        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = _vulkan_renderer_internal__find_memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s->device, &mai, NULL_POINTER, &s->text_vbo_mem[i]) != VK_SUCCESS)
        {
            log_error("%s %d %s", "vkAllocateMemory (text vbo", i, ") failed");
            return FALSE;
        }
        vkBindBufferMemory(s->device, s->text_vbo[i], s->text_vbo_mem[i], 0);
        if (vkMapMemory(s->device, s->text_vbo_mem[i], 0, size, 0, (void**)&s->text_vbo_ptr[i]) != VK_SUCCESS)
        {
            log_error("%s %d %s", "vkMapMemory (text vbo", i, ") failed");
            return FALSE;
        }
    }
    return TRUE;
}

// Atlas pool: next-free-slot scan. Fixed size (MAX_ATLASES); used
// entries mark in_use=TRUE. Release clears in_use and destroys the
// underlying Vulkan objects.
static _vulkan_renderer_internal__atlas_entry_t* _vulkan_renderer_internal__alloc_atlas_slot(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_ATLASES; i++)
    {
        _vulkan_renderer_internal__atlas_entry_t* a = &s->atlas_pool[i];
        if (a->in_use == FALSE)
        {
            stdlib__memset(a, 0, sizeof(*a));
            a->in_use = TRUE;
            return a;
        }
    }
    return NULL_POINTER;
}

static void _vulkan_renderer_internal__retire_atlases(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    boole waited = s->adopted;
    for (int i = 0; i < _VULKAN_RENDERER_INTERNAL__MAX_ATLASES; i++) {
        _vulkan_renderer_internal__atlas_entry_t* a = &s->atlas_pool[i];
        if (!a->in_use || !a->pending_release) continue;
        if (!waited) { vkDeviceWaitIdle(s->device); waited = TRUE; }
        if (s->current_text_atlas == a) s->current_text_atlas = NULL_POINTER;
        _vulkan_renderer_internal__release_atlas(a);
    }
}

static void _vulkan_renderer_internal__release_atlas(_vulkan_renderer_internal__atlas_entry_t* a)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (a == NULL_POINTER)
    {
        return;
    }
    if (a->desc_set != VK_NULL_HANDLE)
    {
        vkFreeDescriptorSets(s->device, s->tx_descriptor_pool, 1, &a->desc_set);
    }
    if (a->view   != VK_NULL_HANDLE)
    {
        vkDestroyImageView(s->device, a->view,   NULL_POINTER);
    }
    if (a->image  != VK_NULL_HANDLE)
    {
        vkDestroyImage    (s->device, a->image,  NULL_POINTER);
    }
    if (a->memory != VK_NULL_HANDLE)
    {
        vkFreeMemory      (s->device, a->memory, NULL_POINTER);
    }
    stdlib__memset(a, 0, sizeof(*a));
}

// One-shot VkImage upload. Staging buffer -> VkImage copy via a
// dedicated command buffer; we block until the GPU finishes the
// copy before creating the SRV-equivalent (image view + descriptor
// set). Matches the "atlas loads are rare, simplicity > batching"
// trade-off the d3d12 path uses.
static boole _vulkan_renderer_internal__upload_atlas(_vulkan_renderer_internal__atlas_entry_t* a, const ubyte* pixels, int w, int h, VkFormat format, int bytes_per_pixel)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    VkDeviceSize byte_count = (VkDeviceSize)w * (VkDeviceSize)h * (VkDeviceSize)bytes_per_pixel;

    // 1. Staging buffer (host-visible) + copy pixels.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size        = byte_count;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s->device, &bci, NULL_POINTER, &staging) != VK_SUCCESS)
        {
            log_error("%s", "staging vkCreateBuffer failed");
            return FALSE;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(s->device, staging, &req);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = _vulkan_renderer_internal__find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s->device, &mai, NULL_POINTER, &staging_mem) != VK_SUCCESS)
        {
            vkDestroyBuffer(s->device, staging, NULL_POINTER);
            log_error("%s", "staging vkAllocateMemory failed");
            return FALSE;
        }
        vkBindBufferMemory(s->device, staging, staging_mem, 0);
        void* mapped = NULL_POINTER;
        vkMapMemory(s->device, staging_mem, 0, byte_count, 0, &mapped);
        stdlib__memcpy(mapped, pixels, (size_t)byte_count);
        vkUnmapMemory(s->device, staging_mem);
    }

    // 2. VkImage + device memory.
    {
        VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = format;
        ici.extent.width  = (uint32_t)w;
        ici.extent.height = (uint32_t)h;
        ici.extent.depth  = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(s->device, &ici, NULL_POINTER, &a->image) != VK_SUCCESS)
        {
            vkDestroyBuffer(s->device, staging, NULL_POINTER);
            vkFreeMemory   (s->device, staging_mem, NULL_POINTER);
            log_error("%s", "vkCreateImage (atlas) failed");
            return FALSE;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(s->device, a->image, &req);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = _vulkan_renderer_internal__find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(s->device, &mai, NULL_POINTER, &a->memory) != VK_SUCCESS)
        {
            vkDestroyBuffer(s->device, staging, NULL_POINTER);
            vkFreeMemory   (s->device, staging_mem, NULL_POINTER);
            vkDestroyImage (s->device, a->image, NULL_POINTER); a->image = VK_NULL_HANDLE;
            log_error("%s", "vkAllocateMemory (atlas) failed");
            return FALSE;
        }
        vkBindImageMemory(s->device, a->image, a->memory, 0);
    }

    // 3. One-shot command buffer: layout UNDEFINED -> TRANSFER_DST,
    // copy staging -> image, layout TRANSFER_DST -> SHADER_READ.
    // Allocated from the main command pool; submitted on the
    // main queue; we block via vkQueueWaitIdle since atlas uploads
    // are rare and this keeps the lifetimes simple.
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = s->command_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer up = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(s->device, &cbai, &up) != VK_SUCCESS)
    {
        vkDestroyBuffer(s->device, staging, NULL_POINTER);
        vkFreeMemory   (s->device, staging_mem, NULL_POINTER);
        log_error("%s", "atlas upload vkAllocateCommandBuffers failed");
        return FALSE;
    }
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(up, &cbbi);

    VkImageMemoryBarrier to_dst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    to_dst.srcAccessMask       = 0;
    to_dst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image               = a->image;
    to_dst.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.baseMipLevel   = 0;
    to_dst.subresourceRange.levelCount     = 1;
    to_dst.subresourceRange.baseArrayLayer = 0;
    to_dst.subresourceRange.layerCount     = 1;
    vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL_POINTER, 0, NULL_POINTER, 1, &to_dst);

    VkBufferImageCopy region = { 0 };
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width  = (uint32_t)w;
    region.imageExtent.height = (uint32_t)h;
    region.imageExtent.depth  = 1;
    vkCmdCopyBufferToImage(up, staging, a->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_read = to_dst;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL_POINTER, 0, NULL_POINTER, 1, &to_read);

    vkEndCommandBuffer(up);

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &up;
    VkFence upload_fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkResult upload_result = vkCreateFence(s->device, &fence_info, NULL_POINTER, &upload_fence);
    if (upload_result == VK_SUCCESS) upload_result = vkQueueSubmit(s->queue, 1, &si, upload_fence);
    if (upload_result == VK_SUCCESS) upload_result = vkWaitForFences(s->device, 1, &upload_fence, VK_TRUE, UINT64_MAX);
    if (upload_fence) vkDestroyFence(s->device, upload_fence, NULL_POINTER);
    vkFreeCommandBuffers(s->device, s->command_pool, 1, &up);
    vkDestroyBuffer(s->device, staging, NULL_POINTER);
    vkFreeMemory(s->device, staging_mem, NULL_POINTER);
    if (upload_result != VK_SUCCESS) return FALSE;

    // 4. ImageView + descriptor set.
    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image    = a->image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format   = format;
    ivci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s->device, &ivci, NULL_POINTER, &a->view) != VK_SUCCESS)
    {
        log_error("%s", "vkCreateImageView (atlas) failed");
        return FALSE;
    }

    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool     = s->tx_descriptor_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &s->tx_dsl;
    if (vkAllocateDescriptorSets(s->device, &dsai, &a->desc_set) != VK_SUCCESS)
    {
        log_error("%s", "vkAllocateDescriptorSets (atlas) failed");
        return FALSE;
    }

    VkDescriptorImageInfo dii = { 0 };
    dii.sampler     = s->tx_sampler;
    dii.imageView   = a->view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet          = a->desc_set;
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo      = &dii;
    vkUpdateDescriptorSets(s->device, 1, &wds, 0, NULL_POINTER);

    a->width  = w;
    a->height = h;
    return TRUE;
}

// Draw queued text runs. Mirrors the d3d12 flush_text:
// - append-only semantics (no reset of vert_count or run_count),
// - a single vkCmdDraw per run, binding that run's atlas descriptor
// first so glyphs sample the right texture.
static void _vulkan_renderer_internal__flush_text(void)
{
    _vulkan_renderer_internal__state_t* s = &_vulkan_renderer_internal__g;
    if (s->frame_active == FALSE)
    {
        return;
    }
    if (s->text_first_undrawn_run >= s->text_run_count)
    {
        return;
    }

    VkCommandBuffer cb = s->cmd_bufs[s->frame_index];

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->text_pipeline);

    VkBuffer     vbs[1] = { s->text_vbo[s->frame_index] };
    VkDeviceSize ofs[1] = { 0 };
    vkCmdBindVertexBuffers(cb, 0, 1, vbs, ofs);

    float viewport[2];
    viewport[0] = (float)s->viewport_w;
    viewport[1] = (float)s->viewport_h;
    vkCmdPushConstants(cb, s->text_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(viewport), viewport);

    for (int i = s->text_first_undrawn_run; i < s->text_run_count; i++)
    {
        _vulkan_renderer_internal__text_run_t* run = &s->text_runs[i];
        if (run->atlas == NULL_POINTER || run->vert_count <= 0)
        {
            continue;
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->text_pipeline_layout, 0, 1, &run->atlas->desc_set, 0, NULL_POINTER);
        vkCmdDraw(cb, (uint32_t)run->vert_count, 1, (uint32_t)run->vert_start, 0);
    }

    s->text_first_undrawn_run = s->text_run_count;
}

// ===== frame capture (see renderer.h) ========================================
// Copy through a host-visible staging buffer after the render pass, and read
// only after the submission fence completes.

/**
 * @brief size of the buffer renderer__request_capture would need: the swapchain's pixel size
 *
 * @param int64* out_w -> receives the width in pixels
 * @param int64* out_h -> receives the height in pixels
 *
 * @return boole -> FALSE before the renderer has a viewport
 */
boole renderer__capture_size(int64* out_w, int64* out_h)
{
    int64 w = (int64)_vulkan_renderer_internal__g.viewport_w;
    int64 h = (int64)_vulkan_renderer_internal__g.viewport_h;

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
 * @brief request readback of the active embedded frame or next standalone frame
 *
 * @param ubyte* out_rgba -> caller buffer, kept alive until frame completion
 * @param int64 cap -> buffer capacity in bytes
 *
 * @return boole -> TRUE when capture was armed
 */
boole renderer__request_capture(ubyte* out_rgba, int64 cap)
{
    _vulkan_renderer_internal__state_t* s=&_vulkan_renderer_internal__g;
    VkDeviceSize bytes=(VkDeviceSize)s->viewport_w*s->viewport_h*4;
    s->capture_done=FALSE;
    s->capture_out=NULL_POINTER;
    if(!s->device || (s->adopted && !s->frame_active) || !out_rgba || cap<0 || (VkDeviceSize)cap<bytes || !bytes ||
       !((s->adopted ? s->host_frame.usage : s->swap_usage) & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return FALSE;
    if(s->capture_capacity<bytes) {
        if(s->capture_buffer) vkDestroyBuffer(s->device,s->capture_buffer,NULL_POINTER);
        if(s->capture_memory) vkFreeMemory(s->device,s->capture_memory,NULL_POINTER);
        s->capture_buffer=VK_NULL_HANDLE;s->capture_memory=VK_NULL_HANDLE;s->capture_capacity=0;
        VkBufferCreateInfo info= { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size=bytes;info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateBuffer(s->device,&info,NULL_POINTER,&s->capture_buffer)!=VK_SUCCESS) return FALSE;
        VkMemoryRequirements need;vkGetBufferMemoryRequirements(s->device,s->capture_buffer,&need);
        VkMemoryAllocateInfo allocation= { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };allocation.allocationSize=need.size;
        allocation.memoryTypeIndex=_vulkan_renderer_internal__find_memory_type(need.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(allocation.memoryTypeIndex==UINT32_MAX || vkAllocateMemory(s->device,&allocation,NULL_POINTER,&s->capture_memory)!=VK_SUCCESS) return FALSE;
        if(vkBindBufferMemory(s->device,s->capture_buffer,s->capture_memory,0)!=VK_SUCCESS) return FALSE;
        s->capture_capacity=bytes;
    }
    s->capture_out=out_rgba; return TRUE;
}

/**
 * @brief report whether the requested frame copy completed
 *
 * @return boole -> FALSE
 */
boole renderer__capture_done(void) {return _vulkan_renderer_internal__g.capture_done;}


// Adopt only the device/queue. All command buffers, pipelines, buffers and
// synchronization objects below are private to meek-ui.
boole renderer__init_adopt_vulkan(const meek_vulkan_config_t* config)
{
    _vulkan_renderer_internal__state_t* s=&_vulkan_renderer_internal__g;
    if(!config || !config->device || !config->physical_device || !config->queue || s->device) return FALSE;
    if(config->color_format!=VK_FORMAT_R8G8B8A8_UNORM && config->color_format!=VK_FORMAT_B8G8R8A8_UNORM &&
       config->color_format!=VK_FORMAT_R8G8B8A8_SRGB && config->color_format!=VK_FORMAT_B8G8R8A8_SRGB) return FALSE;
    uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(config->physical_device,&count,NULL_POINTER);
    if(config->queue_family>=count) return FALSE;
    VkQueueFamilyProperties* families=GUI_MALLOC_T(sizeof(*families)*count,MM_TYPE__GENERIC);
    if(!families) return FALSE;
    vkGetPhysicalDeviceQueueFamilyProperties(config->physical_device,&count,families);
    boole graphics=(families[config->queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT)!=0;
    GUI_FREE(families); if(!graphics) return FALSE;
    stdlib__memset(s,0,sizeof(*s));
    s->adopted=TRUE; s->device=config->device; s->phys_device=config->physical_device;
    s->queue=config->queue; s->queue_family=config->queue_family; s->swap_format=config->color_format;
    if(!_vulkan_renderer_internal__create_render_pass() || !_vulkan_renderer_internal__create_command_pool_and_buffers() ||
       !_vulkan_renderer_internal__create_sync_primitives() || !_vulkan_renderer_internal__create_rect_pipeline() ||
       !_vulkan_renderer_internal__create_rect_vertex_buffers()) {renderer__shutdown();return FALSE;}
    log_info("%s","embedded Vulkan: adopted host device and graphics queue");
    return TRUE;
}

boole renderer__vulkan_target(const meek_vulkan_frame_t* frame)
{
    _vulkan_renderer_internal__state_t* s=&_vulkan_renderer_internal__g;
    if(!s->adopted || s->frame_active || !frame || !frame->image || !frame->view || !frame->width || !frame->height ||
       frame->width>8192 || frame->height>8192 || frame->wait_count>16 || (frame->wait_count && !frame->wait_semaphores) ||
       !(frame->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || frame->initial_layout==VK_IMAGE_LAYOUT_UNDEFINED ||
       frame->initial_layout==VK_IMAGE_LAYOUT_PREINITIALIZED || frame->final_layout==VK_IMAGE_LAYOUT_UNDEFINED ||
       frame->final_layout==VK_IMAGE_LAYOUT_PREINITIALIZED) return FALSE;
    if(s->framebuffers[0]) {vkDestroyFramebuffer(s->device,s->framebuffers[0],NULL_POINTER);s->framebuffers[0]=VK_NULL_HANDLE;}
    s->host_frame=*frame;
    for(uint32_t i=0;i<frame->wait_count;i++) s->host_waits[i]=frame->wait_semaphores[i];
    s->swap_extent=(VkExtent2D){frame->width,frame->height};
    s->swap_images[0]=frame->image; s->swap_image_index=0; s->swap_image_count=1;
    VkFramebufferCreateInfo info= { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    info.renderPass=s->render_pass;info.attachmentCount=1;info.pAttachments=&frame->view;
    info.width=frame->width;info.height=frame->height;info.layers=1;
    return vkCreateFramebuffer(s->device,&info,NULL_POINTER,&s->framebuffers[0])==VK_SUCCESS;
}
boole renderer__vulkan_frame_ok(void) {return _vulkan_renderer_internal__g.frame_ok;}

static void _vulkan_renderer_internal__record_capture(VkCommandBuffer cb)
{
    _vulkan_renderer_internal__state_t* s=&_vulkan_renderer_internal__g;
    if(!s->capture_out) return;
    VkImageLayout layout=s->adopted?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkImageMemoryBarrier barrier= { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout=layout;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=s->swap_images[s->swap_image_index];
    barrier.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,NULL_POINTER,0,NULL_POINTER,1,&barrier);
    VkBufferImageCopy copy={0};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.imageSubresource.layerCount=1;
    copy.imageExtent=(VkExtent3D){(uint32_t)s->viewport_w,(uint32_t)s->viewport_h,1};
    vkCmdCopyImageToBuffer(cb,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s->capture_buffer,1,&copy);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=layout;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,0,NULL_POINTER,0,NULL_POINTER,1,&barrier);
}
static void _vulkan_renderer_internal__finish_capture(void)
{
    _vulkan_renderer_internal__state_t* s=&_vulkan_renderer_internal__g;
    if(!s->capture_out) return;
    const ubyte* pixels=NULL_POINTER;
    size_t bytes=(size_t)s->viewport_w*s->viewport_h*4;
    if(vkMapMemory(s->device,s->capture_memory,0,bytes,0,(void**)&pixels)==VK_SUCCESS) {
        if(s->swap_format==VK_FORMAT_B8G8R8A8_UNORM || s->swap_format==VK_FORMAT_B8G8R8A8_SRGB) {
            for(size_t i=0;i<bytes;i+=4) {
                s->capture_out[i]=pixels[i+2];s->capture_out[i+1]=pixels[i+1];
                s->capture_out[i+2]=pixels[i];s->capture_out[i+3]=pixels[i+3];
            }
        } else stdlib__memcpy(s->capture_out,pixels,bytes);
        vkUnmapMemory(s->device,s->capture_memory);s->capture_done=TRUE;
    }
    s->capture_out=NULL_POINTER;
}

// Vulkan uses the portable painter for feature-complete single-threaded drawing.
GUI_API boole renderer_backend__needs_compositor(void) { return TRUE; }

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
