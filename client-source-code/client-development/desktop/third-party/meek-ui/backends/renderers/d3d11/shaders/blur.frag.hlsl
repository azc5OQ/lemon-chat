Texture2D<float4> source : register(t0);
SamplerState linear_clamp : register(s0);
cbuffer Blur : register(b0) { int width; int height; int radius; int axis; };

// A linear sample halfway between adjacent texels gives their average. Pair
// taps to halve texture traffic, then recover the integer byte sum before the
// CPU contract's per-axis division. Coordinates clamp to the active region,
// which can be smaller than the retained texture allocation.
float4 ps_main(float4 position : SV_POSITION) : SV_TARGET {
    uint tw, th;
    source.GetDimensions(tw, th);
    float2 inverse = 1.0 / float2(tw, th);
    float2 direction = axis == 0 ? float2(1, 0) : float2(0, 1);
    float2 high = float2(width, height) - 0.5;
    float2 p = floor(position.xy) + 0.5;
    float4 sum = source.SampleLevel(linear_clamp, clamp(p + direction*radius, 0.5, high)*inverse, 0);
    [loop] for (int k = -radius; k < radius; k += 2) {
        float2 q = clamp(p + direction*(float(k) + 0.5), 0.5, high);
        sum += source.SampleLevel(linear_clamp, q*inverse, 0) * 2.0;
    }
    sum = floor(sum * 255.0 + 0.5);
    // Correct reciprocal-multiply roundoff at integer quotients. This bias
    // is smaller than the minimum nonzero fraction (1/513).
    return floor((sum + radius) / (2 * radius + 1) + 0.0001) / 255.0;
}
