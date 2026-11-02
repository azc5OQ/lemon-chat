// scene_style.h -- internal header for the style subsystem extracted
// from scene.c. Not part of the public gui API; consumed only by
// scene.c (which still owns the root-tree pointer and calls into
// this module to run the two-pass style resolution).
// Public style API (scene__register_style, scene__clear_styles,
// scene__resolve_styles, scene__set_background_color_override, etc.)
// stays in scene.h.
#ifndef MEEK_UI_SCENE_STYLE_H
#define MEEK_UI_SCENE_STYLE_H

#include "gui.h"

// Walk the tree starting at `root`, matching each registered style
// rule against every node in three specificity passes (type-only,
// class, id). Writes the matched rules' payloads into the node's
// per-state style slots. Call BEFORE scene_style__resolve_tree.
void scene_style__apply_rules_tree(gui_node_t* root);

// Walk the tree starting at `root`, producing each node's `resolved`
// style by overlaying its current-state slot onto the default slot,
// inheriting typography + appear animation from the parent, and
// applying host overrides (background_color_override / font_color_override).
// Call AFTER scene_style__apply_rules_tree.
void scene_style__resolve_tree(gui_node_t* root);

#endif
