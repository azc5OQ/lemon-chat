#ifndef RENDERER_DEBUG_H
#define RENDERER_DEBUG_H
// Internal hooks used by the shared compositor, not individual backends.
void renderer_debug__draw(void);
void renderer_debug__frame_finished(void);
void renderer_debug__reset_timing(void);
#endif
