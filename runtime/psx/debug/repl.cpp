// repl.cpp — interactive REPL driver (PSXPORT_REPL=1): read commands from stdin and drive the native
// port (run/step, memory peek/poke, input, screenshots, RAM dumps, entity/scene inspection,
// audio dumps). The product loop (`TitleSession::step`) calls `beforeStep`/`afterStep` around each
// frame; the title's FrameDriver consumes the class Repl's auto-drive requests and can ask the loop
// to return to the prompt after the current frame.
#include "repl.h"
#include "command_args.h"
#include "config.h"                 // `cvars` / `cvar` — the layered CVar registry + env audit
#include "control_surface_limits.h" // kMaxControlReadWords — the one home for the cap both surfaces obey
#include "core.h"
#include "display_presenter.h"
#include "fmv_decode.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_vk.h"
#include "gpu_vk_internal.h"
#include "ot_attr.h"          // OtAttr — `otattr` command (OT/GTE submission attribution)
#include "render_substrate.h" // RenderMode — the per-Core render path
#include "title_command.h"
#include "watchdog.h"
#include <lucent/log.h>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

// ---- Interactive REPL (PSXPORT_REPL=1) — drive the native port from stdin --------------------
// Mirrors the oracle's (wide60rt -repl) command set so one driver can step BOTH cores and diff.
// Commands: run N | r addr [len] | rw addr [words] | w addr val | w8 addr val | watch lo hi |
//   unwatch | hits | press/release <btn> | tap <btn> [frames] | regs | seq | quit | end. Memory is the
//   game's address space (mem_r*/mem_w*); watchpoints via mem_set_watch (reported during `run`).
// ---- REPL music-dump helpers (build a labeled track library to identify each tune) -------
// Sequenced BGM is rendered through the live SPU (use `wav PATH` then `bgm N` then `run`);
// XA tracks (CD-streamed music/voice) are decoded straight off the disc here, since they
// never touch the sequencer. Both write standard 44100/native-rate stereo S16 WAVs.

void repl_wav_write(const char *path, const int16_t *pcm, uint32_t frames, int rate) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    lucent::error("repl", "wav write: cannot open {}", path);
    return;
  }
  uint32_t data = frames * 4u, riff = 36u + data, brate = (uint32_t)rate * 4u, fmtlen = 16;
  uint16_t pcm1 = 1, ch2 = 2, ba = 4, bits = 16;
  uint32_t r = (uint32_t)rate;
  fwrite("RIFF", 1, 4, fp);
  fwrite(&riff, 4, 1, fp);
  fwrite("WAVE", 1, 4, fp);
  fwrite("fmt ", 1, 4, fp);
  fwrite(&fmtlen, 4, 1, fp);
  fwrite(&pcm1, 2, 1, fp);
  fwrite(&ch2, 2, 1, fp);
  fwrite(&r, 4, 1, fp);
  fwrite(&brate, 4, 1, fp);
  fwrite(&ba, 2, 1, fp);
  fwrite(&bits, 2, 1, fp);
  fwrite("data", 1, 4, fp);
  fwrite(&data, 4, 1, fp);
  fwrite(pcm, 4, frames, fp);
  fclose(fp);
  lucent::info("repl", "xadump -> {} ({} frames @ {} Hz, {:.2f}s)", path, frames, rate, frames / (double)rate);
}

// An XA stream to dump: its subheader channel, first CHD sector and how many seconds to decode.
struct XaDumpRequest {
  uint8_t chan;
  uint32_t start_lba;
  int secs;
};

// Write a WAV of the request at the stream's native rate. Skips interleaved non-matching/non-audio sectors.
void repl_xadump(psx::cd::Disc *disc, XaDumpRequest request, const char *path) {
  const uint8_t chan = request.chan;
  const uint32_t start_lba = request.start_lba;
  const int secs = request.secs;
  int16_t *out = (int16_t *)calloc(400000, sizeof(int16_t)); // ~4.5s of 44100 stereo; XA max 37800*secs
  if (!out) {
    return;
  }
  uint8_t raw[2352];
  int16_t hist[2][2] = {{0, 0}, {0, 0}};
  int freq = 37800;
  uint32_t frames = 0, lba = start_lba, cap = 0;
  for (int guard = 0; guard < 20000; guard++) {
    if (!disc->readRaw(lba, raw, 2352)) {
      break;
    }
    if (raw[15] != 2) {
      break; // ran off the Mode2 stream
    }
    uint8_t fchan = raw[17], submode = raw[18];
    lba++;
    if (!(submode & 0x04) || fchan != chan) {
      if (submode & 0x80) {
        break;
      }
      continue;
    } // not our audio
    int16_t pcm[4032 * 2];
    int f2 = freq;
    int n = psx::movie::decodeXaSector(raw, pcm, hist, &f2);
    freq = f2;
    if (!cap) {
      cap = (uint32_t)freq * (uint32_t)secs;
    }
    for (int i = 0; i < n && frames < cap && frames < 200000; i++) {
      out[std::size_t{2} * frames] = pcm[std::size_t{2} * i];
      out[std::size_t{2} * frames + 1] = pcm[std::size_t{2} * i + 1];
      frames++;
    }
    if (frames >= cap || (submode & 0x80)) {
      break;
    }
  }
  repl_wav_write(path, out, frames, freq);
  free(out);
}

} // namespace

// REPL auto-drive state (navNewgame / skipFrames) lives on class Repl (repl.h) —
// arm here on the appropriate command; the title's FrameDriver consumes it on subsequent frames.

// Read+execute REPL commands until a `run N` (returns N), `quit`/EOF (returns -1) or `end`
// (returns -2).
//
// TWO WAYS TO STOP, because they mean different things and conflating them cost a measurement.
// `quit` DETACHES the REPL and lets the game keep running — what an operator wants when they are
// done typing at a windowed session. A headless CAPTURE wants the opposite: the run to END, so the
// port's run-end reporters (the producer DB, the census denominators, every "did this body actually
// run" counter) get to print. Under `quit` the harness has to SIGKILL the process instead, and the
// log then stops mid-run with those questions unanswered — which is how a screenshot pair can be
// compared while nobody can say whether the code under test executed at all.
//
// So `end` is the clean-shutdown request. The host decides what that means (a port with a run
// lifecycle ends it and exits; one without can treat it as quit) — the REPL only reports the ask.
namespace {

class WatchdogInputWait {
public:
  WatchdogInputWait() {
    psx::platform::Watchdog::suspend();
  }
  ~WatchdogInputWait() {
    psx::platform::Watchdog::resume();
  }
  WatchdogInputWait(const WatchdogInputWait &) = delete;
  WatchdogInputWait &operator=(const WatchdogInputWait &) = delete;
};

// The REPL answers only through Lucent, one line per reply line.
bool runTitleCommand(Core &core, const char *line) {
  const psx::game::TitleCommandReply reply = psx::game::runTitleCommand(core, line);
  std::string_view rest(reply.text);
  while (!rest.empty()) {
    const size_t end = rest.find('\n');
    lucent::info("repl", "{}", rest.substr(0, end));
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
  }
  return reply.handled;
}

} // namespace

bool Repl::readStdinLine(std::span<char> line) {
  return fgets(line.data(), static_cast<int>(line.size()), stdin) != nullptr;
}

bool Repl::beforeStep(Core *c, uint32_t step, LineReader readLine) {
  while (budget_ <= 0) {
    budget_ = read(c, step, readLine);
    if (budget_ < 0) {
      return false;
    }
  }
  --budget_;
  return true;
}

long Repl::read(Core *c, uint32_t f, LineReader readLine) {
  uint16_t &held = mHeldMask; // active-low held mask (persists across REPL entries)
  char line[256];
  lucent::info("repl", "frame={} ready", f);
  while (true) {
    bool receivedLine;
    {
      // The REPL owns this intentional idle even when a title calls it from inside a host turn.
      // Resume before executing commands so a command that stalls still gets diagnosed.
      const WatchdogInputWait wait;
      receivedLine = readLine(std::span<char>{line});
    }
    if (!receivedLine) {
      break;
    }
    const psx::debug::CommandArgs args(line);
    const std::string cmd = args.text(0);
    const std::optional<uint32_t> hex1 = args.hex(1);
    const std::optional<uint32_t> hex2 = args.hex(2);
    const std::optional<uint32_t> number1 = args.number(1);
    const std::optional<uint32_t> number2 = args.number(2);
    if (cmd.empty()) {
      continue;
    }
    if (cmd == "quit" || cmd == "q") {
      return -1;
    } else if (cmd == "end") {
      return -2; // END the run cleanly; see the note above
    } else if (cmd == "run" && number1.has_value()) {
      return static_cast<long>(*number1);
    }
    // `step [n]` — advance n frames (default 1) and return to the prompt. Same mechanism as `run n`
    // (the REPL only resumes the frame loop by returning a frame count); provided because `step` is the
    // command everyone reaches for when single-stepping, and its ABSENCE previously read as "the game is
    // frozen" (the unknown command just reprinted the prompt, advancing nothing).
    else if (cmd == "step") {
      const unsigned frames = number1.value_or(0);
      return frames != 0 ? static_cast<long>(frames) : 1;
    } else if (cmd == "r" && hex1.has_value()) {
      const unsigned a = *hex1;
      unsigned b = number2.value_or(0);
      if (!b) {
        b = 16;
      }
      lucent::Line ln;
      ln.add("{:08X}:", a);
      for (unsigned i = 0; i < b && i < 256; i++) {
        ln.add(" {:02X}", c->mem_r8(a + i));
      }
      ln.flush(lucent::Level::Info, "repl");
    } else if (cmd == "rw" && hex1.has_value()) {
      const unsigned a = *hex1;
      unsigned b = number2.value_or(0);
      if (!b) {
        b = 8;
      }
      // Same cap and the same obligation as the live endpoint, from the same header — see
      // control_surface_limits.h for why two transports must not each carry their own literal.
      const unsigned asked = b;
      const unsigned served = b > psx::control::kMaxControlReadWords ? psx::control::kMaxControlReadWords : b;
      lucent::Line ln;
      ln.add("{:08X}:", a);
      for (unsigned i = 0; i < served; i++) {
        ln.add(" {:08X}", c->mem_r32(a + i * 4));
      }
      ln.flush(lucent::Level::Info, "repl");
      if (served < asked) {
        lucent::warn("repl",
                     "[rw] SHORT ANSWER: served {} of {} word(s) requested; this surface returns at "
                     "most {} word(s) per command and the REST WAS NOT READ. Words from 0x{:08X} to "
                     "0x{:08X} are NOT zero — they were not fetched. Issue {} more `rw` command(s).",
                     served,
                     asked,
                     psx::control::kMaxControlReadWords,
                     a + served * 4u,
                     a + asked * 4u - 4u,
                     (asked - served + psx::control::kMaxControlReadWords - 1u) / psx::control::kMaxControlReadWords);
      }
    } else if (cmd == "w" && hex1.has_value() && hex2.has_value()) {
      c->mem_w32(*hex1, *hex2);
      lucent::info("repl", "ok");
    } else if (cmd == "w8" && hex1.has_value() && hex2.has_value()) {
      c->mem_w8(*hex1, static_cast<uint8_t>(*hex2));
      lucent::info("repl", "ok");
    } else if (cmd == "watch" && hex1.has_value() && hex2.has_value()) {
      c->mem_set_watch({*hex1, *hex2});
    } else if (cmd == "unwatch") {
      c->mem_set_watch({});
      lucent::info("repl", "unwatch");
    } else if (cmd == "hits") {
      lucent::info("repl", "watch hits={}", c->mem_watch_hits());
    } else if (cmd == "press" && args.size() > 1) {
      held &= ~psx::debug::padButtonBit(args.word(1));
      c->game->pad.driveHold(held);
      lucent::info("repl", "held={:04X}", held);
    } else if (cmd == "release" && args.size() > 1) {
      held |= psx::debug::padButtonBit(args.word(1));
      c->game->pad.driveHold(held);
      lucent::info("repl", "held={:04X}", held);
    } else if (cmd == "tap" && args.size() > 1) {
      unsigned a = number2.value_or(0);
      if (!a) {
        a = 4;
      }
      c->game->pad.driveTap({.activeLowMask = static_cast<uint16_t>(0xFFFF & ~psx::debug::padButtonBit(args.word(1))),
                             .frames = static_cast<int>(a)});
      lucent::info("repl", "tap {} {}", args.word(1), a);
    } else if (cmd == "debug") {
      const std::string ch(args.rest(1));
      lucent::enable_channels(ch);
      // ...and record it on PSXPORT_DEBUG's CVar at the RUNTIME layer. That layer exists precisely
      // to give this a name: docs/config.md has always said a REPL `debug` "still overrides the
      // environment for the rest of the run", and that was the only sentence about precedence
      // anywhere in the repo. It is a layer now, and `cvars` will show it. The VALUE is still
      // resolved by lucent (the CVar is marked external) — nothing about who reads PSXPORT_DEBUG
      // changes here; see cmake/psxport.cmake and tests/test_lucent_channel_env.cpp.
      psx::config::note_runtime_debug_channels(ch);
      lucent::info("repl", "debug channels = {}", ch.empty() ? "(none)" : ch);
    } else if (cmd == "cvars") {
      psx::config::report();
    } else if (cmd == "cvar") {
      const std::string nm = args.text(1);
      const std::string val(args.rest(2));
      if (!val.empty()) {
        psx::config::set_runtime(nm.c_str(), val.c_str());
      } else if (!nm.empty()) {
        psx::config::clear_runtime(nm.c_str());
      } else {
        lucent::info(
            "repl",
            "cvar <PSXPORT_NAME> <value> — set for this run only; `cvar <name>` clears it; `cvars` lists everything");
      }
    } else if (cmd == "newgame") {
      this->navNewgame = 1;
      lucent::info("repl", "newgame: pulsing to GAME prologue");
      return 100000;
    } else if (cmd == "skip") {
      unsigned a = number1.value_or(0);
      if (!a) {
        a = 500;
      }
      this->skipFrames = (long)a;
      lucent::info("repl", "skip {} frames", a);
      return (long)a;
    } else if (cmd == "preseq") { // arm a PRESENT-sequence dump: next N presented frames (real + fps60 interp)
      const unsigned n = number1.value_or(0);
      const std::string dir = args.size() > 2 ? args.text(2) : std::string("scratch/screenshots/preseq");
      if (n > 0) {
        c->game->gpuVk().preseq_arm(static_cast<int>(n), dir.c_str());
        lucent::info("repl", "preseq armed: next {} presents -> {}/p%04d.ppm", n, dir);
      } else {
        lucent::info("repl", "preseq <N> [dir] — dump the next N PRESENTED frames (incl. fps60 interp)");
      }
    } else if (cmd == "shot") { // VK-aware, same pick as dbg_server's `shot`: capture what is PRESENTED
      const std::string path = args.text(1);
      if (!path.empty()) {
        c->game->display.shot(path.c_str());
        // The scanned-row count: which rows of this capture a console would have shown.
        lucent::info("repl",
                     "shot -> {}   [render path = {}] guest_scan={}",
                     path,
                     psx::gpu::renderPathName(c->rsub.mode.path()),
                     psx::present::DisplayPresenter::presentedHeight(*c, c->gpuDevice().displayArea().height));
      }
    } else if (cmd == "census") { // the producer census so far, rows ranked by unkeyed primitives
      c->rsub.census.report("census");
    } else if (cmd == "shotregion") { // dump an ARBITRARY VRAM region, not just what is presented
      // `shotregion v.ppm 0 0 1024 512` dumps the whole of VRAM, so a black present can be told apart from a black
      // VRAM.
      const std::optional<int> x = args.integer(2);
      const std::optional<int> y = args.integer(3);
      const std::optional<int> w = args.integer(4);
      const std::optional<int> h = args.integer(5);
      if (x.has_value() && y.has_value() && w.has_value() && h.has_value()) {
        psx::gpu::vramRegion(*c, args.text(1).c_str(), {*x, *y, *w, *h});
      } else {
        lucent::info("repl", "shotregion <path> <x> <y> <w> <h> — raw VRAM region (VRAM is 1024x512)");
      }
    } else if (cmd == "setires") { // live ires toggle (same mods.ires mutation the RmlUi overlay's
      // "ires" row does — 0=Auto, 1..cap=fixed). Exercises GpuVkState::ensure_ires_targets' teardown+
      // rebuild path headless, without needing a windowed run to reach the overlay.
      if (const std::optional<unsigned> n = args.number(1)) {
        c->game->mods.ires = static_cast<int>(*n);
        lucent::info("repl", "mods.ires = {}", c->game->mods.ires);
      } else {
        lucent::info("repl", "setires <0..4> — live ires toggle (0=Auto)");
      }
    } else if (cmd == "vram") {
      const std::string path = args.text(1);
      if (!path.empty()) {
        psx::gpu::vramRegion(*c,
                             path.c_str(),
                             {static_cast<int>(number2.value_or(0)),
                              static_cast<int>(args.number(3).value_or(0)),
                              static_cast<int>(args.number(4).value_or(1024)),
                              static_cast<int>(args.number(5).value_or(512))});
      }
    } else if (cmd == "vramraw") {
      const std::string path = args.text(1);
      if (!path.empty()) {
        psx::gpu::vramRaw(*c, path.c_str());
      }
    } else if (cmd == "dumpram") {
      const std::string path = args.text(1);
      if (!path.empty()) {
        FILE *fp = fopen(path.c_str(), "wb");
        if (fp) {
          fwrite(c->ram, 1, 0x200000, fp);
          fclose(fp);
          lucent::info("repl", "dumpram -> {}", path);
        } else {
          lucent::error("repl", "dumpram: cannot open {}", path);
        }
        // Also dump the 1 KB scratchpad (0x1F800000) to a sidecar .spad — the main-RAM A/B diff is
        // BLIND to scratchpad, where several engine flags live (DEMO: 0x1f80019a/19d/134/198 etc.).
        const std::string spath = path + ".spad";
        FILE *sp = fopen(spath.c_str(), "wb");
        if (sp) {
          fwrite(c->scratch, 1, sizeof c->scratch, sp);
          fclose(sp);
          lucent::info("repl", "dumpram scratchpad -> {}", spath);
        }
      }
    } else if (cmd == "wav") {
      const std::string path = args.text(1);
      if (!path.empty()) {
        c->game->spu_audio.wavReopen(path.c_str());
      }
    }
    // renderpath [record|device] — switch the render path live; bare prints it.
    else if (cmd == "renderpath") {
      const std::string st = args.text(1);
      if (!st.empty()) {
        RenderPath p = c->rsub.mode.path();
        if (!psx::gpu::parseRenderPath(st.c_str(), &p)) {
          lucent::warn("repl",
                       "renderpath: '{}' is not a path — valid: record | device (unchanged: {})",
                       st,
                       psx::gpu::renderPathName(c->rsub.mode.path()));
          continue;
        }
        c->rsub.mode.setPath(p);
      }
      lucent::info("repl", "render path = {}", psx::gpu::renderPathName(c->rsub.mode.path()));
    } else if (cmd == "xadump") {
      const std::optional<unsigned> ch = args.number(1);
      const std::optional<unsigned> lba = args.number(2);
      const unsigned secs = args.number(4).value_or(3);
      if (ch.has_value() && lba.has_value() && args.size() > 3) {
        repl_xadump(
            &c->game->disc,
            {.chan = static_cast<uint8_t>(*ch), .start_lba = *lba, .secs = secs != 0 ? static_cast<int>(secs) : 3},
            args.text(3).c_str());
      }
    }
    // otattr — dump the OT/GTE submission-attribution tables (game/render/ot_attr.h, `debug otattr`).
    // Re-walks the LAST OT this Core drew (RenderSubstrate::lastOrderingTable) READ-ONLY (header/first-word peeks
    // only — no gpu_gp0() call, so no draw/state side effects), and for each non-empty node looks up
    // the packet-pool store span attribution recorded during that frame's submission: {emitter fn = the
    // otattr shadow-stack top, caller fn = one frame below it}. Only produces attribution if `debug otattr` was ALREADY
    // ON while the frame that built this OT ran — turn the channel on BEFORE the frame you want to inspect, then run
    // this after. The report includes raw guest addresses; resolve them through the active title's symbol index.
    else if (cmd == "otattr") {
      OtAttr &oa = c->rsub.otAttr;
      const std::string_view sub = args.word(1);
      // The last-writer sub-commands live in the otattr owner (ot_attr.cpp) so every transport
      // shares one implementation; see there.
      if (oa.runLastWriterCommand(line, [](std::string_view m) {
            lucent::info("otattr", "{}", m);
          })) {
        continue;
      }
      if (sub == "trace") {
        const std::optional<uint32_t> traced = args.hex(2);
        if (!traced.has_value()) {
          lucent::info("otattr", "usage: otattr trace <addr-hex>");
          continue;
        }
        const uint32_t addr = *traced;
        OtAttr::WordRec rec{};
        uint32_t wa = 0;
        if (!oa.watchLookup(addr, &rec, &wa)) {
          lucent::info("otattr", "trace 0x{:08X}: NOT in any watched region — `otattr watch <addr> <len>` first", addr);
          continue;
        }
        if (rec.frame == 0xFFFFFFFFu) {
          lucent::info("otattr", "trace 0x{:08X}: watched, but NEVER WRITTEN since registration", addr);
          continue;
        }
        lucent::info("otattr",
                     "trace 0x{:08X} (word 0x{:08X}): last writer fn=0x{:08X} caller=0x{:08X} frame={}",
                     addr,
                     wa,
                     rec.fn,
                     rec.caller,
                     rec.frame);
        // One-hop heuristic: does the writer LOOK like a copy loop (touches many distinct 4KB pages in
        // the same frame, i.e. it fans out across many destinations rather than emitting one thing)?
        const OtAttr::FnStoreStat *st = oa.fnStatFind(rec.fn);
        if (!st) {
          lucent::warn("repl",
                       "  [trace] no per-fn store stat recorded for this fn this frame (stale/cross-frame lookup) — "
                       "re-run `otattr trace` in the same frame the write happened.");
        } else {
          bool looksLikeCopyLoop = st->pageOverflow || st->pageCount >= 3;
          lucent::info("repl",
                       "  [trace] fn=0x{:08X} made {} store(s) touching {} distinct 4KB page(s) this frame{} -> {}",
                       rec.fn,
                       st->count,
                       st->pageCount,
                       st->pageOverflow ? "+" : "",
                       looksLikeCopyLoop
                           ? "LOOKS LIKE A COPY/BATCH LOOP (writes fan out across many destinations; "
                             "the object identity was likely already lost by the time it reached this word)"
                           : "looks like a direct, single-destination writer");
          lucent::info(
              "repl",
              "  [trace] SOURCE not determinable from store data alone (this tool only sees "
              "writes, not reads) — decompile fn=0x{:08X} with the maintained Ghidra workflow to find its read "
              "pointer/source struct, then `otattr watch <src_addr> <len>` and re-run `otattr who`/`trace` on "
              "the source to walk one more hop back.",
              rec.fn);
        }
        continue;
      }
      lucent::info(
          "otattr",
          "frame={} spans={}(overflow={}) gteBuckets={}(overflow={}) watch: slots={}/{} words={}/{}(overflow={})",
          oa.frame(),
          oa.spanCount(),
          oa.spanOverflow(),
          oa.gteCount(),
          oa.gteOverflow(),
          oa.watchSlotCount(),
          (int)OtAttr::WATCH_SLOTS,
          oa.watchWordsUsed(),
          (int)OtAttr::WATCH_CAP_WORDS,
          oa.watchOverflow());
      uint32_t madr = c->rsub.lastOrderingTable;
      if (!madr) {
        lucent::info("otattr", "no OT walked yet this run");
      } else {
        uint32_t a = madr & 0x1FFFFC;
        for (int idx = 0; idx < 0x10000; idx++) {
          uint32_t hdr = c->mem_r32(a);
          unsigned n = hdr >> 24; // prim GP0-word count (tag high byte) — 0 == link-only sentinel node
          if (n > 0) {
            uint32_t w0 = c->mem_r32(a + 4);
            uint8_t op = (uint8_t)(w0 >> 24);
            int vx = 0, vy = 0;
            if (n >= 2) {
              uint32_t w1 = c->mem_r32(a + 8);
              vx = (int16_t)(w1 & 0xFFFF);
              vy = (int16_t)(w1 >> 16);
            }
            OtAttr::Span sp{};
            bool found = oa.lookupStore(a, &sp);
            lucent::info("repl",
                         "  [{:4}] pool=0x{:08X} op=0x{:02X} n={} v0=({},{})  fn=0x{:08X} caller=0x{:08X}",
                         idx,
                         0x80000000u | a,
                         op,
                         n,
                         vx,
                         vy,
                         found ? sp.fn : 0,
                         found ? sp.caller : 0);
          }
          uint32_t next = hdr & 0xFFFFFF;
          if (next == 0xFFFFFF || next == 0) {
            break;
          }
          a = next & 0x1FFFFC;
        }
      }
      lucent::info("otattr", "GTE RTPS/RTPT per-fn call counts this frame:");
      for (int i = 0; i < oa.gteCount(); i++) {
        const OtAttr::GteBucket *g = oa.gteAt(i);
        lucent::info("repl", "  fn=0x{:08X} count={}", g->fn, g->count);
      }
    }
    // otwhere <pkt-hex> <ot-base-hex> [entries] — locate a packet's OT BUCKET in the LAST-walked OT
    // (read-only chain walk from RenderSubstrate::lastOrderingTable, the same enumeration DrawOTag used; no draw/state
    // side effects). Reports {bucket index, position within the bucket, global chain position}. The OT
    // ARRAY base/entry-count are game data the framework does not know, so the caller supplies them
    // (Tomba2: base = *0x800ED8C8 via `r 800ED8C8`, entries = 2048 — the guest submitter's own range
    // check clamps indices to [4,2048)). Bucket = index of the last OT-array sentinel word the walk
    // passed; packets hang off their bucket's head word, so every node between sentinel k and the next
    // sentinel belongs to bucket k. Diagnostic for the kanban #11 class (which game sort key did the
    // emitter give a prim); shipping-path OT reads remain banned — this is REPL-only.
    else if (cmd == "otwhere") {
      const uint32_t pkt = hex1.value_or(0);
      const uint32_t base = hex2.value_or(0);
      const int entries = args.integer(3).value_or(2048);
      if (!hex1.has_value() || !hex2.has_value()) {
        lucent::info("repl", "usage: otwhere <pkt-hex> <ot-base-hex> [entries=2048]");
      } else {
        uint32_t madr = c->rsub.lastOrderingTable;
        if (!madr) {
          lucent::info("repl", "otwhere: no OT walked yet");
          continue;
        }
        uint32_t a = madr & 0x1FFFFC, want = pkt & 0x1FFFFC;
        uint32_t blo = base & 0x1FFFFF, bhi = blo + (uint32_t)entries * 4;
        int bucket = -1, pos = 0, hits = 0;
        for (int idx = 0; idx < 0x10000; idx++) {
          if (a >= blo && a < bhi && ((a - blo) & 3) == 0) {
            bucket = (int)((a - blo) >> 2);
            pos = 0;
          } else {
            pos++;
          }
          uint32_t hdr = c->mem_r32(a);
          if (a == want) {
            lucent::info("repl",
                         "otwhere 0x{:08X}: bucket={} posInBucket={} chainIdx={} hdr=0x{:08X} (len={} next=0x{:06X})",
                         0x80000000u | a,
                         bucket,
                         pos,
                         idx,
                         hdr,
                         hdr >> 24,
                         hdr & 0xFFFFFF);
            hits++;
          }
          uint32_t next = hdr & 0xFFFFFF;
          if (next == 0xFFFFFF || next == 0) {
            break;
          }
          a = next & 0x1FFFFC;
        }
        if (!hits) {
          lucent::info("repl", "otwhere 0x{:08X}: NOT in the last-walked OT (madr=0x{:08X})", pkt, 0x80000000u | madr);
        }
      }
    }
    // `bugreport <summary>` — the B key without a keyboard: the report is taken at the next frame
    // boundaries exactly as a keypress takes it, and its form is filled with <summary> and saved.
    else if (cmd == "bugreport") {
      std::string summary(args.rest(1));
      if (summary.empty()) {
        lucent::info("repl", "bugreport <summary>");
      } else {
        c->game->bugReport.requestScripted(std::move(summary));
        lucent::info("repl", "bugreport: armed; it is taken over the next two frames");
      }
    }
    // `menu [on|off|toggle]` — drive the RmlUi overlay without a keyboard.
    //
    // WHY THIS EXISTS: the overlay could previously only be opened by an SDL ESC key event, i.e.
    // only through a window. Agents may not run windowed (docs/workspace/PROTOCOL.md), so the entire UI was
    // undrivable by every instrument the project actually uses, and that — together with the
    // windowed-only init this replaced — is why a user-reported dead overlay could not be examined
    // at all. Visibility is host UI state, not guest state, so this is a driving surface like
    // `press`/`tap`, not a behaviour switch. Reports what it DID, including the refusal cases, so
    // a run that changed nothing cannot read like a run that worked.
    else if (cmd == "menu") {
      const std::string_view which = args.word(1);
      RmlOverlay &ov = c->game->rml_overlay;
      if (!ov.inited()) {
        lucent::info("repl", "menu: overlay NOT initialised — nothing to show");
      } else if (!ov.hasMenu()) {
        lucent::info("repl", "menu: overlay is up but has NO DOCUMENT (LoadDocument failed; check PSXPORT_ASSET_DIR)");
      }
      // `dump`, `tab <n>` and `nav <key>` make the menu's CONTENT and NAVIGATION reachable without a
      // window. The menu is driven by SDL keyboard events, which do not exist headless, so before
      // these the component tree could be brought up but never exercised — visibility was the only
      // thing an agent could change, and "does the Graphics tab still have its 13 rows?" had no
      // answer short of a windowed run. They are a DRIVING surface (like press/tap): host UI state
      // only, no guest state, no behaviour switch.
      else if (which == "dump") {
        ov.dumpMenu();
      } else if (which == "tab") {
        if (const std::optional<int> n = args.integer(2)) {
          ov.selectTab(*n);
          lucent::info("repl", "menu: tab {}", *n);
        } else {
          lucent::info("repl", "menu tab <index>");
        }
      } else if (which == "nav") {
        const std::string_view key = args.word(2);
        const SDL_Keycode code = key == "up"      ? SDLK_UP
                                 : key == "down"  ? SDLK_DOWN
                                 : key == "left"  ? SDLK_LEFT
                                 : key == "right" ? SDLK_RIGHT
                                 : key == "enter" ? SDLK_RETURN
                                                  : SDLK_UNKNOWN;
        // A key name nobody recognises must SAY so. A no-op that logged nothing would look
        // identical to a nav that ran and changed nothing — the difference matters when the whole
        // point of the command is to tell those two apart.
        if (code == SDLK_UNKNOWN) {
          lucent::info("repl", "menu nav <up|down|left|right|enter> — \"{}\" is not one of them", key);
        } else if (!ov.visible()) {
          lucent::info("repl", "menu nav {}: IGNORED, the menu is hidden (`menu on` first)", key);
        } else {
          lucent::info("repl", "menu nav {}: {}", key, ov.sendKey(code) ? "handled" : "NOT handled by the menu");
        }
      } else {
        const bool on = which == "off" ? false : which == "on" ? true : !ov.visible(); // bare `menu` / `menu toggle`
        ov.setVisible(on);
        lucent::info("repl", "menu: {}", on ? "shown" : "hidden");
      }
    } else if (cmd == "regs") {
      lucent::Line ln; // 4 registers per row
      for (int i = 0; i < 32; i++) {
        ln.add(" r{:<2}={:08X}", i, c->r[i]);
        if ((i & 3) == 3) {
          ln.flush(lucent::Level::Info, "repl");
        }
      }
      lucent::info("repl", " hi={:08X} lo={:08X}", c->hi, c->lo);
    } else if (runTitleCommand(*c, line)) {
    } else {
      lucent::info("repl", "? {}", cmd);
    }
  }
  return -1; // EOF
}
