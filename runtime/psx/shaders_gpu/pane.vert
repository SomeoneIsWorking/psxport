#version 450
// A pane: the unit square onto its upright sink-pixel rectangle (y down), from no vertex buffer.
layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_x; // the fragment's own sink-pixel column
layout(location = 2) out float v_down; // the fragment's own row, as a fraction of the surface's height
layout(set = 1, binding = 0) uniform UBO {
    vec2 uOrigin;   // the pane's top-left corner, sink pixels
    vec2 uAxisU;    // destination vector ACROSS the top edge (the pane's width)
    vec2 uAxisV;    // destination vector DOWN the left edge (the pane's height)
    vec2 uViewport; // the sink size the two axes are expressed in
} ubo;
void main() {
    // Corners TL, TR, BR, BL as 0,1,2 and 0,2,3: both triangles wind the same way.
    const int kOrder[6] = int[6](0, 1, 2, 0, 2, 3);
    const int corner = kOrder[gl_VertexIndex];
    const float u = (corner == 1 || corner == 2) ? 1.0 : 0.0;
    const float v = (corner >= 2) ? 1.0 : 0.0;
    vec2 p = ubo.uOrigin + u * ubo.uAxisU + v * ubo.uAxisV;
    gl_Position = vec4(2.0 * p.x / ubo.uViewport.x - 1.0, -(2.0 * p.y / ubo.uViewport.y - 1.0), 0.0, 1.0);
    v_uv = vec2(u, v);
    v_x = p.x;
    // Seams are authored in surface rows, not the pane's own.
    v_down = p.y / ubo.uViewport.y;
}