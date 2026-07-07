# PC-native subsystem rebuild — the "class Scene + guest mirror" methodology

USER DIRECTIVE (2026-07-01, refined): rebuild the game's subsystems as **real PC-game structure** — C++
classes (`class Scene`, `class Camera`, `class ObjectWorld`, …) with named state, enums, and meaningful
methods. The code MAY read/write **guest memory directly** so the still-recompiled parts keep working —
**no mandatory mirror layer**. The thing to avoid is NOT "touching guest RAM"; it is **structureless
op-by-op transcription** — "what we could just do via Ghidra": magic constants, magic compares, magic
call-args, raw `mem_r8(base+0x14)` byte pushing, and especially MIPS-in-C (`c->r[29]-=24; sw ra,16(sp)`).

## What "PC-game structure, not transcription" means
- **THE ANTI-PATTERN (do not do this):** `ov_scene_77d8c` — a countdown byte with magic compares (`==1`,
  `==2`), a substrate call with magic args (`0x80074590(41,2,-65)`), magic writes (`135`/`0`) back to a
  raw guest address. No named concept anywhere. That is a decompiler's output wearing a C function.
- **THE GOAL:** the SAME behavior expressed as PC-game structure — e.g. a named SFX-cue timer with an
  `enum Cue`, a named countdown field, and a `tick()` that plays the cue by name. It is FINE for `tick()`
  to keep the state in guest RAM (via a named accessor) and to call the still-substrate sound leaf — what
  changed is that the code now has *meaning* (types, names, methods), not raw offsets and magic numbers.
- **Direct guest use is allowed and expected.** A subsystem class can hold its state in guest RAM behind
  named accessors, or in native fields, or both — whatever is cleanest. A mirror/write-back is only worth
  building when it genuinely simplifies the code, NOT as a rule.
- **0-diff A/B stays a REGRESSION check** wherever the owned code still shares guest RAM with substrate
  (proves you didn't corrupt the interface) — but it does NOT dictate the code shape, and a subsystem that
  no longer shares a field with substrate needn't keep that field byte-identical.

## KEY FINDING — why the top-down ov_field_frame descent forced transcription (later-289 retro)
Descending `ov_field_frame`'s children one control-function at a time lands on clusters whose DATA is
co-owned by large substrate leaves. Example: the field event/command struct @0x800ed058 (driven by
FUN_80025588) is co-mutated by `FUN_80040aa4` (41 insns, calls spawn 0x8007a980) and `FUN_80024f18`
(**182 insns**, touches dozens of struct fields + globals 0x800bf80c/0x1f800137/0x800bf91e). Owning the
control function while those leaves stay substrate means round-tripping the whole struct to guest RAM
around every leaf call — a sync wrapper around PSX code, i.e. transcription. So:
- **DON'T** pick a target by walking control flow down from the frame loop.
- **DO** pick a COHESIVE SUBSYSTEM whose data a native model can genuinely own (few/no substrate leaves
  co-mutate its core state), rebuild the whole thing as a class, mirror the read surface.

## Existing native-model precedent to build on
- `game/render/scene_data.h` — `RenderScene`/`SceneObject`/`SceneCamera`: a native, float IR of the
  frame the renderer consumes. READ-ONLY (built from guest RAM, mirrors nothing back). The gameplay-state
  equivalent (a bidirectional native model that mirrors TO guest) does not exist yet — that's the work.
- `game/render/fps60.h` `Fps60`, `game/render/render_queue.h` `RenderQueue`,
  `game/world/spawn.h` `PoolDesc` — native state structs already in the tree.
- Object BEHAVIORS are already ~149/150 native at A00 free-roam (`ents` REPL) — the object system is the
  most-native subsystem and thus the strongest first candidate to formalize as `class ObjectWorld`
  (entity pool + spawn + node/animation), owning its data model and mirroring the node fields substrate
  render/collision still reads.

## Method (per subsystem)
1. RE the subsystem's data model + lifecycle. Give the state real names, types, and enums.
2. Build `class Foo` (game/<subsystem>/): meaningful methods over named state. State may live in guest RAM
   behind named accessors, in native fields, or both — pick what reads cleanly. No magic constants: name
   the modes/cues/flags. Keep calling still-substrate leaves where the cluster isn't owned yet.
3. Wire it into the native frame loop; drop the substrate dispatch for what it replaces.
4. Verify: `./run.sh` (USER eyeballs it plays right) AND, wherever the owned code still shares guest RAM
   with substrate, the A/B RAM+scratchpad 0-diff as a REGRESSION check (you didn't corrupt the interface).

## Candidate ranking (first target)
1. **ObjectWorld / entity system** — behaviors already native; formalize pool+spawn+node as a class.
2. **Camera** — reported complete (memory: camera-system-done); low-risk to formalize as `class Camera`.
3. **Player** — cohesive (pos/vel/state/health) but needs deep physics RE; higher effort.
AVOID first: the field scene-control state machines (0x800ed058/0x800bf548/0x800bf870) — leaf-entangled.
