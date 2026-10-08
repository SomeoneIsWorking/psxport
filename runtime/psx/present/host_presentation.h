// host_presentation.h — class psxport::HostPresentation: the ONE window and ONE SDL_GPU device a
// whole process presents through.
//
// WHY THIS EXISTS. `GpuDevice` used to be a member of `Game`, so a process that ran one Game after
// another — a title selector, then a title, then the selector again — destroyed the window and the
// device between every pair of sessions and built a new one for the next. A user saw a program open a
// window for the picker, close it, and open a second window for the game: the selector looked like a
// separate program that happened to quit. The device half of `Game` is per-PROCESS (one window, one
// device, one set of shared pipelines — see gpu_vk_device.h), so its lifetime is the host's.
//
// The owner. A product constructs this ONCE, at the top of its run, and hands a reference to every
// session. Each session constructs `Game(device())`, presents into it, and is destroyed; the window
// outlives all of them and is released once, by this object. `GpuDevice` is untouched and still
// releases itself in its own destructor — this class exists to decide WHEN that happens, and to own
// the window identity the first Game's device was created with.
//
// Nothing else may own a presentation device in such a process. `Game`'s default constructor still
// makes a device of its own for the one-Game-per-process case, which is why it stays available.
#pragma once

#include "gpu_vk_device.h"

namespace psxport {

class HostPresentation {
public:
  HostPresentation() = default;
  ~HostPresentation() = default;
  HostPresentation(const HostPresentation &) = delete;
  HostPresentation &operator=(const HostPresentation &) = delete;

  // The device every Game in this process presents through. Pass it to `Game`'s presentation
  // constructor. Valid from construction until this object dies, whether or not a window was ever
  // opened (headless legs create no window and no Game).
  GpuDevice &device() {
    return device_;
  }

  // The title bar's text. The window is created by the FIRST Game that brings the device up, and it
  // used to take that Game's `HostIdentity` — which, in a selector-then-title product, is the
  // selector's and names the window for the rest of the run. A host that owns the window owns its
  // name: set this before the first present and it wins. Null (the default) keeps the historical
  // behaviour of naming the window after the first Game that presents. The string must outlive this
  // object; a `static constexpr` literal is the intended argument.
  void setWindowTitle(const char *title) {
    device_.s_window_title = title;
  }
  const char *windowTitle() const {
    return device_.s_window_title;
  }

private:
  GpuDevice device_;
};

} // namespace psxport