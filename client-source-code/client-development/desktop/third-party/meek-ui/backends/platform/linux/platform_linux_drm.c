// platforms/linux/platform_linux_drm.c - Linux DRM/KMS + GBM + EGL backend.
// Direct-to-GPU kiosk path for Linux. No X11, no Wayland, no
// compositor -- this talks to /dev/dri/card* through libdrm, maps
// GBM buffer objects into framebuffers, drives EGL on top of the
// GBM surface, and renders through gles3_renderer.c (same backend
// the Android port uses).
// Target workloads: embedded UIs, kiosks, signage, and the
// scenario where the developer ssh'd into a headless box and wants
// to draw directly on the framebuffer from a console login.
// Running on a desktop under X11 / Wayland will refuse with EBUSY
// when it tries to become DRM master -- that's correct; use the
// X11 backend (platform_linux_x11.c) there.
// COMPANION FILES in this directory:
// - fs_linux.c           POSIX file I/O + mmap
// - (eventually) platform_linux_x11.c  X11+GLX desktop backend
// DEPENDENCIES (link order in build.sh):
// -ldrm -lgbm -lEGL -lGLESv2 -linput -ludev -lm -ldl -lpthread
// ...minus libinput if we end up using raw evdev; the initial
// implementation reads /dev/input/event* directly with EVIOCGRAB
// so we don't pull libinput into the dependency set. Evdev is
// enough for a keyboard + mouse + touchscreen; if we ever need
// gesture recognition or per-seat config we'll switch to libinput.
// LIFECYCLE:
// main() -> platform__init     opens /dev/dri/card0, picks the first
// connected connector, allocates GBM
// surface, creates EGL context, inits
// renderer, opens input devices.
// main loop { platform__tick } drains input, renders one frame into
// the GBM buffer, flips the CRTC to
// that buffer.
// platform__shutdown           destroys renderer, releases GBM + DRM,
// closes input devices.
// PAGE FLIP MODEL:
// We double-buffer through gbm_surface_lock_front_buffer /
// gbm_surface_release_buffer. Each frame:
// 1. glClear + draw the scene into the EGL surface.
// 2. eglSwapBuffers hands the back buffer to GBM as the new
// "front".
// 3. gbm_surface_lock_front_buffer returns the just-rendered
// GBM BO. We convert it to a DRM framebuffer (cached per
// BO via gbm_bo_set_user_data so we don't re-add-fb every
// frame) and call drmModePageFlip(PAGE_FLIP_EVENT).
// 4. Wait for the flip-done event via drmHandleEvent on the
// drm fd -- that's the v-sync pace. Then release the
// previously-locked BO so GBM can recycle it for the next
// draw.
// First frame uses drmModeSetCrtc instead of PageFlip because
// there's nothing to flip FROM yet -- the CRTC has to be
// programmed at least once to establish the mode.

// Expose clock_gettime and O_CLOEXEC in strict C11 builds.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <linux/input.h>
#include <linux/input-event-codes.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "animator.h"
#include "renderer.h"
#include "widget_registry.h"
#include "widgets/widget_image_cache.h"
#include "font.h"
#include "fs.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "clib/stdlib.h"
#include "third_party/log.h"

#define _PLATFORM_INTERNAL
#include "platform.h"
#undef _PLATFORM_INTERNAL

// ============================================================================
// state
// ============================================================================

// Device caps we query once at init and stash. All the page-flip
// math needs at least crtc_id + mode + connector_id every frame, so
// keeping them on the struct avoids a re-walk through DRM's object
// tables.
typedef struct _platform_linux_drm_internal__device_t
{
    int           drm_fd;
    uint32_t      connector_id;
    uint32_t      crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc*  saved_crtc; // restored on shutdown so the console comes back intact.
    uint32_t      viewport_w;
    uint32_t      viewport_h;
} _platform_linux_drm_internal__device_t;

// libgbm + EGL state. The gbm_surface is what EGL draws into; after
// each SwapBuffers we lock the front BO, turn it into a DRM FB, and
// flip the CRTC to point at it.
typedef struct _platform_linux_drm_internal__gfx_t
{
    struct gbm_device*   gbm_dev;       // 0x0
    struct gbm_surface*  gbm_surface;   // 0x8

    EGLDisplay display;                 // 0x10
    EGLContext context;                 // 0x18
    EGLSurface surface;                 // 0x20
    EGLConfig  config;                  // 0x28

    // BO we handed to drmModePageFlip last frame; released after the next.
    struct gbm_bo* locked_bo;           // 0x30
    // has drmModeSetCrtc run at least once (i.e. can we start using PageFlip)?
    boole          crtc_set;            // 0x38
} _platform_linux_drm_internal__gfx_t;

// Input state. We open every /dev/input/event* file descriptor we
// can read, and poll() them all per frame. The primary pointer (x,
// y) state is tracked here because evdev reports relative motion
// for mice; we need to accumulate into absolute coords ourselves.
#define _PLATFORM_LINUX_DRM_INTERNAL__MAX_INPUT_FDS 16

typedef struct _platform_linux_drm_internal__input_t
{
    int   fds[_PLATFORM_LINUX_DRM_INTERNAL__MAX_INPUT_FDS];   // 0x0
    int   fd_count;                                           // 0x40

    // Accumulated pointer position. Clamped to viewport on each update.
    int64 ptr_x;                                              // 0x48
    int64 ptr_y;                                              // 0x50

    // Touchscreen state. ABS_MT_* events arrive as a per-slot
    // batch terminated by SYN_REPORT. We only track slot 0 (primary
    // finger) -- matches the Android touch state machine.
    boole touch_active;                                       // 0x58
    int32_t touch_x_raw;                                      // 0x5C
    int32_t touch_y_raw;                                      // 0x60
    int32_t touch_x_min;                                      // 0x64
    int32_t touch_x_max;                                      // 0x68
    int32_t touch_y_min;                                      // 0x6C
    int32_t touch_y_max;                                      // 0x70
} _platform_linux_drm_internal__input_t;

typedef struct _platform_linux_drm_internal__state_t
{
    _platform_linux_drm_internal__device_t dev;
    _platform_linux_drm_internal__gfx_t    gfx;
    _platform_linux_drm_internal__input_t  input;

    boole      should_close;
    gui_color_t  clear_color;
} _platform_linux_drm_internal__state_t;

static _platform_linux_drm_internal__state_t _platform_linux_drm_internal__g;

// Panel-DPI-derived scale factor, set by the connector walk in
// try_card and consumed by pick_ui_scale after init. File-scope
// so try_card can write it and the picker can read it without
// threading a pointer through the call chain.
static float _platform_linux_drm_internal__scale_hint = 1.0f;

// ============================================================================
// forward decls
// ============================================================================

static boole _platform_linux_drm_internal__open_drm(void);
static void  _platform_linux_drm_internal__close_drm(void);
static boole _platform_linux_drm_internal__init_gbm_egl(void);
static void  _platform_linux_drm_internal__term_gbm_egl(void);
static boole _platform_linux_drm_internal__open_input(void);
static void  _platform_linux_drm_internal__close_input(void);
static void  _platform_linux_drm_internal__poll_input(void);
static uint32_t _platform_linux_drm_internal__bo_to_fb(struct gbm_bo* bo);
static void  _platform_linux_drm_internal__destroy_fb(struct gbm_bo* bo, void* data);
static void  _platform_linux_drm_internal__page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data);
static gui_handler_fn _platform_linux_drm_internal__resolve_host_symbol(char* name);
static float _platform_linux_drm_internal__pick_ui_scale(void);

// ============================================================================
// public API
// ============================================================================

/**
 * @brief initialize the platform and renderer. Called once at startup.
 *
 * @param const gui_app_config_t* cfg -> Window title / size / clear color.
 *
 * @return boole -> TRUE on success; FALSE if window creation or renderer init failed.
 */
boole platform__init(const gui_app_config_t* cfg)
{
    // tracker first -- matches the Android + Win32 ordering.
    memory_manager__init();

    stdlib__memset(&_platform_linux_drm_internal__g, 0, sizeof(_platform_linux_drm_internal__g));

    if (cfg == NULL_POINTER)
    {
        log_error("%s", "platform__init: cfg is NULL");
        return FALSE;
    }

    _platform_linux_drm_internal__g.clear_color = cfg->clear_color;

    if (_platform_linux_drm_internal__open_drm() == FALSE)
    {
        log_error("%s", "platform__init: open_drm failed");
        memory_manager__shutdown();
        return FALSE;
    }

    if (_platform_linux_drm_internal__init_gbm_egl() == FALSE)
    {
        log_error("%s", "platform__init: init_gbm_egl failed");
        _platform_linux_drm_internal__close_drm();
        memory_manager__shutdown();
        return FALSE;
    }

    if (renderer__init(NULL_POINTER) == FALSE)
    {
        log_error("%s", "platform__init: renderer__init failed");
        _platform_linux_drm_internal__term_gbm_egl();
        _platform_linux_drm_internal__close_drm();
        memory_manager__shutdown();
        return FALSE;
    }

    widget_registry__bootstrap_builtins();
    if (font__init() == FALSE)
    {
        log_error("%s", "platform__init: font__init failed");
        renderer__shutdown();
        _platform_linux_drm_internal__term_gbm_egl();
        _platform_linux_drm_internal__close_drm();
        memory_manager__shutdown();
        return FALSE;
    }

    // dlsym-based handler resolver. Mirror of the Android + Win32
    // paths -- lets on_click= / on_change= names in .ui files
    // auto-resolve to UI_HANDLER-marked functions in the host exe
    // without the host registering each one. RTLD_DEFAULT searches
    // the main program + every already-loaded .so.
    scene__set_symbol_resolver(_platform_linux_drm_internal__resolve_host_symbol);

    // Pick a UI scale from the panel's physical size (connector
    // reports mm width/height). Drives the same code path as Android's
    // density-bucket-based scale.
    float ui_scale = _platform_linux_drm_internal__pick_ui_scale();
    if (ui_scale > 0.0f)
    {
        scene__set_scale(ui_scale);
    }

    if (_platform_linux_drm_internal__open_input() == FALSE)
    {
        // Input is non-fatal -- a kiosk with no human input device
        // (e.g. a digital signage screen driven purely by a timer)
        // is a legitimate use case. Log and continue.
        log_warn("%s", "platform__init: no input devices opened -- UI will be non-interactive");
    }

    log_info("%s %u %s %u %s", "platform_linux_drm: up (", _platform_linux_drm_internal__g.dev.viewport_w, "x", _platform_linux_drm_internal__g.dev.viewport_h, ")");
    return TRUE;
}

/**
 * @brief run one iteration of the platform's main loop: pump OS events, capture the frame timestamp, resolve styles, tick the animator, lay out the tree, emit draws, and present. Returns FALSE when the user has asked to close the application (window close, Android APP_CMD_DESTROY, etc.).
 *
 * @return boole -> TRUE to continue running; FALSE to exit.
 */
boole platform__tick(void)
{
    if (_platform_linux_drm_internal__g.should_close)
    {
        return FALSE;
    }

    // Input before rendering so the frame sees fresh cursor / keys.
    // We don't block on input -- the polling is non-blocking and
    // we render every vsync regardless, same as Android.
    _platform_linux_drm_internal__poll_input();

    // Frame timestamp. CLOCK_MONOTONIC -> ms, same as Android.
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64 now_ms = (int64)ts.tv_sec * 1000 + (int64)(ts.tv_nsec / 1000000);
        scene__begin_frame_time(now_ms);
    }

    int64 vw = (int64)_platform_linux_drm_internal__g.dev.viewport_w;
    int64 vh = (int64)_platform_linux_drm_internal__g.dev.viewport_h;

    scene__resolve_styles();
    animator__tick();
    scene__layout(vw, vh);

    renderer__begin_frame(vw, vh, _platform_linux_drm_internal__g.clear_color);
    scene__emit_draws();
    renderer__end_frame();

    // Publish back buffer -> GBM front BO -> DRM framebuffer -> CRTC.
    if (eglSwapBuffers(_platform_linux_drm_internal__g.gfx.display, _platform_linux_drm_internal__g.gfx.surface) == FALSE)
    {
        log_error("%s %x %s", "eglSwapBuffers failed (0x", (unsigned)eglGetError(), ")");
        return TRUE;
    }

    struct gbm_bo* next_bo = gbm_surface_lock_front_buffer(_platform_linux_drm_internal__g.gfx.gbm_surface);
    if (next_bo == NULL_POINTER)
    {
        log_error("%s", "gbm_surface_lock_front_buffer returned NULL");
        return TRUE;
    }

    uint32_t fb_id = _platform_linux_drm_internal__bo_to_fb(next_bo);
    if (fb_id == 0)
    {
        log_error("%s", "failed to convert GBM BO to DRM framebuffer");
        gbm_surface_release_buffer(_platform_linux_drm_internal__g.gfx.gbm_surface, next_bo);
        return TRUE;
    }

    if (_platform_linux_drm_internal__g.gfx.crtc_set == FALSE)
    {
        // First frame: program the CRTC so there's something to flip
        // FROM. drmModeSetCrtc is synchronous -- returns once the
        // display controller has the new FB + mode, no event.
        int r = drmModeSetCrtc(_platform_linux_drm_internal__g.dev.drm_fd, _platform_linux_drm_internal__g.dev.crtc_id, fb_id, 0, 0, &_platform_linux_drm_internal__g.dev.connector_id, 1, &_platform_linux_drm_internal__g.dev.mode);
        if (r != 0)
        {
            log_error("%s %s", "drmModeSetCrtc failed:", strerror(errno));
            gbm_surface_release_buffer(_platform_linux_drm_internal__g.gfx.gbm_surface, next_bo);
            return TRUE;
        }
        _platform_linux_drm_internal__g.gfx.crtc_set   = TRUE;
        _platform_linux_drm_internal__g.gfx.locked_bo  = next_bo;
        // On the very first frame there is no previous BO to release
        // so we return immediately; subsequent frames go through the
        // PageFlip path below.
        return TRUE;
    }

    // PageFlip with vsync wait. The kernel schedules the flip for
    // the next vblank and posts a DRM event to drm_fd when it's
    // done; drmHandleEvent dispatches that event to our callback
    // which clears the "flip pending" flag. We spin on poll() until
    // the event arrives -- this is the v-sync pace of the loop.
    boole flip_pending = TRUE;
    int r = drmModePageFlip(_platform_linux_drm_internal__g.dev.drm_fd, _platform_linux_drm_internal__g.dev.crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &flip_pending);
    if (r != 0)
    {
        log_error("%s %s", "drmModePageFlip failed:", strerror(errno));
        gbm_surface_release_buffer(_platform_linux_drm_internal__g.gfx.gbm_surface, next_bo);
        return TRUE;
    }

    while (flip_pending)
    {
        struct pollfd pfd;
        pfd.fd      = _platform_linux_drm_internal__g.dev.drm_fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 1000);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            log_error("%s %s", "poll(drm_fd) failed:", strerror(errno));
            break;
        }
        if (pr == 0)
        {
            // 1 s with no vblank is pathological -- the display
            // pipeline is probably wedged. Bail out so the caller
            // sees a non-responsive render loop rather than a
            // blocked main thread.
            log_warn("%s", "drmModePageFlip: no event within 1s; aborting wait");
            break;
        }

        drmEventContext evctx;
        stdlib__memset(&evctx, 0, sizeof(evctx));
        evctx.version           = DRM_EVENT_CONTEXT_VERSION;
        evctx.page_flip_handler = _platform_linux_drm_internal__page_flip_handler;
        drmHandleEvent(_platform_linux_drm_internal__g.dev.drm_fd, &evctx);
    }

    // Flip finished -- the OLD locked_bo is no longer on screen and
    // can go back to the GBM free list. Replace with the new one.
    if (_platform_linux_drm_internal__g.gfx.locked_bo != NULL_POINTER)
    {
        gbm_surface_release_buffer(_platform_linux_drm_internal__g.gfx.gbm_surface, _platform_linux_drm_internal__g.gfx.locked_bo);
    }
    _platform_linux_drm_internal__g.gfx.locked_bo = next_bo;

    return TRUE;
}

/**
 * @brief tear down in reverse of init. Safe to call if init bailed out.
 *
 * @return void
 */
void platform__shutdown(void)
{
    // Image cache + font subsystem release renderer-owned textures,
    // so they have to run while the renderer is still alive.
    widget_image__cache_shutdown();
    font__shutdown();
    renderer__shutdown();
    _platform_linux_drm_internal__close_input();
    _platform_linux_drm_internal__term_gbm_egl();
    _platform_linux_drm_internal__close_drm();
    memory_manager__shutdown();
}

// Capture API: stubbed. DRM has no window manager to BitBlt from;
// a proper implementation would glReadPixels the default framebuffer
// after present. Not implemented yet -- visual-test runner is
// Windows-only.
/**
 * @brief capture the current window client area to a 32-bit BGRA .bmp file. Returns TRUE on success, FALSE on any IO or API failure. Intended for the visual-regression test runner under `tests/visual/`; not used by normal host apps. Path is a filesystem path in the native encoding. Overwrites any existing file. Format choice: BMP over PNG because BMP needs no compression dependency (stb_image_write + zlib) and every browser / image viewer displays it natively. Resulting files are larger than PNG but test captures are small (typical 800x600 ~= 1.8 MB). Must be called AFTER a frame has been rendered + presented (the captured pixels come from the window's composited client area); typically the test runner ticks 5+ frames to let animations settle, then calls this. Only the Win32 backend implements this today; other backends return FALSE.
 *
 * @param const char* path -> output filesystem path.
 *
 * @return boole -> TRUE on success.
 */
boole platform__capture_bmp(const char* path)
{
    (void)path;
    return FALSE;
}

void platform__set_topmost(void) { /* DRM: no window manager; no-op. */ }

// ============================================================================
// DRM device + mode setup
// ============================================================================
// Walk /dev/dri/card*, for each card enumerate connectors, pick the
// first one whose status is DRM_MODE_CONNECTED and has a preferred
// mode, then find an encoder + CRTC combo that can drive it. This
// is the canonical "find a display to draw on" dance every DRM
// client does; libdrm doesn't offer a higher-level helper.

static boole _platform_linux_drm_internal__try_card(const char* path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        return FALSE;
    }

    // Require the card to support DRM_CLIENT_CAP_UNIVERSAL_PLANES
    // because we lean on that for atomic modeset later; at minimum
    // we need DRM master to do any output, which requires no other
    // DRM master on this device. A session with X11 / a logged-in
    // GDM running will fail here with EBUSY -- correct; the user
    // should use the X11 backend in that case.
    drmModeRes* res = drmModeGetResources(fd);
    if (res == NULL_POINTER)
    {
        close(fd);
        return FALSE;
    }

    drmModeConnector* chosen_conn = NULL_POINTER;
    for (int i = 0; i < res->count_connectors; i++)
    {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (c == NULL_POINTER)
        {
            continue;
        }
        if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0)
        {
            drmModeFreeConnector(c);
            continue;
        }
        chosen_conn = c;
        break;
    }

    if (chosen_conn == NULL_POINTER)
    {
        drmModeFreeResources(res);
        close(fd);
        return FALSE;
    }

    // Prefer the mode flagged PREFERRED by the connector; fall back
    // to mode 0 (which on modern drivers is also the preferred one,
    // but not guaranteed on older kernels).
    drmModeModeInfo* mode = NULL_POINTER;
    for (int i = 0; i < chosen_conn->count_modes; i++)
    {
        if (chosen_conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)
        {
            mode = &chosen_conn->modes[i];
            break;
        }
    }
    if (mode == NULL_POINTER)
    {
        mode = &chosen_conn->modes[0];
    }

    // Find a CRTC that can drive this connector. The connector's
    // current encoder (if any) already knows a CRTC id; otherwise
    // we scan encoders for one whose possible_crtcs bitmask
    // intersects any available CRTC index.
    uint32_t crtc_id = 0;
    if (chosen_conn->encoder_id != 0)
    {
        drmModeEncoder* enc = drmModeGetEncoder(fd, chosen_conn->encoder_id);
        if (enc != NULL_POINTER)
        {
            crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    if (crtc_id == 0)
    {
        for (int ei = 0; ei < chosen_conn->count_encoders && crtc_id == 0; ei++)
        {
            drmModeEncoder* enc = drmModeGetEncoder(fd, chosen_conn->encoders[ei]);
            if (enc == NULL_POINTER)
            {
                continue;
            }
            for (int ci = 0; ci < res->count_crtcs; ci++)
            {
                if (enc->possible_crtcs & (1 << ci))
                {
                    crtc_id = res->crtcs[ci];
                    break;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }

    if (crtc_id == 0)
    {
        log_warn("%s %u %s %s", "drm: connector", chosen_conn->connector_id, "has no drivable CRTC on", path);
        drmModeFreeConnector(chosen_conn);
        drmModeFreeResources(res);
        close(fd);
        return FALSE;
    }

    // Save the current CRTC state so shutdown can restore it --
    // otherwise the VT stays on whatever we programmed (blank,
    // usually) after the app exits. This is what keeps a tty
    // returning to readable text after the program quits.
    drmModeCrtc* saved = drmModeGetCrtc(fd, crtc_id);

    _platform_linux_drm_internal__g.dev.drm_fd       = fd;
    _platform_linux_drm_internal__g.dev.connector_id = chosen_conn->connector_id;
    _platform_linux_drm_internal__g.dev.crtc_id      = crtc_id;
    _platform_linux_drm_internal__g.dev.mode         = *mode;
    _platform_linux_drm_internal__g.dev.saved_crtc   = saved;
    _platform_linux_drm_internal__g.dev.viewport_w   = mode->hdisplay;
    _platform_linux_drm_internal__g.dev.viewport_h   = mode->vdisplay;

    // Stash connector mm size for the DPI calc. We only need
    // mmWidth -- hdisplay / mmWidth is pixels/mm, times 25.4 =
    // DPI. Keep the connector alive across init just long enough
    // for _platform_linux_drm_internal__pick_ui_scale to read it,
    // but we can stash the numbers now and free the struct.
    // For simplicity we compute DPI inline here and stash scale
    // via a static slot the scale picker reads. Keeps the device
    // struct DRM-pure.
    if (chosen_conn->mmWidth > 0)
    {
        float dpi = (float)mode->hdisplay * 25.4f / (float)chosen_conn->mmWidth;
        _platform_linux_drm_internal__scale_hint = dpi / 96.0f; // 96 dpi = 1.0x by convention (same as Windows).
    }
    else
    {
        _platform_linux_drm_internal__scale_hint = 1.0f;
    }

    log_info("%s %s %s %u %s %u %s %u %s %u %s %u %s", "drm: card=", path, "connector=", chosen_conn->connector_id, "crtc=", crtc_id, "mode=", (unsigned)mode->hdisplay, "x", (unsigned)mode->vdisplay, "@", (unsigned)mode->vrefresh, "Hz");

    drmModeFreeConnector(chosen_conn);
    drmModeFreeResources(res);
    return TRUE;
}

// Detect WSL by peeking at /proc/sys/kernel/osrelease. WSL1 reports a
// "microsoft" substring; WSL2 reports "WSL2". Either way, DRM/KMS isn't
// available to userspace -- there's no /dev/dri/card* backed by a real
// display controller -- so a clear redirect saves users from staring at
// a generic "no card found" message and wondering what they're missing.
static boole _platform_linux_drm_internal__running_under_wsl(void)
{
    int fd = open("/proc/sys/kernel/osrelease", O_RDONLY);
    if (fd < 0)
    {
        return FALSE;
    }
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
    {
        return FALSE;
    }
    buf[n] = 0;
    // Case-insensitive search for either "microsoft" (WSL1/WSL2) or
    // "WSL" as a standalone token (WSL2 kernel). strcasestr is a GNU
    // extension but Linux is our only target for this file.
    if (stdlib__strstr(buf, "microsoft") || stdlib__strstr(buf, "Microsoft") || stdlib__strstr(buf, "WSL"))
    {
        return TRUE;
    }
    return FALSE;
}

static boole _platform_linux_drm_internal__open_drm(void)
{
    // Try /dev/dri/card0..card7 in order. Most systems only have
    // card0 (the integrated GPU), but machines with a discrete GPU
    // plus the iGPU present both (card0 = iGPU, card1 = dGPU, or
    // vice versa depending on kernel enumeration order). We take
    // the first one that has a connected connector -- not the
    // most robust choice but good enough for a kiosk box where
    // there's exactly one display attached.
    int cards_seen = 0;
    for (int i = 0; i < 8; i++)
    {
        char path[32];
        stdlib__strcpy_safe(path, (int64)sizeof(path), "/dev/dri/card");
        stdlib__i64toa(path + stdlib__strlen(path), (int64)sizeof(path) - stdlib__strlen(path), (int64)i);
        struct stat st;
        if (stat(path, &st) != 0)
        {
            continue;
        }
        cards_seen++;
        if (_platform_linux_drm_internal__try_card(path))
        {
            return TRUE;
        }
    }
    // Direct the user at the most likely fix instead of just reporting
    // "nothing found". The two common cases are (1) WSL, where DRM is
    // structurally unavailable, and (2) a real Linux box where the
    // display manager currently owns the GPU. cards_seen==0 means no
    // /dev/dri/card* files at all (kernel has no DRM driver loaded);
    // cards_seen>0 means cards exist but none had a connected display
    // we could drive, which usually means X11/Wayland is holding them.
    if (_platform_linux_drm_internal__running_under_wsl())
    {
        log_error("%s", "drm: WSL detected -- DRM/KMS isn't available under WSL.");
        log_error("%s", "drm: use the X11 demo instead:  cd demo-linux-x11 && ./build.sh");
        return FALSE;
    }
    if (cards_seen == 0)
    {
        log_error("%s", "drm: no /dev/dri/card* files on this system. Kernel DRM driver not loaded?");
    }
    else
    {
        log_error("%s %d %s", "drm:", cards_seen, "/dev/dri/card* found but none had a connected display.");
        log_error("%s", "drm: is X11 / Wayland running? Switch to a text console (Ctrl+Alt+F3) or stop the DM:");
        log_error("%s", "drm:   sudo systemctl stop gdm    (or sddm / lightdm)");
    }
    return FALSE;
}

static void _platform_linux_drm_internal__close_drm(void)
{
    // Restore the CRTC the VT was using before we took over, so
    // exit leaves the console readable. If we never programmed
    // the CRTC (init failure), saved_crtc is NULL and we skip.
    if (_platform_linux_drm_internal__g.dev.saved_crtc != NULL_POINTER)
    {
        drmModeCrtc* sc = _platform_linux_drm_internal__g.dev.saved_crtc;
        if (_platform_linux_drm_internal__g.dev.drm_fd >= 0)
        {
            drmModeSetCrtc(_platform_linux_drm_internal__g.dev.drm_fd, sc->crtc_id, sc->buffer_id, sc->x, sc->y, &_platform_linux_drm_internal__g.dev.connector_id, 1, &sc->mode);
        }
        drmModeFreeCrtc(sc);
        _platform_linux_drm_internal__g.dev.saved_crtc = NULL_POINTER;
    }
    if (_platform_linux_drm_internal__g.dev.drm_fd >= 0)
    {
        close(_platform_linux_drm_internal__g.dev.drm_fd);
        _platform_linux_drm_internal__g.dev.drm_fd = -1;
    }
}

// ============================================================================
// GBM + EGL
// ============================================================================
// GBM (Generic Buffer Manager) wraps the DRM dumb-buffer / GEM
// allocation interface. Its big job here is exposing a native-window-
// equivalent (gbm_surface*) that the EGL_KHR_platform_gbm extension
// accepts as an EGLNativeWindowType -- that's how we get GLES
// rendering into buffers that DRM can scan out.
// We request GBM_FORMAT_XRGB8888 (no alpha channel in scan-out
// buffer) because the DRM plane underneath is most commonly set to
// that -- the framebuffer doesn't need alpha, and sRGB encoded as
// XRGB8888 matches what every display controller defaults to.
// The EGL_KHR_surfaceless_context extension isn't needed; we have
// a real GBM surface to bind to. Some older GPUs advertise only
// EGL_EXT_platform_base (no _KHR_ variant), so we probe
// eglGetPlatformDisplayEXT via eglGetProcAddress first and fall
// back to eglGetDisplay(gbm_dev) -- which MESA accepts even without
// the extension because GBM is Mesa's native display type.

static boole _platform_linux_drm_internal__init_gbm_egl(void)
{
    // GBM device wraps the DRM fd.
    struct gbm_device* gbm = gbm_create_device(_platform_linux_drm_internal__g.dev.drm_fd);
    if (gbm == NULL_POINTER)
    {
        log_error("%s", "gbm_create_device failed");
        return FALSE;
    }
    _platform_linux_drm_internal__g.gfx.gbm_dev = gbm;

    // Surface: one buffer object per frame, GBM swaps them behind
    // eglSwapBuffers / lock_front_buffer. SCANOUT + RENDERING usage
    // tells the kernel this memory has to satisfy BOTH the display
    // controller's scan-out constraints (contiguous, specific
    // stride alignment) AND the GPU's rendering constraints
    // (tiling, etc.). Mesa falls back to a linear layout on
    // intersections it can't satisfy.
    struct gbm_surface* gs = gbm_surface_create( gbm, _platform_linux_drm_internal__g.dev.mode.hdisplay, _platform_linux_drm_internal__g.dev.mode.vdisplay, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (gs == NULL_POINTER)
    {
        log_error("%s", "gbm_surface_create failed");
        gbm_device_destroy(gbm);
        _platform_linux_drm_internal__g.gfx.gbm_dev = NULL_POINTER;
        return FALSE;
    }
    _platform_linux_drm_internal__g.gfx.gbm_surface = gs;

    // EGL display via the platform-gbm extension if available.
    // eglGetPlatformDisplay is in EGL 1.5; older drivers need the
    // EXT suffixed variant pulled in via eglGetProcAddress.
    PFNEGLGETPLATFORMDISPLAYEXTPROC peglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (peglGetPlatformDisplayEXT != NULL_POINTER)
    {
        dpy = peglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL_POINTER);
    }
    if (dpy == EGL_NO_DISPLAY)
    {
        // Fallback: plain eglGetDisplay with the GBM device as the
        // native display. Mesa handles this correctly even without
        // the extension; other implementations might not, in which
        // case we error out.
        dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    }
    if (dpy == EGL_NO_DISPLAY)
    {
        log_error("%s", "eglGetDisplay(gbm) failed");
        return FALSE;
    }
    if (eglInitialize(dpy, NULL_POINTER, NULL_POINTER) == FALSE)
    {
        log_error("%s %x %s", "eglInitialize failed (0x", (unsigned)eglGetError(), ")");
        return FALSE;
    }
    _platform_linux_drm_internal__g.gfx.display = dpy;

    // Config: GLES 3 capable, window surface, 8888 (no depth/stencil).
    // Must match the GBM surface format -- eglChooseConfig will not
    // implicitly convert XRGB8888 to RGB565 etc.
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };

    EGLConfig cfg;
    EGLint    num_cfg = 0;
    if (eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num_cfg) == FALSE || num_cfg < 1)
    {
        // Some stacks need EGL_NATIVE_VISUAL_ID to match the GBM
        // format literally. Walk the config list and pick one
        // whose visual id equals GBM_FORMAT_XRGB8888.
        EGLint total = 0;
        eglGetConfigs(dpy, NULL_POINTER, 0, &total);
        if (total <= 0)
        {
            log_error("%s", "no EGL configs on display");
            return FALSE;
        }
        EGLConfig* all = (EGLConfig*)GUI_MALLOC_T(sizeof(EGLConfig) * (size_t)total, MM_TYPE__GENERIC);
        eglGetConfigs(dpy, all, total, &total);
        boole found = FALSE;
        for (int i = 0; i < total; i++)
        {
            EGLint id = 0;
            eglGetConfigAttrib(dpy, all[i], EGL_NATIVE_VISUAL_ID, &id);
            if (id == (EGLint)GBM_FORMAT_XRGB8888)
            {
                cfg = all[i];
                found = TRUE;
                break;
            }
        }
        GUI_FREE(all);
        if (found == FALSE)
        {
            log_error("%s", "no EGL config matching GBM_FORMAT_XRGB8888");
            return FALSE;
        }
    }
    _platform_linux_drm_internal__g.gfx.config = cfg;

    // Bind the ES API *before* creating the context -- default is
    // EGL_OPENGL_API which produces a compatibility-profile GL
    // context on some drivers and a GLES context on others. Pin
    // it so we always get GLES.
    if (eglBindAPI(EGL_OPENGL_ES_API) == FALSE)
    {
        log_error("%s", "eglBindAPI(EGL_OPENGL_ES_API) failed");
        return FALSE;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT)
    {
        log_error("%s %x %s", "eglCreateContext failed (0x", (unsigned)eglGetError(), ")");
        return FALSE;
    }
    _platform_linux_drm_internal__g.gfx.context = ctx;

    // Window surface on the GBM surface. eglCreateWindowSurface
    // accepts a gbm_surface* as the native window on Mesa.
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL_POINTER);
    if (surf == EGL_NO_SURFACE)
    {
        log_error("%s %x %s", "eglCreateWindowSurface(gbm_surface) failed (0x", (unsigned)eglGetError(), ")");
        eglDestroyContext(dpy, ctx);
        _platform_linux_drm_internal__g.gfx.context = EGL_NO_CONTEXT;
        return FALSE;
    }
    _platform_linux_drm_internal__g.gfx.surface = surf;

    if (eglMakeCurrent(dpy, surf, surf, ctx) == FALSE)
    {
        log_error("%s %x %s", "eglMakeCurrent failed (0x", (unsigned)eglGetError(), ")");
        return FALSE;
    }

    // Default EGL swap interval is implementation-defined -- on
    // Mesa it's 1 (vsync-limited). We pin it to 1 anyway so the
    // page-flip loop can rely on vblank pacing.
    eglSwapInterval(dpy, 1);

    return TRUE;
}

static void _platform_linux_drm_internal__term_gbm_egl(void)
{
    if (_platform_linux_drm_internal__g.gfx.display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(_platform_linux_drm_internal__g.gfx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (_platform_linux_drm_internal__g.gfx.locked_bo != NULL_POINTER)
    {
        gbm_surface_release_buffer(_platform_linux_drm_internal__g.gfx.gbm_surface, _platform_linux_drm_internal__g.gfx.locked_bo);
        _platform_linux_drm_internal__g.gfx.locked_bo = NULL_POINTER;
    }
    if (_platform_linux_drm_internal__g.gfx.surface != EGL_NO_SURFACE)
    {
        eglDestroySurface(_platform_linux_drm_internal__g.gfx.display, _platform_linux_drm_internal__g.gfx.surface);
        _platform_linux_drm_internal__g.gfx.surface = EGL_NO_SURFACE;
    }
    if (_platform_linux_drm_internal__g.gfx.context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(_platform_linux_drm_internal__g.gfx.display, _platform_linux_drm_internal__g.gfx.context);
        _platform_linux_drm_internal__g.gfx.context = EGL_NO_CONTEXT;
    }
    if (_platform_linux_drm_internal__g.gfx.display != EGL_NO_DISPLAY)
    {
        eglTerminate(_platform_linux_drm_internal__g.gfx.display);
        _platform_linux_drm_internal__g.gfx.display = EGL_NO_DISPLAY;
    }
    if (_platform_linux_drm_internal__g.gfx.gbm_surface != NULL_POINTER)
    {
        gbm_surface_destroy(_platform_linux_drm_internal__g.gfx.gbm_surface);
        _platform_linux_drm_internal__g.gfx.gbm_surface = NULL_POINTER;
    }
    if (_platform_linux_drm_internal__g.gfx.gbm_dev != NULL_POINTER)
    {
        gbm_device_destroy(_platform_linux_drm_internal__g.gfx.gbm_dev);
        _platform_linux_drm_internal__g.gfx.gbm_dev = NULL_POINTER;
    }
}

// ============================================================================
// BO -> DRM framebuffer cache
// ============================================================================
// Every GBM BO needs a DRM framebuffer id before drmModeSetCrtc /
// drmModePageFlip can point at it. drmModeAddFB2 is not free (it
// kmallocs a framebuffer_info), so we stash the id on the BO via
// gbm_bo_set_user_data. Next time the same BO is returned from
// lock_front_buffer (GBM recycles a small pool), we reuse the
// cached id instead of adding another FB.
// The user-data destroy callback fires when the BO is destroyed
// (either by gbm_surface_destroy or by GBM's internal pool
// replacement), at which point we drmModeRmFB the cached id.

typedef struct _platform_linux_drm_internal__fb_cache_t
{
    uint32_t fb_id;    // 0x0
    int      drm_fd;   // 0x4
} _platform_linux_drm_internal__fb_cache_t;

static uint32_t _platform_linux_drm_internal__bo_to_fb(struct gbm_bo* bo)
{
    _platform_linux_drm_internal__fb_cache_t* cached =
        (_platform_linux_drm_internal__fb_cache_t*)gbm_bo_get_user_data(bo);
    if (cached != NULL_POINTER)
    {
        return cached->fb_id;
    }

    uint32_t w      = gbm_bo_get_width(bo);
    uint32_t h      = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t format = gbm_bo_get_format(bo);

    // drmModeAddFB2 takes arrays for multi-plane formats; XRGB8888
    // is single-plane, so handle[0] / stride[0] / offset[0] carry
    // the data and the rest are zero.
    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t strides[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0,      0, 0, 0 };

    uint32_t fb_id = 0;
    int r = drmModeAddFB2(_platform_linux_drm_internal__g.dev.drm_fd, w, h, format, handles, strides, offsets, &fb_id, 0);
    if (r != 0)
    {
        // drmModeAddFB2 is newer; fall back to drmModeAddFB on
        // ancient kernels. The old call doesn't take a format
        // enum -- it assumes 24/32 bpp from depth + bpp args.
        r = drmModeAddFB(_platform_linux_drm_internal__g.dev.drm_fd, w, h, 24, 32, stride, handle, &fb_id);
        if (r != 0)
        {
            log_error("%s %s", "drmModeAddFB failed:", strerror(errno));
            return 0;
        }
    }

    _platform_linux_drm_internal__fb_cache_t* slot =
        (_platform_linux_drm_internal__fb_cache_t*)GUI_MALLOC_T(sizeof(*slot), MM_TYPE__GENERIC);
    if (slot == NULL_POINTER)
    {
        drmModeRmFB(_platform_linux_drm_internal__g.dev.drm_fd, fb_id);
        return 0;
    }
    slot->fb_id  = fb_id;
    slot->drm_fd = _platform_linux_drm_internal__g.dev.drm_fd;
    gbm_bo_set_user_data(bo, slot, _platform_linux_drm_internal__destroy_fb);
    return fb_id;
}

static void _platform_linux_drm_internal__destroy_fb(struct gbm_bo* bo, void* data)
{
    (void)bo;
    _platform_linux_drm_internal__fb_cache_t* slot =
        (_platform_linux_drm_internal__fb_cache_t*)data;
    if (slot == NULL_POINTER)
    {
        return;
    }
    if (slot->fb_id != 0 && slot->drm_fd >= 0)
    {
        drmModeRmFB(slot->drm_fd, slot->fb_id);
    }
    GUI_FREE(slot);
}

static void _platform_linux_drm_internal__page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data)
{
    (void)fd; (void)frame; (void)sec; (void)usec;
    boole* flip_pending = (boole*)data;
    if (flip_pending != NULL_POINTER)
    {
        *flip_pending = FALSE;
    }
}

// ============================================================================
// Input via raw evdev
// ============================================================================
// /dev/input/event* is the kernel's abstract input device tree.
// Each file corresponds to one physical device (keyboard, mouse,
// touchscreen, gamepad, ...). Reading an event yields a struct
// input_event = { time, type, code, value }. Types of interest:
// EV_KEY   key / button press. code is a KEY_*/BTN_* constant.
// EV_REL   relative motion (mouse). code=REL_X/REL_Y delta.
// EV_ABS   absolute position (touchscreen, tablet). code=ABS_X/Y.
// EV_SYN   batch terminator (SYN_REPORT at the end of one event
// group; we treat it as "commit whatever we accumulated").
// Multi-touch (ABS_MT_POSITION_X / _Y, ABS_MT_SLOT) is handled for
// slot 0 only -- the primary finger -- matching the Android port's
// single-pointer model.
// We open every event* file read-only and poll them all. That
// requires read access to /dev/input/*, which on most distros
// means "member of the `input` group" or running as root. If
// opening fails we skip silently; a kiosk without input is a
// legitimate configuration.

static void _platform_linux_drm_internal__open_one_input(const char* path)
{
    if (_platform_linux_drm_internal__g.input.fd_count >= _PLATFORM_LINUX_DRM_INTERNAL__MAX_INPUT_FDS)
    {
        return;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        return;
    }

    // Query ABS_X / ABS_Y ranges if this device reports absolute
    // axes (touchscreen / tablet). We need the min/max to scale
    // raw values into pixel coords.
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > ai.minimum)
    {
        if (_platform_linux_drm_internal__g.input.touch_x_max == 0)
        {
            _platform_linux_drm_internal__g.input.touch_x_min = ai.minimum;
            _platform_linux_drm_internal__g.input.touch_x_max = ai.maximum;
        }
    }
    if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > ai.minimum)
    {
        if (_platform_linux_drm_internal__g.input.touch_y_max == 0)
        {
            _platform_linux_drm_internal__g.input.touch_y_min = ai.minimum;
            _platform_linux_drm_internal__g.input.touch_y_max = ai.maximum;
        }
    }

    _platform_linux_drm_internal__g.input.fds[_platform_linux_drm_internal__g.input.fd_count++] = fd;
    log_info("%s %s %s %d %s", "input: opened", path, "(fd=", fd, ")");
}

static boole _platform_linux_drm_internal__open_input(void)
{
    _platform_linux_drm_internal__g.input.ptr_x = _platform_linux_drm_internal__g.dev.viewport_w / 2;
    _platform_linux_drm_internal__g.input.ptr_y = _platform_linux_drm_internal__g.dev.viewport_h / 2;

    DIR* d = opendir("/dev/input");
    if (d == NULL_POINTER)
    {
        log_warn("%s %s", "input: opendir('/dev/input') failed:", strerror(errno));
        return FALSE;
    }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL_POINTER)
    {
        if (stdlib__strncmp(ent->d_name, "event", 5) != 0)
        {
            continue;
        }
        char path[64];
        stdlib__strcpy_safe(path, (int64)sizeof(path), "/dev/input/");
        stdlib__strcat_safe(path, (int64)sizeof(path), ent->d_name);
        _platform_linux_drm_internal__open_one_input(path);
    }
    closedir(d);
    return _platform_linux_drm_internal__g.input.fd_count > 0;
}

static void _platform_linux_drm_internal__close_input(void)
{
    for (int i = 0; i < _platform_linux_drm_internal__g.input.fd_count; i++)
    {
        close(_platform_linux_drm_internal__g.input.fds[i]);
    }
    _platform_linux_drm_internal__g.input.fd_count = 0;
}

// Translate an evdev KEY_* code to a Win32 virtual-key code (the
// convention scene__on_key expects). Covers the subset we actually
// dispatch; everything else maps to 0 and is dropped. Mirrors the
// Windows WM_KEYDOWN wParam values so the scene's key handling
// logic (BACKSPACE, arrows, Enter, etc.) works identically across
// platforms without a per-platform mapping table inside scene.c.
static int64 _platform_linux_drm_internal__evdev_to_vk(int code)
{
    switch (code)
    {
        case KEY_BACKSPACE: return 0x08;
        case KEY_TAB:       return 0x09;
        case KEY_ENTER:     return 0x0D;
        case KEY_ESC:       return 0x1B;
        case KEY_SPACE:     return 0x20;
        case KEY_LEFT:      return 0x25;
        case KEY_UP:        return 0x26;
        case KEY_RIGHT:     return 0x27;
        case KEY_DOWN:      return 0x28;
        case KEY_DELETE:    return 0x2E;
        case KEY_HOME:      return 0x24;
        case KEY_END:       return 0x23;
        case KEY_PAGEUP:    return 0x21;
        case KEY_PAGEDOWN:  return 0x22;
        default:            return 0;
    }
}

// Optional xkbcommon integration. Defining GUI_USE_XKBCOMMON in the
// build (and linking -lxkbcommon) swaps the hand-rolled US-QWERTY
// path below for a real xkbcommon-backed translator that respects
// the system keymap, dead keys, and compose sequences. Off by
// default so the DRM build doesn't grow a runtime dependency for
// the kiosk-numeric-input case where ASCII suffices.
#if defined(GUI_USE_XKBCOMMON)
  #include <xkbcommon/xkbcommon.h>
  static struct xkb_context* _platform_linux_drm_internal__xkb_ctx   = NULL_POINTER;
  static struct xkb_keymap*  _platform_linux_drm_internal__xkb_keymap = NULL_POINTER;
  static struct xkb_state*   _platform_linux_drm_internal__xkb_state  = NULL_POINTER;

  // Lazy init from the host's locale via XKB_DEFAULT_LAYOUT or
  // /etc/default/keyboard. Returns FALSE on failure; caller falls
  // back to the US-QWERTY path so we never lose input entirely.
  static boole _platform_linux_drm_internal__xkb_ensure(void)
  {
      if (_platform_linux_drm_internal__xkb_state != NULL_POINTER)
      {
          return TRUE;
      }
      _platform_linux_drm_internal__xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (_platform_linux_drm_internal__xkb_ctx == NULL_POINTER)
      {
          return FALSE;
      }
      _platform_linux_drm_internal__xkb_keymap = xkb_keymap_new_from_names( _platform_linux_drm_internal__xkb_ctx, NULL_POINTER, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (_platform_linux_drm_internal__xkb_keymap == NULL_POINTER)
      {
          xkb_context_unref(_platform_linux_drm_internal__xkb_ctx);
          _platform_linux_drm_internal__xkb_ctx = NULL_POINTER;
          return FALSE;
      }
      _platform_linux_drm_internal__xkb_state = xkb_state_new(_platform_linux_drm_internal__xkb_keymap);
      return _platform_linux_drm_internal__xkb_state != NULL_POINTER;
  }
#endif

// Translate a printable keycode + shift state to an ASCII codepoint
// for scene__on_char. This is a MINIMAL US-QWERTY mapping -- enough
// for a demo but nowhere near a real keymap engine (no xkbcommon,
// no locale handling, no dead keys). A proper build would link
// libxkbcommon and feed it raw keycodes; for the DRM kiosk use
// case the expected text input is typically just numeric or
// barely-ASCII, which this covers.
static uint _platform_linux_drm_internal__evdev_to_char(int code, boole shift)
{
#if defined(GUI_USE_XKBCOMMON)
    // xkbcommon path. evdev keycodes are offset by 8 from XKB
    // keycodes (historical X11 baseline). For full correctness the
    // caller would also feed every EV_KEY event into
    // xkb_state_update_key BEFORE this lookup so the state machine
    // tracks modifier presses + dead-key chords; the skeleton here
    // does single-key translation only and falls back to the
    // QWERTY table below for any key xkb can't resolve. Sufficient
    // for an embedded kiosk that just wants a non-US locale layout.
    if (_platform_linux_drm_internal__xkb_ensure())
    {
        xkb_keysym_t sym = xkb_state_key_get_one_sym( _platform_linux_drm_internal__xkb_state, (xkb_keycode_t)(code + 8));
        if (sym != XKB_KEY_NoSymbol)
        {
            uint32_t cp = xkb_keysym_to_utf32(sym);
            if (cp != 0)
            {
                (void)shift;
                return (uint)cp;
            }
        }
    }
#endif
    {
    // Linux input keycodes do NOT follow alphabetical order --
    // they follow the physical keyboard-row ordering (KEY_Q=16..
    // KEY_P=25 for the top row, KEY_A=30..KEY_L=38 for the
    // middle row, KEY_Z=44..KEY_M=50 for the bottom row). So
    // we lay out per-row lookup strings in keycode order rather
    // than trying arithmetic on the A..Z range.
    if (code >= KEY_Q && code <= KEY_P)
    {
        // Input-event ordering is qwertyuiop (top row), so we
        // build a lookup table instead of arithmetic. Same for
        // middle and bottom row.
        static const char qwerty_top_lc[]   = "qwertyuiop";
        static const char qwerty_top_uc[]   = "QWERTYUIOP";
        int idx = code - KEY_Q;
        return (uint)(shift ? qwerty_top_uc[idx] : qwerty_top_lc[idx]);
    }
    if (code >= KEY_A && code <= KEY_L)
    {
        static const char qwerty_mid_lc[]   = "asdfghjkl";
        static const char qwerty_mid_uc[]   = "ASDFGHJKL";
        int idx = code - KEY_A;
        return (uint)(shift ? qwerty_mid_uc[idx] : qwerty_mid_lc[idx]);
    }
    if (code >= KEY_Z && code <= KEY_M)
    {
        static const char qwerty_bot_lc[]   = "zxcvbnm";
        static const char qwerty_bot_uc[]   = "ZXCVBNM";
        int idx = code - KEY_Z;
        return (uint)(shift ? qwerty_bot_uc[idx] : qwerty_bot_lc[idx]);
    }
    // Digits + basic punctuation.
    switch (code)
    {
        case KEY_1:      return shift ? (uint)'!' : (uint)'1';
        case KEY_2:      return shift ? (uint)'@' : (uint)'2';
        case KEY_3:      return shift ? (uint)'#' : (uint)'3';
        case KEY_4:      return shift ? (uint)'$' : (uint)'4';
        case KEY_5:      return shift ? (uint)'%' : (uint)'5';
        case KEY_6:      return shift ? (uint)'^' : (uint)'6';
        case KEY_7:      return shift ? (uint)'&' : (uint)'7';
        case KEY_8:      return shift ? (uint)'*' : (uint)'8';
        case KEY_9:      return shift ? (uint)'(' : (uint)'9';
        case KEY_0:      return shift ? (uint)')' : (uint)'0';
        case KEY_SPACE:  return (uint)' ';
        case KEY_MINUS:  return shift ? (uint)'_' : (uint)'-';
        case KEY_EQUAL:  return shift ? (uint)'+' : (uint)'=';
        case KEY_COMMA:  return shift ? (uint)'<' : (uint)',';
        case KEY_DOT:    return shift ? (uint)'>' : (uint)'.';
        case KEY_SLASH:  return shift ? (uint)'?' : (uint)'/';
        case KEY_SEMICOLON: return shift ? (uint)':' : (uint)';';
        case KEY_APOSTROPHE: return shift ? (uint)'"' : (uint)'\'';
        default: return 0;
    }
    } // close the GUI_USE_XKBCOMMON-introduced inner block
}

static boole _platform_linux_drm_internal__shift_down = FALSE;

static void _platform_linux_drm_internal__handle_event(const struct input_event* ev)
{
    switch (ev->type)
    {
        case EV_KEY:
        {
            // Button codes (BTN_LEFT / _RIGHT / _MIDDLE) are mouse
            // buttons reported through EV_KEY. Separate them from
            // keyboard keys; a BTN_LEFT press should route to
            // scene__on_mouse_button, not scene__on_key.
            if (ev->code == BTN_LEFT || ev->code == BTN_TOUCH)
            {
                scene__on_mouse_button(0, ev->value != 0, _platform_linux_drm_internal__g.input.ptr_x, _platform_linux_drm_internal__g.input.ptr_y);
                return;
            }
            if (ev->code == BTN_RIGHT)
            {
                scene__on_mouse_button(1, ev->value != 0, _platform_linux_drm_internal__g.input.ptr_x, _platform_linux_drm_internal__g.input.ptr_y);
                return;
            }
            if (ev->code == BTN_MIDDLE)
            {
                scene__on_mouse_button(2, ev->value != 0, _platform_linux_drm_internal__g.input.ptr_x, _platform_linux_drm_internal__g.input.ptr_y);
                return;
            }

            // Track shift state for the char translator.
            if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT)
            {
                _platform_linux_drm_internal__shift_down = (ev->value != 0);
                return;
            }

            // A keyboard key. Value: 0 = release, 1 = press, 2 = autorepeat.
            // We treat autorepeat the same as press for char delivery
            // (lets held BACKSPACE work) but only fire down/up edges
            // through scene__on_key.
            boole down = (ev->value != 0);
            int64 vk   = _platform_linux_drm_internal__evdev_to_vk(ev->code);
            if (vk != 0 && ev->value != 2)
            {
                scene__on_key(vk, down);
            }
            if (down)
            {
                uint ch = _platform_linux_drm_internal__evdev_to_char(ev->code, _platform_linux_drm_internal__shift_down);
                if (ch != 0)
                {
                    scene__on_char(ch);
                }
            }
            return;
        }

        case EV_REL:
        {
            // Mouse relative motion. Accumulate into absolute ptr_x/y,
            // clamped to viewport so the cursor can't escape the
            // screen. Wheel scrolling also arrives here (REL_WHEEL /
            // REL_HWHEEL) with value = ticks (+1 or -1 each).
            if (ev->code == REL_X)
            {
                _platform_linux_drm_internal__g.input.ptr_x += ev->value;
            }
            else if (ev->code == REL_Y)
            {
                _platform_linux_drm_internal__g.input.ptr_y += ev->value;
            }
            else if (ev->code == REL_WHEEL)
            {
                scene__on_mouse_wheel(_platform_linux_drm_internal__g.input.ptr_x, _platform_linux_drm_internal__g.input.ptr_y, (float)ev->value);
            }
            // Clamp on every X/Y event. Don't fire scene__on_mouse_move
            // here -- wait for SYN_REPORT so a combined X+Y motion
            // produces one move callback instead of two.
            if (_platform_linux_drm_internal__g.input.ptr_x < 0)
            {
                _platform_linux_drm_internal__g.input.ptr_x = 0;
            }
            if (_platform_linux_drm_internal__g.input.ptr_y < 0)
            {
                _platform_linux_drm_internal__g.input.ptr_y = 0;
            }
            if (_platform_linux_drm_internal__g.input.ptr_x >= (int64)_platform_linux_drm_internal__g.dev.viewport_w)
            {
                _platform_linux_drm_internal__g.input.ptr_x = (int64)_platform_linux_drm_internal__g.dev.viewport_w - 1;
            }
            if (_platform_linux_drm_internal__g.input.ptr_y >= (int64)_platform_linux_drm_internal__g.dev.viewport_h)
            {
                _platform_linux_drm_internal__g.input.ptr_y = (int64)_platform_linux_drm_internal__g.dev.viewport_h - 1;
            }
            return;
        }

        case EV_ABS:
        {
            // Touchscreen / tablet absolute coordinates. Scale the
            // raw value from the device's reported min/max range
            // into viewport pixels.
            if (ev->code == ABS_X || ev->code == ABS_MT_POSITION_X)
            {
                _platform_linux_drm_internal__g.input.touch_x_raw = ev->value;
                if (_platform_linux_drm_internal__g.input.touch_x_max > _platform_linux_drm_internal__g.input.touch_x_min)
                {
                    float t = (float)(ev->value - _platform_linux_drm_internal__g.input.touch_x_min) /
                              (float)(_platform_linux_drm_internal__g.input.touch_x_max - _platform_linux_drm_internal__g.input.touch_x_min);
                    _platform_linux_drm_internal__g.input.ptr_x = (int64)(t * (float)_platform_linux_drm_internal__g.dev.viewport_w);
                }
            }
            else if (ev->code == ABS_Y || ev->code == ABS_MT_POSITION_Y)
            {
                _platform_linux_drm_internal__g.input.touch_y_raw = ev->value;
                if (_platform_linux_drm_internal__g.input.touch_y_max > _platform_linux_drm_internal__g.input.touch_y_min)
                {
                    float t = (float)(ev->value - _platform_linux_drm_internal__g.input.touch_y_min) /
                              (float)(_platform_linux_drm_internal__g.input.touch_y_max - _platform_linux_drm_internal__g.input.touch_y_min);
                    _platform_linux_drm_internal__g.input.ptr_y = (int64)(t * (float)_platform_linux_drm_internal__g.dev.viewport_h);
                }
            }
            return;
        }

        case EV_SYN:
        {
            // End of an event group. Emit the accumulated pointer
            // position as one move callback. SYN_DROPPED means the
            // kernel's input buffer overflowed; we ignore (best
            // effort -- nothing actionable we can do).
            if (ev->code == SYN_REPORT)
            {
                scene__on_mouse_move(_platform_linux_drm_internal__g.input.ptr_x, _platform_linux_drm_internal__g.input.ptr_y);
            }
            return;
        }

        default:
            return;
    }
}

static void _platform_linux_drm_internal__poll_input(void)
{
    // Non-blocking read on every fd. poll() with timeout=0 tells us
    // which fds have data; each one gets read in a loop until EAGAIN.
    // Blocks for 0 ms so the frame isn't held up if input is idle.
    struct pollfd pfds[_PLATFORM_LINUX_DRM_INTERNAL__MAX_INPUT_FDS];
    int n = _platform_linux_drm_internal__g.input.fd_count;
    if (n == 0)
    {
        return;
    }
    for (int i = 0; i < n; i++)
    {
        pfds[i].fd      = _platform_linux_drm_internal__g.input.fds[i];
        pfds[i].events  = POLLIN;
        pfds[i].revents = 0;
    }
    int pr = poll(pfds, n, 0);
    if (pr <= 0)
    {
        return;
    }

    for (int i = 0; i < n; i++)
    {
        if ((pfds[i].revents & POLLIN) == 0)
        {
            continue;
        }
        for (;;)
        {
            struct input_event ev;
            ssize_t r = read(pfds[i].fd, &ev, sizeof(ev));
            if (r != (ssize_t)sizeof(ev))
            {
                // EAGAIN / partial read / closed fd. Any of these
                // means "no more events right now"; break the
                // per-fd read loop and move on to the next fd.
                break;
            }
            _platform_linux_drm_internal__handle_event(&ev);
        }
    }
}

// ============================================================================
// UI scale (DPI-aware)
// ============================================================================
// _platform_linux_drm_internal__scale_hint was computed in try_card
// from the connector's mmWidth / mode hdisplay. We clamp it to a
// sensible range here, same as the Android picker does.

static float _platform_linux_drm_internal__pick_ui_scale(void)
{
    float s = _platform_linux_drm_internal__scale_hint;
    // s != s catches NaN
    if (s <= 0.0f || s != s)
    {
        s = 1.0f;
    }
    if (s < 1.0f)
    {
        s = 1.0f;
    }
    if (s > 4.0f)
    {
        s = 4.0f;
    }
    log_info("%s %.2f %s", "ui_scale:", (double)s, "x (from connector mmWidth / hdisplay)");
    return s;
}

// ============================================================================
// host symbol resolver (dlsym fallback)
// ============================================================================

static gui_handler_fn _platform_linux_drm_internal__resolve_host_symbol(char* name)
{
    if (name == NULL_POINTER || name[0] == 0)
    {
        return NULL_POINTER;
    }
    // RTLD_DEFAULT: search the main program + every loaded .so.
    // UI_HANDLER on POSIX expands to __attribute__((visibility
    // ("default"), used)) so these symbols survive both
    // -fvisibility=hidden and --gc-sections.
    void* sym = dlsym(RTLD_DEFAULT, name);
    return (gui_handler_fn)sym;
}

// ============================================================================
// entry-point trampoline
// ============================================================================
// platform.h rewrites the host's `int main(...)` to `int app_main(...)`
// via `#define main app_main` (suppressed here by _PLATFORM_INTERNAL).
// Provide the real `main` the linker expects, forwarding to the host's
// renamed entry. Same shape as platforms/windows/main_entry_win32.c,
// but inlined into the platform .c since the Linux DRM build is a
// single-binary link (no library / host split).

#include "_main_trampoline.h"
GUI_DEFINE_MAIN_TRAMPOLINE()
