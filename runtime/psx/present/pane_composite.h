// pane_composite.h — psxport::PaneCompositor: one window frame assembled from several sessions' presented
// pictures. It draws rectangles, cuts their seams and tints; layout, timing and sessions belong to the host.
// The composite is built in both legs; only the window blit needs a window.
#pragma once

#include <SDL3/SDL_gpu.h>

#include "present_sink.h"

#include <cstdint>
#include <functional>
#include <span>

class Core;

namespace psx::render {
class RenderDevice;
} // namespace psx::render

namespace psxport {

// One pane: an upright rectangle (corner plus the vectors along its top and left edges) whose shape the
// seam lines cut in the fragment stage, so a leaning seam never shears the picture.
struct Pane {
  Core *core = nullptr; // the session whose picture fills this pane; null = draw nothing
  // A host-owned texture (a title's logo) drawn in place of the session's picture; null draws the picture.
  SDL_GPUTexture *texture = nullptr;
  float originX = 0.0f, originY = 0.0f; // sink pixels, y DOWN — the pane's top-left corner
  float axisUX = 1.0f, axisUY = 0.0f;   // across the top edge
  float axisVX = 0.0f, axisVY = 1.0f;   // down the LEFT edge
  // Sink columns of the left and right boundaries at the pane's top and bottom; neighbours sharing the
  // numbers share the line exactly.
  float seamLeftTop = 0.0f, seamLeftBottom = 0.0f;
  float seamRightTop = 0.0f, seamRightBottom = 0.0f;
  float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
  // Below 1 only for furniture drawn over a pane, such as a logo on a running demo.
  float alpha = 1.0f;
  float desaturation = 0.0f; // 0 = the picture's own colour, 1 = fully grey
  // A divider or backdrop in the tint, drawn from the compositor's white source.
  bool solid = false;
  // The part of the picture shown, as fractions of it; a cover-cropping pane narrows it.
  float sourceU = 0.0f, sourceV = 0.0f, sourceW = 1.0f, sourceH = 1.0f;
};

// The host's own UI, recorded over the panes in the same pass.
using PaneOverlayPass = std::function<void(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, int width, int height)>;

class PaneCompositor {
public:
  // `device` is up and outlives this. `hostCore` is the host's own session, which the window's events
  // feed; null leaves the event pump to the sessions.
  PaneCompositor(psx::render::RenderDevice &device, Core *hostCore);
  ~PaneCompositor();
  PaneCompositor(const PaneCompositor &) = delete;
  PaneCompositor &operator=(const PaneCompositor &) = delete;

  // Build one frame from `panes` into the composite image. A pane whose session has no presented picture
  // is skipped. `overlay` is recorded over the panes in the same pass; null draws host UI nowhere.
  void composite(std::span<const Pane> panes, const PaneOverlayPass &overlay = nullptr);
  // Show the last composite in the window; a paced host waits between composite() and this.
  void show();

  // The composited frame to a file; with no frame it writes nothing.
  void presentShot(const char *path);

  bool hasFrame() const {
    return m_image != nullptr;
  }
  int width() const {
    return m_imageW;
  }
  int height() const {
    return m_imageH;
  }

private:
  void ensureState();
  void ensureImage(psx::present::PixelSize size);

  psx::render::RenderDevice &m_device;
  Core *m_hostCore = nullptr;
  SDL_GPUGraphicsPipeline *m_panePipe = nullptr;
  SDL_GPUTexture *m_image = nullptr; // the composited frame
  SDL_GPUTexture *m_white = nullptr; // 1x1 source for a solid pane
  SDL_GPUTransferBuffer *m_imageRb = nullptr;
  psx::render::SinkIdleState m_sink;
  int m_imageW = 0;
  int m_imageH = 0;
  uint32_t m_frames = 0;
  bool m_showPending = false;
};

} // namespace psxport
