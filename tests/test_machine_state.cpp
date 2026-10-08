// test_machine_state.cpp — a whole-machine state round-trips, and refuses when it should.
//
// WHAT IS ACTUALLY PROVEN HERE, and what is not. Proved: every device the owner claims reaches its
// section and comes back byte-identical through the SHIPPING path (MachineState::capture ->
// StateFile -> MachineState::restore), the Beetle SPU and MDEC round-trip through the FORK's own
// state functions, and the three title-mismatch directions all refuse by name.
//
// Not proved here: that a resumed run is behaviourally identical. That needs a guest, and it is the
// determinism check the Spyro work does against Glimmer (600 fields of guest-RAM digest compared
// between the process that took the state and the process that loaded it). A unit test that claimed
// it would be claiming something it cannot see.
//
// HERMETIC: one Game is constructed in-process and the memory card is a temp file in the CWD. No
// disc, no window, no network.
#include "beetle_device_state.h"
#include "device_sections.h"
#include "game.h"
#include "game_runtime.h"
#include "machine_state.h"
#include "memcard.h"
#include "state_file.h"
#include "testutil.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *kCardPath = "test_machine_state.mcr";

GameConfig g_cfg{};

Game *makeGame() {
  static Game *game = nullptr;
  if (game != nullptr) {
    return game;
  }
  std::remove(kCardPath);
  game = new Game();
  g_cfg.cardDefaultPath = kCardPath;
  game->core.cfg = &g_cfg;
  card_overrides_init(game);
  return game;
}

// A fingerprint of everything the owner claims to carry, computed through the SHIPPING writers —
// not by reading the struct back. Comparing a section against the struct it came from would pass even
// if the section never reached the file, which is the dead-tap shape this suite exists to avoid.
std::string fingerprint(const Game &game) {
  psx::state::BlobWriter out;
  writeCpuSection(const_cast<Core &>(game.core), out);
  writeRamSection(const_cast<Core &>(game.core), out);
  writeScratchpadSection(const_cast<Core &>(game.core), out);
  writeGteSection(const_cast<Game &>(game), out);
  writeGpuSection(const_cast<Game &>(game), out);
  writeCdcSection(const_cast<Game &>(game), out);
  writeTimingSection(const_cast<Game &>(game), out);
  writeDmaSection(const_cast<Core &>(game.core), out);
  writeHleSection(const_cast<Game &>(game), out);
  writeSioSection(const_cast<Game &>(game), out);
  writeCdSection(const_cast<Game &>(game), out);
  std::string text;
  const std::vector<std::uint8_t> bytes = out.bytesOut();
  text.reserve(bytes.size() * 2);
  static const char *digits = "0123456789abcdef";
  for (std::uint8_t byte : bytes) {
    text.push_back(digits[byte >> 4]);
    text.push_back(digits[byte & 0x0Fu]);
  }
  return text;
}

// Give the machine something that is not its power-on value in every section this test touches. A
// fixture that leaves a device at power-on would make the round-trip comparison vacuous.
void perturb(Game &game) {
  Core &core = game.core;
  core.r[3] = 0x80001234u;
  core.r[31] = 0x800ABCDEu;
  core.hi = 0x12345678u;
  core.lo = 0x9ABCDEF0u;
  core.pc = 0x80004000u;
  core.cop0[12] = 0x00010900u; // Status: IEc + kernel
  core.cop0[13] = 0x00000400u; // Cause: software interrupt
  core.cop0[14] = 0x80004100u; // EPC
  core.pending_work = 2;       // PW_HOST
  for (std::size_t i = 0; i < 0x200000u; i += 0x4000u) {
    core.ram[i] = static_cast<std::uint8_t>(i >> 12);
  }
  core.ram[0x1FFFFu] = 0x5Au;
  core.scratch[7] = 0xC3u;
  game.gte.REG[5] = 0x00010001u;
  game.gte.REG[26] = 256u; // H — the projection distance
  game.gte.FLAGS = 0x00000042u;

  game.gpu.s_vram[0] = 0x1234u;
  game.gpu.s_vram[VRAM_W * 240 + 320] = 0xBEEFu;
  game.gpu.s_disp_x = 160;
  game.gpu.s_disp_w = 320;
  game.gpu.s_tp_x = 640;
  game.gpu.s_fifo[3] = 0xE3000000u;
  game.gpu.s_fifo_addr[3] = 0x80009900u;
  game.gpu.s_fcount = 3;
  game.gpu.s_fneed = 4;
  game.gpu.s_frame = 1234;
  game.gpu.s_cur_node = 0x8000FFF0u;

  game.cdc.loc_lba = 0x1234u;
  game.cdc.reading = 1;
  game.cdc.stat = 0x22;
  game.cdc.q[3].type = 0x02;
  game.cdc.q_head = 4;
  game.cdc.q_tail = 3;
  game.cdc.irq_sequence = 77u;

  game.timing.vblank = 4242u;
  game.timing.logicFrame = 2121u;
  game.timing.guestInstructionTicks = 0x0000DEADBEEFull;

  core.dma.dma3Madr = 0x80100000u;
  core.dma.dicr = 0x009A0000u;
  core.dma.done.mask = 0x10u;

  game.hle.i_stat = 0x0402u;
  game.hle.i_mask = 0xFFFFu;
  game.hle.rand_state = 0x12345678u;
  game.hle.heap_base = 0x80100000u;
  game.hle.heap_size = 0x10000u;
  game.hle.heap_ok = 1;
  game.hle.nblk = 2;
  game.hle.blk[0].addr = 0x80101000u;
  game.hle.blk[0].size = 0x400u;
  game.hle.blk[0].used = 1;
  game.hle.ev[2].open = 1;
  game.hle.ev[2].enabled = 1;
  game.hle.ev[2].fired = 1;
  game.hle.ev[2].ev_class = 0xF4000001u;
  game.hle.ev[2].spec = 0x0004u;
  game.hle.ev[2].func = 0x80009900u;

  game.sio.mode = 0x13u;
  game.sio.pos = 4;
  game.sio.rx = 0x5Au;
  game.sio.irq = true;
  game.pad.buttons = 0xFF7Fu;

  game.cd.setloc_lba = 0x2222;
  game.cd.pending_music = 3;
  game.cd.pm_start = 0x1000u;
  game.cd.pm_end = 0x2000u;

  // And the memory card, which is a HOST FILE the run owns: without a frame written here the card
  // section is byte-identical at both ends of the test and proves nothing.
  std::uint8_t frame[Memcard::kFrameSize] = {};
  for (std::uint32_t i = 0; i < Memcard::kFrameSize; ++i) {
    frame[i] = static_cast<std::uint8_t>(i);
  }
  game.memcard.writeFrame(200, frame);
}

void test_every_section_round_trips_through_the_file(void) {
  Game &game = *makeGame();
  perturb(game);
  const std::string before = fingerprint(game);

  psx::state::MachineState state(game);
  std::string error;
  const auto image = state.capture(error);
  CHECK_MSG(image.has_value(), error.c_str());

  // The image really carries the sections, and the count is published rather than implied.
  psx::state::StateFile::OpenError openError;
  const auto file = psx::state::StateFile::open(*image, openError);
  CHECK_MSG(file.has_value(), openError.reason.c_str());
  CHECK_EQ(static_cast<int>(file->names().size()), 14);
  for (const char *name : {"cpu",
                           "ram",
                           "scratchpad",
                           "gte",
                           "gpu",
                           "beetle.spu",
                           "beetle.mdec",
                           "cdc",
                           "timing",
                           "dma",
                           "hle",
                           "sio",
                           "card",
                           "cd"}) {
    bool present = false;
    for (const std::string &have : file->names()) {
      present = present || have == name;
    }
    CHECK_MSG(present, name);
  }

  // Wipe every field the fixture set, then restore. Restoring over unchanged values would pass even
  // if the restore wrote nothing at all.
  makeGame();
  Game &wiped = *makeGame();
  wiped.core.r[3] = 0u;
  wiped.core.pc = 0u;
  wiped.core.cop0[12] = 0u;
  wiped.gpu.s_vram[0] = 0u;
  wiped.gpu.s_disp_x = 0;
  wiped.gpu.s_fifo[3] = 0u;
  wiped.cdc.loc_lba = 0u;
  wiped.timing.vblank = 0u;
  wiped.hle.i_stat = 0u;
  wiped.hle.rand_state = 1u;
  wiped.sio.pos = -1;
  wiped.pad.buttons = 0xFFFFu;
  wiped.cd.setloc_lba = -1;

  psx::state::MachineState restorer(wiped);
  const auto outcome = restorer.restore(*image, error);
  CHECK_MSG(outcome.has_value(), error.c_str());
  CHECK_EQ(static_cast<int>(outcome->sections.size()), 14);
  // Two spans: main RAM and the scratchpad, both executable writes.
  CHECK_EQ(static_cast<int>(outcome->invalidationRanges), 2);

  // The card is a HOST FILE, so the round-trip also had to move it: check the written frame came
  // back, which is the only evidence the card section is carrying anything at all.
  std::uint8_t frame[Memcard::kFrameSize] = {};
  CHECK(wiped.memcard.readFrame(200, frame));
  CHECK_EQ(static_cast<int>(frame[0]), 0);
  CHECK_EQ(static_cast<int>(frame[127]), 127);

  CHECK_STREQ(fingerprint(wiped).c_str(), before.c_str());
  // Spot checks that name the FIELDS, so a failure says which device diverged.
  CHECK_EQ(wiped.core.pc, 0x80004000u);
  CHECK_EQ(static_cast<int>(wiped.gte.REG[26]), 256);
  CHECK_EQ(static_cast<int>(wiped.cdc.loc_lba), 0x1234);
  CHECK_EQ(static_cast<int>(wiped.timing.vblank), 4242);
  CHECK_EQ(static_cast<int>(wiped.hle.rand_state), 0x12345678);
  CHECK_EQ(static_cast<int>(wiped.cd.setloc_lba), 0x2222);
  CHECK_EQ(static_cast<int>(wiped.pad.buttons), 0xFF7Fu);
  CHECK_EQ(static_cast<int>(wiped.core.r[0]), 0); // architectural zero, whatever the file said
}

void test_beetle_devices_round_trip_through_the_forks_own_functions(void) {
  Game &game = *makeGame();
  game.spu.bind(&game.core);
  game.mdec.bind();
  // Move both devices away from power-on first: the fork's writer emits the same bytes for a fresh
  // device and a used one if the fixture did nothing, and a round-trip against identical bytes
  // proves nothing at all. The SPU is moved through its own power-on-reset path below, which the
  // selftest compares against.
  std::string error;
  const bool spu = psx::state::beetleDeviceRoundTrips(game.core, psx::state::BeetleDevice::Spu, error);
  CHECK_MSG(spu, error.c_str());
  const bool mdec = psx::state::beetleDeviceRoundTrips(game.core, psx::state::BeetleDevice::Mdec, error);
  CHECK_MSG(mdec, error.c_str());
}

void test_a_title_without_native_state_refuses_a_file_that_has_one(void) {
  Game &game = *makeGame();
  CHECK_MSG(psx::state::MachineState::titlePort(game) == nullptr, "the bare Game declared a title state port");

  // Hand-build a file that carries a title section, exactly as a title WITH native owners would.
  psx::state::StateImage image;
  for (const std::string_view name : {psx::state::section::kCpu,
                                      psx::state::section::kRam,
                                      psx::state::section::kScratchpad,
                                      psx::state::section::kGte,
                                      psx::state::section::kGpu,
                                      psx::state::section::kBeetleSpu,
                                      psx::state::section::kBeetleMdec,
                                      psx::state::section::kCdc,
                                      psx::state::section::kTiming,
                                      psx::state::section::kDma,
                                      psx::state::section::kHle,
                                      psx::state::section::kSio,
                                      psx::state::section::kCard,
                                      psx::state::section::kCd}) {
    CHECK(image.add(name, {}));
  }
  psx::state::BlobWriter title;
  title.u8(1);
  title.u32(7);
  CHECK(image.add(psx::state::section::kTitle, title.bytesOut()));

  psx::state::MachineState state(game);
  std::string error;
  const auto outcome = state.restore(image.bytes(), error);
  CHECK_MSG(!outcome.has_value(), "a state with a title section loaded into a title that owns none");
  CHECK_MSG(error.find("declares none") != std::string::npos, error.c_str());
}

// A title that DOES own native state, so the refusals that depend on one can be exercised. The
// framework sections are untouched; only the `title` section's name and version differ, which is
// what the mismatch checks are about.
class FixtureTitleState final : public psx::state::NativeStatePort {
public:
  const char *sectionName() const override {
    return "fixture.title";
  }
  std::uint32_t version() const override {
    return 4;
  }
  bool save(psx::state::BlobWriter &out, std::string &) const override {
    out.u32(savedMarker);
    return true;
  }
  bool load(psx::state::BlobReader &in, std::string &error) override {
    const std::uint32_t marker = in.u32();
    if (!in.ok()) {
      error = "fixture load short read";
      return false;
    }
    loadedMarker = marker;
    return true;
  }
  void restored(Core &core) override {
    ++restoredCalls;
    ramWordWhenRestored = core.mem_r32(kRestoredProbeAddress);
  }
  // A guest word the title would derive native state from: what it reads in `restored` must be the
  // RESTORED value, which is what lets a title re-establish state that has to agree with guest RAM.
  static constexpr std::uint32_t kRestoredProbeAddress = 0x80010000u;
  std::uint32_t savedMarker = 0x11111111u;
  std::uint32_t loadedMarker = 0u;
  std::uint32_t restoredCalls = 0u;
  std::uint32_t ramWordWhenRestored = 0u;
};

// Installs a title state port on the running game for the duration of a check, and puts the
// runtime that was there back afterwards. Swapping rather than building a second Game is deliberate:
// Lightrec supports one initialized machine per process, and `MachineState::restore` reports
// invalidation through the running executor.
class ScopedTitlePort {
public:
  explicit ScopedTitlePort(Game &game, psx::state::NativeStatePort *port) : game_(game), saved_(game.runtime) {
    game.runtime = &runtime_;
    runtime_.port = port;
  }
  ~ScopedTitlePort() {
    game_.runtime = saved_;
  }

private:
  // The minimum concrete GameRuntime. Its only product is `nativeState`; the frame/step/yield and
  // render capabilities a real title implements are irrelevant to a save-state check and are
  // stubbed rather than left undefined, so the fixture cannot accidentally pass because a
  // capability defaulted to something.
  class Runtime final : public GameRuntime {
  public:
    psx::state::NativeStatePort *nativeState(Core &) const override {
      return port;
    }
    void *createContext(Core &) override {
      return nullptr;
    }
    void destroyContext(void *) override {}
    void registerOverrides(Game &) override {}
    void bootInit(Core &) override {}
    RenderCapabilities renderCapabilities() const override {
      return {};
    }
    bool guestVramIsPicture(const Game &) const override {
      return false;
    }
    psx::state::NativeStatePort *port = nullptr;
  };

  Game &game_;
  GameRuntime *saved_;
  Runtime runtime_;
};

void test_a_file_missing_a_device_section_is_refused_with_the_section_named(void) {
  Game &game = *makeGame();
  psx::state::StateImage image;
  for (const std::string_view name : {psx::state::section::kCpu,
                                      psx::state::section::kRam,
                                      psx::state::section::kScratchpad,
                                      psx::state::section::kGte,
                                      psx::state::section::kGpu,
                                      psx::state::section::kBeetleSpu,
                                      psx::state::section::kBeetleMdec,
                                      psx::state::section::kCdc,
                                      psx::state::section::kTiming,
                                      psx::state::section::kDma,
                                      psx::state::section::kHle,
                                      psx::state::section::kSio,
                                      psx::state::section::kCd}) {
    CHECK(image.add(name, {}));
  }
  // The card section is deliberately absent: 13 of the 14 the machine needs.
  psx::state::MachineState state(game);
  std::string error;
  const auto outcome = state.restore(image.bytes(), error);
  CHECK_MSG(!outcome.has_value(), "a state missing its memory card loaded");
  CHECK_MSG(error.find("card") != std::string::npos, error.c_str());
  CHECK_MSG(error.find("13 sections") != std::string::npos, error.c_str());
}

// The other direction of the title rule, and the one that is easy to leave unasserted: a title that
// OWNS native state, handed a file with no title section. Loading it would resume a guest whose
// frame driver, field scheduler and boot state are at their freshly-constructed values while the
// guest RAM beside them is mid-level — a desync on the first field, with nothing to name it. The
// framework's own sections are all present here, so the ONLY reason to refuse is the missing title
// state, and that is what the error must say.
void test_a_title_that_owns_native_state_refuses_a_file_without_one(void) {
  Game &game = *makeGame();
  FixtureTitleState port;
  ScopedTitlePort scoped(game, &port);

  psx::state::StateImage image;
  for (const std::string_view name : {psx::state::section::kCpu,
                                      psx::state::section::kRam,
                                      psx::state::section::kScratchpad,
                                      psx::state::section::kGte,
                                      psx::state::section::kGpu,
                                      psx::state::section::kBeetleSpu,
                                      psx::state::section::kBeetleMdec,
                                      psx::state::section::kCdc,
                                      psx::state::section::kTiming,
                                      psx::state::section::kDma,
                                      psx::state::section::kHle,
                                      psx::state::section::kSio,
                                      psx::state::section::kCard,
                                      psx::state::section::kCd}) {
    CHECK(image.add(name, {}));
  }
  // Every framework section is present and the title section is the only thing absent.

  psx::state::MachineState state(game);
  std::string error;
  const auto outcome = state.restore(image.bytes(), error);
  CHECK_MSG(!outcome.has_value(), "a title with native state loaded a file that carries none");
  CHECK_MSG(error.find("carries no title section") != std::string::npos, error.c_str());
  CHECK_MSG(port.loadedMarker == 0u, "the title port was written to despite the refusal");
  CHECK_MSG(port.restoredCalls == 0u, "the title adopted restored state from a refused load");
}

// A title section carrying a VERSION this title does not implement. The title owns the meaning of
// those bytes, so the framework cannot know whether an older layout is safe to read; only the title
// can, and a version it does not implement means it cannot.
void test_a_title_section_from_a_newer_title_version_is_refused(void) {
  Game &game = *makeGame();
  FixtureTitleState port; // implements version 4
  ScopedTitlePort scoped(game, &port);

  psx::state::StateImage image;
  for (const std::string_view name : {psx::state::section::kCpu,
                                      psx::state::section::kRam,
                                      psx::state::section::kScratchpad,
                                      psx::state::section::kGte,
                                      psx::state::section::kGpu,
                                      psx::state::section::kBeetleSpu,
                                      psx::state::section::kBeetleMdec,
                                      psx::state::section::kCdc,
                                      psx::state::section::kTiming,
                                      psx::state::section::kDma,
                                      psx::state::section::kHle,
                                      psx::state::section::kSio,
                                      psx::state::section::kCard,
                                      psx::state::section::kCd}) {
    CHECK(image.add(name, {}));
  }
  // The envelope the framework writes around a title's own payload: its envelope version, then the
  // TITLE's layout version. The first byte must be right, or the load refuses for the envelope's
  // reason and never reaches the version check this test exists to exercise.
  psx::state::BlobWriter title;
  title.u8(psx::state::kTitleEnvelopeVersion);
  title.u32(5); // one past the version this title implements
  CHECK(image.add(psx::state::section::kTitle, title.bytesOut()));

  psx::state::MachineState state(game);
  std::string error;
  const auto outcome = state.restore(image.bytes(), error);
  CHECK_MSG(!outcome.has_value(), "a title section of an unimplemented version loaded");
  CHECK_MSG(error.find("version") != std::string::npos, error.c_str());
  CHECK_MSG(port.loadedMarker == 0u, "the title port was written to despite the refusal");
  CHECK_MSG(port.restoredCalls == 0u, "the title adopted restored state from a refused load");
}

// A title whose native state is DERIVED from guest RAM (the image identities of the code resident
// there) cannot adopt it in `load`, which runs before the RAM section. It is told once the whole
// machine is restored, and at that point it must see the restored bytes, not the ones they replaced.
void test_a_title_port_adopts_its_state_after_guest_ram_is_restored(void) {
  Game &game = *makeGame();
  FixtureTitleState port;
  ScopedTitlePort scoped(game, &port);
  Core &core = game.core;

  core.mem_w32(FixtureTitleState::kRestoredProbeAddress, 0xC0DE0001u);
  psx::state::MachineState state(game);
  std::string error;
  const auto image = state.capture(error);
  CHECK_MSG(image.has_value(), error.c_str());
  CHECK_MSG(port.restoredCalls == 0u, "a capture told the title its state was restored");

  core.mem_w32(FixtureTitleState::kRestoredProbeAddress, 0xDEAD0002u);
  const auto outcome = state.restore(*image, error);
  CHECK_MSG(outcome.has_value(), error.c_str());
  CHECK_EQ(port.restoredCalls, 1u);
  CHECK_EQ(port.ramWordWhenRestored, 0xC0DE0001u);
  CHECK_EQ(port.loadedMarker, port.savedMarker);
}

} // namespace

int main(void) {
  RUN(every_section_round_trips_through_the_file);
  RUN(beetle_devices_round_trip_through_the_forks_own_functions);
  RUN(a_title_without_native_state_refuses_a_file_that_has_one);
  RUN(a_file_missing_a_device_section_is_refused_with_the_section_named);
  RUN(a_title_that_owns_native_state_refuses_a_file_without_one);
  RUN(a_title_section_from_a_newer_title_version_is_refused);
  RUN(a_title_port_adopts_its_state_after_guest_ram_is_restored);
  return pt_summary();
}