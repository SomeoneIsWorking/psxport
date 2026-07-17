#version 450
// bug #55 (part 3 — semi coverage stamp): a DEPTH-ONLY pass (no color target) that re-rasterizes the SAME
// semi/translucent vertex buckets Pass B (trisemi_hw.frag) just blended, purely to WRITE depth wherever a
// real (non-discarded) semi fragment lands. Pass B itself is test-only / depth_write=false (so multiple
// overlapping semi quads can all blend against each other, gpu_vk.cpp's make_semi_pipeline) — real-HW-
// blend semi content therefore leaves NO trace in the depth buffer, which is exactly the signal
// ires_downsample.frag's coverage gate (u_depth) uses to decide "did the 3D pass touch this pixel". A
// scene whose visible 3D content is mostly/entirely semi (e.g. the paused-game "ghosted" world behind the
// pause menu) would otherwise register as fully UNCOVERED, discarding the real (correctly semi-blended)
// picture from the ires composite-back in favor of the native pre-3D snapshot — the opposite of the fix.
// This pass reuses tritex.vert (same vertex layout/NDC transform as Pass A/B) and duplicates ONLY the
// discard conditions from trisemi_hw.frag (draw-area clip + fully-transparent texel) — never SHOULD emit a
// wrong color since it emits none; depth_write=true with the SAME GREATER_OR_EQUAL compare as Pass A marks
// the depth buffer's max ord at any pixel a real semi fragment covers, which downstream composite-back
// reads exactly like an opaque hit. Run in gpu_vk.cpp's render_geom right after Pass B, one draw call per
// non-empty blend-mode bucket, sharing the SAME depth target Pass A cleared (so an opaque write already
// there is preserved — GREATER_OR_EQUAL only overwrites with an equal-or-larger ord, and this pass's ord is
// the SAME per-vertex value Pass B used, so ties simply re-write the identical value).
layout(location = 0) in vec3 v_col;          // unused (discard-only)
layout(location = 1) noperspective in vec2 v_uv;
layout(location = 2) flat in ivec4 v_tp;     // tpx, tpy, mode, raw
layout(location = 3) flat in ivec4 v_clut;   // clutx, cluty, semi, blend
layout(location = 4) flat in ivec4 v_tw;     // texture window
layout(location = 5) flat in ivec4 v_da;     // draw-area clip
layout(set = 2, binding = 0) uniform sampler2D u_vram;
layout(set = 3, binding = 0) uniform PC { int scale; } pc;   // ires scale (same role as trisemi_hw.frag's)

uint vram_at(int x, int y) { vec2 rg = texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).rg;
                             return uint(rg.r * 255.0 + 0.5) | (uint(rg.g * 255.0 + 0.5) << 8); }

void main() {
    int px = int(gl_FragCoord.x) / pc.scale, py = int(gl_FragCoord.y) / pc.scale;
    if (px < v_da.x || px > v_da.z || py < v_da.y || py > v_da.w) discard;
    int mode = v_tp.z;
    if (mode != 3) {   // untextured (mode==3) prims have no texel to check — always covered where rasterized
        int u = int(v_uv.x), v = int(v_uv.y);
        u = (u & ~(v_tw.x * 8)) | ((v_tw.z & v_tw.x) * 8);
        v = (v & ~(v_tw.y * 8)) | ((v_tw.w & v_tw.y) * 8);
        int tpx = v_tp.x, tpy = v_tp.y, clutx = v_clut.x, cluty = v_clut.y;
        uint texel;
        if (mode == 0)      { uint w = vram_at(tpx+(u>>2), tpy+v); texel = vram_at(clutx+int((w>>((u&3)*4))&0xFu), cluty); }
        else if (mode == 1) { uint w = vram_at(tpx+(u>>1), tpy+v); texel = vram_at(clutx+int((w>>((u&1)*8))&0xFFu), cluty); }
        else                { texel = vram_at(tpx+u, tpy+v); }
        if (texel == 0u) discard;
    }
}
