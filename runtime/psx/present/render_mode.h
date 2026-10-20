// render_mode.h — RenderMode: the render path and the display-pass write guard for one Core.
#pragma once

#include <cstdint>
#include <strings.h>

namespace psx::present {

// Who rasterizes the guest's GPU work. Both paths take every word from the GPU device (Beetle).
enum class RenderPath : std::uint8_t {
  // The device's executed GP0 work replayed from the frame record into one VRAM image at scale.
  Record = 0,
  // The device's own display area, presented as is: the 4:3 reference.
  Device = 1,
};

inline const char *renderPathName(RenderPath p) {
  switch (p) {
  case RenderPath::Record:
    return "record";
  case RenderPath::Device:
    return "device";
  }
  return "?";
}

// False, leaving *out untouched, on anything it does not recognise.
inline bool parseRenderPath(const char *s, RenderPath *out) {
  if (!s || !*s || !out) {
    return false;
  }
  if (!strcasecmp(s, "record")) {
    *out = RenderPath::Record;
    return true;
  }
  if (!strcasecmp(s, "device")) {
    *out = RenderPath::Device;
    return true;
  }
  return false;
}

class RenderMode {
public:
  RenderPath path() const {
    return mPath;
  }
  void setPath(RenderPath p) {
    mPath = p;
  }

  // While armed, a guest-memory write aborts (mem.cpp): a host display pass reads guest state only.
  bool displayPassArmed() const {
    return mDisplayPassArmed;
  }
  void setDisplayPassArmed(bool on) {
    mDisplayPassArmed = on;
  }

private:
  RenderPath mPath = RenderPath::Record;
  bool mDisplayPassArmed = false;
};

// Arms RenderMode::displayPassArmed() for its lifetime and restores the prior value.
class DisplayPassGuard {
public:
  explicit DisplayPassGuard(RenderMode &mode) : mMode(mode), mPrev(mode.displayPassArmed()) {
    mMode.setDisplayPassArmed(true);
  }
  ~DisplayPassGuard() {
    mMode.setDisplayPassArmed(mPrev);
  }
  DisplayPassGuard(const DisplayPassGuard &) = delete;
  DisplayPassGuard &operator=(const DisplayPassGuard &) = delete;

private:
  RenderMode &mMode;
  bool mPrev;
};

} // namespace psx::present
