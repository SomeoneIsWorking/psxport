// gpu_vk_internal.h — GpuVkState: one Game's present targets on the SDL GPU device it presents through.
// Internal to the gpu_vk units; the Core-facing entry points are gpu_vk.h.
#ifndef GPU_GPU_INTERNAL_H
#define GPU_GPU_INTERNAL_H
#include "gpu_present_sink.h"     // SinkIdleState — the sink may not wait for the window
#include "gpu_vk_record_raster.h" // RecordRasterizer — RenderPath::Record's picture
#include "pane_host.h"
#include "pixel_size.h"
#include "viewport_layout.h" // PaneRect — where the presented picture sits in its image

#include <cstdint>
#include <memory>
#include <span>

class Game;
class GpuDevice;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;
struct SDL_GPUCommandBuffer;
struct PresentInputs;
struct PresentPlan;

struct GpuVkState {
  Game *game = nullptr;        // set by Game()
  GpuDevice *device = nullptr; // set by Game(): the device this Game presents through

  // The device VRAM image the Device path and a 24bpp display present from.
  SDL_GPUTexture *s_vram_tex = nullptr;
  SDL_GPUTransferBuffer *s_vram_xfer = nullptr;
  void ensure_targets();
  void upload_device_vram(SDL_GPUCommandBuffer *cmd, std::span<const std::uint16_t> vram);
  // The device and this Game's overlay, brought up on first use.
  void bring_up();
  int s_overlay_bound = 0;

  // The presented picture at sink resolution: what the window shows and what a present shot reads.
  SDL_GPUTexture *s_present_img = nullptr;
  SDL_GPUTransferBuffer *s_present_rb = nullptr; // s_present_img → host download (present shot)
  SinkIdleState s_sink;                          // idle/resume latch for an unavailable swapchain image
  int s_present_img_w = 0, s_present_img_h = 0;
  PaneRect s_present_viewport{0, 0, 0, 0}; // where the picture sits inside that image (the letterbox)
  PaneRect s_present_content{0, 0, 0, 0};  // the picture's own lit extent, widened by every filled probe
  void measurePresentedContent(const std::uint8_t *rgba, psx::present::PixelSize size);
  void ensure_present_img(int w, int h);

  // Where a present goes: the window, or this Game's own image for a host compositing several sessions.
  enum class PresentTarget : std::uint8_t { Swapchain, Pane };
  PresentTarget s_present_target = PresentTarget::Swapchain;
  void setPresentTarget(PresentTarget target) {
    s_present_target = target;
  }
  PresentTarget presentTarget() const {
    return s_present_target;
  }
  psx::present::PixelSize s_present_img_requested; // zero = the sink's size
  // Present into this Game's own image of the given size (0 = the sink's) for a pane host, or back to the window.
  void presentToPane(psx::present::PixelSize image);
  void presentToWindow();

  // The host compositing this pane: told after each pane build and at each show, so an in-between is
  // composited and shown like any other present.
  psx::present::PaneHost s_pane_host;
  void setPaneHost(psx::present::PaneHost host) {
    s_pane_host = std::move(host);
  }

  struct PresentedImage {
    SDL_GPUTexture *texture = nullptr;
    int width = 0, height = 0;
    PaneRect viewport{0, 0, 0, 0};
    PaneRect content{0, 0, 0, 0};
    bool valid() const {
      return texture != nullptr && width > 0 && height > 0 && viewport.w > 0 && viewport.h > 0;
    }
  };
  PresentedImage lastPresented() const {
    return PresentedImage{s_present_img, s_present_img_w, s_present_img_h, s_present_viewport, s_present_content};
  }

  // The newest presented frame that held a picture, kept through fades and loads for a pane host.
  SDL_GPUTexture *s_present_filled = nullptr;
  int s_present_filled_w = 0, s_present_filled_h = 0;
  std::uint64_t s_present_serial = 0;
  std::uint64_t s_present_probed_serial = 0;
  PaneRect s_present_filled_viewport{0, 0, 0, 0};
  bool retainFilledPresentImage();
  void releaseFilledPresentImage();
  // The newest present a probe found NOT to be a picture: a fade, a load screen, black. Issue 0162.
  std::uint64_t s_present_blank_serial = 0;
  // The retained copy: a pane's fallback for a frame that is not a picture, refreshed on a gate.
  PresentedImage retainedPicture() const {
    return PresentedImage{
        s_present_filled, s_present_filled_w, s_present_filled_h, s_present_filled_viewport, s_present_content};
  }
  // What a pane shows: the live present image, which the panel's own present just rebuilt and which
  // costs nothing to sample, unless the newest present was probed blank — then the retained copy.
  PresentedImage newestPresentedPicture() const {
    if (s_present_serial != s_present_blank_serial) {
      return lastPresented();
    }
    return retainedPicture();
  }

  // RenderPath::Record's rasterizer and what the last present took from it.
  std::unique_ptr<psx::gpu::RecordRasterizer> s_record;
  bool s_present_record = false; // this present samples s_record's presented picture
  int s_present_fade[4] = {};    // the record present's fade: mode, r, g, b
  int s_present_ires = 0;
  // The last present's display rect in VRAM, for a Device-path shot.
  int s_last_sx = 0, s_last_sy = 0, s_last_w = 320, s_last_h = 240;

  // REPL `preseq`: the next N presents written as numbered PPMs.
  int s_preseq_left = 0, s_preseq_idx = 0;
  char s_preseq_dir[128] = {0};
  void preseq_arm(int count, const char *dir);
  void preseq_capture();

  // Every device object this Game created, while the device is alive.
  void release_device_resources();
  ~GpuVkState() = default;

  // A present is built, then shown. Building rasterizes the picture into the present image and submits
  // that GPU work; showing only acquires the swapchain image, blits the present image and presents it.
  void build_display(int sx, int sy, int w, int h);
  // A host RGBA8 picture (movie frame, splash) built the same way, letterboxed 4:3 and faded by `fade`.
  void build_image(const std::uint8_t *rgba, psx::present::PixelSize size, float fade);
  // Shows the last build when it went to the window; nothing otherwise.
  void show();
  bool s_show_pending = false;
  void repaint();
  void present_record(PresentInputs &inputs);
  bool record_shot(const char *path);
  void build_present_image(SDL_GPUCommandBuffer *cmd, const PresentPlan &plan);
  void show_present_image(SDL_GPUCommandBuffer *cmd);
  void build_image_present_image(SDL_GPUCommandBuffer *cmd, float fade);
  void present_shot(const char *path); // what the player sees, either leg
  void shot(const char *path);         // the record picture, or the device's displayed VRAM
};

#endif // GPU_GPU_INTERNAL_H
