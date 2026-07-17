// engine.cpp — PORT_GEN draft, byte-faithful transcription of ov_game_gen_8010766C.
// ORACLE: ov_game_gen_8010766C (generated/ov_game_shard_1.c:208-274)
// PORT_GEN: 0x8010766C generated/ov_game_shard_1.c:208-274
//
// This body is the gen function's guest-visible operations VERBATIM — every c->r[] op,
// mem_r/mem_w call, func_X/rec_dispatch call with its r31 constant, and label/goto is
// preserved unchanged. Faithful by construction; the only allowed next step is RENAMING
// (locals/labels -> named fields/control-flow), verified equivalent by tools/port_check.py.
// WIRED 2026-07-16 via Engine::installFieldTransitions (engine_set_override_game); port_check PASS +
// MIRROR_VERIFY-gated. Verbatim PORT_GEN body (ov_game overlay).
#include "engine.h"
#include "game_ctx.h"
#include "core.h"

void rec_dispatch(Core*, uint32_t);  // overlay_router.cpp — shared dispatch choke point
void ov_game_func_80108CAC(Core*);  // generated/ov_game_shard_disp.c

void Engine::fieldTransitionCase5() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = (uint32_t)8064u << 16;
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)312));
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[3] + (uint32_t)78));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[2] + (uint32_t)1; if (_t) goto L_801076DC; }
    c->mem_w16((c->r[3] + (uint32_t)78), (uint16_t)c->r[2]);
    c->r[31] = 0x8010769Cu;
    c->mem_w8((c->r[3] + (uint32_t)107), (uint8_t)c->r[0]); rec_dispatch(c, 0x8007B18Cu);
    c->r[31] = 0x801076A4u;
     rec_dispatch(c, 0x800796DCu);
    c->r[31] = 0x801076ACu;
     rec_dispatch(c, 0x800263E8u);
    c->r[31] = 0x801076B4u;
     rec_dispatch(c, 0x80075240u);
    c->r[31] = 0x801076BCu;
     rec_dispatch(c, 0x800783DCu);
    c->r[31] = 0x801076C4u;
     rec_dispatch(c, 0x80078610u);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[31] = 0x801076D4u;
     rec_dispatch(c, 0x80074F24u);
     goto L_80107780;
  L_801076DC:;
    c->r[31] = 0x801076E4u;
     ov_game_func_80108CAC(c);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[4] = c->r[2] + (uint32_t)-2040;
    c->r[2] = (uint32_t)(int8_t)c->mem_r8((c->r[4] + (uint32_t)5));
    c->r[3] = c->r[0] + (uint32_t)3;
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_80107738; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32782u << 16; if (_t) goto L_80107780; }
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)32750));
    c->r[4] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32750));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_8010772C; }
    c->r[3] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[2] + (uint32_t)-1920), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)404), (uint16_t)c->r[4]);
  L_8010772C:;
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)312));
    c->mem_w16((c->r[2] + (uint32_t)78), (uint16_t)c->r[0]); goto L_80107780;
  L_80107738:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)49));
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_80107780; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[0] + (uint32_t)2; if (_t) goto L_80107780; }
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w8((c->r[4] + (uint32_t)49), (uint8_t)c->r[3]);
    c->mem_w8((c->r[2] + (uint32_t)566), (uint8_t)c->r[3]);
    c->r[3] = c->mem_r32((c->r[16] + (uint32_t)312));
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w16((c->r[3] + (uint32_t)74), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w16((c->r[3] + (uint32_t)76), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)6;
    c->mem_w16((c->r[3] + (uint32_t)78), (uint16_t)c->r[2]);
  L_80107780:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}


// ---- Wiring: the field/stage GAME-overlay transition handlers (ov_game band) ------------------
// Installed on the ov_game override table via the oracle-gated setter (engine_set_override_game,
// runtime/recomp/override_registry.cpp) so core B / psx_fallback stays pure gen. submode1's
// `case N: rec_dispatch(c, 0x8010766Cu)` reaches ov_game_func_8010766C → the thunk → this method.
extern void ov_game_gen_8010766C(Core*);
extern void engine_set_override_game(uint32_t, OverrideFn, OverrideFn);
namespace {
void gov_fieldTransitionCase5(Core* c) { eng(c).fieldTransitionCase5(); }
}
void Engine::installFieldTransitions() {
  static bool done = false;
  if (done) return;
  done = true;
  engine_set_override_game(0x8010766Cu, gov_fieldTransitionCase5, ov_game_gen_8010766C);
}
