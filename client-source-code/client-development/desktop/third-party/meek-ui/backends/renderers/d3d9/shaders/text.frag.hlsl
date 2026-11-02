sampler2D u_atlas : register(s0);

struct PSIn {
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

float4 ps_main(PSIn i) : COLOR {
    float a = tex2D(u_atlas, i.uv).r;
    if (a <= 0.0) discard;
    a = pow(a, 1.0 / 2.2);
    return float4(i.col.rgb, i.col.a * a);
}
