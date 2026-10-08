// pane_composite.h — class psxport::PaneCompositor: ONE window frame assembled from SEVERAL sessions'
// presented pictures.
//
// WHY THIS EXISTS. A host that runs more than one Game at a time — a title picker that shows every
// title's own attract demo — cannot present its window from inside any of them: each session's
// `present()` builds its OWN presented image (gpu_vk_internal.h), and the window belongs to the host
// (psxport::HostPresentation). Two sessions therefore cannot both own the frame, and neither may
// blit over the other. This class is the missing half: a session hands over its picture
// (`GpuVkState::lastPresented`) and the host says where each picture lands.
//
// WHAT IT IS NOT. It decides no geometry, no timing and no content: it draws rectangles, cuts their
// seams, and tints. The layout, the widths, the selection and the sessions themselves belong to the
// host. Every pane that names no session draws nothing — a session that has not presented yet has no
// frame, and a blank panel must not be able to pass for a title that booted.
//
// THE TWO LEGS, AS EVERYWHERE ELSE. The composite is BUILT into this object's own image in both legs;
// the window blit is the only leg-dependent step (headless has no window to reach), so a headless
// capture of a composited screen is a statement about the same frame the player sees.
#pragma once

#include <SDL3/SDL_gpu.h>

#include "gpu_present_sink.h" // SinkIdleState — the window may be unavailable; the sink never waits

#include <cstdint>
#include <functional>
#include <span>

class Core;
class GpuDevice;

namespace psxport {

// One pane of a composited frame: where a session's picture lands and how it looks there.
//
// The destination is an UPRIGHT RECTANGLE given as its top-left corner plus the two vectors that
// carry it — across the top (uAxisU) and down (uAxisV) — and the pane's SHAPE is cut out of that
// rectangle afterwards, in the fragment stage, by the two seam lines below.
//
// WHY NOT A SKEWED QUAD. A pane used to carry a third vector, down its RIGHT edge, so the whole
// quad could lean with the surface's seams. That shears whatever the pane shows: a guest's own
// upright geometry came out leaning, because the seam had been baked into the geometry instead of
// being drawn on top of it. So the shear moved to the mask. The seam is decoration over a picture,
// and only decoration belongs in the mask.
struct Pane {
  Core *core = nullptr; // the session whose picture fills this pane; null = draw nothing
  // A HOST-OWNED texture drawn in place of that picture: a title's own logo, extracted from its own
  // disc at runtime and decoded once. Null (the usual case) draws the session's presented image, so
  // this costs nothing for a pane that has no decoration of its own.
  SDL_GPUTexture *texture = nullptr;
  float originX = 0.0f, originY = 0.0f; // sink pixels, y DOWN — the pane's top-left corner
  float axisUX = 1.0f, axisUY = 0.0f;   // across the top edge
  float axisVX = 0.0f, axisVY = 1.0f;   // down the LEFT edge
  // THE SEAMS, in sink pixels: the column this pane's left boundary sits in at the TOP of the pane
  // and at the BOTTOM of it, and the same for the right. A boundary whose two values differ LEANS
  // down the pane; a pane whose seams are its own rectangle's edges draws all of it. Two neighbours
  // cut by the same numbers share one line exactly, which is the whole point of the seam being a
  // number in a uniform rather than geometry either of them owns.
  float seamLeftTop = 0.0f, seamLeftBottom = 0.0f;
  float seamRightTop = 0.0f, seamRightBottom = 0.0f;
  float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
  // 1 for a pane that OWNS its pixels (a picture, a divider, a backdrop). Below 1 only for furniture
  // drawn OVER a pane — a title's logo — which is what lets it sit on a running demo without hiding
  // it behind an opaque bar.
  float alpha = 1.0f;
  float desaturation = 0.0f; // 0 = the picture's own colour, 1 = fully grey
  // A SOLID pane: an authored line, frame or backdrop in `tintR/G/B`, drawn from this object's own
  // white source and naming no session at all. This is how a host draws the dividers between its
  // panes, and the backdrop a panel's picture is composed over — host furniture rather than a title's
  // picture, and giving them the SAME pipeline keeps one contract for "a shape in this colour"
  // instead of two.
  bool solid = false;
  // WHICH PART OF THE PICTURE this pane shows, as fractions of it: (0,0,1,1) is all of it. A pane that
  // cover-crops — the panel is a different shape from the picture, and the overflow must be dropped
  // rather than drawn past the pane into the neighbouring one — sets this; a pane that draws the whole
  // picture leaves it alone.
  float sourceU = 0.0f, sourceV = 0.0f, sourceW = 1.0f, sourceH = 1.0f;
};

// The host's own UI drawn OVER the composite, in the SAME pass — a title picker's screen is the
// picture of a state in which no single guest owns the window, so it cannot ride one session's
// present. It is handed the open pass and the composite's size, exactly as rmlui_render_gpu expects.
using PaneOverlayPass = std::function<void(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass, int width, int height)>;

class PaneCompositor {
public:
  // The device is the process's (psxport::HostPresentation); it must already be brought up
  // (`gpu_vk_ensure_device`) before the first composite, and it outlives this object. `hostCore` is
  // the host's OWN session — the one whose pad and overlay the window's events feed, and the only
  // Game in the process that is not a pane. Null when the host has no session of its own, which
  // leaves the event pump to the sessions.
  PaneCompositor(GpuDevice &device, Core *hostCore);
  ~PaneCompositor();
  PaneCompositor(const PaneCompositor &) = delete;
  PaneCompositor &operator=(const PaneCompositor &) = delete;

  // Build one frame from `panes` and show it. A pane whose session has no presented picture is
  // skipped. `overlay` is recorded over the panes in the same pass; null draws host UI nowhere.
  void composite(std::span<const Pane> panes, const PaneOverlayPass &overlay = nullptr);

  // Read the composited frame back to a file — WHAT THE PLAYER SEES, in either leg, the same contract
  // as `GpuVkState::present_shot`: with no image it says so and writes nothing, because a plausible
  // black picture is exactly how an instrument lies.
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
  void ensureImage(int w, int h);
  void showToWindow(SDL_GPUCommandBuffer *cmd);

  GpuDevice &m_device;
  Core *m_hostCore = nullptr;
  SDL_GPUGraphicsPipeline *m_panePipe = nullptr;
  SDL_GPUGraphicsPipeline *m_blitPipe = nullptr; // the composite → swapchain blit; windowed only
  SDL_GPUSampler *m_linear = nullptr;
  SDL_GPUTexture *m_image = nullptr; // the composited frame, RGBA8
  SDL_GPUTexture *m_white = nullptr; // 1x1 source for an authored line drawn through the pane pass
  SDL_GPUTransferBuffer *m_imageRb = nullptr;
  SinkIdleState m_sink; // idle/resume latch: a window nobody is showing is skipped, not waited on
  int m_imageW = 0, m_imageH = 0;
  uint32_t m_frames = 0;
};

} // namespace psxport
