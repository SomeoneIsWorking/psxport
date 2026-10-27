#include "debug_commands.h"

#include "beetle_device_state.h"
#include "config.h"
#include "control_surface_limits.h"
#include "core.h"
#include "display_presenter.h"
#include "execution_control.h"
#include "execution_ledger.h"
#include "fs_util.h"
#include "game.h"
#include "gpu_debug.h"
#include "gpu_device.h"
#include "guest_call.h"
#include "input_injection.h"
#include "lightrec_executor.h"
#include "machine_state.h"
#include "ordering_table.h"
#include "ot_attr.h"
#include "render.h"
#include "render_device.h"
#include "render_mode.h"
#include "render_state.h"
#include "snapshot.h"
#include "spu_field_cadence.h"
#include "title_debug_menu.h"
#include "wav_writer.h"
#include "xa_state.h"

#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <strings.h>
#include <vector>

namespace psx::debug {
namespace {

using Call = DebugCommands::Call;

struct Verb {
  std::string_view name;
  std::string_view usage;
  std::string_view summary;
  void (*run)(const Call &);
};

void readBytes(const Call &call) {
  const std::optional<std::uint32_t> address = call.args.hex(1);
  if (!address) {
    call.reply("usage: r <hex> [n]");
    return;
  }
  const std::uint32_t count = std::min<std::uint32_t>(call.args.number(2).value_or(16), 256u);
  if (!call.core.isGuestStorage(*address, count)) {
    call.reply(lucent::format("[r] refused: {:08X}+{} is not guest RAM or scratchpad", *address, count));
    return;
  }
  std::string line = lucent::format("{:08X}:", *address);
  for (std::uint32_t i = 0; i < count; i++) {
    line += lucent::format(" {:02X}", call.core.mem_r8(*address + i));
  }
  call.reply(line);
}

void readWords(const Call &call) {
  const std::optional<std::uint32_t> address = call.args.hex(1);
  if (!address) {
    call.reply("usage: rw <hex> [n]");
    return;
  }
  const std::uint32_t asked = call.args.number(2).value_or(8);
  const std::uint32_t served = std::min(asked, psx::control::kMaxControlReadWords);
  if (!call.core.isGuestStorage(*address, served * 4u)) {
    call.reply(lucent::format("[rw] refused: {:08X}+{} word(s) is not guest RAM or scratchpad", *address, served));
    return;
  }
  std::string line = lucent::format("{:08X}:", *address);
  for (std::uint32_t i = 0; i < served; i++) {
    line += lucent::format(" {:08X}", call.core.mem_r32(*address + i * 4u));
  }
  call.reply(line);
  if (served < asked) {
    call.reply(lucent::format("[rw] SHORT ANSWER: served {} of {} word(s); 0x{:08X}..0x{:08X} were NOT read",
                              served,
                              asked,
                              *address + served * 4u,
                              *address + asked * 4u - 4u));
  }
}

template <unsigned Bytes> void writeMemory(const Call &call) {
  const std::optional<std::uint32_t> address = call.args.hex(1);
  const std::optional<std::uint32_t> value = call.args.hex(2);
  if (!address || !value) {
    call.reply(lucent::format("usage: w{} <hex-address> <hex-value>", Bytes * 8u));
    return;
  }
  if constexpr (Bytes == 1) {
    call.core.mem_w8(*address, static_cast<std::uint8_t>(*value));
    call.reply(lucent::format("[{:08X}] <- {:02X}", *address, *value & 0xFFu));
  } else if constexpr (Bytes == 2) {
    call.core.mem_w16(*address, static_cast<std::uint16_t>(*value));
    call.reply(lucent::format("[{:08X}] <- {:04X}", *address, *value & 0xFFFFu));
  } else {
    call.core.mem_w32(*address, *value);
    call.reply(lucent::format("[{:08X}] <- {:08X}", *address, *value));
  }
}

// Side effects are real: the callee runs on the live machine at this frame boundary.
void callGuest(const Call &call) {
  const std::optional<std::uint32_t> entry = call.args.hex(1);
  if (!entry) {
    call.reply("usage: call <hex> [a0..a3]");
    return;
  }
  std::array<std::uint32_t, 4> arguments{};
  for (std::size_t i = 0; i < arguments.size(); i++) {
    const std::optional<std::uint32_t> value = call.args.hex(2 + i);
    if (!value) {
      break;
    }
    arguments[i] = *value;
  }
  Core &core = call.core;
  const std::array<std::uint32_t, 4> saved{core.r[4], core.r[5], core.r[6], core.r[7]};
  const auto result =
      psx::cpu::dispatchGuestWithArguments(core, *entry, arguments, psx::cpu::ExecutionBudget::currentTurn(core));
  if (result.returned()) {
    call.reply(lucent::format("call {:08X}(a0={:08X},a1={:08X},a2={:08X},a3={:08X}) -> v0={:08X} v1={:08X}",
                              *entry,
                              arguments[0],
                              arguments[1],
                              arguments[2],
                              arguments[3],
                              core.r[2],
                              core.r[3]));
  } else {
    call.reply(lucent::format("call {:08X} exited {} at {:08X} after {} cycles: {}",
                              *entry,
                              psx::cpu::executionExitName(result.reason),
                              result.guestPc,
                              result.cycles,
                              result.detail));
  }
  for (std::size_t i = 0; i < saved.size(); i++) {
    core.r[4 + i] = saved[i];
  }
  psx::cpu::completeOrPropagate(core, result);
}

void registers(const Call &call) {
  for (int row = 0; row < 32; row += 4) {
    call.reply(lucent::format(" r{:<2}={:08X} r{:<2}={:08X} r{:<2}={:08X} r{:<2}={:08X}",
                              row,
                              call.core.r[row],
                              row + 1,
                              call.core.r[row + 1],
                              row + 2,
                              call.core.r[row + 2],
                              row + 3,
                              call.core.r[row + 3]));
  }
  call.reply(lucent::format(" hi={:08X} lo={:08X}", call.core.hi, call.core.lo));
}

void watch(const Call &call) {
  const std::optional<std::uint32_t> lo = call.args.hex(1);
  const std::optional<std::uint32_t> hi = call.args.hex(2);
  if (!lo || !hi) {
    call.reply("usage: watch <lo-hex> <hi-hex>");
    return;
  }
  call.core.storeWatch.arm({*lo, *hi});
  call.reply(lucent::format("watch [{:08X},{:08X})", *lo, *hi));
}

void unwatch(const Call &call) {
  call.core.storeWatch.arm({});
  call.reply("unwatch");
}

void watchHits(const Call &call) {
  call.reply(lucent::format("watch hits={}", call.core.storeWatch.hits()));
}

// About a human tap: a tenth of a second of input turns.
inline constexpr int kDefaultHoldTurns = 6;

void press(const Call &call) {
  if (call.args.size() < 2) {
    call.reply("usage: press <button>");
    return;
  }
  call.reply(call.core.game->hostInput.padButtons(padButtonBit(call.args.word(1)), input::InjectedPress::Down, 0));
}

void release(const Call &call) {
  if (call.args.size() < 2) {
    call.reply("usage: release <button>");
    return;
  }
  call.reply(call.core.game->hostInput.padButtons(padButtonBit(call.args.word(1)), input::InjectedPress::Up, 0));
}

// Every button held where the active-low mask clears its bit, every other one let go.
void hold(const Call &call) {
  const std::optional<std::uint32_t> mask = call.args.hex(1);
  if (!mask) {
    call.reply("usage: hold <active-low-hex>");
    return;
  }
  const auto held = static_cast<std::uint16_t>(~*mask);
  const std::string down =
      held != 0 ? call.core.game->hostInput.padButtons(held, input::InjectedPress::Down, 0) : std::string();
  const std::string up =
      call.core.game->hostInput.padButtons(static_cast<std::uint16_t>(~held), input::InjectedPress::Up, 0);
  call.reply(down.empty() ? up : down + "; " + up);
}

void tap(const Call &call) {
  if (call.args.size() < 2) {
    call.reply("usage: tap <button> [turns]");
    return;
  }
  const std::uint32_t turns = call.args.number(2).value_or(0);
  call.reply(call.core.game->hostInput.padButtons(padButtonBit(call.args.word(1)),
                                                  input::InjectedPress::Tap,
                                                  turns != 0 ? static_cast<int>(turns) : kDefaultHoldTurns));
}

// Every frame's mask is kept from frame 0, so a session in progress can be cut into a replay on demand.
void padRecording(const Call &call) {
  const std::size_t have = call.core.game->pad.recordedFrames();
  if (call.args.word(1) != "save" || call.args.size() < 3) {
    call.reply(lucent::format("padrec: {} frames captured; 'padrec save <path> [nframes]' cuts a replay", have));
    return;
  }
  const std::size_t limit = call.args.number(3).value_or(0);
  const std::string path = call.args.text(2);
  const bool ok = call.core.game->pad.saveRecording(path.c_str(), limit);
  call.reply(lucent::format("padrec save -> {} ({} of {} frames){}",
                            path,
                            limit != 0 && limit < have ? limit : have,
                            have,
                            ok ? "" : " FAILED"));
}

void debugChannels(const Call &call) {
  const std::string channels(call.args.rest(1));
  lucent::enable_channels(channels);
  psx::config::note_runtime_debug_channels(channels);
  call.reply(lucent::format("debug channels = {}", channels.empty() ? "(none)" : channels));
}

void cvars(const Call &call) {
  for (const std::string &line : psx::config::cvar_report_lines(true)) {
    call.reply(line);
  }
}

void cvar(const Call &call) {
  const std::string name = call.args.text(1);
  const std::string value(call.args.rest(2));
  if (!value.empty()) {
    call.reply(
        lucent::format("cvar {} = {} -> {}", name, value, psx::config::set_runtime(name, value) ? "ok" : "REJECTED"));
    if (strcasecmp(name.c_str(), "PSXPORT_RENDER_PATH") == 0) {
      call.reply(lucent::format("NOTE: read at boot; `renderpath {}` switches the running renderer", value));
    }
  } else if (!name.empty()) {
    call.reply(lucent::format("cvar {} cleared -> {}", name, psx::config::clear_runtime(name) ? "ok" : "REJECTED"));
  } else {
    call.reply("cvar <PSXPORT_NAME> <value> sets it for this run; `cvar <name>` clears it");
  }
}

void guestLedger(const Call &call) {
  for (const std::string &line : psx::cpu::ledgerLines(call.core.lightrecExecutor().counters())) {
    call.reply(line);
  }
}

void replyLines(const ReplySink &reply, std::string_view text) {
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    reply(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
}

// The owner journal's rows, most intervals lost first; `write` also writes the JSONL now.
void owners(const Call &call) {
  const psx::present::OwnerJournal &journal = call.core.game->ownerJournal;
  if (call.args.word(1) == "write") {
    journal.flush(call.core);
  }
  constexpr std::size_t kDefaultRows = 20;
  const std::size_t limit = call.args.word(1) == "write" ? kDefaultRows : call.args.number(1).value_or(kDefaultRows);
  replyLines(call.reply, journal.describe(limit));
}

void census(const Call &call) {
  call.core.rsub.census.report("census");
  call.reply(g_producer_census_armed ? "census reported to the log" : "the census is off; PSXPORT_PRODUCERS=1 arms it");
}

void renderPath(const Call &call) {
  const std::string name = call.args.text(1);
  if (!name.empty()) {
    psx::present::RenderPath path = call.core.rsub.mode.path();
    if (psx::present::parseRenderPath(name.c_str(), &path)) {
      call.core.rsub.mode.setPath(path);
    } else {
      call.reply(lucent::format("renderpath: '{}' is not a path; valid: record | device", name));
    }
  }
  call.reply(lucent::format("render path = {}", psx::present::renderPathName(call.core.rsub.mode.path())));
}

void setInternalResolution(const Call &call) {
  const std::optional<std::uint32_t> scale = call.args.number(1);
  if (!scale) {
    call.reply("setires <0..4> (0 = auto)");
    return;
  }
  call.core.game->mods.ires = static_cast<int>(*scale);
  call.reply(lucent::format("mods.ires = {}", call.core.game->mods.ires));
}

std::string pathOr(const Call &call, std::size_t index, const char *fallback) {
  return call.args.size() > index ? call.args.text(index) : std::string(fallback);
}

void shot(const Call &call) {
  Core &core = call.core;
  const std::string path = pathOr(call, 1, "scratch/screenshots/dbg.png");
  core.game->display.shot(path.c_str());
  call.reply(lucent::format("shot -> {} [render path = {}] guest_scan={}",
                            path,
                            psx::present::renderPathName(core.rsub.mode.path()),
                            psx::present::DisplayPresenter::presentedRect(core).height));
}

void deviceShot(const Call &call) {
  const std::string path = pathOr(call, 1, "scratch/screenshots/dbg_vk.png");
  call.core.game->render().shot(path.c_str());
  call.reply(lucent::format("vkshot -> {}", path));
}

// The present image the player sees: letterboxed, faded, overlay included.
void presentShot(const Call &call) {
  const std::string path = pathOr(call, 1, "scratch/screenshots/dbg_present.png");
  call.core.game->render().presentShot(path.c_str());
  call.reply(lucent::format("pshot -> {}", path));
}

// The next N presents, real and interpolated: the only headless view of a temporal artefact.
void presentSequence(const Call &call) {
  const std::uint32_t count = call.args.number(1).value_or(0);
  if (count == 0) {
    call.reply("preseq <N> [dir]");
    return;
  }
  const std::string dir = pathOr(call, 2, "scratch/screenshots/preseq");
  call.core.game->render().preseqArm(static_cast<int>(count), dir.c_str());
  call.reply(lucent::format("preseq armed: next {} presents -> {}/p%04d.ppm", count, dir));
}

void vramRegion(const Call &call) {
  if (call.args.size() < 2) {
    call.reply("usage: vram <path> [x y w h] (VRAM is 1024x512)");
    return;
  }
  const std::string path = call.args.text(1);
  const psx::gpu::VramRect rect{static_cast<int>(call.args.number(2).value_or(0)),
                                static_cast<int>(call.args.number(3).value_or(0)),
                                static_cast<int>(call.args.number(4).value_or(1024)),
                                static_cast<int>(call.args.number(5).value_or(512))};
  psx::render::vramRegion(call.core, path.c_str(), rect);
  call.reply(lucent::format("vram ({},{} {}x{}) -> {}", rect.x, rect.y, rect.width, rect.height, path));
}

void vramRaw(const Call &call) {
  if (call.args.size() < 2) {
    call.reply("usage: vramraw <path>");
    return;
  }
  const std::string path = call.args.text(1);
  psx::render::vramRaw(call.core, path.c_str());
  call.reply(lucent::format("vramraw -> {}", path));
}

void vramPixels(const Call &call) {
  const std::uint32_t x = call.args.number(1).value_or(0);
  const std::uint32_t y = call.args.number(2).value_or(0);
  std::array<std::uint16_t, 64> words{};
  const std::uint32_t count = std::min<std::uint32_t>(call.args.number(3).value_or(1), words.size());
  psx::render::vramWords(call.core, {static_cast<int>(x), static_cast<int>(y)}, std::span(words.data(), count));
  std::string line = lucent::format("vkpix ({},{} x{}):", x, y, count);
  for (std::uint32_t i = 0; i < count; i++) {
    line += lucent::format(" {:04X}", words[i]);
  }
  call.reply(line);
}

void dumpRam(const Call &call) {
  const std::string path = pathOr(call, 1, "scratch/bin/dbg_ram.bin");
  call.reply(Snapshotter::writeRam(call.core, path) ? lucent::format("dumpram -> {} (+ .spad)", path)
                                                    : lucent::format("dumpram: cannot write {}", path));
}

void spuWav(const Call &call) {
  if (call.args.size() < 2) {
    call.reply("usage: wav <path>");
    return;
  }
  const std::string path = call.args.text(1);
  call.core.game->spu_audio.wavReopen(path.c_str());
  call.reply(lucent::format("wav -> {}", path));
}

// Beetle's SPU state section and the last field's mix, for the oracle's audio comparison (tools/oracle/spu_state.py).
void spuState(const Call &call) {
  if (call.args.size() < 2) {
    call.reply("usage: spu <path>");
    return;
  }
  const std::string path = call.args.text(1);
  psx::state::BeetleDeviceState state;
  std::string error;
  if (!state.save(call.core, psx::state::BeetleDevice::Spu, error)) {
    call.reply(lucent::format("spu REFUSED: {}", error));
    return;
  }
  const std::vector<std::uint8_t> bytes = state.bytesOut();
  if (!Fs::writeFile(path, bytes.data(), bytes.size(), error)) {
    call.reply(lucent::format("spu REFUSED: {}", error));
    return;
  }
  const SpuOutputSummary &out = call.core.game->spu_audio.lastField();
  call.reply(lucent::format(
      "spu -> {} bytes={} samples={} nonzero={} peak={}", path, bytes.size(), out.samples, out.nonzero, out.peak));
}

// One XA channel of file 1 played off the disc by a clip XaStream, as the SPU would hear it.
void xaDump(const Call &call) {
  const std::optional<std::uint32_t> channel = call.args.number(1);
  const std::optional<std::uint32_t> startLba = call.args.number(2);
  if (!channel || !startLba || call.args.size() < 4) {
    call.reply("usage: xadump <channel> <lba> <path> [seconds]");
    return;
  }
  psx::cd::Disc &disc = call.core.game->disc;
  if (!disc.isOpen()) {
    call.reply("xadump: no disc is open");
    return;
  }
  std::uint32_t seconds = call.args.number(4).value_or(3);
  seconds = seconds != 0 ? seconds : 3u;
  auto stream = std::make_unique<psx::audio::XaStream>();
  stream->disc = &disc;
  stream->drive = &call.core.game->cdc;
  stream->play(static_cast<std::uint8_t>(*channel), *startLba, disc.hunk_count * disc.frames_per_hunk - 1u, false);
  const std::uint32_t frames = seconds * SpuFieldCadence::kSampleRateHz;
  std::vector<std::int16_t> out;
  out.reserve(std::size_t{frames} * 2u);
  for (std::uint32_t i = 0; i < frames && stream->isActive(); i++) {
    std::int32_t sample[2];
    stream->pullSample(sample);
    out.push_back(static_cast<std::int16_t>(sample[0]));
    out.push_back(static_cast<std::int16_t>(sample[1]));
  }
  const std::string path = call.args.text(3);
  psx::audio::WavWriter wav(path, SpuFieldCadence::kSampleRateHz);
  if (!wav.isOpen()) {
    call.reply(lucent::format("xadump: cannot open {}", path));
    return;
  }
  wav.append(out.data(), static_cast<std::uint32_t>(out.size() / 2u));
  call.reply(lucent::format("xadump -> {} ({} frames @ {} Hz)", path, out.size() / 2u, SpuFieldCadence::kSampleRateHz));
}

void captureText(const Call &call, void (*view)(Core &, std::FILE *)) {
  char *buffer = nullptr;
  std::size_t length = 0;
  std::FILE *stream = open_memstream(&buffer, &length);
  if (stream == nullptr) {
    call.reply("no reply stream");
    return;
  }
  view(call.core, stream);
  std::fclose(stream);
  std::string_view text(buffer != nullptr ? buffer : "", length);
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    call.reply(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
  std::free(buffer);
}

void scene(const Call &call) {
  captureText(call, &psx::debug::dumpScene);
}

void display(const Call &call) {
  captureText(call, &psx::debug::dumpDisplay);
}

void otAttribution(const Call &call) {
  call.core.rsub.otAttr.runCommand(call.core, call.args, call.reply);
}

// The OT array base and entry count are the title's, so the caller names them.
void otWhere(const Call &call) {
  const std::optional<std::uint32_t> packet = call.args.hex(1);
  const std::optional<std::uint32_t> base = call.args.hex(2);
  if (!packet || !base) {
    call.reply("usage: otwhere <packet-hex> <ot-base-hex> [entries=2048]");
    return;
  }
  Core &core = call.core;
  if (core.rsub.lastOrderingTable == 0) {
    call.reply("otwhere: no ordering table walked yet");
    return;
  }
  const std::uint32_t entries = call.args.number(3).value_or(2048);
  const std::uint32_t want = psx::gpu::mainRamOffsetOf(*packet);
  const std::uint32_t bucketLo = psx::gpu::mainRamOffsetOf(*base);
  const std::uint32_t bucketHi = bucketLo + entries * 4u;
  int bucket = -1;
  int position = 0;
  int hits = 0;
  psx::gpu::OrderingTableCursor cursor(core, core.rsub.lastOrderingTable);
  do {
    const psx::gpu::OtNode &node = cursor.node();
    if (node.ramOffset() >= bucketLo && node.ramOffset() < bucketHi) {
      bucket = static_cast<int>((node.ramOffset() - bucketLo) >> 2);
      position = 0;
    } else {
      position++;
    }
    if (node.ramOffset() == want) {
      call.reply(lucent::format("otwhere 0x{:08X}: bucket={} posInBucket={} chainIdx={} hdr=0x{:08X}",
                                node.guestAddress(),
                                bucket,
                                position,
                                cursor.nodesEntered() - 1,
                                node.headerWord()));
      hits++;
    }
  } while (cursor.advance());
  if (hits == 0) {
    call.reply(lucent::format("otwhere 0x{:08X}: not in the last-walked ordering table", *packet));
  }
}

void state(const Call &call) {
  const std::string_view verb = call.args.word(1);
  const std::string path = call.args.text(2);
  if ((verb != "save" && verb != "load") || path.empty()) {
    call.reply("usage: state save <path> | state load <path>");
    return;
  }
  std::string error;
  const bool ok = verb == "save" ? psx::state::saveToFile(call.core, path, error)
                                 : psx::state::loadFromFile(call.core, path, error).has_value();
  call.reply(ok ? lucent::format("state {} {}", verb == "save" ? "saved" : "loaded", path)
                : lucent::format("state {} REFUSED: {}", verb, error));
}

void session(const Call &call) {
  if (call.args.word(1) != "return") {
    call.reply("usage: session return");
    return;
  }
  call.reply(call.core.game->session.requestReturn() ? "session return requested"
                                                     : "session return REFUSED: this product has no title selector");
}

// Every row of the overlay with its live value; the menu itself is driven by injected keys and mouse.
void menu(const Call &call) {
  const RmlOverlay &overlay = call.core.game->rml_overlay;
  if (!overlay.inited()) {
    call.reply("menu: overlay NOT initialised");
  } else if (!overlay.hasMenu()) {
    call.reply("menu: overlay is up but has NO DOCUMENT (check PSXPORT_ASSET_DIR)");
  } else {
    overlay.dumpMenu();
    call.reply(lucent::format("menu dumped to the log ({})", overlay.visible() ? "shown" : "hidden"));
  }
}

// Injected input enters the window's own event queue, so every reader sees it as the player's.
input::WindowId windowId(const Call &call) {
  return input::WindowId{SDL_GetWindowID(call.core.game->renderDevice.window().window())};
}

input::InputInjection &injection(const Call &call) {
  return call.core.game->hostInput.injection();
}

// `down`, `up` or `tap` at word `index` (tap when absent) and the tap's hold in input turns after it.
struct PressWords {
  std::optional<input::InjectedPress> press;
  int holdTurns = kDefaultHoldTurns;
};

PressWords pressWords(const Call &call, std::size_t index) {
  const std::string_view word = call.args.word(index);
  PressWords words;
  words.press = word.empty() || word == "tap" ? std::optional(input::InjectedPress::Tap)
                : word == "down"              ? std::optional(input::InjectedPress::Down)
                : word == "up"                ? std::optional(input::InjectedPress::Up)
                                              : std::nullopt;
  words.holdTurns = call.args.integer(index + 1).value_or(kDefaultHoldTurns);
  return words;
}

void key(const Call &call) {
  const PressWords words = pressWords(call, 2);
  if (call.args.word(1).empty() || !words.press) {
    call.reply("usage: key <name|scancode> [down|up|tap] [frames]");
    return;
  }
  call.reply(injection(call).key(windowId(call), call.args.word(1), *words.press, words.holdTurns));
}

void text(const Call &call) {
  call.reply(injection(call).text(windowId(call), call.args.rest(1)));
}

void mouse(const Call &call) {
  const std::string_view what = call.args.word(1);
  if (what == "move") {
    const std::optional<float> x = call.args.decimal(2);
    const std::optional<float> y = call.args.decimal(3);
    call.reply(x && y ? injection(call).mouseMove(windowId(call), {.x = *x, .y = *y})
                      : std::string("usage: mouse move <x> <y>"));
    return;
  }
  if (what == "wheel") {
    const std::optional<float> dy = call.args.decimal(2);
    const float dx = call.args.decimal(3).value_or(0.0f);
    call.reply(dy ? injection(call).mouseWheel(windowId(call), {.x = dx, .y = *dy})
                  : std::string("usage: mouse wheel <dy> [dx]"));
    return;
  }
  const PressWords words = pressWords(call, 2);
  if (what.empty() || !words.press) {
    call.reply(
        "usage: mouse move <x> <y> | mouse <left|middle|right|x1|x2> [down|up|tap] [frames] | mouse wheel <dy> [dx]");
    return;
  }
  call.reply(injection(call).mouseButton(windowId(call), what, *words.press, words.holdTurns));
}

void gamepad(const Call &call) {
  if (call.args.word(1) == "axis") {
    const std::optional<int> value = call.args.integer(3);
    call.reply(value ? injection(call).gamepadAxis(call.args.word(2), *value)
                     : std::string("usage: gamepad axis <name> <value>"));
    return;
  }
  const PressWords words = pressWords(call, 2);
  if (call.args.word(1).empty() || !words.press) {
    call.reply("usage: gamepad <button> [down|up|tap] [frames] | gamepad axis <name> <value>");
    return;
  }
  call.reply(injection(call).gamepadButton(call.args.word(1), *words.press, words.holdTurns));
}

void touch(const Call &call) {
  const std::optional<int> finger = call.args.integer(1);
  const std::string_view what = call.args.word(2);
  const std::optional<float> x = call.args.decimal(3);
  const std::optional<float> y = call.args.decimal(4);
  const std::optional<input::InjectedPress> press = what == "down"  ? std::optional(input::InjectedPress::Down)
                                                    : what == "up"  ? std::optional(input::InjectedPress::Up)
                                                    : what == "tap" ? std::optional(input::InjectedPress::Tap)
                                                                    : std::nullopt;
  if (!finger || !(press || what == "move") || !x || !y) {
    call.reply("usage: touch <finger> <down|move|up|tap> <x> <y> [frames]");
    return;
  }
  if (!press) {
    call.reply(injection(call).touchMove(windowId(call), *finger, {.x = *x, .y = *y}));
    return;
  }
  call.reply(injection(call).touch(
      windowId(call), *finger, *press, {.x = *x, .y = *y}, call.args.integer(5).value_or(kDefaultHoldTurns)));
}

inline constexpr std::array kVerbs{
    Verb{"r", "r <hex> [n]", "read n bytes of guest RAM (default 16, at most 256)", &readBytes},
    Verb{"rw", "rw <hex> [n]", "read n words of guest RAM (default 8)", &readWords},
    Verb{"w8", "w8 <addr> <val>", "write a guest byte", &writeMemory<1>},
    Verb{"w16", "w16 <addr> <val>", "write a guest halfword", &writeMemory<2>},
    Verb{"w32", "w32 <addr> <val>", "write a guest word", &writeMemory<4>},
    Verb{"call", "call <addr> [a0..a3]", "call a guest function now; reports v0/v1", &callGuest},
    Verb{"regs", "regs", "the guest registers", &registers},
    Verb{"watch", "watch <lo> <hi>", "log every guest store into [lo, hi)", &watch},
    Verb{"unwatch", "unwatch", "stop the store watch", &unwatch},
    Verb{"hits", "hits", "store-watch hits so far", &watchHits},
    Verb{"press",
         "press <button>",
         "hold a pad button (up/down/left/right/x/o/t/sq/start/select/l1..r3) on the virtual gamepad",
         &press},
    Verb{"release", "release <button>", "let go of a pad button on the virtual gamepad", &release},
    Verb{"hold", "hold <hex>", "the virtual gamepad's buttons as an active-low pad mask", &hold},
    Verb{"tap", "tap <button> [turns]", "a pad button on the virtual gamepad for n input turns (default 6)", &tap},
    Verb{"padrec",
         "padrec [save <path> [frames]]",
         "frames captured / cut the live session into a replay",
         &padRecording},
    Verb{"debug", "debug <channels>", "enable these log channels (replaces the set)", &debugChannels},
    Verb{"cvars", "cvars", "every knob with its value and layer, then the environment audit", &cvars},
    Verb{"cvar", "cvar <name> [value]", "set a knob for this run; bare clears it", &cvar},
    Verb{"guest", "guest", "the executor's counters now", &guestLedger},
    Verb{"owners", "owners [n|write]", "the owner journal: draws with no owner, held intervals", &owners},
    Verb{"census", "census", "the producer census so far, to the log", &census},
    Verb{"renderpath", "renderpath [record|device]", "switch the live render path; bare prints it", &renderPath},
    Verb{"setires", "setires <0..4>", "live internal resolution (0 = auto)", &setInternalResolution},
    Verb{"shot", "shot [path]", "the displayed picture: the record image, or the device's VRAM", &shot},
    Verb{"vkshot", "vkshot [path]", "the device's capture of the displayed picture", &deviceShot},
    Verb{"pshot", "pshot [path]", "the present image the player sees", &presentShot},
    Verb{
        "preseq", "preseq <N> [dir]", "dump the next N presented frames, interpolated ones included", &presentSequence},
    Verb{"vram", "vram <path> [x y w h]", "a VRAM region (default all of it)", &vramRegion},
    Verb{"vramraw", "vramraw <path>", "raw 16-bit VRAM", &vramRaw},
    Verb{"vkpix", "vkpix <x> <y> [n]", "raw VRAM words at x,y", &vramPixels},
    Verb{"dumpram", "dumpram [path]", "main RAM to path and the scratchpad to path.spad", &dumpRam},
    Verb{"wav", "wav <path>", "record SPU output to a WAV", &spuWav},
    Verb{"spu", "spu <path>", "Beetle's SPU state section to path; the reply has the last field's mix", &spuState},
    Verb{"xadump",
         "xadump <channel> <lba> <path> [seconds]",
         "play an XA channel of file 1 off the disc to a 44.1 kHz WAV",
         &xaDump},
    Verb{"scene", "scene", "the classified display list of the last ordering table", &scene},
    Verb{"disp", "disp", "the scanned-out VRAM rect and the draw area origin", &display},
    Verb{"otattr",
         "otattr [addr|packets|trace A|watch A L|who A [L]]",
         "who submitted this frame's packets",
         &otAttribution},
    Verb{"otwhere", "otwhere <packet> <ot-base> [entries]", "a packet's bucket in the last ordering table", &otWhere},
    Verb{"state", "state save|load <path>", "the whole machine, refused on any mismatch", &state},
    Verb{"session", "session return", "end this session and show the title selector", &session},
    Verb{"menu", "menu", "every overlay row with its live value, to the log", &menu},
    Verb{"key",
         "key <name|scancode> [down|up|tap] [frames]",
         "a keyboard key as SDL events (tap holds 6 input turns); a name with a space by number (224 Left Ctrl); hold "
         "a modifier down for a chord",
         &key},
    Verb{"text", "text <utf8>", "typed text as an SDL text-input event", &text},
    Verb{"mouse",
         "mouse move <x> <y> | <button> [down|up|tap] [frames] | wheel <dy> [dx]",
         "the mouse as SDL events, in window coordinates",
         &mouse},
    Verb{"gamepad",
         "gamepad <button> [down|up|tap] [frames] | axis <name> <value>",
         "an SDL virtual gamepad, attached on first use",
         &gamepad},
    Verb{"touch",
         "touch <finger> <down|move|up|tap> <x> <y> [frames]",
         "a finger as SDL touch events, in window coordinates",
         &touch},
};

} // namespace

bool DebugCommands::run(Core &core, const CommandArgs &args, const ReplySink &reply) {
  const std::string_view name = args.word(0);
  for (const Verb &verb : kVerbs) {
    if (verb.name == name) {
      verb.run({.core = core, .args = args, .reply = reply, .commands = *this});
      return true;
    }
  }
  std::string answer;
  const bool handled = core.runtime.debugMenu().run(core, args, answer);
  replyLines(reply, answer);
  return handled;
}

void DebugCommands::help(const Core &core, std::span<const VerbUsage> transportVerbs, const ReplySink &reply) {
  for (const VerbUsage &verb : transportVerbs) {
    reply(lucent::format("  {:<40} {}", verb.usage, verb.summary));
  }
  for (const Verb &verb : kVerbs) {
    reply(lucent::format("  {:<40} {}", verb.usage, verb.summary));
  }
  for (const psx::game::DebugVerb &verb : core.runtime.debugMenu().verbs()) {
    reply(lucent::format("  {:<40} {}", psx::game::TitleDebugMenu::usage(verb), verb.summary));
  }
}

} // namespace psx::debug
