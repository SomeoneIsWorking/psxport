#version 450
// PC-native fullscreen IMAGE present: sample an RGBA8 (UNORM) texture at uv (linear filter), multiply
// the rgb by a `fade` scalar (0..1) for a fade-in/out, output opaque. No PSX VRAM/1555/CLUT — a real
// image draw (gpu_vk_present_image), used for the SCEA boot splash.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_img;
layout(push_constant) uniform PC { float fade; } pc;
void main() {
    vec4 t = texture(u_img, v_uv);
    o_col = vec4(t.rgb * pc.fade, 1.0);
}
