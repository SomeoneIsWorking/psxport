#include "product_host.h"

#include "cfg.h"
#include "core.h"
#include "dbg_server.h"
#include "game.h"
#include "picker_content.h"
#include "picker_runtime.h"
#include "picker_session.h"
#include "title_availability.h"
#include "title_selection.h"
#include "title_session.h"

#include <lucent/log.h>

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

ProductHost::ProductHost(const TitleCatalog &catalog, std::filesystem::path provisioningRoot)
    : catalog_(catalog), root_(std::move(provisioningRoot)) {
  // The window belongs to the product, not to whichever Game brought the device up first.
  windowTitle_ = std::string(catalog_.productName());
  presentation_.setWindowTitle(windowTitle_.c_str());
}

int ProductHost::runSelector() {
  PickerRuntime pickerRuntime;
  const TitleAvailabilityProbe probe(root_, catalog_.titles());
  for (;;) {
    // Re-probed on every return: a title provisioned while another ran becomes available.
    const std::vector<TitleAvailability> titles = probe.probe();
    // Empty (the default) is every provisioned title.
    const std::vector<std::string> panelSlugs = splitSlugs(cfg_str("PSXPORT_PICKER_TITLES"));
    const PickerContent content(catalog_.productName(), titles, panelSlugs);
    PickerSession picker(pickerRuntime, catalog_, content, cfg_int("PSXPORT_PICKER_FRAMES", 0), presentation_.device());
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
    const int status = runToEnd(*choice.session);
    if (status != 0) {
      return status; // a boot refusal is not something the selector can retry
    }
    // Back to the top: the title finished or asked for the selector, and the selector is rebuilt —
    // with a fresh probe, so a title provisioned in the meantime gets a panel this time.
  }
}

int ProductHost::runToEnd(TitleSession &session) {
  if (!session.boot()) {
    lucent::error("boot", "{} could not be started", session.title().identity->displayName);
    return 2;
  }
  // The selector's Game died with the PickerSession above, so the channel would answer
  // nobody for the rest of the run.
  session.claimDebugEndpoint();
  while (session.end() == TitleSession::End::Running) {
    session.step();
  }
  // Destruction is the whole teardown; the window and its device survive into the next
  // session. Whether the run finished or returned to the selector is the caller's question.
  return 0;
}

int ProductHost::runExecutable(const std::filesystem::path &executable) {
  const SelectionResult selection = selectExecutableFile(executable, catalog_.titles());
  if (!selection) {
    lucent::error("boot", "{}", selection.detail);
    return 2;
  }
  TitleAvailability title;
  title.identity = selection.identity;
  title.index = selection.index;
  title.executable = executable;
  title.status = AvailabilityStatus::Available;
  TitleSession session(catalog_, title, false, presentation_.device());
  runToEnd(session);
  return 0;
}

} // namespace psx::host
