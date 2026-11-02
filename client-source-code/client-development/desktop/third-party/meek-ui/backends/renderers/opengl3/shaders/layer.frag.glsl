#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_opacity;
out vec4 out_color;
// premultiplied texel times the group opacity; the blend function does the rest
void main() {
    out_color = texture(u_tex, v_uv) * u_opacity;
}
