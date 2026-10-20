#version 450
// Premultiplied texture times vertex colour; untextured geometry samples a 1x1 white texture.
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 frag;
layout(set = 2, binding = 0) uniform sampler2D uTex;
void main() {
    frag = texture(uTex, vUv) * vCol;
}
