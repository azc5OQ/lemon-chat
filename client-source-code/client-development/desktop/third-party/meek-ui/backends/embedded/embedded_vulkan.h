#ifndef EMBEDDED_VULKAN_H
#define EMBEDDED_VULKAN_H

#include <vulkan/vulkan.h>
#include "types.h"
#include "meek_ui_config.h"

// Formats: R8G8B8A8/B8G8R8A8 UNORM or SRGB, single sample. With an SRGB
// view the source is decoded before the target's linear-light blending; its
// translucent overlaps can therefore differ from byte-oriented UNORM blending.
// Use a UNORM view for the default cross-backend byte comparison contract.
// Borrowed handles. Use one graphics queue on the calling thread. The host
// serializes access to that queue and keeps every handle alive until detach.
typedef struct meek_vulkan_config_t
{
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkFormat color_format;
} meek_vulkan_config_t;

// An acquired single-sample color target, already owned by the graphics queue
// family (or using concurrent sharing). The host supplies its actual layouts.
// Wait semaphores are binary, consumed by end_frame; do not wait on them again
// in Present. end_frame waits for its own submission before returning, so the
// host may then present without an extra overlay semaphore. No host command
// buffer, command pool or descriptor binding is modified. No borrowed handle
// is destroyed. Drawing changes the target's pixels and requested layout.
typedef struct meek_vulkan_frame_t
{
    VkImage image;
    VkImageView view;
    uint32_t width, height;
    VkImageLayout initial_layout, final_layout;
    uint32_t wait_count;
    const VkSemaphore* wait_semaphores;
    // Must include COLOR_ATTACHMENT. TRANSFER_SRC enables renderer capture.
    VkImageUsageFlags usage;
} meek_vulkan_frame_t;

GUI_API boole embedded_vulkan__attach(const meek_vulkan_config_t* config);
GUI_API boole embedded_vulkan__is_attached(void);
GUI_API boole embedded_vulkan__begin_frame(const meek_vulkan_frame_t* frame);
GUI_API boole embedded_vulkan__end_frame(void);
GUI_API void embedded_vulkan__detach(void);

#endif
