#version 450
layout(location = 0) in vec2  a_pos_px;
layout(location = 1) in vec4  a_color;
layout(location = 2) in vec2  a_local;
layout(location = 3) in vec2  a_rect_size;
layout(location = 4) in float a_radius;
layout(push_constant) uniform PC { vec2 u_viewport; } pc;
layout(location = 0) out vec4  v_color;
layout(location = 1) out vec2  v_local;
layout(location = 2) out vec2  v_rect_size;
layout(location = 3) out float v_radius;
void main() {
    vec2 ndc = vec2(
        (a_pos_px.x / pc.u_viewport.x) * 2.0 - 1.0,
        (a_pos_px.y / pc.u_viewport.y) * 2.0 - 1.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_color     = a_color;
    v_local     = a_local;
    v_rect_size = a_rect_size;
    v_radius    = a_radius;
}
