// rmlui_library.h — RmlUi's process state on one RenderDevice: the render and system interfaces and
// the one Rml::Initialise/Shutdown, released before the device is destroyed.
#pragma once

#include <memory>
#include <string>

class RmlRenderInterfaceGpu;
struct SDL_Window;

namespace Rml {
class Context;
class SystemInterface;
} // namespace Rml

namespace psx::render {
class RenderDevice;
} // namespace psx::render

namespace psx::ui {

class RmlUiLibrary {
public:
  explicit RmlUiLibrary(render::RenderDevice &device);
  ~RmlUiLibrary();
  RmlUiLibrary(const RmlUiLibrary &) = delete;
  RmlUiLibrary &operator=(const RmlUiLibrary &) = delete;

  bool ready() const {
    return ready_;
  }
  RmlRenderInterfaceGpu &renderer() const {
    return *renderer_;
  }
  // Null headless.
  SDL_Window *window() const {
    return window_;
  }
  // A context name no overlay of this process used before: Rml::CreateContext refuses a registered one.
  std::string nextContextName(const char *prefix);
  int fontsLoaded() const {
    return fontsLoaded_;
  }
  static constexpr int kFontCount = 3;
  // Shows or hides RmlUi's debugger over `context`; it starts on first use, never for a player.
  void toggleDebugger(Rml::Context *context);

private:
  SDL_Window *window_ = nullptr;
  std::unique_ptr<RmlRenderInterfaceGpu> renderer_;
  std::unique_ptr<Rml::SystemInterface> system_;
  unsigned long long contextSerial_ = 0;
  int fontsLoaded_ = 0;
  bool ready_ = false;
  bool debuggerUp_ = false;
};

} // namespace psx::ui
