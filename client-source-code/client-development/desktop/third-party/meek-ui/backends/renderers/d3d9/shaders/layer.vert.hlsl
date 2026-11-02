float4 viewport : register(c0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float opacity : TEXCOORD1; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float opacity : TEXCOORD1; };
VSOut vs_main(VSIn i) {
    VSOut o;
    // Homogeneous pixel coordinates retain perspective interpolation. D3D9
    // places pixel centers at integers, so apply its half-pixel correction
    // before dividing by w (the correction must itself be multiplied by w).
    float2 pixel = i.pos.xy - 0.5 * i.pos.z;
    o.pos = float4(pixel.x / viewport.x * 2.0 - i.pos.z,
                   i.pos.z - pixel.y / viewport.y * 2.0, 0, i.pos.z);
    o.uv = i.uv;
    o.opacity = i.opacity;
    return o;
}
