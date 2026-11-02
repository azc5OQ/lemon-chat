#include "embedded_opengl2.h"
#include "renderer.h"

static boole attached, in_frame;

boole embedded_opengl2__set_display_gamma(const ushort* ramp)
{
    return renderer__opengl2_set_display_gamma(ramp);
}

boole embedded_opengl2__attach(void)
{
    return embedded_opengl2__attach_with_loader(NULL_POINTER);
}

boole embedded_opengl2__attach_with_loader(meek_opengl2_get_proc_fn loader)
{
    if (attached)
    {
        return TRUE;
    }
    attached = renderer__init_adopt_opengl2(loader);
    return attached;
}

boole embedded_opengl2__is_attached(void)
{
    return attached;
}

boole embedded_opengl2__begin_frame(int64 width, int64 height)
{
    if (!attached || in_frame || width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        return FALSE;
    }
    if (!renderer__opengl2_save_state())
    {
        return FALSE;
    }
    renderer__begin_frame(width, height, (gui_color_t){0, 0, 0, 0});
    in_frame = TRUE;
    return TRUE;
}

void embedded_opengl2__end_frame(void)
{
    if (!in_frame)
    {
        return;
    }
    renderer__end_frame();
    renderer__opengl2_restore_state();
    in_frame = FALSE;
}

void embedded_opengl2__detach(void)
{
    if (!attached)
    {
        return;
    }
    embedded_opengl2__end_frame();
    // Deleting our textures does not change a host-owned binding.
    renderer__shutdown();
    attached = FALSE;
}
