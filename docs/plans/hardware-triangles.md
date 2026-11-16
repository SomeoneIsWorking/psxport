# Hardware triangles and fixed-function blending

User, 2026-11-16: "I want hardware triangles and hardware blending, pixel-exact isn't a constraint".

## Why

The record rasterizer still rasterizes in software, just on the GPU. Every primitive goes out as
rectangles of target pixels (`RecordQuad`, six vertices each in `record.vert`). A triangle sends its
bounding box (since 323a44f, one box per 4-row band, `kTriangleBandRows`), and `record.frag` then:

- tests every pixel against the triangle's edges by `gpu.c`'s rules and discards the ones outside;
- rebuilds colour and UV from the CPU-computed bases and deltas (`kOpBase`, `kOpDx`, `kOpDy`);
- reads the target back (framebuffer fetch on Metal, the snapshot elsewhere) for semi-transparency and
  the mask test, and blends in the shader.

At 8x a Spyro frame is millions of pixels that are shaded and then thrown away, plus a target read for
every blended one. Measured 2026-11-15 (Spyro 1 Stone Hill, 8x, gputime): Apple M-series about 15 ms
per present after 624a0f9..323a44f, AMD RX 6700 XT on Fedora about 11 ms. 4x already runs at 60.
Bands took 18.4 → 14.8 ms, which shows that most of the remaining cost is pixels outside the triangle.

DuckStation's hardware renderer is the model: real triangles through the hardware rasterizer,
hardware-interpolated colour and UV, fixed-function blend states per semi-transparency mode, the mask
bit in destination alpha, VRAM copied to a read texture only when a draw samples what was drawn.

## What we give up

- Edges and interpolants follow the GPU's top-left fill rule and float interpolation, not `gpu.c`'s.
  `recordcheck` at 1x will report edge pixels. That's accepted: the device path stays the reference
  picture. Check before landing whether any oracle or shot test compares the record image itself,
  and move it to the device picture if so.
- The sliver handling in `addTriangle` (a triangle with area in its stored places but none in its
  words) has no hardware equivalent. Expect seams to come back in Stone Hill at 8x (issues 0199,
  0206). Fix them when seen, likely by snapping such vertices or by keeping a software path just for
  slivers.

## Phase 1: hardware triangles on today's target

Smallest step that removes the bounding boxes. The target stays RG8 holding packed 1555, and blending
stays in the shader.

1. **Vertices, not rectangles.** A triangle becomes three vertices at scale S with per-vertex colour,
   UV and the op index. Its UVs are the stored native UVs; the hardware interpolates them
   (`noperspective`). Sprites, fills, uploads and copies keep their rectangles (they're already exact
   boxes). Lines become thin quads, one per segment.
   - New vertex stream beside the quad buffer: `RecordVertex {vec2 pos; uint rgb; uint uv; uint op;}`.
   - `RecordPlanner::Builder::addTriangle` stops doing bands and `coveredColumns`, and emits the three
     vertices. The 1x walk that decides batches and which ops read what stays as it is: batch splits
     are still needed for texture reads of drawn pixels.
2. **Shader split.** `record.vert` gets a triangle variant. `record.frag` gets a triangle entry that
   skips the edge test and the base/delta rebuild, and keeps texel fetch (4/8/15bpp, CLUT, texture
   window, the S×S block lookup), modulation, the transparent-texel discard, and the existing blend and
   mask code for ops with `kOpSemi | kOpMaskCheck`.
3. **Draw order.** Triangles and quads of a batch must still draw in record order. Either interleave
   draws by run (triangle run, quad run, …), which is cheap because runs are long, or put both through
   one triangle stream with a quad as two triangles. Prefer the second: one pipeline per state, fewer
   draw calls.
4. **Depth groups.** The depth pipelines (`groupTest`, `groupMark`, `model*`) keep their stencil and
   depth state. They just get vertices instead of boxes. `RECORD_DEPTH_MARK` loses its edge test.
5. **Measure.** gputime on Spyro 1 Stone Hill at 8x on Fedora, against 11 ms. Shots by eye at 1x and
   8x, plus `recordcheck` to see the edge difference is only edges.

## Phase 2: RGBA8 target and fixed-function blending

1. **Target format.** `kVramFormat` R8G8 → R8G8B8A8_UNORM. Colour in RGB, the mask bit in A.
   - Uploads expand 5-bit channels exactly (`v << 3 | v >> 2`). A texel fetch that needs the raw
     16-bit word (4bpp/8bpp indices, CLUT entries) packs back with `round(c * 31)`. That's exact for
     uploaded pixels and close enough for drawn ones, which are rarely read as indices.
   - Everything that reads the image as 1555 changes with it: `record_shot`, `render_readback`, the
     present shader, `recordcheck`, the in-between copies, outline-text samplers.
2. **Blend states.** One pipeline per mode. The shader outputs premultiplied source where needed:

   | PSX mode | shader out | src factor | op | dst factor |
   |---|---|---|---|---|
   | opaque | F | ONE | ADD | ZERO |
   | 0: B/2 + F/2 | F/2 | ONE | ADD | CONSTANT (0.5) |
   | 1: B + F | F | ONE | ADD | ONE |
   | 2: B − F | F | ONE | REVERSE_SUBTRACT | ONE |
   | 3: B + F/4 | F/4 | ONE | ADD | ONE |

   Alpha replaces: A = mask set | texel STP. UNORM saturates as the PSX does.
3. **Textured semi-transparency.** Only texels with STP set blend. Draw such a primitive twice: an
   opaque pass that discards STP texels, then the blend pass that discards non-STP texels.
   SDL_GPU has no dual-source blending, so it's two passes.
4. **Mask test through destination alpha.** With `kOpMaskCheck`, use `ONE_MINUS_DST_ALPHA` for the
   source factor (and `DST_ALPHA` for opaque's destination). That leaves masked pixels as they were,
   for opaque and for modes 1, 2 and 3. Mode 0 with mask check can't be written that way; it keeps
   the shader path that reads the target. Count how often titles hit it before doing anything smarter.
   Stencil and depth stay with the depth groups.
5. **Drop the target read.** Once every remaining blend is fixed-function, framebuffer fetch and the
   `reads_` pipeline set go, except for the mode-0-with-mask-check fallback.
6. **Measure** as in phase 1.

## Order and testing

Phase 1 first. It's independent of the format change and should take most of the cost away on its
own. Work and measure on Fedora (the Mac is too slow to iterate on). Runs stay windowless and silent
(`PSXPORT_SHOW_WINDOW=0`, `PSXPORT_NOAUDIO=1`), and `gputime` plus RenderDoc (`PSXPORT_RENDERDOC_AT`,
`tools/renderdoc_times.py`, `presentation.md`) show where time goes.

Once it lands, `presentation.md` § Record path needs rewriting: coverage is no longer "computed on the
CPU exactly as gpu.c does", and the known differences grow by edges and interpolants.
