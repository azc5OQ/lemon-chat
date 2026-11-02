#ifndef WIDGET_IMAGE_CACHE_H
#define WIDGET_IMAGE_CACHE_H

#include "types.h"
#include "gui_api.h"

// widget_image_cache.h - public API for the shared path-keyed image
// cache that lives in widget_image.c.
// Two callers outside widget_image.c need to talk to this cache:
// - scene.c reaches in to load `background-image` textures lazily
// during emit_default_bg.
// - the platform layer needs to release the cache at shutdown so
// GPU textures + path strings don't leak past renderer__shutdown
// (the memory tracker logs them as leaks at process exit).
// Both prototypes used to live as `extern` declarations in their
// respective TUs; consolidating them here makes the API discoverable
// and lets us add documentation in one place.

GUI_API void* widget_image__cache_get_or_load(char* path, int* out_w, int* out_h);

GUI_API void  widget_image__cache_shutdown(void);

#endif
