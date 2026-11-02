#version 330 core
uniform sampler2D u_tex;
uniform int u_radius;
uniform int u_axis;
uniform vec2 u_size;
out vec4 out_color;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 size = ivec2(u_size);
    vec4 sum = vec4(0.0);
    for (int k = -u_radius; k <= u_radius; ++k) {
        ivec2 q = clamp(p + (u_axis == 0 ? ivec2(k, 0) : ivec2(0, k)), ivec2(0), size - 1);
        sum += floor(texelFetch(u_tex, q, 0) * 255.0 + 0.5);
    }
    // Guard integer quotients from reciprocal roundoff; 0.0001 < 1/513.
    out_color = floor((sum + float(u_radius)) / float(2 * u_radius + 1) + 0.0001) / 255.0;
}
