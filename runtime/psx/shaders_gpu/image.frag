#version 450
// The source part of an RGBA8 image scaled by a 0..1 fade, opaque.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_img;
layout(set = 3, binding = 0) uniform PC { vec4 fade; vec4 source; } pc; // fade in .x; source uv origin in .xy, size in .zw
void main() {
    vec4 t = texture(u_img, pc.source.xy + v_uv * pc.source.zw);
    o_col = vec4(t.rgb * pc.fade.x, 1.0);
}
