# psxport — a PSX→PC static-recompilation framework

This repo is the **game-agnostic framework** extracted from the Tomba!2 port: the static recompiler
(`tools/recomp/`), the `runtime/recomp/` substrate (MIPS interp + Beetle GTE/MDEC/SPU backends + the
native SDL_GPU renderer + the SBS differential harness + BIOS/SDK HLE), and the `psxport` STATIC library
a game links. It carries **no game code** — the framework `#include`s nothing from a game; a game provides
`GameConfig` + `GameHooks` (`runtime/recomp/game_iface.h`) + the recompiled substrate (`generated/`) and
links `libpsxport`. The `psxport_smoke` target proves agnosticism (links libpsxport with a stub, zero game
symbols).

- **Consuming game:** Tomba2Engine (vendors this repo as `external/psxport`).
- **Porting a NEW game:** see `docs/porting-a-new-psx-game.md` + `docs/port-framework.md`.
- **Build:** `cmake -S . -B build && cmake --build build --target psxport` (library) or `psxport_smoke`
  (agnosticism proof; needs a consumer's `generated/` headers present to compile).
- **Never commit CHDs or machine-specific paths.**
