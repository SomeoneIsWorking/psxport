// SessionControl: how a running Game asks its host to end it and show the title selector again.
#pragma once

namespace psx::frame {

// The Game only records the request; the host's per-frame check ends the session by destroying it.
// The menu row "Return to Title Selection" and `session return` both land in requestReturn().
class SessionControl {
public:
  // Declared once, before the first frame, by a host that owns a selector.
  void setReturnAvailable(bool available) {
    mReturnAvailable = available;
  }
  bool returnAvailable() const {
    return mReturnAvailable;
  }
  // False, recording nothing, when there is no selector to return to.
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
