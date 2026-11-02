cbuffer Globals : register(b0) {
    float4 u_viewport_and_pad;
};

struct VSIn {
    float2 pos : POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float4 col : COLOR;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(VSIn i) {
    VSOut o;
    float2 vp = u_viewport_and_pad.xy;
    float2 ndc = float2(
        (i.pos.x / vp.x) * 2.0 - 1.0,
        1.0 - (i.pos.y / vp.y) * 2.0
    );
    o.pos = float4(ndc, 0.0, 1.0);
    o.col = i.col;
    o.uv  = i.uv;
    return o;
}
