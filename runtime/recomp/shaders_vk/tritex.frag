#version 450
// Textured/untextured PSX fragment with semi-transparency. mode: 0=4bpp,1=8bpp,2=15bpp,3=untextured.
// v_clut = (clutx, cluty, semi, blendmode). For semi prims u_vram is the post-opaque framebuffer
// snapshot, so it doubles as the blend destination (read at gl_FragCoord).
layout(location = 0) in vec3 v_col;
layout(location = 1) noperspective in vec2 v_uv;
layout(location = 2) flat in ivec4 v_tp;     // tpx, tpy, mode, raw
layout(location = 3) flat in ivec4 v_clut;   // clutx, cluty, semi, blend
layout(location = 4) flat in ivec4 v_tw;     // texture window
layout(location = 5) flat in ivec4 v_da;     // draw-area clip
layout(set = 0, binding = 0) uniform usampler2D u_vram;
// The color attachment is a BLENDABLE A1R5G5B5_UNORM_PACK16 view aliasing the R16_UINT VRAM image (16-bit
// compat class; A=bit15, R=bits10-14, G=5-9, B=bits0-4 — PSX 1555 with R<->B swapped). The fragment computes
// the SOURCE texel color (texture sample × vertex modulate) and emits it as a normalized vec4 (swizzled so
// the stored bits stay PSX 1555 — see the output below); the GPU's fixed-function HARDWARE BLEND composites
// the 4 PSX semi modes natively (avg/add/sub/add4) against the live framebuffer — no in-shader
// framebuffer-snapshot blend any more. Opaque prims use a non-blending pipeline (their alpha is ignored).
layout(location = 0) out vec4 o_col;
// SEMI_PASS selects how this draw treats a textured prim's PER-TEXEL STP bit, because PSX blends a
// textured semi prim ONLY where the texel STP bit is set; STP=0 texels draw OPAQUE. Fixed-function blend
// is per-DRAW, so a textured-semi prim is drawn in TWO sub-passes over the same geometry:
//   0 = OPAQUE draw  (non-blend pipeline): keep ONLY the draw-opaque fragments (textured texel, STP=0);
//       discard everything else (they belong to the blend sub-pass / opaque prims aren't drawn here).
//   1 = BLEND draw   (HW-blend pipeline):  keep the fragments that BLEND (untextured, or textured STP=1);
//       discard the draw-opaque ones (handled by sub-pass 0).
//   2 = plain OPAQUE prim (the normal opaque-textured pipeline): no STP gating, draw every visible texel.
layout(constant_id = 1) const int SEMI_PASS = 2;

uint vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r; }

void main() {
    int px = int(gl_FragCoord.x), py = int(gl_FragCoord.y);
    if (px < v_da.x || px > v_da.z || py < v_da.y || py > v_da.w) discard;
    int mode = v_tp.z;
    uint texel; uint stp = 1u;
    if (mode == 3) {                                  // untextured: use the vertex color directly
        texel = uint(clamp(v_col.r,0.,1.)*31.+0.5) | (uint(clamp(v_col.g,0.,1.)*31.+0.5)<<5) | (uint(clamp(v_col.b,0.,1.)*31.+0.5)<<10);
    } else {
        int u = int(v_uv.x), v = int(v_uv.y);
        u = (u & ~(v_tw.x * 8)) | ((v_tw.z & v_tw.x) * 8);
        v = (v & ~(v_tw.y * 8)) | ((v_tw.w & v_tw.y) * 8);
        // NO unconditional u&=255/v&=255: the SW reference (sample_tex) wraps U/V ONLY through the
        // texture window (above); forcing &255 re-wrapped interpolated U>=256 back into the same texpage
        // -> a sprite that spans into the next page re-sampled its left columns = vertical bars (#8/#9
        // dust + effect stripes). vram_at already masks the final VRAM address to 1023x511.
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

    // PSX rule: a textured semi prim BLENDS only where the texel STP bit is set; STP=0 texels are OPAQUE.
    // (Untextured semi prims always blend.) Split that per-texel decision across the two semi sub-passes.
    bool blends = (mode == 3) || (stp == 1u);   // this fragment is a blending one (vs. draw-opaque)
    if (SEMI_PASS == 0 && blends) discard;      // opaque sub-pass: keep only the draw-opaque fragments
    if (SEMI_PASS == 1 && !blends) discard;     // blend sub-pass: keep only the blending fragments
    // Emit the SOURCE 1555 word as a normalized vec4 for the A1R5G5B5 view: output order (B,G,R,STP), i.e.
    // PSX-blue (bits10-14) -> the view's R slot, PSX-red (bits0-4) -> its B slot, so the STORED word keeps
    // PSX-red in the low 5 bits (identical to the old uint pack). The HW blend pipeline (sub-pass 1)
    // composites it per-channel against the live framebuffer; opaque draws ignore alpha.
    o_col = vec4(float((texel >> 10) & 31u) / 31.0, float((texel >> 5) & 31u) / 31.0,
                 float(texel & 31u) / 31.0, float((texel >> 15) & 1u));
}
