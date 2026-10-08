// One live title session per available title, and the rules about which of them runs.
#pragma once

#include "panel_logo.h"
#include "title_availability.h"
#include "title_catalog.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

class Core;
class GpuDevice;

namespace psx::host {

class TitleSession;

class PanelSessions {
public:
  // One session per available title in catalog order. `titles` is the probed catalog and is not
  // retained: the sessions reference its available entries, which outlive the picker.
  PanelSessions(const TitleCatalog &catalog, std::span<const TitleAvailability> titles, GpuDevice &presentation);
  ~PanelSessions();
  PanelSessions(const PanelSessions &) = delete;
  PanelSessions &operator=(const PanelSessions &) = delete;

  int count() const {
    return static_cast<int>(sessions_.size());
  }
  // Panels are numbered by available title, so panel 0 is the first provisioned title in the
  // catalog whatever its slug.
  const TitleAvailability &title(int panel) const;
  // The panel showing this slug, or -1 when it is unprovisioned or unauthenticated.
  int panelOf(std::string_view slug) const;
  // The session behind a panel, or null. Null before boot() — which is also when it has no picture.
  TitleSession *session(int panel);
  // The same handle from a const host, for a caller that only asks what a panel is showing.
  const TitleSession *session(int panel) const;
  // The session behind a panel, if it has ever presented a picture.
  bool hasPicture(int panel) const;
  // The panels draw from this: black boot frames leave a panel empty, since an empty panel reads
  // as "not loaded" and a black one as "broken".
  bool hasVisiblePicture(int panel) const;
  // Whether the panel may be drawn: an unready panel is left empty, since an empty panel reads
  // as "not chosen" and a black one as "broken".
  bool panelReady(int panel) const;
  // A picture past the boot prefix that has pre-rolled a fixed number of steps.
  bool panelSettled(int panel) const;
  // The picture's own aspect, which decides how a panel is cover-cropped; 1:1 for an empty panel,
  // which the layout treats as the panel's own shape.
  int pictureWidth(int panel) const;
  int pictureHeight(int panel) const;
  // What PickerComposite draws.
  Core *core(int panel);
  // This panel's own logo, decoded once from its own session; a logo cannot outlive the machine
  // that decoded it.
  PanelLogo *logo(int panel);

  // Which demo the player watches and hears. advance() decides which session runs, since a panel
  // with no picture must be brought up first.
  void select(int panel);
  int selected() const {
    return selected_;
  }

  // One picker frame. Runs the selected session's step, or spends `bootStepBudget` steps bringing
  // the next panel that has no picture yet up to one. Returns true when a guest advanced.
  bool advance(int bootStepBudget);
  // Ends every session and hands back a fresh unbooted session for the chosen title: the demo was
  // a preview, so choosing a game must start it at power-on rather than resume its preview.
  std::unique_ptr<TitleSession> confirm();
  // Report what the sessions are doing, once per picker frame, at `lucent::debug`.
  void report() const;

private:
  // The panel to run this frame: the next one needing work, or the selected one when none do.
  int nextPanelNeedingWork();
  // Asked of the guest and not monotonic, so it only latches; see settled_.
  bool hasArrived(int panel) const;
  // Record that arrival, once.
  void latchArrival(int panel);
  // Read this panel's own logo out of that panel's own disc, once, on the first step the panel
  // runs. A no-op after the first success, and a no-op for a title whose logo has not been located.
  void takeLogo(int panel, TitleSession &session);

  const TitleCatalog &catalog_;
  std::vector<const TitleAvailability *> titles_;
  std::vector<std::unique_ptr<TitleSession>> sessions_;
  // The window and its device, kept because confirm() builds the fresh session the player gets.
  GpuDevice &presentation_;
  std::vector<bool> booted_;
  // Latches on arrival: readiness is a property of the panel having arrived once, not of what its
  // guest happens to be doing this instant.
  std::vector<bool> settled_;
  std::vector<std::unique_ptr<PanelLogo>> logos_;
  int selected_ = 0;
  int running_ = -1;
  // Where the boot rotation resumes, so panels that all need work take turns.
  int mBootCursor_ = 0;
};

} // namespace psx::host
