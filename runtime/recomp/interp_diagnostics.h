// Diagnostics owned by the flat interpreter but kept out of its instruction engine.
#pragma once

#include "core.h"

void interp_trace_open(Core *core, const char *path);
void interp_trace_call(InterpDiag &diag, uint32_t from, uint32_t to);
void interp_ncall_open_once(InterpDiag &diag);
void interp_ncall_log(InterpDiag &diag,
                      char kind,
                      uint32_t target,
                      uint32_t a0,
                      uint32_t a1,
                      uint32_t a2,
                      uint32_t a3,
                      uint32_t v0,
                      uint32_t v1);
