sampler2D source : register(s0);
// Inverse allocation size, integer radius and axis; active-region UV bounds.
float4 parameters : register(c0);
float4 limits : register(c1);
float4 fetch(float2 uv) {
    return tex2Dlod(source, float4(clamp(uv, limits.xy, limits.zw), 0, 0));
}
float4 ps_main(float2 uv : TEXCOORD0) : COLOR0 {
    float2 step = parameters.w == 0 ? float2(parameters.x, 0) : float2(0, parameters.y);
    // Pair adjacent taps through exact half-texel linear interpolation.
    float4 sum = fetch(uv + step*parameters.z);
    [loop] for (int i = 0; i < min(parameters.z, 255.0); ++i) {
        sum += fetch(uv + step*(-parameters.z + 0.5 + 2.0*i)) * 2.0;
    }
    // Shader Model 3 has a 255-iteration loop limit.
    if (parameters.z > 255.0) sum += fetch(uv + step*254.5) * 2.0;
    sum = floor(sum*255.0 + 0.5);
    // Correct reciprocal-multiply roundoff at integer quotients. This bias
    // is smaller than the minimum nonzero fraction (1/513).
    return floor((sum + parameters.z) / (2.0*parameters.z + 1.0) + 0.0001) / 255.0;
}
