// Premultiplied RGBA8 composition. Manual sampling follows the portable CPU
// compositor, including clamped texels, projective transforms and byte rounding.
Texture2D<uint> source_pixels : register(t0);
RWTexture2D<uint> destination_pixels : register(u0);
cbuffer Parameters : register(b0)
{
    float4 inverse0;
    float4 inverse1;
    float4 inverse2;
    int4 source_info; // width, height, device origin x, y
    int4 region; // device left, top, width, height
    float4 options; // opacity, blend mode, aligned, unused
};

float4 unpack(uint pixel)
{
    return float4(pixel & 255, (pixel >> 8) & 255, (pixel >> 16) & 255, pixel >> 24);
}

float4 fetch(int2 position)
{
    return unpack(source_pixels.Load(int3(clamp(position, int2(0, 0), source_info.xy - 1), 0)));
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= (uint)region.z || thread.y >= (uint)region.w) return;
    precise float2 p = float2(region.xy) + float2(thread.xy) + 0.5;
    precise float denominator = inverse2.x * p.x + inverse2.y * p.y + inverse2.z;
    if (abs(denominator) < 0.00001) denominator = 0.00001;
    precise float2 position = float2((inverse0.x * p.x + inverse0.y * p.y + inverse0.z) / denominator,
        (inverse1.x * p.x + inverse1.y * p.y + inverse1.z) / denominator) - float2(source_info.zw);
    if (any(position < 0) || any(position >= float2(source_info.xy))) return;
    precise float4 color;
    if (options.z != 0)
    {
        color = fetch(int2(position)) / 255.0;
    }
    else
    {
        position -= 0.5;
        int2 base = int2(floor(position));
        precise float2 f = position - float2(base);
        precise float w00 = (1.0 - f.x) * (1.0 - f.y) / 255.0;
        precise float w10 = f.x * (1.0 - f.y) / 255.0;
        precise float w01 = (1.0 - f.x) * f.y / 255.0;
        precise float w11 = f.x * f.y / 255.0;
        color = fetch(base) * w00 + fetch(base + int2(1,0)) * w10 +
            fetch(base + int2(0,1)) * w01 + fetch(base + int2(1,1)) * w11;
    }
    if (color.a <= 0) return;
    float4 destination = unpack(destination_pixels[thread.xy]);
    precise float alpha = color.a * options.x;
    precise float remaining = 1.0 - alpha;
    precise float3 rgb;
    int blend = (int)options.y;
    if (blend == 0)
    {
        rgb = min(color.rgb, color.aaa) * options.x * 255.0 + destination.rgb * remaining;
    }
    else
    {
        float3 front = color.rgb / color.a;
        float3 back = destination.a > 0 ? destination.rgb / destination.a : float3(0,0,0);
        float3 mixed = front;
        if (blend == 1) mixed = 1.0 - (1.0 - front) * (1.0 - back);
        if (blend == 2) mixed = front * back;
        if (blend == 3) mixed = float3(back.r <= 0.5 ? 2*front.r*back.r : 1-2*(1-front.r)*(1-back.r),
            back.g <= 0.5 ? 2*front.g*back.g : 1-2*(1-front.g)*(1-back.g),
            back.b <= 0.5 ? 2*front.b*back.b : 1-2*(1-front.b)*(1-back.b));
        if (blend == 4) mixed = abs(front - back);
        rgb = saturate(front + (mixed - front) * destination.a / 255.0) * alpha * 255.0 + destination.rgb * remaining;
    }
    uint4 result = (uint4)(float4(rgb, alpha * 255.0 + destination.a * remaining) + 0.5);
    destination_pixels[thread.xy] = result.x | (result.y << 8) | (result.z << 16) | (result.w << 24);
}
