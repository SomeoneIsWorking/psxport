#ifndef NATIVE_GATE_H
#define NATIVE_GATE_H
extern "C" int native_gate(const char* name);   // 1=native layer active, 0=routed to PSX recomp (REPL `native <name> off`)
void native_gate_set(const char* name, int on);
void native_gate_list();
#endif
