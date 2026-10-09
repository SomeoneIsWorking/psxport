#pragma once

#include "picker_composite.h"
#include "picker_content.h"
#include "picker_layout.h"
#include "picker_runtime.h"
#include "title_catalog.h"

#include "gpu_vk_device.h"

#include <memory>
#include <span>
#include <vector>

class Core;
class Game;
class GpuDevice;

namespace psx::host {

class PanelSessions;
class TitleSession;

// The title selector: the product state in which several games are alive at once, each showing its
// own picture in its own panel.
//
// Exactly one guest advances per frame, only the selected session is heard, no panel's guest sees
// the player's pad, a panel with no picture draws nothing, and confirming destroys every panel
// session and hands the caller a fresh unbooted one for the chosen title.
//
// `presentation` (the process's window and device) must outlive this session.
class PickerSession {
public:
  enum class Outcome : std::uint8_t { Chosen, Ended };
  struct Result {
    Outcome outcome = Outcome::Ended;
    std::unique_ptr<TitleSession> session; // the chosen title's LIVE session, set when Chosen
  };

  // `content` names the panels (available entries in catalog order); both outlive this session. `frameCap` of 0 runs
  // until a title is chosen.
  PickerSession(PickerRuntime &runtime,
                const TitleCatalog &catalog,
                const PickerContent &content,
                int frameCap,
                GpuDevice &presentation);
  // Out of line because the result owns a session: a unique_ptr over a forward-declared type can
  // only be destroyed where that type is complete.
  ~PickerSession();

  Result run();

private:
  // Read the pad and the control channel for one frame and act on them. Returns the chosen panel,
  // or -1 to keep the selector up.
  int serviceInput(Game &host);

  PickerRuntime &runtime_;
  const TitleCatalog &catalog_;
  const PickerContent &content_;
  int frameCap_;
  GpuDevice &presentation_;
  PickerLayout layout_;
  std::unique_ptr<PickerComposite> composite_;
  std::unique_ptr<PanelSessions> panels_;
  std::vector<PanelSource> sources_;
};

} // namespace psx::host
