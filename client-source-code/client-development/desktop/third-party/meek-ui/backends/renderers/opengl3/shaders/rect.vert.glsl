#version 330 core
layout(location = 0) in vec2  a_pos_px;
layout(location = 1) in vec4  a_color;
layout(location = 2) in vec2  a_local;
layout(location = 3) in vec2  a_rect_size;
layout(location = 4) in float a_radius;
uniform vec2 u_viewport;
out vec4  v_color;
out vec2  v_local;
out vec2  v_rect_size;
out float v_radius;
flat out float v_quad;
flat out vec2 v_rect_origin;
void main()
{
    v_quad = a_radius < 0.0 ? 1.0 : 0.0;
    v_rect_origin = a_pos_px - a_local;
    vec2 pos_px = a_pos_px;
    if (v_quad > 0.5)
        pos_px += sign(a_local - a_rect_size * 0.5) * 0.5;
    // convert pixel coords (top-left origin) to NDC ([-1,+1], y-up).
    vec2 ndc = vec2(
        (pos_px.x / u_viewport.x) * 2.0 - 1.0,
        1.0 - (pos_px.y / u_viewport.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_color     = a_color;
    v_local     = a_local;
    v_rect_size = a_rect_size;
    v_radius    = v_quad > 0.5 ? -a_radius - 1.0 : a_radius;
}
