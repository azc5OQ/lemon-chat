// gradient.c - CSS gradient evaluation and tessellation, shared by every
// renderer. See gradient.h for the contract.
//
// Geometry (CSS Images 3):
// - linear: the gradient line passes through the rect's center at the
//   angle (0deg points up, angles turn clockwise, 180deg is the default
//   `to bottom`); its length is |w sin a| + |h cos a|, so the corners
//   farthest along and against the direction sit at 0 % and 100 %. `to
//   <corner>` picks the angle whose 50 % line passes through the two
//   other corners (the line is perpendicular to that diagonal).
// Every band and ring sector is clipped to the rect on the CPU, so a
// gradient rect covers exactly the pixels a solid rect covers (the
// rect shader's mask then rounds the corners for both alike).
// - radial: the ending shape is a circle or an ellipse centered at the
//   position, sized by the keyword (closest-side, farthest-side,
//   closest-corner, farthest-corner, the default) or explicitly; stop
//   positions run along the ray from the center to the right, 100 % at
//   the shape's horizontal radius; beyond the last stop the last color
//   continues.
// Color stops: the first and last default to 0 % and 100 %, a position
// smaller than an earlier one is raised to it, runs without a position
// spread evenly between their neighbours (the CSS fixup), colors mix in
// premultiplied sRGB.
#include "gradient.h"
#include "gradient_sample_internal.h"
#include "clib/stdlib.h"

#define _GRADIENT_INTERNAL__PI              3.14159265358979f
#define _GRADIENT_INTERNAL__MAX_RINGS       40
#define _GRADIENT_INTERNAL__MAX_SEGMENTS    64
#define _GRADIENT_INTERNAL__MIN_SEGMENTS    12
#define _GRADIENT_INTERNAL__RING_PX         6.0f   // target ring thickness in device pixels
#define _GRADIENT_INTERNAL__SEGMENT_PX      5.0f   // target arc length per segment at the outer ring
#define _GRADIENT_INTERNAL__SUB_BANDS_MAX   32     // sub-bands per interval of a linear gradient whose end alphas differ (scaled by the alpha change)
#define _GRADIENT_INTERNAL__MAX_POLY        12     // vertices of a band or ring sector after clipping to the rect (a quad gains at most one per edge)

// stops after the CSS fixup: positions as fractions of the gradient line
// (linear) or of the horizontal radius (radial), colors with the opacity
// applied
typedef struct _gradient_internal__stops_t
{
    int         count;                            // 0x0
    float       pos[GUI_GRADIENT_MAX_STOPS];      // 0x4
    gui_color_t color[GUI_GRADIENT_MAX_STOPS];    // 0x24
} _gradient_internal__stops_t;

// a convex polygon with a color per vertex: one band of a linear
// gradient or one ring sector of a radial one, before and after the
// clip to the rect
typedef struct _gradient_internal__poly_t
{
    int         count;                              // 0x0
    float       x[_GRADIENT_INTERNAL__MAX_POLY];    // 0x4
    float       y[_GRADIENT_INTERNAL__MAX_POLY];    // 0x34
    gui_color_t c[_GRADIENT_INTERNAL__MAX_POLY];    // 0x64
} _gradient_internal__poly_t;

static void        _gradient_internal__resolve_stops(const gui_gradient_t* g, float line_len, float scale, float opacity, _gradient_internal__stops_t* out);
static gui_color_t _gradient_internal__color_at(const _gradient_internal__stops_t* s, float t, boole from_left);
static gui_color_t _gradient_internal__color_wrapped(const _gradient_internal__stops_t* s, boole repeating, float t, boole from_left);
static void        _gradient_internal__poly_push(_gradient_internal__poly_t* p, float x, float y, gui_color_t c);
static void        _gradient_internal__poly_clip(const _gradient_internal__poly_t* in, _gradient_internal__poly_t* out, int axis, float bound, boole keep_greater);
static boole       _gradient_internal__emit_polygon(gradient_vertex_t* out, int64* n, int64 cap, const _gradient_internal__poly_t* p, gui_rect_t r);
static gui_color_t _gradient_internal__extrapolate(gui_color_t ca, gui_color_t cb, float a, float b, float t);
static int64       _gradient_internal__linear(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap);
static int64       _gradient_internal__radial(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap);
static float       _gradient_internal__corner_distance(float cx, float cy, float w, float h, boole farthest);
static void        _gradient_internal__ellipse_radii(const gui_gradient_t* g, float cx, float cy, float w, float h, float scale, float* rx, float* ry);
static float       _gradient_internal__lerp_f(float a, float b, float t);
static gui_color_t _gradient_internal__lerp_color(gui_color_t a, gui_color_t b, float t);
static int64 _gradient_internal__conic(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap);

void gradient__two_color(gui_gradient_t* out, gui_color_t from, gui_color_t to, gui_gradient_dir_e dir)
{
    if (out == NULL_POINTER)
    {
        return;
    }

    stdlib__null_memory(out, sizeof(*out));

    out->kind             = GUI_GRADIENT_KIND__LINEAR;
    out->angle_deg        = 180.0f;
    out->center_x         = 0.5f;
    out->center_y         = 0.5f;
    out->stop_count       = 2;
    out->stops[0].color   = from;
    out->stops[0].pos     = 0.0f;
    out->stops[0].has_pos = TRUE;
    out->stops[1].color   = to;
    out->stops[1].pos     = 1.0f;
    out->stops[1].has_pos = TRUE;

    if (dir == GUI_GRADIENT_DIR__HORIZONTAL)
    {
        out->angle_deg = 90.0f;
    }
    else if (dir == GUI_GRADIENT_DIR__DIAGONAL_TL)
    {
        out->corner_x = 1;
        out->corner_y = 1;
    }
    else if (dir == GUI_GRADIENT_DIR__DIAGONAL_TR)
    {
        out->corner_x = -1;
        out->corner_y = 1;
    }
}

void gradient__lerp(gui_gradient_t* out, const gui_gradient_t* a_in, const gui_gradient_t* b_in, float t)
{
    // copies, so the caller may pass the output as one of the inputs
    // (the animator interpolates a node's stored gradient in place)
    gui_gradient_t        ga;
    gui_gradient_t        gb;
    const gui_gradient_t* a = &ga;
    const gui_gradient_t* b = &gb;
    int                   i = 0;

    stdlib__null_memory(&ga, sizeof(ga));
    stdlib__null_memory(&gb, sizeof(gb));

    if (out == NULL_POINTER || a_in == NULL_POINTER || b_in == NULL_POINTER)
    {
        return;
    }

    ga = *a_in;
    gb = *b_in;

    if (a->kind != b->kind || a->stop_count != b->stop_count || a->circle != b->circle || a->size != b->size || a->corner_x != b->corner_x || a->corner_y != b->corner_y)
    {
        *out = (t < 0.5f) ? *a : *b;
        return;
    }

    *out = *b;

    out->angle_deg = _gradient_internal__lerp_f(a->angle_deg, b->angle_deg, t);

    if (a->center_x_px == b->center_x_px)
    {
        out->center_x = _gradient_internal__lerp_f(a->center_x, b->center_x, t);
    }

    if (a->center_y_px == b->center_y_px)
    {
        out->center_y = _gradient_internal__lerp_f(a->center_y, b->center_y, t);
    }

    if (a->radius_pct == b->radius_pct)
    {
        out->radius_x = _gradient_internal__lerp_f(a->radius_x, b->radius_x, t);
        out->radius_y = _gradient_internal__lerp_f(a->radius_y, b->radius_y, t);
    }

    for (i = 0; i < out->stop_count && i < GUI_GRADIENT_MAX_STOPS; i++)
    {
        out->stops[i].color = _gradient_internal__lerp_color(a->stops[i].color, b->stops[i].color, t);

        if (a->stops[i].has_pos == b->stops[i].has_pos && a->stops[i].pos_is_px == b->stops[i].pos_is_px)
        {
            out->stops[i].pos = _gradient_internal__lerp_f(a->stops[i].pos, b->stops[i].pos, t);
            out->stops[i].px_offset = _gradient_internal__lerp_f(a->stops[i].px_offset, b->stops[i].px_offset, t);
        }
    }
}

int64 gradient__tessellate(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap)
{
    if (g == NULL_POINTER || out == NULL_POINTER || cap <= 0)
    {
        return 0;
    }

    if (r.w <= 0.0f || r.h <= 0.0f || g->stop_count <= 0)
    {
        return 0;
    }

    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }

    if (g->kind == GUI_GRADIENT_KIND__CONIC)
    {
        return _gradient_internal__conic(g, r, scale, opacity, out, cap);
    }
    if (g->kind == GUI_GRADIENT_KIND__RADIAL)
    {
        return _gradient_internal__radial(g, r, scale, opacity, out, cap);
    }

    return _gradient_internal__linear(g, r, scale, opacity, out, cap);
}

static float _gradient_internal__lerp_f(float a, float b, float t)
{
    return a + (b - a) * t;
}

/**
 * @brief prepare CSS gradient geometry and fixed-up stops for repeated sampling
 *
 * @param const gui_gradient_t* g -> parsed CSS gradient
 * @param float width -> image width in device pixels
 * @param float height -> image height in device pixels
 * @param float scale -> logical-to-device scale for CSS lengths
 * @param gradient_sampler_t* out -> reusable prepared gradient
 * @return void
 */
void gradient__prepare(const gui_gradient_t* g, float width, float height, float scale, gradient_sampler_t* out)
{
    _gradient_internal__stops_t stops;
    float norm = 0.0f;
    int i = 0;

    stdlib__null_memory(&stops, sizeof(stops));
    if (out == NULL_POINTER)
    {
        return;
    }
    stdlib__null_memory(out, sizeof(*out));
    if (g == NULL_POINTER || width <= 0.0f || height <= 0.0f)
    {
        return;
    }
    out->kind = g->kind;
    out->repeating = g->repeating;
    out->angle = g->angle_deg / 360.0f;
    out->cx = (g->center_x_px == TRUE) ? g->center_x * scale : g->center_x * width;
    out->cy = (g->center_y_px == TRUE) ? g->center_y * scale : g->center_y * height;
    out->line = 1.0f;
    if (g->kind == GUI_GRADIENT_KIND__LINEAR)
    {
        out->cx = width * 0.5f;
        out->cy = height * 0.5f;
        out->dx = stdlib__sinf(g->angle_deg * _GRADIENT_INTERNAL__PI / 180.0f);
        out->dy = -stdlib__cosf(g->angle_deg * _GRADIENT_INTERNAL__PI / 180.0f);
        if (g->corner_x != 0 || g->corner_y != 0)
        {
            out->dx = (float)g->corner_x * ((g->corner_y != 0) ? height : 1.0f);
            out->dy = (float)g->corner_y * ((g->corner_x != 0) ? width : 1.0f);
        }
        norm = stdlib__sqrtf(out->dx * out->dx + out->dy * out->dy);
        if (norm > 0.0f)
        {
            out->dx /= norm;
            out->dy /= norm;
        }
        out->line = stdlib__fabsf(width * out->dx) + stdlib__fabsf(height * out->dy);
    }
    else if (g->kind == GUI_GRADIENT_KIND__RADIAL)
    {
        _gradient_internal__ellipse_radii(g, out->cx, out->cy, width, height, scale, &out->rx, &out->ry);
        out->rx = stdlib__fmaxf(out->rx, 0.0001f);
        out->ry = stdlib__fmaxf(out->ry, 0.0001f);
        out->line = out->rx;
    }
    _gradient_internal__resolve_stops(g, out->line, scale, 1.0f, &stops);
    out->count = stops.count;
    for (i = 0; i < stops.count; i++)
    {
        out->pos[i] = stops.pos[i];
        out->color[i] = stops.color[i];
    }
}

/**
 * @brief evaluate a prepared gradient at one image-space point
 *
 * @param const gradient_sampler_t* s -> prepared geometry and stops
 * @param float x -> horizontal sample position in device pixels
 * @param float y -> vertical sample position in device pixels
 * @return gui_color_t -> straight-alpha sRGB color, interpolated in premultiplied space
 */
gui_color_t gradient__sample(const gradient_sampler_t* s, float x, float y)
{
    gui_color_t clear;
    float t = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;

    if (s == NULL_POINTER || s->count <= 0)
    {
        stdlib__null_memory(&clear, sizeof(clear));
        return clear;
    }
    dx = x - s->cx;
    dy = y - s->cy;
    if (s->kind == GUI_GRADIENT_KIND__CONIC)
    {
        t = stdlib__atan2f(dx, -dy) / (2.0f * _GRADIENT_INTERNAL__PI) - s->angle;
        t -= stdlib__floorf(t);
    }
    else if (s->kind == GUI_GRADIENT_KIND__RADIAL)
    {
        dx /= s->rx;
        dy /= s->ry;
        t = stdlib__sqrtf(dx * dx + dy * dy);
    }
    else
    {
        t = 0.5f + (dx * s->dx + dy * s->dy) / stdlib__fmaxf(s->line, 0.0001f);
    }
    return _gradient_internal__sample_parameter(s, t);
}

/**
 * @brief shade the color ramp independently of its geometric coordinate
 * @param const gradient_sampler_t* s -> prepared gradient
 * @param float t -> geometric parameter, already wrapped for a conic
 * @return gui_color_t -> straight RGBA sample
 */
gui_color_t gradient__sample_parameter(const gradient_sampler_t* s, float t)
{
    return _gradient_internal__sample_parameter(s, t);
}

/**
 * @brief tessellate a conic gradient as narrow angular wedges, split at hard stops
 *
 * @param const gui_gradient_t* g -> parsed conic gradient
 * @param gui_rect_t r -> painted rectangle
 * @param float scale -> CSS length scale
 * @param float opacity -> inherited opacity
 * @param gradient_vertex_t* out -> destination vertices
 * @param int64 cap -> destination capacity
 * @return int64 -> emitted vertex count
 */
static int64 _gradient_internal__conic(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap)
{
    gradient_sampler_t sampler;
    _gradient_internal__poly_t poly;
    gui_color_t ca;
    gui_color_t cb;
    float boundaries[1024];
    float t = 0.0f;
    float key = 0.0f;
    float radius = 0.0f;
    float angle_a = 0.0f;
    float angle_b = 0.0f;
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    float period = 0.0f;
    int count = 0;
    int i = 0;
    int j = 0;
    int cycle = 0;
    int cycles = 1;
    int64 result = 0;

    stdlib__null_memory(&sampler, sizeof(sampler));
    stdlib__null_memory(&poly, sizeof(poly));
    stdlib__null_memory(&ca, sizeof(ca));
    stdlib__null_memory(&cb, sizeof(cb));
    stdlib__null_memory(boundaries, sizeof(boundaries));
    gradient__prepare(g, r.w, r.h, scale, &sampler);
    radius = _gradient_internal__corner_distance(sampler.cx, sampler.cy, r.w, r.h, TRUE) + 2.0f;
    for (i = 0; i <= 360; i++)
    {
        boundaries[count++] = (float)i / 360.0f;
    }
    period = sampler.pos[sampler.count - 1] - sampler.pos[0];
    if (g->repeating == TRUE && period > 0.00001f)
    {
        cycles = (int)stdlib__ceilf(1.0f / period) + 2;
    }
    for (cycle = -1; cycle < cycles && count + sampler.count < 1024; cycle++)
    {
        for (i = 0; i < sampler.count; i++)
        {
            t = sampler.pos[i] + ((g->repeating == TRUE) ? (float)cycle * period : 0.0f);
            if (t > 0.0f && t < 1.0f)
            {
                boundaries[count++] = t;
            }
        }
        if (g->repeating == FALSE)
        {
            break;
        }
    }
    for (i = 1; i < count; i++)
    {
        key = boundaries[i];
        j = i - 1;
        while (j >= 0 && boundaries[j] > key)
        {
            boundaries[j + 1] = boundaries[j];
            j--;
        }
        boundaries[j + 1] = key;
    }
    for (i = 0; i + 1 < count; i++)
    {
        if (boundaries[i + 1] - boundaries[i] < 0.000001f)
        {
            continue;
        }
        angle_a = (boundaries[i] + sampler.angle) * 2.0f * _GRADIENT_INTERNAL__PI;
        angle_b = (boundaries[i + 1] + sampler.angle) * 2.0f * _GRADIENT_INTERNAL__PI;
        ax = stdlib__sinf(angle_a);
        ay = -stdlib__cosf(angle_a);
        bx = stdlib__sinf(angle_b);
        by = -stdlib__cosf(angle_b);
        ca = gradient__sample(&sampler, sampler.cx + stdlib__sinf(angle_a + 0.000001f), sampler.cy - stdlib__cosf(angle_a + 0.000001f));
        cb = gradient__sample(&sampler, sampler.cx + stdlib__sinf(angle_b - 0.000001f), sampler.cy - stdlib__cosf(angle_b - 0.000001f));
        ca.a *= opacity;
        cb.a *= opacity;
        poly.count = 0;
        _gradient_internal__poly_push(&poly, r.x + sampler.cx, r.y + sampler.cy, _gradient_internal__mix(ca, cb, 0.5f));
        _gradient_internal__poly_push(&poly, r.x + sampler.cx + radius * ax, r.y + sampler.cy + radius * ay, ca);
        _gradient_internal__poly_push(&poly, r.x + sampler.cx + radius * bx, r.y + sampler.cy + radius * by, cb);
        if (_gradient_internal__emit_polygon(out, &result, cap, &poly, r) == FALSE)
        {
            return result;
        }
    }
    return result;
}

static gui_color_t _gradient_internal__lerp_color(gui_color_t a, gui_color_t b, float t)
{
    gui_color_t c;

    stdlib__null_memory(&c, sizeof(c));

    c.r = _gradient_internal__lerp_f(a.r, b.r, t);
    c.g = _gradient_internal__lerp_f(a.g, b.g, t);
    c.b = _gradient_internal__lerp_f(a.b, b.b, t);
    c.a = _gradient_internal__lerp_f(a.a, b.a, t);

    return c;
}

// the CSS color stop fixup; pixel positions become fractions of
// line_len (the gradient line for linear, the horizontal radius for
// radial gradients)
static void _gradient_internal__resolve_stops(const gui_gradient_t* g, float line_len, float scale, float opacity, _gradient_internal__stops_t* out)
{
    int   i    = 0;
    int   j    = 0;
    int   k    = 0;
    float prev = 0.0f;
    float span = 0.0f;
    boole has[GUI_GRADIENT_MAX_STOPS];

    stdlib__null_memory(out, sizeof(*out));
    stdlib__null_memory(has, sizeof(has));

    out->count = g->stop_count;

    if (out->count > GUI_GRADIENT_MAX_STOPS)
    {
        out->count = GUI_GRADIENT_MAX_STOPS;
    }

    for (i = 0; i < out->count; i++)
    {
        out->color[i]    = g->stops[i].color;
        out->color[i].a *= opacity;
        has[i]           = g->stops[i].has_pos;
        out->pos[i]      = g->stops[i].pos;

        if (has[i] == TRUE && g->stops[i].pos_is_px == TRUE)
        {
            out->pos[i] = (line_len > 0.0f) ? g->stops[i].pos * scale / line_len : 0.0f;
        }
        if (has[i] == TRUE && line_len > 0.0f)
        {
            out->pos[i] += g->stops[i].px_offset * scale / line_len;
        }
    }

    if (out->count <= 0)
    {
        return;
    }

    // the first and last stop default to the ends
    if (has[0] == FALSE)
    {
        out->pos[0] = 0.0f;
        has[0]      = TRUE;
    }

    if (has[out->count - 1] == FALSE)
    {
        out->pos[out->count - 1] = 1.0f;
        has[out->count - 1]      = TRUE;
    }

    // a position smaller than any specified before it is raised to the
    // largest one before it, which keeps the list monotonic
    prev = out->pos[0];

    for (i = 1; i < out->count; i++)
    {
        if (has[i] == FALSE)
        {
            continue;
        }

        if (out->pos[i] < prev)
        {
            out->pos[i] = prev;
        }

        prev = out->pos[i];
    }

    // runs without a position spread evenly between their neighbours
    for (i = 1; i < out->count; i++)
    {
        if (has[i] == TRUE)
        {
            continue;
        }

        j = i;

        while (j < out->count && has[j] == FALSE)
        {
            j++;
        }

        span = out->pos[j] - out->pos[i - 1];

        for (k = i; k < j; k++)
        {
            out->pos[k] = out->pos[i - 1] + span * (float)(k - i + 1) / (float)(j - i + 1);
            has[k]      = TRUE;
        }

        i = j;
    }
}

// the color at position t. At a hard stop (two stops sharing a
// position) the color jumps; from_left picks the color just before the
// jump, which the end of the band before it needs, FALSE the color just
// after, for the start of the band that follows.
static gui_color_t _gradient_internal__color_at(const _gradient_internal__stops_t* s, float t, boole from_left)
{
    int   i      = 0;
    float span   = 0.0f;
    float f      = 0.0f;
    boole inside = FALSE;
    gui_color_t none;

    stdlib__null_memory(&none, sizeof(none));

    if (s->count <= 0)
    {
        return none;
    }

    if (from_left == TRUE)
    {
        if (t <= s->pos[0])
        {
            return s->color[0];
        }

        if (t > s->pos[s->count - 1])
        {
            return s->color[s->count - 1];
        }
    }
    else
    {
        if (t < s->pos[0])
        {
            return s->color[0];
        }

        if (t >= s->pos[s->count - 1])
        {
            return s->color[s->count - 1];
        }
    }

    for (i = 0; i + 1 < s->count; i++)
    {
        inside = (from_left == TRUE) ? (boole)(t <= s->pos[i + 1]) : (boole)(t < s->pos[i + 1]);

        if (inside == FALSE)
        {
            continue;
        }

        span = s->pos[i + 1] - s->pos[i];

        if (span > 0.0f)
        {
            f = (t - s->pos[i]) / span;
        }
        else
        {
            f = (from_left == TRUE) ? 0.0f : 1.0f;
        }

        return _gradient_internal__mix(s->color[i], s->color[i + 1], f);
    }

    return s->color[s->count - 1];
}

static void _gradient_internal__poly_push(_gradient_internal__poly_t* p, float x, float y, gui_color_t c)
{
    if (p->count >= _GRADIENT_INTERNAL__MAX_POLY)
    {
        return;
    }

    p->x[p->count] = x;
    p->y[p->count] = y;
    p->c[p->count] = c;
    p->count++;
}

// Sutherland-Hodgman against one half-plane: axis 0 is x, 1 is y;
// keep_greater keeps the side at or beyond `bound`. A vertex made on
// the boundary gets the color the GPU would have interpolated there,
// so the clipped polygon paints exactly what the unclipped one would
// have inside the rect.
static void _gradient_internal__poly_clip(const _gradient_internal__poly_t* in, _gradient_internal__poly_t* out, int axis, float bound, boole keep_greater)
{
    int   i    = 0;
    int   j    = 0;
    float vi   = 0.0f;
    float vj   = 0.0f;
    float s    = 0.0f;
    boole in_i = FALSE;
    boole in_j = FALSE;

    out->count = 0;

    for (i = 0; i < in->count; i++)
    {
        j    = (i + 1) % in->count;
        vi   = (axis == 0) ? in->x[i] : in->y[i];
        vj   = (axis == 0) ? in->x[j] : in->y[j];
        in_i = (keep_greater == TRUE) ? (boole)(vi >= bound) : (boole)(vi <= bound);
        in_j = (keep_greater == TRUE) ? (boole)(vj >= bound) : (boole)(vj <= bound);

        if (in_i == TRUE)
        {
            _gradient_internal__poly_push(out, in->x[i], in->y[i], in->c[i]);
        }

        if (in_i != in_j)
        {
            s = (bound - vi) / (vj - vi);
            _gradient_internal__poly_push(out, in->x[i] + (in->x[j] - in->x[i]) * s, in->y[i] + (in->y[j] - in->y[i]) * s, _gradient_internal__lerp_color(in->c[i], in->c[j], s));
        }
    }
}

// clips the polygon to the rect and writes it as a triangle fan;
// FALSE when the buffer is full
static boole _gradient_internal__emit_polygon(gradient_vertex_t* out, int64* n, int64 cap, const _gradient_internal__poly_t* p, gui_rect_t r)
{
    _gradient_internal__poly_t a;
    _gradient_internal__poly_t b;
    int k = 0;

    stdlib__null_memory(&a, sizeof(a));
    stdlib__null_memory(&b, sizeof(b));

    _gradient_internal__poly_clip(p, &a, 0, r.x, TRUE);
    _gradient_internal__poly_clip(&a, &b, 0, r.x + r.w, FALSE);
    _gradient_internal__poly_clip(&b, &a, 1, r.y, TRUE);
    _gradient_internal__poly_clip(&a, &b, 1, r.y + r.h, FALSE);

    if (b.count < 3)
    {
        return TRUE;
    }

    if (*n + 3 * (int64)(b.count - 2) > cap)
    {
        return FALSE;
    }

    for (k = 1; k + 1 < b.count; k++)
    {
        out[*n].x = b.x[0];
        out[*n].y = b.y[0];
        out[*n].c = b.c[0];
        *n        = *n + 1;
        out[*n].x = b.x[k];
        out[*n].y = b.y[k];
        out[*n].c = b.c[k];
        *n        = *n + 1;
        out[*n].x = b.x[k + 1];
        out[*n].y = b.y[k + 1];
        out[*n].c = b.c[k + 1];
        *n        = *n + 1;
    }

    return TRUE;
}

// the band's straight ramp continued past its end: the color a vertex
// placed at t (outside a..b) must carry so the ramp inside a..b stays
// exact
static gui_color_t _gradient_internal__extrapolate(gui_color_t ca, gui_color_t cb, float a, float b, float t)
{
    float s = (b > a) ? (t - a) / (b - a) : 0.0f;

    return _gradient_internal__lerp_color(ca, cb, s);
}

/**
 * @brief the color at a position of a repeating gradient: the position wrapped into the first cycle
 * @param const _gradient_internal__stops_t* s -> resolved stops
 * @param boole repeating -> whether the stops recur every period (last position minus first)
 * @param float t -> position in line units
 * @param boole from_left -> the color just before a hard stop (a cycle start seen from the left is the cycle end)
 * @return gui_color_t -> straight color
 */
static gui_color_t _gradient_internal__color_wrapped(const _gradient_internal__stops_t* s, boole repeating, float t, boole from_left)
{
    float first = 0.0f;
    float period = 0.0f;
    float wrapped = t;

    if (repeating == TRUE && s->count > 1)
    {
        first = s->pos[0];
        period = s->pos[s->count - 1] - first;
        if (period > 0.00001f)
        {
            // the wrap's float error must not cross a hard stop: the
            // position moves a millionth of the line towards the side the
            // lookup wants first (the conic tessellator offsets its angle
            // the same way)
            wrapped = t + ((from_left == TRUE) ? -0.000001f : 0.000001f);
            wrapped = wrapped - stdlib__floorf((wrapped - first) / period) * period;
        }
    }
    return _gradient_internal__color_at(s, wrapped, from_left);
}

static int64 _gradient_internal__linear(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap)
{
    _gradient_internal__stops_t stops;
    _gradient_internal__poly_t  poly;
    float       bounds[1024];
    int         nb     = 0;
    int         i      = 0;
    int         j      = 0;
    int         sub    = 0;
    int         by_px  = 0;
    int         cycle  = 0;
    int         cycles = 1;
    int64       n      = 0;
    float       period = 0.0f;
    float       t      = 0.0f;
    float       dx     = 0.0f;
    float       dy     = 0.0f;
    float       len    = 0.0f;
    float       line   = 0.0f;
    float       cx     = 0.0f;
    float       cy     = 0.0f;
    float       nx     = 0.0f;
    float       ny     = 0.0f;
    float       ext    = 0.0f;
    float       margin = 0.0f;
    float       t0     = 0.0f;
    float       t1     = 0.0f;
    float       a      = 0.0f;
    float       b      = 0.0f;
    float       ga     = 0.0f;
    float       gb     = 0.0f;
    float       ax     = 0.0f;
    float       ay     = 0.0f;
    float       bx     = 0.0f;
    float       by     = 0.0f;
    float       rad    = 0.0f;
    gui_color_t ca;
    gui_color_t cb;
    gui_color_t cga;
    gui_color_t cgb;

    stdlib__null_memory(&stops, sizeof(stops));
    stdlib__null_memory(&poly, sizeof(poly));
    stdlib__null_memory(bounds, sizeof(bounds));
    stdlib__null_memory(&ca, sizeof(ca));
    stdlib__null_memory(&cb, sizeof(cb));
    stdlib__null_memory(&cga, sizeof(cga));
    stdlib__null_memory(&cgb, sizeof(cgb));

    // the direction of the gradient line, y down: 0deg points up, 90deg right
    if (g->corner_x != 0 || g->corner_y != 0)
    {
        if (g->corner_x != 0 && g->corner_y != 0)
        {
            // to a corner: perpendicular to the diagonal joining the other two corners
            dx = (float)g->corner_x * r.h;
            dy = (float)g->corner_y * r.w;
        }
        else
        {
            dx = (float)g->corner_x;
            dy = (float)g->corner_y;
        }
    }
    else
    {
        rad = g->angle_deg * _GRADIENT_INTERNAL__PI / 180.0f;
        dx  = stdlib__sinf(rad);
        dy  = -stdlib__cosf(rad);
    }

    len = stdlib__sqrtf(dx * dx + dy * dy);

    if (len <= 0.0f)
    {
        return 0;
    }

    dx   = dx / len;
    dy   = dy / len;
    line = stdlib__fabsf(r.w * dx) + stdlib__fabsf(r.h * dy);

    if (line <= 0.0f)
    {
        return 0;
    }

    _gradient_internal__resolve_stops(g, line, scale, opacity, &stops);

    // bands: the rect's extent along the line is 0..1, every stop inside it
    // starts a new band; a repeating gradient's stops recur every period
    // (the cycles come out in increasing order, so no sort is needed)
    period = stops.pos[stops.count - 1] - stops.pos[0];

    if (g->repeating == TRUE && period > 0.00001f)
    {
        cycles = (int)stdlib__ceilf(1.0f / period) + 2;
    }

    bounds[nb++] = 0.0f;

    for (cycle = -1; cycle < cycles && nb + stops.count < 1023; cycle++)
    {
        for (i = 0; i < stops.count; i++)
        {
            t = stops.pos[i] + ((g->repeating == TRUE) ? (float)cycle * period : 0.0f);

            if (t > 0.0f && t < 1.0f)
            {
                bounds[nb++] = t;
            }
        }

        if (g->repeating == FALSE)
        {
            break;
        }
    }

    bounds[nb++] = 1.0f;

    cx     = r.x + r.w * 0.5f;
    cy     = r.y + r.h * 0.5f;
    nx     = -dy;
    ny     = dx;
    ext    = 0.5f * stdlib__sqrtf(r.w * r.w + r.h * r.h) + 1.0f;
    margin = 1.0f / line;

    for (i = 0; i + 1 < nb; i++)
    {
        t0 = bounds[i];
        t1 = bounds[i + 1];

        if (t1 <= t0)
        {
            continue;
        }

        // the GPU interpolates straight; where the alpha changes the
        // premultiplied ramp bends, so the interval is cut into pieces
        // in proportion to the alpha change
        ca  = _gradient_internal__color_wrapped(&stops, g->repeating, t0, FALSE);
        cb  = _gradient_internal__color_wrapped(&stops, g->repeating, t1, TRUE);
        sub = (int)stdlib__ceilf(stdlib__fabsf(ca.a - cb.a) * (float)_GRADIENT_INTERNAL__SUB_BANDS_MAX);

        // a piece thinner than a device pixel cannot be seen
        by_px = (int)stdlib__ceilf((t1 - t0) * line);

        if (by_px < 1)
        {
            by_px = 1;
        }

        if (sub > by_px)
        {
            sub = by_px;
        }

        if (sub < 1)
        {
            sub = 1;
        }

        if (sub > _GRADIENT_INTERNAL__SUB_BANDS_MAX)
        {
            sub = _GRADIENT_INTERNAL__SUB_BANDS_MAX;
        }

        for (j = 0; j < sub; j++)
        {
            a  = t0 + (t1 - t0) * (float)j / (float)sub;
            b  = t0 + (t1 - t0) * (float)(j + 1) / (float)sub;
            ca = _gradient_internal__color_wrapped(&stops, g->repeating, a, (j == 0) ? FALSE : TRUE);
            cb = _gradient_internal__color_wrapped(&stops, g->repeating, b, (j == sub - 1) ? TRUE : FALSE);

            // the outermost bands reach one pixel past the rect (the clip
            // trims them to the edge, whatever the rounding at the corners)
            // with the ramp continued so the colors inside stay exact
            ga  = (i == 0 && j == 0) ? a - margin : a;
            gb  = (i == nb - 2 && j == sub - 1) ? b + margin : b;
            cga = _gradient_internal__extrapolate(ca, cb, a, b, ga);
            cgb = _gradient_internal__extrapolate(ca, cb, a, b, gb);
            ax  = cx + dx * (ga - 0.5f) * line;
            ay  = cy + dy * (ga - 0.5f) * line;
            bx  = cx + dx * (gb - 0.5f) * line;
            by  = cy + dy * (gb - 0.5f) * line;

            poly.count = 0;
            _gradient_internal__poly_push(&poly, ax - nx * ext, ay - ny * ext, cga);
            _gradient_internal__poly_push(&poly, ax + nx * ext, ay + ny * ext, cga);
            _gradient_internal__poly_push(&poly, bx + nx * ext, by + ny * ext, cgb);
            _gradient_internal__poly_push(&poly, bx - nx * ext, by - ny * ext, cgb);

            if (_gradient_internal__emit_polygon(out, &n, cap, &poly, r) == FALSE)
            {
                return n;
            }
        }
    }

    return n;
}

static float _gradient_internal__corner_distance(float cx, float cy, float w, float h, boole farthest)
{
    float kx[4];
    float ky[4];
    float d    = 0.0f;
    float best = 0.0f;
    int   i    = 0;

    stdlib__null_memory(kx, sizeof(kx));
    stdlib__null_memory(ky, sizeof(ky));

    kx[1] = w;
    kx[3] = w;
    ky[2] = h;
    ky[3] = h;

    for (i = 0; i < 4; i++)
    {
        d = stdlib__sqrtf((kx[i] - cx) * (kx[i] - cx) + (ky[i] - cy) * (ky[i] - cy));

        if (i == 0 || (farthest == TRUE && d > best) || (farthest == FALSE && d < best))
        {
            best = d;
        }
    }

    return best;
}

// the ending shape's radii per the CSS size rules; cx / cy are the center
// relative to the rect
static void _gradient_internal__ellipse_radii(const gui_gradient_t* g, float cx, float cy, float w, float h, float scale, float* rx, float* ry)
{
    float dl     = stdlib__fabsf(cx);
    float dr     = stdlib__fabsf(w - cx);
    float dt     = stdlib__fabsf(cy);
    float db     = stdlib__fabsf(h - cy);
    float sx_min = (dl < dr) ? dl : dr;
    float sx_max = (dl > dr) ? dl : dr;
    float sy_min = (dt < db) ? dt : db;
    float sy_max = (dt > db) ? dt : db;
    float side_x = 0.0f;
    float side_y = 0.0f;
    float q      = 1.0f;
    float cand   = 0.0f;
    float best   = 0.0f;
    float ddx    = 0.0f;
    float ddy    = 0.0f;
    float kx[4];
    float ky[4];
    int   i      = 0;
    boole far    = FALSE;

    stdlib__null_memory(kx, sizeof(kx));
    stdlib__null_memory(ky, sizeof(ky));

    if (g->size == GUI_GRADIENT_SIZE__EXPLICIT)
    {
        *rx = (g->radius_pct == TRUE) ? g->radius_x / 100.0f * w : g->radius_x * scale;
        *ry = (g->circle == TRUE) ? *rx : ((g->radius_pct == TRUE) ? g->radius_y / 100.0f * h : g->radius_y * scale);
        return;
    }

    if (g->circle == TRUE)
    {
        if (g->size == GUI_GRADIENT_SIZE__CLOSEST_SIDE)
        {
            *rx = (sx_min < sy_min) ? sx_min : sy_min;
        }
        else if (g->size == GUI_GRADIENT_SIZE__FARTHEST_SIDE)
        {
            *rx = (sx_max > sy_max) ? sx_max : sy_max;
        }
        else
        {
            *rx = _gradient_internal__corner_distance(cx, cy, w, h, (boole)(g->size == GUI_GRADIENT_SIZE__FARTHEST_CORNER));
        }

        *ry = *rx;
        return;
    }

    if (g->size == GUI_GRADIENT_SIZE__CLOSEST_SIDE)
    {
        *rx = sx_min;
        *ry = sy_min;
        return;
    }

    if (g->size == GUI_GRADIENT_SIZE__FARTHEST_SIDE)
    {
        *rx = sx_max;
        *ry = sy_max;
        return;
    }

    // closest-corner / farthest-corner: the aspect ratio of the matching
    // side rule, scaled so the ellipse passes through that corner
    far    = (boole)(g->size == GUI_GRADIENT_SIZE__FARTHEST_CORNER);
    side_x = (far == TRUE) ? sx_max : sx_min;
    side_y = (far == TRUE) ? sy_max : sy_min;
    q      = (side_x > 0.001f && side_y > 0.001f) ? side_y / side_x : 1.0f;
    kx[1]  = w;
    kx[3]  = w;
    ky[2]  = h;
    ky[3]  = h;

    for (i = 0; i < 4; i++)
    {
        ddx  = kx[i] - cx;
        ddy  = (ky[i] - cy) / q;
        cand = stdlib__sqrtf(ddx * ddx + ddy * ddy);

        if (i == 0 || (far == TRUE && cand > best) || (far == FALSE && cand < best))
        {
            best = cand;
        }
    }

    *rx = best;
    *ry = best * q;
}

static int64 _gradient_internal__radial(const gui_gradient_t* g, gui_rect_t r, float scale, float opacity, gradient_vertex_t* out, int64 cap)
{
    _gradient_internal__stops_t stops;
    _gradient_internal__poly_t  poly;
    float       base[GUI_GRADIENT_MAX_STOPS + 2];
    float       ring[_GRADIENT_INTERNAL__MAX_RINGS + 1];
    float       cs[_GRADIENT_INTERNAL__MAX_SEGMENTS + 1];
    float       sn[_GRADIENT_INTERNAL__MAX_SEGMENTS + 1];
    int         nbase    = 0;
    int         nring    = 0;
    int         rings    = 0;
    int         segs     = 0;
    int         pieces   = 0;
    int         by_a     = 0;
    int         by_px    = 0;
    int         i        = 0;
    int         j        = 0;
    int         p        = 0;
    int64       n        = 0;
    float       cx       = 0.0f;
    float       cy       = 0.0f;
    float       rx       = 0.0f;
    float       ry       = 0.0f;
    float       rmin     = 0.0f;
    float       rmax     = 0.0f;
    float       tmax     = 0.0f;
    float       t        = 0.0f;
    float       step     = 0.0f;
    float       ta       = 0.0f;
    float       tb       = 0.0f;
    float       ox       = 0.0f;
    float       oy       = 0.0f;
    float       kx[4];
    float       ky[4];
    gui_color_t ca;
    gui_color_t cb;

    stdlib__null_memory(&stops, sizeof(stops));
    stdlib__null_memory(&poly, sizeof(poly));
    stdlib__null_memory(base, sizeof(base));
    stdlib__null_memory(ring, sizeof(ring));
    stdlib__null_memory(cs, sizeof(cs));
    stdlib__null_memory(sn, sizeof(sn));
    stdlib__null_memory(kx, sizeof(kx));
    stdlib__null_memory(ky, sizeof(ky));
    stdlib__null_memory(&ca, sizeof(ca));
    stdlib__null_memory(&cb, sizeof(cb));

    cx = (g->center_x_px == TRUE) ? g->center_x * scale : g->center_x * r.w;
    cy = (g->center_y_px == TRUE) ? g->center_y * scale : g->center_y * r.h;

    _gradient_internal__ellipse_radii(g, cx, cy, r.w, r.h, scale, &rx, &ry);

    // a degenerate shape paints the last color everywhere: tiny radii
    // put every pixel past the last stop
    if (rx < 0.001f)
    {
        rx = 0.001f;
    }

    if (ry < 0.001f)
    {
        ry = 0.001f;
    }

    _gradient_internal__resolve_stops(g, rx, scale, opacity, &stops);

    // the rings stop at the rect corner farthest from the center, in
    // ending-shape units, plus one pixel
    kx[1] = r.w;
    kx[3] = r.w;
    ky[2] = r.h;
    ky[3] = r.h;
    rmin  = (rx < ry) ? rx : ry;
    rmax  = (rx > ry) ? rx : ry;

    for (i = 0; i < 4; i++)
    {
        t = stdlib__sqrtf(((kx[i] - cx) / rx) * ((kx[i] - cx) / rx) + ((ky[i] - cy) / ry) * ((ky[i] - cy) / ry));

        if (t > tmax)
        {
            tmax = t;
        }
    }

    tmax = tmax + 1.0f / rmin;

    // past a fully transparent last stop nothing is visible: the rings
    // end there (a repeating gradient goes on)
    if (g->repeating == FALSE && stops.count > 0 && stops.color[stops.count - 1].a <= 0.0f && stops.pos[stops.count - 1] < tmax)
    {
        tmax = stops.pos[stops.count - 1];
    }

    if (tmax <= 0.0f)
    {
        return 0;
    }

    // ring boundaries: the stops inside the range, subdivided to the
    // target thickness; segments by the outer ring's circumference
    base[nbase++] = 0.0f;

    for (i = 0; i < stops.count; i++)
    {
        if (stops.pos[i] > 0.0f && stops.pos[i] < tmax)
        {
            base[nbase++] = stops.pos[i];
        }
    }

    base[nbase++] = tmax;

    rings = (int)stdlib__ceilf(tmax * rmax / _GRADIENT_INTERNAL__RING_PX);

    if (rings < 4)
    {
        rings = 4;
    }

    if (rings > _GRADIENT_INTERNAL__MAX_RINGS)
    {
        rings = _GRADIENT_INTERNAL__MAX_RINGS;
    }

    step = tmax / (float)rings;

    for (i = 0; i + 1 < nbase; i++)
    {
        if (base[i + 1] <= base[i])
        {
            continue;
        }

        pieces = (int)stdlib__ceilf((base[i + 1] - base[i]) / step);
        ca     = _gradient_internal__color_at(&stops, base[i], FALSE);
        cb     = _gradient_internal__color_at(&stops, base[i + 1], TRUE);
        by_a   = (int)stdlib__ceilf(stdlib__fabsf(ca.a - cb.a) * (float)_GRADIENT_INTERNAL__SUB_BANDS_MAX);

        // where the hue and the alpha both change, the straight ramp
        // between two rings bends away from the premultiplied one, so
        // such an interval gets more rings, like a linear band (never
        // thinner than a device pixel)
        by_px = (int)stdlib__ceilf((base[i + 1] - base[i]) * rmax);

        if (by_a > by_px)
        {
            by_a = by_px;
        }

        if (by_a > pieces)
        {
            pieces = by_a;
        }

        if (pieces < 1)
        {
            pieces = 1;
        }

        if (nring + pieces > _GRADIENT_INTERNAL__MAX_RINGS - (nbase - 2 - i))
        {
            pieces = _GRADIENT_INTERNAL__MAX_RINGS - (nbase - 2 - i) - nring;
        }

        if (pieces < 1)
        {
            pieces = 1;
        }

        for (p = 0; p < pieces; p++)
        {
            ring[nring++] = base[i] + (base[i + 1] - base[i]) * (float)p / (float)pieces;
        }
    }

    ring[nring] = tmax;

    segs = (int)(2.0f * _GRADIENT_INTERNAL__PI * tmax * rmax / _GRADIENT_INTERNAL__SEGMENT_PX);

    if (segs < _GRADIENT_INTERNAL__MIN_SEGMENTS)
    {
        segs = _GRADIENT_INTERNAL__MIN_SEGMENTS;
    }

    if (segs > _GRADIENT_INTERNAL__MAX_SEGMENTS)
    {
        segs = _GRADIENT_INTERNAL__MAX_SEGMENTS;
    }

    for (j = 0; j <= segs; j++)
    {
        cs[j] = stdlib__cosf(2.0f * _GRADIENT_INTERNAL__PI * (float)j / (float)segs);
        sn[j] = stdlib__sinf(2.0f * _GRADIENT_INTERNAL__PI * (float)j / (float)segs);
    }

    ox = r.x + cx;
    oy = r.y + cy;

    for (i = 0; i < nring; i++)
    {
        ta = ring[i];
        tb = ring[i + 1];

        if (tb <= ta)
        {
            continue;
        }

        ca = _gradient_internal__color_at(&stops, ta, FALSE);
        cb = _gradient_internal__color_at(&stops, tb, TRUE);

        for (j = 0; j < segs; j++)
        {
            poly.count = 0;

            if (ta <= 0.0f)
            {
                // the innermost ring is a fan around the center
                _gradient_internal__poly_push(&poly, ox, oy, ca);
                _gradient_internal__poly_push(&poly, ox + rx * tb * cs[j], oy + ry * tb * sn[j], cb);
                _gradient_internal__poly_push(&poly, ox + rx * tb * cs[j + 1], oy + ry * tb * sn[j + 1], cb);
            }
            else
            {
                _gradient_internal__poly_push(&poly, ox + rx * ta * cs[j], oy + ry * ta * sn[j], ca);
                _gradient_internal__poly_push(&poly, ox + rx * tb * cs[j], oy + ry * tb * sn[j], cb);
                _gradient_internal__poly_push(&poly, ox + rx * tb * cs[j + 1], oy + ry * tb * sn[j + 1], cb);
                _gradient_internal__poly_push(&poly, ox + rx * ta * cs[j + 1], oy + ry * ta * sn[j + 1], ca);
            }

            if (_gradient_internal__emit_polygon(out, &n, cap, &poly, r) == FALSE)
            {
                return n;
            }
        }
    }

    return n;
}
