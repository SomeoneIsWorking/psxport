#version 450
// SDL_GPU textured/untextured PSX fragment with semi-transparency. The color target is an R16_UINT VRAM
// image — the fragment OUTPUTS the packed 1555 word as a uint. The CLUT/paletted sampling (usampler2D /
// texelFetch, modes 0/1/2, texture-window masking) is LIFTED VERBATIM from the working Vulkan tritex.frag.
//
// SEMI BLEND IS IN-SHADER (SDL_GPU has no HW blend on integer targets / no format alias): a blending
// fragment samples the VRAM SNAPSHOT (u_vram) at its own pixel as the destination and composites per the
// PSX blend mode (avg/add/sub/add4). The snapshot is the pre-batch VRAM, so a semi prim blends over the
// uploaded 2D background (+ earlier semi groups, once grouping lands); blending over THIS frame's opaque 3D
// is a Pass-2b refinement (per-group snapshots). PSX rule: a textured semi prim blends only where the texel
// STP bit is set; STP=0 texels (and opaque prims) draw opaque.
layout(location = 0) in vec3 v_col;
layout(location = 1) noperspective in vec2 v_uv;
layout(location = 2) flat in ivec4 v_tp;     // tpx, tpy, mode, raw
layout(location = 3) flat in ivec4 v_clut;   // clutx, cluty, semi, blend
layout(location = 4) flat in ivec4 v_tw;     // texture window
layout(location = 5) flat in ivec4 v_da;     // draw-area clip
// VRAM is R8G8_UNORM (R=low byte, G=high byte of the 1555 word — SDL_GPU forbids integer SAMPLER formats).
layout(set = 2, binding = 0) uniform sampler2D u_vram;
// ires (internal-resolution) scale: v_da (draw-area clip) is in NATIVE VRAM pixel units, and the manual
// destination sample below (vram_at(px,py)) must index the SNAPSHOT atlas (always fixed VRAM_W x VRAM_H —
// see gpu_vk.cpp render_geom, this pass never touches a scaled snapshot), so gl_FragCoord — which spans
// the ires-scaled render target's OWN (bigger) pixel range — must be divided back down to native units
// before either use. 1 at i==1 (a no-op divide, byte-identical to the pre-ires shader).
layout(set = 3, binding = 0) uniform PC { int scale; } pc;
layout(location = 0) out vec4 o_col;

uint vram_at(int x, int y) { vec2 rg = texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).rg;
                             return uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8); }

void main() {
    int px = int(gl_FragCoord.x) / pc.scale, py = int(gl_FragCoord.y) / pc.scale;
    if (px < v_da.x || px > v_da.z || py < v_da.y || py > v_da.w) discard;
    int mode = v_tp.z;
    uint texel; uint stp = 1u;
    if (mode == 3) {                                  // untextured: use the vertex color directly
        texel = uint(clamp(v_col.r,0.,1.)*31.+0.5) | (uint(clamp(v_col.g,0.,1.)*31.+0.5)<<5) | (uint(clamp(v_col.b,0.,1.)*31.+0.5)<<10);
    } else {
        int u = int(v_uv.x), v = int(v_uv.y);
        u = (u & ~(v_tw.x * 8)) | ((v_tw.z & v_tw.x) * 8);
        v = (v & ~(v_tw.y * 8)) | ((v_tw.w & v_tw.y) * 8);
        int tpx = v_tp.x, tpy = v_tp.y, clutx = v_clut.x, cluty = v_clut.y;
        if (mode == 0)      { uint w = vram_at(tpx+(u>>2), tpy+v); texel = vram_at(clutx+int((w>>((u&3)*4))&0xFu), cluty); }
        else if (mode == 1) { uint w = vram_at(tpx+(u>>1), tpy+v); texel = vram_at(clutx+int((w>>((u&1)*8))&0xFFu), cluty); }
        else                { texel = vram_at(tpx+u, tpy+v); }
        if (texel == 0u) discard;                     // transparent texel
        stp = (texel >> 15) & 1u;
        uint tr = texel & 31u, tg = (texel>>5)&31u, tb = (texel>>10)&31u;
        if (v_tp.w == 0) {                            // modulate by vertex color
            tr = min(31u, uint(float(tr)*v_col.r*(255.0/128.0)+0.5));
            tg = min(31u, uint(float(tg)*v_col.g*(255.0/128.0)+0.5));
            tb = min(31u, uint(float(tb)*v_col.b*(255.0/128.0)+0.5));
        }
        texel = tr | (tg<<5) | (tb<<10) | (stp<<15);
    }

    int semi = v_clut.z, blend = v_clut.w & 3;
    bool blends = (semi != 0) && ((mode == 3) || (stp == 1u));
    if (blends) {
        uint d = vram_at(px, py);                     // destination = VRAM snapshot at this pixel
        int sr=int(texel&31u), sg=int((texel>>5)&31u), sb=int((texel>>10)&31u);
        int dr=int(d&31u), dg=int((d>>5)&31u), db=int((d>>10)&31u);
        int rr, rg, rb;
        if (blend == 0)      { rr=(sr+dr)>>1; rg=(sg+dg)>>1; rb=(sb+db)>>1; }     // avg:  .5s + .5d
        else if (blend == 1) { rr=sr+dr; rg=sg+dg; rb=sb+db; }                    // add:  s + d
        else if (blend == 2) { rr=dr-sr; rg=dg-sg; rb=db-sb; }                    // sub:  d - s
        else                 { rr=(sr>>2)+dr; rg=(sg>>2)+dg; rb=(sb>>2)+db; }     // add4: .25s + d
        rr=clamp(rr,0,31); rg=clamp(rg,0,31); rb=clamp(rb,0,31);
        texel = uint(rr) | (uint(rg)<<5) | (uint(rb)<<10) | (stp<<15);
    }
    o_col = vec4(float(texel & 0xFFu) / 255.0, float((texel >> 8) & 0xFFu) / 255.0, 0.0, 1.0);
}
