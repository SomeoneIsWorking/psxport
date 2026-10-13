#pragma once

#include "run_budget.h"
#include "title_availability.h"
#include "title_catalog.h"
#include "title_session.h"

#include "host_input.h"
#include "host_presentation.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace psx::host {

// The product's top level: the title selector, then the chosen title, then the selector again, all
// in this one process. An explicit executable skips the selector and runs that one title.
//
// Owns what the process has one of: the window and its SDL_GPU device, the frame watchdog, the quit
// request and the run budget. Each outlives every session, so picking a title keeps the selector's
// window. Session teardown releases everything session-owned.
class ProductHost {
public:
  // `catalog` must outlive the host; its product name is the window title.
  ProductHost(const TitleCatalog &catalog, std::filesystem::path provisioningRoot);
  ~ProductHost();
  ProductHost(const ProductHost &) = delete;
  ProductHost &operator=(const ProductHost &) = delete;

  int runSelector();
  int runExecutable(const std::filesystem::path &executable);

private:
  // The one loop every entered title runs in, whether it came from the picker or from an
  // explicit executable argument.
  // Returns how the session ended, or Refused when it could not boot.
  TitleSession::End runToEnd(TitleSession &session);

  const TitleCatalog &catalog_;
  std::filesystem::path root_;
  psxport::HostPresentation presentation_;
  // The presentation keeps the title pointer, so the text lives as long as the host.
  std::string windowTitle_;
  psx::input::QuitScope quit_;
  RunBudget budget_;
  SessionServices services_;
};

} // namespace psx::host
