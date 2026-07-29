// fntrace.h — PSXPORT_FNTRACE=<addr>[,...]: log when a guest function is REACHED, and from where.
// Answers "does this branch actually execute?" by proxy, via a callee unique to the path in question.
// Call AFTER psxport_install_recomp(). See fntrace.cpp for the two limits (MAIN entries only;
// recursion counted once).
#pragma once
void fntrace_init();
