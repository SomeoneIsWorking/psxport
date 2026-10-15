// A delivered-but-deliberately-unpresented field advances the fence as commit() does, without
// reaching any output path.

#include "frame_presenter.h"
#include "record_raster_setup.h"
#include "testutil.h"

namespace {

class CountingBackend final : public psx::frame::FramePresentationBackend {
public:
  void presentReal() override {
    ++presents;
  }
  void captureDiagnostic(uint64_t, bool) override {
    ++diagnostics;
  }
  void pace(psx::frame::FieldCadence cadence) override {
    pacedFields += cadence.fields;
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

  int presents = 0;
  int diagnostics = 0;
  int pacedFields = 0;
};

void test_plain_commit_still_presents() {
  psx::frame::FramePresenter presenter;
  CountingBackend backend;
  presenter.commit(backend, 1);
  CHECK_EQ(backend.presents, 1);
  CHECK_EQ(backend.pacedFields, 1);
  CHECK_EQ(presenter.fence(), 1u);
}

void test_unpresented_fields_advance_the_fence_without_output() {
  psx::frame::FramePresenter presenter;
  CountingBackend backend;
  for (int field = 0; field < 8; ++field) {
    presenter.commitUnpresented();
  }
  CHECK_EQ(backend.presents, 0);
  CHECK_EQ(backend.diagnostics, 0);
  CHECK_EQ(backend.pacedFields, 0);
  CHECK_EQ(presenter.fence(), 8u);

  presenter.commit(backend, 1);
  CHECK_EQ(backend.presents, 1);
  CHECK_EQ(presenter.fence(), 9u);
}

} // namespace

int main() {
  RUN(plain_commit_still_presents);
  RUN(unpresented_fields_advance_the_fence_without_output);
  return psx::test::summary();
}
