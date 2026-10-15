// enhancement_gate.h — how a pc_enh knob is read: its value, or neutral in a comparison run.
#pragma once

#include "config_var.h"

namespace psx::config {

// Resolve one enhancement and announce an active selection once per key.
bool enh_gate(const char *key, bool asked);
// `enh(v)` is for a title that declares its enhancements as its own CVars; `enh_named(name)` is for a
// name selected through PSXPORT_ENH.
bool enh(const CVar<bool> &v);
// A graded pc_enh (a percentage, a distance): the knob's value, or 0 in a comparison run.
long enh_int(const CVar<long> &v);
bool enh_named(const char *name);

} // namespace psx::config
