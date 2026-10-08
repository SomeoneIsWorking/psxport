#version 450
// SDL_GPU PANE pass (vertex): a destination RECTANGLE drawn from no vertex buffer at all.
//
// Two triangles cover any quad exactly, and UVs stay affine along it: this maps the unit square
// (u across, v down) onto the pane's own upright rectangle, `uOrigin + u*uAxisU + v*uAxisV`, in sink
// pixels with y DOWN (the host's own convention — the same one rml.vert speaks). Negating clip-space
// Y puts pixel y=0 at the top, exactly like present.vert and rml.vert.
//
// WHY A RECTANGLE AND NOT A SKEWED QUAD. A pane used to be drawn as the affine image of its own
// four corners, so a slanted seam sheared the picture inside it: the guest's geometry came out
// leaning with the seam, which is wrong — the game is upright and the seam is decoration. The shape
// of a pane is now decided where the shape is authored, in the FRAGMENT stage: this shader hands
// the fragment its own x in sink pixels, and pane.frag drops whatever falls outside the seam lines.
// Everything a pane draws is therefore axis-aligned and unsheared, and the only slanted thing on
// screen is the seam itself, cut exactly where the host said it is.
//
// The varying is exact, not an approximation: clip x is affine in p.x, so interpolating the sink
// coordinate across the primitive recovers the fragment's own pixel column to the bit.
//
// SDL_GPU binding convention: VERTEX uniform buffers live in set=1, fed once per draw by
// SDL_PushGPUVertexUniformData(cmd, slot 0, &ubo, sizeof ubo).
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
    // Two triangles over the corners TL, TR, BR, BL of the unit square: 0,1,2 and 0,2,3.
    //
    // BOTH must wind the SAME way. The corner pattern 0,1,2 then 2,1,3 gives the second triangle the
    // opposite orientation — with backface culling on, one of the two is dropped and a pane renders
    // as a notched quad with a triangular bite out of it, which is what a broken pane looks like.
    const int kOrder[6] = int[6](0, 1, 2, 0, 2, 3);
    const int corner = kOrder[gl_VertexIndex];
    const float u = (corner == 1 || corner == 2) ? 1.0 : 0.0;
    const float v = (corner >= 2) ? 1.0 : 0.0;
    vec2 p = ubo.uOrigin + u * ubo.uAxisU + v * ubo.uAxisV;
    gl_Position = vec4(2.0 * p.x / ubo.uViewport.x - 1.0, -(2.0 * p.y / ubo.uViewport.y - 1.0), 0.0, 1.0);
    v_uv = vec2(u, v);
    v_x = p.x;
    // THE SEAM IS SURFACE-RELATIVE, NOT PANE-RELATIVE. The host authors a seam's two endpoints
    // as its column at the surface's top and its column at the surface's bottom, so which side of
    // the line a fragment falls on is decided by where the fragment is IN THE SURFACE. A sub-rect
    // pane (a logo over its panel) evaluating that against its own local v would pick up the seam
    // columns from the wrong rows and cut the pane along a line that is not the one the neighbours
    // share. v_x/v_down are exact, so the interpolation of clip-space x and y recovers this bit-exact.
    v_down = p.y / ubo.uViewport.y;
}