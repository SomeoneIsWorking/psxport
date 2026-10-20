// The knob registry's outside surface: launch settings, the environment audit, runtime changes and the report.
#pragma once
#include "config_var.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace psx::config {

const char *layer_name(Layer layer);

// The first of `keys` set in the environment, else in ./.env; empty when none is. Each key counts as known to the audit.
std::string launch_setting(std::initializer_list<const char *> keys);

struct EnvAudit {
  std::vector<std::string> set_in_env; // every PSXPORT_* variable present, "NAME=value"
  std::vector<std::string> declared;
  std::vector<std::string> identity; // keys a title passed to launch_setting()
  std::vector<std::string> unknown;  // matched nothing, so did nothing
};
EnvAudit audit_environment();

// Every knob with its value and layer when `withKnobs`, then the audit; WARN-worthy lines start with "UNKNOWN".
std::vector<std::string> cvar_report_lines(bool withKnobs);
void report();
// The boot report, plus the audit again at a clean exit.
void report_once();

// This run only; false (and logged) for an unknown name or text that does not parse.
bool set_runtime(std::string_view name, std::string_view text);
bool clear_runtime(std::string_view name);
// Records at the Runtime layer the PSXPORT_DEBUG channel set a console command applied.
void note_runtime_debug_channels(std::string_view channels);

// The settings file (PSXPORT_SETTINGS): every saved knob's Value layer.
void load_settings();
bool save_settings();

} // namespace psx::config
