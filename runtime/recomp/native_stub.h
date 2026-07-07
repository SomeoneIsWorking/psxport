// native_stub.h — class BootStub — the PC-native boot entry (replaces interpreting the disc's PSX
// boot stub SCUS_944.54), owned by Game (`c->game->stub`, back-pointer wired in Game()).
// Implemented in native_stub.cpp: renders the SCEA license screen from a baked asset, then loads
// MAIN.EXE and enters the native MAIN boot (native_boot.cpp).
#pragma once
class Game;

class BootStub {
public:
  Game* game = nullptr;
  const char* main_path = nullptr;  // MAIN.EXE path, reloaded at LoadExec hand-off (was g_main_path)

  // run(main_exe_path): SCEA splash -> load MAIN.EXE image + initial registers -> native MAIN boot.
  void run(const char* main_exe_path);
};
