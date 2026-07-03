// class NativeGates — the PC-native-layer A/B GATE registry (REPL `native <name> on|off`).
//
// Each PC-native layer that REPLACES game behavior is gated behind a named flag (default ON);
// dropping it routes that behavior back to the recompiled PSX body so it can be A/B-compared
// against the native replacement. Diagnostic-only mechanism, not a shipped behavior toggle.
//
// One singleton per process (32-entry fixed registry, auto-registered on first query). Legacy
// `native_gate*` C entries are one-liner bridges through `NativeGates::instance()`.
#ifndef NATIVE_GATE_H
#define NATIVE_GATE_H

#ifdef __cplusplus
class NativeGates {
public:
  static NativeGates& instance();
  int  get(const char* name);            // 1 = native layer active; 0 = routed to PSX recomp. Auto-registers.
  void set(const char* name, int on);
  void list() const;
private:
  NativeGates() = default;
  struct Entry { const char* name; int on; };
  Entry mGates[32];
  int   mCount = 0;
};
#endif

// Legacy free-function API — thin bridges to `NativeGates::instance()`.
#ifdef __cplusplus
extern "C" {
#endif
int  native_gate(const char* name);
void native_gate_set(const char* name, int on);
void native_gate_list(void);
#ifdef __cplusplus
}
#endif
#endif
