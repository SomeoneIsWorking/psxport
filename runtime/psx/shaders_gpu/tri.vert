#version 450
// SDL_GPU untextured PSX triangle (vertex): VRAM pixel coords (0..1023, 0..511) -> NDC; gouraud color
// passed through; per-vertex OT/native depth in i_ord carried into gl_Position.z. Pass 2a = 4:3, no scratch
// FB, so the VRAM render target is 1024x512: x maps over 1024 (÷512-1), y over 512 (÷256-1).
layout(location = 0) in vec2 i_pos;  // VRAM pixel coords (post draw-offset)
layout(location = 1) in vec3 i_col;  // 0..1 RGB
layout(location = 2) in float i_ord; // depth [0,1] (band-mapped; later prim/nearer = greater)
layout(location = 3) in float i_gouraud;
layout(location = 4) in float i_dither;
layout(location = 5) in ivec4 i_da; // draw-area clip: x0, y0, x1, y1 (VRAM px)
layout(location = 0) out vec3 v_col;
layout(location = 1) flat out float v_gouraud;
layout(location = 2) flat out float v_dither;
layout(location = 3) flat out ivec4 v_da;
void main() {
  v_col = i_col;
  v_gouraud = i_gouraud;
  v_dither = i_dither;
  v_da = i_da;
  // Negate Y: SDL_GPU offscreen render targets are Y-up (NDC +1 = texture row 0), so VRAM row 0 must map
  // to NDC y=+1 to land at texture row 0 — matching the copy-uploaded 2D backdrop and the present sample
  // (else the rendered geometry is vertically flipped vs the backdrop and the in-shader blend reads the
  // wrong row). gl_FragCoord.y then equals the VRAM row (draw-area clip + blend depend on this).
  // Vulkan samples pixel centers at n+0.5; shift PSX integer-coordinate coverage onto those centers.
  vec2 psx_sample_center = i_pos + vec2(0.5);
  gl_Position = vec4(psx_sample_center.x / 512.0 - 1.0, -(psx_sample_center.y / 256.0 - 1.0), i_ord, 1.0);
}
