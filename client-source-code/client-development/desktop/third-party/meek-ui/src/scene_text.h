#ifndef SCENE_TEXT_H
#define SCENE_TEXT_H

#include "scene.h"

// Internal ownership helpers. A release resets the node to its inline buffer.
void scene_text__release(gui_node_t* node);
void scene_text__move(gui_node_t* destination, gui_node_t* source);
int scene_text__encode(uint codepoint, char encoded[4]);
// Shared UTF-8 editing state; byte offsets always land between codepoints.
typedef struct scene_text_state_t
{
    int64 caret;
    int64 anchor;
    uint fingerprint;
    boole initialized;
    float scroll_x;
    char* mask;
    int64 mask_capacity;
    int64 mask_characters;
    float preferred_x;
    boole has_preferred_x;
    boole prefer_line_end;
    boole reveal_caret;
} scene_text_state_t;

scene_text_state_t* scene_text__state(gui_node_t* node);
int64 scene_text__previous(gui_node_t* node, int64 at);
int64 scene_text__next(gui_node_t* node, int64 at);
void scene_text__replace(gui_node_t* node, scene_text_state_t* state, const char* bytes, int64 length);
void scene_text__on_char(gui_node_t* node, uint codepoint);
void scene_text__on_key(gui_node_t* node, int64 vk, boole down);

#endif
