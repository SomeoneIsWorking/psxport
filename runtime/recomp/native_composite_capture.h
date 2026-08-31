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

struct NativeCompositeRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] bool valid() const {
    return width > 0 && height > 0;
  }
  [[nodiscard]] bool operator==(const NativeCompositeRect &other) const {
    return x == other.x && y == other.y && width == other.width && height == other.height;
  }
  [[nodiscard]] bool operator!=(const NativeCompositeRect &other) const {
    return !(*this == other);
  }
};

// The completed post-render target. This is part of the capture token rather than a second renderer
// latch: a request must copy the exact target whose completion fence made it valid.
enum class NativeCompositeSource {
  Native,
  Ires,
};

// The completed display rectangle inside the renderer's current composite. Captures are deliberately
// display-rect scoped: the rest of a VRAM-sized target can hold texture pages and must never become a
// pause backdrop. `scale` and `source` identify the target geometry that can safely receive it later.
struct NativeCompositeFrame {
  NativeCompositeSource source = NativeCompositeSource::Native;
  NativeCompositeExtent canvas{};
  NativeCompositeRect sourceRect{};
  int scale = 0;

  [[nodiscard]] bool valid() const {
    if (!canvas.valid() || !sourceRect.valid() || scale <= 0 || sourceRect.x < 0 || sourceRect.y < 0 ||
        sourceRect.x + sourceRect.width > canvas.width || sourceRect.y + sourceRect.height > canvas.height) {
      return false;
    }
    return source == NativeCompositeSource::Ires ? scale > 1 : scale == 1;
  }
  [[nodiscard]] bool operator==(const NativeCompositeFrame &other) const {
    return source == other.source && canvas == other.canvas && sourceRect == other.sourceRect && scale == other.scale;
  }
  [[nodiscard]] bool operator!=(const NativeCompositeFrame &other) const {
    return !(*this == other);
  }
};

struct NativeCompositeCapturePlan {
  bool copy = false;
  bool allocate = false;
  NativeCompositeFrame frame{};
  NativeCompositeExtent captureExtent{};
};

// The next render consumes a retained capture only when it has the identical canvas, source rectangle,
// and scale. A mismatch explicitly discards it: later scene changes must not resurrect a stale backdrop.
struct NativeCompositeBasePlan {
  bool blit = false;
  bool refused = false;
  NativeCompositeRect destination{};
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
    requestedFrame_ = completedFrame_;
    valid_ = false;
    capturedFrame_ = {};
    return true;
  }

  // Called only after render_geom has completed a native composite. It intentionally does not alter a
  // retained capture: a later native frame is merely the source available to the next request.
  void noteCompletedComposite(NativeCompositeFrame frame) {
    if (!frame.valid()) {
      sourceAvailable_ = false;
      completedFrame_ = {};
      return;
    }
    completedFrame_ = frame;
    sourceAvailable_ = true;
  }

  [[nodiscard]] NativeCompositeCapturePlan plan() const {
    if (!requested_) {
      return {};
    }
    NativeCompositeCapturePlan result;
    result.copy = true;
    result.frame = requestedFrame_;
    result.captureExtent = {requestedFrame_.sourceRect.width, requestedFrame_.sourceRect.height};
    result.allocate = !resourceExtent_.valid() || resourceExtent_ != result.captureExtent;
    return result;
  }

  // The GPU copy completed in the command stream. Refuse a stale or fabricated plan; otherwise the
  // retained texture describes exactly the display rectangle selected at the completed fence.
  [[nodiscard]] bool didCapture(const NativeCompositeCapturePlan &plan) {
    const NativeCompositeExtent expected{requestedFrame_.sourceRect.width, requestedFrame_.sourceRect.height};
    if (!requested_ || !plan.copy || plan.frame != requestedFrame_ || plan.captureExtent != expected ||
        !plan.captureExtent.valid()) {
      return false;
    }
    resourceExtent_ = plan.captureExtent;
    capturedExtent_ = plan.captureExtent;
    capturedFrame_ = plan.frame;
    requested_ = false;
    valid_ = true;
    return true;
  }

  [[nodiscard]] NativeCompositeBasePlan takeBase(NativeCompositeFrame target) {
    if (!valid_) {
      return {};
    }
    if (!target.valid() || target != capturedFrame_) {
      valid_ = false;
      capturedFrame_ = {};
      return {.refused = true};
    }
    valid_ = false;
    NativeCompositeBasePlan result;
    result.blit = true;
    result.destination = capturedFrame_.sourceRect;
    capturedFrame_ = {};
    return result;
  }

  // Device/target teardown invalidates both the opaque renderer resource and every source token
  // pointing at its now-released targets. No delayed request may survive a renderer lifetime.
  void resetResource() {
    requested_ = false;
    sourceAvailable_ = false;
    completedFrame_ = {};
    requestedFrame_ = {};
    resourceExtent_ = {};
    capturedExtent_ = {};
    capturedFrame_ = {};
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
  NativeCompositeFrame completedFrame_{};
  NativeCompositeFrame requestedFrame_{};
  NativeCompositeExtent resourceExtent_{};
  NativeCompositeExtent capturedExtent_{};
  NativeCompositeFrame capturedFrame_{};
};
