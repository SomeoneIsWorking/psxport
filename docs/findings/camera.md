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
- **remaining work (top-down ownership, not a mystery hunt):** to make free-roam run the native class
  (instead of the equivalent substrate), own the camera DISPATCHER chain natively — `0x8006ec4c` (the
  per-frame camera driver that calls snapFollow at 0x8006eef0) and `0x8006ea7c` (the mode selector: 21-entry
  jump table on render-mode 0x800bf870 → picks snapFollow/mainFollow/…) — as `CutsceneCamera::update()`, and
  wire it from the native field frame (0x80022xxx caller). The leaves are already owned; this is contiguous
  top-down ownership of the caller, verifiable with the oracle unit test (add an `update`/dispatcher case).
- **refs:** generated/shard_3.c:16799 / shard_4.c:11310 (`func_8006E3B0(c)` direct); guest backtrace via
  PSXPORT_WWATCH=9F8000F8,.. + PSXPORT_WWATCH_BT; game/camera/cutscene_camera.cpp; 2026-07-01 (later-293).

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
