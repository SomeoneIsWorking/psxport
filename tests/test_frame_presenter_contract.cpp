// The ordinary frame fence is a presentation service, not an interpolation service. This test drives
// the shipping FramePresenter state machine through its backend seam and poisons every temporal route
// by providing none: one captured frame must emit/present/pace/reconcile/reset exactly once.
#include "frame_presenter.h"
#include "testutil.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class CountingBackend final : public FramePresentationBackend {
public:
  void emit(std::span<const RqItem> items) override {
    calls.emplace_back("emit");
    emitted += (int)items.size();
  }
  void presentReal() override {
    calls.emplace_back("present");
    ++presents;
  }
  void captureDiagnostic(uint64_t, bool interpolated) override {
    calls.emplace_back(interpolated ? "dump-interp" : "dump-real");
    ++diagnostics;
    CHECK(!interpolated);
  }
  void pace(int guestFields, int parts) override {
    calls.emplace_back("pace");
    pacedFields += guestFields;
    paceParts += parts;
  }
  void reconcile(uint64_t) override {
    calls.emplace_back("reconcile");
    ++reconciles;
  }
  void beginLedgerFrame() override {
    calls.emplace_back("ledger-reset");
    ++ledgerResets;
  }

  std::vector<std::string> calls;
  int emitted = 0;
  int presents = 0;
  int diagnostics = 0;
  int pacedFields = 0;
  int paceParts = 0;
  int reconciles = 0;
  int ledgerResets = 0;
};

void test_neutral_commit_owns_the_complete_non_temporal_fence() {
  FramePresenter presenter;
  RqItem first{};
  RqItem second{};
  first.seq = 0;
  second.seq = 1;
  const RqItem captured[] = {first, second};
  presenter.capture(captured, 2);

  CountingBackend backend;
  presenter.commit(backend, 1);

  CHECK_EQ(backend.emitted, 2);
  CHECK_EQ(backend.presents, 1);
  CHECK_EQ(backend.diagnostics, 1);
  CHECK_EQ(backend.pacedFields, 1);
  CHECK_EQ(backend.paceParts, 1);
  CHECK_EQ(backend.reconciles, 1);
  CHECK_EQ(backend.ledgerResets, 1);
  CHECK_EQ(presenter.capturedCount(), 0);
  CHECK_EQ(presenter.fence(), 1u);

  const std::vector<std::string> expected = {"emit", "present", "dump-real", "pace", "reconcile", "ledger-reset"};
  CHECK_EQ(backend.calls.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(backend.calls[i] == expected[i]);
  }
}

void test_capture_accumulates_flushes_and_rebases_sequence_once() {
  FramePresenter presenter;
  RqItem a{};
  RqItem b{};
  RqItem c{};
  a.seq = 0;
  b.seq = 1;
  c.seq = 0;
  const RqItem first[] = {a, b};
  const RqItem second[] = {c};
  presenter.capture(first, 2);
  presenter.capture(second, 1);

  const auto frame = presenter.capturedFrame();
  CHECK_EQ(frame.items.size(), 3u);
  CHECK_EQ(frame.items[0].seq, 0u);
  CHECK_EQ(frame.items[1].seq, 1u);
  CHECK_EQ(frame.items[2].seq, 2u);
  CHECK_EQ(frame.items[0].flush_ordinal, 0u);
  CHECK_EQ(frame.items[2].flush_ordinal, 1u);
}

std::string read_source(const std::filesystem::path &path) {
  std::ifstream stream(path);
  std::ostringstream out;
  out << stream.rdbuf();
  return out.str();
}

void test_neutral_game_header_does_not_include_or_embed_fps60() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::string game = read_source(root / "runtime/psx/game.h");
  const std::string presenter = read_source(root / "runtime/psx/frame_presenter.h");
  CHECK(game.find("#include \"fps60.h\"") == std::string::npos);
  CHECK(game.find("Fps60 fps60") == std::string::npos);
  CHECK(presenter.find("fps60.h") == std::string::npos);
  CHECK(presenter.find("lerp") == std::string::npos);
}

} // namespace

int main() {
  RUN(neutral_commit_owns_the_complete_non_temporal_fence);
  RUN(capture_accumulates_flushes_and_rebases_sequence_once);
  RUN(neutral_game_header_does_not_include_or_embed_fps60);
  return pt_summary();
}
