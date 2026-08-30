// PSX UV is evaluated once per NATIVE pixel, at that pixel's integer coordinate. A GPU rasterizer
// interpolates at the fragment CENTRE instead, so the affine plane must be rewound by the fragment's
// subpixel offset — half a pixel at 1x. That correction is not cosmetic: a mirrored sprite has a
// DESCENDING u, so the centre value sits half a texel BELOW the PSX one and int() truncates a whole
// texel early. (Measured on Tomba! 2's HP ring: the mirrored half runs u 39->23 over x 31->47; PSX
// reads 39 at x=31, centre sampling reads 38.5 -> 38. The two halves share column 31 by design — the
// guest emitter FUN_8007E1B8 builds corners x0..x0+w and the template overlaps them deliberately — so
// hardware draws the same texel there twice, idempotently, while an off-by-one texel makes it a seam.)
//
// THE BOUND IS THE OTHER HALF. Rewinding moves the evaluation point OFF the fragment centre, and on an
// edge that is not axis-aligned it lands outside the primitive entirely — a covered fragment then
// fetches a texel up to a full texel-gradient beyond its own UV footprint: a foreign texel, or texel 0,
// which discards and punches a one-pixel hole. That is what traced a dark dashed line down every quad's
// split diagonal. Hardware cannot do this, because it only ever evaluates where it covers. uvbb — the
// primitive's own vertex-UV bounding box (see TexVtx in gpu_vk.cpp) — restores that invariant: a no-op
// for every axis-aligned prim and every interior fragment, and a hard stop at the primitive's own
// texels everywhere else.
vec2 psxUvAtIntegerPixel(vec2 centerUv, int scale, ivec4 uvbb) {
    vec2 subpixel = vec2(ivec2(gl_FragCoord.xy) % scale) + vec2(0.5);
    vec2 integerPixelUv = centerUv - subpixel.x * dFdx(centerUv) - subpixel.y * dFdy(centerUv);
    // The PSX rasterizer carries affine UV with 12 fractional bits, then adds half a texel before
    // converting to integer. Snap the float reconstruction to that representable grid before the
    // round-to-nearest step; otherwise ires interpolation error can put an exact half-texel on the
    // wrong side. uvbb bounds the result to the primitive's own texels.
    vec2 snapped = floor(integerPixelUv * 4096.0 + vec2(0.5)) / 4096.0;
    vec2 rounded = floor(snapped + vec2(0.5));
    return clamp(rounded, vec2(uvbb.xy), vec2(uvbb.zw));
}
