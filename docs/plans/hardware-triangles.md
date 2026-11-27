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

### Phase 1 result (2026-11-17, M2, landed)

Done as planned, with three choices the plan left open:

- One shape stream. `RecordQuad` became `RecordShape`: a rect, or a triangle carrying its three corners with u, v
  and colour interpolants. Every shape is still six vertices in `record.vert`; a triangle's last three collapse onto
  its third corner. Batches, runs and depth pipelines are untouched.
- Interpolants are set from `gpu.c`'s own setup (base, dx, dy at S) as planes moved half a pixel, so the hardware's
  value at a pixel centre is `gpu.c`'s at that pixel, flip offsets and bias included. The op no longer carries bases,
  deltas or edges (`kRecordOpWords` 40 → 28).
- Triangles are clipped to the draw area on the CPU (Sutherland-Hodgman, a fan per plane) and drawn shifted by the
  11-bit wrap of their top-left corner. Texels are clamped to the polygon's UV range, low to one short of high, as
  DuckStation does: without it the edge pixels the hardware covers past `gpu.c`'s walk sampled neighbouring texels
  (white dashes along Spyro's chest lid at 8x).

Spyro 1 attract demo at 8x (psx-spyro `tools/demo_gputime.py`, shots from `tools/demo_shots.py`):

| | before | after |
|---|---|---|
| GPU ms/present p50 (gputime) | 40.9 | 19.4 |
| record / record composed, ms/submit | 17.9 / 19.5 | 8.0 / 9.6 |
| CPU present, cpu-ms/frame p50 | 19.7 | 10.7 |
| CPU frame, cpu-ms/frame p50 | 24.5 | 13.4 |
| triangles, ktri/present p50 | 41.0 | 5.1 |
| pixels shaded, Mpx/present p50 | 116 | 85 |
| recordcheck at 1x, differing pixels p50 | 57.3k | 60.0k |

CPU spread 1-5% over two runs a side; GPU counts repeat exactly. Shots at 1x and 8x differ only along polygon
edges; no seams seen in the demo. Stone Hill's slivers (0199, 0206) were not revisited.

### After phase 1 (M2)

With `gputime` split into stages (`record keep before`, `upload`, `draw`, `snapshot`, `in-between seed`,
`snapshot restore`), the draw passes were about half the GPU time and whole-plane copies a third. Landed, each
against the one before (two runs each, ms/present p50; the host is shared, so each step is also read against the
stages it did not touch):

| commit | change | total p50 | draw stage |
|---|---|---|---|
| b0b6375 | stage timing, shaded-pixel count, Model marks without texel work | 22.6 | 13.6 |
| 57c22db | depth interpolated and tested by the hardware (1/z, reversed); only marks write it | 22.1 / 22.7 | 13.8 / 14.3 |
| 192d36d | the VRAM plane's `before`, `inBetween` and snapshot copied only where they differ from the image | 18.3 / 18.4 | 12.1 / 12.1 |
| ae8a304 | op flags, colour, CLUT and window as flat varyings from `record.vert` | 17.9 / 17.9 | 11.8 / 11.7 |
| b97b60d | lean `record.frag` (no copy, upload, read-back or outline paths) for the runs that need none | 17.5 / 17.4 | 10.8 / 10.8 |
| be373cf | margin canvases shade only their margins; their buffer columns come from the image (shaded 58.7 → 34.1 Mpx) | 16.1 / 16.2 | 8.1 / 8.1 |
| 3a1959f | a margin canvas's before, in-between and snapshot copies take only its margin strips | 14.4 / 14.4 | 8.0 / 8.0 |
| 11cd44f | ops that never discard (fills, untextured, no mask test or skipped rows) through a `record.frag` with no `discard`, so the GPU keeps hidden-surface removal | 13.0 / 13.0 | 6.3 / 6.1 |
| f67fd79 | triangle- and sprite-only lean shaders, a sprite's texel base from `record.vert`, float colour and modulation (exact) | 12.6 | 5.1 |
| 8bbae08 | keep-before and the in-between seed skip what a record fills before it reads it (seed 2.45 → 1.26) | 12.7 (mean 13.62 → 13.25) | 4.8 |
| 33c6cc1 | snapshots refreshed only where a batch reads them; no per-batch copy, before-to-snapshot seed or restore | 10.2 (mean 10.69) | 4.8 |

From f67fd79 on, one run each (the host is shared; a second only when untouched stages drift). p10 went 9.0 → 7.3.

p10 went 16.9 → 10.3. Hardware depth was level on Spyro (it draws back to front, so early depth testing finds
little to reject) but leaves the test draws as plain pipelines. Copied pixels went 72 → 46 Mpx a present. The
draw stage is fragment shading: a trivial shader draws the same coverage in about 2 ms, and the demo shades about
59 Mpx a present (a 7.9 Mpx display, about 7x over).

At 3a1959f the mean present (ms) is draw 8.33, batch snapshot refresh 1.41, present build 1.40 and show 0.76,
in-between seed 1.16 and snapshot restore 0.99 (in-between presents only), keep-before 0.57, canvas interior copies
0.38, upload 0.20. Timing-only builds of the lean shader put the draw stage at 2.5 ms writing a constant (passes,
depth, marks, coverage), 6.5 without the texture fetch, and 8.5 fetching without the CLUT lookup: texturing costs
about 1.8 ms, the CLUT nothing, the rest of the shader body about 4. A reachable `discard` alone costs about 1.35 ms (it
turns off hidden-surface removal), which the opaque variant takes back for every op that cannot discard. The ranked
next steps were: slim that body, refresh read snapshots only where a plan samples them, skip seeding what a record
overwrites with an opaque fill before reading it (all three landed above), present in one pass, and present a margin
canvas from two textures instead of copying its interior (after the one-pass present, built on it).

### Phase 2 on Metal: deferred

Measured on the M2, phase 2 has little to win and real costs:

- Semi-transparent pixels are about 1% of the shaded ones (3.9k of 316k Mpx over the demo), and on Metal the
  shader reads the target in-pass (framebuffer fetch, tile memory), so no batch break or copy pays for a blend.
- A 4-byte target doubles every copy's bytes, and copies are still about a quarter of the GPU time; every texel
  read also packs RGBA8 back to 1555.

It still pays where the target read is a snapshot copy and a batch break (Vulkan, D3D12), so it stays the plan
there, done and measured on that machine. On the Mac the draw stage's shading and the remaining copies come first.

### Vulkan: the target read in-pass (2026-11-27)

The SDL fork's Vulkan backend reads colour target 0 in-pass (`read_in_pass` on the pass and the pipelines,
`SDL_PROP_GPU_DEVICE_COLOR_TARGET_READ_IN_PASS_BOOLEAN`): it becomes input attachment 0 of a subpass that has the
rasterization-order flag (`VK_EXT`/`VK_ARM_rasterization_order_attachment_access`). The record rasterizer uses it,
like Metal's framebuffer fetch, wherever the device offers it. Linux builds link the pinned fork for this
(`release_platform.pinned`).

RADV (RX 6700 XT, Ryzen Z1) and NVIDIA don't offer that extension. Those devices read in-pass through a barrier,
and get interlock only if that proves short:

1. **In-pass barrier (landed 2026-11-27).** The same input attachment with a subpass self-dependency, and no
   rasterization-order flag (`SDL_PROP_GPU_DEVICE_COLOR_TARGET_READ_IN_PASS_ORDERED_BOOLEAN` false). Where a draw
   reads pixels an earlier draw in the batch wrote since the plane's last sync, the planner records a sync before it
   (`RecordRasterPlan::syncs`) instead of closing the batch, and the rasterizer calls `SDL_SyncGPUColorTargetRead`
   there: a by-region barrier inside the pass, colour attachment write to input attachment read. Tomba! 2's village
   (the 2026-11-20 culling report's state, RX 6700 XT, 4K, 8x): render passes p50 22 → 2 per present, texture copies
   143 → 12, copy pixels 83 → 53 Mpx, against 13 syncs; the pictures match llvmpipe's ordered read at every pixel
   where they differ from the snapshot path (issue 0211). The meter's `target syncs` line counts the barriers.
2. **Fragment-shader interlock** (`VK_EXT_fragment_shader_interlock`), only if measured breaks show tier 1 is not
   enough.
   - Every draw that writes colour in such a pass stores through the interlock. Attachment writes and storage writes
     to one image can't share a pass.
   - Colour draws need early fragment tests, and a discard then leaves the depth and stencil writes in place. The
     stencil "passed" mark moves to a second ordered storage image, written in the same critical section.
   - On AMD, interlock serialises the whole overlapping fragment.

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
