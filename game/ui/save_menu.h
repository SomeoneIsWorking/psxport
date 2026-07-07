// class SaveMenu — the PC-native SAVE / LOAD FLOW head dispatcher (FUN_80036DFC).
//
// PROPER OOP: an instance per Core (embedded as `Core::saveMenu`, next to Inventory), back-pointer
// wired in Core's constructor. Owns the save/load-menu 6-state dispatch head; the page handlers
// (cursor/page logic + libmcrd file I/O) stay PSX via rec_dispatch — see save_menu.cpp for the full RE.
#pragma once
#include <cstdint>
class Core;

class SaveMenu {
public:
  Core* core = nullptr;

  // runHandler(task): FUN_80036DFC — reproduce the dispatcher's prologue (0x30 frame, s0=ctx,
  //   s1=task), bounds-check the substate at task[1], and tail-dispatch the page handler from the
  //   table at 0x80010668 (the handler unwinds the frame itself). Substate >= 6 is the no-op
  //   restore-and-return path.
  void runHandler(uint32_t task);

  // dispatchBody(c): guest-ABI entry replacing FUN_80036DFC (a0 = the save-menu task struct).
  static void dispatchBody(Core* c);
};
