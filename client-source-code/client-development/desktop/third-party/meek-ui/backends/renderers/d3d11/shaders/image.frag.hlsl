Texture2D    u_tex  : register(t0);
SamplerState u_samp : register(s0);

struct PSIn {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD0;
};

float4 ps_main(PSIn i) : SV_TARGET {
    float4 s = u_tex.Sample(u_samp, i.uv);
    return s * i.col;
}
