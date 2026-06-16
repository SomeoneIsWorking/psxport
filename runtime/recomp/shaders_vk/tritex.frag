#version 450
// Textured PSX fragment: sample VRAM (R16_UINT 1555) with 4/8/16bpp + CLUT, transparency, modulation.
layout(location = 0) in vec3 v_col;
layout(location = 1) noperspective in vec2 v_uv;
layout(location = 2) flat in ivec4 v_tp;     // tpx, tpy, mode, raw
layout(location = 3) flat in ivec4 v_clut;   // clutx, cluty
layout(location = 4) flat in ivec4 v_tw;     // texture window mask_x, mask_y, off_x, off_y
layout(location = 5) flat in ivec4 v_da;     // draw-area clip x0,y0,x1,y1
layout(set = 0, binding = 0) uniform usampler2D u_vram;
layout(location = 0) out uint o_px;

uint vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r; }

void main() {
    int px = int(gl_FragCoord.x), py = int(gl_FragCoord.y);     // VRAM pixel (viewport = full VRAM)
    if (px < v_da.x || px > v_da.z || py < v_da.y || py > v_da.w) discard;   // draw-area clip (SW parity)
    int u = int(v_uv.x), v = int(v_uv.y);
    u = (u & ~(v_tw.x * 8)) | ((v_tw.z & v_tw.x) * 8);     // texture-window wrap (matches SW sample_tex)
    v = (v & ~(v_tw.y * 8)) | ((v_tw.w & v_tw.y) * 8);
    u &= 255; v &= 255;
    int tpx = v_tp.x, tpy = v_tp.y, mode = v_tp.z;
    int clutx = v_clut.x, cluty = v_clut.y;
    uint texel;
    if (mode == 0) {                                  // 4bpp: 4 texels per 16-bit word
        uint w = vram_at(tpx + (u >> 2), tpy + v);
        uint idx = (w >> ((u & 3) * 4)) & 0xFu;
        texel = vram_at(clutx + int(idx), cluty);
    } else if (mode == 1) {                           // 8bpp: 2 texels per word
        uint w = vram_at(tpx + (u >> 1), tpy + v);
        uint idx = (w >> ((u & 1) * 8)) & 0xFFu;
        texel = vram_at(clutx + int(idx), cluty);
    } else {                                          // 15bpp direct
        texel = vram_at(tpx + u, tpy + v);
    }
    if (texel == 0u) discard;                         // 0x0000 = fully transparent texel

    uint tr = texel & 31u, tg = (texel >> 5) & 31u, tb = (texel >> 10) & 31u, stp = (texel >> 15) & 1u;
    if (v_tp.w == 0) {                                // modulate by vertex color (PSX: tex*col/128)
        tr = min(31u, uint(float(tr) * v_col.r * (255.0 / 128.0) + 0.5));
        tg = min(31u, uint(float(tg) * v_col.g * (255.0 / 128.0) + 0.5));
        tb = min(31u, uint(float(tb) * v_col.b * (255.0 / 128.0) + 0.5));
    }
    o_px = tr | (tg << 5) | (tb << 10) | (stp << 15);
}
