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
layout(location = 8) in ivec4 i_uvbb;   // this primitive's vertex-UV bounds (see TexVtx)
layout(location = 0) out vec3 v_col;
layout(location = 1) noperspective out vec2 v_uv;
layout(location = 2) flat out ivec4 v_tp;
layout(location = 3) flat out ivec4 v_clut;
layout(location = 4) flat out ivec4 v_tw;
layout(location = 5) flat out ivec4 v_da;
layout(location = 6) flat out ivec4 v_uvbb;
void main() {
    v_col = i_col; v_uv = i_uv; v_tp = i_tp; v_clut = i_clut; v_tw = i_tw; v_da = i_da; v_uvbb = i_uvbb;
    // Negate Y: SDL_GPU offscreen targets are Y-up, so VRAM row 0 -> NDC y=+1 -> texture row 0 (matches the
    // uploaded backdrop). gl_FragCoord.y then equals the VRAM row, which the draw-area clip + in-shader
    // semi blend (vram_at(px,py)) rely on. See tri.vert.
    // PSX coverage is evaluated at the pixel's INTEGER coordinate, while Vulkan samples pixel centres.
    // Shift by half a native pixel exactly as tri.vert does, so both pipelines carry one coverage rule.
    // psxUvAtIntegerPixel() rewinds UV against this same convention.
    vec2 psx_sample_center = i_pos + vec2(0.5);
    gl_Position = vec4(psx_sample_center.x / 512.0 - 1.0, -(psx_sample_center.y / 256.0 - 1.0), i_ord, 1.0);
}
