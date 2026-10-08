#include "ui_assets.h"

#include "config_vars.h" // cv_asset_dir — PSXPORT_ASSET_DIR
#include "fs_util.h"     // Fs::exists — the framework's one host-filesystem wrapper

#include <lucent/log.h>

#include <filesystem>

namespace psx::ui {
namespace {

// The framework ships its RML/fonts here, relative to the directory PSXPORT_ASSET_DIR names.
constexpr const char *kAssetSubdir = "assets/rml";

std::string cwd_or_unknown() {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::current_path(ec);
  return ec ? std::string("<cwd unavailable: ") + ec.message() + ">" : p.string();
}

// Make the reported path absolute even when the knob is empty (cwd-relative), because a relative
// path in an error message is only actionable if the reader also knows the cwd — and quoting both
// separately is how "assets/rml/menu.rml not found" stayed unactionable for a whole issue.
std::string absolutize(const std::string &p) {
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(p, ec);
  return ec ? p : abs.lexically_normal().string();
}

} // namespace

bool AssetSet::open() {
  const std::string &base = psx::config::cv_asset_dir.get();
  mDir = absolutize(base.empty() ? std::string(kAssetSubdir) : base + "/" + kAssetSubdir);
  mOpened = true;
  mBaseOk = Fs::exists(mDir);
  if (!mBaseOk) {
    // ONE sentence that names the actual cause. Without this, a typo'd knob produced only a
    // stream of per-file misses and the reader had to infer that the directory itself was wrong.
    lucent::error("rmlui",
                  "ASSET DIRECTORY NOT FOUND: {} — the overlay has NO fonts and NO menu. "
                  "PSXPORT_ASSET_DIR={} (cwd {}). Set PSXPORT_ASSET_DIR to the directory "
                  "CONTAINING assets/ (e.g. external/psxport); run.sh exports it, so a binary "
                  "launched directly needs it in the environment.",
                  mDir,
                  base.empty() ? "<unset, so paths are cwd-relative>" : base,
                  cwd_or_unknown());
  }
  return mBaseOk;
}

std::string AssetSet::resolve(const char *rel) const {
  // mDir is set by open(); if the caller skipped open() this still produces a usable path rather
  // than an empty string, so an error message can never be about "".
  if (!mOpened) {
    const std::string &base = psx::config::cv_asset_dir.get();
    return absolutize((base.empty() ? std::string(kAssetSubdir) : base + "/" + kAssetSubdir) + "/" + rel);
  }
  return mDir + "/" + rel;
}

bool AssetSet::require(const char *rel, std::string &out_path) {
  out_path = resolve(rel);
  mWanted++;
  if (Fs::exists(out_path)) {
    mFound++;
    return true;
  }
  mMissing.push_back(out_path);
  // Only worth a per-file line when the directory itself is fine — otherwise open() already said
  // the one thing that matters and N copies of it are noise.
  if (mBaseOk) {
    lucent::error("rmlui", "asset MISSING: {}", out_path);
  }
  return false;
}

bool AssetSet::report() const {
  const bool ok = mMissing.empty() && mBaseOk;
  if (ok) {
    lucent::info("rmlui", "assets: {} of {} found in {}", mFound, mWanted, mDir);
    return true;
  }
  // The denominator is the point. "assets missing" alone cannot be told apart from "I never
  // looked"; "0 of 4 found in <dir>" names both the scope and the result.
  lucent::error("rmlui",
                "assets: {} of {} found in {} — {} MISSING",
                mFound,
                mWanted,
                mDir,
                mBaseOk ? (int)mMissing.size() : mWanted);
  for (const std::string &m : mMissing) {
    lucent::error("rmlui", "  missing: {}", m);
  }
  return false;
}

} // namespace psx::ui
