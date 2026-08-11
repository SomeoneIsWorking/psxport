# The THREE-WAY render-path toggle — orientation + design

**Status: A PLAN. Nothing here is implemented.** Written 2026-08-11, orientation pass only; no
framework file was edited and no `coord/claims/` lock was taken. Companion doc:
`docs/plans/graphics-producer-db.md` — this toggle is the instrument that makes that DB's
"GTE/OT vs native producers" comparison observable per producer.

Citations are `file:line` in a psxport checkout at `d6b8e17d` (all three trees on that gitlink, clean).

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

## What is NOT free — the two real risks

1. **Mode 3's present is plausible but UNVERIFIED.** `present_window()` blits `s_vram` through
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
