cbuffer RootCB : register(b0) { float2 u_viewport; float2 _pad; };
Texture2D    u_tex  : register(t0);
SamplerState u_samp : register(s0);

struct VSIn {
    float2 pos : POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float2 ndc = float2(
        (v.pos.x / u_viewport.x) * 2.0 - 1.0,
        1.0 - (v.pos.y / u_viewport.y) * 2.0
    );
    o.pos = float4(ndc, 0.0, 1.0);
    o.col = v.col;
    o.uv  = v.uv;
    return o;
}

float4 PSText(VSOut i) : SV_Target
{
    float a = u_tex.Sample(u_samp, i.uv).r;
    if (a <= 0.0) discard;
    a = pow(a, 1.0 / 2.2);
    return float4(i.col.rgb, i.col.a * a);
}

float4 PSImage(VSOut i) : SV_Target
{
    float4 s = u_tex.Sample(u_samp, i.uv);
    return s * i.col;
}
