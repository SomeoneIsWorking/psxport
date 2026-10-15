#include "product_host.h"

#include "c_subsys.h"
#include "cfg.h"
#include "config_vars.h"
#include "core.h"
#include "dbg_server.h"
#include "executable_provisioner.h"
#include "first_run_setup.h"
#include "game.h"
#include "memcensus.h"
#include "picker_content.h"
#include "picker_runtime.h"
#include "picker_session.h"
#include "title_availability.h"
#include "title_selection.h"
#include "title_session.h"

#include <lucent/log.h>
#include <lucent/platform.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace psx::host {
namespace {
// Split comma/space-separated slug list. Empty string means "every title".
std::vector<std::string> splitSlugs(const char *text) {
  if (text == nullptr) {
    return {};
  }
  std::vector<std::string> slugs;
  std::string current;
  for (const char *p = text; *p != '\0'; ++p) {
    if (*p == ',') {
      if (!current.empty()) {
        slugs.push_back(current);
      }
      current.clear();
      continue;
    }
    if (*p == ' ' || *p == '\t') {
      continue;
    }
    current.push_back(*p);
  }
  if (!current.empty()) {
    slugs.push_back(current);
  }
  return slugs;
}
} // namespace

ProductHost::Process::Process(const TitleCatalog &catalog, const psx::config::Config &launch)
    : config(launch), windowTitle(catalog.productName()), budget(RunBudget::fromConfig(config)),
      services{config, presentation.device(), quit, budget} {
  // The window belongs to the product, not to whichever Game brought the device up first.
  presentation.setWindowTitle(windowTitle.c_str());
  // The watchdog is process-wide signal state: armed once here, never by a session.
  watchdog_init();
  memcensus_init();
  cfg_dump();
  lucent::info("cfg", "launch config: {}", config.describe());
}

ProductHost::Process::~Process() {
  watchdog_disable();
}

ProductHost::ProductHost(const TitleCatalog &catalog)
    : catalog_(catalog), config_(psx::config::Config::fromEnvironment()) {}

ProductHost::~ProductHost() = default;

ProductHost::Process &ProductHost::process() {
  if (process_ == nullptr) {
    process_ = std::make_unique<Process>(catalog_, config_);
  }
  return *process_;
}

int ProductHost::run(int argc, char **argv) {
  const char *program = argc > 0 ? argv[0] : "product";
  if (config_.packageRoot) {
    psx::config::applyPackagedDefaults(*config_.packageRoot);
  }
  if (argc <= 1) {
    return catalog_.titles().size() == 1 ? runOnlyTitle() : runSelector();
  }
  const std::string_view argument = argc == 2 ? argv[1] : "";
  if (argument == "-h" || argument == "--help") {
    printUsage(program);
    return 0;
  }
  if (argc == 2 && !argument.starts_with('-')) {
    return runExecutable(argument);
  }
  lucent::error("cli", "unexpected arguments; see {} --help", program);
  return 2;
}

void ProductHost::printUsage(const char *program) const {
  const std::span<const TitleIdentity> titles = catalog_.titles();
  lucent::info("cli", "Usage: {} [executable]", std::filesystem::path(program).filename().string());
  if (titles.size() == 1) {
    lucent::info("cli",
                 "With no argument: {} from {}.",
                 titles.front().displayName,
                 titles.front().defaultExecutable(catalog_.provisioningRoot()).string());
  } else {
    lucent::info("cli",
                 "With no argument: the {} title selector over {}, then the chosen title, in this process.",
                 catalog_.productName(),
                 catalog_.provisioningRoot().string());
  }
  lucent::info("cli", "With an executable: run exactly that serial-identified executable (maintainer override).");
  lucent::info("cli", "Titles:");
  for (const TitleIdentity &title : titles) {
    lucent::info("cli", "  {} ({})", title.displayName, title.serial);
  }
  lucent::info("cli", "Options: -h, --help  Show this help and exit.");
}

int ProductHost::runSelector() {
  Process &state = process();
  PickerRuntime pickerRuntime;
  for (const TitleIdentity &title : catalog_.titles()) {
    provisionExecutable(title, title.defaultExecutable(catalog_.provisioningRoot()));
  }
  const TitleAvailabilityProbe probe(catalog_.provisioningRoot(), catalog_.titles());
  for (;;) {
    // Re-probed on every return: a title provisioned while another ran becomes available.
    const std::vector<TitleAvailability> titles = probe.probe();
    // Empty (the default) is every provisioned title.
    const std::vector<std::string> panelSlugs = splitSlugs(cfg_str("PSXPORT_PICKER_TITLES"));
    const PickerContent content(catalog_.productName(), titles, panelSlugs);
    PickerSession picker(pickerRuntime, catalog_, content, state.services);
    const PickerSession::Result choice = picker.run();
    if (choice.outcome != PickerSession::Outcome::Chosen || choice.session == nullptr) {
      return 0;
    }
    // The panel's demo was a preview; the player chose the title, not the moment its
    // attract loop had reached.
    lucent::info("host",
                 "starting {} ({})",
                 choice.session->title().identity->displayName,
                 choice.session->title().identity->serial);
    const TitleSession::End end = runToEnd(*choice.session);
    if (end == TitleSession::End::Refused) {
      return 2; // a boot refusal is not something the selector can retry
    }
    if (end != TitleSession::End::ReturnedToSelector && end != TitleSession::End::TitleEnded) {
      return 0; // a quit or the spent run budget ends the process
    }
    // Back to the top, with a fresh probe, so a title provisioned in the meantime gets a panel.
  }
}

int ProductHost::runOnlyTitle() {
  const TitleIdentity &only = catalog_.titles().front();
  if (config_.packageRoot && !enterUserData(only)) {
    return 2;
  }
  const std::filesystem::path executable = only.defaultExecutable(catalog_.provisioningRoot());
  if (provisionExecutable(only, executable) == Provisioning::Unavailable) {
    switch (FirstRunSetup(only).run(psx::config::windowRequested())) {
    case FirstRunSetup::Outcome::Accepted:
      break;
    case FirstRunSetup::Outcome::Declined:
      return 0;
    case FirstRunSetup::Outcome::Unavailable:
      return 2;
    }
    provisionExecutable(only, executable);
  }
  return runExecutable(executable);
}

bool ProductHost::enterUserData(const TitleIdentity &title) const {
  const std::optional<std::filesystem::path> directory = lucent::platform::user_data_directory(title.slug);
  std::string error = "no user-data directory";
  if (!directory || !lucent::platform::ensure_user_data_directory(*directory, error)) {
    lucent::error("host", "{} cannot keep its data: {}", title.displayName, error);
    return false;
  }
  std::error_code entered;
  std::filesystem::current_path(*directory, entered);
  if (entered) {
    lucent::error("host", "cannot enter {}: {}", directory->string(), entered.message());
    return false;
  }
  lucent::info("host", "packaged run from {}; data in {}", config_.packageRoot->string(), directory->string());
  return true;
}

TitleSession::End ProductHost::runToEnd(TitleSession &session) {
  if (!session.boot()) {
    lucent::error("boot", "{} could not be started", session.title().identity->displayName);
    return TitleSession::End::Refused;
  }
  // The selector's Game died with the PickerSession above, so the channel would answer
  // nobody for the rest of the run.
  session.claimDebugEndpoint();
  while (session.end() == TitleSession::End::Running) {
    session.step();
  }
  // Destruction is the whole teardown; the window and its device survive into the next session.
  return session.end();
}

int ProductHost::runExecutable(const std::filesystem::path &executable) {
  SelectionResult selection = selectExecutableFile(executable, catalog_.titles());
  if (!selection) {
    lucent::error("boot", "{}", selection.detail);
    return 2;
  }
  TitleAvailability title;
  title.identity = selection.identity;
  title.index = selection.index;
  title.executable = executable;
  title.bytes = std::move(selection.bytes);
  title.status = AvailabilityStatus::Available;
  TitleSession session(catalog_, title, TitleSession::Role::Product, false, process().services);
  return runToEnd(session) == TitleSession::End::Refused ? 2 : 0;
}

} // namespace psx::host
