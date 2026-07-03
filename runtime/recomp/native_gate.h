// class NativeGates — the PC-native-layer A/B GATE registry (REPL `native <name> on|off`).
//
// Each PC-native layer that REPLACES game behavior is gated behind a named flag (default ON);
// dropping it routes that behavior back to the recompiled PSX body so it can be A/B-compared
// against the native replacement. Diagnostic-only mechanism.
//
// One per Game (`c->game->native_gates.method()`). 32-entry fixed registry, auto-registered on
// first query. In SBS both Games have independent gate maps; the REPL `native` subcommand mutates
// the debug-target Core's gates.
#ifndef NATIVE_GATE_H
#define NATIVE_GATE_H

#ifdef __cplusplus
class NativeGates {
public:
  int  get(const char* name);            // 1 = native layer active; 0 = routed to PSX recomp. Auto-registers.
  void set(const char* name, int on);
  void list() const;
private:
  struct Entry { const char* name; int on; };
  Entry mGates[32];
  int   mCount = 0;
};
#endif
#endif
