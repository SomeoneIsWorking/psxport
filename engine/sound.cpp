// engine/sound.cpp — Tomba!2 PC-native SOUND FRONT-END (the SFX / BGM trigger API the game logic
// calls). This owns the game's sound-TRIGGER layer — the functions that decide WHICH song/effect
// plays, at what volume/pan, and that mutate the engine's sound STATE (current-song byte, the
// sound-command request byte, the per-voice/seq slot tables, the KON mask). The actual libsnd
// leaves (SsSeqOpen/Play/SetVol/Stop, the per-voice SsUtKeyOn submit, the reverb/bank set) and the
// SPU are NOT here — they stay the recompiled libsnd leaf, invoked in EXACT order via rec_dispatch
// so the SPU result is byte-identical. This is the ENGINE/PLATFORM-glue side of the boundary:
// structured the way a PC game's audio module is (sound_play_sfx / sound_play_bgm / sound_stop_bgm),
// not as a PSX simulation.
//
// RE (tools/disas.py; full linear dumps in docs/journal.md later-207):
//   FUN_80074590  = the SFX / song-id ROUTER (`sound_play_sfx`):
//                   id >= 0x70 -> a fixed id->song map -> sound_play_bgm(song). id < 0x70 -> the real
//                   per-effect SFX path: look up the effect descriptor (tables 0x800a4d18 / 0x800a4ef8),
//                   compute pan/volume, jal 0x80075e04 (the SFX submit leaf, kept dispatched).
//   FUN_80074BF8  = BGM START (`sound_play_bgm`): classify the requested song vs the current song /
//                   request state, write the current-song halfword 0x800bed80 + the sound-cmd request
//                   byte 0x800be22a, stop the old BGM, then drive the libsnd sequencer leaves
//                   (SsSeqOpen/SetVol/Play) per the seq-slot table 0x800be368 (stride 8) and reset the
//                   per-voice table 0x800be238 (stride 12) / voice-count word 0x800bed78.
//   FUN_80074E48  = BGM STOP (`sound_stop_bgm`): if a song is playing, stop the seq (leaf 0x80091af0),
//                   reset the per-voice table, zero the voice-count word, set current-song = 0xFFFF.
//
// VERIFY (`soundverify`, a full RAM+scratchpad A/B gate, same scheme as scriptvm/gridresolve): run the
// native body, snapshot+rollback, run rec_super_call, diff the whole 2MB RAM + 0x400 scratchpad + v0.
// The dispatched libsnd leaves run in BOTH passes; the only legitimate divergence is transient stack
// residue below the entry sp (the dispatched leaves + this fn's own dead frame), so the gate excludes
// the top-of-RAM stack window [sp-0x800, sp) — far above all game/sound data. 0 mismatches over live
// calls (BGM starts at scene load; SFX fires on menu cursor / actions) flips each to its native body.
#include "core.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)
void rec_dispatch(Core*, uint32_t);     // hybrid call: recomp body if emitted, else interpret
void rec_set_override(uint32_t, void(*)(Core*));
void xa_music_cut_if_dialog(Core*);     // cd_override.cpp: cut looping ingame music when a dialog tone starts

// ---- engine sound STATE (the guest fields this module owns) ---------------------------------------
#define SND_SONG        0x800BED80u   // current-song index (s16; 0xFFFF = none)
#define SND_REQ         0x800BE22Au   // sound-command request byte (FUN_80075A80 services this)
#define SND_VOICE_CNT   0x800BED78u   // active voice count (word)
#define SND_SEQTAB      0x800BE368u   // seq-slot table base (stride 8): +0 seq handle(s16), +4 bank(u8), +6 vcount(s16)
#define SND_VOICETAB    0x800BE238u   // per-voice slot table base (stride 12)
#define SND_MODE        0x800BF870u   // sound mode/scene byte
#define SND_VOL_A       0x800FB164u   // master volume source byte A (0..9)
#define SND_VOL_B       0x800FB165u   // master volume source byte B (0..9)

// libsnd leaves (kept dispatched, exact order/args — these touch SPU state):
#define LF_BANKSET      0x800963A0u   // SsUt* reverb/bank set (a0 = bank byte)
#define LF_SEQ_SETVOL   0x80091F50u   // seq volume set (a0 = seq handle, a2 = vol)
#define LF_SEQ_PLAY     0x80090560u   // seq play (a0 = seq handle, a1 = play-mode, a2 = loop/once)
#define LF_SEQ_STOP     0x80091AF0u   // seq stop (a0 = seq handle)

#define A_PLAY_BGM      0x80074BF8u
#define A_STOP_BGM      0x80074E48u
#define A_PLAY_SFX      0x80074590u

// ===================================================================================================
// sound_stop_bgm — FUN_80074E48.  RE'd (full disasm in journal later-207):
//   v1 = song@0x800bed80; if v1 == -1 -> nothing playing, return.
//   else: seq = seqtab[v1].handle (0x800be368 + v1*8, +0 s16); jal SsSeqStop(seq).
//         v0 = voice_cnt@0x800bed78; if v0 > 0: for a0 in [0,v0): voicetab[a0].byte0 = 0;
//             voicetab[a0].byte1 &= 0xC0  (voicetab base 0x800be238, stride 12). (re-read cnt each iter)
//         voice_cnt = 0; jal SsUt*(a0 = -1) [0x800963a0]; song = 0xFFFF.
//
// BOUNDARY NOTE (later-207): the per-voice/seq orchestration of BOTH stop AND start is libsnd
// SEQUENCER work whose voicetab (0x800be238) state is CO-EVOLVED with the SsSeqPlay/SsSeqStop LEAVES
// (the leaf decides per-voice keep/kill from its own internal voice state, which the gen sets up via
// the EXACT inline register/call context). A native re-drive that dispatches those leaves does NOT
// reproduce their voicetab side-effects bit-for-bit (verified: vcount-14 songs leave the voicetab
// half-written through dispatched SsSeqPlay — the leaf's voice bookkeeping diverges). That is exactly
// the "leaf, keep dispatched" case in the boundary: start/stop are SEQUENCER glue, so we own the
// ENGINE-FACING contract (the dialog-cut hook, the clean API) and keep the gen body as the live
// orchestration via rec_super_call. The OWNED-native trigger surface is the SFX / song-id ROUTER
// (FUN_80074590) below, which is pure control flow (0-diff verified). stop_bgm_body is retained as
// the documented native reference but is NOT registered (see sound_register).
static void stop_bgm_body(Core* c) {
  int32_t song = (int16_t)c->mem_r16(SND_SONG);
  if (song == -1) return;
  uint32_t seq = c->mem_r16(SND_SEQTAB + (uint32_t)song * 8);  // lh a0,0(v1) — handle
  c->r[4] = seq; rec_dispatch(c, LF_SEQ_STOP);                  // SsSeqStop(seq)
  int32_t cnt = (int32_t)c->mem_r32(SND_VOICE_CNT);
  if (cnt > 0) {
    for (int32_t a0 = 0; ; ) {
      uint32_t e = SND_VOICETAB + (uint32_t)a0 * 12;
      c->mem_w8(e + 0, 0);
      c->mem_w8(e + 1, (uint8_t)(c->mem_r8(e + 1) & 0xC0));
      a0 += 1;
      if (!(a0 < (int32_t)c->mem_r32(SND_VOICE_CNT))) break;    // slt a0, cnt (re-read)
    }
  }
  c->mem_w32(SND_VOICE_CNT, 0);
  c->r[4] = 0xFFFFFFFFu; rec_dispatch(c, LF_BANKSET);           // SsUt*(-1)
  c->mem_w16(SND_SONG, 0xFFFF);
}

// ===================================================================================================
// sound_play_bgm — FUN_80074BF8(idx). idx low7 = song s0; idx bit7 -> s2 = (bit7==0).
//   if s0 >= 14 -> no-op (return).  Else classify (jump table on s0, 0..12):
//     class 0  (s0 in {0,1,11,12}): if cur-song in {2,3} -> no-op (don't interrupt those).
//     class 1  (s0 in {2,3}):       if cur-song in {4,5,6,7} OR cur-song in {10,13}: set REQ |= 0x80
//                                    (defer: a dialog tone is sounding) ; then fall to START.
//                                    else START.
//     class 2  (s0 == 13):          if cur-song in {1,2,3}: REQ = idx, return (no-op start).
//                                    else: REQ = idx, fall to START.
//   START (common tail 0x80074cd8):
//     stop_bgm_body();  song = s0;
//     bank = seqtab[s0].byte4 ; jal SsUt*(bank) [0x800963a0]
//     vol  = master_vol_for(s0)  (a 0..9 byte * 127 / 9, signed; scene-21 override -> byte A) ; s16
//     seqh = seqtab[s0].handle(s16)
//     jal SsSeqSetVol(seqh, vol) [0x80091f50]
//     jal SsSeqPlay(seqh, 1, s2) [0x80090560]
//     vcount = seqtab[s0].vcount(s16); if vcount > 0: voice_cnt = vcount;
//        for a0 in [0,vcount): voicetab[a0].word0 = -1 (0x800be238 stride 12).
// ===================================================================================================
static inline int32_t master_vol_div9(Core* c, uint32_t volbyte_addr) {
  // v1 = lbu(volbyte_addr); v0 = v1*127; a1 = (v1*127) signed-/9 ; matches the 0x38e38e39 magic-mult.
  int32_t v1 = c->mem_r8(volbyte_addr);
  int32_t prod = v1 * 127;                 // (v1<<7)-v1, fits in 32 bits
  return prod / 9;                          // MIPS magic-div-by-9 == signed trunc-toward-zero /9 here (prod>=0)
}
// Volume source selector (the s0 / scene-21 logic), returns the chosen byte address.
static inline uint32_t vol_src_addr(Core* /*c*/, int32_t s0) {
  // s0<0 -> A; s0<4 -> B; s0>=13 -> A; 4<=s0<11 -> A; 11<=s0<13 -> B.
  if (s0 < 0)        return SND_VOL_A;
  else if (s0 < 4)   return SND_VOL_B;
  else if (s0 >= 13) return SND_VOL_A;
  else if (s0 < 11)  return SND_VOL_A;
  else               return SND_VOL_B;     // 11..12
}

static void play_bgm_body(Core* c) {
  uint32_t idx = c->r[4];
  int32_t s0 = (int32_t)(idx & 0x7Fu);
  uint32_t s2 = (idx & 0x80u) ? 0u : 1u;   // sltiu (a0&0x80) < 1
  if (s0 >= 14) return;                      // sltiu s0,14 == 0 -> tail/return

  int32_t cur = (int16_t)c->mem_r16(SND_SONG);
  bool do_start = true;

  // ---- classification (jump table on s0) ----
  switch (s0) {
    case 0: case 1: case 11: case 12: {
      // class 0: if (cur-2) <2  i.e. cur in {2,3} -> no-op.
      if ((uint32_t)(cur - 2) < 2u) do_start = false;
      break;
    }
    case 2: case 3: {
      // class 1: if cur in {4,5,6,7} -> 0x80074ca0 (REQ |= 0x80) ; if cur==10 || cur==13 -> same.
      bool defer = ((uint32_t)(cur - 4) < 4u) || (cur == 10) || (cur == 13);
      if (defer) {
        uint8_t r = (uint8_t)(c->mem_r8(SND_SONG) | 0x80u);  // lbu 0x800bed80 | 0x80
        c->mem_w8(SND_REQ, r);
      }
      // either way: fall through to START.
      break;
    }
    default: {  // s0 == 13 (the only remaining value < 14 routed here -> 0x80074cb4)
      // class 2 (0x80074cb4): if (cur-1) <3 i.e. cur in {1,2,3} -> REQ=idx, no-op start; else REQ=idx, start.
      if ((uint32_t)(cur - 1) < 3u) { c->mem_w8(SND_REQ, (uint8_t)idx); return; }
      c->mem_w8(SND_REQ, (uint8_t)idx);
      break;
    }
  }
  if (!do_start) return;

  // ---- START (common tail 0x80074cd8) ----
  stop_bgm_body(c);                                  // jal FUN_80074e48 (now native)
  c->mem_w16(SND_SONG, (uint16_t)s0);                // sh s0, 0x800bed80
  uint32_t slot = SND_SEQTAB + (uint32_t)s0 * 8;
  c->r[4] = c->mem_r8(slot + 4); rec_dispatch(c, LF_BANKSET);    // SsUt*(bank byte = low byte of +4)

  // vol = master_vol_div9(selected source); scene-21 override -> byte A.
  int32_t a1 = master_vol_div9(c, vol_src_addr(c, s0));
  if (c->mem_r8(SND_MODE) == 21) a1 = master_vol_div9(c, SND_VOL_A);
  a1 = (int16_t)a1;                                   // sll16/sra16 -> s16

  uint32_t seqh = c->mem_r16(SND_SEQTAB + (uint32_t)s0 * 8);     // lh a0,0(seqtab[s0])
  c->r[4] = seqh; c->r[6] = (uint32_t)a1; rec_dispatch(c, LF_SEQ_SETVOL);   // SsSeqSetVol(seq, vol)

  seqh = c->mem_r16(SND_SEQTAB + (uint32_t)s0 * 8);
  c->r[4] = seqh; c->r[5] = 1; c->r[6] = s2; rec_dispatch(c, LF_SEQ_PLAY);  // SsSeqPlay(seq, 1, s2)

  int32_t vcount = (int16_t)c->mem_r16(SND_SEQTAB + (uint32_t)s0 * 8 + 4);  // lh v1,4(seqtab[s0]) — vcount at +4 (low byte = bank)
  if (vcount > 0) {
    c->mem_w32(SND_VOICE_CNT, (uint32_t)vcount);                // sw v1, 0x800bed78
    uint32_t e = SND_VOICETAB;
    for (int32_t a0 = 0; a0 < vcount; a0++) { c->mem_w32(e, 0xFFFFFFFFu); e += 12; }   // word0 of each 12-byte voice slot = -1
  }
}

// ===================================================================================================
// sound_play_sfx — FUN_80074590(id, a1, a2).  The SFX / song-id router.
//   if (id<<24) < 0 (id bit7 set as a signed byte) -> low-id SFX path (0x800746c8):
//       if (id & 0xff) >= 225 -> return 0.  else the descriptor-table SFX path.
//   id = id & 0xff:
//     if id >= 0x70: id2 = id - 0x70; if id2 >= 16 -> return 0 (0x800746d8).
//        else jump-table[id2] (0x80016c04): each entry is `sound_play_bgm(<fixed song>)` then return 0.
//        Entries: 0->bgm2 1->bgm3 2->bgm4 3->bgm5 4->bgm6 5->bgm7 6->bgm10 7->bgm11 8->bgm12 9->bgm13
//                 10->bgm10 11->bgm11 12->bgm12 13,14->return0 15->FUN_80074eec then return0.
//     else (id < 0x70): the descriptor-table SFX path (0x80074690 / 0x800746e0) -> jal SFX submit
//        (0x80075e04). Owned part = the id router; the descriptor math is kept dispatched (see below).
// Return v0 = 0 in all the routed/early cases (the body does `j 0x80074800` = epilogue, v0 already 0).
// ===================================================================================================
// id->song map for the id>=0x70 jump table (index = id-0x70):
static const int SFX_BGM_MAP[16] = { 2,3,4,5,6,7,10,11,12,13,10,11,12,-1,-1,-2 };  // -1 = ret0, -2 = FUN_80074eec

// Guard against re-entrancy of the descriptor sub-path's rec_dispatch(A_PLAY_SFX): a thread-local
// flag makes the inner dispatch run the recomp body, not this override.
static __thread int s_sfx_reenter = 0;

// Descriptor-table SFX submit (id < 0x70). This sub-path reads game CONTENT descriptor tables
// (0x800a4d18 / 0x800a4ef8) for pan/vol and funnels into the dispatched submit leaf (0x80075e04).
// We keep it dispatched in-context (it runs the SAME submit leaf -> identical SPU effect); the OWNED
// surface of sound_play_sfx is the id ROUTER (BGM mapping + bounds), which is the menu/SFX trigger the
// game logic calls. (Listed for a deeper pass — see journal later-207 "rest".)
static void sfx_descriptor_path(Core* c) {
  s_sfx_reenter = 1;             // the inner dispatch lands back in ov_sound_play_sfx -> gen body
  rec_dispatch(c, A_PLAY_SFX);
  s_sfx_reenter = 0;
}

static void play_sfx_body(Core* c) {
  uint32_t a0 = c->r[4];
  int32_t id = (int32_t)(a0 & 0xFFu);
  // (id<<24) < 0  <=>  id bit7 set:
  if ((int32_t)(a0 << 24) < 0) {            // bltz of (a0<<24)
    if (id >= 225) { c->r[2] = 0; return; } // sltiu id,225 == 0 -> return 0
    sfx_descriptor_path(c);                 // else the descriptor SFX path (0x800746e0)
    return;
  }
  if (id >= 0x70) {
    int id2 = id - 0x70;
    if (id2 >= 16) { c->r[2] = 0; return; } // sltiu (id-0x70),16 == 0 -> 0x800746d8 return 0
    int song = SFX_BGM_MAP[id2];
    if (song == -2) { rec_dispatch(c, 0x80074EECu); }   // entry 15
    else if (song >= 0) { c->r[4] = (uint32_t)song; rec_dispatch(c, A_PLAY_BGM); }  // sound_play_bgm(song) -> BGM override
    c->r[2] = 0;                            // every routed case does `j 0x80074800` with v0=0
    return;
  }
  sfx_descriptor_path(c);                   // id < 0x70: descriptor SFX path
}

// ===================================================================================================
// VERIFY GATE — `soundverify`: full RAM+scratchpad A/B vs rec_super_call, per call.
// ===================================================================================================
static void sound_verify(Core* c, uint32_t addr, void(*body)(Core*), const char* nm) {
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  body(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, addr);
  uint32_t v0_o = c->r[2];
  // Exclude pure stack scratch below entry sp: dispatched libsnd leaves run in BOTH passes and leave
  // transient values in their own frames below sp (and this fn's own gen frame is dead there). The
  // window is the top-of-RAM stack [sp-0x800, sp), far above all game/sound data.
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40)
      fprintf(stderr, "[soundverify] %s MISMATCH v0 n=%x o=%x ram@%x spad@%x sp=%x a0=%x song=%x\n",
                           nm, v0_n, v0_o, ro, so, sp, regs0[4], (int16_t)c->mem_r16(SND_SONG));
  } else if (++ng % 100 == 0) fprintf(stderr, "[soundverify] %ld matches (last %s)\n", ng, nm);
}

// ---- override entry points -----------------------------------------------------------------------
static int s_verify = -1;
static inline int verify_on(void) { if (s_verify < 0) s_verify = cfg_dbg("soundverify") ? 1 : 0; return s_verify; }

// BGM START — engine-glue wrapper. The trigger-CONTROL surface the game logic calls is owned native
// by the SFX/song router (ov_sound_play_sfx); the BGM start's BODY is libsnd-sequencer orchestration
// whose voicetab state is co-evolved with the SsSeqPlay/SsSeqOpen LEAVES (see the BOUNDARY NOTE on
// stop_bgm_body), so the gen body runs as the live sequencer via rec_super_call. The OWNED engine glue
// here is the instant-CD dialog-music cut hook (was native_boot ov_bgm_start) + the clean API.
void ov_sound_play_bgm(Core* c) {
  rec_super_call(c, A_PLAY_BGM);                      // libsnd sequencer start (the leaf-coupled body)
  xa_music_cut_if_dialog(c);                          // engine glue: cut looping ingame music on a dialog tone
}
// BGM STOP — engine-glue wrapper (was native_boot ov_bgm_stop). Same boundary as start.
void ov_sound_stop_bgm(Core* c) {
  rec_super_call(c, A_STOP_BGM);
}
// SFX / song-id ROUTER — fully OWNED PC-native (pure control flow; 0-diff verified via `soundverify`).
void ov_sound_play_sfx(Core* c) {
  if (s_sfx_reenter) { s_sfx_reenter = 0; rec_super_call(c, A_PLAY_SFX); return; }  // inner dispatch -> gen body
  if (verify_on()) { sound_verify(c, A_PLAY_SFX, play_sfx_body, "play_sfx"); return; }
  play_sfx_body(c);
}

// ---- public API (clean PC-game audio entry points) -----------------------------------------------
// The engine-facing names; the engine can call them directly once more callers are owned. They marshal
// into the trigger functions via the Core register ABI (BGM start/stop go through the gen sequencer).
void sound_play_bgm(Core* c, int song)              { c->r[4] = (uint32_t)song; ov_sound_play_bgm(c); }
void sound_stop_bgm(Core* c)                        { ov_sound_stop_bgm(c); }
void sound_play_sfx(Core* c, int id, int a1, int a2){ c->r[4]=(uint32_t)id; c->r[5]=(uint32_t)a1; c->r[6]=(uint32_t)a2; play_sfx_body(c); }

// ONE line added to game_tomba2.cpp init: sound_register();
void sound_register(void) {
  rec_set_override(A_PLAY_BGM, ov_sound_play_bgm);   // FUN_80074BF8 — BGM start  (replaces native_boot ov_bgm_start)
  rec_set_override(A_STOP_BGM, ov_sound_stop_bgm);   // FUN_80074E48 — BGM stop   (replaces native_boot ov_bgm_stop)
  rec_set_override(A_PLAY_SFX, ov_sound_play_sfx);   // FUN_80074590 — SFX / song-id router (OWNED native)
}
