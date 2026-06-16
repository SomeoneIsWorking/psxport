#version 450
// PSX triangle: vertices in VRAM pixel coords (0..1023, 0..511) -> NDC; gouraud color passed through.
layout(location = 0) in vec2 i_pos;   // VRAM pixel coords (post draw-offset)
layout(location = 1) in vec3 i_col;   // 0..1 RGB (flat = same on all 3 verts)
layout(location = 0) out vec3 v_col;
void main() {
    v_col = i_col;
    gl_Position = vec4(i_pos.x / 512.0 - 1.0, i_pos.y / 256.0 - 1.0, 0.0, 1.0);
}
