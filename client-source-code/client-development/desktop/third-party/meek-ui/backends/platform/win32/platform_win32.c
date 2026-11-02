// platform_win32.c - window creation + message pump + input dispatch.
// renderer-agnostic. creates an HWND, pumps WM_* messages through the
// window procedure, translates mouse events into scene__on_* calls, then
// hands the HWND to whichever renderer backend was compiled in.
// everything graphics-API-specific (OpenGL context, D3D device+swapchain,
// Present/SwapBuffers) lives in the renderer backend -- see
// renderers/opengl3_renderer.c for the working reference, and d3d11/d3d9
// stubs for what a different backend would do. this file knows zero about
// the GPU.

#include <windows.h>

#include "types.h"
#include "gui.h"
#include "scene.h"
#include "renderer.h"
#include "renderer_backend.h"
#include "widget_registry.h"
#include "widgets/widget_image_cache.h"
#include "font.h"
#include "animator.h"
#include "clib/memory_manager.h"
#include "memory_types.h"
#include "third_party/log.h"

// DPI-awareness APIs. Declared inline instead of pulling in
// <shellscalingapi.h> because the SDK headers gate those functions
// behind Windows-version macros that aren't always set in the
// project's WIN32_LEAN_AND_MEAN build, and we runtime-resolve
// anyway so we compile + run on older Windows (falls through to
// system-DPI awareness on Windows 7, per-monitor V1 on Windows 8.1,
// per-monitor V2 on Windows 10 1703+).
typedef HANDLE DPI_AWARENESS_CONTEXT_compat;
#define _PLATFORM_WIN32_INTERNAL__DPI_V2_CTX ((DPI_AWARENESS_CONTEXT_compat)(-4))
// V1 per-monitor awareness is an enum value for SetProcessDpiAwareness
// (shcore.dll, Windows 8.1+):
// PROCESS_DPI_UNAWARE           = 0
// PROCESS_SYSTEM_DPI_AWARE      = 1
// PROCESS_PER_MONITOR_DPI_AWARE = 2
#define _PLATFORM_WIN32_INTERNAL__PROCESS_PER_MONITOR_DPI_AWARE 2

typedef BOOL  (WINAPI* fncp_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT_compat);
typedef HRESULT (WINAPI* fncp_SetProcessDpiAwareness)(int);
typedef BOOL  (WINAPI* fncp_SetProcessDPIAware)(void);
typedef UINT  (WINAPI* fncp_GetDpiForWindow)(HWND);
typedef UINT  (WINAPI* fncp_GetDpiForSystem)(void);

// Match the existing Wayland gate: ten seconds of activity, then an
// interruptible 250ms idle wait. Explicitly enabled by the host so legacy
// hosts that mutate the tree on every tick keep their existing behavior.
static boole _platform_win32_internal__render_gating = FALSE;
static boole _platform_win32_internal__force_render = TRUE;
static ULONGLONG _platform_win32_internal__last_activity = 0;
static LARGE_INTEGER _platform_win32_internal__last_frame_qpc;
static LARGE_INTEGER _platform_win32_internal__pacing_frequency;

// Try the modern per-monitor-V2 path first; fall back to the older
// ones in order. V2 is the only context that handles monitor-DPI
// changes and dialog scaling cleanly, but it's Windows 10 1703+; on
// anything older we settle for system-DPI awareness which is at
// least better than "Windows virtualises coordinates and hands us
// blurry pixels".
static void _platform_win32_internal__enable_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != NULL_POINTER)
    {
        fncp_SetProcessDpiAwarenessContext p_ctx = (fncp_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (p_ctx != NULL_POINTER && p_ctx(_PLATFORM_WIN32_INTERNAL__DPI_V2_CTX))
        {
            return;
        }
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != NULL_POINTER)
    {
        fncp_SetProcessDpiAwareness p_dpi = (fncp_SetProcessDpiAwareness)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (p_dpi != NULL_POINTER && SUCCEEDED(p_dpi(_PLATFORM_WIN32_INTERNAL__PROCESS_PER_MONITOR_DPI_AWARE)))
        {
            // Leave shcore.dll handle alive; the call already took
            // effect process-wide.
            return;
        }
    }
    if (user32 != NULL_POINTER)
    {
        fncp_SetProcessDPIAware p_sys = (fncp_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
        if (p_sys != NULL_POINTER)
        {
            p_sys();
        }
    }
}

// Read the DPI associated with a window (or system default). Per-
// monitor-V2 returns the monitor the window is currently on; older
// modes may return a cached value. 96 is the "100% scaling" default
// on Windows -- treat that as the 1.0 scale anchor.
static float _platform_win32_internal__scale_for_window(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    UINT dpi = 96;
    if (user32 != NULL_POINTER && hwnd != NULL_POINTER)
    {
        fncp_GetDpiForWindow p_win = (fncp_GetDpiForWindow)GetProcAddress(user32, "GetDpiForWindow");
        if (p_win != NULL_POINTER)
        {
            dpi = p_win(hwnd);
        }
        else
        {
            fncp_GetDpiForSystem p_sys = (fncp_GetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem");
            if (p_sys != NULL_POINTER)
            {
                dpi = p_sys();
            }
        }
    }
    // GetDpiForWindow can return 0 during some transitions; clamp
    // to 96 in that case so we at least don't scale by 0.
    if (dpi < 72)
    {
        dpi = 96;
    }
    return (float)dpi / 96.0f;
}

// Pull in the unified platform.h declarations so platform__init /
// tick / shutdown wrappers below compile. _PLATFORM_INTERNAL
// suppresses the `#define main app_main` rename inside that header,
// since this file deliberately defines the REAL `int main(...)`
// entry that forwards to the host's (already-renamed) app_main.
#define _PLATFORM_INTERNAL
#include "platform.h"
#include "clib/stdlib.h"
#include "fs.h"
#undef _PLATFORM_INTERNAL

/**
 *platform-wide state. kept as file-local globals because this is the
 *single process-wide window the poc manages.
 */
typedef struct _platform_win32_internal__state_t
{
    // owning module instance.
    HINSTANCE hinst;          // 0x0
    // window handle. the renderer borrows this via renderer__init.
    HWND      hwnd;           // 0x8
    // current client-area width.
    int64     viewport_w;     // 0x10
    // current client-area height.
    int64     viewport_h;     // 0x18
    // last cursor x in client coords.
    int64     mouse_x;        // 0x20
    // last cursor y in client coords.
    int64     mouse_y;        // 0x28
    // set TRUE when wm_close is received.
    boole     should_close;   // 0x30
    // back-buffer clear colour each frame.
    gui_color_t clear_color;    // 0x34
} _platform_win32_internal__state_t;

static _platform_win32_internal__state_t _platform_win32_internal__g;

static const wchar_t* _platform_win32_internal__class_name = L"GuiDemoWindowClass";

// forward declarations of statics.
static LRESULT CALLBACK _platform_win32_internal__wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static boole            _platform_win32_internal__register_class(HINSTANCE hinst);
static void             _platform_win32_internal__log_win32_error(char* where);
static gui_handler_fn   _platform_win32_internal__resolve_host_symbol(char* name);

// public api: initialize window and hand it to the renderer.
static boole platform_win32__init(const gui_app_config_t* cfg)
{
    // bring the allocation tracker up before anything else so every
    // subsequent MEM_* call lands in the live-list. No-op unless the
    // tracker wrapper is compiled in (GUI_TRACK_ALLOCATIONS).
    memory_manager__init();

    // Declare this process DPI-aware BEFORE creating any window.
    // Order matters: if awareness is flipped on after a window
    // exists, Windows may have already baked in a virtualised DPI
    // for that window and moving to a per-monitor mode can produce
    // sizes that don't agree with what GetDpiForWindow reports.
    // With awareness enabled, every coordinate we get from the OS
    // (WM_SIZE, WM_MOUSEMOVE, etc.) is in real pixels, and our
    // scene__set_scale multiplier compensates for high-DPI by
    // sizing widgets/fonts up. Matches what Android's density-
    // bucket auto-scale does.
    _platform_win32_internal__enable_dpi_awareness();

    // register every built-in widget into the registry first so that
    // parser_xml + scene see them all before any tree is loaded. host
    // apps that ship custom widgets call widget_registry__register
    // after this returns.
    widget_registry__bootstrap_builtins();

    _platform_win32_internal__g.hinst        = GetModuleHandleW(NULL_POINTER);
    _platform_win32_internal__g.viewport_w   = cfg->width;
    _platform_win32_internal__g.viewport_h   = cfg->height;
    _platform_win32_internal__g.should_close = FALSE;
    _platform_win32_internal__g.clear_color  = cfg->clear_color;

    if (_platform_win32_internal__register_class(_platform_win32_internal__g.hinst) == FALSE)
    {
        _platform_win32_internal__log_win32_error("RegisterClassExW");
        return FALSE;
    }

    // adjust client-area size so the window ends up at the requested size.
    // CreateWindowExW's width/height include the frame + title bar; we
    // want the client area to match cfg->width/height.
    RECT r;
    r.left   = 0;
    r.top    = 0;
    r.right  = (LONG)cfg->width;
    r.bottom = (LONG)cfg->height;
    DWORD style    = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    DWORD style_ex = 0;

    // a hidden window is created without WS_VISIBLE and never shown: the
    // swap chain and the back buffer exist all the same, only the
    // present has nothing to show. the test runner uses this so a sweep
    // does not put windows on the desktop.
    if (cfg->hidden == TRUE)
    {
        style = WS_OVERLAPPEDWINDOW;
    }

    AdjustWindowRectEx(&r, style, FALSE, style_ex);

    _platform_win32_internal__g.hwnd = CreateWindowExW( style_ex, _platform_win32_internal__class_name, cfg->title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL_POINTER, NULL_POINTER, _platform_win32_internal__g.hinst, NULL_POINTER );
    if (_platform_win32_internal__g.hwnd == NULL_POINTER)
    {
        _platform_win32_internal__log_win32_error("CreateWindowExW");
        UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);
        return FALSE;
    }

    // Now that we have a window on a specific monitor, read its DPI
    // and set the global UI scale. The cfg->width / height were
    // interpreted as LOGICAL pixels (100%-scaling reference); with
    // DPI awareness on, Windows created the window at those pixel
    // counts but at e.g. 200% DPI they'll occupy only half the
    // physical space a 100% user would expect. Resize the client
    // area to the DPI-scaled equivalent so a `width: 800` request
    // produces an 800-logical-pixel visual regardless of monitor.
    float ui_scale = _platform_win32_internal__scale_for_window(_platform_win32_internal__g.hwnd);
    scene__set_scale(ui_scale);

    if (ui_scale > 1.01f || ui_scale < 0.99f)
    {
        // Bump the window size to match. AdjustWindowRectExForDpi
        // would be the ideal call here but it's Windows 10+ only;
        // the plain AdjustWindowRectEx already gives non-client
        // areas in physical pixels under DPI awareness, so using
        // it is fine.
        RECT target;
        target.left   = 0;
        target.top    = 0;
        target.right  = (LONG)((float)cfg->width  * ui_scale);
        target.bottom = (LONG)((float)cfg->height * ui_scale);
        AdjustWindowRectEx(&target, style, FALSE, style_ex);
        SetWindowPos(_platform_win32_internal__g.hwnd, NULL_POINTER, 0, 0, target.right - target.left, target.bottom - target.top, SWP_NOMOVE | SWP_NOZORDER);
        log_info("%s %.0f %s %.2f %s", "platform_win32: DPI", (double)(ui_scale * 100.0f), "%  (scale", (double)ui_scale, "x)");
    }

    // hand the window over to whichever renderer backend was linked in.
    // the backend creates its own graphics context against this HWND:
    // OpenGL grabs an HDC + makes a GL context; D3D backends create a
    // swapchain; etc. we stay ignorant of all that.
    if (renderer__init((void*)_platform_win32_internal__g.hwnd) == FALSE)
    {
        log_error("%s", "renderer__init failed");
        // renderer__init is documented as safe-on-partial-state, so
        // call shutdown to release whatever it managed to allocate
        // (HDC, partial GL context, vertex buffers) before we drop
        // the window. Without this, every failed launch leaks all
        // those resources for the lifetime of the process.
        renderer__shutdown();
        DestroyWindow(_platform_win32_internal__g.hwnd);
        UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);
        _platform_win32_internal__g.hwnd = NULL_POINTER;
        return FALSE;
    }

    // font subsystem. runs after renderer__init because atlas upload
    // (lazily, on first font__at) needs the graphics context live.
    // font__init scans gui/src/fonts/ and auto-registers every .ttf
    // found there -- host apps don't call any font functions; they just
    // reference families by name in their .style files.
    if (font__init() == FALSE)
    {
        log_error("%s", "font__init failed");
        // Tear down in reverse of the work done above. font__shutdown
        // is a no-op if font__init bailed early enough that nothing
        // was registered.
        font__shutdown();
        renderer__shutdown();
        DestroyWindow(_platform_win32_internal__g.hwnd);
        UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);
        _platform_win32_internal__g.hwnd = NULL_POINTER;
        return FALSE;
    }

    // install the host-symbol resolver. scene's dispatch falls back to
    // this when the registration table doesn't know a handler hash; we
    // look up the name in the host exe's export table so any function
    // marked UI_HANDLER in the host app is reachable from .ui files.
    scene__set_symbol_resolver(_platform_win32_internal__resolve_host_symbol);

    return TRUE;
}

// public api: pump one frame of os messages, then render.
// returns FALSE once the window has been asked to close.
static boole platform_win32__tick(void)
{
    MSG msg;
    ULONGLONG now = GetTickCount64();
    DWORD wait_ms = 0;
    boole animation = (boole)(animator__has_active() != 0);
    LARGE_INTEGER pacing_now;
    if (_platform_win32_internal__pacing_frequency.QuadPart == 0)
        QueryPerformanceFrequency(&_platform_win32_internal__pacing_frequency);
    QueryPerformanceCounter(&pacing_now);

    if (_platform_win32_internal__render_gating == TRUE)
    {
        if (animation == TRUE || _platform_win32_internal__force_render == TRUE)
        {
            _platform_win32_internal__last_activity = now;
        }
        if (now - _platform_win32_internal__last_activity >= 10000)
        {
            wait_ms = 250;
        }
        else if ((_platform_win32_internal__g.viewport_w <= 0 || _platform_win32_internal__g.viewport_h <= 0 ||
            renderer_backend__presents_with_vsync() == FALSE) && _platform_win32_internal__last_frame_qpc.QuadPart != 0)
        {
            double elapsed = (pacing_now.QuadPart - _platform_win32_internal__last_frame_qpc.QuadPart) * 1000.0 /
                _platform_win32_internal__pacing_frequency.QuadPart;
            // GetTickCount64 can advance in ~15.6 ms steps. It can therefore
            // report <16 ms after an entire vsynced frame and add a second
            // wait. Use precise elapsed time; leave sub-ms pacing to Present.
            // A minimized window does not Present, so it still needs this wait.
            if (elapsed < 16.0) wait_ms = (DWORD)(16.0 - elapsed);
        }
        if (wait_ms > 0)
        {
            MsgWaitForMultipleObjectsEx(0, NULL_POINTER, wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
    while (PeekMessageW(&msg, NULL_POINTER, 0, 0, PM_REMOVE))
    {
        _platform_win32_internal__force_render = TRUE;
        _platform_win32_internal__last_activity = GetTickCount64();
        if (msg.message == WM_QUIT)
        {
            _platform_win32_internal__g.should_close = TRUE;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (_platform_win32_internal__g.should_close)
    {
        return FALSE;
    }
    now = GetTickCount64();
    if (_platform_win32_internal__render_gating == TRUE && _platform_win32_internal__force_render == FALSE && animator__has_active() == 0 && now - _platform_win32_internal__last_activity >= 10000)
    {
        return TRUE;
    }
    _platform_win32_internal__force_render = FALSE;
    QueryPerformanceCounter(&_platform_win32_internal__last_frame_qpc);

    // a minimized window reports a 0x0 client area; laying the scene out
    // against that would collapse every node and there is nothing to draw,
    // so the frame is skipped and the last layout stays valid
    if (_platform_win32_internal__g.viewport_w <= 0 || _platform_win32_internal__g.viewport_h <= 0)
    {
        return TRUE;
    }

    // capture this frame's start time in milliseconds and hand it to
    // scene. anything time-sensitive in the rest of this tick (the
    // animator, future hot-reload poll) reads scene__frame_time_ms /
    // scene__frame_delta_ms instead of taking its own clock sample,
    // so all subsystems see one consistent timestamp per frame.
    // QueryPerformanceCounter is the precise Win32 monotonic clock.
    // we convert ticks to ms via the frequency, sampled once per
    // tick (cheap; cached by the kernel).
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        int64 now_ms = (int64)((now.QuadPart * 1000) / freq.QuadPart);
        scene__begin_frame_time(now_ms);
    }

    // resolve styles, run animator, layout, draw. ordering matters:
    // 1. resolve writes resolved.* from style[state].
    // 2. animator mutates resolved.* in place (alpha fades, etc.).
    // 3. layout reads resolved.* (pad, gap, size).
    // 4. emit_draws reads resolved.* (bg, color).
    scene__resolve_styles();
    animator__tick();
    scene__layout(_platform_win32_internal__g.viewport_w, _platform_win32_internal__g.viewport_h);

    renderer__begin_frame(_platform_win32_internal__g.viewport_w, _platform_win32_internal__g.viewport_h, _platform_win32_internal__g.clear_color);

    // type-aware tree walk lives in scene -- it knows that sliders emit
    // two rects (track + thumb), regular nodes emit one. the platform
    // layer just hands the renderer to it via the previously configured
    // begin_frame.
    scene__emit_draws();

    // end_frame finalizes the draw AND presents the back buffer (GL:
    // SwapBuffers, D3D: swapchain->Present). no platform-side present
    // call needed -- the renderer backend owns that.
    renderer__end_frame();
    return TRUE;
}

// public api: tear down in reverse order of platform_win32__init.
static void platform_win32__shutdown(void)
{
    // Image cache first: every cached entry holds a renderer-owned
    // texture, so we release them while the renderer is still alive.
    // Without this, the textures + path strings show up as leaks in
    // memory_manager's exit-time report.
    widget_image__cache_shutdown();

    // font subsystem next: it releases atlas textures through the
    // renderer's destroy_atlas entry point, so it has to run while the
    // renderer is still alive.
    font__shutdown();

    // shutdown the renderer next -- it releases its HDC (for GL
    // backends), its context, its GPU resources. must happen while the
    // window still exists so GDI/DX can release the HDC cleanly.
    renderer__shutdown();

    if (_platform_win32_internal__g.hwnd != NULL_POINTER)
    {
        DestroyWindow(_platform_win32_internal__g.hwnd);
        _platform_win32_internal__g.hwnd = NULL_POINTER;
    }

    UnregisterClassW(_platform_win32_internal__class_name, _platform_win32_internal__g.hinst);

    // tracker shutdown logs any leaks and walks the live list. Runs
    // after every subsystem has freed its owned blocks; anything still
    // outstanding at this point is a real leak.
    memory_manager__shutdown();
}

// ============================================================================
// UNIFIED platform__* API (see gui/src/platform.h)
// ============================================================================
// Thin wrappers so host code can call platform__init / platform__tick /
// platform__shutdown regardless of target. The internal platform_win32__*
// names are kept for backward compatibility.

boole platform__init(const gui_app_config_t* cfg)     { return platform_win32__init(cfg); }
boole platform__tick(void)                          { return platform_win32__tick();    }
void  platform__shutdown(void)                      {        platform_win32__shutdown(); }

/**
 * @brief opt a Win32 host into the existing animation-aware render-gating design
 * @param boole enabled -> TRUE to pace active frames and wait while idle
 * @return void
 */
void platform_win32__set_render_gating(boole enabled)
{
    _platform_win32_internal__render_gating = enabled;
    _platform_win32_internal__force_render = TRUE;
    _platform_win32_internal__last_activity = GetTickCount64();
}

/**
 * @brief request a frame after a host-side change such as hot reload
 * @return void
 */
void platform_win32__request_render(void)
{
    _platform_win32_internal__force_render = TRUE;
    _platform_win32_internal__last_activity = GetTickCount64();
}

// Bring the window to the top of the z-order and pin it there.
// Used by the visual-regression test runner so capture-via-BitBlt
// doesn't pick up whatever happens to be in front (debugger,
// console, another running test). SWP_TOPMOST sets the topmost
// flag so subsequent windows don't cover it; SetForegroundWindow
// actually brings it forward.
/**
 * @brief bring the platform window to the top of the z-order and pin it there (Win32: SWP_TOPMOST + SetForegroundWindow). Called from the visual-regression test runner before capturing so nothing obscures the window's client area during BitBlt. No-op on platforms other than Windows.
 *
 * @return void
 */
void platform__set_topmost(void)
{
    HWND hwnd = _platform_win32_internal__g.hwnd;
    if (hwnd == NULL_POINTER)
    {
        return;
    }

    // a window created hidden stays hidden: raising it would show it
    if (IsWindowVisible(hwnd) == FALSE)
    {
        return;
    }

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(hwnd);
}

// Capture the window client area to a 32-bit BGRA .bmp. Win32 path:
// 1. GetClientRect(hwnd) to get the size we need.
// 2. GetDC(hwnd) + CreateCompatibleDC + CreateCompatibleBitmap to
// make an offscreen DIB-compatible bitmap.
// 3. BitBlt from the window DC to the offscreen DC.
// 4. GetDIBits into a host buffer with a BITMAPINFO asking for
// 32 bpp top-down (negative height) + BI_RGB so the decoded
// layout is plain BGRA with no row padding beyond DWORD.
// 5. Write the BMP file: BITMAPFILEHEADER + BITMAPINFOHEADER +
// pixels. Hand-rolled; avoids pulling in a PNG encoder
// dependency just for test captures.
// Used by the visual-regression runner at tests/visual/test_runner.c.
// Intentionally tolerant -- failures log and return FALSE so the
// test driver can record the miss and continue.
/**
 * @brief capture the current window client area to a 32-bit BGRA .bmp file. Returns TRUE on success, FALSE on any IO or API failure. Intended for the visual-regression test runner under `tests/visual/`; not used by normal host apps. Path is a filesystem path in the native encoding. Overwrites any existing file. Format choice: BMP over PNG because BMP needs no compression dependency (stb_image_write + zlib) and every browser / image viewer displays it natively. Resulting files are larger than PNG but test captures are small (typical 800x600 ~= 1.8 MB). Must be called AFTER a frame has been rendered + presented (the captured pixels come from the window's composited client area); typically the test runner ticks 5+ frames to let animations settle, then calls this. Only the Win32 backend implements this today; other backends return FALSE.
 *
 * @param const char* path -> output filesystem path.
 *
 * @return boole -> TRUE on success.
 */
// Writes top-down BGRA rows as a 32-bit .bmp (stored bottom-up, as the
// format wants). Shared by the renderer capture and the GDI fallback.
static boole _platform_win32_internal__write_bmp32(const char* path, const BYTE* bgra_topdown, LONG w, LONG h)
{
    DWORD row_bytes   = (DWORD)(w * 4);
    DWORD pixel_bytes = row_bytes * (DWORD)h;
    DWORD file_off    = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    DWORD file_size   = file_off + pixel_bytes;
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    BYTE* file = NULL_POINTER;
    LONG  row  = 0;
    boole ok   = FALSE;

    ZeroMemory(&bfh, sizeof(bfh));
    bfh.bfType    = 0x4D42; // 'BM'
    bfh.bfSize    = file_size;
    bfh.bfOffBits = file_off;
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = w;
    bih.biHeight      = h; // POSITIVE: legacy readers expect rows stored bottom-up.
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage   = pixel_bytes;

    // the whole file is assembled in memory and handed to fs, so the capture path needs no stdio
    file = (BYTE*)GUI_MALLOC_T((size_t)file_size, MM_TYPE__GENERIC);

    if (file == NULL_POINTER)
    {
        log_error("%s", "capture: out of memory for the .bmp buffer");
        return FALSE;
    }

    stdlib__memcpy(file, &bfh, sizeof(bfh));
    stdlib__memcpy(file + sizeof(bfh), &bih, sizeof(bih));

    // Pixel rows: buffer is top-down (row 0 = top), file wants bottom-up.
    for (row = h - 1; row >= 0; row--)
    {
        stdlib__memcpy(file + file_off + (size_t)(h - 1 - row) * row_bytes, bgra_topdown + (size_t)row * row_bytes, row_bytes);
    }

    ok = fs__write_entire_file((char*)path, file, (int64)file_size);
    GUI_FREE(file);

    return ok;
}

// The renderer's own copy of the frame: armed, produced by one more tick
// (inside renderer__end_frame, before the present), written as .bmp. A
// flip-model swap chain has nothing GDI can copy and a desktop window in
// front would end up in a GDI copy, so this is the path the test harness
// relies on; the GDI copy below stays as the fallback for backends
// without a readback.
static boole _platform_win32_internal__capture_through_renderer(const char* path)
{
    int64  w    = 0;
    int64  h    = 0;
    int64  cap  = 0;
    int64  i    = 0;
    ubyte* rgba = NULL_POINTER;
    boole  ok   = FALSE;

    if (renderer__capture_size(&w, &h) == FALSE || w <= 0 || h <= 0)
    {
        return FALSE;
    }

    cap  = w * h * 4;
    rgba = (ubyte*)GUI_MALLOC_T((size_t)cap, MM_TYPE__GENERIC);

    if (rgba == NULL_POINTER)
    {
        return FALSE;
    }

    if (renderer__request_capture(rgba, cap) == FALSE)
    {
        GUI_FREE(rgba);
        return FALSE;
    }

    platform_win32__request_render();
    platform_win32__tick();

    if (renderer__capture_done() == FALSE)
    {
        log_warn("%s", "platform__capture_bmp: the renderer did not produce the frame copy; copying the window instead");
        GUI_FREE(rgba);
        return FALSE;
    }

    // RGBA to BGRA in place for the .bmp
    for (i = 0; i < w * h; i++)
    {
        ubyte r = rgba[i * 4 + 0];
        rgba[i * 4 + 0] = rgba[i * 4 + 2];
        rgba[i * 4 + 2] = r;
        rgba[i * 4 + 3] = 255;
    }

    ok = _platform_win32_internal__write_bmp32(path, rgba, (LONG)w, (LONG)h);
    GUI_FREE(rgba);

    return ok;
}

boole platform__capture_bmp(const char* path)
{
    if (path != NULL_POINTER && _platform_win32_internal__capture_through_renderer(path) == TRUE)
    {
        log_info("%s %s", "capture: frame read back through the renderer to", path);
        return TRUE;
    }

    if (path == NULL_POINTER || path[0] == 0)
    {
        log_error("%s", "platform__capture_bmp: NULL path");
        return FALSE;
    }
    HWND hwnd = _platform_win32_internal__g.hwnd;
    if (hwnd == NULL_POINTER)
    {
        log_error("%s", "platform__capture_bmp: no window");
        return FALSE;
    }
    RECT rc;
    if (GetClientRect(hwnd, &rc) == FALSE)
    {
        log_error("%s", "GetClientRect failed");
        return FALSE;
    }
    LONG w = rc.right  - rc.left;
    LONG h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
    {
        // A minimized window reports a 0x0 client area. The test runner
        // hits this when something on the desktop minimizes or hides the
        // window between creation and capture; restore it and read again.
        ShowWindow(hwnd, SW_RESTORE);
        UpdateWindow(hwnd);
        Sleep(50);
        if (GetClientRect(hwnd, &rc) == FALSE)
        {
            log_error("%s", "GetClientRect failed after restore");
            return FALSE;
        }
        w = rc.right  - rc.left;
        h = rc.bottom - rc.top;
    }
    if (w <= 0 || h <= 0)
    {
        log_error("%s", "client area is 0x0");
        return FALSE;
    }

    HDC     src_dc  = GetDC(hwnd);
    HDC     dst_dc  = NULL_POINTER;
    HBITMAP dst_bmp = NULL_POINTER;
    BYTE*   pixels  = NULL_POINTER;
    boole   ok      = FALSE;

    if (src_dc == NULL)
    {
        log_error("%s", "GetDC failed");
        goto cleanup;
    }
    dst_dc = CreateCompatibleDC(src_dc);
    if (dst_dc == NULL)
    {
        log_error("%s", "CreateCompatibleDC failed");
        goto cleanup;
    }

    // Target bitmap in 32-bit BGRA, top-down (negative biHeight so
    // row 0 is the top row in memory). BI_RGB + 32 bpp = plain BGRA
    // with DWORD row alignment -- for any width the stride is
    // w * 4, no padding needed.
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    dst_bmp = CreateDIBSection(src_dc, &bmi, DIB_RGB_COLORS, (void**)&pixels, NULL_POINTER, 0);
    if (dst_bmp == NULL || pixels == NULL)
    {
        log_error("%s", "CreateDIBSection failed");
        goto cleanup;
    }

    HGDIOBJ old_bmp = SelectObject(dst_dc, dst_bmp);
    BOOL blt_ok = BitBlt(dst_dc, 0, 0, w, h, src_dc, 0, 0, SRCCOPY);
    SelectObject(dst_dc, old_bmp);
    if (blt_ok == FALSE)
    {
        log_error("%s", "BitBlt failed");
        goto cleanup;
    }

    // the DIB is top-down BGRA, exactly what the writer takes
    ok = _platform_win32_internal__write_bmp32(path, pixels, w, h);

cleanup:
    if (dst_bmp != NULL_POINTER)
    {
        DeleteObject(dst_bmp);
    }
    if (dst_dc  != NULL_POINTER)
    {
        DeleteDC(dst_dc);
    }
    if (src_dc  != NULL_POINTER)
    {
        ReleaseDC(hwnd, src_dc);
    }
    return ok;
}

// The actual `int main()` entry that forwards to the host's
// (renamed) `app_main` lives in platforms/windows/main_entry_win32.c
// and gets compiled into the host EXECUTABLE, not into gui.dll. The
// DLL can't resolve `app_main` at link time because the symbol
// belongs to the exe -- putting the stub here would fail the gui.dll
// link with "undefined symbol: app_main". Each Windows host's
// CMakeLists (e.g. demo-windows/CMakeLists.txt) adds that entry
// stub file to its `add_executable(...)` source list.

// register the window class.
static boole _platform_win32_internal__register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = _platform_win32_internal__wnd_proc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(NULL_POINTER, IDC_ARROW);
    wc.lpszClassName = _platform_win32_internal__class_name;
    return (boole)(RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

static void _platform_win32_internal__modifiers(void)
{
    uint modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
        modifiers |= 1;
    }
    if (GetKeyState(VK_CONTROL) & 0x8000)
    {
        modifiers |= 4;
    }
    if (GetKeyState(VK_MENU) & 0x8000)
    {
        modifiers |= 8;
    }
    scene__set_active_modifiers(modifiers);
}

// the one and only window procedure. translates win32 messages into
// scene__on_* calls, keeps a bit of platform state up to date, and
// delegates the rest to DefWindowProcW.
static LRESULT CALLBACK _platform_win32_internal__wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static uint pending_surrogate = 0;
    switch (msg)
    {
        case WM_KILLFOCUS:
        {
            pending_surrogate = 0;
            scene__set_active_modifiers(0);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_CLOSE:
        {
            _platform_win32_internal__g.should_close = TRUE;
            PostQuitMessage(0);
            return 0;
        }

        case WM_SIZE:
        {
            int64 w = (int64)LOWORD(lp);
            int64 h = (int64)HIWORD(lp);
            _platform_win32_internal__g.viewport_w = w;
            _platform_win32_internal__g.viewport_h = h;
            scene__on_resize(w, h);
            return 0;
        }

        case 0x02E0: // WM_DPICHANGED (Windows 8.1+, defined by value so we don't need shellscalingapi.h)
        {
            // Fired when the window moves to a monitor with a
            // different DPI, or when the user changes display scaling.
            // wParam low word = new DPI (high word same on V2; per-axis
            // on V1). lParam points to a suggested new window rect
            // that preserves physical size across the DPI change;
            // ignoring it would mean the window shrinks or grows
            // awkwardly, so we apply it via SetWindowPos.
            UINT new_dpi = LOWORD(wp);
            if (new_dpi < 72)
            {
                new_dpi = 96;
            }
            float new_scale = (float)new_dpi / 96.0f;
            scene__set_scale(new_scale);
            RECT* suggested = (RECT*)lp;
            if (suggested != NULL_POINTER)
            {
                SetWindowPos(hwnd, NULL_POINTER, suggested->left, suggested->top, suggested->right  - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            log_info("%s %u %s %.2f %s", "platform_win32: DPI changed to", new_dpi, "(scale", (double)new_scale, "x)");
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            int64 x = (int64)(SHORT)LOWORD(lp);
            int64 y = (int64)(SHORT)HIWORD(lp);
            _platform_win32_internal__g.mouse_x = x;
            _platform_win32_internal__g.mouse_y = y;
            scene__on_mouse_move(x, y);
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            _platform_win32_internal__modifiers();
            SetCapture(hwnd);
            int64 x = (int64)(SHORT)LOWORD(lp);
            int64 y = (int64)(SHORT)HIWORD(lp);
            scene__on_mouse_button(0, TRUE, x, y);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            ReleaseCapture();
            int64 x = (int64)(SHORT)LOWORD(lp);
            int64 y = (int64)(SHORT)HIWORD(lp);
            scene__on_mouse_button(0, FALSE, x, y);
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            // WM_MOUSEWHEEL delivers the cursor position in SCREEN
            // coordinates (not client) in lParam -- different from
            // the WM_MOUSE* messages. ScreenToClient converts back
            // to the window's coordinate space so scene__hit_test
            // finds the right node.
            // wParam high word carries a signed 16-bit delta in
            // units of WHEEL_DELTA (120). A "standard" wheel tick
            // gives ±WHEEL_DELTA; high-res wheels pass smaller
            // fractions of WHEEL_DELTA. Positive = wheel moved AWAY
            // from the user (content scrolls up), which matches the
            // convention scene__on_mouse_wheel expects.
            POINT p;
            p.x = (SHORT)LOWORD(lp);
            p.y = (SHORT)HIWORD(lp);
            ScreenToClient(hwnd, &p);
            int   raw   = (int)(SHORT)HIWORD(wp);
            float delta = (float)raw / (float)WHEEL_DELTA;
            scene__on_mouse_wheel((int64)p.x, (int64)p.y, delta);
            return 0;
        }

        case WM_CHAR:
        {
            uint cp = (uint)wp;
            _platform_win32_internal__modifiers();
            if (cp >= 0xd800 && cp <= 0xdbff)
            {
                pending_surrogate = cp;
                return 0;
            }
            if (cp >= 0xdc00 && cp <= 0xdfff)
            {
                if (pending_surrogate == 0)
                {
                    return 0;
                }
                cp = 0x10000 + ((pending_surrogate - 0xd800) << 10) + cp - 0xdc00;
            }
            pending_surrogate = 0;
            scene__on_char(cp);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_KEYUP:
        {
            // key-level events (as opposed to WM_CHAR's translated text).
            // forwarded so widgets can handle non-character actions:
            // backspace, delete, arrow keys, enter, ctrl+shortcuts. we
            // still let DefWindowProcW run afterward so TranslateMessage
            // can synthesize the matching WM_CHAR.
            _platform_win32_internal__modifiers();
            scene__on_key((int64)wp, msg == WM_KEYDOWN);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_ERASEBKGND:
        {
            // the renderer owns the client area; skip gdi's default fill
            // to avoid flicker on resize.
            return 1;
        }

        default:
        {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
}

// best-effort win32 error log. no-op if there's no error code pending.
static void _platform_win32_internal__log_win32_error(char* where)
{
    DWORD err = GetLastError();
    log_error("%s %s %lu %s", where, "failed (error", (unsigned long)err, ")");
}

// symbol resolver: find a function by name in the host process's exe.
// GetModuleHandleW(NULL) returns the host exe's module regardless of
// whether the caller lives in gui.dll or in the exe itself. functions
// marked UI_HANDLER (__declspec(dllexport)) end up in the exe's export
// table and are reachable through GetProcAddress.
static gui_handler_fn _platform_win32_internal__resolve_host_symbol(char* name)
{
    HMODULE host = GetModuleHandleW(NULL_POINTER);
    if (host == NULL_POINTER)
    {
        return NULL_POINTER;
    }
    FARPROC sym = GetProcAddress(host, name);
    return (gui_handler_fn)(void*)sym;
}
