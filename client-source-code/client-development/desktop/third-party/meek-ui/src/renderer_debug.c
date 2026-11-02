// Optional diagnostics shared by standalone and embedded renderers. No font
// files, widget nodes, allocations, locks or worker threads are needed.
#define _POSIX_C_SOURCE 200809L
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif
#include "renderer.h"
#include "renderer_debug.h"
#include "clib/stdlib.h"

static uint g_renderer_debug_flags;
static boole g_renderer_debug_configured;
static double g_renderer_debug_window_start;
static uint g_renderer_debug_frame_count;
static char g_renderer_debug_label[24] = "-- FPS";

static double _renderer_debug_internal__now_ms(void);
static void _renderer_debug_internal__draw_label(float offset, gui_color_t color);

/**
 * @brief read real elapsed time independently of the animation clock
 * @return double -> monotonic milliseconds
 */
static double _renderer_debug_internal__now_ms(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER tick;
    if (!frequency.QuadPart)
    {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&tick);
    return (double)tick.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    struct timespec tick;
    clock_gettime(CLOCK_MONOTONIC, &tick);
    return (double)tick.tv_sec * 1000.0 + (double)tick.tv_nsec / 1000000.0;
#endif
}
void renderer_debug__reset_timing(void)
{
    g_renderer_debug_window_start = 0;
    g_renderer_debug_frame_count = 0;
    stdlib__strcpy_safe(g_renderer_debug_label, sizeof(g_renderer_debug_label), "-- FPS");
}
void renderer__set_debug_flags(uint value)
{
    g_renderer_debug_flags = value;
    g_renderer_debug_configured = TRUE;
    renderer_debug__reset_timing();
}
uint renderer__debug_flags(void)
{
    if (!g_renderer_debug_configured)
    {
        const char* value = stdlib__getenv("MEEK_UI_FPS");
        g_renderer_debug_flags = (value && !stdlib__strcmp(value, "1")) ? GUI_RENDERER_DEBUG__FPS : 0;
        g_renderer_debug_configured = TRUE;
    }
    return g_renderer_debug_flags;
}
void renderer__parse_launch_flags(int* argc, char** argv)
{
    if (!argc || !argv || *argc < 1)
    {
        return;
    }
    int kept = 1;
    for (int i = 1; i < *argc; i++)
    {
        if (!stdlib__strcmp(argv[i], "--fps"))
        {
            renderer__set_debug_flags(renderer__debug_flags() | GUI_RENDERER_DEBUG__FPS);
        }
        else if (!stdlib__strcmp(argv[i], "--no-fps"))
        {
            renderer__set_debug_flags(renderer__debug_flags() & ~(uint)GUI_RENDERER_DEBUG__FPS);
        }
        else if (!stdlib__strcmp(argv[i], "--frame-compositor"))
        {
            renderer__set_compositor_mode(GUI_COMPOSITOR_MODE__FRAME);
        }
        else if (!stdlib__strcmp(argv[i], "--layer-compositor"))
        {
            renderer__set_compositor_mode(GUI_COMPOSITOR_MODE__LAYERS);
        }
        else
        {
            argv[kept++] = argv[i];
        }
    }
    argv[kept] = NULL;
    *argc = kept;
}

// Five-bit rows; this tiny diagnostic alphabet is independent of font atlases.
static const ubyte g_renderer_debug_digits[10][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30}, {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30}, {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};
static const ubyte f[7] = {31, 16, 16, 30, 16, 16, 16}, p[7] = {30, 17, 17, 30, 16, 16, 16};
static const ubyte s[7] = {15, 16, 16, 14, 1, 1, 30}, g_renderer_debug_dash[7] = {0, 0, 0, 14, 0, 0, 0};
static void _renderer_debug_internal__draw_label(float offset, gui_color_t color)
{
    for (int i = 0; g_renderer_debug_label[i]; i++)
    {
        char c = g_renderer_debug_label[i];
        const ubyte* glyph = c >= '0' && c <= '9' ? g_renderer_debug_digits[c - '0'] : c == 'F' ? f
                                                                  : c == 'P'   ? p
                                                                  : c == 'S'   ? s
                                                                  : c == '-'   ? g_renderer_debug_dash
                                                                               : NULL;
        if (!glyph)
        {
            continue;
        }
        for (int y = 0; y < 7; y++)
        {
            for (int x = 0; x < 5; x++)
            {
                if (glyph[y] & (1 << (4 - x)))
                {
                    renderer__submit_rect((gui_rect_t){6.0f + i * 18.0f + x * 3.0f + offset, 6.0f + y * 3.0f + offset, 3, 3}, color, 0);
                }
            }
        }
    }
}
void renderer_debug__draw(void)
{
    if (!(renderer__debug_flags() & GUI_RENDERER_DEBUG__FPS))
    {
        return;
    }
    if (!g_renderer_debug_window_start)
    {
        g_renderer_debug_window_start = _renderer_debug_internal__now_ms();
    }
    // Flush the scene first so its late text batch cannot cover this overlay.
    renderer__flush_pending_draws();
    _renderer_debug_internal__draw_label(1, (gui_color_t){0, 0, 0, 1});
    _renderer_debug_internal__draw_label(0, (gui_color_t){1, 1, 0, 1});
}
void renderer_debug__frame_finished(void)
{
    if (!(g_renderer_debug_flags & GUI_RENDERER_DEBUG__FPS) || !g_renderer_debug_window_start)
    {
        return;
    }
    double now = _renderer_debug_internal__now_ms(), elapsed = now - g_renderer_debug_window_start;
    g_renderer_debug_frame_count++;
    if (elapsed >= 500)
    {
        double fps = g_renderer_debug_frame_count * 1000.0 / elapsed;
        if (fps > 99999)
        {
            fps = 99999;
        }
        stdlib__i64toa(g_renderer_debug_label, (int64)sizeof(g_renderer_debug_label), (int64)(fps + 0.5));
        stdlib__strcat_safe(g_renderer_debug_label, sizeof(g_renderer_debug_label), " FPS");
        g_renderer_debug_window_start = now;
        g_renderer_debug_frame_count = 0;
    }
}
