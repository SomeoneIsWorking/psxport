// PSX UV interpolation is evaluated once at each native integer pixel. SDL_GPU interpolates at every
// internal-resolution fragment centre, so rewind the affine plane by that fragment's subpixel offset.
// This is 0.5 derivative at 1x and keeps all NxN fragments of a native pixel on the same PSX texel at Nx.
vec2 psxUvAtIntegerPixel(vec2 centerUv, int scale) {
    vec2 subpixel = vec2(ivec2(gl_FragCoord.xy) % scale) + vec2(0.5);
    vec2 integerPixelUv = centerUv - subpixel.x * dFdx(centerUv) - subpixel.y * dFdy(centerUv);
    // The PSX rasterizer carries affine UV with 12 fractional bits. Snap the float reconstruction to
    // that representable grid before int() truncation; otherwise ires interpolation error can put an
    // exact fixed-point integer just below its texel boundary. Primitive UVs remain in the unsigned byte range.
    return floor(integerPixelUv * 4096.0 + vec2(0.5)) / 4096.0;
}
