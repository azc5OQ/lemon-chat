#version 330 core
layout(location = 0) in vec2 a_pos_px;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
uniform vec2 u_viewport;
out vec4 v_color;
out vec2 v_uv;
void main()
{
    vec2 ndc = vec2(
        (a_pos_px.x / u_viewport.x) * 2.0 - 1.0,
        1.0 - (a_pos_px.y / u_viewport.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_color = a_color;
    v_uv    = a_uv;
}
