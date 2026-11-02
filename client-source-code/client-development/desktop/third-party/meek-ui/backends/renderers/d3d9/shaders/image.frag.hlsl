sampler2D u_tex : register(s0);

struct PSIn {
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

float4 ps_main(PSIn i) : COLOR {
    float4 s = tex2D(u_tex, i.uv);
    return s * i.col;
}
