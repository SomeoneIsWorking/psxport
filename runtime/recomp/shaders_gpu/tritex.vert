#version 450
// SDL_GPU textured PSX triangle (vertex). VRAM coords; UV is AFFINE (noperspective) to match PSX mapping;
// per-prim page/CLUT/window/clip state is flat-interpolated. Pass 2a = 4:3 (target 1024x512, no scratch FB).
layout(location = 0) in vec2  i_pos;    // VRAM pixel coords (post draw-offset)
layout(location = 1) in vec2  i_uv;     // texel coords within the page (0..255)
layout(location = 2) in vec3  i_col;    // modulation color 0..1 (128/255 = neutral)
layout(location = 3) in ivec4 i_tp;     // tpx, tpy, mode(0=4bpp,1=8bpp,2=15bpp,3=untex), raw
layout(location = 4) in ivec4 i_clut;   // clutx, cluty, semi, blend
layout(location = 5) in ivec4 i_tw;     // texture window: mask_x, mask_y, off_x, off_y (8px units)
layout(location = 6) in ivec4 i_da;     // draw-area clip: x0, y0, x1, y1 (VRAM px)
layout(location = 7) in float i_ord;    // depth [0,1]
layout(location = 0) out vec3 v_col;
layout(location = 1) noperspective out vec2 v_uv;
layout(location = 2) flat out ivec4 v_tp;
layout(location = 3) flat out ivec4 v_clut;
layout(location = 4) flat out ivec4 v_tw;
layout(location = 5) flat out ivec4 v_da;
void main() {
    v_col = i_col; v_uv = i_uv; v_tp = i_tp; v_clut = i_clut; v_tw = i_tw; v_da = i_da;
    gl_Position = vec4(i_pos.x / 512.0 - 1.0, i_pos.y / 256.0 - 1.0, i_ord, 1.0);
}
