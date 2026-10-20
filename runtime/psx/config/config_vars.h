// The product and run knobs, read through these typed accessors. Diagnostic knobs are in diagnostic_config.h.
#pragma once
#include "fallback_policy.h"
#include "override_differential_config.h"
#include "render_mode.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace psx::config {

bool no_audio();        // PSXPORT_NOAUDIO: open no audio device
bool producer_census(); // PSXPORT_PRODUCERS: arm the producer census (ot_attr.h)

// PSXPORT_DEBUG_SERVER: the loopback debug port, 1 meaning 5959; 0 when off or not a port.
int debug_server_port();
// PSXPORT_STORE_OBSERVE: hex PCs of guest store instructions (not the addresses they write); nullopt if malformed.
std::optional<std::vector<std::uint32_t>> store_observe_pcs();
// PSXPORT_RAMDUMP: write guest RAM here when the product loop returns.
std::string ramdump_path();
// PSXPORT_LOAD_STATE: a machine state to resume before the first field; one that cannot load refuses the run.
std::string load_state_path();
// PSXPORT_BUG_REPORT_DIR: B-key bug reports; unset saves under the title's user-data directory.
std::string bug_report_dir();

// PSXPORT_PAD_RECORD / _REPLAY / _RESUME: the phase-keyed .pad sink, real-speed replay, and fast-forwarded resume.
std::string pad_record_path();
std::string pad_replay_path();
std::string pad_resume_path();

long watchdog_seconds(); // PSXPORT_WATCHDOG: frame-progress timeout in seconds, 0 off
long spin_ticks();       // PSXPORT_SPIN_TICKS: guest instructions per spin-detector sample, 0 off
long spin_runs();        // PSXPORT_SPIN_RUNS: consecutive starved in-region samples that declare a spin
// PSXPORT_WATCHDOG_BOOT: the first present's grace; -1 derives max(PSXPORT_WATCHDOG, 45).
long watchdog_boot_seconds();

long native_frames(); // PSXPORT_NATIVE_FRAMES: run cap in presentation-fence steps, 0 the run budget's default

std::string asset_dir(); // PSXPORT_ASSET_DIR: the directory containing assets/, empty for cwd

// PSXPORT_VK_WINDOW unless PSXPORT_VK_HEADLESS.
bool windowRequested();
// A packaged product's defaults: a window, and the assets under `<packageRoot>/usr/share/psxport`.
void applyPackagedDefaults(const std::filesystem::path &packageRoot);

std::string settings_path(); // PSXPORT_SETTINGS: the settings file; selects the Value layer, so never saved

// PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT: interpreter-fallback blocks one bounded execution may admit.
psx::cpu::FallbackPolicy lightrec_fallback_policy();
// PSXPORT_OVERRIDE_DIFF (+ _FIRST, _EVERY, _DEAD_STACK, _REPORT): the override differential; empty selectors disarm.
psx::cpu::OverrideDifferentialConfig override_differential_config();
std::string function_reach_report_path(); // PSXPORT_REACH_REPORT: empty disarms

// PSXPORT_RENDER_PATH: record | device; an unknown value warns and gives `fallback`.
present::RenderPath render_path(present::RenderPath fallback = present::RenderPath::Record);

bool fullscreen_requested(); // PSXPORT_FULLSCREEN, or PSXPORT_WINDOWED=0
std::string picker_titles(); // PSXPORT_PICKER_TITLES: panel slugs, empty for every provisioned title

} // namespace psx::config
