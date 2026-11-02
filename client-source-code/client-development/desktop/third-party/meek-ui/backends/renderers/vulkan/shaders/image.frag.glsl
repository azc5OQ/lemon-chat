#version 450
layout(constant_id = 0) const uint target_srgb = 0u;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec4 s = texture(u_tex, v_uv);
    o_color = s * v_color;
    // The framebuffer performs linear-light blending and encodes its result.
    // Decode our byte-oriented source color first so opaque colors stay equal.
    if (target_srgb != 0u) {
        vec3 low = o_color.rgb / 12.92;
        vec3 high = pow((o_color.rgb + 0.055) / 1.055, vec3(2.4));
        o_color.rgb = mix(high, low, lessThanEqual(o_color.rgb, vec3(0.04045)));
    }
}
