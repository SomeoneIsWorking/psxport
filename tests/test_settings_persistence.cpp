// A save must never land inside a source checkout. It did, and it cost a widescreen setting.
//
// WHY THIS FILE EXISTS
// --------------------
// `PSXPORT_SETTINGS` is BOTH the launch-configuration input and the persistence target, and every agent
// run and the launcher point it at a TRACKED file: `spyro/tools/shipping_settings.ini`. So opening the
// options overlay and changing any value called `Mods::save()`, which rewrote that tracked file with a
// full serialisation of the live mod state.
//
// MEASURED 2026-09-27: `spyro/tools/shipping_settings.ini` was found rewritten from `aspect=1` to
// `aspect=3`, with its 21 lines of documentation deleted, and an mtime coincident with a player run.
// `aspect=3` is `ASPECT_AUTO`, which resolves to the SINK's aspect — and a headless agent run has no wide
// sink, so it resolves to 4:3. **Every subsequent run, and the player's own, silently lost widescreen**,
// and the comment documenting precisely that hazard was the thing that got deleted.
//
// The fix is deliberately NOT "stop pointing PSXPORT_SETTINGS at the tracked file": the agent runs need
// that file as their configuration input, which is the entire reason it is tracked. Reading configuration
// out of the tree is correct. Writing a player's saved state into the tree is not, and the standing rule
// is explicit that settings live in the OS user-data location, never the checkout.
//
// WHAT MAKES THIS A TEST RATHER THAN A COMMENT
// --------------------------------------------
// A guard that has only ever been exercised in the direction that refuses is untested. So this file
// asserts BOTH answers on a real filesystem:
//
//   1. a save into a directory containing `.git` writes NOTHING and says so;
//   2. a save into an ordinary directory still WRITES, with the knobs serialised.
//
// Case 2 is the one that matters most and the one a naive "just refuse everything" fix would fail. A guard
// that breaks saving entirely would leave the player unable to keep their settings, which is a worse defect
// than the one being fixed, and it would not show up in any test that only checks the refusal.

#include "config.h"
#include "config_vars.h"
#include "mods.h"
#include "render_capabilities.h"
#include "testutil.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

// A checkout is identified by a `.git` entry in some ancestor, which is what makes a directory a working
// tree. Both fixtures below are real directories on disk, because the guard walks the filesystem and a
// mocked path would not test the walk.
struct Fixture {
  fs::path root;
  fs::path checkout_file;
  fs::path plain_file;

  explicit Fixture(const char *name) {
    root = fs::temp_directory_path() / ("psxport-settings-save-" + std::string(name));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path checkout = root / "checkout" / "tools";
    fs::create_directories(checkout);
    // A DIRECTORY named .git, not a file: `git worktree` and submodule checkouts both use a `.git` FILE,
    // so both spellings must be found, and a directory is the simpler of the two to create portably.
    fs::create_directories(root / "checkout" / ".git");
    checkout_file = checkout / "shipping_settings.ini";
    const fs::path plain = root / "userdata";
    fs::create_directories(plain);
    plain_file = plain / "psxport_settings.ini";
  }

  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};

void setPath(const fs::path &path) {
  psx::config::cv_settings_path.set_text(psx::config::Layer::Override, path.string());
}

bool existsNonEmpty(const fs::path &path) {
  std::error_code ignored;
  if (!fs::exists(path, ignored) || fs::file_size(path, ignored) == 0) {
    return false;
  }
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()).find('=') !=
         std::string::npos;
}

void test_a_save_into_a_checkout_writes_nothing() {
  Fixture fixture("refuse");
  setPath(fixture.checkout_file);
  Mods mods;
  mods.init(RenderCapabilities::interpolatedNative());
  mods.save();
  std::error_code ignored;
  CHECK(!fs::exists(fixture.checkout_file, ignored));
}

void test_a_save_outside_a_checkout_still_writes() {
  Fixture fixture("allow");
  setPath(fixture.plain_file);
  Mods mods;
  mods.init(RenderCapabilities::interpolatedNative());
  mods.save();
  CHECK(existsNonEmpty(fixture.plain_file));
}

void test_the_written_file_carries_the_knobs() {
  Fixture fixture("content");
  setPath(fixture.plain_file);
  Mods mods;
  mods.init(RenderCapabilities::interpolatedNative());
  mods.aspect = 1;
  mods.save();
  std::ifstream in(fixture.plain_file);
  const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(body.find("aspect=1") != std::string::npos);
  CHECK(body.find("fps60=") != std::string::npos);
}

void test_a_deeper_path_inside_a_checkout_is_still_refused() {
  // The walk has to climb more than one level: a real settings path is `repo/tools/x.ini`, and a guard
  // that only checked the immediate parent would pass this through and destroy the file.
  Fixture fixture("deep");
  setPath(fixture.checkout_file);
  Mods mods;
  mods.init(RenderCapabilities::interpolatedNative());
  mods.save();
  std::error_code ignored;
  CHECK(!fs::exists(fixture.checkout_file, ignored));
}

} // namespace

int main() {
  RUN(a_save_into_a_checkout_writes_nothing);
  RUN(a_save_outside_a_checkout_still_writes);
  RUN(the_written_file_carries_the_knobs);
  RUN(a_deeper_path_inside_a_checkout_is_still_refused);
  return pt_summary();
}
