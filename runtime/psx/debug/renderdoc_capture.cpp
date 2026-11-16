// renderdoc_capture.cpp — the RenderDoc in-app API, declared here: the few entries used, in its ABI order.
#include "renderdoc_capture.h"

#include "diagnostic_config.h"

#include <lucent/log.h>

#include <algorithm>

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace psx::debug {

// RENDERDOC_API_1_1_2 (renderdoc_app.h): a table of function pointers, these the ones before EndFrameCapture.
struct RenderDocCapture::Api {
  using Fn = void (*)();
  Fn getApiVersion, setCaptureOptionU32, setCaptureOptionF32, getCaptureOptionU32, getCaptureOptionF32;
  Fn setFocusToggleKeys, setCaptureKeys, getOverlayBits, maskOverlayBits, removeHooks, unloadCrashHandler;
  Fn setCaptureFilePathTemplate, getCaptureFilePathTemplate, getNumCaptures, getCapture, triggerCapture;
  Fn isTargetControlConnected, launchReplayUi, setActiveWindow;
  void (*startFrameCapture)(void *device, void *window);
  std::uint32_t (*isFrameCapturing)();
  std::uint32_t (*endFrameCapture)(void *device, void *window);
};

namespace {

constexpr int kApiVersion_1_1_2 = 10102;

} // namespace

RenderDocCapture::RenderDocCapture() : presents_(psx::config::renderdoc_presents()) {
  if (presents_.empty()) {
    return;
  }
#if defined(__linux__)
  void *library = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
  using GetApi = int (*)(int, void **);
  auto *getApi = library != nullptr ? reinterpret_cast<GetApi>(dlsym(library, "RENDERDOC_GetAPI")) : nullptr;
  void *table = nullptr;
  if (getApi != nullptr && getApi(kApiVersion_1_1_2, &table) == 1) {
    api_ = static_cast<const Api *>(table);
    lucent::info("renderdoc", "capturing presents {}", presents_.size());
    return;
  }
#endif
  lucent::warn("renderdoc", "PSXPORT_RENDERDOC_AT is set but RenderDoc is not in this process; run it under renderdoccmd");
}

void RenderDocCapture::begin(std::uint64_t serial) {
  if (api_ == nullptr || std::find(presents_.begin(), presents_.end(), serial) == presents_.end()) {
    return;
  }
  // Null device and window: RenderDoc's only device, captured whatever window it has.
  api_->startFrameCapture(nullptr, nullptr);
  capturing_ = true;
  lucent::info("renderdoc", "present {} capture started", serial);
}

void RenderDocCapture::end() {
  if (!capturing_) {
    return;
  }
  capturing_ = false;
  const bool ok = api_->endFrameCapture(nullptr, nullptr) == 1;
  lucent::info("renderdoc", "capture {}", ok ? "written" : "failed");
}

} // namespace psx::debug
