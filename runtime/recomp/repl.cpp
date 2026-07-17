// repl.cpp — interactive REPL driver (PSXPORT_REPL=1): read commands from stdin and drive the native
// port (run/step, memory peek/poke, input, screenshots, RAM dumps, entity/scene inspection, area warp,
// audio dumps). Extracted from native_boot.cpp (later-288) so the boot + scheduler file is not crammed
// with the debug driver. The scheduler (native_boot.cpp) calls c->game->repl.read() between frames and
// consumes the class Repl's auto-drive request fields (navNewgame / skipFrames / warpArmed / warpDest).
#include "core.h"
#include "game_iface.h"          // GameHooks — game-side command dispatch (replCommand) + REPL diag hooks
#include "game.h"
#include "c_subsys.h"
#include "cfg.h"
#include "render_substrate.h"    // Render::psxRender / setPsxRender (per-Core render-path switch)
#include "ot_attr.h"   // OtAttr — `otattr` command (OT/GTE submission attribution)
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
static void repl_xadump(DiscState* disc, uint8_t chan, uint32_t start_lba, const char* path, int secs) {
  int16_t* out = (int16_t*)calloc(400000, sizeof(int16_t));   // ~4.5s of 44100 stereo; XA max 37800*secs
  if (!out) return;
  uint8_t raw[2352]; int16_t hist[2][2] = {{0,0},{0,0}}; int freq = 37800;
  uint32_t frames = 0, lba = start_lba, cap = 0;
  for (int guard = 0; guard < 20000; guard++) {
    if (!disc_read_raw(disc, lba, raw, 2352)) break;
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
  free(out);
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
  uint16_t& held = mHeldMask;                 // active-low held mask (persists across REPL entries)
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
          const char* bn = c->hooks->replBehaviorName(c, hh);
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
      if (sscanf(line, "%*s %d %d %d", &x, &y, &z) == 3) { c->hooks->replCamTeleport(c, x, y, z); fprintf(stderr, "[repl] tp camera -> (%d,%d,%d)\n", x, y, z); }
      else { c->hooks->replCamTeleportOff(c); fprintf(stderr, "[repl] tp off (camera follows player)\n"); } }
    else if (!strcmp(cmd, "newgame")) { this->navNewgame = 1; fprintf(stderr, "[repl] newgame: pulsing to GAME prologue\n"); return 100000; }
    else if (!strcmp(cmd, "skip")) { a = 0; sscanf(line, "%*s %u", &a); if (!a) a = 500; this->skipFrames = (long)a; fprintf(stderr, "[repl] skip %u frames\n", a); return (long)a; }
    else if (!strcmp(cmd, "warp")) {
      // warp <area_id> [sub] — load a different area on demand via the REAL DOOR RECORD (foundation for a
      // level/boss selector). Only valid from the field (GAME stage 0x8010637C, sm[0x48]==2). Arms the
      // dest; the frame loop writes the door record (0x800BF839/0x800BF83A) so the running field-run
      // machine runs the game's own transition sequence — fade-out, object teardown, CD settle, reload —
      // exactly as a real door does (engine_re.md "Area WARP"). Replaces the old forced-case0 warp, which
      // skipped the teardown and flooded recomp-misses from stale objects.
      unsigned sub = 0; int nargs = sscanf(line, "%*s %u %u", &a, &sub);
      if (nargs >= 1) {
        if (c->mem_r32(0x801fe00c) != 0x8010637Cu)
          fprintf(stderr, "[repl] warp: not in GAME stage (stage=%08X) — reach the field first (newgame/skip)\n",
                  c->mem_r32(0x801fe00c));
        else {
          this->warpDest = a; this->warpSub = (nargs == 2) ? sub : 0; this->warpArmed = 1;
          fprintf(stderr, "[repl] warp: armed dest area id=%u sub=%u (cur=%u) via door record — run frames to load\n",
                  a, this->warpSub, c->mem_r8(0x800bf870u));
        }
      } else fprintf(stderr, "[repl] warp <area_id> [sub]  (area table @0x800be118, ids 0..0x1f; sub = 0x800BF871 sub-state)\n");
    }
    else if (!strcmp(cmd, "preseq")) {   // arm a PRESENT-sequence dump: next N presented frames (real + fps60 interp)
      unsigned n = 0; char dir[120] = "scratch/screenshots/preseq";
      if (sscanf(line, "%*s %u %119s", &n, dir) >= 1 && n > 0) {
        extern void gpu_vk_preseq_arm(Core*, int, const char*);
        gpu_vk_preseq_arm(c, (int)n, dir);
        fprintf(stderr, "[repl] preseq armed: next %u presents -> %s/p%%04d.ppm\n", n, dir);
      } else fprintf(stderr, "[repl] preseq <N> [dir] — dump the next N PRESENTED frames (incl. fps60 interp)\n");
    }
    else if (!strcmp(cmd, "shot")) {   // VK-aware, same pick as dbg_server's `shot`: capture what is PRESENTED
      char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) {
        int gpu_vk_enabled(void); void gpu_vk_shot(Core*, const char*); void gpu_native_shot(Core*, const char*);
        if (gpu_vk_enabled()) { gpu_vk_shot(c, path); fprintf(stderr, "[repl] shot (VK) -> %s\n", path); }
        else                   { gpu_native_shot(c, path); fprintf(stderr, "[repl] shot (SW) -> %s\n", path); }
      }
    }
    else if (!strcmp(cmd, "setires")) {   // live ires toggle (same mods.ires mutation the RmlUi overlay's
      // "ires" row does — 0=Auto, 1..cap=fixed). Exercises GpuVkState::ensure_ires_targets' teardown+
      // rebuild path headless, without needing a windowed run to reach the overlay.
      unsigned n = 0;
      if (sscanf(line, "%*s %u", &n) == 1) { c->game->mods.ires = (int)n; fprintf(stderr, "[repl] mods.ires = %d\n", c->game->mods.ires); }
      else fprintf(stderr, "[repl] setires <0..4> — live ires toggle (0=Auto)\n");
    }
    else if (!strcmp(cmd, "iresdump")) {   // DEBUG ONLY (ires bring-up): raw dump of the ires-scaled target
      char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) { void gpu_vk_ires_rawdump(Core*, const char*); gpu_vk_ires_rawdump(c, path); }
    }
    else if (!strcmp(cmd, "vram")) { char path[200] = {0}; unsigned x=0,y=0,w=1024,h=512;
      if (sscanf(line, "%*s %199s %u %u %u %u", path, &x,&y,&w,&h) >= 1) {
        void gpu_vk_vram_region(Core*, const char*, int, int, int, int); gpu_vk_vram_region(c, path, (int)x,(int)y,(int)w,(int)h); } }
    else if (!strcmp(cmd, "vramraw")) { char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) { void gpu_vk_vram_raw(Core*, const char*); gpu_vk_vram_raw(c, path); } }
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
        c->rsub.mode.setPsxRender(!(!strcmp(st, "off") || !strcmp(st, "0")));
      fprintf(stderr, "[repl] Render::psxRender = %d\n", c->rsub.mode.psxRender());
    }
    else if (!strcmp(cmd, "xadump")) { unsigned ch = 0, lba = 0, secs = 3; char path[200] = {0};
      if (sscanf(line, "%*s %u %u %199s %u", &ch, &lba, path, &secs) >= 3) repl_xadump(&c->game->disc, (uint8_t)ch, lba, path, secs ? (int)secs : 3); }
    else if (!strcmp(cmd, "prof")) {
      void prof_start(Core*); void prof_stop(Core*); void prof_dump(Core*, const char*);
      char sub[32] = {0}, path[200] = {0}; sscanf(line, "%*s %31s %199s", sub, path);
      if (!strcmp(sub, "start")) prof_start(c);
      else if (!strcmp(sub, "stop") || !strcmp(sub, "off")) prof_stop(c);
      else if (!strcmp(sub, "dump")) prof_dump(c, path[0] ? path : 0);
      else fprintf(stderr, "[repl] prof: start | stop | dump <path>\n");
    }
    else if (!strcmp(cmd, "trace")) {   // trace <path> : open the interp call tracer; `trace` alone closes
      void interp_trace_open(Core*, const char*); char path[200] = {0};
      if (sscanf(line, "%*s %199s", path) == 1) { interp_trace_open(c, path); fprintf(stderr, "[repl] trace -> %s\n", path); }
      else { interp_trace_open(c, 0); fprintf(stderr, "[repl] trace closed\n"); }
    }
    // otattr — dump the OT/GTE submission-attribution tables (game/render/ot_attr.h, `debug otattr`).
    // Re-walks the LAST OT this Core drew (GpuState::s_ot_madr) READ-ONLY (header/first-word peeks
    // only — no gpu_gp0() call, so no draw/state side effects), and for each non-empty node looks up
    // the packet-pool store span attribution recorded during that frame's submission: {emitter fn = the
    // otattr shadow-stack top, caller fn = one frame below it, node = the render-walk object scope open
    // at store time}. Only produces attribution if `debug otattr` was ALREADY ON while the frame that
    // built this OT ran — turn the channel on BEFORE the frame you want to inspect, then run this after.
    // Pipe stderr through tools/symres.py to resolve the raw hex fn/node addresses to FUN_/native names.
    else if (!strcmp(cmd, "otattr")) {
      OtAttr& oa = c->rsub.otAttr;
      char sub[32] = {0};
      sscanf(line, "%*s %31s", sub);
      // LAST-WRITER PROVENANCE sub-commands (ot_attr.h) — answer "who wrote this WORD", independent of
      // call-flow, for the staging-buffer case where call-path attribution only names the batcher.
      if (!strcmp(sub, "watch")) {
        uint32_t addr = 0, len = 0;
        if (sscanf(line, "%*s %*s %x %x", &addr, &len) != 2 || len == 0) {
          fprintf(stderr, "[otattr] usage: otattr watch <addr-hex> <len-hex>\n");
        } else {
          int slot = oa.watchRegister(addr, len);
          if (slot < 0)
            fprintf(stderr, "[otattr] watch REJECTED (slots=%d/%d wordsUsed=%d/%d overflow=%d) — free a "
                             "slot or shrink the region\n",
                    oa.watchSlotCount(), OtAttr::WATCH_SLOTS, oa.watchWordsUsed(), OtAttr::WATCH_CAP_WORDS,
                    oa.watchOverflow());
          else {
            const OtAttr::WatchRegion* r = oa.watchAt(slot);
            // Decorate as KSEG0 (0x800xxxxx) ONLY for main-RAM regions — scratchpad (0x1F800000-
            // 0x1F8003FF) is NOT mirrored across segments the way RAM is, so blindly OR-ing 0x80000000
            // onto it prints a bogus 0x9F8xxxxx address. Print scratchpad addresses as-is (their own
            // canonical form).
            uint32_t dlo = r->lo < 0x200000u ? (0x80000000u | r->lo) : r->lo;
            uint32_t dhi = r->hi < 0x200000u ? (0x80000000u | r->hi) : r->hi;
            fprintf(stderr, "[otattr] watch[%d] = [0x%08X,0x%08X) (%u words) — slots %d/%d, %d/%d words used\n",
                    slot, dlo, dhi, (r->hi - r->lo) / 4,
                    oa.watchSlotCount(), OtAttr::WATCH_SLOTS, oa.watchWordsUsed(), OtAttr::WATCH_CAP_WORDS);
          }
        }
        fflush(stderr);
        continue;
      }
      if (!strcmp(sub, "who")) {
        uint32_t addr = 0, len = 4;
        int got = sscanf(line, "%*s %*s %x %x", &addr, &len);
        if (got < 1) { fprintf(stderr, "[otattr] usage: otattr who <addr-hex> [len-hex]\n"); fflush(stderr); continue; }
        if (got == 1) len = 4;
        fprintf(stderr, "[otattr] who 0x%08X..0x%08X (word-granular, coalesced runs):\n", addr, addr + len);
        uint32_t w = addr & ~3u, end = addr + len;
        bool any = false;
        OtAttr::WordRec cur{}; uint32_t runLo = 0, runHi = 0; bool haveRun = false;
        auto flush_run = [&]() {
          if (!haveRun) return;
          if (cur.frame == 0xFFFFFFFFu)
            fprintf(stderr, "  [0x%08X,0x%08X) NEVER WRITTEN since watch registered\n", runLo, runHi);
          else
            fprintf(stderr, "  [0x%08X,0x%08X) fn=0x%08X caller=0x%08X frame=%u\n",
                    runLo, runHi, cur.fn, cur.caller, cur.frame);
          any = true; haveRun = false;
        };
        for (; w < end; w += 4) {
          OtAttr::WordRec rec{}; uint32_t wa = 0;
          if (!oa.watchLookup(w, &rec, &wa)) {
            flush_run();
            fprintf(stderr, "  [0x%08X,0x%08X) NOT WATCHED — run `otattr watch` first\n", w, w + 4);
            continue;
          }
          if (haveRun && rec.fn == cur.fn && rec.caller == cur.caller && rec.frame == cur.frame && wa == runHi) {
            runHi = wa + 4;
          } else {
            flush_run();
            cur = rec; runLo = wa; runHi = wa + 4; haveRun = true;
          }
        }
        flush_run();
        if (!any) fprintf(stderr, "  (nothing in range)\n");
        fflush(stderr);
        continue;
      }
      if (!strcmp(sub, "trace")) {
        uint32_t addr = 0;
        if (sscanf(line, "%*s %*s %x", &addr) != 1) { fprintf(stderr, "[otattr] usage: otattr trace <addr-hex>\n"); fflush(stderr); continue; }
        OtAttr::WordRec rec{}; uint32_t wa = 0;
        if (!oa.watchLookup(addr, &rec, &wa)) {
          fprintf(stderr, "[otattr] trace 0x%08X: NOT in any watched region — `otattr watch <addr> <len>` first\n", addr);
          fflush(stderr); continue;
        }
        if (rec.frame == 0xFFFFFFFFu) {
          fprintf(stderr, "[otattr] trace 0x%08X: watched, but NEVER WRITTEN since registration\n", addr);
          fflush(stderr); continue;
        }
        fprintf(stderr, "[otattr] trace 0x%08X (word 0x%08X): last writer fn=0x%08X caller=0x%08X frame=%u\n",
                addr, wa, rec.fn, rec.caller, rec.frame);
        // One-hop heuristic: does the writer LOOK like a copy loop (touches many distinct 4KB pages in
        // the same frame, i.e. it fans out across many destinations rather than emitting one thing)?
        const OtAttr::FnStoreStat* st = oa.fnStatFind(rec.fn);
        if (!st) {
          fprintf(stderr, "  [trace] no per-fn store stat recorded for this fn this frame (stale/cross-frame "
                           "lookup) — re-run `otattr trace` in the same frame the write happened.\n");
        } else {
          bool looksLikeCopyLoop = st->pageOverflow || st->pageCount >= 3;
          fprintf(stderr, "  [trace] fn=0x%08X made %u store(s) touching %d distinct 4KB page(s) this frame%s -> %s\n",
                  rec.fn, st->count, st->pageCount, st->pageOverflow ? "+" : "",
                  looksLikeCopyLoop ? "LOOKS LIKE A COPY/BATCH LOOP (writes fan out across many destinations; "
                                      "the object identity was likely already lost by the time it reached this word)"
                                    : "looks like a direct, single-destination writer");
          fprintf(stderr, "  [trace] SOURCE not statically determinable from store data alone (this tool only "
                           "sees writes, not reads) — Ghidra-decompile fn=0x%08X (tools/decomp.sh) to find its "
                           "read pointer/source struct, then `otattr watch <src_addr> <len>` and re-run `otattr "
                           "who`/`trace` on the source to walk one more hop back.\n", rec.fn);
        }
        fflush(stderr);
        continue;
      }
      fprintf(stderr, "[otattr] frame=%u spans=%d(overflow=%d) gteBuckets=%d(overflow=%d) watch: slots=%d/%d "
                       "words=%d/%d(overflow=%d)\n",
              oa.frame(), oa.spanCount(), oa.spanOverflow(), oa.gteCount(), oa.gteOverflow(),
              oa.watchSlotCount(), OtAttr::WATCH_SLOTS, oa.watchWordsUsed(), OtAttr::WATCH_CAP_WORDS, oa.watchOverflow());
      uint32_t madr = c->game->gpu.s_ot_madr;
      if (!madr) {
        fprintf(stderr, "[otattr] GpuState::s_ot_madr == 0 — no OT walked yet this run\n");
      } else {
        uint32_t a = madr & 0x1FFFFC;
        for (int idx = 0; idx < 0x10000; idx++) {
          uint32_t hdr = c->mem_r32(a);
          unsigned n = hdr >> 24;   // prim GP0-word count (tag high byte) — 0 == link-only sentinel node
          if (n > 0) {
            uint32_t w0 = c->mem_r32(a + 4);
            uint8_t  op = (uint8_t)(w0 >> 24);
            int vx = 0, vy = 0;
            if (n >= 2) { uint32_t w1 = c->mem_r32(a + 8); vx = (int16_t)(w1 & 0xFFFF); vy = (int16_t)(w1 >> 16); }
            OtAttr::Span sp{};
            bool found = oa.lookupStore(a, &sp);
            uint32_t node = found ? sp.node : 0;
            uint32_t beh = node ? c->mem_r32((node & 0x1FFFFFFF) + 0x1C) : 0;
            fprintf(stderr, "  [%4d] pool=0x%08X op=0x%02X n=%u v0=(%d,%d)  fn=0x%08X caller=0x%08X node=0x%08X beh@node+1C=0x%08X\n",
                    idx, 0x80000000u | a, op, n, vx, vy,
                    found ? sp.fn : 0, found ? sp.caller : 0, node, beh);
          }
          uint32_t next = hdr & 0xFFFFFF;
          if (next == 0xFFFFFF || next == 0) break;
          a = next & 0x1FFFFC;
        }
      }
      fprintf(stderr, "[otattr] GTE RTPS/RTPT per-(fn,node) call counts this frame:\n");
      for (int i = 0; i < oa.gteCount(); i++) {
        const OtAttr::GteBucket* g = oa.gteAt(i);
        fprintf(stderr, "  fn=0x%08X node=0x%08X count=%u\n", g->fn, g->node, g->count);
      }
    }
    else if (!strcmp(cmd, "stage")) fprintf(stderr, "[repl] stage=%08X sm48=%d\n", c->mem_r32(0x801fe00c), (int)c->mem_r16(0x801fe048));
    else if (!strcmp(cmd, "regs")) { for (int i = 0; i < 32; i++) { fprintf(stderr, " r%-2d=%08X", i, c->r[i]); if ((i & 3) == 3) fprintf(stderr, "\n"); } fprintf(stderr, " hi=%08X lo=%08X\n", c->hi, c->lo); }
    else if (!strcmp(cmd, "seq")) fprintf(stderr, "[repl] seq open=%d playmask=%04X tickmode=%d seqfn=%08X stage=%08X\n",
                                          c->mem_r16s(0x801054B0), c->mem_r32(0x80104C28) & 0xFFFF, c->mem_r8(0x800AC424), c->mem_r32(0x800AC42C), c->mem_r32(0x801fe00c));
    // game-side commands (invtest/bgm/bgmstop/seqsolo/musictest) — game classes / Tomba guest addrs,
    // dispatched into game/core/repl_commands.cpp so the framework REPL names no game type.
    else if (c->hooks->replCommand(c, cmd, line)) { /* handled game-side */ }
    else fprintf(stderr, "[repl] ? %s\n", cmd);
    fflush(stderr);
  }
  return -1;  // EOF
}
