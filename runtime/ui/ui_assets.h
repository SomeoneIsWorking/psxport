// psx::ui::AssetSet — resolve the overlay's disk assets, and REFUSE TO REPORT SUCCESS WHEN THEY
// ARE NOT THERE.
//
// THE DEFECT THIS EXISTS FOR (spyro issue #52): the overlay disk-loads its fonts and `menu.rml`
// from `<PSXPORT_ASSET_DIR>/assets/rml/`. `run.sh` exports that variable, so launching
// `scratch/bin/<port>` directly — which every agent and every developer does — resolved every
// asset against the cwd, found nothing, and CARRIED ON. The user got a port with no menu and a log
// that said the overlay was up. A loader that cannot find its corpus and returns "fine" is the
// canonical lying diagnostic: silence was indistinguishable from success.
//
// SO THE NEGATIVE IS DESIGNED FIRST, and it is what this class is really for:
//
//   * every failure NAMES THE ABSOLUTE PATH IT TRIED, plus the cwd it resolved against and the
//     value of PSXPORT_ASSET_DIR. "Not found" without the path is a bug report nobody can action.
//   * the BASE is checked once, before any individual asset. A typo'd PSXPORT_ASSET_DIR otherwise
//     produces four separate "missing file" lines and never the one sentence that matters — the
//     whole directory is wrong.
//   * the summary carries its DENOMINATOR — `loaded N of M` — so a partial load is a visible
//     failure. The old code was `if (!loaded) warn(...)`: loading 1 font of 3 printed NOTHING, and
//     a menu rendered in a fallback face looked like a styling bug rather than a missing asset.
//
// WHAT IT DELIBERATELY DOES NOT DO: abort. The overlay is developer/mod UI, and killing the port
// because a debug menu's font is missing would be a worse failure than the one being fixed. "Loud"
// here means an `error`-level line naming the path (always printed, not channel-gated) plus
// `RmlOverlay::hasMenu()` going false so every downstream question — ESC, `wantsKeyboard()`, the
// REPL `menu` command — answers honestly instead of pretending.
#ifndef PSXPORT_UI_ASSETS_H
#define PSXPORT_UI_ASSETS_H

#include <string>
#include <vector>

namespace psx::ui {

class AssetSet {
public:
  // Resolve `<PSXPORT_ASSET_DIR>/assets/rml/` and check it exists. Logs one error naming the
  // resolved directory, the cwd and the knob when it does not. Call once before loading anything.
  // Returns false if the directory is missing — every subsequent resolve() will also fail, and
  // the caller should say "no menu" rather than attempt four doomed loads.
  bool open();

  // Absolute path for `rel` (e.g. "menu.rml") inside the asset directory. Always returns a path,
  // even if `open()` failed, so an error message can name what WOULD have been tried.
  std::string resolve(const char *rel) const;

  // resolve() + an existence check. Records the miss for report() and logs it with its path.
  bool require(const char *rel, std::string &out_path);

  // One summary line carrying the denominator: `assets: N of M found in <dir>`. Emitted at
  // error level when anything is missing, info otherwise. Returns true if nothing was missing.
  bool report() const;

  const std::string &dir() const {
    return mDir;
  }
  bool base_ok() const {
    return mBaseOk;
  }
  int found() const {
    return mFound;
  }
  int wanted() const {
    return mWanted;
  }

private:
  std::string mDir;
  std::vector<std::string> mMissing;
  int mFound = 0;
  int mWanted = 0;
  bool mBaseOk = false;
  bool mOpened = false;
};

} // namespace psx::ui

#endif
