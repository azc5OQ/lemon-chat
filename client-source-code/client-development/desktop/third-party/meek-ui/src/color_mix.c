#include "types.h"
#include "color_mix.h"
#include "clib/stdlib.h"

static float _color_mix_internal__linear(float v);
static float _color_mix_internal__encoded(float v);
static void _color_mix_internal__to_lab(gui_color_t color, float* lab);
static gui_color_t _color_mix_internal__from_lab(const float* lab);

/**
 * @brief decode the sRGB transfer function
 * @param float v -> encoded channel
 * @return float -> linear-light channel
 */
static float _color_mix_internal__linear(float v)
{
    return (v <= 0.04045f) ? v / 12.92f : stdlib__powf((v + 0.055f) / 1.055f, 2.4f);
}

/**
 * @brief encode and gamut-clip an sRGB channel
 * @param float v -> linear-light channel
 * @return float -> encoded channel in zero to one
 */
static float _color_mix_internal__encoded(float v)
{
    v = (v <= 0.0031308f) ? v * 12.92f : 1.055f * stdlib__powf(v, 1.0f / 2.4f) - 0.055f;
    return stdlib__fminf(1.0f, stdlib__fmaxf(0.0f, v));
}

/**
 * @brief convert sRGB to Oklab using the CSS Color conversion matrices
 * @param gui_color_t color -> sRGB input
 * @param float* lab -> three Oklab coordinates
 * @return void
 */
static void _color_mix_internal__to_lab(gui_color_t color, float* lab)
{
    float r = _color_mix_internal__linear(color.r);
    float g = _color_mix_internal__linear(color.g);
    float b = _color_mix_internal__linear(color.b);
    float l = 0.0f;
    float m = 0.0f;
    float s = 0.0f;

    l = stdlib__powf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b, 1.0f / 3.0f);
    m = stdlib__powf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b, 1.0f / 3.0f);
    s = stdlib__powf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b, 1.0f / 3.0f);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

/**
 * @brief convert Oklab back into the renderer's sRGB gamut
 * @param const float* lab -> Oklab coordinates
 * @return gui_color_t -> opaque sRGB color
 */
static gui_color_t _color_mix_internal__from_lab(const float* lab)
{
    gui_color_t color;
    float l = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    float m = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    float s = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];

    stdlib__null_memory(&color, sizeof(color));
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    color.r = _color_mix_internal__encoded(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
    color.g = _color_mix_internal__encoded(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
    color.b = _color_mix_internal__encoded(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s);
    color.a = 1.0f;
    return color;
}

/**
 * @brief mix colors with normalized percentages and premultiplied alpha
 * @param gui_color_t a -> first color
 * @param gui_color_t b -> second color
 * @param float weight_a -> first fraction
 * @param float weight_b -> second fraction
 * @param const char* space -> srgb, srgb-linear or oklab
 * @return gui_color_t -> mixed straight-alpha color
 */
gui_color_t color_mix__mix(gui_color_t a, gui_color_t b, float weight_a, float weight_b, const char* space)
{
    gui_color_t result;
    float sum = weight_a + weight_b;
    float alpha = 0.0f;
    float lab_a[3];
    float lab_b[3];
    int i = 0;

    stdlib__null_memory(&result, sizeof(result));
    stdlib__null_memory(lab_a, sizeof(lab_a));
    stdlib__null_memory(lab_b, sizeof(lab_b));
    if (sum <= 0.0f)
    {
        return result;
    }
    weight_a /= sum;
    weight_b /= sum;
    alpha = a.a * weight_a + b.a * weight_b;
    if (alpha <= 0.0f)
    {
        return result;
    }
    weight_a = a.a * weight_a / alpha;
    weight_b = b.a * weight_b / alpha;
    if (stdlib__strcmp(space, "oklab") == 0)
    {
        _color_mix_internal__to_lab(a, lab_a);
        _color_mix_internal__to_lab(b, lab_b);
        for (i = 0; i < 3; i++)
        {
            lab_a[i] = lab_a[i] * weight_a + lab_b[i] * weight_b;
        }
        result = _color_mix_internal__from_lab(lab_a);
    }
    else if (stdlib__strcmp(space, "srgb-linear") == 0)
    {
        result.r = _color_mix_internal__encoded(_color_mix_internal__linear(a.r) * weight_a + _color_mix_internal__linear(b.r) * weight_b);
        result.g = _color_mix_internal__encoded(_color_mix_internal__linear(a.g) * weight_a + _color_mix_internal__linear(b.g) * weight_b);
        result.b = _color_mix_internal__encoded(_color_mix_internal__linear(a.b) * weight_a + _color_mix_internal__linear(b.b) * weight_b);
    }
    else
    {
        result.r = a.r * weight_a + b.r * weight_b;
        result.g = a.g * weight_a + b.g * weight_b;
        result.b = a.b * weight_a + b.b * weight_b;
    }
    result.a = alpha * stdlib__fminf(1.0f, sum);
    return result;
}
