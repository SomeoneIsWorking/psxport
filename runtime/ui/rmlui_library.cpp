#include "rmlui_library.h"

#include "render_device.h"
#include "rmlui_render_gpu.h"

#include <RmlUi/Core.h>
#include <lucent/log.h>

#include "RmlUi_Platform_SDL.h"

namespace psx::ui {

RmlUiLibrary::RmlUiLibrary(render::RenderDevice &device)
    : window_(device.window().window()), renderer_(std::make_unique<RmlRenderInterfaceGpu>(device)) {
  // RmlUi's SDL system interface needs a window; headless, the base interface supplies the clock.
  system_ =
      window_ != nullptr ? std::make_unique<SystemInterface_SDL>(window_) : std::make_unique<Rml::SystemInterface>();
  Rml::SetSystemInterface(system_.get());
  Rml::SetRenderInterface(renderer_.get());
  ready_ = Rml::Initialise();
  if (!ready_) {
    lucent::error("rmlui", "Rml::Initialise failed; overlay disabled");
  }
}

RmlUiLibrary::~RmlUiLibrary() {
  if (ready_) {
    Rml::Shutdown();
  }
}

std::string RmlUiLibrary::nextContextName(const char *prefix) {
  return std::string(prefix) + "#" + std::to_string(++contextSerial_);
}

} // namespace psx::ui
