# Porting another PSX game with this framework

This project (psxport / Tomba2Engine) is built along a deliberate seam: a **PSX-generic
framework** plus a **game-specific reimplementation**. This doc is the map for pointing the
framework at a *different* PSX game — what transfers as-is, what needs per-game RE, and the
architectural patterns that make the per-game work small. It's written from the design
conversation behind the Tomba!2 port; treat it as the north star, not a checklist to follow
blindly.

Companion skills/docs (generic): `recomp-init`, `recomp-recompiler`, `recomp-overrides`,
`recomp-harness`, `decomp-port`, `ghidra-re`. Project-specific mechanics live in `docs/`
(`faithful-execution.md`, `render-arch.md`, `bug-hunt-workflow.md`, `fleet-workflow.md`).

---

## 1. The two-layer model

**Generic (~80% of the infrastructure — lift and reuse):**

- **Static recompiler** (`tools/`) — MIPS→C for any `MAIN.EXE` + overlays. N64Recomp-style.
- **Substrate/runtime** (`runtime/recomp/`) — `Core` (regs/RAM/scratchpad), `rec_dispatch`,
  the engine-override table, the guest-ABI + stack machinery, the fiber scheduler, boot.
  The whole "recompiled blob you progressively own" model.
- **PSX hardware backends** — GTE (COP2), SPU, MDEC, CD/XA/CDDA, GPU (GP0/GP1/OT/VRAM),
  beetle-derived. Pure PSX hardware; game-agnostic.
- **PSX-SDK HLE** — libcd/libgs/libgpu/libgte/libspu/libpad. Generic to any Sony-SDK PSX game
  (almost all of them). Version differences are the main caveat.
- **Differential harness + tooling** — SBS/oracle/abcompare, `port_check`/`port_gen`/
  `abi_extract`/`codemap`/`findings`, the Ghidra pipeline. The *methodology* of byte-gating
  native code against the recomp is fully generic.
- **Render substrate** — the VK renderer, the read-only-overlay rule, the widescreen/60fps/
  native-depth machinery. The *mechanism* (a PSX game's OT/GP0 stream → native picture) is generic.

**Game-specific (`game/` — the actual reimplementation):**

- Every RE'd object handler and its addresses (actors, AI, physics, quests, camera, the scene
  tables). Anything that reads a specific guest address or reproduces a specific `FUN_xxxx`.
- The area/overlay layout and specific scenes.

The per-game *code* doesn't transfer — but the **workflow, tooling, and patterns for producing
it do, completely.** The irreducible cost of a new game is RE labor, and the whole point of the
patterns below is to shrink that labor to a well-bounded surface.

---

## 2. Phase 0 — stand up recomp + harness FIRST

Before any game logic (see `recomp-init`):

1. Vendor a reference emulator (the **oracle**) and the CHD/BIOS provisioning.
2. Recompile `MAIN.EXE` (+ overlays) to `generated/` shards. Generated code is sacrosanct.
3. Boot the recomp under a native boot+frame loop with everything dispatched to the substrate.
4. Stand up the **differential harness** (SBS two-core / abcompare) that byte-compares the
   native run against the oracle every frame. **This exists before you own a single function.**

You now have a byte-gated recomp running. Everything after is *progressively replacing substrate
with owned native code*, gated by the harness.

---

## 3. Phase 1 — faithful: byte-exact native ownership

Own functions top-down along the execution spine, each gated to **byte-match the recomp**.

- **Mirror the guest stack.** A recomp'd MIPS leaf spills its incoming callee-saved registers
  onto its own guest frame; a shared callee re-spills them. So a native port of a caller must
  reproduce the guest register/stack frame exactly — descend `sp`, spill `r16..r23/r30/r31` at
  the RE'd offsets with the LIVE values, set `c->r[31]` to the RE'd jal-site constants before
  each dispatch, restore + ascend. `tools/abi_extract.py <addr> --contract` derives the frame.
  See `docs/faithful-execution.md` and `game/world/object_table.cpp`.
- **Pass-through registers are the caller's job.** If a function never references a callee-saved
  register (e.g. `r22/r23/r30`) but a callee spills it, that register must be maintained
  *upstream* (by the faithful entry that set up the frame), NOT inside the pass-through function.
  Getting this wrong looks like a divergence "in" the innocent function. (Tomba #37 lesson.)
- **No bandaids.** No magic offsets, no compare relaxations, no residual allowlists. A
  `rec_dispatch` MISS is fixed by seeding the recompiler, porting, or wiring an override — never
  by papering over it.

### Verification discipline — honest gates

The harness only tells you about what it **exercised**. Two traps that produce fake-green:

1. **Registration/override wiring** — if the native override isn't actually installed on the
   compared core, both cores run substrate and you get a hollow 0-diff. Probe that the override
   fired (Tomba: `PSXPORT_DEBUG=ovhit`).
2. **Mode-gated code paths** — some native code only runs under specific stage/mode state (Tomba:
   the faithful field-frame path only runs at `sm[0x4c]==3`; free-roam dispatches to substrate).
   A free-roam 0-diff never touched it. **Force the mode state AND probe the native body ran**
   (Tomba: `PSXPORT_SBS_FORCES4C` + an entry probe with count>0) before trusting the gate.

A green gate means what it exercised, nothing more. See `docs/findings/sbs.md`.

---

## 4. Phase 2 — enhancements via a read-only overlay

Enhancements (widescreen, 60fps, native depth, hi-res) are a **read-only overlay**: the PSX
render path still executes underneath (its guest-memory writes are part of the faithful state),
and the enhancement produces the *picture* from its own pass. **The overlay MUST NOT write guest
RAM** — a guest write from render is a bug that breaks the byte-compare. This rule is what keeps
enhancements orthogonal to faithfulness: you don't have to finish Phase 1 to have wide/60fps.

### Native depth is the north star

Painter's/OT order has no real depth — it only works for the *fixed* PSX camera because the game
pre-sorted for that one viewpoint. The moment you widen the FOV or interpolate a frame you've
moved the camera, and paint order is wrong (z-fighting, bad occlusion). **Native depth — real
world-Z in a depth buffer — is the only thing that survives a moving/wider camera.**

Crucially, wide + 60fps + correct occlusion are not three features; they are one capability:
**recovering world-space coordinates behind what's drawn.**

- native depth = the world Z per vertex,
- 60fps per-object anchor = the world XYZ per object,
- wide = projecting those world coords through a widened camera.

All three are "get the world coords."

### The reusable capability: bottom-up GTE capture

You do NOT need to RE every object handler top-down to get the world coords. The recomp gives
you a **single GTE execution choke point** — every perspective transform (`RTPS`/`RTPT`) runs
through one place. At that point you already see, per projection:

- the **input** — the local vertex + the translation/rotation control regs (the world/view
  coordinates being projected),
- the **output** — screen X/Y + Z,
- the **caller** — `ra` and whatever object/slot pointer is live in the arg regs.

So the world data is *free*. Capturing it per-primitive (this project's `projprim`/PGXP is the
seed) gives you native depth generically. Extend that capture with **one field — who issued this
projection** — and the same tap feeds per-object 60fps anchors and wide, for any PSX game.

**Identity is the real per-game slice.** To interpolate you must match this frame's primitive to
last frame's *same* object. World position can't be the key (moving objects). You need a stable
per-primitive identity read from the caller's context:

- `(node address, command/slot index)` — objects are usually fixed slots in a manager array, so
  the index is stable across frames. This is the 80% solution and it dissolves the classic
  "manager groups all its sprites under one OT node" problem: the OT node is shared, but each
  sprite gets its **own** GTE projection with its own coords and its own slot. The grouping never
  existed at the GTE level.
- a fingerprint (texture page + CLUT + relative offset) as a fallback.

`RotTransPers`/`RotTransPers3`/the `ldv`+`rtps` sequences are **Sony-SDK-shared**, so the
identity plumbing you write against them works across essentially every PSX game; game-custom
projection wrappers exist but are few and central. "RE what writes to the GTE" is exactly this:
understand the projection call chain well enough to read the object id out of the caller.

### The anti-pattern to avoid: OT classification + per-site tagging

When you render by walking the guest OT and *classifying leftover GP0 packets* — "is this a 3D
world poly the native pass already drew (drop) or a guest-only billboard (keep)?" — you end up
with a keep/drop gate that can't tell them apart intrinsically, so it uses a proxy: "was this
object's dispatch site manually tagged?" That tag becomes a per-site allowlist that grows every
time a class goes invisible (Tomba: `s_ot_2d_only` + `obj_depth`/`withDepthTag`). **It is a
symptom of not owning the object**, not a fix.

The proper fix is the project's core principle: **REBUILD from game state, don't transcribe the
OT.** Once you *own the object's handler*, the PC renderer reads it directly and draws each
instance from its own world position — per-object by construction (no grouping), correct depth
(no tag), no double-draw, no classification. The object never enters the OT-classification path
at all, so the gate has nothing to reason about. The bottom-up GTE capture above is the *cheap*
version of this for enhancement purposes (you get per-object world+depth without a full handler
port); a full handler port is the faithful end-state.

---

## 5. What transfers vs what's per-game (honest)

| Concern | Genericity |
|---|---|
| Recompiler, substrate, harness, tooling, PSX-HW, SDK-HLE | **Generic** — lift and reuse |
| Camera | **Mostly generic** — it *is* the GTE control regs (CR24-26); reading them is game-agnostic |
| 3D geometry through GTE (terrain, meshes, world billboards) | **Mostly generic** — world-Z recoverable from the GTE tap; heavy per-game RE here usually means the game feeds GTE pre-transformed |
| Per-object identity for interpolation | **Small per-game RE** — which register/slot holds the object id; spawn/despawn/slot-reuse tuning |
| **Parallax / 2D backdrops** | **Irreducibly per-game** — they *fake* depth with 2D scroll and never touch the GTE, so there is no world coordinate to recover. You must RE the game's parallax model (layers, scroll rates, pinned depth) |
| Object gameplay logic (AI, physics, quests) | **Per-game** — this is the long tail; own it top-down for faithfulness |

So the honest version of "make the next port easier": lean as hard as possible on the GTE tap for
native depth → camera + 3D + billboards become mostly generic (depth, 60fps, wide fall out
together). The per-game render RE then concentrates on the layers that don't go through GTE —
parallax/2D — a much smaller, well-bounded surface than "RE the whole renderer top-down."

Not fully automated (identity heuristics + parallax need hands), but the transferable core is
real and already isolated.

---

## 6. A concrete order of operations for a new game

1. **Provision + recompile** the target's `MAIN.EXE`/overlays; vendor the oracle emulator.
2. **Stand up the harness** (SBS/abcompare) and confirm byte-lockstep with everything on substrate.
3. **Lift the framework** unchanged: substrate, PSX-HW backends, SDK-HLE, tooling, render substrate.
4. **Bring up the GTE tap** (`projprim`/PGXP equivalent) → native depth for the 3D world. This is
   the single highest-leverage capability; wide + 60fps ride on it.
5. **Add per-primitive identity** from the caller context → per-object anchors (60fps) and the
   dissolution of manager-node grouping. Start with `(node, slot)`.
6. **RE the parallax/2D backdrop model** — the irreducible per-game render RE. Feed those layers
   correct depth/width explicitly.
7. **Own object handlers top-down** along the execution spine for faithfulness (Phase 1), each
   byte-gated with honest gates (force mode state + probe). Owning a handler also removes that
   object from any OT-classification scaffolding.
8. **Never** grow a keep/drop tag allowlist or write guest RAM from render. If you're tempted to,
   the object wants owning, not tagging.

---

## 7. One-paragraph summary

The framework is ~80% PSX-generic and already factored that way (`runtime/` + `tools/` + PSX-HW
vs `game/`). A new game inherits the recompiler, substrate, harness, PSX hardware, SDK HLE, and
render substrate on day one. The per-game work is RE labor, and the patterns keep it small:
**native depth from the GTE tap** makes camera/3D/billboards mostly generic; **per-primitive
identity from the caller** gives per-object 60fps/anchoring and un-groups manager sprites;
**owning handlers** (rebuild from state, don't transcribe the OT) removes the classification
bandaids. The irreducible per-game surface is **parallax/2D backdrops** (no GTE coords to
recover) and **gameplay logic**. Gate everything against the oracle, and never trust a green gate
without proving it exercised the code.
