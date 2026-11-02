float4 u_viewport_and_pad : register(c0);

struct VSIn {
    float2 pos : POSITION;
    float4 col : COLOR0;
    float2 lc  : TEXCOORD0;
    float2 rs  : TEXCOORD1;
    float  rad : TEXCOORD2;
};
struct VSOut {
    float4 pos : POSITION;
    float4 col : COLOR0;
    float2 lc  : TEXCOORD0;
    float2 rs  : TEXCOORD1;
    float  rad : TEXCOORD2;
};

VSOut vs_main(VSIn i) {
    VSOut o;
    float2 vp = u_viewport_and_pad.xy;
    // d3d9 pixel-center correction: shift by -0.5px.
    float2 adj = i.pos - float2(0.5, 0.5);
    float2 ndc = float2(
        (adj.x / vp.x) * 2.0 - 1.0,
        1.0 - (adj.y / vp.y) * 2.0
    );
    o.pos = float4(ndc, 0.0, 1.0);
    o.col = i.col;
    o.lc  = i.lc;
    o.rs  = i.rs;
    o.rad = i.rad;
    return o;
}
