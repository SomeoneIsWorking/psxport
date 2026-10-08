#version 450
// SDL_GPU PANE pass (fragment): sample ONE pane's presented picture over the sub-rectangle the pane
// draws from, tint it, desaturate it, and cut its seams.
//
// THE SEAM MASK. `uSeam` is the pane's own shape: its left and right boundaries, each given as the
// sink-pixel column it sits in at the TOP of the pane and at the BOTTOM of it. A boundary that leans
// is one line, and everything outside the two is dropped in the fragment stage. This is what lets a
// row of panels have slanted seams while every picture inside them stays UPRIGHT and UNSHEARED —
// the picture is a plain axis-aligned rectangle and the seam is cut out of it, instead of the whole
// quad being sheared to make the seam. The host decides where the seams are; this only decides which
// side of them a fragment is on. A pane with no seams names them at the edges of its own rectangle
// and so draws all of it.
//
// A panel that is not selected is GREY, not dimmed: `uTint.w` is the amount of the picture's own
// colour to replace with its luminance, so 1 is fully grey and 0 is untouched. `uTint.rgb` scales
// the channels and is how an authored line, frame or backdrop is drawn from the same pipeline (a 1x1
// white source texture and the colour in the tint), which is why this pass has no second pipeline.
//
// A pane's alpha is 1 unless it is drawn ON TOP of one — a logo or a strip over a running demo.
// Picture panes own their pixels; only a deliberately drawn pane blends.
//
// A pane with no picture never reaches this shader; the compositor skips the draw instead of showing
// an empty frame that looks like a title that failed to boot.
//
// SDL_GPU bindings: fragment SAMPLERS live in set=2, fragment UNIFORM BUFFERS in set=3.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_x; // this fragment's own sink-pixel column
layout(location = 2) in float v_down;  // this fragment's own row, as a fraction of the surface's height
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_pane;
layout(set = 3, binding = 0) uniform PC {
    vec4 uSource; // xy = the source rect's origin in UV, zw = its size in UV
    vec4 uTint;   // rgb = channel scale, w = desaturation
    vec4 uSeam;   // left top, left bottom, right top, right bottom — sink pixels
    float uAlpha; // 1 for a pane that owns its pixels; below 1 for one drawn OVER another
} pc;
void main() {
    // The pane's two endpoints per boundary are its columns at the SURFACE's top and bottom,
    // so the interpolation runs over the fragment's own row in the surface, not over the pane's
    // own local v: a sub-rect pane (a logo) would otherwise see the wrong rows of the seam line.
    // A full-height pane's local v and the surface row are the same number, so panes that span the
    // surface are unchanged. Outside the two boundaries, the pane is not here.
    float down = clamp(v_down, 0.0, 1.0);
    float left = mix(pc.uSeam.x, pc.uSeam.y, down);
    float right = mix(pc.uSeam.z, pc.uSeam.w, down);
    if (v_x < left || v_x > right) {
        discard;
    }
    vec2 uv = pc.uSource.xy + v_uv * pc.uSource.zw;
    vec4 texel = texture(u_pane, uv);
    vec3 rgb = texel.rgb * pc.uTint.rgb;
    const float grey = dot(rgb, vec3(0.299, 0.587, 0.114));
    // THE TEXTURE'S OWN ALPHA IS PART OF WHAT THIS PANE WRITES. A host texture can have real
    // transparency — a title's logo decoded off its own disc, keyed on the game's untouched black —
    // and a pane that took only rgb drew every one of those pixels at full opacity, which is how a
    // logo arrived as a black rectangle lying on a running demo. `uAlpha` is the pane's OWN opacity
    // (a logo drawn over a picture may be partly transparent); the sample's alpha is the picture's,
    // and the two multiply because they are two reasons for the same pixel to be see-through.
    o_col = vec4(mix(rgb, vec3(grey), clamp(pc.uTint.w, 0.0, 1.0)), pc.uAlpha * texel.a);
}