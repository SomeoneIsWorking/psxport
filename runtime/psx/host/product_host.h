#pragma once

#include "watchdog.h"

#include "run_budget.h"
#include "title_availability.h"
#include "title_catalog.h"
#include "title_session.h"
#include "typed_config.h"

#include "host_input.h"
#include "render_device.h"

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace psx::host {

// The selector, then the chosen title, in one process; owns the window, device, watchdog and run budget.
class ProductHost {
public:
  // `catalog` must outlive the host; its product name is the window title.
  explicit ProductHost(const TitleCatalog &catalog);
  ~ProductHost();
  ProductHost(const ProductHost &) = delete;
  ProductHost &operator=(const ProductHost &) = delete;

  // `[executable]` or `-h`/`--help`; returns the process exit status. Help and refusals arm nothing.
  int run(int argc, char **argv);

private:
  // The process-wide state a run needs; the watchdog is armed for exactly its lifetime.
  struct Process {
    Process(const TitleCatalog &catalog, const psx::config::Config &launch);
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    const psx::config::Config config;
    psx::render::RenderDevice presentation;
    // The presentation keeps the title pointer, so the text lives as long as the process state.
    std::string windowTitle;
    psx::input::QuitScope quit;
    // The window's event queue has one reader for every session; destroyed before the window.
    psx::input::HostInput input;
    RunBudget budget;
    SessionServices services;
    psx::platform::Watchdog watchdog;
  };

  void printUsage(const char *program) const;
  Process &process();
  int runSelector();
  // The zero-argument run of a one-title product: its executable (from a disc asked for at first run), then the title.
  int runOnlyTitle();
  // A packaged run keeps every relative path (executable, settings, card) in the title's user data.
  bool enterUserData(const TitleIdentity &title, const std::filesystem::path &packageRoot) const;
  int runExecutable(const std::filesystem::path &executable);

  // The one loop every entered title runs in, whether it came from the picker or from an
  // explicit executable argument.
  // Returns how the session ended, or Refused when it could not boot.
  TitleSession::End runToEnd(TitleSession &session);

  const TitleCatalog &catalog_;
  const psx::config::Config config_;
  std::unique_ptr<Process> process_;
};

} // namespace psx::host
