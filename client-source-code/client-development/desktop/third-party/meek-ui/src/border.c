#include "border.h"
#include "gradient.h"
#include "renderer.h"
#include "clib/stdlib.h"

static gui_vec2_t _border_internal__point(gui_rect_t bounds, float radius, int side, float fraction);
static gui_color_t _border_internal__color(gui_color_t color, const gradient_sampler_t* gradient, gui_rect_t bounds, gui_vec2_t point, int side, int bevel, float opacity);
static void _border_internal__ring(gui_rect_t bounds, float radius, float width, gui_border_style_e style, gui_color_t color, const gradient_sampler_t* gradient, gui_rect_t paint_bounds, float opacity, int bevel);

/** @brief evaluates matching outer and inner points on a rounded rectangle side
 * @param gui_rect_t bounds -> rectangle in device pixels
 * @param float radius -> clamped corner radius
 * @param int side -> clockwise straight edge or corner, starting with the top edge
 * @param float fraction -> normalized distance along this edge or corner
 * @return gui_vec2_t -> perimeter point
 */
static gui_vec2_t _border_internal__point(gui_rect_t bounds, float radius, int side, float fraction)
{
    gui_vec2_t point = {0};
    float angle = 0.0f;

    if (side == 0 || side == 4)
    {
        point.x = bounds.x + radius + (bounds.w - 2.0f * radius) * (side == 0 ? fraction : 1.0f - fraction);
        point.y = bounds.y + (side == 4 ? bounds.h : 0.0f);
    }
    else if (side == 2 || side == 6)
    {
        point.x = bounds.x + (side == 2 ? bounds.w : 0.0f);
        point.y = bounds.y + radius + (bounds.h - 2.0f * radius) * (side == 2 ? fraction : 1.0f - fraction);
    }
    else
    {
        angle = ((float)(side / 2) - 1.0f + fraction) * 1.57079632679f;
        point.x = bounds.x + ((side == 1 || side == 3) ? bounds.w - radius : radius) + stdlib__cosf(angle) * radius;
        point.y = bounds.y + ((side == 3 || side == 5) ? bounds.h - radius : radius) + stdlib__sinf(angle) * radius;
    }
    return point;
}

/** @brief samples a border color and applies the optional inset or outset lighting
 * @param gui_color_t color -> fallback solid color
 * @param const gradient_sampler_t* gradient -> optional prepared gradient
 * @param gui_rect_t bounds -> gradient coordinate origin
 * @param gui_vec2_t point -> sample location
 * @param int side -> clockwise perimeter side
 * @param int bevel -> negative for inset, positive for outset, zero for flat
 * @param float opacity -> inherited alpha multiplier
 * @return gui_color_t -> straight RGBA paint
 */
static gui_color_t _border_internal__color(gui_color_t color, const gradient_sampler_t* gradient, gui_rect_t bounds, gui_vec2_t point, int side, int bevel, float opacity)
{
    boole light = FALSE;

    if (gradient != NULL_POINTER)
    {
        color = gradient__sample(gradient, point.x - bounds.x, point.y - bounds.y);
    }
    if (bevel != 0)
    {
        light = (boole)((side == 0 || side >= 6) == (bevel > 0));
        if (light)
        {
            color.r += (1.0f - color.r) * 0.35f;
            color.g += (1.0f - color.g) * 0.35f;
            color.b += (1.0f - color.b) * 0.35f;
        }
        else
        {
            color.r *= 0.55f;
            color.g *= 0.55f;
            color.b *= 0.55f;
        }
    }
    color.a *= opacity;
    return color;
}

/** @brief emits nonoverlapping ring triangles or regularly spaced circular dots
 * @param gui_rect_t bounds -> outer ring bounds
 * @param float radius -> outer corner radius
 * @param float width -> ring thickness
 * @param gui_border_style_e style -> solid, dashed or dotted
 * @param gui_color_t color -> solid paint
 * @param const gradient_sampler_t* gradient -> optional prepared paint
 * @param gui_rect_t paint_bounds -> original gradient origin
 * @param float opacity -> inherited opacity
 * @param int bevel -> lighting orientation
 * @return void
 */
static void _border_internal__ring(gui_rect_t bounds, float radius, float width, gui_border_style_e style, gui_color_t color, const gradient_sampler_t* gradient, gui_rect_t paint_bounds, float opacity, int bevel)
{
    gui_rect_t inner = {0};
    gui_rect_t center = {0};
    gui_vec2_t a = {0};
    gui_vec2_t b = {0};
    gui_vec2_t c = {0};
    gui_vec2_t d = {0};
    gui_vec2_t point = {0};
    gui_color_t paint = {0};
    float lengths[8] = {0};
    float inner_radius = 0.0f;
    float center_radius = 0.0f;
    float perimeter = 0.0f;
    float cursor = 0.0f;
    float end = 0.0f;
    float step = 0.0f;
    float phase = 0.0f;
    float offset = 0.0f;
    float fraction = 0.0f;
    float next_fraction = 0.0f;
    float spacing = 0.0f;
    float next_dot = 0.0f;
    int side = 0;
    int dots = 0;

    inner = bounds;
    inner.x += width;
    inner.y += width;
    inner.w -= width * 2.0f;
    inner.h -= width * 2.0f;
    inner_radius = stdlib__fmaxf(0.0f, radius - width);
    center = bounds;
    center.x += width * 0.5f;
    center.y += width * 0.5f;
    center.w -= width;
    center.h -= width;
    center_radius = stdlib__fmaxf(0.0f, radius - width * 0.5f);
    lengths[0] = lengths[4] = (style == GUI_BORDER_STYLE__DOTTED ? center.w - center_radius * 2.0f : bounds.w - radius * 2.0f);
    lengths[2] = lengths[6] = (style == GUI_BORDER_STYLE__DOTTED ? center.h - center_radius * 2.0f : bounds.h - radius * 2.0f);
    lengths[1] = lengths[3] = lengths[5] = lengths[7] = (style == GUI_BORDER_STYLE__DOTTED ? center_radius : radius) * 1.57079632679f;
    for (side = 0; side < 8; side++)
    {
        perimeter += lengths[side];
    }
    if (style == GUI_BORDER_STYLE__DOTTED)
    {
        dots = (int)(perimeter / stdlib__fmaxf(width * 2.5f, 1.0f));
        spacing = perimeter / (float)(dots > 0 ? dots : 1);
    }
    for (side = 0; side < 8; side++)
    {
        if (lengths[side] <= 0.0f)
        {
            continue;
        }
        if (style == GUI_BORDER_STYLE__DOTTED)
        {
            while (next_dot < offset + lengths[side])
            {
                point = _border_internal__point(center, center_radius, side, (next_dot - offset) / lengths[side]);
                paint = _border_internal__color(color, gradient, paint_bounds, point, side, bevel, opacity);
                renderer__submit_circle(point, width * 0.5f, paint);
                next_dot += spacing;
            }
            offset += lengths[side];
            continue;
        }
        for (cursor = 0.0f; cursor < lengths[side]; cursor = end)
        {
            step = ((side & 1) != 0 || gradient != NULL_POINTER) ? 1.5f : lengths[side];
            if (style == GUI_BORDER_STYLE__DASHED)
            {
                phase = stdlib__fmodf(offset + cursor, stdlib__fmaxf(width * 5.0f, 1.0f));
                if (phase >= width * 3.0f)
                {
                    end = stdlib__fminf(lengths[side], cursor + stdlib__fmaxf(width * 5.0f, 1.0f) - phase + 0.001f);
                    continue;
                }
                step = stdlib__fminf(step, width * 3.0f - phase + 0.001f);
            }
            end = stdlib__fminf(lengths[side], cursor + step);
            fraction = cursor / lengths[side];
            next_fraction = end / lengths[side];
            a = _border_internal__point(bounds, radius, side, fraction);
            b = _border_internal__point(bounds, radius, side, next_fraction);
            c = _border_internal__point(inner, inner_radius, side, next_fraction);
            d = _border_internal__point(inner, inner_radius, side, fraction);
            point.x = (a.x + b.x + c.x + d.x) * 0.25f;
            point.y = (a.y + b.y + c.y + d.y) * 0.25f;
            paint = _border_internal__color(color, gradient, paint_bounds, point, side, bevel, opacity);
            renderer__submit_triangle(a, b, c, paint);
            renderer__submit_triangle(a, c, d, paint);
        }
        offset += lengths[side];
    }
}

/** @brief paints a CSS-style uniform border without covering its transparent center
 * @param gui_rect_t bounds -> outer bounds in device pixels
 * @param float radius -> corner radius in device pixels
 * @param float width -> requested thickness in device pixels
 * @param gui_border_style_e style -> solid, dashed, dotted, double or beveled style
 * @param gui_color_t color -> base color
 * @param const gui_gradient_t* gradient -> optional gradient covering the original bounds
 * @param float scale -> logical-to-device scale for gradient lengths
 * @param float opacity -> inherited alpha multiplier
 * @return void
 */
void border__draw(gui_rect_t bounds, float radius, float width, gui_border_style_e style, gui_color_t color, const gui_gradient_t* gradient, float scale, float opacity)
{
    gradient_sampler_t sampler = {0};
    const gradient_sampler_t* paint = NULL_POINTER;
    gui_rect_t inner = {0};
    float inset = 0.0f;
    float stripe = 0.0f;
    int bevel = 0;

    if (bounds.w <= 0.0f || bounds.h <= 0.0f || width <= 0.0f || opacity <= 0.0f || style == GUI_BORDER_STYLE__NONE || style == GUI_BORDER_STYLE__HIDDEN)
    {
        return;
    }
    radius = stdlib__fmaxf(0.0f, stdlib__fminf(radius, stdlib__fminf(bounds.w, bounds.h) * 0.5f));
    width = stdlib__fminf(width, stdlib__fminf(bounds.w, bounds.h) * 0.5f);
    if (gradient != NULL_POINTER)
    {
        gradient__prepare(gradient, bounds.w, bounds.h, scale, &sampler);
        paint = &sampler;
    }
    if (style == GUI_BORDER_STYLE__DOUBLE && width >= 3.0f)
    {
        stripe = width / 3.0f;
        inset = stripe * 2.0f;
    }
    else if (style == GUI_BORDER_STYLE__GROOVE || style == GUI_BORDER_STYLE__RIDGE)
    {
        stripe = width * 0.5f;
        inset = stripe;
        bevel = style == GUI_BORDER_STYLE__GROOVE ? -1 : 1;
    }
    else
    {
        bevel = style == GUI_BORDER_STYLE__INSET ? -1 : style == GUI_BORDER_STYLE__OUTSET ? 1 : 0;
        _border_internal__ring(bounds, radius, width, style, color, paint, bounds, opacity, bevel);
        return;
    }
    _border_internal__ring(bounds, radius, stripe, GUI_BORDER_STYLE__SOLID, color, paint, bounds, opacity, bevel);
    inner = bounds;
    inner.x += inset;
    inner.y += inset;
    inner.w -= inset * 2.0f;
    inner.h -= inset * 2.0f;
    _border_internal__ring(inner, stdlib__fmaxf(0.0f, radius - inset), stripe, GUI_BORDER_STYLE__SOLID, color, paint, bounds, opacity, -bevel);
}
