#ifndef _VISUAL_CONTRACT_H
#define _VISUAL_CONTRACT_H

// _visual_contract.h - shared shader snippets + vertex layout
// constants enforcing the VISUAL CONTRACT declared in renderer.h.
// The six renderers (opengl3 / gles3 / d3d9 / d3d11 / d3d12 /
// vulkan) each carry their own compiled shader for the solid-
// rect SDF path. Historically every one pasted the same SDF
// formula + smoothstep AA + vertex layout verbatim -- a visual
// contract change (e.g. swap 1-px AA for fwidth-adaptive) had to
// edit six files and risk divergence.
// This header centralizes the bits that MUST stay identical:
// RENDERER_SDF_ROUND_BOX_FN    the SDF body. Paste into any GLSL
// or HLSL shader string -- syntax
// is the common subset that both
// language families accept
// (float2/vec2 are aliased via a
// small prelude each backend picks).
// RENDERER_SDF_AA_RANGE_MIN    smoothstep range for the 1-pixel
// RENDERER_SDF_AA_RANGE_MAX    AA band. Pass both into smoothstep
// at the call site.
// RENDERER_RECT_VERTS_PER_QUAD how many vertices each rect contributes.
// RENDERER_TOPOLOGY_*          the two-triangle topology order.
// A backend includes this header from its .c file, then pastes the
// shader-side constant into its GLSL / HLSL string literal. Any
// change to the SDF formula or AA band happens here once.
// What this file does NOT do:
// - provide language-specific preludes (vec2 vs float2). Each
// backend owns that via a 2-line macro defined before the
// shader concatenation.
// - provide the blend state. Different APIs (Vulkan
// VkPipelineColorBlendAttachmentState vs D3D12
// D3D12_RENDER_TARGET_BLEND_DESC) need different struct
// shapes, so we document the VALUES here and each backend
// fills its own struct.

// The SDF rounded-box distance function. Works unmodified in
// GLSL 300 es / 330 / 410 / 450 and HLSL 5.x because it sticks
// to the intersection of both languages' syntax (vec2/float2
// type name is the only difference, and a per-backend macro
// aliases it).
// Contract: returns signed distance from `p` to a rounded rect
// of half-size `b` with corner radius `r`. Negative inside, 0 on
// the edge, positive outside. Used by every backend's fragment
// shader with p = local_pixel - rect_size*0.5.
#define RENDERER_SDF_ROUND_BOX_GLSL                                  \
    "float sd_round_box(vec2 p, vec2 b, float r) {\n"                \
    "    vec2 q = abs(p) - b + vec2(r);\n"                           \
    "    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"\
    "}\n"

#define RENDERER_SDF_ROUND_BOX_HLSL                                            \
    "float sd_round_box(float2 p, float2 b, float r) {\n"                      \
    "    float2 q = abs(p) - b + float2(r, r);\n"                              \
    "    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"          \
    "}\n"

// AA band: 1 pixel wide, centered on the mathematical edge.
// alpha = 1 - smoothstep(MIN, MAX, d). Exact same numbers across
// every backend -- moving this is a VISUAL CONTRACT change and
// breaks the pixel-identical property.
#define RENDERER_SDF_AA_MIN "-0.5"
#define RENDERER_SDF_AA_MAX " 0.5"

// Quad topology. Each rect is two triangles:
// tri 1:  (x0,y0) (x1,y0) (x0,y1)
// tri 2:  (x1,y0) (x1,y1) (x0,y1)
// 6 vertices, no index buffer. Winding is counter-clockwise in
// y-down pixel space; face culling should be OFF on every
// backend so the winding doesn't matter for the SDF pass. Kept
// in sync here so a change in one backend forces a conscious
// update everywhere.
#define RENDERER_RECT_VERTS_PER_QUAD 6

// Vertex layout: 11 floats per vertex. Identical across every
// backend (gles3 / opengl3 / d3d11 / d3d9 / d3d12 / vulkan).
// Slot names -> offsets are documented here so a new backend
// knows exactly what the CPU-side vertex struct looks like.
// Offset 0..1   pos_px       vec2  top-left-origin pixel position
// Offset 2..5   color        vec4  RGBA in linear 0..1
// Offset 6..7   local        vec2  (0,0) at rect top-left, (w,h) at bottom-right
// Offset 8..9   rect_size    vec2  rect width + height in pixels
// Offset 10     radius       float corner radius in pixels, clamped to min(w,h)*0.5
#define RENDERER_RECT_VERTEX_FLOATS 11

// Separate-alpha blend (see VISUAL CONTRACT in renderer.h for
// the full explanation of why). Values documented here so every
// backend struct is filled identically.
// src_rgb   = SRC_ALPHA
// dst_rgb   = ONE_MINUS_SRC_ALPHA
// op_rgb    = ADD
// src_a     = ONE
// dst_a     = ONE_MINUS_SRC_ALPHA
// op_a      = ADD
// write mask = RGBA

#endif
