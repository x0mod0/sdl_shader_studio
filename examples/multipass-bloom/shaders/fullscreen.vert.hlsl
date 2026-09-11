// Covers the screen with a single triangle built from the vertex id, so the
// draw needs no vertex buffer and no input layout. Writes no varyings: every
// fragment shader in this chain reads SV_Position instead.
//
// One vertex shader serves the whole pipeline. Passes differ in what they
// compute, not in the geometry they cover - which is why PassChain carries a
// single `vertex_shader` rather than one per pass.
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
    // (0,0) (2,0) (0,2) in UV space covers the screen with one triangle.
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
