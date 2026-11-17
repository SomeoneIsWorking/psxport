#include "first_run_setup.h"

#include "saved_disc.h"
#include "title_catalog.h"
#include "title_disc.h"
#include "title_selection.h"
#include "watchdog.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <lucent/log.h>
#include <lucent/platform.h>
#include <setup_ui/setup_ui.h>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

namespace psx::host {
namespace {

// The host's own file chooser; SDL answers on the thread that pumps events, so this pumps.
class DiscPicker {
public:
  std::optional<std::filesystem::path> choose() {
    SDL_ShowOpenFileDialog(&DiscPicker::selected,
                           this,
                           nullptr,
                           kFilters.data(),
                           static_cast<int>(kFilters.size()),
                           SDL_GetUserFolder(SDL_FOLDER_HOME),
                           false);
    while (!done_.load(std::memory_order_acquire)) {
      SDL_PumpEvents();
      SDL_Delay(10);
    }
    const std::lock_guard lock(mutex_);
    if (!error_.empty()) {
      lucent::warn("setup", "the file picker failed: {}", error_);
    }
    return selection_;
  }

private:
  static constexpr std::array<SDL_DialogFileFilter, 2> kFilters{{{"Disc image (CHD)", "chd"}, {"All files", "*"}}};

  static void SDLCALL selected(void *userdata, const char *const *files, int /*filter*/) {
    auto &picker = *static_cast<DiscPicker *>(userdata);
    {
      const std::lock_guard lock(picker.mutex_);
      if (files == nullptr) {
        picker.error_ = SDL_GetError();
      } else if (files[0] != nullptr) {
        picker.selection_ = std::filesystem::path(files[0]);
      }
    }
    picker.done_.store(true, std::memory_order_release);
  }

  std::atomic<bool> done_ = false;
  std::mutex mutex_;
  std::optional<std::filesystem::path> selection_;
  std::string error_;
};

#if defined(__ANDROID__)
// Android's picker answers with a content:// document, and libchdr reads a path, so the image is copied
// into the title's own storage first.
std::optional<std::filesystem::path> localCopy(const std::filesystem::path &document, const TitleIdentity &title) {
  const std::optional<std::filesystem::path> directory = lucent::platform::user_data_directory(title.slug);
  std::string error = "no user-data directory";
  if (!directory || !lucent::platform::ensure_user_data_directory(*directory, error)) {
    lucent::error("setup", "nowhere to keep the disc image: {}", error);
    return std::nullopt;
  }
  const std::filesystem::path target = *directory / "disc.chd";
  const std::filesystem::path pending = std::filesystem::path(target) += ".pending";
  SDL_IOStream *source = SDL_IOFromFile(document.c_str(), "rb");
  SDL_IOStream *sink = source != nullptr ? SDL_IOFromFile(pending.c_str(), "wb") : nullptr;
  bool copied = source != nullptr && sink != nullptr;
  std::vector<std::byte> block(std::size_t{4} << 20);
  while (copied) {
    const std::size_t read = SDL_ReadIO(source, block.data(), block.size());
    if (read == 0) {
      copied = SDL_GetIOStatus(source) == SDL_IO_STATUS_EOF;
      break;
    }
    copied = SDL_WriteIO(sink, block.data(), read) == read;
  }
  const std::string why = copied ? std::string() : std::string(SDL_GetError());
  if (source != nullptr) {
    SDL_CloseIO(source);
  }
  copied = sink != nullptr && SDL_CloseIO(sink) && copied;
  std::error_code renamed;
  if (copied) {
    std::filesystem::rename(pending, target, renamed);
  }
  if (!copied || renamed) {
    lucent::error("setup",
                  "the disc image could not be copied from {}: {}",
                  document.string(),
                  renamed ? renamed.message() : why);
    std::filesystem::remove(pending, renamed);
    return std::nullopt;
  }
  return target;
}
#endif

} // namespace

FirstRunSetup::FirstRunSetup(const TitleIdentity &title) : title_(title) {}

std::string FirstRunSetup::judge(const std::filesystem::path &image) const {
  TitleDisc disc(image);
  if (!disc.opened()) {
    return image.filename().string() + " is not a CHD disc image";
  }
  const SelectionResult selection = selectDiscExecutable(disc.state(), title_);
  if (!selection) {
    return image.filename().string() + " is not " + std::string(title_.displayName) + " (" +
           std::string(title_.serial) + "): " + selection.detail;
  }
  return {};
}

FirstRunSetup::Outcome FirstRunSetup::run(bool shown) const {
  const std::string name(title_.displayName);
  setup_ui::Config config;
  config.title = name + " setup";
  config.message = name + " needs your " + std::string(title_.serial) + " disc image.";
  config.hint = "A .chd image of the original disc. It is checked before anything is saved.";
  config.footer = "The original game is not included.";
  config.files = {setup_ui::FileSpec{.key = "disc", .name = "", .label = "Game disc"}};
  config.placement = setup_ui::Placement::Adopt;
  config.browse_label = "Choose disc image";
  config.accepted_message = "Disc accepted.";

  std::filesystem::path accepted;
  setup_ui::Session session(
      config, setup_ui::SessionOptions{}, [this, &accepted](const std::vector<setup_ui::StagedFile> &files) {
        accepted.clear();
        if (files.size() != 1) {
          return std::string("choose one disc image");
        }
        std::string reason = judge(files.front().path);
        if (reason.empty()) {
          accepted = files.front().path;
        }
        return reason;
      });
  setup_ui::ViewOptions options;
  options.window_title = config.title;
  options.offscreen = !shown;
  setup_ui::View view(session, options);
  if (!view.open()) {
    lucent::error("setup", "the {} setup screen could not open: {}", name, view.last_error());
    return Outcome::Unavailable;
  }
  if (!shown) {
    view.frame();
    view.close();
    lucent::error("setup",
                  "{} has no disc: set {} or PSXPORT_DISC, or set PSXPORT_SHOW_WINDOW=1 to choose one",
                  name,
                  title_.discEnvVar != nullptr ? title_.discEnvVar : "PSXPORT_DISC");
    return Outcome::Unavailable;
  }
  // The player may take as long as they like to find the disc.
  psx::platform::Watchdog::suspend();
  const Outcome outcome = choose(session, view, accepted);
  psx::platform::Watchdog::resume();
  return outcome;
}

FirstRunSetup::Outcome
FirstRunSetup::choose(setup_ui::Session &session, setup_ui::View &view, const std::filesystem::path &accepted) const {
  DiscPicker picker;
  while (view.running()) {
    for (const setup_ui::Request &request : view.poll()) {
      if (request.kind == setup_ui::RequestKind::Browse) {
        std::optional<std::filesystem::path> chosen = picker.choose();
#if defined(__ANDROID__)
        if (chosen) {
          chosen = localCopy(*chosen, title_);
        }
#endif
        if (chosen) {
          // A refusal is shown on the screen by the session.
          std::string refusal;
          session.add_selected({*chosen}, refusal);
        }
      } else if (request.kind == setup_ui::RequestKind::Cancel) {
        view.finish();
      }
    }
    // Judged on arrival, so a refusal is a line on the screen.
    session.validate_if_ready();
    if (session.status() == setup_ui::Status::Accepted) {
      view.frame();
      view.close();
      return SavedDisc(title_).save(accepted) ? Outcome::Accepted : Outcome::Unavailable;
    }
    view.frame();
  }
  view.close();
  lucent::info("setup", "{} setup was closed without a disc", title_.displayName);
  return Outcome::Declined;
}

} // namespace psx::host
