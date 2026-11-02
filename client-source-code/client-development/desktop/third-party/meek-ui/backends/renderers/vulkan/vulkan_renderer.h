#ifndef VULKAN_RENDERER_H
#define VULKAN_RENDERER_H
#include "embedded_vulkan.h"
GUI_API boole renderer__init_adopt_vulkan(const meek_vulkan_config_t* config);
GUI_API boole renderer__vulkan_target(const meek_vulkan_frame_t* frame);
GUI_API boole renderer__vulkan_frame_ok(void);
#endif
