#ifndef MOTION_H
#define MOTION_H

#include "types.h"
#include "gui.h"
#include "gui_api.h"

// Extended CSS values and timelines shared by the existing scene nodes.
// prepare_styles returns an owned filtered copy, or NULL when no extension
// is needed. The caller frees that copy with GUI_FREE.
char* motion__prepare_styles(const char* bytes, int64 length);
void motion__reset(void);
void motion__forget(gui_node_t* node);
void motion__tick(void);
boole motion__active(void);
// Simple resolved-style keyframes can use the ordinary renderer directly.
GUI_API boole motion__needs_compositor(void);
boole motion__animating(void);
// Property ids. Every declaration, keyframe value, per-node value and
// @property default carries the id its name was interned to when the
// stylesheet was parsed, so nothing on the per-frame path compares
// names. The known properties have fixed ids (the enum) and a per-node
// slot table; custom `--*` names and anything else get ids above
// MOTION_PROP__KNOWN_COUNT from motion__prop_id.
typedef enum motion_prop_e
{
    MOTION_PROP__TRANSFORM = 0,
    MOTION_PROP__TRANSFORM_ORIGIN = 1,
    MOTION_PROP__FILTER = 2,
    MOTION_PROP__BACKDROP_FILTER = 3,
    MOTION_PROP__MASK = 4,
    MOTION_PROP__MASK_IMAGE = 5,
    MOTION_PROP__MASK_SIZE = 6,
    MOTION_PROP__MASK_POSITION = 7,
    MOTION_PROP__MASK_REPEAT = 8,
    MOTION_PROP__MASK_COMPOSITE = 9,
    MOTION_PROP__CLIP_PATH = 10,
    MOTION_PROP__BORDER_RADIUS = 11,
    MOTION_PROP__MIX_BLEND_MODE = 12,
    MOTION_PROP__BACKGROUND = 13,
    MOTION_PROP__BACKGROUND_IMAGE = 14,
    MOTION_PROP__BACKGROUND_SIZE = 15,
    MOTION_PROP__BACKGROUND_POSITION = 16,
    MOTION_PROP__BACKGROUND_REPEAT = 17,
    MOTION_PROP__BACKGROUND_CLIP = 18,
    MOTION_PROP__WEBKIT_BACKGROUND_CLIP = 19,
    MOTION_PROP__BACKGROUND_COLOR = 20,
    MOTION_PROP__COLOR = 21,
    MOTION_PROP__FONT_COLOR = 22,
    MOTION_PROP__OPACITY = 23,
    MOTION_PROP__WIDTH = 24,
    MOTION_PROP__HEIGHT = 25,
    MOTION_PROP__TOP = 26,
    MOTION_PROP__LEFT = 27,
    MOTION_PROP__BORDER_COLOR = 28,
    MOTION_PROP__TEXT_SHADOW = 29,
    MOTION_PROP__WEBKIT_TEXT_STROKE = 30,
    MOTION_PROP__POINTER_EVENTS = 31,
    MOTION_PROP__LETTER_SPACING = 32,
    MOTION_PROP__FONT_VARIATION_SETTINGS = 33,
    MOTION_PROP__ANIMATION = 34,
    MOTION_PROP__ANIMATION_NAME = 35,
    MOTION_PROP__ANIMATION_DURATION = 36,
    MOTION_PROP__ANIMATION_DELAY = 37,
    MOTION_PROP__ANIMATION_DIRECTION = 38,
    MOTION_PROP__ANIMATION_ITERATION_COUNT = 39,
    MOTION_PROP__ANIMATION_FILL_MODE = 40,
    MOTION_PROP__ANIMATION_TIMING_FUNCTION = 41,
    MOTION_PROP__ANIMATION_TIMELINE = 42,
    MOTION_PROP__ANIMATION_RANGE = 43,
    MOTION_PROP__KNOWN_COUNT = 44
} motion_prop_e;

#define MOTION_PROP__CAP 512

int motion__prop_id(const char* name);
boole motion__value_id(gui_node_t* node, int id, char* out, int64 capacity);
boole motion__has_id(gui_node_t* node, int id);
// the name forms intern the name first; for parse time and rare callers only
boole motion__value(gui_node_t* node, const char* property, char* out, int64 capacity);
boole motion__has(gui_node_t* node, const char* property);
float motion__number(const char* value, float percent_reference);
void motion__format(float value, char* out, int64 capacity);
boole motion__matches(gui_node_t* node, const char* selector);
GUI_API void motion__set_reduced_motion(boole enabled);
GUI_API boole motion__reduced_motion(void);

#endif
