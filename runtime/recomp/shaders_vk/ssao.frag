#version 450
// PC-native DEFERRED shading post pass (PSXPORT_SSAO + PSXPORT_LIGHT). A fullscreen pass over the opaque
// geometry buffer: read the per-pixel 1555 color (u_color) and the native 3-band D32 depth (u_depth),
// and apply ambient occlusion and/or a directional light reconstructed PURELY from depth — no PSX GTE
// hooks, no per-face PGXP normals. Output is the same 1555 format (copied back into the VRAM image so
// present/dump see it). Only 3D-world pixels are touched; sky/HUD/backdrop bands pass through unchanged.
//
// Depth model: the geometry pass stored depth = ord3d(proj_pz_to_ord(pz)). proj_pz_to_ord is AFFINE in
// 1/pz (gl_Position.w==1, no perspective divide on z), so the stored value linearizes back to view-Z by
// undoing the band remap then the affine map (see gte_beetle.c proj_pz_to_ord / proj_near_pz).
// View position from a depth pixel: P = ((screenX-cx)*pz/H, (screenY-cy)*pz/H, pz), where screenX/Y map
// the VRAM pixel back to PSX screen space (origin/scale/offset cover faithful + wide/hi-res FB modes) and
// cx,cy,H are the GTE projection center + plane (gte_beetle proj_screen_center / proj_plane_h). The
// surface normal comes from the cross product of the view-position derivatives (closer-neighbour pick to
// avoid bleeding a normal across a depth silhouette). This is a standard deferred reconstruction — the
// "proper" PC-native way, decoupled from the PSX vertex pipeline.
layout(location = 0) out uint o_px;
layout(set = 0, binding = 0) uniform usampler2D u_color;   // R16_UINT 1555 geometry color
layout(set = 0, binding = 1) uniform sampler2D  u_depth;   // D32 native 3-band depth
layout(set = 0, binding = 2) uniform sampler2D  u_shadow;  // D32 light-view shadow depth map
layout(push_constant) uniform PC {
    vec4  p0;   // inv_near (1/nearp), inv_far (1/65535), ssao_strength, ssao_radius (px)
    vec4  p1;   // ssao_bias_frac, ssao_range_frac, band_min (NATIVE_3D_MIN), band_max (NATIVE_3D_MAX)
    ivec4 p2;   // img_w, img_h, viz(0 none|1 AO|2 normal|3 lit|4 shadow), flags(bit0=ssao, bit1=light, bit2=shadow)
    vec4  p3;   // light dir (to-light, view space) xyz, diffuse intensity
    vec4  p4;   // ambient, cx, cy, H
    vec4  p5;   // screen map: origin_x, origin_y, off_x, off_y
    vec4  p6;   // screen map: inv_scale, shadow_strength, shadow_texel (1/shadowmap_dim), shadow_depth_bias
    mat4  lvp;  // light view-projection (view-space pos -> light clip), for shadow sampling
} pc;

uint  cat(int x, int y) { return texelFetch(u_color, ivec2(clamp(x,0,pc.p2.x-1), clamp(y,0,pc.p2.y-1)), 0).r; }
float dat(int x, int y) { return texelFetch(u_depth, ivec2(clamp(x,0,pc.p2.x-1), clamp(y,0,pc.p2.y-1)), 0).r; }

// Linearize a banded depth D to view-space pz; returns -1 for non-3D pixels (background/HUD/cleared).
float lin(float D) {
    if (D <= pc.p1.z || D >= pc.p1.w) return -1.0;            // outside the 3D world band
    float ord = (D - pc.p1.z) / (pc.p1.w - pc.p1.z);          // undo ord3d() band remap -> [0,1]
    float inv = ord * (pc.p0.x - pc.p0.y) + pc.p0.y;          // 1/pz (affine)
    return 1.0 / max(inv, 1e-6);
}
// View-space position of VRAM pixel (x,y) at view depth pz.
vec3 vpos(int x, int y, float pz) {
    float sx = (float(x) - pc.p5.x) * pc.p6.x - pc.p5.z;      // VRAM -> PSX screen coord
    float sy = (float(y) - pc.p5.y) * pc.p6.x - pc.p5.w;
    return vec3((sx - pc.p4.y) * pz / pc.p4.w, (sy - pc.p4.z) * pz / pc.p4.w, pz);
}

// Shadow factor at a reconstructed VIEW-space position P: 1 = fully lit, 0 = fully in shadow. Transform P
// into the light's clip space (lvp), do the perspective/ortho divide, map to [0,1] shadow-map UV + depth,
// then PCF-compare against the stored light-view depth. A constant depth bias kills self-shadow acne; PCF
// (3x3) softens the hard edge. Out-of-map (frustum edge) -> lit (no shadow), so the bounded ortho volume
// doesn't shadow-band the whole scene.
float shadow_at(vec3 P) {
    vec4 lp = pc.lvp * vec4(P, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;                            // light-clip XY -> [0,1] map UV
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    if (ndc.z < 0.0 || ndc.z > 1.0) return 1.0;              // outside the ortho near/far -> lit
    float ref = ndc.z - pc.p6.w;                             // bias toward the light (depth bias)
    float texel = pc.p6.z, lit = 0.0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            float sd = texture(u_shadow, uv + vec2(float(i), float(j)) * texel).r;
            lit += (ref <= sd) ? 1.0 : 0.0;                  // closer-or-equal than the occluder -> lit
        }
    return lit / 9.0;
}

void main() {
    int px = int(gl_FragCoord.x), py = int(gl_FragCoord.y);
    uint c = cat(px, py);
    float z0 = lin(dat(px, py));
    if (z0 < 0.0) { o_px = c; return; }                      // not 3D -> pass color through untouched
    bool do_ssao   = (pc.p2.w & 1) != 0;
    bool do_light  = (pc.p2.w & 2) != 0;
    bool do_shadow = (pc.p2.w & 4) != 0;

    // --- ambient occlusion (curvature via opposite-neighbour pairs) ------------------------------------
    // A flat surface (even steeply TILTED) has center == average(opposite neighbours) -> 0 AO; only a
    // genuine concavity/contact darkens. Pairs touching a non-3D pixel are skipped (no silhouette halos).
    float ao = 0.0;
    if (do_ssao) {
        float bias = z0 * pc.p1.x, range = z0 * pc.p1.y, r = pc.p0.w, wsum = 0.0;
        const vec2 base[4] = vec2[4](vec2(1.0,0.0), vec2(0.707,0.707), vec2(0.0,1.0), vec2(-0.707,0.707));
        for (int ring = 0; ring < 2; ring++) {
            float rr = r * (ring == 0 ? 0.5 : 1.0);
            for (int i = 0; i < 4; i++) {
                ivec2 o = ivec2(round(base[i] * rr));
                float zp = lin(dat(px + o.x, py + o.y)), zm = lin(dat(px - o.x, py - o.y));
                if (zp < 0.0 || zm < 0.0) continue;
                wsum += 1.0;
                float conc = z0 - 0.5 * (zp + zm);
                if (conc > bias) ao += clamp((conc - bias) / range, 0.0, 1.0);
            }
        }
        ao = (wsum > 0.0) ? ao / wsum : 0.0;
    }

    // --- directional light from a depth-reconstructed normal ------------------------------------------
    float lit = 1.0; vec3 N = vec3(0.0, 0.0, -1.0);
    if (do_light) {
        vec3 P = vpos(px, py, z0);
        float zr = lin(dat(px+1, py)), zl = lin(dat(px-1, py));
        float zd = lin(dat(px, py+1)), zu = lin(dat(px, py-1));
        // horizontal/vertical tangent: pick the side with the smaller depth step (avoid silhouettes)
        vec3 dX = (zr >= 0.0 && (zl < 0.0 || abs(zr-z0) <= abs(z0-zl))) ? vpos(px+1,py,zr) - P
                : (zl >= 0.0)                                          ? P - vpos(px-1,py,zl) : vec3(0.0);
        vec3 dY = (zd >= 0.0 && (zu < 0.0 || abs(zd-z0) <= abs(z0-zu))) ? vpos(px,py+1,zd) - P
                : (zu >= 0.0)                                          ? P - vpos(px,py-1,zu) : vec3(0.0);
        vec3 cr = cross(dX, dY);
        if (dot(cr, cr) > 1e-9) {
            N = normalize(cr);
            if (N.z > 0.0) N = -N;                            // face the camera (view -Z)
            float nl = max(0.0, dot(N, normalize(pc.p3.xyz)));
            lit = pc.p4.x + pc.p3.w * nl;                     // ambient + diffuse * N·L
        } else {
            lit = pc.p4.x + pc.p3.w * 0.5;                    // unreliable normal -> neutral mid-shade
        }
    }

    // --- shadow factor (cast by the directional light) ------------------------------------------------
    // shf in [0,1]: 1 = lit, 0 = occluded. Darkening multiplies the lit color by (1 - strength*(1-shf)),
    // so a fully-shadowed pixel drops to (1-strength) of its lit value (strength=1 -> ambient-only feel).
    float shf = 1.0;
    if (do_shadow) shf = shadow_at(vpos(px, py, z0));
    float shade = 1.0 - pc.p6.y * (1.0 - shf);

    // --- viz / composite ------------------------------------------------------------------------------
    if (pc.p2.z == 1) { uint q = uint(clamp(1.0 - pc.p0.z*ao,0.0,1.0)*31.0+0.5); o_px = q|(q<<5)|(q<<10); return; }
    if (pc.p2.z == 2) { uvec3 q = uvec3(clamp(N*0.5+0.5,0.0,1.0)*31.0+0.5);      o_px = q.x|(q.y<<5)|(q.z<<10); return; }
    if (pc.p2.z == 3) { uint q = uint(clamp(lit,0.0,1.0)*31.0+0.5);              o_px = q|(q<<5)|(q<<10); return; }
    if (pc.p2.z == 4) { uint q = uint(clamp(shf,0.0,1.0)*31.0+0.5);              o_px = q|(q<<5)|(q<<10); return; }

    float f = lit * shade * (do_ssao ? clamp(1.0 - pc.p0.z * ao, 0.0, 1.0) : 1.0);
    uint a = c & 0x8000u;
    float cr2 = clamp(float(c & 31u)        * f, 0.0, 31.0);
    float cg2 = clamp(float((c >> 5) & 31u) * f, 0.0, 31.0);
    float cb2 = clamp(float((c >> 10) & 31u)* f, 0.0, 31.0);
    o_px = uint(cr2 + 0.5) | (uint(cg2 + 0.5) << 5) | (uint(cb2 + 0.5) << 10) | a;
}
