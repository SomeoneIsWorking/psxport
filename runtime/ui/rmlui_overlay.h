// rmlui_overlay.h — RmlOverlay: one Game's RmlUi context and menu document on the device's RmlUiLibrary.
// The UI itself lives in psx::ui::MenuDocument.
#pragma once
#include "pixel_size.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <memory>
#include <string>

class Game;

namespace bug_report {
class Draft;
class Form;
} // namespace bug_report

namespace psx::ui {
class MenuDocument;
class RmlUiLibrary;
} // namespace psx::ui

class RmlOverlay {
public:
  Game *game = nullptr; // back-pointer wired by Game()

  RmlOverlay();
  ~RmlOverlay();
  RmlOverlay(const RmlOverlay &) = delete;
  RmlOverlay &operator=(const RmlOverlay &) = delete;

  // A context laid out for `sink` on `library`, whose window (null headless) feeds input only. No-op once up.
  void init(psx::ui::RmlUiLibrary &library, psx::present::PixelSize sink);
  void shutdown();

  // Feed every SDL event (ESC toggles the menu; F1 debugger). Safe if not inited.
  void event(const SDL_Event *e);
  // CPU-side per-frame update (RmlUi context + live row/readout refresh). Safe if not inited.
  void newFrame();
  // Record the menu geometry into the present render pass. No-op when menu is hidden / not inited.
  void recordGpu(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h);

  // Up is not "there is a menu": a failed LoadDocument leaves a live context with no document.
  bool inited() const {
    return mInited;
  }
  bool hasMenu() const {
    return mMenu != nullptr;
  }
  bool visible() const;
  void setVisible(bool v);
  void setOptionsMode(bool v) {
    mOptionsMode = v;
  }

  // pad_input suppresses gameplay keyboard input while this returns true.
  bool wantsKeyboard() const;
  // A text field has the keyboard (the bug report form, or a focused input): HostInput then treats
  // P / '.' / B as letters rather than debug commands.
  bool typingText() const;

  // ---- the bug report form (shared/bug-report) -----------------------------------------------------
  // A modal document in the menu context, drawn over the window like the menu (never into the present
  // image, so a present shot is the game's picture). While it is open every event goes to it, ESC
  // included. Null when the overlay is not up. The draft must outlive the form; `closeBugReport`
  // before the draft is committed or discarded.
  bug_report::Form *showBugReport(const bug_report::Draft &draft);
  void closeBugReport();

  // Headless driving (REPL `menu ...`): the menu without SDL keyboard events.
  void selectTab(int index);
  bool sendKey(SDL_Keycode key); // false = the menu did not claim that key
  void dumpMenu() const;         // enumerate every tab/pane/row + its live value

private:
  bool mInited = false;
  bool mOptionsMode = false; // stands in for the game's in-game Options menu

  psx::ui::RmlUiLibrary *mLibrary = nullptr;
  void *mCtx = nullptr; // Rml::Context*, kept out of this header

  // The UI. Null when LoadDocument failed — which is exactly what hasMenu() reports.
  std::unique_ptr<psx::ui::MenuDocument> mMenu;

  std::unique_ptr<bug_report::Form> mBugReport;
};
