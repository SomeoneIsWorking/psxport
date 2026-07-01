# PC-native subsystem rebuild — the "class Scene + guest mirror" methodology

USER DIRECTIVE (2026-07-01): rebuild the game's subsystems as **real native C++ classes** (`class Scene`,
`class Camera`, `class ObjectWorld`, …) that OWN their data model, AND **write-through mirror** the fields
that still-substrate content reads back into guest RAM, so the recompiled game keeps working. Do BOTH.
"I just don't want you to do what we could just do via Ghidra" — i.e. do NOT hand-translate MIPS bodies
op-by-op (`c->r[29]-=24; sw ra,16(sp)`, raw `mem_r8(base+0x14)` FIFO shifts). Build a genuine engine.

## What "native, not transcription" means here
- **Native model owns the truth.** A subsystem's state lives in native fields with real names/types and
  real logic (`enum Phase`, `std::deque<Cmd>`, `float pos[3]`). The per-frame update reads/writes those
  native fields, expressed as normal C++.
- **The guest mirror is a boundary layer, not the model.** A `mirror_to_guest(Core*)` writes the native
  fields into the guest addresses that substrate readers consume; `sync_from_guest(Core*)` pulls back any
  field a substrate leaf mutated. Mirror ONLY the fields substrate actually reads — private native state
  never touches guest RAM.
- **0-diff A/B proves the MIRROR, not the code shape.** Keep the RAM+scratchpad 0-diff gate as a
  correctness check on `mirror_to_guest`, but let the code be written as an engine. If a field isn't read
  by substrate, it doesn't need to match and doesn't belong in the mirror.

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
- `game/render/scene/scene_data.h` — `RenderScene`/`SceneObject`/`SceneCamera`: a native, float IR of the
  frame the renderer consumes. READ-ONLY (built from guest RAM, mirrors nothing back). The gameplay-state
  equivalent (a bidirectional native model that mirrors TO guest) does not exist yet — that's the work.
- `game/render/fps60_internal.h` `Fps60State`, `game/render/render_queue.h` `RenderQueue`,
  `game/world/world_pool.h` `PoolDesc` — native state structs already in the tree.
- Object BEHAVIORS are already ~149/150 native at A00 free-roam (`ents` REPL) — the object system is the
  most-native subsystem and thus the strongest first candidate to formalize as `class ObjectWorld`
  (entity pool + spawn + node/animation), owning its data model and mirroring the node fields substrate
  render/collision still reads.

## Method (per subsystem)
1. RE the subsystem's data model — what state, what lifecycle. Name the native fields.
2. Find the MIRROR SURFACE: which guest addresses does still-substrate code READ? (grep generated/substrate
   + the leaves). Those are the only fields `mirror_to_guest` must write. Everything else stays native-only.
3. Build `class Foo` (game/<subsystem>/): native fields + native update logic + `mirror_to_guest` /
   `sync_from_guest`.
4. Wire it into the native frame loop; drop the substrate dispatch for what it replaces.
5. Verify: `./run.sh` (USER eyeballs it plays right) AND, while substrate still reads the mirror, the
   A/B RAM+scratchpad 0-diff (proves the mirror). Drop the 0-diff gate for a field once nothing reads it.

## Candidate ranking (first target)
1. **ObjectWorld / entity system** — behaviors already native; formalize pool+spawn+node as a class.
2. **Camera** — reported complete (memory: camera-system-done); low-risk to formalize as `class Camera`.
3. **Player** — cohesive (pos/vel/state/health) but needs deep physics RE; higher effort.
AVOID first: the field scene-control state machines (0x800ed058/0x800bf548/0x800bf870) — leaf-entangled.
