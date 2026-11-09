#version 330 core
in vec4 v_color;
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_atlas;
void main()
{
    float a = texture(u_atlas, v_uv).r;
    if (a <= 0.0)
    {
        discard;
    }
    // Coverage is an alpha fraction, not an sRGB color. Raising it
    // makes dark text on light surfaces heavier than the font outlines.
    o_color = vec4(v_color.rgb, v_color.a * a);
}
