
namespace psx::frame {
// SessionControl — how a running Game asks its host to end it and show the title selector again.
//
// A product that runs one Game after another (a picker, then a title, then the picker) owns the loop
// that constructs and destroys Games. The Game itself must not tear itself down or reach back into
// that loop, so it only records a REQUEST here, and the host's per-frame check ends the session through
// ordinary destruction. Nothing about the guest is touched: no phase, timer or scene pointer is written.
//
// The route is host-level on purpose: the ESC menu row "Return to Title Selection" and the control
// channel's `session return` both land in requestReturn(). A product without a selector never calls
// setReturnAvailable(true), so neither route exists for it (the menu row is removed, the command refuses).
#pragma once

class SessionControl {
public:
  // Declared once by the host that owns the selector, before the first frame.
  void setReturnAvailable(bool available) {
    mReturnAvailable = available;
  }
  bool returnAvailable() const {
    return mReturnAvailable;
  }
  // Returns false (and records nothing) when this product has no selector to return to.
  bool requestReturn() {
    if (!mReturnAvailable) {
      return false;
    }
    mReturnRequested = true;
    return true;
  }
  bool returnRequested() const {
    return mReturnRequested;
  }

private:
  bool mReturnAvailable = false;
  bool mReturnRequested = false;
};
} // namespace psx::frame
