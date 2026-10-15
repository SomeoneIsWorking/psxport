#ifndef PSXPORT_CONFIG_VARS_H
#define PSXPORT_CONFIG_VARS_H
// config_vars.h — the product and run knobs, each a private CVar in config.cpp read through the typed
// accessor here. Diagnostic knobs are in diagnostic_config.h. A test sets one by name through
// set_runtime()/clear_runtime() (config.h).
#include "diagnostic_run.h"
#include "fallback_policy.h"
#include "override_differential_config.h"
#include "render_mode.h" // RenderPath

#include <filesystem>
#include <string>

namespace psx::config {

// PSXPORT_NOAUDIO — open no audio device. Half of what "headless" means (the other half, no window,
// is the consuming game's own launch decision).
bool no_audio();

bool producer_census(); // PSXPORT_PRODUCERS: arm the producer census (ot_attr.h)

// PSXPORT_DEBUG_SERVER — the live, non-blocking TCP debug endpoint on 127.0.0.1, which is how a game
// is driven and inspected WHILE it runs. The value is a port, and 1 means the default port, so the
// text is interpreted once, in dbg_server.h's `psx::debug::debugServerPort`.
std::string debug_server();
// PSXPORT_STORE_OBSERVE — hex guest STORE-INSTRUCTION PCs the dynarec store observer watches. The list
// is matched against the PC OF each executed translated store (`target.guestPc != guestPc` in
// LightrecExecutor::Impl::observeStore), so it is NOT a list of guest data addresses; arming a data
// address produces a guaranteed MATCHED NONE. Interpreted once by runtime/cpu/store_observe.cpp; see the
// declaration in config.cpp for what it is for and what it cannot answer.
std::string store_observe();
// PSXPORT_RAMDUMP — write main RAM to this path AFTER the frame loop returns.
// PSXPORT_RAMDUMP_FRAME=N writes from INSIDE the loop at native frame N, which is the one that works
// while a title is playing: overlay state during gameplay is not end-of-run state, and not every
// product returns from the frame loop at all (measured 2026-09-27 on Spyro 1, where the end-of-run
// dump silently did nothing). See the declarations in config.cpp.
std::string ramdump_path();
std::string ramdump_frame();

// PSXPORT_LOAD_STATE — a whole-machine state file to resume from BEFORE the first field, read once
// at boot through this configuration owner (environment, then ./.env). This is how a headless tool
// starts inside a level instead of at power-on. Unset means the machine boots normally; a set path
// that cannot be loaded is a REFUSAL, not a silent fall back to power-on — a run that quietly
// booted from scratch after a broken state path would spend the whole budget re-deriving the state
// it was asked to start from.
std::string load_state_path();

// PSXPORT_BUG_REPORT_DIR — where B-key bug reports are saved. Unset saves under the title's user-data
// directory (HostIdentity::userDataName) in `bug-reports/`; see runtime/psx/debug/bug_report_session.h.
std::string bug_report_dir();

// PSXPORT_PAD_RECORD / PSXPORT_PAD_REPLAY / PSXPORT_PAD_RESUME — the pad session's record sink, the
// recording a gate replays at real speed, and the recording a player resumes from (fast-forwarded).
// All three are phase-keyed .pad files (runtime/psx/input/pad_recording.h); PSXPORT_PAD_RECORD=0 disables
// the windowed default sink. Resolved once by psx::input::PadRecordReplay on the first pad frame.
std::string pad_record_path();
std::string pad_replay_path();
std::string pad_resume_path();

// ── watchdog ────────────────────────────────────────────────────────────────────────────────────
// PSXPORT_WATCHDOG — frame-progress timeout in seconds. Default 3, ON even when unset, so a hang
// self-aborts with a backtrace instead of wedging. 0 disables it.
long watchdog_seconds();
long spin_ticks(); // PSXPORT_SPIN_TICKS: guest instructions per spin-detector sample (0 = off)
long spin_runs();  // PSXPORT_SPIN_RUNS: consecutive starved in-region samples that declare a spin
// PSXPORT_WATCHDOG_BOOT — the larger grace for the FIRST present, which legitimately blocks while
// the driver compiles every pipeline. -1 (the default) means "derive": max(PSXPORT_WATCHDOG, 45).
// An explicit 0 still means 0, which is why the sentinel is -1 and not 0.
long watchdog_boot_seconds();

// ── run length ──────────────────────────────────────────────────────────────────────────────────
// PSXPORT_NATIVE_FRAMES — the run's cap in presentation-fence steps; 0 leaves it to the run budget's
// defaults (host/run_budget.h).
long native_frames();

// ── assets and settings ─────────────────────────────────────────────────────────────────────────
// PSXPORT_ASSET_DIR — the directory CONTAINING `assets/`, for a consumer whose cwd is its own repo
// root. Empty = cwd-relative `assets/rml/...`.
std::string asset_dir();

// PSXPORT_VK_WINDOW / PSXPORT_VK_HEADLESS — a run is headless unless a window is asked for, and
// HEADLESS wins. Read through windowRequested(); the backing vars are private.
bool windowRequested();

// What a packaged product (an AppImage, `packageRoot`) runs with when nothing overrides it: a
// window, and the assets shipped under `<packageRoot>/usr/share/psxport`. Default layer only.
void applyPackagedDefaults(const std::filesystem::path &packageRoot);

// PSXPORT_SETTINGS — path to the settings file. NOT persistable, and that is not a detail: this knob
// SELECTS the file the Value layer is read from and written to, so a Value-layer copy of it inside
// that file could disagree with the file it is in.
std::string settings_path();

// PSXPORT_ENH=<name,name|all> — the pc_enh selection, read through enh_named()/enh() (enhancement_gate.h).

// PSXPORT_DIAGNOSTIC_RUN — product | compare-candidate | compare-reference. This is a diagnostic
// role for the same shipping dynarec/native runtime, never an interpreter/backend selector. The
// backing TextVar is intentionally private: consumers use this typed API and ScopedDiagnosticRun.
// The API is declared once in diagnostic_run.h, included above.

// PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT — maximum automatic interpreter-fallback blocks admitted
// during one bounded executor call. The compiled default is one: the known difficult-block escape
// remains available, while a second block is a typed fault instead of an interpreter-dominated run.
psx::cpu::FallbackPolicy lightrec_fallback_policy();

// PSXPORT_OVERRIDE_DIFF (+ _FIRST, _EVERY, _DEAD_STACK, _REPORT) — the per-function override
// differential (runtime/cpu/override_differential.h): which native overrides to shadow against their
// original guest bodies in a real run, how many calls to sample, and where the JSON report goes. The
// backing vars are private; this typed value is the only way to read them. Empty selectors disarm.
psx::cpu::OverrideDifferentialConfig override_differential_config();

// PSXPORT_REACH_REPORT — where the function-reach recorder writes its JSON report; empty disarms it.
std::string function_reach_report_path();

// PSXPORT_RENDER_PATH — the render path: record | device. An unknown value warns and gives `fallback`.
RenderPath render_path(RenderPath fallback = RenderPath::Record);

// PSXPORT_FULLSCREEN, or PSXPORT_WINDOWED=0 — the window opens fullscreen.
bool fullscreen_requested();
// PSXPORT_PICKER_TITLES — the picker's panel slugs, comma-separated; empty is every provisioned title.
std::string picker_titles();
// $XDG_STATE_HOME/psxport, else $HOME/.local/state/psxport; empty when neither is set.
std::string user_state_dir();

} // namespace psx::config

#endif // PSXPORT_CONFIG_VARS_H
