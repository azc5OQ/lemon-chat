// Cross-platform adapter: every Vulkan object belongs to the host except the
// renderer's own pipelines, buffers, command pool, framebuffer and fence.
#include "embedded_vulkan.h"
#include "vulkan_renderer.h"
#include "renderer.h"

static boole attached;
static boole in_frame;

boole embedded_vulkan__attach(const meek_vulkan_config_t* config)
{
    if (attached)
    {
        return FALSE;
    }
    attached = renderer__init_adopt_vulkan(config);
    return attached;
}
boole embedded_vulkan__is_attached(void)
{
    return attached;
}
boole embedded_vulkan__begin_frame(const meek_vulkan_frame_t* frame)
{
    gui_color_t transparent = {0, 0, 0, 0};
    if (!attached || in_frame || !renderer__vulkan_target(frame))
    {
        return FALSE;
    }
    renderer__begin_frame(frame->width, frame->height, transparent);
    in_frame = renderer__vulkan_frame_ok();
    return in_frame;
}
boole embedded_vulkan__end_frame(void)
{
    if (!in_frame)
    {
        return FALSE;
    }
    renderer__end_frame();
    in_frame = FALSE;
    return renderer__vulkan_frame_ok();
}
void embedded_vulkan__detach(void)
{
    if (!attached)
    {
        return;
    }
    if (in_frame)
    {
        embedded_vulkan__end_frame();
    }
    renderer__shutdown();
    attached = FALSE;
}
