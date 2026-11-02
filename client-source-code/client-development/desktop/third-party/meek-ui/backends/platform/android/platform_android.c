// ============================================================================
// platform_android.c - Android window + EGL context + event pump.
// ============================================================================
// Android equivalent of platform_win32.c. uses:
// - android_native_app_glue   main loop / lifecycle event dispatch
// (from the NDK; provided by rawdrawandroid's
// android_native_app_glue.c sitting next to
// this .c in the build)
// - EGL                       GL context creation + SwapBuffers
// - GLES 3.0                  rendering (through gles3_renderer.c)
// - ALooper                   blocking / non-blocking event polling
// LIFECYCLE:
// NativeActivity ----> ANativeActivity_onCreate ----> android_native_app_glue
// (zygote fork)                                       spawns a worker thread
// that calls android_main()
// android_main() (in the host app -- see demo-android/main.c) calls:
// platform_android__init(app, &cfg)   -- wires glue, registers widgets
// loop { platform_android__tick() }   -- polls events + renders one frame
// platform_android__shutdown()
// events come in as APP_CMD_* messages through app->onAppCmd (set by
// platform_android__init). the interesting ones:
// APP_CMD_INIT_WINDOW   the OS handed us an ANativeWindow. create an
// EGLSurface against it, then renderer__init.
// APP_CMD_TERM_WINDOW   surface is going away. destroy renderer + EGL
// surface (context survives).
// APP_CMD_WINDOW_RESIZED  same handling as INIT_WINDOW? the simple
// choice: just update viewport_w/viewport_h at
// the start of each frame.
// APP_CMD_DESTROY       app is being killed. platform_android__tick
// returns FALSE so the host main can exit.
// INPUT (not yet wired in this MVP -- see the touch routing TODO below).

#include <android_native_app_glue.h>
#include <android/asset_manager.h> // AAssetManager_openDir / AAssetDir_* for the font-asset scan.
#include <android/configuration.h> // AConfiguration_getDensity for the DPI-based UI scale.
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <stdio.h> // snprintf for the asset-font scan path builder.
#include <stdlib.h>
#include <string.h>
#include <time.h> // clock_gettime(CLOCK_MONOTONIC) for animator frame timing.
#include <dlfcn.h> // dlsym(RTLD_DEFAULT, name) for host-symbol auto-resolve.

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
#include "third_party/log.h"

// Pull the unified platform.h API declarations. _PLATFORM_INTERNAL
// suppresses the `#define main app_main` rename since this file
// provides the actual android_main entry (and forwards to app_main).
#define _PLATFORM_INTERNAL
#include "platform.h"
#include "clib/stdlib.h"
#undef _PLATFORM_INTERNAL

// we expose android_main-facing setup via a distinct pair of names so the
// Win32 init/tick/shutdown keep their signatures. the android host's
// android_main calls these directly.

typedef struct _platform_android_internal__state_t
{
    // from android_main.
    struct android_app* app;   // 0x0

    EGLDisplay display;        // 0x8
    EGLSurface surface;        // 0x10
    EGLContext context;        // 0x18
    EGLConfig  config;         // 0x20

    int64 viewport_w;          // 0x28
    int64 viewport_h;          // 0x30

    boole renderer_up;         // 0x38
    boole should_close;        // 0x39

    gui_color_t clear_color;     // 0x3C
} _platform_android_internal__state_t;

static _platform_android_internal__state_t _platform_android_internal__g;

// ============================================================================
// forward decls
// ============================================================================

static void    _platform_android_internal__on_app_cmd(struct android_app* app, int32_t cmd);
static int32_t _platform_android_internal__on_input_event(struct android_app* app, AInputEvent* event);
static boole   _platform_android_internal__init_egl(ANativeWindow* window);
static void    _platform_android_internal__term_egl_surface(void);
static void    _platform_android_internal__term_egl_all(void);
static void    _platform_android_internal__query_window_size(void);
static void    _platform_android_internal__scan_asset_fonts(AAssetManager* mgr, const char* dir);
static float   _platform_android_internal__pick_ui_scale(struct android_app* app);

// Host symbol resolver. Maps an on_click / on_change name from a .ui
// file back to a function pointer by looking it up across every
// loaded shared object. RTLD_DEFAULT starts the search at the main
// program (our libguidemo.so -- which has both the library code
// AND the host main.c compiled in), so UI_HANDLER-marked handlers
// in demo-android/main.c resolve cleanly. Silent NULL on miss; the
// caller (scene's dispatcher) logs its own "handler not found"
// warning in that case.
static gui_handler_fn _platform_android_internal__resolve_host_symbol(char* name)
{
    if (name == NULL_POINTER || name[0] == 0)
    {
        return NULL_POINTER;
    }
    void* sym = dlsym(RTLD_DEFAULT, name);
    return (gui_handler_fn)sym;
}

// ============================================================================
// public API -- mirrors platform_win32__* shape. android_main (host app)
// drives these in the same init/tick/shutdown order.
// ============================================================================

/**
 * Wire up android_native_app_glue to our event handler and register
 * built-in widgets. Must be called exactly once from android_main(),
 * before the tick loop starts.
 *
 * @function platform_android__init
 * @param void* android_app_ptr -> struct android_app* from android_main.
 * @param const gui_app_config_t* cfg -> same config struct as the Win32 path.
 * @return {boole} TRUE on success.
 */
static boole platform_android__init(void* android_app_ptr, const gui_app_config_t* cfg)
{
    // tracker first -- see the matching call in platform_win32__init.
    memory_manager__init();

    stdlib__memset(&_platform_android_internal__g, 0, sizeof(_platform_android_internal__g));

    struct android_app* app = (struct android_app*)android_app_ptr;
    if (app == NULL_POINTER || cfg == NULL_POINTER)
    {
        log_error("%s", "platform_android__init: app or cfg is NULL");
        return FALSE;
    }

    _platform_android_internal__g.app          = app;
    _platform_android_internal__g.clear_color  = cfg->clear_color;
    _platform_android_internal__g.viewport_w   = cfg->width  > 0 ? cfg->width  : 1;
    _platform_android_internal__g.viewport_h   = cfg->height > 0 ? cfg->height : 1;
    _platform_android_internal__g.display      = EGL_NO_DISPLAY;
    _platform_android_internal__g.context      = EGL_NO_CONTEXT;
    _platform_android_internal__g.surface      = EGL_NO_SURFACE;

    // install our command handler on the glue. it fires for lifecycle
    // events (INIT_WINDOW, TERM_WINDOW, SAVE_STATE, ...). the glue also
    // provides onInputEvent for touch/keys; we ignore it this pass.
    app->userData     = &_platform_android_internal__g;
    app->onAppCmd     = _platform_android_internal__on_app_cmd;
    app->onInputEvent = _platform_android_internal__on_input_event;

    // Install the AAssetManager so fs__read_entire_file can resolve
    // paths against APK-packaged assets. Parser calls like
    // parser_xml__load_ui("main.ui") and parser_style__load_styles(
    // "main.style") now work identically on Android and desktop --
    // on Android the path is looked up inside the APK's assets/
    // directory; elsewhere it falls through to POSIX open().
    // Must happen BEFORE anything that reads files (widget registry
    // bootstrap doesn't, but font__init might once AAssetManager-based
    // font loading lands).
    fs__set_asset_manager(app->activity->assetManager);

    // Install the sideload dir so adb-pushed .ui / .style files
    // override the APK-packaged versions on the next hot_reload poll.
    // externalDataPath is /storage/emulated/0/Android/data/<pkg>/files/
    // -- readable by the app without permissions and writable by
    // `adb push` on every modern Android version. NULL on ancient
    // devices without external storage, in which case the sideload
    // check is skipped and hot_reload effectively no-ops on Android.
    fs__set_sideload_dir(app->activity->externalDataPath);

    // bootstrap widgets + font system early so the first frame has a
    // complete registry. font__init's auto-discovery is a no-op on
    // Android for now (no GUI_FONTS_SOURCE_DIR define); host app can
    // ship TTFs via APK assets and call font__register_from_memory
    // once the AAssetManager pointer is extracted from app->activity.
    widget_registry__bootstrap_builtins();
    font__init();

    // Install a dlsym-based host symbol resolver so on_click= / on_change=
    // handlers declared in .ui files auto-resolve without the host
    // calling scene__register_handler for each one. Matches the
    // GetProcAddress fallback the Windows platform layer installs.
    // RTLD_DEFAULT searches every already-loaded shared object for
    // the given symbol, starting with the main program. UI_HANDLER-
    // marked functions in demo-android/main.c compile with
    // visibility("default"), so dlsym finds them even though the
    // rest of the app is -fvisibility=hidden. Without this resolver
    // every handler name lookup on Android returned NULL and every
    // on_click / on_change dispatch silently did nothing -- which
    // is why theme toggle, the show/hide checkboxes, etc. appeared
    // dead on device.
    scene__set_symbol_resolver(_platform_android_internal__resolve_host_symbol);

    // Enumerate TTFs under assets/fonts/ and register each as a font
    // family using its filename stem. AAssetManager_openDir walks
    // the APK's asset index; each name returned is the leaf filename
    // (no directory prefix), so we reconstruct the full asset path
    // "fonts/<name>" before handing it to font__register_from_file.
    // That function's Android-path fallback (fs__map_file -> POSIX
    // open fails -> fs__read_entire_file heap-reads through
    // AAssetManager) is what actually pulls the bytes out.
    // Build scripts stage gui/src/fonts/*.ttf into the APK at build
    // time (see demo-android/build.ps1 / build.sh), mirroring the
    // Windows side's GUI_FONTS_SOURCE_DIR auto-scan. host apps with
    // different TTFs would add to that stage step and/or call
    // font__register_from_memory from their android_main.
    _platform_android_internal__scan_asset_fonts(app->activity->assetManager, "fonts");

    // Auto-pick the UI scale factor from the display's DPI so the
    // same .ui / .style layout looks physically similar across a
    // mdpi phone, xxhdpi tablet, and xxxhdpi high-end flagship. the
    // computed scale is passed straight to scene__set_scale which
    // then multiplies every style-driven size (pad, gap, size,
    // radius, font_size) during layout + rasterization.
    // WITHOUT this, a 14 px body font on a 480-dpi tablet renders at
    // literally 14 pixels tall -- readable in isolation but visually
    // tiny relative to the physical screen size. density=480 maps
    // this to scale=3.0, producing 42-px tall body text that reads
    // at normal subjective size.
    float ui_scale = _platform_android_internal__pick_ui_scale(app);
    if (ui_scale > 0.0f)
    {
        scene__set_scale(ui_scale);
    }

    return TRUE;
}

/**
 * One iteration of the main loop. Processes pending OS events, brings
 * up the renderer once an ANativeWindow arrives, renders one frame,
 * and presents via eglSwapBuffers. Returns FALSE once the OS has told
 * the activity to destroy itself.
 *
 * @function platform_android__tick
 * @return {boole} FALSE when the app should exit.
 */
static boole platform_android__tick(void)
{
    struct android_app* app = _platform_android_internal__g.app;

    // event pump. timeout of 0 when the renderer is up (keep rendering),
    // -1 (block) when no window exists yet (nothing else to do). matches
    // the canonical android_native_app_glue main-loop pattern.
    // NDK r30+ removed ALooper_pollAll -- it ignored one-shot wake signals
    // in a way the Android team decided was unsafe. ALooper_pollOnce has
    // the same prototype but returns after one event is available (or the
    // timeout elapses). we loop it ourselves with timeout=0 on subsequent
    // iterations to drain every pending event before rendering, which is
    // what pollAll used to do internally.
    int timeout_ms = _platform_android_internal__g.renderer_up ? 0 : -1;
    int events;
    struct android_poll_source* source;

    while (ALooper_pollOnce(timeout_ms, NULL_POINTER, &events, (void**)&source) >= 0)
    {
        if (source != NULL_POINTER)
        {
            source->process(app, source);
        }
        if (app->destroyRequested)
        {
            _platform_android_internal__g.should_close = TRUE;
            return FALSE;
        }
        // subsequent polls in this drain loop must not block, even if
        // we started with timeout=-1 (no window yet). renderer_up flips
        // true inside source->process when APP_CMD_INIT_WINDOW fires.
        timeout_ms = 0;
    }

    if (_platform_android_internal__g.should_close)
    {
        return FALSE;
    }

    // nothing to draw until the OS has handed us a window.
    if (_platform_android_internal__g.renderer_up == FALSE)
    {
        return TRUE;
    }

    // pick up any window-size changes the OS might have signalled
    // (rotation, IME panel open/close). inexpensive to query per frame.
    _platform_android_internal__query_window_size();

    // capture frame timestamp for the animator (mirrors what
    // platform_win32 does with QueryPerformanceCounter). clock_gettime
    // with CLOCK_MONOTONIC is the standard POSIX/Linux/Android monotonic
    // clock; converting to ms keeps the API consistent across platforms.
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64 now_ms = (int64)ts.tv_sec * 1000 + (int64)(ts.tv_nsec / 1000000);
        scene__begin_frame_time(now_ms);
    }

    scene__resolve_styles();
    animator__tick();
    scene__layout(_platform_android_internal__g.viewport_w, _platform_android_internal__g.viewport_h);

    renderer__begin_frame(_platform_android_internal__g.viewport_w, _platform_android_internal__g.viewport_h, _platform_android_internal__g.clear_color);

    scene__emit_draws();

    // renderer__end_frame issues the gpu draws; eglSwapBuffers actually
    // presents. we keep present in the platform (not the renderer) so
    // the renderer stays API-neutral -- same split the Win32 side
    // will eventually adopt.
    renderer__end_frame();
    eglSwapBuffers(_platform_android_internal__g.display, _platform_android_internal__g.surface);
    return TRUE;
}

/**
 * Tear down in reverse of init. Called from android_main after the tick
 * loop returns FALSE.
 *
 * @function platform_android__shutdown
 * @return {void}
 */
static void platform_android__shutdown(void)
{
    // Image cache + font subsystem release renderer-owned textures,
    // so they have to run while the renderer is still alive. The
    // renderer_up gate guards us if init bailed before the renderer
    // was ever created (in which case the cache should be empty).
    if (_platform_android_internal__g.renderer_up)
    {
        widget_image__cache_shutdown();
    }
    font__shutdown();
    if (_platform_android_internal__g.renderer_up)
    {
        renderer__shutdown();
        _platform_android_internal__g.renderer_up = FALSE;
    }
    _platform_android_internal__term_egl_all();

    // tracker last -- see the matching call in platform_win32__shutdown.
    memory_manager__shutdown();
}

// ============================================================================
// event handler
// ============================================================================

static void _platform_android_internal__on_app_cmd(struct android_app* app, int32_t cmd)
{
    (void)app;
    switch (cmd)
    {
        case APP_CMD_INIT_WINDOW:
        {
            // the OS gave us an ANativeWindow. build an EGL surface +
            // context, then spin up the renderer's GL pipeline.
            log_info("%s %p %s", "APP_CMD_INIT_WINDOW (window=", app->window, ")");
            if (app->window == NULL_POINTER)
            {
                log_warn("%s", "INIT_WINDOW with NULL window; ignoring");
                break;
            }
            // defensive tear-down. If a previous INIT_WINDOW succeeded
            // and the OS fires another one without an intervening
            // TERM_WINDOW (possible on some Samsung ROMs when the
            // screen wakes during activity start), make sure we don't
            // stack contexts.
            if (_platform_android_internal__g.renderer_up)
            {
                font__drop_gpu_cache();
                renderer__shutdown();
                _platform_android_internal__g.renderer_up = FALSE;
            }
            if (_platform_android_internal__g.display != EGL_NO_DISPLAY)
            {
                _platform_android_internal__term_egl_all();
            }

            if (_platform_android_internal__init_egl(app->window) == FALSE)
            {
                log_error("%s", "EGL init failed");
                break;
            }
            _platform_android_internal__query_window_size();
            if (renderer__init(NULL_POINTER) == FALSE)
            {
                log_error("%s", "renderer__init failed");
                _platform_android_internal__term_egl_all();
                break;
            }
            _platform_android_internal__g.renderer_up = TRUE;
            log_info("%s %lld %s %lld %s", "window up, renderer live (", (long long)_platform_android_internal__g.viewport_w, "x", (long long)_platform_android_internal__g.viewport_h, ")");
            break;
        }

        case APP_CMD_TERM_WINDOW:
        {
            // OS is taking back the ANativeWindow. tear down EVERYTHING
            // (surface + context + display refs + renderer's GL objects)
            // so the next INIT_WINDOW starts from a clean slate. Earlier
            // this only destroyed the surface and tried to reuse the
            // context, but the Samsung/ANGLE stack doesn't always tolerate
            // that, and when the system kicks us out mid-init (e.g.
            // launching while the screen is off, which causes a rapid
            // INIT/TERM pair) the leftover state produces a silent
            // render-nothing on resume.
            // font__drop_gpu_cache first, BEFORE the renderer goes away:
            // the font cache is holding atlas_tex handles that live in
            // the about-to-die GL context. Dropping them here invalidates
            // the cache so the next font__at rebuilds against the NEW
            // context. Without this, the app rebinds stale texture IDs
            // after resume and all glyphs rasterize from a dead atlas
            // -- visible as "fonts disappeared when backgrounded".
            if (_platform_android_internal__g.renderer_up)
            {
                font__drop_gpu_cache();
                renderer__shutdown();
                _platform_android_internal__g.renderer_up = FALSE;
            }
            _platform_android_internal__term_egl_all();
            log_info("%s", "platform_android: window terminated");
            break;
        }

        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        {
            _platform_android_internal__query_window_size();
            scene__on_resize(_platform_android_internal__g.viewport_w, _platform_android_internal__g.viewport_h);
            // config changed -- re-pick the UI scale. density can flip
            // on a fold event (one-screen -> two-screen), on a multi-
            // window launch where the system reports a different bucket
            // to a smaller window, or when an external display mirrors
            // at a different DPI. recomputing on every config_changed
            // keeps the layout consistent without the host having to
            // listen for anything.
            float ui_scale = _platform_android_internal__pick_ui_scale(app);
            if (ui_scale > 0.0f)
            {
                scene__set_scale(ui_scale);
            }
            break;
        }

        case APP_CMD_DESTROY:
        {
            _platform_android_internal__g.should_close = TRUE;
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// EGL setup / teardown
// ============================================================================

static boole _platform_android_internal__init_egl(ANativeWindow* window)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY)
    {
        log_error("%s", "eglGetDisplay failed");
        return FALSE;
    }
    if (eglInitialize(dpy, NULL_POINTER, NULL_POINTER) == FALSE)
    {
        log_error("%s", "eglInitialize failed");
        return FALSE;
    }

    // ask for an ES 3.0 capable config with RGBA8 + no depth. we draw
    // 2D only.
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_BLUE_SIZE,  8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE,   8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };

    EGLConfig cfg;
    EGLint    num_cfg = 0;
    if (eglChooseConfig(dpy, attribs, &cfg, 1, &num_cfg) == FALSE || num_cfg < 1)
    {
        log_error("%s", "eglChooseConfig: no matching config");
        eglTerminate(dpy);
        return FALSE;
    }

    // ANativeWindow_setBuffersGeometry + native visual id keeps things
    // happy across devices (some drivers reject the surface otherwise).
    EGLint native_visual_id = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &native_visual_id);
    ANativeWindow_setBuffersGeometry(window, 0, 0, native_visual_id);

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, window, NULL_POINTER);
    if (surf == EGL_NO_SURFACE)
    {
        log_error("%s", "eglCreateWindowSurface failed");
        eglTerminate(dpy);
        return FALSE;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT)
    {
        log_error("%s", "eglCreateContext failed");
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return FALSE;
    }

    if (eglMakeCurrent(dpy, surf, surf, ctx) == FALSE)
    {
        log_error("%s", "eglMakeCurrent failed");
        eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return FALSE;
    }

    _platform_android_internal__g.display = dpy;
    _platform_android_internal__g.config  = cfg;
    _platform_android_internal__g.surface = surf;
    _platform_android_internal__g.context = ctx;
    return TRUE;
}

static void _platform_android_internal__term_egl_surface(void)
{
    if (_platform_android_internal__g.display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(_platform_android_internal__g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (_platform_android_internal__g.surface != EGL_NO_SURFACE)
    {
        eglDestroySurface(_platform_android_internal__g.display, _platform_android_internal__g.surface);
        _platform_android_internal__g.surface = EGL_NO_SURFACE;
    }
}

static void _platform_android_internal__term_egl_all(void)
{
    _platform_android_internal__term_egl_surface();
    if (_platform_android_internal__g.context != EGL_NO_CONTEXT)
    {
        eglDestroyContext(_platform_android_internal__g.display, _platform_android_internal__g.context);
        _platform_android_internal__g.context = EGL_NO_CONTEXT;
    }
    if (_platform_android_internal__g.display != EGL_NO_DISPLAY)
    {
        eglTerminate(_platform_android_internal__g.display);
        _platform_android_internal__g.display = EGL_NO_DISPLAY;
    }
}

// ============================================================================
// input event handler
// ============================================================================
// android_native_app_glue dequeues AInputEvents from the input queue and
// hands each one to this callback before finishing it. We translate touch
// events into the scene's existing mouse-event API (scene__on_mouse_move
// and scene__on_mouse_button), reusing the Windows-side input state
// machine verbatim -- "touch" and "left mouse button" have identical
// semantics for our purposes (press + drag + release over a widget).
// Return value: 1 means "handled, don't forward to system"; 0 means "let
// the OS do default processing" (e.g. back-button navigation).
// Multi-touch: for the first pass we only track pointer index 0 (primary
// finger). Secondary fingers (ACTION_POINTER_DOWN / _UP) are ignored.
// The hover-tracking model scene uses wouldn't gracefully handle two
// simultaneous pressed points without a per-pointer state machine anyway.

// Touch-drag-to-scroll gesture detector. A finger on a scrollable
// container needs to PAN the content, not press-and-drag the widget
// underneath. We can't know at DOWN time whether the user intends a
// tap or a swipe, so we run a small state machine:
// IDLE -> DOWN received: press-start. scene__on_mouse_button(down)
// fires immediately so :pressed styling and slider drags
// still feel instant. Remember start position.
// PRESSED -> MOVE with |dy| > slop: the user is swiping, not
// tapping. Cancel the pressed widget with an on_mouse_button
// (up) at the current position, then stop forwarding move
// as scene__on_mouse_move and start feeding the delta into
// scene__on_mouse_wheel, which walks up to the nearest
// scrollable ancestor and bumps its scroll_y_target.
// Staying below slop keeps forwarding moves normally so
// slider / scrollbar-thumb drags still work 1:1.
// SCROLLING -> MOVE: per-move pixel delta fed into the wheel API
// as `delta_pixels / LINE_STEP_PX` so the existing
// "scroll_y_target -= delta * LINE_STEP_PX" math yields a
// 1:1 pixel follow of the finger.
// SCROLLING -> UP: already released; just reset state.
// PRESSED -> UP: normal tap -- fire scene__on_mouse_button(up).
// Slop: 16 logical pixels, matching the standard Android ViewConfig
// touchSlop on mdpi. Above that the user almost certainly intended a
// swipe, and canceling the tap is less annoying than a tap the user
// didn't mean to fire.
#define _PLATFORM_ANDROID_INTERNAL__TOUCH_SLOP_PX    16.0f
#define _PLATFORM_ANDROID_INTERNAL__TOUCH_LINE_STEP  40.0f

typedef enum _platform_android_internal__touch_phase_e
{
    _PLATFORM_ANDROID_INTERNAL__TOUCH_IDLE      = 0,
    _PLATFORM_ANDROID_INTERNAL__TOUCH_PRESSED   = 1,
    _PLATFORM_ANDROID_INTERNAL__TOUCH_SCROLLING = 2,
} _platform_android_internal__touch_phase_e;

static _platform_android_internal__touch_phase_e _platform_android_internal__touch_phase_g = _PLATFORM_ANDROID_INTERNAL__TOUCH_IDLE;
static int64 _platform_android_internal__touch_start_x = 0;
static int64 _platform_android_internal__touch_start_y = 0;
static int64 _platform_android_internal__touch_last_y  = 0;

static int32_t _platform_android_internal__on_input_event(struct android_app* app, AInputEvent* event)
{
    (void)app;

    int32_t type = AInputEvent_getType(event);
    if (type != AINPUT_EVENT_TYPE_MOTION)
    {
        // key events land here too; not routed yet. returning 0 lets
        // the system handle e.g. BACK / HOME properly.
        return 0;
    }

    int32_t action        = AMotionEvent_getAction(event);
    int32_t action_masked = action & AMOTION_EVENT_ACTION_MASK;

    // primary pointer coordinates. getX/getY at index 0 returns the
    // position in the window's coordinate space (top-left origin, y
    // grows down) -- same convention as our scene uses, so no flip.
    float fx = AMotionEvent_getX(event, 0);
    float fy = AMotionEvent_getY(event, 0);
    int64 x  = (int64)fx;
    int64 y  = (int64)fy;

    switch (action_masked)
    {
        case AMOTION_EVENT_ACTION_DOWN:
        {
            _platform_android_internal__touch_phase_g = _PLATFORM_ANDROID_INTERNAL__TOUCH_PRESSED;
            _platform_android_internal__touch_start_x = x;
            _platform_android_internal__touch_start_y = y;
            _platform_android_internal__touch_last_y  = y;
            scene__on_mouse_move(x, y);
            scene__on_mouse_button(0, TRUE, x, y);
            return 1;
        }
        case AMOTION_EVENT_ACTION_MOVE:
        {
            if (_platform_android_internal__touch_phase_g == _PLATFORM_ANDROID_INTERNAL__TOUCH_PRESSED)
            {
                // Still in the "is it a tap or a swipe?" window. Once
                // the finger has moved more than slop away from the
                // start, commit to SCROLLING: cancel the pressed
                // widget and start feeding the wheel API.
                int64 dy = y - _platform_android_internal__touch_start_y;
                int64 adx = x - _platform_android_internal__touch_start_x;
                float dy_abs = (dy < 0) ? (float)(-dy) : (float)dy;
                float dx_abs = (adx < 0) ? (float)(-adx) : (float)adx;
                // Require vertical dominance: diagonal swipes where the
                // horizontal component wins shouldn't steal slider /
                // scrollbar drags. Small tolerance (1.2x) so near-
                // vertical swipes still count even when the finger
                // wobbles a bit on the way down.
                // captures_drag escape hatch. Widgets like <canvas>
                // and <colorpicker> explicitly want the finger drag
                // to reach them regardless of direction / distance;
                // without this check the first 16 vertical pixels
                // of a paint stroke would steal the canvas press
                // and turn it into a scroll gesture on the parent.
                gui_node_t* pressed = scene__pressed();
                boole keep_drag = FALSE;
                if (pressed != NULL_POINTER)
                {
                    const widget_vtable_t* vt = widget_registry__get(pressed->type);
                    if (vt != NULL_POINTER && (vt->captures_drag || (vt->flags & GUI_WIDGET_FLAGS__CAPTURES_DRAG)))
                    {
                        keep_drag = TRUE;
                    }
                }

                if (keep_drag == FALSE && dy_abs > _PLATFORM_ANDROID_INTERNAL__TOUCH_SLOP_PX && dy_abs > dx_abs * 1.2f)
                {
                    scene__on_mouse_button(0, FALSE, x, y);
                    _platform_android_internal__touch_phase_g = _PLATFORM_ANDROID_INTERNAL__TOUCH_SCROLLING;
                    _platform_android_internal__touch_last_y  = _platform_android_internal__touch_start_y;
                    // Fall through to SCROLLING branch so the finger's
                    // travel so far is actually applied -- otherwise
                    // the first slop pixels would be silently dropped.
                }
                else
                {
                    scene__on_mouse_move(x, y);
                    return 1;
                }
            }

            if (_platform_android_internal__touch_phase_g == _PLATFORM_ANDROID_INTERNAL__TOUCH_SCROLLING)
            {
                // Per-move pixel delta. scene__on_mouse_wheel positive
                // = "scroll up" in the Windows wheel convention, which
                // `scroll_y_target -= delta * LINE_STEP_PX` implements
                // as scroll_y DECREASING. Finger moving UP (y decreases)
                // should increase scroll_y (content goes up, later
                // content visible), which means we need a NEGATIVE
                // wheel delta -- i.e. (curr_y - last_y) / line_step
                // naturally gets the sign right.
                // scene__on_touch_scroll bypasses scroll-smooth lerp
                // so the container tracks the finger 1:1 with no
                // lag. Previously the mouse-wheel entry was
                // converting this to a wheel tick and letting the
                // 180ms smooth-scroll animator chase the finger --
                // perceptually slow even though the math was right.
                // Sign convention: finger up = y decreases, so
                // (curr_y - last_y) is negative. scene__on_touch_scroll
                // takes delta_pixels where positive = finger DOWN
                // (shows earlier content). Our sign matches that
                // directly, no flip needed.
                float dy_pixels = (float)(y - _platform_android_internal__touch_last_y);
                scene__on_touch_scroll(x, y, dy_pixels);
                _platform_android_internal__touch_last_y = y;
                return 1;
            }

            return 1;
        }
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
        {
            // CANCEL fires when the OS steals the gesture (e.g. the
            // user swipes in from the edge to trigger a system gesture).
            // treat it as a release over the last known position so
            // widget drag state gets cleaned up.
            if (_platform_android_internal__touch_phase_g == _PLATFORM_ANDROID_INTERNAL__TOUCH_PRESSED)
            {
                scene__on_mouse_move(x, y);
                scene__on_mouse_button(0, FALSE, x, y);
            }
            // SCROLLING already released the press at the transition;
            // IDLE can happen if we somehow missed a DOWN.
            _platform_android_internal__touch_phase_g = _PLATFORM_ANDROID_INTERNAL__TOUCH_IDLE;
            return 1;
        }
        case AMOTION_EVENT_ACTION_SCROLL:
        {
            // External mouse or trackpad vertical scroll. Android
            // reports it through AXIS_VSCROLL in "unit" (normalized)
            // values -- ~1.0 per standard wheel tick, fractional for
            // high-res devices. Semantically matches what
            // scene__on_mouse_wheel expects (positive = content
            // should scroll UP).
            float vscroll = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
            scene__on_mouse_wheel(x, y, vscroll);
            return 1;
        }
        default:
            // POINTER_DOWN / POINTER_UP / HOVER_* etc. -- not wired up.
            // let the glue finish the event so Android doesn't stall.
            return 0;
    }
}

static void _platform_android_internal__query_window_size(void)
{
    if (_platform_android_internal__g.display == EGL_NO_DISPLAY || _platform_android_internal__g.surface == EGL_NO_SURFACE)
    {
        return;
    }
    EGLint w = 0;
    EGLint h = 0;
    eglQuerySurface(_platform_android_internal__g.display, _platform_android_internal__g.surface, EGL_WIDTH,  &w);
    eglQuerySurface(_platform_android_internal__g.display, _platform_android_internal__g.surface, EGL_HEIGHT, &h);
    if (w > 0 && h > 0)
    {
        _platform_android_internal__g.viewport_w = (int64)w;
        _platform_android_internal__g.viewport_h = (int64)h;
    }
}

// walk `dir` (an asset subdirectory like "fonts") and call
// font__register_from_file for each .ttf found, deriving the family
// name from the filename stem. AAssetManager_openDir returns an
// opaque iterator; AAssetDir_getNextFileName yields each leaf name
// in the directory. we reconstruct the full asset path "<dir>/<name>"
// before handing it to font__register_from_file because that's the
// path shape fs__read_entire_file's Android branch expects when
// routing through AAssetManager_open.
// robust-to-missing-dir: if the "fonts" directory doesn't exist in
// the APK (build script didn't stage TTFs, or a host doesn't want
// bundled fonts), AAssetManager_openDir returns NULL and we quietly
// no-op. host apps that want different fonts register their own
// via font__register_from_memory.
static void _platform_android_internal__scan_asset_fonts(AAssetManager* mgr, const char* dir)
{
    if (mgr == NULL_POINTER || dir == NULL_POINTER)
    {
        return;
    }

    AAssetDir* adir = AAssetManager_openDir(mgr, dir);
    if (adir == NULL_POINTER)
    {
        log_warn("%s %s %s", "fonts: AAssetManager_openDir('", dir, "') returned NULL; no bundled fonts will be registered");
        return;
    }

    int64 registered = 0;
    const char* name = NULL_POINTER;
    while ((name = AAssetDir_getNextFileName(adir)) != NULL_POINTER)
    {
        // skip anything that isn't a .ttf. the match is case-insensitive
        // against the four-byte trailing extension so Capitalized names
        // (.TTF / .Ttf) still get picked up.
        size_t nlen = stdlib__strlen(name);
        if (nlen < 5)
        {
            continue;
        }
        const char* ext = name + nlen - 4;
        if (((ext[0] == '.') && (ext[1] == 't' || ext[1] == 'T') && (ext[2] == 't' || ext[2] == 'T') && (ext[3] == 'f' || ext[3] == 'F')) == FALSE)
        {
            continue;
        }

        // build the asset path "<dir>/<name>" for fs__read_entire_file
        // to resolve through AAssetManager_open.
        char path[256];
        int64 path_len = 0;
        stdlib__strcpy_safe(path, (int64)sizeof(path), dir);
        stdlib__strcat_safe(path, (int64)sizeof(path), "/");
        path_len = stdlib__strcat_safe(path, (int64)sizeof(path), name);
        if (path_len <= 0 || path_len >= (int64)sizeof(path))
        {
            log_warn("%s %s %s", "fonts: skipping '", name, "' (path too long)");
            continue;
        }

        // derive family name = filename with the .ttf extension stripped.
        // matches the Windows build's GUI_FONTS_SOURCE_DIR scan convention
        // so .style files reference fonts by the same identifier on both
        // platforms ("Roboto-Regular", "ProggyClean", etc.).
        char family[64];
        size_t stem_n = nlen - 4;
        if (stem_n >= sizeof(family))
        {
            stem_n = sizeof(family) - 1;
        }
        stdlib__memcpy(family, name, stem_n);
        family[stem_n] = 0;

        if (font__register_from_file(family, path))
        {
            registered++;
        }
        else
        {
            log_warn("%s %s %s %s %s", "fonts: font__register_from_file failed for '", family, "' (asset path '", path, "')");
        }
    }

    AAssetDir_close(adir);
    log_info("%s %lld %s %s %s", "fonts: registered", (long long)registered, "families from asset dir '", dir, "'");
}

// compute the UI scale factor from the display's DPI. Android reports
// density buckets via AConfiguration_getDensity:
// 120 ldpi    (LOW)         ~0.75x -- obsolete, rarely seen
// 160 mdpi    (MEDIUM)       1.0x  -- baseline; 1 logical px == 1 phys px
// 213 tvdpi   (TV)           1.33x -- HDTV set-top boxes
// 240 hdpi    (HIGH)         1.5x  -- old phones
// 320 xhdpi   (XHIGH)        2.0x  -- common tablets
// 480 xxhdpi  (XXHIGH)       3.0x  -- modern mid-range phones
// 640 xxxhdpi (XXXHIGH)      4.0x  -- modern flagship phones
// or an arbitrary integer DPI on some devices. the conversion is just
// DPI / 160 (mdpi is the baseline "one logical px per physical px"
// class); we clamp to [1.0, 4.0] so a weird-DPI report (ACONFIGURATION
// _DENSITY_ANY = 0xfffe, _NONE = 0xffff, or 0 / DEFAULT) can't produce
// a degenerate scale and make the UI invisible / hundreds of pixels tall.
// this keeps the UI's physical size (in millimeters on the screen)
// roughly constant across devices without the host app having to
// think about DPI -- same .ui + .style reads well on everything from
// a low-DPI emulator to a xxxhdpi flagship phone.
static float _platform_android_internal__pick_ui_scale(struct android_app* app)
{
    if (app == NULL_POINTER || app->config == NULL_POINTER)
    {
        // no config available -- fall back to 1.0x. the app still works,
        // just at "one style px == one physical px" which reads tiny on
        // high-DPI devices.
        log_warn("%s", "ui_scale: app->config is NULL; leaving scale at 1.0");
        return 1.0f;
    }

    int32_t density = AConfiguration_getDensity(app->config);

    // filter out the sentinel / garbage values (DEFAULT=0, ANY=0xfffe,
    // NONE=0xffff, plus anything absurdly high like >10000). anything
    // outside (0, 2000] DPI treats the report as unknown and keeps
    // scale at 1.0.
    if (density <= 0 || density > 2000)
    {
        log_warn("%s %d %s", "ui_scale: AConfiguration_getDensity returned", (int)density, "(out of range); leaving scale at 1.0");
        return 1.0f;
    }

    float scale = (float)density / 160.0f;
    // don't shrink the UI on ldpi / TV
    if (scale < 1.0f)
    {
        scale = 1.0f;
    }
    // guard against hypothetical 8xdpi reports
    if (scale > 4.0f)
    {
        scale = 4.0f;
    }

    log_info("%s %d %s %.2f %s", "ui_scale: density=", (int)density, "dpi -> scale=", (double)scale, "x");
    return scale;
}

// ============================================================================
// UNIFIED platform__* API (see gui/src/platform.h)
// ============================================================================
// Thin wrappers so host code can call platform__init / platform__tick /
// platform__shutdown regardless of platform. On Android the android_app
// pointer has to be stashed here before host code runs because
// platform__init's signature doesn't carry it -- see the android_main
// entry below for where _platform_android_internal__stashed_app gets
// set.

static struct android_app* _platform_android_internal__stashed_app = NULL_POINTER;

/**
 * @brief initialize the platform and renderer. Called once at startup.
 *
 * @param const gui_app_config_t* cfg -> Window title / size / clear color.
 *
 * @return boole -> TRUE on success; FALSE if window creation or renderer init failed.
 */
boole platform__init(const gui_app_config_t* cfg)
{
    if (_platform_android_internal__stashed_app == NULL_POINTER)
    {
        log_error("%s", "platform__init: no android_app pointer stashed; was the host called from android_main?");
        return FALSE;
    }
    return platform_android__init(_platform_android_internal__stashed_app, cfg);
}

boole platform__tick(void)     { return platform_android__tick();    }
void  platform__shutdown(void) {        platform_android__shutdown(); }
boole platform__capture_bmp(const char* path) { (void)path; return FALSE; }
void  platform__set_topmost(void) { /* Android: activity lifecycle owns window visibility; no-op. */ }

// ============================================================================
// entry-point wrapper
// ============================================================================
// android_native_app_glue's ANativeActivity_onCreate spawns a worker
// thread that calls android_main(app). We stash the pointer, then
// forward to the host's app_main (host wrote `int main(int, char**)`
// which the platform.h rename turned into `int app_main(int, char**)`
// at translation-unit scope). Synthetic argc/argv are 0/NULL -- Android
// doesn't pass command-line args to native activities.
// Return value of app_main is discarded; android_main is void-returning
// and the Activity's lifetime is owned by the OS after this returns.

extern int app_main(int argc, char** argv);

void android_main(struct android_app* app)
{
    _platform_android_internal__stashed_app = app;
    (void)app_main(0, NULL_POINTER);
}
