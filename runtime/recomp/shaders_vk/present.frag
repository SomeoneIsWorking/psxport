#version 450
// Present pass: sample the uploaded display-region texture (already RGBA8 from VRAM).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
void main() {
    o_col = texture(u_tex, v_uv);
}
