#include "effect_acceleration.h"

static gui_effect_acceleration_e _effect_acceleration_internal__mode = GUI_EFFECT_ACCELERATION__AUTO;

boole effect_acceleration__native_enabled(void)
{
    return (boole)(_effect_acceleration_internal__mode == GUI_EFFECT_ACCELERATION__GPU);
}

boole effect_acceleration__native_blur_enabled(void)
{
    return (boole)(_effect_acceleration_internal__mode != GUI_EFFECT_ACCELERATION__CPU);
}

boole effect_acceleration__enabled(void)
{
#ifdef _WIN32
    return (boole)(_effect_acceleration_internal__mode == GUI_EFFECT_ACCELERATION__GPU);
#else
    return FALSE;
#endif
}

void renderer__set_effect_acceleration(gui_effect_acceleration_e mode)
{
    // the shutdown leaves AUTO selected; any other value than the two below keeps it
    effect_acceleration__shutdown();
    if (mode == GUI_EFFECT_ACCELERATION__GPU || mode == GUI_EFFECT_ACCELERATION__CPU)
    {
        _effect_acceleration_internal__mode = mode;
    }
}

boole effect_acceleration__compose(const effect_surface_t* source, effect_surface_t* destination,
    int left, int top, int width, int height, const float* inverse, float opacity, int blend)
{
#ifdef _WIN32
    // Upload/readback overhead outweighs arithmetic for small surfaces.
    if (_effect_acceleration_internal__mode == GUI_EFFECT_ACCELERATION__GPU &&
        (int64)width * height >= 16384 && width > 0 && height > 0)
    {
        return effects_d3d11__compose(source, destination, left, top, width, height, inverse, opacity, blend);
    }
#endif
    return FALSE;
}

boole effect_acceleration__background(effect_surface_t* destination, int left, int top, int width, int height,
    gui_rect_t bounds, float radius, float opacity, gui_color_t base, const effect_background_layer_t* layers,
    int count, const float* const* coordinates)
{
#ifdef _WIN32
    if (_effect_acceleration_internal__mode == GUI_EFFECT_ACCELERATION__GPU &&
        (int64)width * height >= 16384 && width > 0 && height > 0)
    {
        return effects_d3d11__background(destination, left, top, width, height, bounds, radius, opacity, base,
            layers, count, coordinates, width, 0, FALSE);
    }
#endif
    return FALSE;
}

boole effect_acceleration__background_compose(effect_surface_t* destination, int left, int top, int width, int height,
    gui_rect_t bounds, float radius, float opacity, gui_color_t base, const effect_background_layer_t* layers,
    int count, const float* const* coordinates, int coordinate_width, int blend)
{
#ifdef _WIN32
    if (effect_acceleration__enabled() && (int64)width * height >= 16384 && width > 0 && height > 0)
    {
        return effects_d3d11__background(destination, left, top, width, height, bounds, radius, opacity, base,
            layers, count, coordinates, coordinate_width, blend, TRUE);
    }
#endif
    return FALSE;
}

void effect_acceleration__shutdown(void)
{
#ifdef _WIN32
    effects_d3d11__shutdown();
#endif
    _effect_acceleration_internal__mode = GUI_EFFECT_ACCELERATION__AUTO;
}
