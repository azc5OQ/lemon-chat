cbuffer Globals : register(b0) {
    float4 u_viewport_and_pad;
};

// A layer quad: the position is homogeneous (x * w, y * w, w) in device
// pixels so a perspective transform interpolates the texture correctly;
// the group opacity rides on the vertex (no second constant buffer, so an
// embedded host's pixel-shader slots stay untouched).
struct VSIn {
    float3 pos     : POSITION;
    float2 uv      : TEXCOORD0;
    float  opacity : TEXCOORD1;
};
struct VSOut {
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float  opacity : TEXCOORD1;
};

VSOut vs_main(VSIn i) {
    VSOut o;
    float2 vp = u_viewport_and_pad.xy;
    // device pixels to clip space, keeping w: ndc = pos / vp * 2 - 1
    float x = (i.pos.x / vp.x) * 2.0 - i.pos.z;
    float y = i.pos.z - (i.pos.y / vp.y) * 2.0;
    o.pos     = float4(x, y, 0.0, i.pos.z);
    o.uv      = i.uv;
    o.opacity = i.opacity;
    return o;
}
