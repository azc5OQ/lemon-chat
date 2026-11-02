Texture2D    u_tex  : register(t0);
SamplerState u_samp : register(s0);

struct PSIn {
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float  opacity : TEXCOORD1;
};

// premultiplied texel times the group opacity; the blend state does the rest
float4 ps_main(PSIn i) : SV_TARGET {
    return u_tex.Sample(u_samp, i.uv) * i.opacity;
}
