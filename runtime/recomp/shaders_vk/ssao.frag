#version 450
// PC-native SSAO (PSXPORT_SSAO). A fullscreen post pass over the geometry buffer: read the per-pixel
// 1555 color (u_color) and the native 3-band D32 depth (u_depth), estimate ambient occlusion in
// screen space, and darken creases/contacts. Output is the same 1555 format (copied back into the VRAM
// image so present/dump see it). Only 3D-world pixels are touched; sky/HUD/backdrop bands pass through.
//
// Depth model: the geometry pass stored depth = ord3d(proj_pz_to_ord(pz)). proj_pz_to_ord is AFFINE in
// 1/pz (gl_Position.w==1, no perspective divide on z), so the stored value linearizes back to view-Z by
// undoing the band remap then the affine map (see gte_beetle.c proj_pz_to_ord / proj_near_pz). Working
// in true view-Z (pz) makes the bias/range scale with distance, so AO is consistent near and far.
layout(location = 0) out uint o_px;
layout(set = 0, binding = 0) uniform usampler2D u_color;   // R16_UINT 1555 geometry color
layout(set = 0, binding = 1) uniform sampler2D  u_depth;   // D32 native 3-band depth
layout(push_constant) uniform PC {
    vec4 p0;   // x=inv_near (1/nearp), y=inv_far (1/65535), z=strength, w=radius (px)
    vec4 p1;   // x=bias_frac, y=range_frac, z=band_min (NATIVE_3D_MIN), w=band_max (NATIVE_3D_MAX)
    ivec4 p2;  // x=img_w, y=img_h, z=viz (0=normal; 1=AO factor as grayscale on 3D, color elsewhere)
} pc;

uint  cat(int x, int y) { return texelFetch(u_color, ivec2(clamp(x,0,pc.p2.x-1), clamp(y,0,pc.p2.y-1)), 0).r; }
float dat(int x, int y) { return texelFetch(u_depth, ivec2(clamp(x,0,pc.p2.x-1), clamp(y,0,pc.p2.y-1)), 0).r; }

// Linearize a banded depth D to view-space pz; returns -1 for non-3D pixels (background/HUD/cleared).
float lin(float D) {
    if (D <= pc.p1.z || D >= pc.p1.w) return -1.0;             // outside the 3D world band
    float ord = (D - pc.p1.z) / (pc.p1.w - pc.p1.z);           // undo ord3d() band remap -> [0,1]
    float inv = ord * (pc.p0.x - pc.p0.y) + pc.p0.y;           // 1/pz (affine)
    return 1.0 / max(inv, 1e-6);
}

void main() {
    int px = int(gl_FragCoord.x), py = int(gl_FragCoord.y);
    uint c = cat(px, py);
    float z0 = lin(dat(px, py));
    if (z0 < 0.0) { o_px = c; return; }                        // not 3D -> pass color through untouched

    float bias  = z0 * pc.p1.x;
    float range = z0 * pc.p1.y;
    float r = pc.p0.w;
    // CURVATURE-based AO via opposite-neighbor pairs. For each pair (p+o, p-o) the linear-interpolated
    // depth is their average; a flat surface (even steeply TILTED) has center == that average, so it
    // contributes 0 — this is what kills the false "whole tilted ground darkens" wash that a naive
    // "is the neighbor closer" test produces. Only a genuine CONCAVITY (center recedes behind the
    // straight line through its neighbors: crease, inner corner, contact valley) yields occlusion.
    // Pairs touching a non-3D pixel (silhouette against sky/HUD) are skipped -> no edge halos.
    const vec2 base[4] = vec2[4](vec2(1.0,0.0), vec2(0.707,0.707), vec2(0.0,1.0), vec2(-0.707,0.707));
    float ao = 0.0, wsum = 0.0;
    for (int ring = 0; ring < 2; ring++) {
        float rr = r * (ring == 0 ? 0.5 : 1.0);
        for (int i = 0; i < 4; i++) {
            ivec2 o = ivec2(round(base[i] * rr));
            float zp = lin(dat(px + o.x, py + o.y));
            float zm = lin(dat(px - o.x, py - o.y));
            if (zp < 0.0 || zm < 0.0) continue;                // need both sides 3D (skip silhouettes)
            wsum += 1.0;
            float conc = z0 - 0.5 * (zp + zm);                 // >0 : center recedes = concave = occluded
            if (conc > bias) ao += clamp((conc - bias) / range, 0.0, 1.0);
        }
    }
    ao = (wsum > 0.0) ? ao / wsum : 0.0;
    float f = clamp(1.0 - pc.p0.z * ao, 0.0, 1.0);
    if (pc.p2.z != 0) {                                   // viz: 3D pixels as the AO factor (white=lit, dark=occluded)
        uint q = uint(f * 31.0 + 0.5);
        o_px = q | (q << 5) | (q << 10);
        return;
    }
    uint a = c & 0x8000u;
    float cr = float(c & 31u) * f, cg = float((c >> 5) & 31u) * f, cb = float((c >> 10) & 31u) * f;
    o_px = uint(cr + 0.5) | (uint(cg + 0.5) << 5) | (uint(cb + 0.5) << 10) | a;
}
