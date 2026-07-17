// class Sop — the PC-native SOP (intro-cutscene) FIELD stage machine.
//
// PROPER OOP: one instance per Core, embedded as `Core::engine::sop`. Back-pointer wired once by
// Core's constructor. Callers reach the SOP stage through the object graph:
//
//     eng(c).sop.fieldMode();     // per-frame outer state dispatcher (was ov_sop_field_mode)
//     eng(c).sop.fieldUpdate();   // per-frame gameplay body        (was ov_sop_field_update)
//
// No `extern "C"` shim, no free function, no static, no Core-as-first-arg. Same shape as Demo,
// ObjectList, SceneTransition, TransitionState3.
//
// Owns the SOP overlay's FIELD-MODE state machine (guest FUN_80109450) and the per-frame FIELD
// update body (guest FUN_801092B4). Implementations + full doc-comments live in sop.cpp.
#pragma once
#include <cstdint>
class Core;

class Sop {
public:
  Core* core = nullptr;

  // Live-spine entry points.
  void fieldMode();     // was ov_sop_field_mode  (per-frame outer state dispatcher)
  // pc_faithful mirror of ov_sop_gen_80109450: guest frame -32 with live ra/s0-s2 spills,
  // per-state substrate leaf dispatch at the RE'd jal sites (fade engine 0x8007E9C8, pool
  // leaves, spawn 0x8007A980, overlay ticks 0x8010A8D4/0x801092B4), the area load through
  // rec_dispatch(0x80044BD4) -> the spawn-and-wait primitive. The rebuilt fieldMode() above
  // (defer-steps + native leaf calls) is the pc_skip flavor.
  void fieldModeFaithful();
  // pc_faithful mirror of ov_sop_gen_80109164 — the SOP area-load TASK-1 body, run on a
  // PcScheduler native fiber (spawned by fieldModeFaithful's 0x80044BD4 dispatch). Substrate
  // leaves at their RE'd jal sites; ends with done_flag + selfClose. areaLoad() above is the
  // pc_skip sync flavor.
  void areaLoadFaithful();
  void fieldUpdate();   // was ov_sop_field_update (per-frame gameplay body — states 1/2/3)

  // Sync area-DATA load entry points (both mirror each other; both end with 1f80019b=1).
  // areaLoad         — owned synchronous SOP area-DATA load (replaces LAB_80109164 body).
  //                    Was the free function `native_sop_area_load(Core*)`.
  // transitionAreaLoad — synchronous FIELD transition area-DATA load (replaces the cooperative
  //                    FUN_80044bd4(0x800452c0, ...) spawn-and-yield used by the field area
  //                    machine's state-0). Was `native_transition_area_load(Core*)`.
  void areaLoad();
  void transitionAreaLoad();

  // == scenePrepass (guest FUN_8010A0E0) ==
  //   The immediately-next callee below fieldUpdate in the top-down chain (called RIGHT before the
  //   list-2 walk each field frame). Computes a per-frame 2D CAMERA-FRUSTUM TRIANGLE in scene-grid
  //   space (cam pos ± halfFOV yaw offsets, view distance 0x5780=22400, half-FOV 0x1C7=455/4096,
  //   pitch-tilted, scaled by 1/0x280=640) and hands it to the native scanline gatherer (guest
  //   FUN_8010A3AC — also owned; Sop::sceneGridGather below) which
  //   scanline-rasters it into SCENE_STATE.count / SCENE_STATE.list at table+6/+0x10.
  //   `table` = 0x800F2418 (SCENE_STATE). Header copy at +8/+10 comes from *(u16*)(table+0xC).
  //   Also writes two engine globals 0x800A3F90=0x5780 (view dist) and 0x800A3F94=0x1C7 (halfFOV).
  //   Faithful to the recomp: yaw sign/masking (12-bit wrap), signed >>12 fixed-point, signed div
  //   by 0x280.
  void scenePrepass(uint32_t table);

private:
  // sceneGridGather (guest FUN_8010A3AC): scanline-raster the frustum triangle into the scene grid,
  // appending cell ids to table+0x10 / count at table+6. Called only by scenePrepass.
  void sceneGridGather(uint32_t table, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       int32_t x2, int32_t y2);
};
