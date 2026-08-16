# ONE RENDERER — the frame-build invariant

> **USER, 2026-08-16:** *"fps60 and regular should be rendering the same thing and should work the same
> underneath and again this has been repeated a million times, the only difference would be whether to
> add the extra lerp frames or not."*
>
> Earlier statements of the same rule, both already cited in `fps60.h`: **2026-07-15** — *"no difference
> between real and interpolated frames aside from lerp"*; **2026-07-22** — *"there should be just one
> site, the only difference should be whether to lerp"*.

Those two earlier rounds unified the two **frame kinds** (real vs interpolated) inside `fps60=1`. They
did not unify the two **configs**. This document is the invariant that does, and the evidence for why it
is worth defending.

## The invariant

**A logic frame's picture is built exactly once, by `Fps60::presentPass(t)`.**

- The **world** comes from the present-time build (`tier1Render` → the game's `fps60WorldPass` hook),
  reading inputs captured during guest execution: camera (`sceneCam` → `mCamCur`), per-object transforms
  (`projObj` → `mObjCur`), backdrop scroll (`bgScroll` → `mBgCur`).
- The **2D** comes from the captured render queue (`rq_capture`, accumulated across every flush).
- `t` is the only parameter. At `t == 1` every lerped input resolves to its current value, so the real
  frame is the in-between at its near endpoint — not a second code path that happens to agree.

**`fps60` changes exactly one thing: whether an ADDITIONAL `presentPass(t < 1)` is inserted before the
real one, and therefore whether the frame is paced as two halves or one whole.**

Anything else that differs between the two configs is a bug in this file's terms.

## What is allowed to branch, and why

| branch | why it is legitimate |
|---|---|
| `active()` in `present_vk` | THE extra frame. This is the invariant's one permitted difference. |
| `mHavePrev` in `present_vk` | scheduling, not building: with no previous frame there is nothing to lerp from, so no in-between is inserted. It selects *whether* to add a frame, never *how* one is built. |
| `active()` in `Fps60::rtp` | the logic-rate detector exists to schedule in-betweens; with none to schedule it has no job. Feeds no pixel. |
| `diff_mode` in `RenderQueue::flush` | SBS suppresses per-core present, so nothing would consume a capture and the panes would be black. A harness, not a config. |
| `psxRender()` in `presentPass` / `mWorldCaptureOnly` | the substrate/ORACLE leg is a different RENDERER by definition, not a different setting of this one. |

Adding a branch outside that table means the two configs have started to diverge again. Do not.

## Why this is load-bearing, not tidiness

Four independent bugs found on 2026-08-16 were all children of the config split, and **not one was caught
by a test** — all four were found by a person looking at the screen, days to weeks late, while 58
framework tests and the precommit gate stayed green:

- **kanban #94 / #35** — the whole 2D panel/prompt/dialog family was invisible at `fps60=1` and perfect
  at `fps60=0`. `Fps60::rq_capture` overwrote its snapshot per flush; a logic frame flushes once per
  guest `DrawOTag`, commonly twice, and Tomba!2 emits its 2D chrome in the first flush. At `fps60=0`
  `flush()` called `emitQueue()` directly, so every flush reached the picture. The fixes for that family
  had never regressed — they were verified on the leg where they worked.
- **`zfightScan` / `rqhist`** scan the FLUSH queue, which at `fps60=1` is not what gets drawn. The
  z-fight finder therefore reported `fight=0` on every outdoor scene from a denominator of **zero**
  candidate prims — a clean bill of health from an instrument that could not see the subject.
- **the painter-object layer** added 2026-08-14 is unreachable in the shipped config: its only call site
  is in `emitQueue`, which `fps60=1` never reaches. Dead code that reads as a feature.

The pattern is the point. A config product is a place for a bug to hide where nobody is looking, and this
project's users exercise exactly one cell of it.

## The three layers that had to be unpicked together

A partial unification is worse than none; both partial attempts were measured and produced broken
pictures. Recorded so the next person does not rediscover them:

1. **Present path.** `fps60=0`: `flush()` → `emitQueue()` → the guest's own `gpu_present`. `fps60=1`:
   `flush()` → `rq_capture()` → `frame_commit` → `present_vk` → `presentPass`. `frame_commit` itself
   early-returned when the tier was off, and the game chose a presenter with `if (!mods.fps60)`.
   *Unifying this alone leaves the cliff BLACK at `fps60=1`* — the field world is not in the captured
   queue at all, because it is capture-only at guest time.
2. **World emission TIME.** `mWorldCaptureOnly` gated on `mods.fps60`, so the world was built at guest
   time at 30 and at present time at 60. This is the layer that made it two renderers rather than one
   renderer with two presenters. *Unifying 1+2 without 3 renders the cliff as bare sea-and-sky bands at
   `fps60=0`* — `tier1Render` running against an uncaptured, zero camera.
3. **Capture chokes.** `sceneCam` / `bgScroll` recorded their cur-slots only `if (active())`, so
   `fps60=0` had no camera history at all. They are inputs to the one renderer and are now unconditional.

## Which direction the world build was unified in, and the cost

Two directions were available and both would satisfy the invariant:

- **World at GUEST time** (capture holds the world; the extra frame re-runs it lerped). Free for
  `fps60=0`, but it undoes kanban #33 and costs the DEFAULT config roughly a second world draw per
  frame — #33 measured the guest-time world draw at ~half of fps60's CPU.
- **World at PRESENT time** (chosen). Free for `fps60=1`, the shipped default; costs `fps60=0` the
  capture bookkeeping and the present-time build.

Measured, 900 headless frames, `AUTO_SKIP` field, `PSXPORT_NOPACE=1`:

| config | before | after |
|---|---|---|
| `fps60=0` | 2.5 s | 3.5 s (**+40 %**, 294 fps, `SCHED-LOGIC 2.15 ms` + `post 1.0 ms`) |
| `fps60=1` | 4.9 s | 4.8 s (unchanged) |

The +40 % is real and is recorded rather than waved off — but it is against a **30 fps logic target**,
i.e. ~10x headroom on the non-default config, so it was not worth optimising ahead of the correctness
this buys. If it ever matters, the lever is the capture bookkeeping (`mObjCur`'s per-frame map churn),
not the world build, which happens once either way.

## The acceptance gate

The REAL present must be **pixel-identical between the two configs**, and the check must be shown to
fail as well as pass. Measured on the four panel/scene replays:

```
                     before unification   after
cliff  fps60=0 vs 1        DIFFER        IDENTICAL
hut    fps60=0 vs 1      IDENTICAL       IDENTICAL
menu   fps60=0 vs 1      IDENTICAL       IDENTICAL
start  fps60=0 vs 1        DIFFER        IDENTICAL
```

`hut` and `menu` were already identical before, because their scenes are not tier1-eligible / issue one
flush — which is exactly why a two-scene gate would have passed throughout the entire period the bug was
live. **Gate on a scene set that includes a tier1-eligible field scene** (`cliff`) **and a two-flush 2D
scene** (`start`), or the gate proves nothing.

One trap, recorded: `shot` captures the LAST present, which is always the real frame, so
`PSXPORT_FPS60_TFORCE` cannot affect it and is **not** a usable negative control for this gate. The
control that does fire is the before/after above.
