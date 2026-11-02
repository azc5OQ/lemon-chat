#version 450
layout(constant_id = 0) const uint target_srgb = 0u;
layout(set = 0, binding = 0) uniform sampler2D u_atlas;
layout(location = 0) in  vec4 v_color;
layout(location = 1) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    float a = texture(u_atlas, v_uv).r;
    if (a <= 0.0) discard;
    a = pow(a, 1.0 / 2.2);
    o_color = vec4(v_color.rgb, v_color.a * a);
    // The framebuffer performs linear-light blending and encodes its result.
    // Decode our byte-oriented source color first so opaque colors stay equal.
    if (target_srgb != 0u) {
        vec3 low = o_color.rgb / 12.92;
        vec3 high = pow((o_color.rgb + 0.055) / 1.055, vec3(2.4));
        o_color.rgb = mix(high, low, lessThanEqual(o_color.rgb, vec3(0.04045)));
    }
}
