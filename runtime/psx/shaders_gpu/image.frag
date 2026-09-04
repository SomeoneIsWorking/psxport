#version 450
// SDL_GPU fullscreen IMAGE present (fragment): sample an RGBA8 (UNORM) texture (linear), scale rgb by a
// `fade` scalar (0..1), output opaque. No PSX VRAM/1555/CLUT — a real image draw (SCEA splash). SDL_GPU
// bindings: fragment sampler set=2, fragment uniform buffer set=3 (fade pushed via FragmentUniformData).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 2, binding = 0) uniform sampler2D u_img;
layout(set = 3, binding = 0) uniform PC { vec4 fade; } pc;   // fade scalar in .x (vec4 for 16B alignment)
void main() {
    vec4 t = texture(u_img, v_uv);
    o_col = vec4(t.rgb * pc.fade.x, 1.0);
}
