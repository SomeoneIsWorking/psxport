// hostprof.h — host-PC sampling profiler. See hostprof.cpp for why the existing interpreter-based
// profiler cannot answer this port's question.
//
// Call once at startup. No-op unless PSXPORT_PROF=1, so it costs nothing in a normal run.
#pragma once
void hostprof_init();
