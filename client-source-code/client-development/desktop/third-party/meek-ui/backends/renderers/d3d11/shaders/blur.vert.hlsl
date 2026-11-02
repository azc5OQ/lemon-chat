// Fullscreen triangle, without a vertex buffer or per-frame vertex upload.
float4 vs_main(uint vertex : SV_VertexID) : SV_POSITION {
    return float4(vertex == 2 ? 3.0 : -1.0, vertex == 1 ? 3.0 : -1.0, 0, 1);
}
