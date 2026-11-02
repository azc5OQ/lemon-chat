#ifndef MEEK_UI_RENDERERS_MIP_CPU_H
#define MEEK_UI_RENDERERS_MIP_CPU_H

// _mip_cpu.h - shared CPU-side mipmap chain for the image textures of
// every renderer backend. Each backend used to ask its API to build the
// chain (glGenerateMipmap, GenerateMips, D3DUSAGE_AUTOGENMIPMAP) or had
// none at all (d3d12, and d3d9 whenever the managed pool was needed),
// so a photo scaled down to a tile was filtered differently per
// backend and the visual sweep showed it. Building the levels here,
// with one box filter, and uploading them explicitly gives every
// backend the same texels; the samplers are trilinear everywhere, so
// what remains is the GPU's own filtering arithmetic.
// Level i+1 is the 2x2 box average of level i. An odd edge pairs its
// last pixel with itself (clamp). Averaging happens on the stored
// bytes, without a transfer function, as the driver generators did.
// The chain goes down to 1x1: floor(log2(max(w, h))) + 1 levels, which
// is what makes a GL texture complete without GL_TEXTURE_MAX_LEVEL.
// Storage for levels 1.. is one caller-provided block of
// mip_cpu__storage_bytes(w, h) bytes (about a third of the top level);
// the helper allocates nothing. A NULL block leaves the chain at one
// level, which the backends then sample bilinearly.

#include "types.h"
#include "clib/stdlib.h"

// enough for a 65536-pixel edge
#define MIP_CPU__MAX_LEVELS 17

typedef struct mip_cpu__level_t
{
    const ubyte* pixels; // 0x0  tightly packed RGBA8 rows
    int          w;      // 0x8
    int          h;      // 0xc
} mip_cpu__level_t;

typedef struct mip_cpu__chain_t
{
    mip_cpu__level_t levels[MIP_CPU__MAX_LEVELS]; // 0x0
    int              count;                       // 0x110
} mip_cpu__chain_t;

// Number of levels a full chain has for a w x h top level.
static inline int mip_cpu__level_count(int w, int h)
{
    int m = w > h ? w : h;
    int n = 1;

    while (m > 1 && n < MIP_CPU__MAX_LEVELS)
    {
        m >>= 1;
        n++;
    }

    return n;
}

// Bytes the levels below the top one need together. 0 for a 1x1 image.
static inline size_t mip_cpu__storage_bytes(int w, int h)
{
    size_t total = 0;
    int    lw    = w;
    int    lh    = h;
    int    n     = 1;

    while ((lw > 1 || lh > 1) && n < MIP_CPU__MAX_LEVELS)
    {
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
        total += (size_t)lw * (size_t)lh * 4u;
        n++;
    }

    return total;
}

// One level down: dst (dw x dh) is the 2x2 box average of src (sw x sh).
static inline void mip_cpu__downsample(const ubyte* src, int sw, int sh, ubyte* dst, int dw, int dh)
{
    int x = 0;
    int y = 0;
    int c = 0;

    for (y = 0; y < dh; y++)
    {
        int          y0   = y * 2;
        int          y1   = y0 + 1 < sh ? y0 + 1 : sh - 1;
        const ubyte* row0 = src + (size_t)y0 * (size_t)sw * 4u;
        const ubyte* row1 = src + (size_t)y1 * (size_t)sw * 4u;
        ubyte*       out  = dst + (size_t)y * (size_t)dw * 4u;

        for (x = 0; x < dw; x++)
        {
            int x0 = x * 2;
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;

            for (c = 0; c < 4; c++)
            {
                int sum = row0[x0 * 4 + c] + row0[x1 * 4 + c] + row1[x0 * 4 + c] + row1[x1 * 4 + c];
                out[x * 4 + c] = (ubyte)((sum + 2) / 4);
            }
        }
    }
}

// Fills the chain: level 0 points at rgba, the levels below are
// written into storage back to back. storage may be NULL (one level).
static inline void mip_cpu__build(mip_cpu__chain_t* chain, const ubyte* rgba, int w, int h, ubyte* storage)
{
    int i = 0;

    chain->levels[0].pixels = rgba;
    chain->levels[0].w      = w;
    chain->levels[0].h      = h;
    chain->count            = 1;

    if (storage == NULL_POINTER)
    {
        return;
    }

    while ((chain->levels[i].w > 1 || chain->levels[i].h > 1) && chain->count < MIP_CPU__MAX_LEVELS)
    {
        const mip_cpu__level_t* src = &chain->levels[i];
        int                     dw  = src->w > 1 ? src->w / 2 : 1;
        int                     dh  = src->h > 1 ? src->h / 2 : 1;

        mip_cpu__downsample(src->pixels, src->w, src->h, storage, dw, dh);
        chain->levels[i + 1].pixels = storage;
        chain->levels[i + 1].w      = dw;
        chain->levels[i + 1].h      = dh;
        chain->count++;
        storage += (size_t)dw * (size_t)dh * 4u;
        i++;
    }
}

#endif
