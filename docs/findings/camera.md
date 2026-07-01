# Findings — camera

## ✅ RESOLVED: the free-roam camera IS the resident camera (snapFollow→lookat), already owned + verified
- **symptom:** across later-290/291/292 I repeatedly concluded the resident MAIN.EXE camera "never runs in
  A00 free-roam" (camtrace/recdep showed zero camera dispatch with the player moving) and chased a phantom
  overlay / per-frame native camera. That conclusion was WRONG.
- **status:** resolved (2026-07-01, later-293). The free-roam field camera IS the resident camera.
- **root cause of the wrong conclusion — a MEASUREMENT ARTIFACT:** `recdep`/`camtrace` hook `rec_dispatch`,
  but the recompiler emits **intra-MAIN-module calls as DIRECT C calls** (`func_8006E3B0(c)`), NOT via
  `rec_dispatch` (which is only for cross-module/overlay/unknown targets). So `rec_dispatch` is STRUCTURALLY
  BLIND to every resident→resident call. The resident field code calls the resident camera directly, so it
  never appeared in camtrace/recdep. Proof: a GUEST-STACK backtrace (`PSXPORT_WWATCH` on the camera view
  matrix 0x1F8000F8 + `guest_backtrace_to`) at a moving-free-roam frame showed the chain
  `…0x80022AB8 → 0x8006EEF8 (field camera driver @0x8006ec4c) → 0x8006E3B0 (snapFollow) → 0x8006E3E0
  (post-lookat return) → MulMatrix0`. And `generated/shard_*.c` call `func_8006E3B0(c)` / `func_8006D02C(c)`
  directly (shard_3/4/5), confirming the bypass.
- **implication:** `CutsceneCamera::snapFollow` (0x8006E3B0) + `lookAt` (0x8006D02C) — already restructured,
  wired into sop.cpp, camverify 0-diff, and oracle-unit-tested 0-diff over 39k runs — ARE the free-roam
  field camera. It currently runs as the equivalent substrate `gen_func_8006E3B0` in free-roam (behaviourally
  identical, proven). There is NO separate overlay/native free-roam camera to find. The class name
  "CutsceneCamera" is now a misnomer — it is the general field/follow camera (SOP + free-roam). (Rename to
  `class Camera` deferred; low priority.)
- **refs:** generated/shard_3.c:16799 / shard_4.c:11310 (`func_8006E3B0(c)` direct); guest backtrace via
  PSXPORT_WWATCH=9F8000F8,.. + PSXPORT_WWATCH_BT; game/camera/cutscene_camera.cpp; 2026-07-01 (later-293).

## ✅ DONE: camera DISPATCHER owned natively — `CutsceneCamera::update()` + `::init()` (later-294)
- **what:** owned the per-frame camera DRIVER and the mode selector as methods, completing contiguous
  top-down ownership of the whole camera tree (the leaves/orchestrators were already owned):
  - `update()` = **0x8006EC44** (NOT 0x8006ec4c — the recompiled fn entry is 0x8006EC44; the handoff's
    0x8006ec4c is 8 bytes into it. gen_func_8006EC44 exists; gen_func_8006EC4C does not). ARG-LESS: it
    hardcodes the camera object at **0x800E8008** and reads its OUTER STATE from `cam[0]` (0=first-frame
    init, 1=run, else idle), runs the `cam[1]` sub-state machine, then dispatches on `cam[0x64]&0x3F`
    (18-entry jump table @0x80016A44) to a follow orchestrator / a still-substrate leaf / a field overlay,
    then always runs the post-mode tail 0x8006C988.
  - `init()` = **0x8006EA7C** — first-frame field reset + a render-mode-keyed (0x800BF870) 21-entry jump
    table @0x800169EC selecting the initial mode, then an optional mainFollow path + a scripted-follow
    post-check (timing byte 0x1F800236 ∈ {5,6}).
- **ownership model:** owned methods (mainFollow/rotBuild/trackFollow/snapFollow/simpleFollow) are called
  DIRECTLY; still-unowned resident leaves (0x8006E294/E360/E2FC/E918/CBA8/C988) and ALL field overlays
  (mode 0/1 render dispatch via table @0x800A4AA0, modes 9/10/17 = 0x8018B924/0x8010D89C/0x80111AB4) run via
  the substrate (`sub()` = rec_dispatch), exactly as trackFollow already does. Same 0-diff guarantee.
- **verified:** oracle unit test (PSXPORT_SELFTEST=camera) `init` + `update` cases, **0 mismatching words
  over ~10k verified iters** (skips = mainFollow-inherited oracle gaps + the overlay modes, which MISS since
  no overlay is loaded in the test — expected, same class as the yFloor gap). The test's `check()` now
  tolerates a MISS during the native run too (overlay modes) and skips those states.
- **NOT yet wired:** update() is reached indirectly (camera-object behaviour pointer → rec_dispatch). It is a
  verified-but-latent method; the substrate `gen_func_8006EC44` runs it 0-diff in the live game. Wire it when
  the object walk that dispatches the camera node is native (route that node → CutsceneCamera::update).
- **refs:** game/camera/cutscene_camera.cpp (update/dispatchMode/init), cutscene_camera_test.cpp (cases 13/14);
  tools/disas.py 0x8006EC44 / 0x8006EA7C; jump tables @0x80016A44 (18) + @0x800169EC (21); 2026-07-01.

## ✅ DONE: driver modes 2/3/4 owned — snap-follow variants (later-294b)
- **what:** owned the three scripted-camera SNAP follow orchestrators the driver dispatches for modes 2/3/4,
  plus the two trivial snap-accumulator leaves they share:
  - `snapAccXZ` = 0x8006D934 (w32 S+0x0C/S+0x14 from target); `snapAccY` = 0x8006D950 (w32 S+0x10). snapFollow
    refactored to use both (still 0-diff).
  - `snapFollowA` = 0x8006E294 (mode 2 + the init post-check): snapXZ+snapY, then look-build A, then lookAt.
  - `pitchFollow` = 0x8006E360 (mode 3): pitch(), snapXZ+snapY, lookAt. (Confirms pitch() is correct when
    called with a1=target — it hardcodes its G reads, so the arg is irrelevant.)
  - `snapFollowB` = 0x8006E2FC (mode 4): snapXZ+snapY, then look-build B, then lookAt.
- **verified:** oracle unit test snapFollowA/pitchFollow/snapFollowB = 0 mismatching words, 0 skips (no
  overlay involvement); the driver's modes 2/3/4 now call these native methods instead of the substrate.
- **refs:** game/camera/cutscene_camera.cpp; cutscene_camera_test.cpp cases 13/14/15; 2026-07-01.

## ✅ DONE: scripted LOOK-ANGLE builder unit owned — follow tree now fully native (later-294c)
- **what:** owned the four scripted look-angle builders that snapFollowA/B call (previously substrate),
  completing the resident camera FOLLOW tree — every orchestrator, sub-op, and builder is now native:
  - `posBuildA` = 0x8006DC38 (pair A): rcos/rsin place → OVERWRITE the X/Z look accumulators S+0/S+8, set
    cam[0x66]|=1. `posBuildB` = 0x8006DAD8 (pair B): same rcos/rsin place, then the yaw/dist ACCUMULATE tail
    (ratan2/isqrt, += cos·dist/2 into S+0, −= sin·dist/2 into S+8) — that tail is IDENTICAL to lookatTail's,
    so it was extracted into a shared `yawDistAccumulate(dx,dz)` (lookatTail now calls it too, still 0-diff).
  - `headBuildA` = 0x8006DF88, `headBuildB` = 0x8006DEF0 (heading steppers writing S+6/S+4, set cam[0x66]|=2;
    headBuildB has the same ±10 snap as heading()).
- **BIT-PRECISION gotchas the oracle caught/confirmed:** posBuildB's intermediate `P` is truncated to
  int16 (`sra 12; sll 16; sra 16`) whereas posBuildA keeps it 32-bit; posBuildB reads the S positions with
  lhu (unsigned) whereas posBuildA uses lh (signed). Replicated exactly.
- **verified:** oracle unit test posBuildA/posBuildB/headBuildA/headBuildB = **0 mismatching words over 8000
  iters each, 0 skips**. snapFollowA/B still 0-diff with the native builders wired in.
- **remaining substrate from the camera (small, non-follow):** init's 0x8006E918 / 0x8006CBA8 and the
  post-mode tail 0x8006C988 (runs after every mode), plus the true field OVERLAYS (modes 0/1 render dispatch,
  modes 9/10/17). Those are the next candidates if the camera is revisited.
- **refs:** game/camera/cutscene_camera.cpp (posBuild*/headBuild*/yawDistAccumulate); cutscene_camera_test.cpp
  cases 16-19; 2026-07-01.

## Superseded earlier conclusions (kept so the dead ends aren't re-walked)
- later-290 "resident camera is CUTSCENE-only, free-roam camera is in the 0x8013xxxx overlay" — WRONG
  (rec_dispatch-blindness artifact, see above). The 0x8013xxxx cluster is field render/objects.
- later-292 "A00 overlay camera ov_a00_gen_8010D89C" — that IS a real A00 SCRIPTED-camera state machine (5
  states; follow states call snapFollow 0x8006E3B0) but it is NOT the per-frame free-roam driver; the
  per-frame driver is the resident 0x8006ec4c reached by direct MAIN calls.

## Oracle recompiler gap: yFloor (0x8006C80C) render-mode 1 target 0x8006C844 not discovered
- **symptom:** the camera oracle unit test (PSXPORT_SELFTEST=camera) skips ~188/3000 yFloor iterations;
  `rec_interp(0x8006C80C)` with render-mode byte 0x800BF870==1 fail-fast MISSES on 0x8006C844.
- **status:** known-issue (oracle/recompiler function-discovery gap; native `CutsceneCamera::yFloor` handles it).
- **cause:** the yFloor jump table @0x80016874 maps render-mode 1 → label 0x8006C844, but the recompiled
  gen_func_8006C80C's jump-table `switch` lacks that case → `default: rec_dispatch(0x8006C844)` → miss.
- **fix:** native yFloor already handles mode 1; to close the oracle gap add 0x8006C844 to the recompiler's
  jump-table target discovery for 0x8006C80C.
- **refs:** game/camera/cutscene_camera_test.cpp (188 oracle-skipped); runtime/recomp/hle.cpp g_rec_miss_tolerant.
