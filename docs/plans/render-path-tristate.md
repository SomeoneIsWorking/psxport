# The THREE-WAY render-path toggle — orientation + design

**Status: LANDED as psxport `e16c58dc`** (2026-08-11) — steps 1-4 of the plan below are done and
measured; steps 5 (the F1 overlay row) and 6 (converting the games' gates, then deleting the aliases)
are NOT. Kept as the design record: what was measured, what was decided with the user, and what the
two present-path bugs actually were. The measurements live in spyro C168 and issue 0055.

Companion doc: `docs/plans/graphics-producer-db.md` — this toggle is the instrument that makes that
DB's "GTE/OT vs native producers" comparison observable per producer.

Citations are `file:line` as of `d6b8e17d`, the state this was WRITTEN against; the code has since
moved (that is what `e16c58dc` is), so read a line number as "where this was", not "where it is".

---

## The ask (USER, 2026-08-11)

> *"one more thing, need a toggle to switch between PC render native, PC render from GTE and pure PSX
> restraizer"*

Three modes, one switch:

| # | mode | what it means | who produces geometry | who rasterizes |
|---|---|---|---|---|
| 1 | **native** | the PC owns the picture from game state | native producers (`game/render/`) | SDL_GPU (`gpu_vk.cpp`) |
| 2 | **gte** | the guest's own GTE+OT output, drawn by the PC | guest code, replayed via the OT walk | SDL_GPU |
| 3 | **psx** | pure PSX reference | guest code | the CPU rasterizer into `s_vram` |

## All three already exist. None of them is a toggle.

| mode | mechanism today | how it is selected today |
|---|---|---|
| 1 | `RenderMode::psxRender()==false` — the native scene walk (`render_mode.h:15`) | the default |
| 2 | `psxRender()==true` → the guest OT walk (`gpu_native.cpp:2071-2096`) feeds `gpu_gp0`, which tees prims to the VK rasterizer (`vk_path()`, `gpu_native_internal.h:56`) | `PSXPORT_RENDER_PSX` (`native_boot.cpp:610`), REPL `renderpsx` (`repl.cpp:283`), per-core in SBS (`sbs.cpp:658`) |
| 3 | `GpuState::soft_gpu=1` → `sw_path()` (`gpu_native_internal.h:57`) runs `tri()`/`raster_sprite()` into `s_vram` (`gpu_native.cpp:940,991`) | **NOWHERE user-reachable.** Only `sbs.cpp:2022` (`M_ORACLE` core B) and `selftest.cpp:217,337` set it |

**The measured gap is not a missing renderer — it is four overlapping switches with no single source
of truth:**

| switch | what it actually sets | reachable |
|---|---|---|
| `PSXPORT_RENDER_PSX` | `RenderMode::psxRender` only | env + REPL, per-Core |
| `PSXPORT_ORACLE` | `setOracle()` + forces `psxRender=true`, and is consulted by every enhancement gate so no PC enhancement can leak into the picture (`native_boot.cpp:597`, `cfg.h:25`) | env only, launch-time |
| `PSXPORT_GATE` | `Game::psx_fallback` — gameplay runs the substrate; **not a render switch at all**, but it silences every native override (`native_boot.cpp:604`) | env + REPL `gate` |
| `GpuState::soft_gpu` | the rasterizer | not reachable outside SBS/selftest |

The cost of that sprawl is documented, not theoretical: `Tomba2Engine/docs/gfx-debug.md` carries a
whole section on **which flag to use — `PSXPORT_ORACLE=1`, NOT `PSXPORT_RENDER_PSX=1`** (kanban #78,
measured 2026-08-06), because the two look interchangeable and are not. Mode 2 as spelled by
`RENDER_PSX` still has PC enhancements live in the picture; mode 2 as spelled by `ORACLE` does not.
**Those are two different modes, and the tri-state has to say which one it means.**

---

## Design

### One enum, on `RenderMode`, per Core

```cpp
enum class RenderPath { Native = 0, Gte = 1, Psx = 2 };
```

- Lives on `RenderMode` (`render_mode.h`), which is already per-Core via `Core::rsub`
  (`render_substrate.h:25`) — SBS/dualcore set A and B independently, which is exactly what the
  compare needs and what a process global would break.
- `psxRender()` and `softGpu()` become **derived** from it (`path != Native`, `path == Psx`), not
  independently settable. That is the point: it is impossible to express "PSX rasterizer + native
  producers", which is not a mode, only a way to get a black screen.
- `soft_gpu` moves from `GpuState` to being read off the Core's `RenderMode` at prim time
  (`vk_path()`/`sw_path()` consult it), so the rasterizer choice and the geometry-source choice
  cannot disagree. `GpuState::soft_gpu` is DELETED, not left as an alias — a second way to say it is
  how the four switches above happened.

### DECIDED (USER, 2026-08-11): THE ENHANCEMENTS ARE NATIVE-ONLY. BOTH guest paths are pure.

> *"I don't want GTE enhancements, GTE/OT should stay pure"* — and, confirmed directly when this
> consequence was put to them: *"Yes fps60/wide/native-depth is supposed to be native-only"*.

So **the enhancement lockout hangs on `path != Native`**, not on `path == Psx`. `fps60` interpolation,
widescreen geometry, native depth compositing and observer tagging run in `native` and nowhere else.
Those gates already refuse to run under `oracle_mode()` (`native_boot.cpp:593-600`), so the change is
to widen that one predicate — either repoint each gate at `path != RenderPath::Native`, or make
`oracle_mode()` mean exactly that and leave the call sites alone. Prefer the second: there are only
four `oracle_mode()` call sites, and one predicate cannot drift out of sync with itself.

**This is settled, not a recommendation — do not re-derive it.** The consequence is that
**modes 2 and 3 differ by exactly one thing: the rasterizer**, which is what makes the pair a usable
A/B (a difference between them is attributable to rasterization and nothing else), and that there is
NO mode meaning "guest geometry with PC enhancements". An enhanced frame is a frame the PC produced.

This does not add a mode; it DELETES one. Today's four switches map onto the enum like this:

| today | new path | note |
|---|---|---|
| (default) | `native` | unchanged |
| `PSXPORT_ORACLE=1` | `gte` | today's ORACLE already = pure guest render on the PC rasterizer |
| `PSXPORT_ORACLE=1` + `soft_gpu` (only `M_ORACLE`/selftest reach it) | `psx` | now user-reachable |
| `PSXPORT_RENDER_PSX=1` | **no equivalent — deleted** | guest geometry WITH PC enhancements live is exactly the thing kanban #78 warns people not to mistake for a reference. It stops existing rather than being documented around |
| `PSXPORT_GATE=1` | orthogonal, stays | it is a GAMEPLAY switch (`psx_fallback`), not a render one |

**Two consequences to state up front, because they are behaviour changes, not renames:**

1. **There is no longer any way to interpolate or widen the guest's own render** — which is the
   intended shape, not a cost: it agrees with the standing rule that interpolation is unlocked by
   *execution ownership* (`docs/workspace/WORKSPACE.md` / `CLAUDE.md`, the Dusklight
   record-and-replace caveat). You get 60fps by owning the producer, not by lerping guest output.
   It also means the enhancements stop being a variable in every guest-leg measurement.
2. **Every existing `PSXPORT_RENDER_PSX` A/B changes meaning** (it becomes pure). The sites to
   re-read before flipping the switch: `dualcore.cpp:95` (per-core render path), `sbs.cpp:656-662`
   (`M_RENDER`/`M_GAMEPLAY`/`M_FULL`/`M_SKIP` set `psxRender` per leg), `repl.cpp:283`
   (`renderpsx`), and each game's A/B scripts. An SBS B-leg becoming enhancement-free is a
   strictly better oracle; a script that was measuring "guest render + fps60" is measuring something
   that no longer exists and must say so rather than silently reporting the pure number.

### Selection, through the CVar ladder — not a fifth env var

A `TextVar cv_render_path` in `config_vars.h` / `config.cpp`: `Default` = `native`, `Value` from
`psxport_settings.ini`, `Override` from `PSXPORT_RENDER_PATH`, `Runtime` from the REPL
(`config_var.h:56`). Consequences that come free and matter:

- `cvars` at the REPL prints the mode **and which layer it came from** — so "was the flag actually
  applied" stops being a question you have to already suspect.
- An unknown value is NAMED at startup by the existing audit
  (`[cfg:warn] UNKNOWN knob X is set and matched nothing`), instead of silently meaning `native`.
- `PSXPORT_RENDER_PSX` / `PSXPORT_ORACLE` become **compatibility aliases that map onto the enum and
  log the mapping**, then get deleted once the games' scripts/gates are converted. No tombstones
  (global CLAUDE.md): once nothing sets them, every reference goes.
- **HOTKEY: `F5` cycles the path live** — `native -> gte -> psx -> native` — which is what the USER
  actually asked for ("need a toggle to switch between PC render native, PC render from GTE and pure PSX
  rasterizer"). The env var alone does not satisfy that ask: the three paths only mean anything next to
  each other, and comparing them by relaunching loses the scene, so you can never hold one frame still
  and swap the renderer under it. `pad_input.cpp`, edge-detected beside the existing `P` / `.` debug keys.
  The cycle ORDER lives in one place (`render_path_next`, `render_mode.h`) and is shared with the REPL, so
  key and command cannot drift; the order is asserted by `tests/test_render_path_cycle.cpp` because it is
  part of the contract — each step changes exactly ONE variable (Native->Gte swaps the geometry source
  with the rasterizer fixed; Gte->Psx swaps the rasterizer with the geometry fixed), so a visible
  difference stays attributable. The key writes the CVar's Runtime layer too, so `cvars` and any capture's
  provenance report the LIVE path rather than the launch value. REFUSED OUT LOUD under
  `PSXPORT_ORACLE`/SBS: those runs exist to BE the reference, and a keystroke that silently changed the
  renderer mid-compare would invalidate the run while looking like nothing happened.
- REPL: `renderpath [native|gte|psx]`, bare form cycles and prints. `renderpsx` stays as an alias
  that logs the mapping, for the A/B scripts that already pipe it (`repl.cpp:279-288` documents that
  a bare `renderpsx` toggling — rather than printing — was itself a past silent-A/B bug; do not
  regress that).
- F1 overlay row (`mods.cpp` writes the `Value` layer today for `fps60`) — a three-state row, so the
  user can flip modes without a relaunch. This is the toggle the ask actually asks for; the env var
  is the agent-facing half.

### THE MODE MUST APPEAR IN EVERY OUTPUT

Non-negotiable, and it is the whole reason to build this rather than three flags: the path name goes
into the per-frame `gpu` log line, the exit summary, the `shot`/`vkshot` PPM's sidecar, and the
producer-DB run log. A screenshot whose mode is unrecorded is a capture that can be attributed to the
wrong renderer, which is the failure this workspace has already paid for twice (every headless timing
number before psxport `80e3d203`; the `RENDER_PSX`/`ORACLE` mixup).

---

## MEASURED (2026-08-11): mode 3 was BLACK, for TWO reasons

Stage 1 below was run before anything was designed around mode 3, and it was right to be: the
presented frame was **0.0% non-black / 1 colour** at presents 700, 1200 and 2010, while a direct
`s_vram` capture of the SAME run was **81.5% / 2117 colours**. The rasterizer was drawing; the present
was discarding it. Both halves are one blindness — the VK present learns of framebuffer change ONLY
through `gpu_vk_dirty()`, and every call site of it is gated `if (vk_path())`:

1. `present_rebuild_decision` saw `batchEmpty=true` (permanently — the software path never tees a
   primitive to VK) and a write counter that never moved, so it returned `REUSE_LAST` for the whole
   run, re-showing a composite that had never been built once.
2. **Fixing only that still presented black:** the upload takes the dirty RECT list, which is empty for
   the same reason, so a rebuild uploaded nothing.

After both fixes: 81.7%/2166, 93.3%/3737, 93.3%/3508 — with colour counts DIFFERENT from the `gte`
leg's 2008/3534/3284 at the same frames, which is what proves the capture is the software rasterizer's
own picture rather than the VK one leaking through. Full write-up: spyro `docs/issues/0055`.

**What that run does NOT show:** with `PSXPORT_SPYRO_FRAME_LOOP` unset, spyro's picture comes from the
guest's own driver on BOTH the `native` and `gte` legs, so their near-identical numbers are not
evidence that a native producer drew anything. It is evidence that all three paths PRESENT.

## What was NOT free — the risks, as they turned out

1. **Mode 3's present was the predicted failure, and it was worse than predicted** (two bugs, not one).
   Original note, kept because it was the reason to measure first: `present_window()` blits `s_vram` through
   `gpu_vk_present` (`gpu_native.cpp:1526-1527`), and under `soft_gpu` the SW rasterizer writes
   `s_vram` — so the picture should be coherent by construction, and mode 3 needs no new present
   path. But the VK geometry batch is EMPTY in that mode, and an empty batch has produced a fully
   black present before: **spyro issue 0043**, "the empty-batch present early-out re-introduced issue
   0029 one level up" — presents 30-420 at 0.00% non-black, 1 colour, resolved 2026-08-04. Stage 1
   below is a captured frame with a colour count, not an assertion that this works.
2. **Tomba2Engine deleted its PSX reference by user directive.** `docs/gfx-debug.md`: *"There is no
   oracle and no render-diff tool anymore … the engine OWNS its world, projection, and render
   ordering PC-native — so there is nothing PSX to diff against."* Mode 3 in that repo will run the
   guest's own render, which the port has largely stopped feeding. **Expect mode 3 there to be
   degraded or empty, and report that as a measurement, not as a bug in the toggle.** The toggle is
   framework-level and correct in all three trees; what mode 3 can SHOW is a per-game property of how
   much the PC has taken over. That fraction is exactly what the producer DB records.

## Staged plan — RED test first (`docs/workspace/PROTOCOL.md`)

Claim: `mkdir coord/claims/render-path-tristate`. Framework-only; the games change only their launch
scripts and gates.

1. **Prove mode 3 presents at all, before designing around it.** No framework change: a spyro run
   with `soft_gpu` forced on (temporary local edit, or via `M_ORACLE`), `PSXPORT_SHOT_AT`, read with
   `tools/ppm_look.py` — the same instrument that settled issue 0043, reporting `% non-black` and
   distinct-colour count. If it is black, fixing that IS stage 1, and the toggle waits.
2. **`RenderPath` enum on `RenderMode`; `psxRender()`/`softGpu()` derived.** RED test
   (`psxport/tests/test_render_path.cpp`, hermetic): every enum value maps to exactly one
   (`psxRender`, `softGpu`) pair, and the invalid pair is unrepresentable. Delete
   `GpuState::soft_gpu`; repoint `sbs.cpp:2022` and `selftest.cpp:217,337` at the enum.
3. **`cv_render_path`, and the enhancement gates repointed at `path != Native`.** RED test in
   `tests/test_config_cvar.cpp` (the existing compatibility gate): `PSXPORT_ORACLE=1` resolves to
   `gte` and is byte-for-byte the behaviour it had — that one IS a pure rename and must be checked,
   not asserted. `PSXPORT_RENDER_PSX` is the opposite case: it does NOT round-trip, so it maps to
   `gte` and **logs at warn** that its enhancement-live meaning is gone (per the decision above),
   until step 6 deletes it. A test asserts the warn fires — a silently-changed flag is how the
   headless-pacing class of bug happened.
4. **REPL `renderpath` + the mode stamp in the frame line / exit summary / shot sidecar.** RED test:
   flipping the path mid-run changes what the stamp reports on the next frame.
5. **F1 overlay three-state row** (`mods.cpp`), writing the `Value` layer like `fps60` does.
6. **Convert the games' gates and A/B scripts** to `PSXPORT_RENDER_PATH`, then delete the aliases and
   every reference to them. Re-run each repo's boot gate; it must reach at least as far as before
   (`docs/workspace/PROTOCOL.md`).
