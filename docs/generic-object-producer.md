# The GENERIC OBJECT PRODUCER — how a port stops losing graphics one object at a time

**The problem this solves, in the shape it actually arrives in.** Under `pc_render` nothing walks the
guest's ordering table, so a primitive reaches the screen only if a native producer drew it. Ports
therefore accumulate a long tail of "X is missing" reports — a torch flame, a fisherman, a winch, a
minimap — each investigated, RE'd and ported separately. That tail does not converge: it is as long as
the game's object catalogue.

> USER, 2026-08-19: *"try to solve the missing graphics problems globally so we don't have to solve them
> object by object"*.

The answer is NOT a fallback that re-draws the guest's output. It is a **generic producer**: one native
draw path fed by the same inputs the guest's own renderer starts from — an object's MODEL and its OWN
TRANSFORM — reached by redirecting the game's per-object dispatcher for every object whose emitter has
no specific producer. Specific producers stay; the generic one covers everything they do not.

---

## 0. FIRST, THE THING THAT DOES NOT WORK — do not rebuild it

Re-emitting the guest's ordering table for anything with no native producer is the obvious idea, it is
cheap to build, and it is structurally wrong:

> **OT/GP0 content is POST-PROJECTION 2D in the guest's own display space.** World position and depth are
> gone by the time a packet exists.

Built and measured on Tomba!2, 2026-08-19, then reverted the same day:

| symptom | measurement |
|---|---|
| cannot be re-projected | engine wide (draw clip 0..693): the native pass drew the world at 694 px and the re-emitted prims drew it AGAIN at 4:3 — the scene rendered **twice, side by side** |
| cannot take part in depth | an early revision re-emitted **832 of 1034** OT nodes and buried the player behind duplicate terrain |
| drags the CPU rasterizer in | replaying GP0 words ran the software raster over the whole OT: **110 fps → 25 fps** |

USER: *"no CPU raster ever"*, and, on seeing the duplicated frame: *"This is probably why I banned GTE
before"*. `docs/workspace/PROTOCOL.md` holds the ban. Everything below draws from the guest's INPUTS
instead, which is why it survives widescreen, the depth buffer and 60fps interpolation.

---

## 1. THE SHAPE, IN FOUR PARTS

Every PSX-era engine this framework has met has the same skeleton, under different names:

1. **An object/entity list** the frame walks.
2. **A per-object COMMAND LIST** — each command carries a model pointer plus that command's own
   rotation and world position.
3. **A per-mode EMITTER RESOLUTION** — a mode byte and a jump table pick which routine turns this
   command into primitives, with a GENERIC emitter as the fallback case.
4. **A MODEL RECORD FORMAT** — counts, then fixed-stride triangle and quad records.

The generic producer sits at (3). At the point the dispatcher has resolved which emitter a command
would go to, ask one question — *does that emitter have a native producer?* — and if not, draw the
command natively from (2) and (4) instead, then let the substrate body run untouched underneath.

```
resolve emitter for this command
  ├── emitter has a native producer      → let it run; nothing to do here
  └── emitter has NO native producer     → NATIVE DRAW from (model, transform), then run the
                                            substrate body anyway (it fills the packet pool /
                                            ordering table, which is part of byte-exact state)
```

## 2. WHAT EACH GAME MUST RE — and it is not much

This is the part that cannot be shared, and it is four facts:

| fact | how to find it | Tomba!2's answer |
|---|---|---|
| the per-object command walk | the function that iterates a node's command list | `FUN_8003CDD8` (`Render::cmdListDispatch`) |
| the emitter resolution | mode byte + jump table + the generic fallback label | `FUN_8003F698`, table of 10 cases, generic `func_800803DC` |
| the command's transform fields | the fields the guest feeds to `SetRotMatrix`/`SetTransMatrix` before emitting | `cmd+0x18` rotation, `cmd+0x2C` world position |
| the model record format | the emitter's own parse loop | `geomblk+0` = {tri count lo16, quad count hi16}, tri records at `+16` stride 36, quads after, stride 44 |

Take the transform from **the same fields the guest passes to the GTE**, never from the GTE's output
registers. That is the whole distinction between this and the banned fallback: the inputs are world
space, the outputs are not.

## 3. THE INVARIANTS — each one is a bug someone already shipped

- **No double-draw.** A node a specific producer already drew this frame must not be drawn again. Keep a
  per-frame "drawn natively" set and consult it (Tomba!2: `Render::nativeObjDrawn`). Double-drawing is
  not merely wasteful — the duplicate geometry wins depth ties and swallows sprites drawn earlier.
- **The native draw is a DISPLAY-PASS addition inside a substrate body that legitimately writes guest
  RAM.** Scope the read-only guard tightly to the draw itself, not to the enclosing function.
- **Give the prims the real object identity** (the owning node), so depth, per-object interpolation and
  every diagnostic keyed on object identity keep working.
- **Attribute the draw to the EMITTER it stands in for**, not to the dispatcher, or the producer census
  grows a row for the dispatcher and the emitter's own row stays looking unowned forever.
- **The substrate body still runs.** The generic producer changes the PICTURE decision only; the guest's
  packet-pool and ordering-table writes are part of the byte-exact state a dual-core compare checks.
- **Gate to the scene window where the native pass owns the picture**, so it can never run on a pure
  guest-render leg (oracle / SBS / menus), where an extra native draw would double the picture.

## 4. HOW TO KNOW IT WORKED — and how not to fool yourself

- **Gate on an IN-BAND COUNT, not on pixels.** A per-redirect diagnostic channel with a denominator
  (commands seen / redirected / declined) says the path ran. Equal pixel counts prove nothing on their
  own; a leg that failed to rebuild produces them too.
- **A/B at a FIXED FRAME, one line apart.** Same replay, same frame, with and without the widened gate.
  On Tomba!2 the free-roam A/B came back **0 of 76800 px** — no regression, and also no coverage in that
  scene, which is information: it says the scene under test had nothing missing on this path, so pick a
  scene where something IS missing before claiming the feature works.
- **The producer census is the work list.** Run one leg native and one leg guest-render over the same
  session and diff the submitter rows: submitters with guest primitives and no native production are
  what remains. On Tomba!2 that was **27 of 43 submitters**, with the generic emitter second at 6.1M
  primitives across one 30k-frame session — i.e. most of the tail was one unowned emitter, which is the
  whole reason to fix it generically.
- **A guest-render reference leg can be BLACK.** Do not read "the reference has no content" as "the
  native picture is wrong" — check the reference's own non-black count first.

## 5. WHY THIS IS NOT A STOPGAP

`PROTOCOL.md`'s BREAK FIRST, THEN REBUILD bans keeping a wrongly-sourced producer alive beside its
replacement. The generic producer is not wrongly-sourced: it draws from the model and transform the game
itself supplies, with the port's own projection and depth. A specific producer added later replaces it
for that object because the object's emitter gains native ownership and the generic path stops firing
for it — the mechanism narrows by itself as ownership grows, and never needs to be torn out.
