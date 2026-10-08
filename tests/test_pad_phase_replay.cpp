// Phase-keyed pad recordings: the v1 file format, its streaming writer, the phase matcher, the
// session's card and phase-source refusals, and the Pad seam that feeds them a title phase.
//
// The negatives matter more than the round trips. A replay that silently reads a raw file as
// absolute input, passes over a recorded phase that held a press, or plays against a different card
// is exactly how spyro issue 0116's route came to answer the wrong dialog while looking like a run.

#include "game.h"
#include "game_runtime.h"
#include "pad_phase_replay.h"
#include "pad_record_replay.h"
#include "pad_recording.h"
#include "testutil.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

using psx::input::CardIdentity;
using psx::input::CardSnapshot;
using psx::input::DecodedPadRecording;
using psx::input::kNeutralMask;
using psx::input::kUnkeyedPhase;
using psx::input::PadRecording;
using psx::input::PadRecordingWriter;
using psx::input::PadRecordReplay;
using psx::input::PadSessionConfig;
using psx::input::PhaseReplay;
using psx::input::ReplayStatus;

constexpr std::uint64_t kTitle = 0x0D00000003ull;
constexpr std::uint64_t kMenu = 0x0D0001000Full;
constexpr std::uint64_t kLoad = 0x0D00020004ull;
constexpr std::uint64_t kPlay = 0x000A000000ull;
constexpr std::uint16_t kStart = 0xFFF7u;
constexpr std::uint16_t kCross = 0xBFFFu;

CardIdentity cardImage(std::uint8_t fill) {
  CardIdentity card{.kind = CardIdentity::Kind::Image, .sha256 = {}};
  card.sha256.fill(fill);
  return card;
}

void appendFrames(PadRecording &recording, std::uint64_t phase, std::uint16_t mask, int frames) {
  for (int i = 0; i < frames; i++) {
    recording.append(phase, mask);
  }
}

// Title (8 neutral, Start at offset 8..9), menu (3 neutral, Cross at 3..4), then play (4 neutral).
PadRecording menuRoute() {
  PadRecording recording(cardImage(0x11));
  appendFrames(recording, kTitle, kNeutralMask, 8);
  appendFrames(recording, kTitle, kStart, 2);
  appendFrames(recording, kMenu, kNeutralMask, 3);
  appendFrames(recording, kMenu, kCross, 2);
  appendFrames(recording, kPlay, kNeutralMask, 4);
  return recording;
}

std::string scratchFile(const char *name) {
  return (std::filesystem::current_path() / name).string();
}

void writeBytes(const std::string &path, const std::vector<std::uint8_t> &bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> readBytes(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool fileExists(const std::string &path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored);
}

void removeFile(const std::string &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

// Drive `replay` through `phases`, one pad frame each; returns every delivered mask (nullopt when the
// replay no longer drives the pad).
std::vector<std::optional<std::uint16_t>> drive(PhaseReplay &replay, const std::vector<std::uint64_t> &phases) {
  std::vector<std::optional<std::uint16_t>> masks;
  masks.reserve(phases.size());
  for (const std::uint64_t phase : phases) {
    masks.push_back(replay.next(phase));
  }
  return masks;
}

std::vector<std::uint64_t> repeat(std::uint64_t phase, int frames) {
  return std::vector<std::uint64_t>(static_cast<std::size_t>(frames), phase);
}

std::vector<std::uint64_t> concat(std::initializer_list<std::vector<std::uint64_t>> parts) {
  std::vector<std::uint64_t> all;
  for (const std::vector<std::uint64_t> &part : parts) {
    all.insert(all.end(), part.begin(), part.end());
  }
  return all;
}

// ---- format ----------------------------------------------------------------------------------------

void test_segments_and_runs_are_recorded_by_phase() {
  const PadRecording recording = menuRoute();
  CHECK_EQ(recording.totalFrames(), 19u);
  CHECK_EQ(recording.segments().size(), 3u);
  CHECK_EQ(recording.segments()[0].phase, kTitle);
  CHECK_EQ(recording.segments()[0].runs.size(), 2u);
  CHECK_EQ(recording.segments()[0].frames(), 10u);
  CHECK_EQ(recording.segments()[0].maskAt(8), kStart);
  CHECK_EQ(recording.segments()[0].inputFramesFrom(0), 2u);
  CHECK_EQ(recording.segments()[0].inputFramesFrom(9), 1u);
  CHECK_EQ(recording.segments()[2].inputFramesFrom(0), 0u);
  CHECK(recording.keyed());
}

void test_encode_decode_round_trips_header_segments_and_card() {
  const PadRecording recording = menuRoute();
  const std::vector<std::uint8_t> bytes = recording.encode();
  CHECK_EQ(bytes.size(),
           PadRecording::kHeaderBytes + 3 * PadRecording::kPhaseRecordBytes + 5 * PadRecording::kRunRecordBytes);
  const DecodedPadRecording decoded = PadRecording::decode(bytes);
  CHECK(decoded.recording.has_value());
  CHECK(decoded.recording->card() == recording.card());
  CHECK_EQ(decoded.recording->totalFrames(), recording.totalFrames());
  CHECK(decoded.recording->encode() == bytes);
}

void test_a_prefix_cuts_inside_a_run_and_never_offers_a_suffix() {
  const PadRecording recording = menuRoute();
  const DecodedPadRecording decoded = PadRecording::decode(recording.encode(9));
  CHECK(decoded.recording.has_value());
  CHECK_EQ(decoded.recording->totalFrames(), 9u);
  CHECK_EQ(decoded.recording->segments().size(), 1u);
  CHECK_EQ(decoded.recording->segments()[0].maskAt(8), kStart);
}

void test_a_raw_pre_v1_recording_is_refused_and_names_the_migration() {
  const std::vector<std::uint8_t> raw = {0xFF, 0xFF, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const DecodedPadRecording decoded = PadRecording::decode(raw);
  CHECK(!decoded.recording.has_value());
  CHECK(decoded.error.find("PSXPADPH") != std::string::npos);
  CHECK(decoded.error.find("psx_pad.py migrate") != std::string::npos);
}

void test_an_unknown_version_is_refused_by_number() {
  std::vector<std::uint8_t> bytes = menuRoute().encode();
  bytes[8] = 2;
  const DecodedPadRecording decoded = PadRecording::decode(bytes);
  CHECK(!decoded.recording.has_value());
  CHECK(decoded.error.find("version 2 is not supported") != std::string::npos);
}

void test_malformed_bodies_are_refused_not_truncated() {
  const std::vector<std::uint8_t> good = menuRoute().encode();

  std::vector<std::uint8_t> truncated(good.begin(), good.end() - 2);
  CHECK(PadRecording::decode(truncated).error.find("truncated run record") != std::string::npos);

  std::vector<std::uint8_t> headerOnly;
  PadRecording::putHeader(headerOnly, {});
  CHECK(PadRecording::decode(headerOnly).error.find("holds no frames") != std::string::npos);

  std::vector<std::uint8_t> orphanRun = headerOnly;
  PadRecording::putRun(orphanRun, kNeutralMask, 1);
  CHECK(PadRecording::decode(orphanRun).error.find("precedes any phase") != std::string::npos);

  std::vector<std::uint8_t> zeroRun = headerOnly;
  PadRecording::putPhase(zeroRun, kTitle);
  PadRecording::putRun(zeroRun, kNeutralMask, 0);
  CHECK(PadRecording::decode(zeroRun).error.find("zero-length") != std::string::npos);

  std::vector<std::uint8_t> emptyPhase = headerOnly;
  PadRecording::putPhase(emptyPhase, kTitle);
  PadRecording::putPhase(emptyPhase, kMenu);
  PadRecording::putRun(emptyPhase, kNeutralMask, 1);
  CHECK(PadRecording::decode(emptyPhase).error.find("holds no frames") != std::string::npos);

  std::vector<std::uint8_t> badTag = headerOnly;
  badTag.push_back('X');
  CHECK(PadRecording::decode(badTag).error.find("unknown record tag") != std::string::npos);
}

void test_the_streaming_writer_matches_the_in_memory_encoding_byte_for_byte() {
  const std::string path = scratchFile("test_pad_phase_replay_stream.pad");
  const PadRecording expected = menuRoute();
  {
    std::string error;
    std::unique_ptr<PadRecordingWriter> writer = PadRecordingWriter::open(path.c_str(), expected.card(), error);
    CHECK(writer != nullptr);
    for (const psx::input::PadSegment &segment : expected.segments()) {
      for (const psx::input::PadRun &run : segment.runs) {
        for (std::uint32_t i = 0; i < run.frames; i++) {
          CHECK(writer->append(segment.phase, run.mask));
        }
      }
    }
  }
  CHECK(readBytes(path) == expected.encode());
  removeFile(path);
}

// ---- matching --------------------------------------------------------------------------------------

void test_a_longer_boot_moves_the_press_with_its_phase() {
  PhaseReplay replay(menuRoute());
  // The title phase now lasts 25 frames instead of 10, and the game is still there when the recorded
  // frames run out: the replay holds neutral for the overrun, then the menu press lands at offset 3.
  const auto masks = drive(replay, concat({repeat(kTitle, 25), repeat(kMenu, 6), repeat(kPlay, 4)}));
  CHECK_EQ(*masks[8], kStart);
  CHECK_EQ(*masks[10], kNeutralMask);
  CHECK_EQ(*masks[25 + 3], kCross);
  CHECK_EQ(*masks[25 + 4], kCross);
  CHECK_EQ(replay.progress().framesWaited, 15u + 1u);
  CHECK_EQ(replay.progress().phasesWaitedFor, 2u);
  CHECK(replay.progress().status == ReplayStatus::Complete);
  CHECK_EQ(replay.progress().framesEmitted, 19u);
  CHECK_EQ(replay.progress().inputFramesDropped, 0u);
}

void test_a_shorter_phase_drops_only_neutral_frames_and_counts_it() {
  PhaseReplay replay(menuRoute());
  // The menu is left 5 frames in, as recorded. The play phase ends after 2 of its 4 neutral frames:
  // nothing held a button, so nothing is reported dropped, and the recording is complete.
  const auto masks = drive(replay, concat({repeat(kTitle, 10), repeat(kMenu, 5), repeat(kPlay, 2), repeat(kLoad, 1)}));
  CHECK_EQ(*masks[13], kCross);
  CHECK_EQ(*masks[16], kNeutralMask);
  CHECK(!masks[17].has_value());
  CHECK(replay.progress().status == ReplayStatus::Complete);
  CHECK_EQ(replay.progress().framesEmitted, 17u);
  CHECK_EQ(replay.progress().inputFramesDropped, 0u);
}

void test_leaving_a_phase_before_its_press_is_counted_as_dropped_input() {
  PhaseReplay replay(menuRoute());
  // The game leaves the title after 9 frames: one of the two recorded Start frames was never
  // delivered. The replay carries on into the menu phase and reports the loss.
  const auto masks = drive(replay, concat({repeat(kTitle, 9), repeat(kMenu, 5), repeat(kPlay, 4)}));
  CHECK_EQ(*masks[8], kStart);
  CHECK_EQ(*masks[9 + 3], kCross);
  CHECK_EQ(replay.progress().inputFramesDropped, 1u);
  CHECK_EQ(replay.progress().segmentsWithDroppedInput, 1u);
  CHECK(replay.progress().status == ReplayStatus::Complete);
}

void test_a_phase_that_never_arrives_is_reported_not_skipped() {
  PhaseReplay replay(menuRoute(), /*phaseWaitLimit=*/20);
  // The card sends the title somewhere the recording never went (kLoad). The menu segment holds a
  // press, so it can never be passed over: the replay waits, then stops as STALLED and says so.
  const auto masks = drive(replay, concat({repeat(kTitle, 10), repeat(kLoad, 30)}));
  CHECK_EQ(*masks[10], kNeutralMask);
  CHECK_EQ(*masks[29], kNeutralMask);
  CHECK(!masks[30].has_value());
  CHECK(!masks[39].has_value());
  CHECK(replay.progress().status == ReplayStatus::Stalled);
  CHECK_EQ(replay.progress().stallSegment, 1u);
  CHECK_EQ(replay.progress().stallExpectedPhase, kMenu);
  CHECK_EQ(replay.progress().stallObservedPhase, kLoad);
  CHECK_EQ(replay.progress().framesWaited, 20u);
  CHECK_EQ(replay.progress().framesEmitted, 10u);
}

void test_a_skipped_phase_holding_input_is_never_passed_over() {
  PadRecording recording;
  appendFrames(recording, kTitle, kNeutralMask, 2);
  appendFrames(recording, kMenu, kCross, 1); // a one-frame phase with a press
  appendFrames(recording, kPlay, kNeutralMask, 2);
  PhaseReplay replay(std::move(recording), /*phaseWaitLimit=*/5);
  drive(replay, concat({repeat(kTitle, 2), repeat(kPlay, 10)}));
  CHECK(replay.progress().status == ReplayStatus::Stalled);
  CHECK_EQ(replay.progress().stallExpectedPhase, kMenu);
  CHECK_EQ(replay.progress().segmentsPassed, 0u);
}

void test_an_unobserved_neutral_phase_is_passed_and_counted() {
  PadRecording recording;
  appendFrames(recording, kTitle, kNeutralMask, 2);
  appendFrames(recording, kLoad, kNeutralMask, 1); // transient, no input
  appendFrames(recording, kMenu, kCross, 2);
  PhaseReplay replay(std::move(recording));
  const auto masks = drive(replay, concat({repeat(kTitle, 2), repeat(kMenu, 2)}));
  CHECK_EQ(*masks[2], kCross);
  CHECK_EQ(*masks[3], kCross);
  CHECK_EQ(replay.progress().segmentsPassed, 1u);
  CHECK_EQ(replay.progress().segmentsEntered, 2u);
  CHECK(replay.progress().status == ReplayStatus::Complete);
}

void test_an_unkeyed_recording_replays_absolutely_from_boot() {
  PadRecording recording;
  appendFrames(recording, kUnkeyedPhase, kNeutralMask, 3);
  appendFrames(recording, kUnkeyedPhase, kStart, 1);
  CHECK(!recording.keyed());
  PhaseReplay replay(std::move(recording));
  const auto masks = drive(replay, {kTitle, kMenu, kLoad, kPlay, kPlay});
  CHECK_EQ(*masks[3], kStart);
  CHECK(!masks[4].has_value());
  CHECK_EQ(replay.progress().framesWaited, 0u);
  CHECK(replay.progress().status == ReplayStatus::Complete);
}

// ---- session policy --------------------------------------------------------------------------------

// The session keeps the bytes of the card it started from, not only their digest: a recording is
// replayable only against that card, so whoever is handed the recording needs those exact bytes.
void test_the_session_keeps_the_card_image_it_started_from() {
  PadRecordReplay session;
  const std::vector<std::uint8_t> start = {0x4D, 0x43, 0x00, 0x7F};
  session.configure(PadSessionConfig{.recordPath = "0", .liveInputOnly = true, .card = [&start] {
                                       return CardSnapshot{cardImage(0x33), start};
                                     }});
  CHECK(session.startCardImage() == start);
  CHECK(session.recording().card() == cardImage(0x33));
}

void test_a_replay_against_a_different_card_is_refused() {
  const std::string path = scratchFile("test_pad_phase_replay_card.pad");
  writeBytes(path, menuRoute().encode());
  PadRecordReplay session;
  session.configure(
      PadSessionConfig{.recordPath = "", .replayPath = path, .resumePath = "", .windowed = false, .card = [] {
                         return CardSnapshot{cardImage(0x22), {}};
                       }});
  CHECK(!session.replayPending());
  CHECK_EQ(session.service(kTitle, 0x1234u, kNeutralMask), 0x1234u);

  PadRecordReplay same;
  same.configure(
      PadSessionConfig{.recordPath = "", .replayPath = path, .resumePath = "", .windowed = false, .card = [] {
                         return CardSnapshot{cardImage(0x11), {}};
                       }});
  CHECK(same.replayPending());
  CHECK_EQ(same.service(kTitle, 0x1234u, kNeutralMask), kNeutralMask);
  removeFile(path);
}

void test_a_resume_against_a_changed_card_proceeds_and_fast_forwards() {
  const std::string path = scratchFile("test_pad_phase_replay_resume.pad");
  writeBytes(path, menuRoute().encode());
  PadRecordReplay session;
  session.configure(
      PadSessionConfig{.recordPath = "", .replayPath = "", .resumePath = path, .windowed = false, .card = [] {
                         return CardSnapshot{cardImage(0x22), {}};
                       }});
  CHECK(session.replayPending());
  CHECK(session.fastForwarding());
  removeFile(path);
}

void test_a_keyed_recording_is_refused_by_a_title_without_phases() {
  const std::string path = scratchFile("test_pad_phase_replay_unkeyed.pad");
  writeBytes(path, menuRoute().encode());
  PadRecordReplay session;
  session.configure(
      PadSessionConfig{.recordPath = "", .replayPath = path, .resumePath = "", .windowed = false, .card = [] {
                         return CardSnapshot{cardImage(0x11), {}};
                       }});
  CHECK(session.replayPending());
  CHECK_EQ(session.service(kUnkeyedPhase, 0x1234u, kNeutralMask), 0x1234u);
  CHECK(!session.replayPending());
  removeFile(path);
}

void test_a_raw_file_on_the_replay_knob_is_refused_not_replayed() {
  const std::string path = scratchFile("test_pad_phase_replay_raw.pad");
  writeBytes(path, {0xFF, 0xFF, 0xF7, 0xFF});
  PadRecordReplay session;
  session.configure(
      PadSessionConfig{.recordPath = "", .replayPath = path, .resumePath = "", .windowed = false, .card = [] {
                         return CardSnapshot{};
                       }});
  CHECK(!session.replayPending());
  CHECK_EQ(session.service(kTitle, 0x1234u, kNeutralMask), 0x1234u);
  removeFile(path);
}

void test_the_record_sink_captures_what_a_replay_delivers_under_the_live_phases() {
  const std::string source = scratchFile("test_pad_phase_replay_src.pad");
  const std::string sink = scratchFile("test_pad_phase_replay_sink.pad");
  writeBytes(source, menuRoute().encode());
  {
    PadRecordReplay session;
    session.configure(
        PadSessionConfig{.recordPath = sink, .replayPath = source, .resumePath = "", .windowed = false, .card = [] {
                           return CardSnapshot{cardImage(0x11), {}};
                         }});
    for (const std::uint64_t phase : concat({repeat(kTitle, 10), repeat(kMenu, 5), repeat(kPlay, 4)})) {
      session.service(phase, kNeutralMask, kNeutralMask);
    }
    CHECK(!session.replayPending());
    CHECK_EQ(session.recording().totalFrames(), 19u);
  }
  const DecodedPadRecording decoded = PadRecording::decode(readBytes(sink));
  CHECK(decoded.recording.has_value());
  CHECK(decoded.recording->encode() == menuRoute().encode());
  removeFile(source);
  removeFile(sink);
}

void test_a_live_input_only_session_neither_sinks_nor_replays() {
  // A host-only screen (a title picker) runs before any game, so a default sink would rotate the
  // player's real capture and a replay would drive the pad with a file belonging to the title that
  // starts AFTER this screen. Both are refused here; the in-memory recording still runs, so the
  // control channel's `padrec save` keeps answering.
  const std::string sink = scratchFile("test_pad_phase_replay_live_only.pad");
  const std::string source = scratchFile("test_pad_phase_replay_live_only_src.pad");
  writeBytes(source, menuRoute().encode());
  PadRecordReplay session;
  session.configure(PadSessionConfig{
      .recordPath = sink,
      .replayPath = source,
      .resumePath = "",
      .windowed = true, // would open kDefaultSink if the session were not live-input-only
      .liveInputOnly = true,
      .card =
          [] {
            return CardSnapshot{cardImage(0x11), {}};
          },
  });
  // The live mask passes through untouched: nothing here owns the pad.
  for (const std::uint64_t phase : concat({repeat(kTitle, 3), repeat(kMenu, 2)})) {
    CHECK_EQ(session.service(phase, kNeutralMask, kNeutralMask), kNeutralMask);
  }
  CHECK(!session.replayPending());
  CHECK(!fileExists(sink));
  CHECK(!fileExists(scratchFile("test_pad_phase_replay_live_only.pad.saved")));
  // ...and the in-memory recording is still there, which is the half that must survive.
  CHECK_EQ(session.recording().totalFrames(), 5u);
  removeFile(source);
}

// ---- the Pad seam ----------------------------------------------------------------------------------

constexpr std::uint32_t kPhaseWord = 0x1000u;

class PhaseRuntime final : public GameRuntime {
public:
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
  RenderCapabilities renderCapabilities() const override {
    return RenderCapabilities::direct();
  }
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  std::uint64_t inputPhase(Core &core) const override {
    return core.mem_r32(kPhaseWord);
  }
};

void test_pad_records_the_title_phase_of_every_frame() {
  PhaseRuntime runtime;
  psxport_install_game(runtime);
  std::unique_ptr<Game> game = std::make_unique<Game>();
  game->core.mem_w32(kPhaseWord, 7u);
  game->pad.serviceFrame();
  game->pad.serviceFrame();
  game->core.mem_w32(kPhaseWord, 9u);
  game->pad.serviceFrame();
  CHECK_EQ(game->pad.recordedFrames(), 3u);

  const std::string path = scratchFile("test_pad_phase_replay_pad.pad");
  CHECK(game->pad.saveRecording(path.c_str(), 0));
  const DecodedPadRecording decoded = PadRecording::decode(readBytes(path));
  CHECK(decoded.recording.has_value());
  CHECK_EQ(decoded.recording->segments().size(), 2u);
  CHECK_EQ(decoded.recording->segments()[0].phase, 7u);
  CHECK_EQ(decoded.recording->segments()[0].frames(), 2u);
  CHECK_EQ(decoded.recording->segments()[1].phase, 9u);
  CHECK(decoded.recording->card().kind == CardIdentity::Kind::Image);
  removeFile(path);
}

} // namespace

int main() {
  RUN(segments_and_runs_are_recorded_by_phase);
  RUN(encode_decode_round_trips_header_segments_and_card);
  RUN(a_prefix_cuts_inside_a_run_and_never_offers_a_suffix);
  RUN(a_raw_pre_v1_recording_is_refused_and_names_the_migration);
  RUN(an_unknown_version_is_refused_by_number);
  RUN(malformed_bodies_are_refused_not_truncated);
  RUN(the_streaming_writer_matches_the_in_memory_encoding_byte_for_byte);
  RUN(a_longer_boot_moves_the_press_with_its_phase);
  RUN(a_shorter_phase_drops_only_neutral_frames_and_counts_it);
  RUN(leaving_a_phase_before_its_press_is_counted_as_dropped_input);
  RUN(a_phase_that_never_arrives_is_reported_not_skipped);
  RUN(a_skipped_phase_holding_input_is_never_passed_over);
  RUN(an_unobserved_neutral_phase_is_passed_and_counted);
  RUN(an_unkeyed_recording_replays_absolutely_from_boot);
  RUN(the_session_keeps_the_card_image_it_started_from);
  RUN(a_replay_against_a_different_card_is_refused);
  RUN(a_resume_against_a_changed_card_proceeds_and_fast_forwards);
  RUN(a_keyed_recording_is_refused_by_a_title_without_phases);
  RUN(a_raw_file_on_the_replay_knob_is_refused_not_replayed);
  RUN(the_record_sink_captures_what_a_replay_delivers_under_the_live_phases);
  RUN(a_live_input_only_session_neither_sinks_nor_replays);
  RUN(pad_records_the_title_phase_of_every_frame);
  return pt_summary();
}
