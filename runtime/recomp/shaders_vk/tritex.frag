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
layout(location = 6) flat in vec3  v_normal; // view-space per-face normal (0 = unlit)
layout(location = 7) in float v_depth;       // view-space Z
layout(set = 0, binding = 0) uniform usampler2D u_vram;
layout(location = 0) out uint o_px;
// Native lighting engine (replaces Tomba2's baked-color+GTE-depth-cue model, see docs/engine_re.md).
// l0 = (light dir xyz [view space], mode: 0=off 1=directional 2=normal-viz);
// l1 = (ambient, diffuse, fogStart, fogEnd); l2 = (fog tint rgb, fogEnable).
layout(push_constant) uniform LPC {
    layout(offset = 48) vec4 l0; vec4 l1; vec4 l2;
} L;

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
        u &= 255; v &= 255;
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

    // --- Native lighting: modulate the SOURCE color (before semi-blend). Only lit 3D faces have a
    // non-zero normal; 2D sprites/HUD/lines (normal=0) pass through untouched. ----------------------
    int lmode = int(L.l0.w + 0.5);
    bool lit = lmode != 0 && dot(v_normal, v_normal) > 1e-6;
    bool fog = L.l2.w > 0.5;
    if (lit || fog) {
        uint a = texel & 0x8000u;
        vec3 c = vec3(float(texel & 31u), float((texel>>5)&31u), float((texel>>10)&31u)) / 31.0;
        if (lit) {
            vec3 N = normalize(v_normal);
            if (lmode == 2) {                         // debug: visualize the reconstructed normal
                c = N * 0.5 + 0.5;
            } else {                                  // directional diffuse + ambient
                float d = max(0.0, dot(N, normalize(L.l0.xyz)));
                c *= (L.l1.x + L.l1.y * d);
            }
        }
        if (fog) {                                    // native distance fog toward l2.rgb tint
            float f = clamp((v_depth - L.l1.z) / max(1.0, L.l1.w - L.l1.z), 0.0, 1.0);
            c = mix(c, L.l2.xyz, f);
        }
        c = clamp(c, 0.0, 1.0);
        texel = uint(c.r*31.+0.5) | (uint(c.g*31.+0.5)<<5) | (uint(c.b*31.+0.5)<<10) | a;
    }

    // semi-transparency: textured blends only STP-set texels; untextured always blends.
    if (v_clut.z != 0 && (mode == 3 || stp == 1u)) {
        uint d = vram_at(px, py);
        int sr=int(texel&31u), sg=int((texel>>5)&31u), sb=int((texel>>10)&31u);
        int dr=int(d&31u), dg=int((d>>5)&31u), db=int((d>>10)&31u);
        int br, bg, bb; int m = v_clut.w;
        if (m == 0)      { br=(dr+sr)/2; bg=(dg+sg)/2; bb=(db+sb)/2; }            // average
        else if (m == 1) { br=dr+sr;     bg=dg+sg;     bb=db+sb; }                // add
        else if (m == 2) { br=dr-sr;     bg=dg-sg;     bb=db-sb; }                // subtract
        else             { br=dr+sr/4;   bg=dg+sg/4;   bb=db+sb/4; }              // add quarter
        br=clamp(br,0,31); bg=clamp(bg,0,31); bb=clamp(bb,0,31);
        texel = uint(br) | (uint(bg)<<5) | (uint(bb)<<10) | (texel & 0x8000u);
    }
    o_px = texel;
}
