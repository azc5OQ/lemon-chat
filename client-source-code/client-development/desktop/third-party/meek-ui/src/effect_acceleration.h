#ifndef EFFECT_ACCELERATION_H
#define EFFECT_ACCELERATION_H

#include "renderer.h"
#include "gradient.h"

#define EFFECT_BACKGROUND_LAYER_CAP 16

// Prepared premultiplied layer shared by portable paint and optional compute.
typedef struct effect_background_layer_t
{
    gradient_sampler_t gradient;
    gui_color_t color;
    float w, h, x, y;
    boole is_gradient, repeat_x, repeat_y;
    // the parsed gradient the sampler was prepared from (layer mode draws
    // an untiled one with the backend's gradient pipeline)
    gui_gradient_t source;
} effect_background_layer_t;

// Internal premultiplied RGBA8 surface view. Pixel origins use device coordinates.
typedef struct effect_surface_t
{
    ubyte* pixels;
    int x, y, w, h;
    // Nonzero only for immutable decoration-cache content. A version is never
    // reused for different bytes; zero requests an ordinary uncached upload.
    uint64 content_version;
} effect_surface_t;

boole effect_acceleration__compose(const effect_surface_t* source, effect_surface_t* destination,
    int left, int top, int width, int height, const float* inverse, float opacity, int blend);
boole effect_acceleration__background(effect_surface_t* destination, int left, int top, int width, int height,
    gui_rect_t bounds, float radius, float opacity, gui_color_t base, const effect_background_layer_t* layers,
    int count, const float* const* coordinates);
boole effect_acceleration__enabled(void);
// Native renderer effects also work on platforms without D3D11 compute.
boole effect_acceleration__native_enabled(void);
// The renderer's own backdrop blur (renderer_backend__blur_layer) is used in
// every mode except GUI_EFFECT_ACCELERATION__CPU.
boole effect_acceleration__native_blur_enabled(void);
// Paint an otherwise empty leaf and blend its RGBA8 result directly into its parent.
boole effect_acceleration__background_compose(effect_surface_t* destination, int left, int top, int width, int height,
    gui_rect_t bounds, float radius, float opacity, gui_color_t base, const effect_background_layer_t* layers,
    int count, const float* const* coordinates, int coordinate_width, int blend);
void effect_acceleration__shutdown(void);

#ifdef _WIN32
boole effects_d3d11__compose(const effect_surface_t* source, effect_surface_t* destination,
    int left, int top, int width, int height, const float* inverse, float opacity, int blend);
boole effects_d3d11__background(effect_surface_t* destination, int left, int top, int width, int height,
    gui_rect_t bounds, float radius, float opacity, gui_color_t base, const effect_background_layer_t* layers,
    int count, const float* const* coordinates, int coordinate_width, int blend, boole compose);
void effects_d3d11__shutdown(void);
#endif

#endif
