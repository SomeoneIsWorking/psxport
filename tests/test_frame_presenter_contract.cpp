// The ordinary frame fence is a presentation service, not an interpolation service. This test drives
// the shipping psx::frame::FramePresenter state machine through its backend seam and poisons every temporal route
// by providing none: one commit must present, dump and pace exactly once.
#include "frame_presenter.h"
#include "record_raster_setup.h"
#include "source_tree.h"
#include "testutil.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class CountingBackend final : public psx::frame::FramePresentationBackend {
public:
  void presentReal() override {
    calls.emplace_back("present");
    ++presents;
  }
  void captureDiagnostic(uint64_t, bool interpolated) override {
    calls.emplace_back(interpolated ? "dump-interp" : "dump-real");
    ++diagnostics;
    CHECK(!interpolated);
  }
  void pace(psx::frame::FieldCadence cadence) override {
    calls.emplace_back("pace");
    pacedFields += cadence.fields;
    paceParts += cadence.parts;
  }
  bool interpolatesRecords() const override {
    return false;
  }
  bool sealedFrameIsCut() override {
    return false;
  }
  void presentInBetween() override {}
  psx::gpu::RecordRect displayedBuffer() override {
    return {};
  }

  std::vector<std::string> calls;
  int presents = 0;
  int diagnostics = 0;
  int pacedFields = 0;
  int paceParts = 0;
};

void test_neutral_commit_owns_the_complete_non_temporal_fence() {
  psx::frame::FramePresenter presenter;
  CountingBackend backend;
  presenter.commit(backend, 1);

  CHECK_EQ(backend.presents, 1);
  CHECK_EQ(backend.diagnostics, 1);
  CHECK_EQ(backend.pacedFields, 1);
  CHECK_EQ(backend.paceParts, 1);
  CHECK_EQ(presenter.fence(), 1u);

  const std::vector<std::string> expected = {"pace", "present", "dump-real"};
  CHECK_EQ(backend.calls.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(backend.calls[i] == expected[i]);
  }
}

std::string read_source(const std::filesystem::path &path) {
  std::ifstream stream(path);
  std::ostringstream out;
  out << stream.rdbuf();
  return out.str();
}

void test_neutral_game_header_does_not_include_or_embed_fps60() {
  const auto root = psx::test::sourceRoot();
  const std::string game = read_source(root / "runtime/psx/game/game.h");
  const std::string presenter = read_source(root / "runtime/psx/frame/frame_presenter.h");
  CHECK(game.find("#include \"fps60.h\"") == std::string::npos);
  CHECK(game.find("Fps60 fps60") == std::string::npos);
  CHECK(presenter.find("fps60.h") == std::string::npos);
  CHECK(presenter.find("lerp") == std::string::npos);
}

} // namespace

int main() {
  RUN(neutral_commit_owns_the_complete_non_temporal_fence);
  RUN(neutral_game_header_does_not_include_or_embed_fps60);
  return psx::test::summary();
}
