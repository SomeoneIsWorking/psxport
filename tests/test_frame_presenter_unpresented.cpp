// A delivered-but-deliberately-unpresented diff-mode field must rotate the presenter's bookkeeping
// exactly as commit() does, without reaching any output path.

#include "../runtime/recomp/frame_presenter.h"
#include "testutil.h"

#include <span>

namespace {

class CountingBackend final : public FramePresentationBackend {
public:
  void emit(std::span<const RqItem> items) override {
    emitted += static_cast<int>(items.size());
  }
  void presentReal() override {
    ++presents;
  }
  void captureDiagnostic(uint64_t, bool) override {
    ++diagnostics;
  }
  void pace(int guestFields, int) override {
    pacedFields += guestFields;
  }
  void reconcile(uint64_t fence) override {
    lastFence = fence;
    ++reconciles;
  }
  void beginLedgerFrame() override {
    ++ledgerResets;
  }

  int emitted = 0;
  int presents = 0;
  int diagnostics = 0;
  int pacedFields = 0;
  int reconciles = 0;
  int ledgerResets = 0;
  uint64_t lastFence = 0;
};

RqItem item(unsigned sequence) {
  RqItem result{};
  result.seq = sequence;
  return result;
}

void test_plain_commit_still_presents() {
  FramePresenter presenter;
  const RqItem batch[] = {item(0), item(1)};
  presenter.capture(batch, 2);
  CountingBackend backend;
  presenter.commit(backend, 1);
  CHECK_EQ(backend.presents, 1);
  CHECK_EQ(backend.emitted, 2);
  CHECK_EQ(backend.pacedFields, 1);
  CHECK_EQ(presenter.capturedCount(), 0);
  CHECK_EQ(presenter.fence(), 1u);
}

void test_unpresented_field_rotates_bookkeeping_without_output() {
  FramePresenter presenter;
  const RqItem batch[] = {item(10), item(11), item(12)};
  presenter.capture(batch, 3);
  CountingBackend backend;
  presenter.commitUnpresented(backend);
  CHECK_EQ(backend.presents, 0);
  CHECK_EQ(backend.emitted, 0);
  CHECK_EQ(backend.diagnostics, 0);
  CHECK_EQ(backend.pacedFields, 0);
  CHECK_EQ(backend.reconciles, 1);
  CHECK_EQ(backend.ledgerResets, 1);
  CHECK_EQ(presenter.capturedCount(), 0);
  CHECK_EQ(presenter.fence(), 1u);
}

void test_unpresented_window_does_not_leak_capture_into_next_present() {
  FramePresenter presenter;
  CountingBackend backend;
  for (unsigned field = 0; field < 8; ++field) {
    const RqItem hidden[] = {item(field * 16), item(field * 16 + 1)};
    presenter.capture(hidden, 2);
    presenter.commitUnpresented(backend);
  }
  CHECK_EQ(backend.emitted, 0);
  CHECK_EQ(backend.reconciles, 8);

  const RqItem visible[] = {item(999)};
  presenter.capture(visible, 1);
  presenter.commit(backend, 1);
  CHECK_EQ(backend.emitted, 1);
  CHECK_EQ(backend.presents, 1);
  CHECK_EQ(presenter.capturedCount(), 0);
  CHECK_EQ(presenter.fence(), 9u);
}

} // namespace

int main() {
  RUN(plain_commit_still_presents);
  RUN(unpresented_field_rotates_bookkeeping_without_output);
  RUN(unpresented_window_does_not_leak_capture_into_next_present);
  return pt_summary();
}
