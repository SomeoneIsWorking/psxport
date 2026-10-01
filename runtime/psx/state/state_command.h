// state_command.h — the two ENTRY POINTS into the save-state owner, and nothing else.
//
//   * the live control channel: `state save <path>` / `state load <path>`, serviced between frames
//     so a state is never taken mid-frame;
//   * PSXPORT_LOAD_STATE, read once at boot through the configuration owner, which is how a headless
//     tool starts in a level instead of at power-on.
//
// Both refuse by name and both say what they refused. Neither invents a default path: a knob whose
// value is empty means "do not touch the machine", and a state file that does not exist is an error,
// never an empty state.
//
// WHERE THE SAVE IS TAKEN. The control channel services one command per frame, after that frame's
// step and before the next, so the CPU is at a guest function boundary with no native override
// active — the only point at which every device is in a state a later run can resume from.
#pragma once

#include <string>

class Core;

namespace psx::state {

// Handle one `state ...` control-channel command. Returns false when the line is not a `state`
// command at all; returns true after writing the reply to `out`, including for a REFUSAL, so the
// client gets the reason rather than a fallback "unknown command".
bool handleControlCommand(Core &core, const char *line, void *out);

// Save `core`'s machine to `path`. Reports the sections written and their total size.
bool save(Core &core, const std::string &path, std::string &error);
// Load `core`'s machine from `path`.
bool load(Core &core, const std::string &path, std::string &error);

// Apply PSXPORT_LOAD_STATE if it names a file. Returns false (with `error`) when the knob is set
// and the load failed; returns true when the knob is unset, or the load succeeded. Called once,
// after the Core is constructed and before the first field.
bool applyConfiguredState(Core &core, std::string &error);

} // namespace psx::state