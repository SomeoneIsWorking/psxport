# Frame and guest-widescreen presentation contracts

`FramePresenter` is the ordinary frame-fence owner. It captures every queue flush in one guest
frame, rebases ordering metadata across those flushes, emits one real picture, captures diagnostics,
paces the measured display fields, reconciles the presentation ledger, and resets current-frame
capture. None of those responsibilities is interpolation.

`FrameLoopShell` is the product entry boundary above that fence. After the title has registered its
generated overrides, `prepareProduct(Game&)` validates the mandatory `Game::frameDriver`, reinstalls
the framework-owned fatal VSync trap as the last generated override at that address, and requires the
measured VSync fact. It performs no implicit pad, audio, render, or present service; `step` delegates
exactly one finite `stepFrame(Core&, frame)` and refuses if that preflight was skipped. The title driver owns the measured ordering of those services and
must call `Game::presentation.commit(...)` exactly once, or implement a measured unpresented fence.
The shell snapshots `FramePresenter::fence()` around the call and aborts unless it advances by exactly
one, so both a missing fence and accidental double presentation are product-contract violations.
This keeps harness stepping and standalone stepping on one host-owned route without inventing one
console-engine frame recipe for every title.

`GameRuntime::createTemporalFramePresentation(Game&)` is an optional decorator factory. Direct
runtimes get `nullptr`; `LegacyGameRuntimeAdapter` creates `Fps60` only to preserve temporal consumers
during migration. An already-60fps title commits with:

```cpp
core->game->presentation.commit(core, guestFields);
```

A title whose measured logic cadence needs interpolation creates a temporal product and passes it to
the same fence. `Fps60::frame_commit` remains a bounded compatibility entry, while temporal capture
chokes use the checked `fps60(Game&)` accessor. A direct-runtime link falsifier proves that constructing
and committing a neutral `Game` does not link `Fps60` symbols.

The ownership follows from psxport's own state boundary: the game-clock/presentation fence is neutral,
while interpolation is composed only on a path that owns previous/current temporal state. Neither the
neutral path nor a non-temporal title depends on interpolation code.

## Title-declared presentation capabilities

Every `GameRuntime` returns a `RenderCapabilities` profile. This is the single title-owned answer for
whether Native producers exist, whether temporal interpolation exists, which render path ships by
default, and which paths a player may select. Direct runtimes must answer explicitly; the legacy
adapter explicitly preserves Native plus temporal interpolation while its consumers migrate.

An already-60fps widescreen-only runtime returns `RenderCapabilities::widescreenOnly()`: GTE is the
shipping/player path, PSX remains a diagnostic path, and Native plus temporal interpolation are
unsupported. `render_path_install`, RmlUi, REPL, and the debug server all consult this same policy.
An unsupported launch request resolves to the declared default and the live CVar reports that
effective path rather than the rejected request.

`Mods` consumes the temporal declaration before loading settings. Unsupported titles refuse an
enabled saved/environment fps60 request, keep the live field off, omit `fps60=` on the next save, and
publish no fps60 row binding. `MenuPane` removes unavailable bindings from both layout and navigation.
This is capability absence, not a disabled implementation and not a game-owned menu fork.

## Title-owned guest widescreen

The broad `RenderMode::enhancementsAllowed()` gate remains Native-only. Guest widescreen does not
relax it: GTE still receives no interpolation, internal-resolution scaling, native depth, or deferred
native passes.

A direct runtime may separately return a `GuestWidescreenProjection`. The policy declares an aspect,
but declaration alone cannot stretch or widen a frame. The title must call
`gpu_vk_latch_guest_projection` at its measured guest projection publication site and apply the
returned plan to its own projection, draw clipping, culling, and layout. The framework then exposes
the matching latched host presentation span on `RenderPath::Gte`. `RenderPath::Psx`, oracle, and SBS
remain 4:3 reference pictures.

The latch accepts three deliberately distinct title facts:

- GP1 display extent, decoded by the framework (including the dedicated 368-dot mode);
- title-authored projection extent, whose center determines OFX;
- title-authored guest draw/clip width.

Those values are not interchangeable. For example, a title may publish a 384x480 projection around
OFX 192 while displaying and clipping 368x448. The pure `GuestProjectionPlan` derives separate
presentation, projection, and draw widths and margins from one aspect rule. Each extent is widened
directly from its own 4:3 width to the smallest even width that does not undershoot the requested
aspect; rounding from a 320-wide display is never compounded into a distinct projection width. It never owns title
culling formulae, view matrices, H, OFY, primitive offsets, or executable addresses.

The plan is frame-stable: host presentation reads only the stored latch. Changing a setting cannot
widen an old guest projection; the title must publish the matching projection again. Invalid or zero
title geometry refuses instead of inventing a plausible default.

## Consumer migration

An already-60fps direct runtime should:

1. derive `GameRuntime`, not use the legacy compatibility bags;
2. return `RenderCapabilities::widescreenOnly()` and leave
   `createTemporalFramePresentation` at its null default;
3. implement and return a title-owned `GuestWidescreenProjection` only after locating the real guest
   projection/culling owners;
4. latch positive projection and draw geometry at that publication boundary, then apply the returned
   center/clip extents in title code;
5. commit each measured frame through `Game::presentation` with its real guest-field count.

Consumer verification must include 4:3 identity, the requested wide aspect, reference-path
suppression, and a real-frame A/B proving the original central picture is not rescaled and that H,
vertical center, UV, color, depth/order, and unrelated guest state remain unchanged.
