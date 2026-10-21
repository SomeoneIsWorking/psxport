#include "rmlui_library.h"

#include "render_device.h"
#include "rmlui_render_gpu.h"
#include "ui_assets.h"

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <lucent/log.h>

#include "RmlUi_Platform_SDL.h"

namespace psx::ui {

RmlUiLibrary::RmlUiLibrary(render::RenderDevice &device)
    : window_(device.window().window()), renderer_(std::make_unique<RmlRenderInterfaceGpu>(device)) {
  system_ = std::make_unique<SystemInterface_SDL>(window_);
  Rml::SetSystemInterface(system_.get());
  Rml::SetRenderInterface(renderer_.get());
  ready_ = Rml::Initialise();
  if (!ready_) {
    lucent::error("rmlui", "Rml::Initialise failed; overlay disabled");
    return;
  }
  // Font faces are process state: loaded once, shared by every overlay context.
  constexpr const char *const kFonts[kFontCount] = {
      "FiraSans-Regular.ttf",
      "FiraSans-Bold.ttf",
      "FiraSansCondensed-Bold.ttf",
  };
  AssetSet assets;
  assets.open();
  for (const char *font : kFonts) {
    std::string path;
    if (!assets.require(font, path)) {
      continue;
    }
    if (Rml::LoadFontFace(path.c_str())) {
      ++fontsLoaded_;
    } else {
      lucent::error("rmlui", "font present but rejected by the font engine: {}", path);
    }
  }
  assets.report();
}

void RmlUiLibrary::toggleDebugger(Rml::Context *context) {
  if (!debuggerUp_) {
    debuggerUp_ = Rml::Debugger::Initialise(context);
  } else {
    Rml::Debugger::SetContext(context);
  }
  Rml::Debugger::SetVisible(debuggerUp_ && !Rml::Debugger::IsVisible());
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
