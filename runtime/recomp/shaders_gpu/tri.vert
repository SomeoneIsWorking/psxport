#version 450
// SDL_GPU untextured PSX triangle (vertex): VRAM pixel coords (0..1023, 0..511) -> NDC; gouraud color
// passed through; per-vertex OT/native depth in i_ord carried into gl_Position.z. Pass 2a = 4:3, no scratch
// FB, so the VRAM render target is 1024x512: x maps over 1024 (÷512-1), y over 512 (÷256-1).
layout(location = 0) in vec2  i_pos;   // VRAM pixel coords (post draw-offset)
layout(location = 1) in vec3  i_col;   // 0..1 RGB
layout(location = 2) in float i_ord;   // depth [0,1] (band-mapped; later prim/nearer = greater)
layout(location = 0) out vec3 v_col;
void main() {
    v_col = i_col;
    gl_Position = vec4(i_pos.x / 512.0 - 1.0, i_pos.y / 256.0 - 1.0, i_ord, 1.0);
}
