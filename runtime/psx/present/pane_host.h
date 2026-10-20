// pane_host.h — the host compositing a session's pane, told about each of that pane's presents.
#pragma once

#include <functional>

namespace psx::present {

// A pane present is built, paced and shown like a window present: `built` composites after the pane's
// build, `show` puts that composite in the window after the pane's wait.
struct PaneHost {
  std::function<void()> built;
  std::function<void()> show;

  bool attached() const {
    return built && show;
  }
};

} // namespace psx::present
