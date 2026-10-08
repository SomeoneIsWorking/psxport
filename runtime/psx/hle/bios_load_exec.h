// BIOS A0:0x51 LoadAndExecute — the load-and-jump BIOS service, in one cohesive module.
//
// nocash psx-spx names the leaf "LoadAndExecute(filename,stackbase,stackoffset)". The two
// arguments after the path are the STACK the loaded program runs on; they are NOT a load address
// and NOT argc/argv. The executable always loads at the address its own PS-X EXE header declares.
// That reading is fixed by the one real call site measured in a shipped image: Spyro 1
// `SCUS_942.28` builds `LoadExec("cdrom:\S0\CRASH.EXE;1", 0x801FFF00, 0)` at `0x8002BB08` and calls
// the tail-jump stub at `0x8005DB24`; `0x801FFF00` is the top of main RAM, which is a stack and is
// not a load address any program could be loaded at.
//
// The service reuses the framework's executable-admission owner (`psx::cpu::loadPsxExeImage`) and its
// top-level register policy (`applyPsxExeTopLevelRegisters`) rather than admitting a second loader,
// and it says where the guest continues through the executor's exit contract instead of returning.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

class Core;
struct DiscState;

namespace psx::hle {

// The CD device's answer to "give me this file's bytes", by absolute ISO9660 path. Production binds
// the run's own disc through `discFileReader`; a hermetic test binds a buffer. It is an explicit
// dependency rather than a global so the code under test is the SHIPPING service — the loader, the
// register policy, the invalidation and the jump are identical either way.
using DiscFileReader = std::function<bool(const char *isoPath, std::vector<std::uint8_t> &out)>;

// The reader over one run's disc. The only binding of `DiscFileReader` the product makes.
DiscFileReader discFileReader(DiscState &disc);

// A0:0x51, from `core`'s own register file: a0 = filename, a1 = stack base, a2 = stack offset.
//
// Returns true when the service was serviced, which INCLUDES a failed load: "-1 in V0, then return
// to the caller" is the BIOS's own failure answer, not an unimplemented call. A true return with a
// pending execution exit means the named program was started and the guest continues at ITS entry;
// without one, the guest resumes at this leaf's own `r[31]` as a return to the caller.
bool dispatchLoadExec(Core &core, const DiscFileReader &readFile);

} // namespace psx::hle