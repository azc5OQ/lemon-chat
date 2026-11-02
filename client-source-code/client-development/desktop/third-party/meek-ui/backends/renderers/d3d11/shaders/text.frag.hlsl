Texture2D    u_atlas : register(t0);
SamplerState u_samp  : register(s0);

struct PSIn {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD0;
};

float4 ps_main(PSIn i) : SV_TARGET {
    float a = u_atlas.Sample(u_samp, i.uv).r;
    if (a <= 0.0) discard;
    a = pow(a, 1.0 / 2.2);
    return float4(i.col.rgb, i.col.a * a);
}
