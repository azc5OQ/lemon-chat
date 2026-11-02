// Layered premultiplied backgrounds. Conic angles are exact CPU coordinate
// fields; color ramps and rounded coverage follow the portable compositor.
Texture2D<uint> coordinate_pixels : register(t0);
RWTexture2D<uint> destination_pixels : register(u0);

struct Layer
{
    float4 tile; // x, y, width, height
    int4 info; // kind (-1 solid), repeat x, repeat y, stop count
    float4 geometry0; // center x/y, direction x/y
    float4 geometry1; // line, radius x/y, angle
    int4 extra; // repeating ramp, conic coordinate row offset
    float4 solid;
    float4 positions[4];
    float4 colors[16];
};
cbuffer Parameters : register(b0)
{
    int4 region; // device left, top, width, height
    float4 bounds;
    float4 base_color;
    float4 options; // radius, opacity, layer count
    float4 composition; // group opacity, blend mode, fuse into parent, unused
    Layer layers[16];
};

float stop_position(int layer, uint stop)
{
    return layers[layer].positions[stop / 4][stop % 4];
}

float4 ramp(int layer, precise float t)
{
    int count = layers[layer].info.w;
    if (count <= 0) return 0;
    precise float first = stop_position(layer, 0);
    if (layers[layer].extra.x != 0)
    {
        precise float period = stop_position(layer, count - 1) - first;
        if (period <= 0.000001) return layers[layer].colors[count - 1];
        t = first + (t - first - floor((t - first) / period) * period);
    }
    if (t < first) return layers[layer].colors[0];
    [loop] for (int i = 1; i < count; ++i)
    {
        precise float next = stop_position(layer, i);
        if (t < next)
        {
            precise float previous = stop_position(layer, i - 1);
            precise float span = next - previous;
            precise float f = span > 0 ? (t - previous) / span : 1.0;
            precise float4 a = layers[layer].colors[i - 1];
            precise float4 b = layers[layer].colors[i];
            precise float3 rgb = a.rgb * (1.0 - f) + b.rgb * f;
            precise float alpha = a.a + (b.a - a.a) * f;
            return float4(rgb, alpha);
        }
    }
    return layers[layer].colors[count - 1];
}

float4 sample_layer(int layer, precise float2 p, uint2 pixel)
{
    precise float t = 0;
    if (layers[layer].info.x == 2)
    {
        t = asfloat(coordinate_pixels.Load(int3(pixel.x, pixel.y + layers[layer].extra.y, 0)));
        if (t == 2.0) return 0; // outside a non-repeated tile
        t -= layers[layer].geometry1.w;
        t -= floor(t);
    }
    else
    {
        precise float4 tile = layers[layer].tile;
        p -= tile.xy;
        if (tile.z <= 0 || tile.w <= 0) return 0;
        if (layers[layer].info.y != 0) p.x -= floor(p.x / tile.z) * tile.z;
        if (layers[layer].info.z != 0) p.y -= floor(p.y / tile.w) * tile.w;
        if (any(p < 0) || any(p >= tile.zw)) return 0;
        if (layers[layer].info.x < 0) return layers[layer].solid;
        p -= layers[layer].geometry0.xy;
        if (layers[layer].info.x == 1)
        {
            p /= layers[layer].geometry1.yz;
            t = sqrt(p.x * p.x + p.y * p.y);
        }
        else
        {
            t = 0.5 + (p.x * layers[layer].geometry0.z + p.y * layers[layer].geometry0.w) /
                max(layers[layer].geometry1.x, 0.0001);
        }
    }
    return ramp(layer, t);
}

float rounded(precise float2 p)
{
    precise float radius = min(max(options.x, 0), min(bounds.z, bounds.w) * 0.5);
    precise float2 q = abs(p - bounds.zw * 0.5) - bounds.zw * 0.5 + radius;
    if (q.x <= 0 && q.y <= 0 && max(q.x, q.y) <= radius - 0.5) return 1;
    precise float2 corner = max(q, 0);
    precise float d = min(max(q.x, q.y), 0) + sqrt(corner.x * corner.x + corner.y * corner.y) - radius;
    precise float t = saturate(d + 0.5);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x >= (uint)region.z || thread.y >= (uint)region.w) return;
    precise float2 p = float2(region.xy) + float2(thread.xy) + 0.5 - bounds.xy;
    precise float4 color = base_color;
    [loop] for (int i = (int)options.z - 1; i >= 0; --i)
    {
        precise float4 value = sample_layer(i, p, thread.xy);
        color = value + color * (1.0 - value.a);
    }
    precise float coverage = rounded(p) * options.y;
    precise float alpha = color.a * coverage;
    if (alpha <= 0) return;
    uint packed = destination_pixels[thread.xy];
    float4 destination = float4(packed & 255, (packed >> 8) & 255, (packed >> 16) & 255, packed >> 24);
    if (composition.z != 0)
    {
        // Preserve the intermediate transparent surface's RGBA8 rounding even
        // though that surface is never transferred to or from the CPU.
        uint4 leaf = (uint4)(float4(min(color.rgb, color.aaa) * coverage, alpha) * 255.0 + 0.5);
        color = float4(leaf) / 255.0;
        if (color.a <= 0) return;
        alpha = color.a * composition.x;
        precise float remaining = 1.0 - alpha;
        precise float3 rgb;
        int blend = (int)composition.y;
        if (blend == 0)
        {
            rgb = min(color.rgb, color.aaa) * composition.x * 255.0 + destination.rgb * remaining;
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
        return;
    }
    precise float remaining = 1.0 - alpha;
    precise float3 rgb = min(color.rgb, color.aaa) * coverage * 255.0 + destination.rgb * remaining;
    precise float output_alpha = alpha * 255.0 + destination.a * remaining;
    uint4 result = (uint4)(float4(rgb, output_alpha) + 0.5);
    destination_pixels[thread.xy] = result.x | (result.y << 8) | (result.z << 16) | (result.w << 24);
}
