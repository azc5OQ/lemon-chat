sampler2D layer_texture : register(s0);
struct PSIn { float2 uv : TEXCOORD0; float opacity : TEXCOORD1; };
float4 ps_main(PSIn i) : COLOR {
    // The texture is already premultiplied. Opacity scales all channels;
    // render state supplies source-over, screen, replace or multiply.
    return tex2D(layer_texture, i.uv) * i.opacity;
}
