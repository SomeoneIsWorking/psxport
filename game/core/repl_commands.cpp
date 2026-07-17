// repl_commands.cpp — the game-side REPL commands (GameHooks::replCommand).
//
// The framework REPL (runtime/recomp/repl.cpp) drives framework commands (memory/input/screenshot/
// gate/render toggles) that touch only c->mem_*, c->game->pad/spu_audio, mods and cfg. For any command
// it does NOT itself handle, it calls c->hooks->replCommand(c, cmd, line); the commands here that reach
// Tomba!2 game CLASSES (Inventory, MusicList) or Tomba-specific guest addresses (BGM/sequencer) live on
// this side so the framework names no game type. Returns true iff the command was recognised+handled.
#include "core.h"
#include "game_iface.h"
#include "game_ctx.h"           // inv(c) Inventory + gctx(c)->music_list MusicList
#include "audio/music_list.h"   // class MusicList — `musictest`
#include "guest_call.h"         // rc0/rc1/rc3 — rec_dispatch of the Tomba BGM / libsnd-seq guest leaves
#include <stdio.h>
#include <string.h>

bool tomba_repl_command(Core* c, const char* cmd, const char* line) {
  unsigned a = 0;
  if (!strcmp(cmd, "invtest")) {   // diagnostic: exercise the inventory subsystem with a test vector
    // invtest [type] [amt] — fire FUN_8004D338/D4C4/D4F4(type,amt) through the override path (with the
    // `invverify` gate enabled this runs the full RAM+scratchpad A/B vs the recomp body). With no args,
    // sweep a spread of item types/amounts covering both quest-ref variants + the 23..28 ring + the cap.
    int ty = -1, am = -1; sscanf(line, "%*s %d %d", &ty, &am);
    static const int vt[] = { 1, 2, 5, 10, 23, 25, 28, 40, 60, 99 };
    static const int va[] = { 1, 3, 1, 50, 1, 99, 2, 7, 1, 5 };
    int n = (ty >= 0) ? 1 : (int)(sizeof vt / sizeof vt[0]);
    for (int i = 0; i < n; i++) {
      uint32_t t = (ty >= 0) ? (uint32_t)ty : (uint32_t)vt[i];
      uint32_t m = (am >= 0) ? (uint32_t)am : (ty >= 0 ? 1u : (uint32_t)va[i]);
      inv(c).add(t, m);          // FUN_8004D338 core (via invverify gate)
      inv(c).give(t, m);         // FUN_8004D4F4 give_only
      inv(c).giveAndFlag(t, m);  // FUN_8004D4C4 give_and_flag
    }
    fprintf(stderr, "[repl] invtest: fired %d vector(s) through inventory overrides\n", n * 3);
    return true;
  }
  if (!strcmp(cmd, "bgm") && sscanf(line, "%*s %u", &a) == 1) {
    rc1(c, 0x80074BF8u, a); fprintf(stderr, "[repl] bgm %u (song@800bed80=%04X)\n", a, c->mem_r16(0x800bed80));
    return true;
  }
  if (!strcmp(cmd, "bgmstop")) { rc0(c, 0x80074E48u); fprintf(stderr, "[repl] bgmstop\n"); return true; }
  // seqsolo <i> — stop ALL open libsnd sequences then SsSeqPlay just sequence <i> at full vol, via the
  // GAME'S OWN sequencer. Lets each area SEP sequence be rendered in isolation (the area's field theme
  // otherwise plays continuously). SsSeqStop=0x80091AF0, SsSeqPlay(h,mode,loop)=0x80090560, SsSeqSetVol
  // (h,volL,volR)=0x80091F50. handle == the seq access index (0..13).
  if (!strcmp(cmd, "seqsolo") && sscanf(line, "%*s %u", &a) == 1) {
    for (uint32_t i = 0; i < 14; i++) rc1(c, 0x80091AF0u, i);   // SsSeqStop(i) — silence all
    rc3(c, 0x80090560u, a, 1, 0);                                // SsSeqPlay(a, mode=1, loop=0)
    rc3(c, 0x80091F50u, a, 127, 127);                           // SsSeqSetVol(a, 127, 127)
    fprintf(stderr, "[repl] seqsolo %u\n", a);
    return true;
  }
  // musictest <n> — play catalogued music track <n> through the NATIVE audio engine (sound test).
  // 'musictest stop' (or n<0) stops. Bypasses the broken libsnd path entirely (engine/audio/).
  if (!strcmp(cmd, "musictest")) {
    MusicList& ml = gctx(c)->music_list;   // music_list moved off Game onto the game-side TombaCtx
    char sub[32] = {0}; int n = -1;
    if (sscanf(line, "%*s %31s", sub) == 1 && !strcmp(sub, "stop")) { ml.stop(); fprintf(stderr, "[repl] musictest stop\n"); }
    else if (sscanf(line, "%*s %d", &n) == 1 && n >= 0) {
      int rc = ml.play(n);
      fprintf(stderr, "[repl] musictest %d (%s) -> %s\n", n, ml.name(n) ? ml.name(n) : "?", rc ? "FAIL" : "ok");
    } else {
      fprintf(stderr, "[repl] musictest: tracks 0..%d, or 'stop'\n", ml.count()-1);
      for (int i = 0; i < ml.count(); i++) fprintf(stderr, "   %d: %s\n", i, ml.name(i));
    }
    return true;
  }
  return false;   // not a game command — framework prints "? <cmd>"
}
