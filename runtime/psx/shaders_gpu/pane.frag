#version 450
// One pane's picture: its source rect, tinted and desaturated, cut to its seam lines. A 1x1 white
// source draws authored lines and backdrops through the tint.
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
    // Seam endpoints are the boundary's columns at the surface's top and bottom.
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
    // The pane's opacity and the picture's own transparency both apply.
    o_col = vec4(mix(rgb, vec3(grey), clamp(pc.uTint.w, 0.0, 1.0)), pc.uAlpha * texel.a);
}