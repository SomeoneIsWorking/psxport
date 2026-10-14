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
// in this one process. An explicit executable, or a catalog of one title, skips the selector.
//
// Owns the command line and, once it asks for a run, what the process has one of: the window and its
// SDL_GPU device, the frame watchdog, the quit request and the run budget. Each outlives every
// session, so picking a title keeps the selector's window. Session teardown releases everything
// session-owned.
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
    explicit Process(const TitleCatalog &catalog);
    ~Process();
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    const psx::config::Config config;
    psxport::HostPresentation presentation;
    // The presentation keeps the title pointer, so the text lives as long as the process state.
    std::string windowTitle;
    psx::input::QuitScope quit;
    RunBudget budget;
    SessionServices services;
  };

  void printUsage(const char *program) const;
  Process &process();
  int runSelector();
  int runExecutable(const std::filesystem::path &executable);

  // The one loop every entered title runs in, whether it came from the picker or from an
  // explicit executable argument.
  // Returns how the session ended, or Refused when it could not boot.
  TitleSession::End runToEnd(TitleSession &session);

  const TitleCatalog &catalog_;
  std::unique_ptr<Process> process_;
};

} // namespace psx::host
