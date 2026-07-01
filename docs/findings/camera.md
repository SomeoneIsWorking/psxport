# Findings — camera

## The resident engine_camera code is the CUTSCENE/SOP camera, NOT the free-roam field camera
- **symptom:** handoff/plan assumed `0x8006E3B0` (and the `game/camera/engine_camera.cpp` sub-ops/orchestrators `0x8006c80c`–`0x8006e464`, `e0f0`/`e228`/`e3f4`) is the LIVE per-frame free-roam camera ("recdep shows 0x8006E3B0 @ ~967/1000 frames"). Wiring plan targeted it as the free-roam camera.
- **status:** dead-end (premise falsified) / done-partial (SOP camera now `class CutsceneCamera`, wired+verified) / open (free-roam overlay camera not yet located)
- **cause:** measured with camtrace (temp instrument in rec_dispatch, range 0x0006c800–0x0006e480) + recdep on A00 free-roam WITH real player movement (`press right`, verified player X 0x800E7EAC advanced 0x0F64→0x1770): **ZERO** dispatches of any resident camera fn across 200 moving frames. The `~967/1000` figure was dominated by the SOP INTRO CUTSCENE (frames 0–216), where `game/scene/sop.cpp` natively calls `d2(c,0x8006e3b0, 0x800e8008/BGcam, 0x800e8040)` every frame. The A00 free-roam camera lives in the MODE-slot FIELD OVERLAY (base 0x00108F9C; the hot recdep cluster during movement is 0x8013xxxx: 0x80130AC4/0x80131134/0x801316CC/0x8012F494/0x80133550/0x80132A88/0x80132954/0x801337E4), not in resident MAIN.EXE.
- **fix:** DONE for the SOP camera: the resident camera was restructured into `class CutsceneCamera`
  (game/camera/cutscene_camera.{h,cpp}, later-290) and `CutsceneCamera::snapFollow` (0x8006e3b0 = snap XZ via 0x8006d934 +
  snap Y via 0x8006d950 + lookat 0x8006d02c) is wired native into game/scene/sop.cpp (`cam_snap_follow`),
  camverify 0-diff over 51+ live SOP calls. That only exercises trackXZ/trackY-snap + lookAt live.
  STILL OPEN — the FREE-ROAM camera: RE the MODE-slot field-overlay camera in the 0x8013xxxx cluster and own
  it top-down from the native SOP/field frame that rec_dispatches into it. It may reuse the resident lookat/
  track leaves — check before re-deriving (`tools/codemap.py --addr` now indexes the `class CutsceneCamera` methods).
- **refs:** scratch/handoff_pc_structure.md (falsified premise); game/scene/sop.cpp:202,263; game/camera/engine_camera.cpp; runtime/recomp/overlay_router.cpp slot_index (MODE slot = SOP/A0* field); 2026-07-01 session.
