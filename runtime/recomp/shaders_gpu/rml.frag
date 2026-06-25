#version 450
// RmlUi 2D overlay fragment stage (SDL_GPU). Both texture and vertex colour are premultiplied alpha; the
// pipeline blends with (ONE, ONE_MINUS_SRC_ALPHA). Untextured geometry samples a 1x1 white texture.
//
// SDL_GPU binding convention: FRAGMENT samplers live in set=2 (bound via SDL_BindGPUFragmentSamplers).
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 frag;
layout(set = 2, binding = 0) uniform sampler2D uTex;
void main() {
    frag = texture(uTex, vUv) * vCol;
}
