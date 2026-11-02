#version 330 core
// A layer quad: the position is homogeneous (x * w, y * w, w) in device
// pixels so a perspective transform interpolates the texture correctly.
layout(location=0) in vec3 in_pos;
layout(location=1) in vec2 in_uv;
uniform vec2 u_viewport;
out vec2 v_uv;
void main() {
    float x = (in_pos.x / u_viewport.x) * 2.0 - in_pos.z;
    float y = in_pos.z - (in_pos.y / u_viewport.y) * 2.0;
    gl_Position = vec4(x, y, 0.0, in_pos.z);
    v_uv = in_uv;
}
