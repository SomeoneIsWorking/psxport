// repl.cpp — interactive REPL driver (PSXPORT_REPL=1): read commands from stdin and drive the native
// port (run/step, memory peek/poke, input, screenshots, RAM dumps, entity/scene inspection, area warp,
// audio dumps). Extracted from native_boot.cpp (later-288) so the boot + scheduler file is not crammed
// with the debug driver. The scheduler (native_boot.cpp) calls c->game->repl.read() between frames and
// consumes the class Repl's auto-drive request fields (navNewgame / skipFrames / warpArmed / warpDest).
#include "core.h"
#include "game.h"
#include "c_subsys.h"
#include "cfg.h"
#include "asset.h"
#include "audio/music_list.h"
#include "render/render.h"    // Render::psxRender / setPsxRender (per-Core render-path switch)
#include "guest_call.h"
#include "repl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "native_gate.h"  // gate registry (native_gate.cpp); the `gate` command drives it.

// ---- Interactive REPL (PSXPORT_REPL=1) — drive the native port from stdin --------------------
// Mirrors the oracle's (wide60rt -repl) command set so one driver can step BOTH cores and diff.
// Commands: run N | r addr [len] | rw addr [words] | w addr val | w8 addr val | watch lo hi |
//   unwatch | hits | press/release <btn> | tap <btn> [frames] | regs | seq | quit. Memory is the
//   game's address space (mem_r*/mem_w*); watchpoints via mem_set_watch (reported during `run`).
static uint16_t repl_btn(const char* n) {     // name -> active-HIGH PSX pad bit
  if (!strcmp(n,"start"))    return 0x0008; if (!strcmp(n,"select")) return 0x0001;
  if (!strcmp(n,"x")||!strcmp(n,"cross"))  return 0x4000;
  if (!strcmp(n,"o")||!strcmp(n,"circle")) return 0x2000;
  if (!strcmp(n,"triangle")||!strcmp(n,"t")) return 0x1000;
  if (!strcmp(n,"square")||!strcmp(n,"sq"))  return 0x8000;
  if (!strcmp(n,"up"))    return 0x0010; if (!strcmp(n,"down"))  return 0x0040;
  if (!strcmp(n,"left"))  return 0x0080; if (!strcmp(n,"right")) return 0x0020;
  return (uint16_t)strtoul(n, 0, 16);
}
// ---- REPL music-dump helpers (build a labeled track library to identify each tune) -------
// Sequenced BGM is rendered through the live SPU (use `wav PATH` then `bgm N` then `run`);
// XA tracks (CD-streamed music/voice) are decoded straight off the disc here, since they
// never touch the sequencer. Both write standard 44100/native-rate stereo S16 WAVs.

static void repl_wav_write(const char* path, const int16_t* pcm, uint32_t frames, int rate) {
  FILE* fp = fopen(path, "wb");
  if (!fp) { fprintf(stderr, "[repl] wav write: cannot open %s\n", path); return; }
  uint32_t data = frames * 4u, riff = 36u + data, brate = (uint32_t)rate * 4u, fmtlen = 16;
  uint16_t pcm1 = 1, ch2 = 2, ba = 4, bits = 16; uint32_t r = (uint32_t)rate;
  fwrite("RIFF", 1, 4, fp); fwrite(&riff, 4, 1, fp); fwrite("WAVE", 1, 4, fp);
  fwrite("fmt ", 1, 4, fp); fwrite(&fmtlen, 4, 1, fp);
  fwrite(&pcm1, 2, 1, fp); fwrite(&ch2, 2, 1, fp); fwrite(&r, 4, 1, fp);
  fwrite(&brate, 4, 1, fp); fwrite(&ba, 2, 1, fp); fwrite(&bits, 2, 1, fp);
  fwrite("data", 1, 4, fp); fwrite(&data, 4, 1, fp);
  fwrite(pcm, 4, frames, fp); fclose(fp);
  fprintf(stderr, "[repl] xadump -> %s (%u frames @ %d Hz, %.2fs)\n", path, frames, rate, frames / (double)rate);
}

// Decode ~`secs` of the XA stream on subheader channel `chan` starting at CHD `start_lba`,
// write a WAV at the stream's native rate. Skips interleaved non-matching/non-audio sectors.
static void repl_xadump(uint8_t chan, uint32_t start_lba, const char* path, int secs) {
  static int16_t out[400000];                 // ~4.5s of 44100 stereo; XA max 37800*secs
  uint8_t raw[2352]; int16_t hist[2][2] = {{0,0},{0,0}}; int freq = 37800;
  uint32_t frames = 0, lba = start_lba, cap = 0;
  for (int guard = 0; guard < 20000; guard++) {
    if (!disc_read_raw(lba, raw, 2352)) break;
    if (raw[15] != 2) break;                   // ran off the Mode2 stream
    uint8_t fchan = raw[17], submode = raw[18];
    lba++;
    if (!(submode & 0x04) || fchan != chan) { if (submode & 0x80) break; continue; }  // not our audio
    int16_t pcm[4032 * 2]; int f2 = freq;
    int n = xa_decode_sector(raw, pcm, hist, &f2); freq = f2;
    if (!cap) cap = (uint32_t)freq * (uint32_t)secs;
    for (int i = 0; i < n && frames < cap && frames < 200000; i++) {
      out[2 * frames] = pcm[2 * i]; out[2 * frames + 1] = pcm[2 * i + 1]; frames++;
    }
    if (frames >= cap || (submode & 0x80)) break;
  }
  repl_wav_write(path, out, frames, freq);
}

// REPL auto-drive state (navNewgame / skipFrames / warpArmed / warpDest) lives on class Repl (repl.h) —
// arm here on the appropriate command, the native scheduler frame loop consumes on subsequent frames.
// `warp <id>` (dev/diagnostic): arm an AREA WARP. The GAME-stage area machine loads the area whose id is
// in the current-area global 0x800bf870; an area CHANGE is driven by FUN_80044bd4(area_task_entry=0x800452c0,
// dest_area_id, mode, phase) from inside the GAME stage SM (the steady handler 0x801088d8 case0 calls it with
// a1 = the current/destination area id). That registers/restarts the AREA-LOAD TASK (0x800452c0) which, via
// FUN_8004514c, commits 0x800bf870 = translate(dest), pulls the area overlay (disc LBA/size from the area
// table at 0x800be118, stride 8, indexed by id+3) to 0x80182000, and walks the per-area asset table at
// area_base+0x51000. We arm the dest id here and fire FUN_80044bd4 from the frame loop (scheduler context
// active, like `newgame`). See docs/engine_re.md "Area WARP / destination mechanism".

// Read+execute REPL commands until a `run N` (returns N) or quit/EOF (returns -1).
long Repl::read(Core* c, uint32_t f) {
  static uint16_t held = 0xFFFF;              // active-low held mask (all released)
  char line[256];
  fprintf(stderr, "[repl] frame=%u ready\n", f); fflush(stderr);
  while (fgets(line, sizeof line, stdin)) {
    char cmd[24] = {0}, arg[32] = {0}; unsigned a = 0, b = 0;
    if (sscanf(line, "%23s", cmd) != 1) continue;
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) return -1;
    else if (!strcmp(cmd, "run") && sscanf(line, "%*s %u", &a) == 1) return (long)a;
    else if (!strcmp(cmd, "r") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
      if (!b) b = 16; fprintf(stderr, "[repl] %08X:", a);
      for (unsigned i = 0; i < b && i < 256; i++) fprintf(stderr, " %02X", c->mem_r8(a + i)); fprintf(stderr, "\n");
    } else if (!strcmp(cmd, "rw") && sscanf(line, "%*s %x %u", &a, &b) >= 1) {
      if (!b) b = 8; fprintf(stderr, "[repl] %08X:", a);
      for (unsigned i = 0; i < b && i < 64; i++) fprintf(stderr, " %08X", c->mem_r32(a + i * 4)); fprintf(stderr, "\n");
    } else if (!strcmp(cmd, "w") && sscanf(line, "%*s %x %x", &a, &b) == 2) { c->mem_w32(a, b); fprintf(stderr, "[repl] ok\n"); }
    else if (!strcmp(cmd, "w8") && sscanf(line, "%*s %x %x", &a, &b) == 2) { c->mem_w8(a, (uint8_t)b); fprintf(stderr, "[repl] ok\n"); }
    else if (!strcmp(cmd, "watch") && sscanf(line, "%*s %x %x", &a, &b) == 2) c->mem_set_watch(a, b);
    else if (!strcmp(cmd, "unwatch")) { c->mem_set_watch(0, 0); fprintf(stderr, "[repl] unwatch\n"); }
    else if (!strcmp(cmd, "hits")) fprintf(stderr, "[repl] watch hits=%d\n", c->mem_watch_hits());
    else if (!strcmp(cmd, "press") && sscanf(line, "%*s %31s", arg) == 1)   { held &= ~repl_btn(arg); c->game->pad.driveHold(held); fprintf(stderr, "[repl] held=%04X\n", held); }
    else if (!strcmp(cmd, "release") && sscanf(line, "%*s %31s", arg) == 1) { held |= repl_btn(arg);  c->game->pad.driveHold(held); fprintf(stderr, "[repl] held=%04X\n", held); }
    else if (!strcmp(cmd, "tap") && sscanf(line, "%*s %31s %u", arg, &a) >= 1) { if (!a) a = 4; c->game->pad.driveTap((uint16_t)(0xFFFF & ~repl_btn(arg)), (int)a); fprintf(stderr, "[repl] tap %s %u\n", arg, a); }
    else if (!strcmp(cmd, "debug")) { char ch[200] = {0}; sscanf(line, "%*s %199[^\n]", ch); void cfg_dbg_set(const char*); cfg_dbg_set(ch); fprintf(stderr, "[repl] debug channels = %s\n", ch[0] ? ch : "(none)"); }
    else if (!strcmp(cmd, "ents")) {   // enumerate live GAME OBJECTS across the 3 entity lists, with identity
      // Each object is a node in a doubly-linked list (next @ +0x24). Identity fields: type @+0xc, render
      // intrinsic @+0xb (0x10..0x14 = sprite/billboard, 0/0xf = mesh), behavior handler @+0x1c (the object's
      // "what is it" — different per Tomba / enemy / prop), model id @+0xe & 0x3fff, world pos @+0x2e/32/36,
      // and the 3D MODEL = geomblk of render cmd[0] (cmd @+0xc0, geomblk @ cmd+0x40). later-241.
      // OWNERSHIP + IDENTITY (later-287, for drive/diagnose): flag each object's behavior handler as
      // native-OWNED (readable C, shown by name) or still-PSX substrate; and mark the PLAYER node (the one
      // whose int16 pos mirrors Tomba's 16.16 master position at 0x800E7EAC/B0/B4). This makes "what is
      // this object and is its logic ours" answerable at a glance.
      int16_t px = (int16_t)(c->mem_r32(0x800E7EACu) >> 16), pz = (int16_t)(c->mem_r32(0x800E7EB4u) >> 16);
      const uint32_t heads[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
      int total = 0, owned = 0;
      for (int h = 0; h < 3; h++) {
        uint32_t n = c->mem_r32(heads[h]);
        fprintf(stderr, "[ents] -- list %d head=%08X --\n", h, n);
        for (int g = 0; n && g < 400; g++, n = c->mem_r32(n + 0x24)) {
          uint32_t cmd0 = c->mem_r8(n + 8) ? c->mem_r32(n + 0xC0) : 0;
          uint32_t hh = c->mem_r32(n + 0x1c);
          const char* bn = c->engine.behaviors.nativeName(hh);
          int16_t nx = c->mem_r16s(n + 0x2e), nz = c->mem_r16s(n + 0x36);
          int is_player = (c->mem_r32(n + 0x38) == 0) && (nx == px) && (nz == pz);
          if (bn) owned++;
          fprintf(stderr, "[ents]  %08X t=%02X ri=%02X model=%04X h=%08X pos=(%6d,%6d,%6d) rf=%u cmds=%u gb0=%08X  %s%s\n",
                  n, c->mem_r8(n + 0xc), c->mem_r8(n + 0xb), c->mem_r16(n + 0xe) & 0x3fff, hh,
                  c->mem_r16s(n + 0x2e), c->mem_r16s(n + 0x32), nz,
                  c->mem_r8(n + 1), c->mem_r8(n + 8), cmd0 ? c->mem_r32(cmd0 + 0x40) : 0,
                  bn ? bn : "PSX", is_player ? "  <== PLAYER" : "");
          total++;
        }
      }
      fprintf(stderr, "[ents] (%d nodes; %d native-owned, %d still-PSX)\n", total, owned, total - owned);
    }
    else if (!strcmp(cmd, "tp")) { int x=0,y=0,z=0;
      if (sscanf(line, "%*s %d %d %d", &x, &y, &z) == 3) { c->engine.camTeleport(x, y, z); fprintf(stderr, "[repl] tp camera -> (%d,%d,%d)\n", x, y, z); }
      else { c->engine.camTeleportOff(); fprintf(stderr, "[repl] tp off (camera follows player)\n"); } }
    else if (!strcmp(cmd, "invtest")) {   // diagnostic: exercise the inventory subsystem with a test vector
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
        c->inventory.add(t, m);          // FUN_8004D338 core (via invverify gate)
        c->inventory.give(t, m);         // FUN_8004D4F4 give_only
        c->inventory.giveAndFlag(t, m);  // FUN_8004D4C4 give_and_flag
      }
      fprintf(stderr, "[repl] invtest: fired %d vector(s) through inventory overrides\n", n * 3); }
    else if (!strcmp(cmd, "newgame")) { this->navNewgame = 1; fprintf(stderr, "[repl] newgame: pulsing to GAME prologue\n"); return 100000; }
    else if (!strcmp(cmd, "skip")) { a = 0; sscanf(line, "%*s %u", &a); if (!a) a = 500; this->skipFrames = (long)a; fprintf(stderr, "[repl] skip %u frames\n", a); return (long)a; }
    else if (!strcmp(cmd, "warp")) {
      // warp <area_id> — load a different area on demand (foundation for a level/boss selector). Only valid
      // from the field (GAME stage 0x8010637C, sm[0x48]==2). Arms the dest; the frame loop fires it.
      if (sscanf(line, "%*s %u", &a) == 1) {
        if (c->mem_r32(0x801fe00c) != 0x8010637Cu)
          fprintf(stderr, "[repl] warp: not in GAME stage (stage=%08X) — reach the field first (newgame/skip)\n",
                  c->mem_r32(0x801fe00c));
        else {
          this->warpDest = a; this->warpArmed = 1;
          fprintf(stderr, "[repl] warp: armed dest area id=%u (cur=%u) — run frames to load\n",
                  a, c->mem_r8(0x800bf870u));
        }
      } else fprintf(stderr, "[repl] warp <area_id>  (area table @0x800be118, ids 0..23)\n");
    }
    else if (!strcmp(cmd, "shot")) { char path[200] = {0}; if (sscanf(line, "%*s %199s", path) == 1) { void gpu_native_shot(Core*, const char*); gpu_native_shot(c, path); } }
    else if (!strcmp(cmd, "vram")) { char path[200] = {0}; unsigned x=0,y=0,w=1024,h=512;
      if (sscanf(line, "%*s %199s %u %u %u %u", path, &x,&y,&w,&h) >= 1) {
        void gpu_gpu_vram_region(const char*, int, int, int, int); gpu_gpu_vram_region(path, (int)x,(int)y,(int)w,(int)h); } }
    else if (!strcmp(cmd, "vramraw")) { char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) { void gpu_gpu_vram_raw(const char*); gpu_gpu_vram_raw(path); } }
    else if (!strcmp(cmd, "dumpram")) {
      char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) {
        FILE* fp = fopen(path, "wb");
        if (fp) { fwrite(c->ram, 1, 0x200000, fp); fclose(fp); fprintf(stderr, "[repl] dumpram -> %s\n", path); }
        else fprintf(stderr, "[repl] dumpram: cannot open %s\n", path);
        // Also dump the 1 KB scratchpad (0x1F800000) to a sidecar .spad — the main-RAM A/B diff is
        // BLIND to scratchpad, where several engine flags live (DEMO: 0x1f80019a/19d/134/198 etc.).
        char spath[208]; snprintf(spath, sizeof spath, "%s.spad", path);
        FILE* sp = fopen(spath, "wb");
        if (sp) { fwrite(c->scratch, 1, sizeof c->scratch, sp); fclose(sp); fprintf(stderr, "[repl] dumpram scratchpad -> %s\n", spath); }
      }
    }
    else if (!strcmp(cmd, "wav")) { char path[200] = {0}; if (sscanf(line, "%*s %199s", path) == 1) c->game->spu_audio.wavReopen(path); }
    else if (!strcmp(cmd, "bgm") && sscanf(line, "%*s %u", &a) == 1) { rc1(c, 0x80074BF8u, a); fprintf(stderr, "[repl] bgm %u (song@800bed80=%04X)\n", a, c->mem_r16(0x800bed80)); }
    else if (!strcmp(cmd, "bgmstop")) { rc0(c, 0x80074E48u); fprintf(stderr, "[repl] bgmstop\n"); }
    // native <name> on|off  /  native list — gate PC-native layers (default ON) so the recomp oracle
    // runs in their place. e.g. `native music off` drops the native field-BGM engine.
    else if (!strcmp(cmd, "native")) {
      char nm[64] = {0}, st[16] = {0}; int k = sscanf(line, "%*s %63s %15s", nm, st);
      if (k <= 0 || !strcmp(nm, "list")) c->game->native_gates.list();
      else { c->game->native_gates.set(nm, strcmp(st, "off") != 0);
             fprintf(stderr, "[repl] native %s = %s\n", nm, strcmp(st, "off") ? "on" : "off"); }
    }
    // gate on|off (or 1|0) — toggle PSX-fallback live: everything the frame loop calls runs as PSX recomp
    // (sync CD) instead of the native owners. Applies to tasks freshly (re)entered after the toggle; an
    // already-running native dispatcher is reset on its next scheduler step so it re-enters as PSX.
    else if (!strcmp(cmd, "gate")) {
      char st[16] = {0};
      if (sscanf(line, "%*s %15s", st) == 1)
        c->game->psx_fallback = (!strcmp(st, "off") || !strcmp(st, "0")) ? 0 : 1;
      fprintf(stderr, "[repl] psx_fallback = %d\n", c->game->psx_fallback);
    }
    // renderpsx on|off — render the FIELD via the PSX recomp path (vs the native world-coord path) with the
    // SAME native game state, for a native-vs-PSX RENDER diff (must match at 1x/4:3/30fps). Diagnostic.
    else if (!strcmp(cmd, "renderpsx")) {
      char st[16] = {0};
      if (sscanf(line, "%*s %15s", st) == 1)
        c->mRender->mode.setPsxRender(!(!strcmp(st, "off") || !strcmp(st, "0")));
      fprintf(stderr, "[repl] Render::psxRender = %d\n", c->mRender->mode.psxRender());
    }
    // seqsolo <i> — stop ALL open libsnd sequences then SsSeqPlay just sequence <i> at full vol, via the
    // GAME'S OWN sequencer. Lets each area SEP sequence be rendered in isolation (the area's field theme
    // otherwise plays continuously). SsSeqStop=0x80091AF0, SsSeqPlay(h,mode,loop)=0x80090560, SsSeqSetVol
    // (h,volL,volR)=0x80091F50. handle == the seq access index (0..13).
    else if (!strcmp(cmd, "seqsolo") && sscanf(line, "%*s %u", &a) == 1) {
      for (uint32_t i = 0; i < 14; i++) rc1(c, 0x80091AF0u, i);   // SsSeqStop(i) — silence all
      rc3(c, 0x80090560u, a, 1, 0);                                // SsSeqPlay(a, mode=1, loop=0)
      rc3(c, 0x80091F50u, a, 127, 127);                           // SsSeqSetVol(a, 127, 127)
      fprintf(stderr, "[repl] seqsolo %u\n", a);
    }
    // musictest <n> — play catalogued music track <n> through the NATIVE audio engine (sound test).
    // 'musictest stop' (or n<0) stops. Bypasses the broken libsnd path entirely (engine/audio/).
    else if (!strcmp(cmd, "musictest")) {
      MusicList& ml = c->game->music_list;
      char sub[32] = {0}; int n = -1;
      if (sscanf(line, "%*s %31s", sub) == 1 && !strcmp(sub, "stop")) { ml.stop(); fprintf(stderr, "[repl] musictest stop\n"); }
      else if (sscanf(line, "%*s %d", &n) == 1 && n >= 0) {
        int rc = ml.play(n);
        fprintf(stderr, "[repl] musictest %d (%s) -> %s\n", n, ml.name(n) ? ml.name(n) : "?", rc ? "FAIL" : "ok");
      } else {
        fprintf(stderr, "[repl] musictest: tracks 0..%d, or 'stop'\n", ml.count()-1);
        for (int i = 0; i < ml.count(); i++) fprintf(stderr, "   %d: %s\n", i, ml.name(i));
      }
    }
    else if (!strcmp(cmd, "xadump")) { unsigned ch = 0, lba = 0, secs = 3; char path[200] = {0};
      if (sscanf(line, "%*s %u %u %199s %u", &ch, &lba, path, &secs) >= 3) repl_xadump((uint8_t)ch, lba, path, secs ? (int)secs : 3); }
    else if (!strcmp(cmd, "prof")) {
      void prof_start(void); void prof_stop(void); void prof_dump(const char*);
      char sub[32] = {0}, path[200] = {0}; sscanf(line, "%*s %31s %199s", sub, path);
      if (!strcmp(sub, "start")) prof_start();
      else if (!strcmp(sub, "stop") || !strcmp(sub, "off")) prof_stop();
      else if (!strcmp(sub, "dump")) prof_dump(path[0] ? path : 0);
      else fprintf(stderr, "[repl] prof: start | stop | dump <path>\n");
    }
    else if (!strcmp(cmd, "trace")) {   // trace <path> : open the interp call tracer; `trace` alone closes
      void interp_trace_open(const char*); char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) { interp_trace_open(path); fprintf(stderr, "[repl] trace -> %s\n", path); }
      else { interp_trace_open(0); fprintf(stderr, "[repl] trace closed\n"); }
    }
    else if (!strcmp(cmd, "stage")) fprintf(stderr, "[repl] stage=%08X sm48=%d\n", c->mem_r32(0x801fe00c), (int)c->mem_r16(0x801fe048));
    else if (!strcmp(cmd, "regs")) { for (int i = 0; i < 32; i++) { fprintf(stderr, " r%-2d=%08X", i, c->r[i]); if ((i & 3) == 3) fprintf(stderr, "\n"); } fprintf(stderr, " hi=%08X lo=%08X\n", c->hi, c->lo); }
    else if (!strcmp(cmd, "seq")) fprintf(stderr, "[repl] seq open=%d playmask=%04X tickmode=%d seqfn=%08X stage=%08X\n",
                                          c->mem_r16s(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF, c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(0x801fe00c));
    else fprintf(stderr, "[repl] ? %s\n", cmd);
    fflush(stderr);
  }
  return -1;  // EOF
}
