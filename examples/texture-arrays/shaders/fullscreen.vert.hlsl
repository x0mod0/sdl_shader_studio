// Covers the screen with a single triangle built from the vertex id, so the
// draw needs no vertex buffer and no input layout. Writes no varyings: both
// fragment shaders here read the fragment position instead.
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
    // (0,0) (2,0) (0,2) in UV space covers the screen with one triangle.
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
