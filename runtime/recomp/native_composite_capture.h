// Native-composite capture policy. A title can request that the renderer retain the completed native
// composite before a subsequent frame rebuilds the live target (for example, a pause scene that
// backgrounds its UI with the preceding world frame). The renderer alone owns the SDL texture; this
// class owns only the per-GpuVkState request, validity, and extent contract.
#pragma once

struct NativeCompositeExtent {
  int width = 0;
  int height = 0;

  [[nodiscard]] bool valid() const {
    return width > 0 && height > 0;
  }
  [[nodiscard]] bool operator==(const NativeCompositeExtent &other) const {
    return width == other.width && height == other.height;
  }
  [[nodiscard]] bool operator!=(const NativeCompositeExtent &other) const {
    return !(*this == other);
  }
};

// The completed post-render target. This is part of the capture token rather than a second renderer
// latch: a request must copy the exact target whose completion fence made it valid.
enum class NativeCompositeSource {
  Native,
  Ires,
};

struct NativeCompositeCapturePlan {
  bool copy = false;
  bool allocate = false;
  NativeCompositeSource source = NativeCompositeSource::Native;
  NativeCompositeExtent extent{};
};

class NativeCompositeCapture {
public:
  // A request is only meaningful at a completed post-render fence. Rejecting an earlier request is
  // intentional: accepting it would make a pause capture whichever later scene first happened to
  // render. A successful new request invalidates the older retained picture until its replacement
  // copy is recorded.
  [[nodiscard]] bool request() {
    if (!sourceAvailable_) {
      return false;
    }
    requested_ = true;
    requestedSource_ = completedSource_;
    requestedExtent_ = completedExtent_;
    valid_ = false;
    return true;
  }

  // Called only after render_geom has completed a native composite. It intentionally does not alter a
  // retained capture: a later native frame is merely the source available to the next request.
  void noteCompletedComposite(NativeCompositeSource source, NativeCompositeExtent extent) {
    if (!extent.valid()) {
      sourceAvailable_ = false;
      completedExtent_ = {};
      return;
    }
    completedSource_ = source;
    completedExtent_ = extent;
    sourceAvailable_ = true;
  }

  [[nodiscard]] NativeCompositeCapturePlan plan() const {
    if (!requested_) {
      return {};
    }
    NativeCompositeCapturePlan result;
    result.copy = true;
    result.allocate = !resourceExtent_.valid() || resourceExtent_ != requestedExtent_;
    result.source = requestedSource_;
    result.extent = requestedExtent_;
    return result;
  }

  // The GPU copy completed in the command stream. Refuse a stale or fabricated plan; otherwise the
  // retained texture describes exactly the source selected at the completed fence.
  [[nodiscard]] bool didCapture(const NativeCompositeCapturePlan &plan) {
    if (!requested_ || !plan.copy || plan.source != requestedSource_ || plan.extent != requestedExtent_ ||
        !plan.extent.valid()) {
      return false;
    }
    resourceExtent_ = plan.extent;
    capturedExtent_ = plan.extent;
    requested_ = false;
    valid_ = true;
    return true;
  }

  // Device/target teardown invalidates both the opaque renderer resource and every source token
  // pointing at its now-released targets. No delayed request may survive a renderer lifetime.
  void resetResource() {
    requested_ = false;
    sourceAvailable_ = false;
    completedExtent_ = {};
    requestedExtent_ = {};
    resourceExtent_ = {};
    capturedExtent_ = {};
    valid_ = false;
  }

  [[nodiscard]] bool valid() const {
    return valid_;
  }
  [[nodiscard]] bool requested() const {
    return requested_;
  }
  [[nodiscard]] NativeCompositeExtent extent() const {
    return capturedExtent_;
  }

private:
  bool requested_ = false;
  bool sourceAvailable_ = false;
  bool valid_ = false;
  NativeCompositeSource completedSource_ = NativeCompositeSource::Native;
  NativeCompositeExtent completedExtent_{};
  NativeCompositeSource requestedSource_ = NativeCompositeSource::Native;
  NativeCompositeExtent requestedExtent_{};
  NativeCompositeExtent resourceExtent_{};
  NativeCompositeExtent capturedExtent_{};
};
