// renderdoc_capture.h — RenderDoc frame captures of chosen presents (PSXPORT_RENDERDOC_AT).
#pragma once

#include <cstdint>
#include <vector>

namespace psx::debug {

// A present has no swapchain frame in an unmapped window, so RenderDoc's own frame boundaries never come. When
// RenderDoc injected itself (`renderdoccmd capture`), each present PSXPORT_RENDERDOC_AT names is captured from its
// record rasterization to its present-build submit, through RenderDoc's in-app API. Linux only; elsewhere, and
// without RenderDoc, nothing happens.
class RenderDocCapture {
public:
  RenderDocCapture();
  // Around present `serial` (1-based, RenderState::presentSerial after it).
  void begin(std::uint64_t serial);
  void end();

private:
  struct Api;
  const Api *api_ = nullptr;
  std::vector<std::uint32_t> presents_;
  bool capturing_ = false;
};

} // namespace psx::debug
