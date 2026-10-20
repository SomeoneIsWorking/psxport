// How a pc_enh knob is read: its value, or neutral in a comparison run.
#pragma once

#include "config_var.h"

#include <string>

namespace psx::config {

// Resolves one enhancement and announces each key's outcome once.
bool enh_gate(const char *key, bool asked);
// A title's own enhancement knob.
bool enh(const Var<bool> &v);
// A graded enhancement (a percentage, a distance): its value, or 0 in a comparison run.
long enh_int(const Var<long> &v);
// An enhancement selected by name through PSXPORT_ENH.
bool enh_named(const char *name);

} // namespace psx::config
