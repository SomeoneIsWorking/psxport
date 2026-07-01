// engine/sop.cpp — PC-native ownership of the SOP field-gameplay-mode overlay (the per-area scene
// loop reached once the GAME stage enters gameplay). The SOP field-mode machine 0x80109450 (sm[0x50]
// LOAD->FADE->GAMEPLAY) and its per-frame handler are the gameplay-start engine systems; this file
// owns them top-down. Map: scratch/gameplay_start_flow_re.md + scratch/sop_mode_re.md.
//
// FIRST OWNED PIECE — the area-DATA load (LAB_80109164, SOP.BIN 0x80109164).
// In the PSX flow, SOP state-0 spawns this as a COOPERATIVE task in slot 1 (FUN_80044bd4 ->
// FUN_80051f14(1, LAB_80109164)) and then BLOCKS on the byte *0x1f80019b until the task sets it.
// LAB_80109164 itself does only SYNCHRONOUS work — 4 CD reads via FUN_8001dc40 (= ov_cd_dc40, the
// native synchronous sector read), an unpack (FUN_80044e84), a collision-grid load (FUN_80045258,
// itself sync), and an ecf58 reloc-patch loop — then sets *0x1f80019b = 1 and calls FUN_80051fb4
// (task-complete/yield). We reimplement the BODY natively (the leaves stay dispatched as they are
// already sync) and DROP the task-complete yield: the native scheduler marks the slot done. This
// removes the load's cross-frame cooperative dependency (the prerequisite for owning the SOP machine
// as a native per-frame dispatcher) WITHOUT changing the observable result — *0x1f80019b ends 1 and
// ecf58[..] is patched exactly as the recomp body leaves it. RE: scratch/sop_mode_re.md + the disasm
// of 0x80109164 (faithful below, addresses annotated).

#include "core.h"
#include "cfg.h"
#include <stdio.h>

// dispatch a still-recomp leaf with up to 3 args set (helpers for the SOP/transition machines).
static void d0(Core* c, uint32_t fn);
extern "C" void ffspan_begin(void), ffspan_end(const char*);   // PSXPORT_BDTAG attribution (engine_stage.cpp)
void ov_field_entity_render(Core*);   // engine_submit.cpp — native reimpl of SOP FUN_80109fe0
void ov_bg_scene_transition_sm(Core*);  // bg_scene_transition_sm.cpp — native FUN_8002655c
void engine_fade_set(Core*, uint32_t color, uint32_t a1);  // engine/gpu_lib.cpp — engine-owned screen fade
static void d1(Core* c, uint32_t fn, uint32_t a0);
static void d2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1);
extern "C" void cam_snap_follow(Core* c, uint32_t cam, uint32_t target);   // game/camera/camera.cpp
static void d3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);

// Owned synchronous area-DATA load (replaces the body of LAB_80109164 0x80109164). Runs in the
// slot-1 task register context; uses c->r[] for the dispatched leaves' args; writes guest RAM.
void native_sop_area_load(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  c->mem_w8(sm + 0x6e, 3);                              // sm[0x6e] = 3 (area sub-index; 0x80109198)

  // LOAD 1 — FUN_8001dc40(0x800ef478, *0x800be0f0 + sm[0x6e], 2048)  (0x801091ac)
  uint8_t  a6e = c->mem_r8(sm + 0x6e);                  // = 3 (re-read, as the disasm does)
  c->r[4] = 0x800ef478u;
  c->r[5] = c->mem_r32(0x800be0f0u) + a6e;
  c->r[6] = 2048;
  rec_dispatch(c, 0x8001dc40u);

  // LOAD 2 — FUN_8001dc40(0x8018a000, *0x800be0f8 + (*0x800ef478>>11), *0x800ef47c - *0x800ef478)
  uint32_t l2 = c->mem_r32(0x800ef478u);               // (0x801091bc)
  c->r[4] = 0x8018a000u;
  c->r[5] = c->mem_r32(0x800be0f8u) + (l2 >> 11);
  c->r[6] = c->mem_r32(0x800ef47cu) - l2;
  rec_dispatch(c, 0x8001dc40u);

  // UNPACK — FUN_80044e84(0x8018a000, 0x1f8000)  (0x801091e4)
  c->r[4] = 0x8018a000u;
  c->r[5] = 0x001f8000u;
  rec_dispatch(c, 0x80044e84u);

  // LOAD 4 — FUN_8001dc40(0x8018a000, *0x800be100 + (*0x800ef480>>11), *0x800ef484 - *0x800ef480);
  //          *0x800a3ec8 = *0x800ef480>>11  (0x80109210/0x80109214)
  uint32_t l4 = c->mem_r32(0x800ef480u);
  c->mem_w32(0x800a3ec8u, l4 >> 11);
  c->r[4] = 0x8018a000u;
  c->r[5] = c->mem_r32(0x800be100u) + (l4 >> 11);
  c->r[6] = c->mem_r32(0x800ef484u) - l4;
  rec_dispatch(c, 0x8001dc40u);

  // COLLISION GRID — FUN_80045258((area&0xf)<<1, 0x2f)  (0x80109228)
  uint16_t area = c->mem_r16(0x800bf89eu);
  c->r[4] = (uint32_t)((area & 0xf) << 1);
  c->r[5] = 0x2f;
  rec_dispatch(c, 0x80045258u);

  // RELOC PATCH — for i in 0..*0x800ef488: ecf58[w>>24] = 0x8018a000 + (w & 0xffffff), w=*0x800ef48c[i]
  int32_t count = (int32_t)c->mem_r32(0x800ef488u);    // (0x80109234; blez skip if <=0)
  for (int32_t i = 0; i < count; i++) {
    uint32_t w   = c->mem_r32(0x800ef48cu + (uint32_t)i * 4);
    uint32_t idx = w >> 24;
    uint32_t off = w & 0x00ffffffu;
    c->mem_w32(0x800ecf58u + idx * 4, 0x8018a000u + off);
  }

  // LOAD DONE — *0x1f80019b = 1  (0x80109290). NB: the recomp's FUN_80051fb4 task-complete/yield is
  // intentionally DROPPED — the native scheduler marks the slot done after this returns.
  c->mem_w8(0x1f80019bu, 1);
  if (cfg_dbg("stage"))
    fprintf(stderr, "[sop] native area-load done: 1f80019b=1, ecf58 patched %d entries (area&0xf=%u)\n",
            count, (unsigned)(area & 0xf));
}

// Synchronous TRANSITION area-DATA load — replaces the cooperative spawn-and-wait of
// FUN_80044bd4(0x800452c0, *0x800bf870, 0, 2) used by the field area machine's state-0 (GAME.BIN
// 0x80108918). In the PSX flow FUN_80044bd4 kills slot 2, clears 1f80019b, spawns 0x800452c0 in
// slot 1, and YIELD-waits on 1f80019b; the task body 0x800452c0 does the load and ends with
// FUN_80051fb4 (task-complete, longjmp-yield). We can NOT rec_dispatch 0x800452c0 wholesale because
// (a) its closing FUN_80051fb4 longjmps out (= frame done mid-state, sm[0x4c] never advances) and
// (b) its CD-settle / audio-busy waits would yield. So we transcribe the BODY natively (faithful to
// 0x800452c0), DROP the FUN_80051fb4 task-completes and the settle/busy waits (no-ops in our
// synchronous CD/audio runtime), and rec_dispatch the leaf callees (CD read, collision grid, unpack,
// BGM trigger — all synchronous). Ends by writing 1f80019b=1, exactly as the recomp body leaves it.
// Mirrors native_sop_area_load for the SOP intro load.
void native_transition_area_load(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  if (cfg_dbg("stage"))
    fprintf(stderr, "[sop] XLOAD enter: sm6d=%u sm6e=%u bf870=%u bf872=%u bf838=%u bf839=%u bf83a=0x%04X "
            "1f8001ff=%u bfe56=0x%04X 1f800278=0x%04X\n",
            c->mem_r8(sm+0x6d), c->mem_r8(sm+0x6e), c->mem_r8(0x800bf870u), c->mem_r8(0x800bf872u),
            c->mem_r8(0x800bf838u), c->mem_r8(0x800bf839u), c->mem_r16(0x800bf83au),
            c->mem_r8(0x1f8001ffu), c->mem_r16(0x800bfe56u), c->mem_r16(0x1f800278u));
  // --- early/quick path test (0x800452d8-0x8004531c): sm[0x6d]==0 AND *1f8001ff==sm[0x6e] AND
  //     (*0x800bfe56 & (1<<sm[0x6e])) == (*0x1f800278 & (1<<sm[0x6e])) ---
  uint8_t s6d = c->mem_r8(sm + 0x6d);
  if (s6d == 0) {
    uint8_t  s6e  = c->mem_r8(sm + 0x6e);
    uint32_t mask = 1u << s6e;
    uint32_t a2   = (uint32_t)c->mem_r16(0x800bfe56u) & mask;
    uint8_t  s1ff = c->mem_r8(0x1f8001ffu);
    uint32_t v0   = (uint32_t)c->mem_r16(0x1f800278u) & mask;
    if (s1ff == s6e && a2 == v0) {
      // QUICK PATH (0x80045320-0x80045344): collision grid + done, no DMA load.
      d2(c, 0x80045258u, (uint32_t)((c->mem_r16(0x800bf89eu) & 0xf) << 1), 47);
      c->mem_w8(0x1f800206u, 0);
      c->mem_w8(0x1f80019bu, 1);
      if (cfg_dbg("stage")) fprintf(stderr, "[sop] native transition area-load (quick path) done\n");
      return;
    }
  }
  // --- MAIN LOAD PATH (0x80045350+) ---
  d0(c, 0x8001cf2cu);                                   // kill slot-2 task / settle CD (sync; settle-wait dropped)
  sm = c->mem_r32(0x1f800138u);
  c->mem_w8(sm + 0x6d, 2);                              // sm[0x6d] = 2
  uint8_t s6e = c->mem_r8(sm + 0x6e);
  c->mem_w16(0x1f800278u, c->mem_r16(0x800bfe56u));     // *1f800278 = *0x800bfe56
  c->mem_w8(0x1f8001ffu, s6e);                          // *1f8001ff = sm[0x6e]
  c->mem_w8(0x800bf872u, s6e);                          // *0x800bf872 = sm[0x6e]
  // *0x800bf870 = sm[0x6e]  (stored in the jal's DELAY SLOT = the OLD v0, i.e. sm[0x6e], NOT the
  // FUN_80045080 return); then FUN_80045080(0x80108f9c, (sm[0x6e]+3)&0xff) loads the next-area file
  // (its return is discarded).
  c->mem_w8(0x800bf870u, s6e);
  d2(c, 0x80045080u, 0x80108f9cu, (uint32_t)((s6e + 3) & 0xff));
  // FUN_8007566c(*0x800bf870, *0x1f80022c)   — area BGM/asset trigger
  d2(c, 0x8007566cu, c->mem_r8(0x800bf870u), c->mem_r32(0x1f80022cu));
  d0(c, 0x80044f58u);                                  // ov_load_texgroup (sync)
  // FUN_8001dc40(0x8018a000, *0x800be100 + (*0x800ef480>>11), *0x800ef484 - *0x800ef480);
  //   *0x800a3ec8 = *0x800ef480>>11    (the area-asset overlay DMA load)
  uint32_t l = c->mem_r32(0x800ef480u);
  c->mem_w32(0x800a3ec8u, l >> 11);
  d3(c, 0x8001dc40u, 0x8018a000u, c->mem_r32(0x800be100u) + (l >> 11), c->mem_r32(0x800ef484u) - l);
  // if (*0x800bf89c == 2) FUN_80045558(0)
  if (c->mem_r8(0x800bf89cu) == 2) d1(c, 0x80045558u, 0);
  // FUN_80045258((*0x800bf89e & 0xf)<<1, 47)   — collision grid
  d2(c, 0x80045258u, (uint32_t)((c->mem_r16(0x800bf89eu) & 0xf) << 1), 47);
  // RELOC PATCH (0x80045468-0x800454b0): for i in 0..*0x800ef488:
  //   w=*(0x800ef48c + i*4); ecf58[w>>24] = 0x8018a000 + (w & 0xffffff)
  int32_t count = (int32_t)c->mem_r32(0x800ef488u);
  for (int32_t i = 0; i < count; i++) {
    uint32_t w   = c->mem_r32(0x800ef48cu + (uint32_t)i * 4);
    c->mem_w32(0x800ecf58u + (w >> 24) * 4, 0x8018a000u + (w & 0x00ffffffu));
  }
  // tail (0x800454b4-0x80045538): if *0x800bf870 == 8: bonus-area asset bump (0x800ecf94 += 0x1000;
  //   FUN_80045258(idx, 8) by *0x800bf871 bracket); then *0x1f800206 = 1; *1f80019b = 1.
  if (c->mem_r8(0x800bf870u) == 8) {
    c->mem_w32(0x800ecf94u, c->mem_r32(0x800ecf94u) + 0x1000);
    uint8_t b = c->mem_r8(0x800bf871u);
    uint32_t idx = (b < 9) ? 34 : (b < 16) ? 38 : (b < 21) ? 40 : 36;
    d2(c, 0x80045258u, idx, 8);
    c->mem_w8(0x800bfe60u, (uint8_t)idx);
  }
  c->mem_w8(0x1f800206u, 1);
  c->mem_w8(0x1f80019bu, 1);
  if (cfg_dbg("stage"))
    fprintf(stderr, "[sop] native transition area-load (main path) done: 1f80019b=1, ecf58 patched %d, "
            "bf870=%u\n", count, (unsigned)c->mem_r8(0x800bf870u));
}

static void d0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static void d1(Core* c, uint32_t fn, uint32_t a0) { c->r[4]=a0; rec_dispatch(c, fn); }
static void d2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) { c->r[4]=a0; c->r[5]=a1; rec_dispatch(c, fn); }
static void d3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4]=a0; c->r[5]=a1; c->r[6]=a2; rec_dispatch(c, fn);
}

// SOP per-frame FIELD UPDATE — native ownership of FUN_801092b4 (decomp scratch/decomp/sop/801092b4.c).
// The running-gameplay frame: a startup-delay countdown (sm[0x60]), then BG scene SM + entity update +
// Tomba update + BG layer SM + entity render + GPU submit, then the sm[0x52] intro/end-scroller tail.
// Control flow + every field write owned native; the heavy callees stay rec_dispatched (engine
// subsystems to own next: entity update 0x8010a0e0 / render 0x80109fe0, Tomba update 0x8007b008; and
// the per-scene content). Called from ov_sop_field_mode states 1/2/3.
void ov_sop_field_update(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  int16_t delay = (int16_t)c->mem_r16(sm + 0x60);
  if (delay != 0) {
    c->mem_w16(sm + 0x60, (uint16_t)(delay - 1));          // startup delay: just count down
  } else {
    ffspan_begin(); ov_bg_scene_transition_sm(c); ffspan_end("bgscene");   // BG scene transition SM (native, FUN_8002655c)
    ffspan_begin(); d1(c, 0x8010a0e0u, 0x800f2418u); ffspan_end("entupd");    // entity update loop
    d0(c, 0x8007b008u);                                    // Tomba update
    c->mem_w8(0x1f800234u, 1);
    uint8_t bg = c->mem_r8(0x800e8008u);                   // BG layer SM
    if (bg == 0) { c->mem_w8(0x800e8008u, 1); c->mem_w8(0x800e806cu, 0); }
    else if (bg == 1) {
      uint8_t sub = c->mem_r8(0x800e806cu);
      if (sub == 0) cam_snap_follow(c, 0x800e8008u, 0x800e8040u);   // native class CutsceneCamera (was 0x8006e3b0)
      else if (sub == 1) c->mem_w8(0x800e806cu, 0);
    }
    d0(c, 0x80075a80u);                                    // per-frame area update
    if (c->mem_r8(0x800bf9b4u) != 5) { ffspan_begin(); d1(c, 0x8010bffcu, 0x800ed018u); ffspan_end("parallaxBG"); }   // parallax BG draw
    // SOP-mode entity render. The native world-coord version (ov_field_entity_render, engine_submit.cpp)
    // is ready but UNWIRED — this SOP path isn't exercised by the walkable field (which renders via
    // 0x8010810c -> 0x8003D074 -> 0x8003F698), so wiring it native is unverified. Own the live chain first.
    // OWN the SOP entity render FUN_80109fe0 NATIVE (later-238): its loop over 0x800f2418 + its GT3/GT4
    // submitters (FUN_801099b4 stride36 / FUN_80109c80 stride44, both RTPT/RTPS-projecting) are byte-identical
    // to ov_field_entity_render's loop + submit_poly_gt3/gt4_native. Routing it native projects every record
    // (grass scene AND the sky/sea backdrop, all GTE world geometry) through float eproj with REAL per-vertex
    // depth — so the backdrop sorts behind the world instead of compositing on top (is3d=0). This is the LIVE
    // field render path (NOT ov_render_frame). later-238 pinned this as the "sea on top" builder.
    c->r[4] = 0x800f2418u; ov_field_entity_render(c);          // was d1(c, 0x80109fe0u, 0x800f2418u) (PSX)
    void ov_render_walk(Core*);                            // engine_submit.cpp — NATIVE 0x8003c048 walk
    ffspan_begin(); ov_render_walk(c); ffspan_end("renderwalk");   // object render-list walk (PC-native depth + fps60)
    if (c->mem_r8(0x800bf9b4u) != 5) { ffspan_begin(); d1(c, 0x8010c26cu, 0x800ed018u); ffspan_end("bgscroll"); }   // BG tile scroller
    c->mem_w8(0x1f800234u, 0);
  }
  // tail — sm[0x52]: 0 = intro zone setup, 1 = end-of-area text scroller, 2+ = done
  sm = c->mem_r32(0x1f800138u);
  uint16_t s52 = c->mem_r16(sm + 0x52);
  if (s52 == 1) {
    d0(c, 0x8010c79cu);                                    // end-of-area scroller
    if (c->r[2] == 0) return;                              // still running -> stay
  } else {
    if (s52 != 0) return;                                  // s52 >= 2: done
    c->mem_w16(sm + 0x62, 0);
    d1(c, 0x8001d71cu, 0xe);                               // zone/area transition setup
  }
  c->mem_w16(sm + 0x52, (uint16_t)(c->mem_r16(sm + 0x52) + 1));
}

// SOP FIELD-MODE MACHINE — native ownership of FUN_80109450 (decomp scratch/decomp/sop/80109450.c).
// Owns the sm[0x50] switch + every field write; dispatches the heavy per-state callees (those not yet
// owned native). CRITICAL: state 0 does NOT call FUN_80044bd4 (which clears 1f80019b, spawns the slot-1
// load task, and yields-waits — fatal to re-enter per-frame). It calls native_sop_area_load INLINE.
// Called from the native bridge ov_game_submode0 (per frame) once the GAME loop is native per-frame.
void ov_sop_field_mode(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x50);
  switch (st) {
    case 0: {  // LOAD
      engine_fade_set(c, 0xffffffu, 0);       // LOAD: a1=0 = subtractive 0xffffff = full screen to BLACK
      native_sop_area_load(c);                 // INLINE sync load (replaces FUN_80044bd4) -> 1f80019b=1
      d0(c, 0x8007b18cu);
      d0(c, 0x800796dcu);
      d0(c, 0x80078610u);
      d1(c, 0x8010a8d4u, 0x800f2418u);         // SOP bg-ptr setup
      // 3 scene objects: spawn + stamp fields from the SOP overlay tables @0x8010c98c (stride 12).
      for (int i = 0; i < 3; i++) {
        d3(c, 0x8007a980u, 3, 3, 1);           // spawn -> node in v0
        uint32_t node = c->r[2];
        uint32_t t = 0x8010c98cu + (uint32_t)i * 12;
        c->mem_w16(node + 0x2e, c->mem_r16(t + 0));
        c->mem_w16(node + 0x32, c->mem_r16(t + 2));
        c->mem_w16(node + 0x36, c->mem_r16(t + 4));
        c->mem_w32(node + 0x1c, c->mem_r32(t + 8));   // per-scene handler (content)
      }
      d2(c, 0x8006cbd0u, 0x800e8008u, 0x8010c95cu);   // BG xform setup
      cam_snap_follow(c, 0x800e8008u, 0x800e8040u);   // BG init (native class CutsceneCamera; was 0x8006e3b0)
      sm = c->mem_r32(0x1f800138u);                   // (callees don't move sm, but reload defensively)
      c->mem_w16(sm + 0x50, 1);
      d0(c, 0x80075240u);
      c->mem_w16(sm + 0x60, 0x1e);
      c->mem_w16(sm + 0x52, 0);
      c->mem_w16(sm + 0x54, 0);
      c->mem_w8 (sm + 0x6c, 0x1f);
      c->mem_w8 (0x1f800137u, 1);
      break;
    }
    case 1: {  // FADE-IN
      // BUG-1 (double fade-in) ROOT CAUSE + FIX. This state's fade ramps sm[0x6c] 0x1f->0 over 31 frames,
      // but the area's 30-frame STARTUP DELAY (sm[0x60], counted down inside ov_sop_field_update) is still
      // running for the first 30 of them. During that delay ov_sop_field_update does NOT run the per-frame
      // scene content / the bg-scene-transition machine — yet the end-of-area TEXT scroller (tail) AND our
      // native ov_scene_native still draw, so this fade reveals a half-built frame ("only the text fades
      // in"). Then once the delay ends, ov_bg_scene_transition_sm runs its OWN state-0 full-black + state-1
      // fade-in (the real "scene appears" fade, run AFTER the scene is built) — so the screen visibly fades
      // twice. On PSX the scene render is suppressed during the delay, so this fade ramps over BLACK and is
      // invisible; the single visible fade-in is the bg-transition's. Match that: HOLD BLACK while the
      // startup delay is active and let ov_bg_scene_transition_sm own the one fade-in. The sm[0x6c] ramp
      // still counts down so state 1 ends exactly as the delay ends (then bg-transition has taken over).
      // Shared with every SOP area (free-roam too) — correct there as well (same delay-then-bg-fade entry).
      uint32_t u = (uint32_t)c->mem_r8(sm + 0x6c) & 0x1f;
      bool startup_delay = (int16_t)c->mem_r16(sm + 0x60) != 0;
      engine_fade_set(c, startup_delay ? 0xffffffu : ((u << 19) | (u << 11) | (u << 3)), 0);   // hold black during delay; else subtractive fade-in
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6c) - 1);
      c->mem_w8(sm + 0x6c, v);
      if (v == 0) { c->mem_w8(sm + 0x6c, 0x1f); c->mem_w16(sm + 0x50, (uint16_t)(c->mem_r16(sm + 0x50) + 1)); }
      ov_sop_field_update(c);                  // native per-frame field update
      break;
    }
    case 2: {  // GAMEPLAY
      ov_sop_field_update(c);
      if (c->mem_r8(0x800bf839u) != 0 || (c->mem_r32(0x800e7e68u) & 8) != 0)
        c->mem_w16(sm + 0x50, (uint16_t)(c->mem_r16(sm + 0x50) + 1));
      break;
    }
    case 3: {  // FADE-OUT
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6c) * (uint32_t)-8) & 0xff;
      engine_fade_set(c, (u << 16) | (u << 8) | u, 0);   // FADE-OUT: a1=0 = subtractive (to black)
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6c) - 1);
      c->mem_w8(sm + 0x6c, v);
      if (v == 0) c->mem_w16(sm + 0x50, (uint16_t)(c->mem_r16(sm + 0x50) + 1));
      ov_sop_field_update(c);
      break;
    }
    case 4: {  // RESET -> next area
      d0(c, 0x8001cf2cu);                             // kill load task slot 2 (settle CD)
      c->mem_w8(0x1f800137u, 0);
      int16_t s4c = (int16_t)c->mem_r16(sm + 0x4c);
      c->mem_w16(sm + 0x4e, 0);
      c->mem_w16(sm + 0x50, 0);
      c->mem_w16(sm + 0x52, 0);
      c->mem_w16(sm + 0x54, 0);
      c->mem_w16(sm + 0x4c, (uint16_t)(s4c + 1));
      c->mem_w8(0x800bf9b4u, 0);
      break;
    }
    default: return;
  }
}
