// render_state.h — RenderState: one Game's present targets on the RenderDevice it presents through.
#pragma once
#include "pane_host.h"
#include "pixel_size.h"
#include "present_sink.h"
#include "record_rasterizer.h"
#include "render_pane.h"
#include "viewport_layout.h"

#include <cstdint>
#include <memory>
#include <span>

class Game;

namespace psx::present {
struct PresentInputs;
struct PresentPlan;
} // namespace psx::present

namespace psx::render {

class RenderDevice;

// A presented picture: its image and where the picture sits in it.
struct PresentedImage {
  SDL_GPUTexture *texture = nullptr;
  int width = 0;
  int height = 0;
  present::PaneRect viewport{0, 0, 0, 0};

  bool valid() const {
    return texture != nullptr && width > 0 && height > 0 && viewport.w > 0 && viewport.h > 0;
  }
};

class RenderState {
public:
  RenderState(Game &game, RenderDevice &device);
  // Releases this Game's device objects; the device outlives this.
  ~RenderState();
  RenderState(const RenderState &) = delete;
  RenderState &operator=(const RenderState &) = delete;

  RenderDevice &device() const {
    return device_;
  }
  // The device and this Game's overlay, brought up on first use.
  void bringUp();

  // A present is built, then shown. Building rasterizes the picture into the present image and submits
  // it; showing only acquires the swapchain image, blits the present image and presents it.
  // `display` is the guest display rect in VRAM at its native width.
  void buildDisplay(gpu::VramRect display);
  // A host RGBA8 picture (movie frame, splash) letterboxed 4:3 and faded by `fade`.
  void buildImage(const std::uint8_t *rgba, PixelSize size, float fade);
  // A host screen assembled from panes, drawn in order (render_panes.cpp).
  void buildPanes(std::span<const Pane> panes);
  // Shows the last build when it went to the window; nothing otherwise.
  void show();
  // Re-shows the last build without building anything (debug-server pause loop).
  void repaint();
  // What the player sees, either leg.
  void presentShot(const char *path);
  // The record picture, or the device's displayed VRAM.
  void shot(const char *path);
  // REPL `preseq`: the next `count` presents written as numbered PPMs.
  void preseqArm(int count, const char *dir);
  void preseqCapture();

  // Where a present goes: the window, or this Game's own image for a host compositing several sessions.
  enum class PresentTarget : std::uint8_t { Swapchain, Pane };
  PresentTarget presentTarget() const {
    return target_;
  }
  // Present into this Game's own image of `image` (0 = the sink's size) for a pane host.
  void presentToPane(PixelSize image);
  void presentToWindow();
  // The size this Game's present image is built at: a pane's requested size, else the sink's.
  PixelSize presentImageSize();
  // The host compositing this pane, told after each pane build and at each show.
  void setPaneHost(present::PaneHost host) {
    paneHost_ = std::move(host);
  }

  PresentedImage lastPresented() const {
    return {presentImage_, presentImageSize_.width, presentImageSize_.height, viewport_};
  }
  // What a pane shows: the live present image, unless the newest present was probed blank (a fade, a
  // load screen), then the retained copy.
  PresentedImage newestPresentedPicture() const;
  // Holds the newest presented frame that has a picture; true when one is held.
  bool retainFilledPresentImage();

private:
  void ensureVramTexture();
  void uploadDeviceVram(SDL_GPUCommandBuffer *cmd, std::span<const std::uint16_t> vram);
  void ensurePresentImage(PixelSize size);
  void ensureHostImage(PixelSize size);
  present::PresentInputs presentInputs(gpu::VramRect display, int nativeWidth);
  void presentRecord(present::PresentInputs &inputs);
  void checkRecord(const present::FrameRecord &record, bool drewComposed, bool deviceAhead, int scale);
  bool recordShot(const char *path);
  void buildPresentImage(SDL_GPUCommandBuffer *cmd, const present::PresentPlan &plan);
  void buildHostImage(SDL_GPUCommandBuffer *cmd, float fade);
  void showPresentImage(SDL_GPUCommandBuffer *cmd);
  // One readback of the present image: whether it holds a picture, and where.
  bool probePicture(const std::uint8_t *rgba);

  Game &game_;
  RenderDevice &device_;
  bool overlayBound_ = false;

  // The device VRAM image the Device path and a 24bpp display present from.
  SDL_GPUTexture *vramTexture_ = nullptr;
  SDL_GPUTransferBuffer *vramUpload_ = nullptr;
  // A host picture's source.
  SDL_GPUTexture *hostImage_ = nullptr;
  SDL_GPUTransferBuffer *hostUpload_ = nullptr;
  PixelSize hostImageSize_;

  // The presented picture at sink resolution: what the window shows and what a present shot reads.
  SDL_GPUTexture *presentImage_ = nullptr;
  SDL_GPUTransferBuffer *presentReadback_ = nullptr;
  PixelSize presentImageSize_;
  present::PaneRect viewport_{0, 0, 0, 0}; // where the picture sits in that image
  SinkIdleState sink_;

  PresentTarget target_ = PresentTarget::Swapchain;
  PixelSize requestedImage_; // zero = the sink's size
  present::PaneHost paneHost_;
  bool showPending_ = false;

  // The newest presented frame that held a picture, kept through fades and loads for a pane host.
  SDL_GPUTexture *filled_ = nullptr;
  PixelSize filledSize_;
  present::PaneRect filledViewport_{0, 0, 0, 0};
  std::uint64_t presentSerial_ = 0;
  std::uint64_t probedSerial_ = 0;
  std::uint64_t blankSerial_ = 0; // the newest present a probe found not to be a picture (issue 0162)

  // RenderPath::Record's rasterizer and what the last present took from it.
  std::unique_ptr<RecordRasterizer> record_;
  bool presentsRecord_ = false;
  int recordFade_[4] = {}; // mode, r, g, b
  // The last present's display rect in VRAM, for a Device-path shot.
  gpu::VramRect lastDisplay_{0, 0, 320, 240};

  int preseqLeft_ = 0;
  int preseqIndex_ = 0;
  char preseqDir_[128] = {0};
};

} // namespace psx::render
