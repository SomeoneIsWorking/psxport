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

private:
  RenderPath mPath = RenderPath::Record;
};

} // namespace psx::present
