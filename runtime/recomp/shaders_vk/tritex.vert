#version 450
// Textured PSX triangle. Vertices in VRAM coords; UV is AFFINE (noperspective) to match PSX mapping.
// State (texpage base, color mode, CLUT) is constant per primitive -> flat-interpolated.
layout(location = 0) in vec2  i_pos;    // VRAM pixel coords (post draw-offset)
layout(location = 1) in vec2  i_uv;     // texel coords within the page (0..255)
layout(location = 2) in vec3  i_col;    // modulation color 0..1 (128/255 = neutral 1.0x)
layout(location = 3) in ivec4 i_tp;     // tpx, tpy (VRAM px base of page), mode(0=4bpp,1=8bpp,2=15bpp), raw
layout(location = 4) in ivec4 i_clut;   // clutx, cluty (VRAM px), -, -
layout(location = 5) in ivec4 i_tw;     // texture window: mask_x, mask_y, off_x, off_y (8px units)
layout(location = 6) in ivec4 i_da;     // draw-area clip: x0, y0, x1, y1 (VRAM px)
layout(location = 7) in float i_ord;    // OT submission order as depth [0,1] (later prim = greater)
layout(location = 8) in float i_ordn;   // NATIVE per-vertex depth [0,1] (PSXPORT_SBS native panel)
layout(location = 0) out vec3 v_col;
layout(location = 1) noperspective out vec2 v_uv;
layout(location = 2) flat out ivec4 v_tp;
layout(location = 3) flat out ivec4 v_clut;
layout(location = 4) flat out ivec4 v_tw;
layout(location = 5) flat out ivec4 v_da;
// PSXPORT_SBS depth channel, baked into the pipeline (specialization constant, NOT a push constant) so
// each Panel binds its OWN pipeline variant and the two panels can never alias state. 0 = default OT
// .ord, 1 = native .ordn.
layout(constant_id = 0) const int SBS_NATIVE = 0;
// PC-native widescreen / supersample transform (vertex push constant, offset 16 to clear the
// fragment present push at 0). wa=(enabled, fb_y0, ss, img_h); wb=(reserved, fbw, fbh, fb_x0).
layout(push_constant) uniform VPC { layout(offset = 16) ivec4 wa; ivec4 wb; } w;
void main() {
    v_col = i_col; v_uv = i_uv; v_tp = i_tp; v_clut = i_clut; v_tw = i_tw;
    float ny = float(w.wa.w) * 0.5;   // image is IMG_H tall; map VRAM y -> NDC over the full image
    float fx, fy;
    if (w.wa.x != 0) {
        // Relocate into the scratch FB: keep native projection scale (no squish), placing the
        // framebuffer-local view 1:1 into the (wider/hi-res) FB. da.xy is the active framebuffer origin.
        float ss = float(w.wa.z);
        vec2 local = i_pos - vec2(i_da.xy);
        fx = local.x * ss + float(w.wb.w);
        fy = float(w.wa.y) + local.y * ss;
        v_da = ivec4(w.wb.w, w.wa.y, w.wb.w + w.wb.y - 1, w.wa.y + w.wb.z - 1);   // clip = FB rect
    } else {
        v_da = i_da;
        fx = i_pos.x; fy = i_pos.y;
    }
    float z = (SBS_NATIVE != 0) ? i_ordn : i_ord;
    gl_Position = vec4(fx / 512.0 - 1.0, fy / ny - 1.0, z, 1.0);
}
