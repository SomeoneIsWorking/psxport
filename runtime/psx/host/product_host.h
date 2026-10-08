#pragma once

#include "title_availability.h"
#include "title_catalog.h"

#include "host_presentation.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace psx::host {

class TitleSession;

// The product's top level: the title selector, then the chosen title, then the selector again, all
// in this one process. An explicit executable skips the selector and runs that one title.
//
// Owns the window: one window and one SDL_GPU device for the process, created before the first
// session and released after the last, so picking a title keeps the selector's window. Session
// teardown releases everything session-owned.
class ProductHost {
public:
  // `catalog` must outlive the host; its product name is the window title.
  ProductHost(const TitleCatalog &catalog, std::filesystem::path provisioningRoot);

  int runSelector();
  int runExecutable(const std::filesystem::path &executable);

private:
  // The one loop every entered title runs in, whether it came from the picker or from an
  // explicit executable argument.
  int runToEnd(TitleSession &session);

  const TitleCatalog &catalog_;
  std::filesystem::path root_;
  psxport::HostPresentation presentation_;
  // The presentation keeps the title pointer, so the text lives as long as the host.
  std::string windowTitle_;
};

} // namespace psx::host
