#version 330 core
in vec4 v_color;
in vec2 v_uv;
out vec4 o_color;
uniform sampler2D u_atlas;
void main()
{
    float a = texture(u_atlas, v_uv).r;
    if (a <= 0.0) discard;
    vec3 linear_rgb = pow(v_color.rgb, vec3(2.2));
    o_color = vec4(linear_rgb, v_color.a * a);
}
