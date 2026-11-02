// Private inlining boundary for hot CPU gradient samples. Public symbols remain
// in gradient.c. Both entry points use this single implementation, so color
// interpolation and hard-stop rules cannot drift between the two paths.
#ifndef MEEK_UI_GRADIENT_SAMPLE_INTERNAL_H
#define MEEK_UI_GRADIENT_SAMPLE_INTERNAL_H
#include "gradient.h"
#include "clib/stdlib.h"

static inline gui_color_t _gradient_internal__mix(gui_color_t a, gui_color_t b, float f)
{
    gui_color_t c;
    float pr = 0.0f;
    float pg = 0.0f;
    float pb = 0.0f;

    // Every output channel is assigned below, without clearing a temporary per sample.
    // a fully transparent end has no hue of its own (premultiplied, its
    // rgb never shows), so it takes the other end's hue: then the GPU's
    // straight interpolation between the two vertices of a band or ring
    // reproduces the premultiplied ramp exactly (`transparent` is black
    // with alpha 0; without this the fade would darken toward black)
    if (a.a <= 0.0f)
    {
        a.r = b.r;
        a.g = b.g;
        a.b = b.b;
    }

    if (b.a <= 0.0f)
    {
        b.r = a.r;
        b.g = a.g;
        b.b = a.b;
    }

    c.a = a.a + (b.a - a.a) * f;
    pr = a.r * a.a * (1.0f - f) + b.r * b.a * f;
    pg = a.g * a.a * (1.0f - f) + b.g * b.a * f;
    pb = a.b * a.a * (1.0f - f) + b.b * b.a * f;

    if (c.a > 0.00001f)
    {
        c.r = pr / c.a;
        c.g = pg / c.a;
        c.b = pb / c.a;
    }
    else
    {
        // fully transparent: the hue is invisible, keep a plain mix so a
        // neighbouring vertex interpolates toward something sensible
        c.r = a.r + (b.r - a.r) * f;
        c.g = a.g + (b.g - a.g) * f;
        c.b = a.b + (b.b - a.b) * f;
    }

    return c;
}

static inline gui_color_t _gradient_internal__sample_parameter(const gradient_sampler_t* s, float t)
{
    gui_color_t clear = {0.0f, 0.0f, 0.0f, 0.0f};
    gui_color_t color;
    float period = 0.0f;
    float span = 0.0f;
    float f = 0.0f;
    int i = 0;

    if (s == NULL_POINTER || s->count <= 0)
    {
        return clear;
    }
    if (s->repeating == TRUE)
    {
        period = s->pos[s->count - 1] - s->pos[0];
        if (period <= 0.000001f)
        {
            return s->color[s->count - 1];
        }
        t = s->pos[0] + (t - s->pos[0] - stdlib__floorf((t - s->pos[0]) / period) * period);
    }
    if (t < s->pos[0])
    {
        return s->color[0];
    }
    for (i = 1; i < s->count; i++)
    {
        if (t < s->pos[i])
        {
            span = s->pos[i] - s->pos[i - 1];
            f = (span > 0.0f) ? (t - s->pos[i - 1]) / span : 1.0f;
            if (s->premultiplied == TRUE)
            {
                color.r = s->color[i - 1].r * (1.0f - f) + s->color[i].r * f;
                color.g = s->color[i - 1].g * (1.0f - f) + s->color[i].g * f;
                color.b = s->color[i - 1].b * (1.0f - f) + s->color[i].b * f;
                color.a = s->color[i - 1].a + (s->color[i].a - s->color[i - 1].a) * f;
                return color;
            }
            return _gradient_internal__mix(s->color[i - 1], s->color[i], f);
        }
    }
    return s->color[s->count - 1];
}

#endif
