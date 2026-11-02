#ifndef MEEK_UI_RENDERERS_BLUR_CPU_H
#define MEEK_UI_RENDERERS_BLUR_CPU_H

// _blur_cpu.h - shared CPU-side Gaussian blur for all renderer
// backends. Each backend reads back the framebuffer rect into an
// RGBA8 buffer (top-row-first), calls blur_cpu__apply_gaussian,
// uploads the result as a texture, and draws via the backend's
// own image pipeline.
// The CPU pass is identical across backends; only readback +
// texture upload differ. Pulling the duplicated kernel-build +
// separable-pass code into a shared helper keeps the per-backend
// blur_region down to ~50 lines.
// Math: standard separable 2D Gaussian. Kernel radius = ceil(3 *
// sigma) clamped at 32; horizontal pass writes to scratch, vertical
// pass writes back to the input buffer. Edge handling is
// clamp-to-edge (matches GL_CLAMP_TO_EDGE / D3D_TEXTURE_ADDRESS_CLAMP).
// Performance: pure scalar C, no SIMD. At a 400x100 rect with
// sigma=8 that's ~3.6 ms on a desktop CPU; phones see ~10 ms.
// Real-time UI blur (notification drawer drag) is borderline at
// large rects; future GPU FBO ping-pong implementation would
// supersede this for hot paths but the CPU fallback stays as the
// portable baseline.

#include "types.h"
#include "clib/stdlib.h" // stdlib__memcpy / stdlib__expf

// Run a separable Gaussian on the in-place RGBA8 buffer.
// pixels: top-row-first RGBA8, w*h*4 bytes. Modified in place.
// tmp:    scratch buffer of the same size; not preserved.
// w, h:   pixel dimensions of `pixels` and `tmp`.
// sigma:  Gaussian standard deviation in pixels. >= 1 typically.
// Caller should clamp; values <= 0 are treated as no-op.
// Caller-allocated buffers; helper does not allocate.
static inline void blur_cpu__apply_gaussian(ubyte* pixels, ubyte* tmp, int w, int h, float sigma)
{
    if (pixels == NULL_POINTER || tmp == NULL_POINTER)
    {
        return;
    }
    if (w <= 0 || h <= 0)
    {
        return;
    }
    if (sigma <= 0.0f)
    {
        return;
    }

    // Build the 1D Gaussian kernel. Radius covers ~99.7% of the
    // distribution's weight at 3 sigma; bigger adds cost without
    // visible benefit. Capped at 32 so extreme sigma values don't
    // blow up kernel width unboundedly.
    int ksize = (int)(sigma * 3.0f);
    if (ksize < 1)
    {
        ksize = 1;
    }
    if (ksize > 32)
    {
        ksize = 32;
    }
    int kw = ksize * 2 + 1;

    float kernel[65]; // ksize<=32 -> kw<=65.
    float ksum = 0.0f;
    float two_sigma_sq = 2.0f * sigma * sigma;
    for (int i = 0; i < kw; i++)
    {
        float d = (float)(i - ksize);
        kernel[i] = stdlib__expf(-(d* d) / two_sigma_sq);
        ksum += kernel[i];
    }
    for (int i = 0; i < kw; i++)
    {
        kernel[i] /= ksum;
    }

    // Horizontal pass: pixels -> tmp. Edge pixels clamp to nearest
    // valid column.
    for (int py = 0; py < h; py++)
    {
        ubyte* dst_row = tmp    + (size_t)py * (size_t)w * 4;
        ubyte* src_row = pixels + (size_t)py * (size_t)w * 4;
        for (int px = 0; px < w; px++)
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            for (int k = 0; k < kw; k++)
            {
                int sx = px + (k - ksize);
                if (sx < 0)
                {
                    sx = 0;
                }
                if (sx >= w)
                {
                    sx = w - 1;
                }
                ubyte* s = src_row + (size_t)sx * 4;
                float kv = kernel[k];
                r += (float)s[0] * kv;
                g += (float)s[1] * kv;
                b += (float)s[2] * kv;
                a += (float)s[3] * kv;
            }
            ubyte* d = dst_row + (size_t)px * 4;
            d[0] = (ubyte)r;
            d[1] = (ubyte)g;
            d[2] = (ubyte)b;
            d[3] = (ubyte)a;
        }
    }

    // Vertical pass: tmp -> pixels. Same kernel, stride the other
    // way.
    for (int py = 0; py < h; py++)
    {
        for (int px = 0; px < w; px++)
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            for (int k = 0; k < kw; k++)
            {
                int sy = py + (k - ksize);
                if (sy < 0)
                {
                    sy = 0;
                }
                if (sy >= h)
                {
                    sy = h - 1;
                }
                ubyte* s = tmp + ((size_t)sy * (size_t)w + (size_t)px) * 4;
                float kv = kernel[k];
                r += (float)s[0] * kv;
                g += (float)s[1] * kv;
                b += (float)s[2] * kv;
                a += (float)s[3] * kv;
            }
            ubyte* d = pixels + ((size_t)py * (size_t)w + (size_t)px) * 4;
            d[0] = (ubyte)r;
            d[1] = (ubyte)g;
            d[2] = (ubyte)b;
            d[3] = (ubyte)a;
        }
    }
}

// In-place top-bottom row swap. Used by GL backends after readback
// to flip GL's bottom-left-origin pixel buffer into our top-left-
// origin convention.
// pixels: w*h*4 byte buffer.
// scratch: w*4 byte temporary row buffer.
static inline void blur_cpu__flip_rows_v(ubyte* pixels, ubyte* scratch, int w, int h)
{
    if (pixels == NULL_POINTER || scratch == NULL_POINTER)
    {
        return;
    }
    for (int py = 0; py < h / 2; py++)
    {
        ubyte* row_a = pixels + (size_t)py         * (size_t)w * 4;
        ubyte* row_b = pixels + (size_t)(h-1-py)   * (size_t)w * 4;
        stdlib__memcpy(scratch, row_a,   (size_t)w * 4);
        stdlib__memcpy(row_a,   row_b,   (size_t)w * 4);
        stdlib__memcpy(row_b,   scratch, (size_t)w * 4);
    }
}

#endif
