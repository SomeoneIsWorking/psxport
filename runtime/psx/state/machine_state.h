// machine_state.h — psx::state::MachineState, the owner of one whole-machine state.
//
// WHY THIS IS NOT `machine_snapshot.h`. That module captures CPU/GTE/RAM/scratchpad for the override
// DIFFERENTIAL and says so: devices are not captured and restoring does not rewind them. A file it
// produces is not resumable, and pretending otherwise is the mistake this module exists to end.
//
// WHAT A WHOLE-MACHINE STATE OWNS, and in what order:
//
//   1. capture, in this order: CPU + COP0 + interrupt latch + DMA channel shadows, main RAM, the
//      scratchpad, the GTE, the native GPU (VRAM + guest-visible registers), the vendored Beetle SPU
//      and MDEC through the FORK'S OWN state functions, the CD controller, the display clock, the DMA
//      controller, the BIOS HLE/interrupt state, the controller port + pad, the memory card image,
//      the native CD subsystem, and finally the TITLE's own state.
//   2. load, in the REVERSE order, so a failure anywhere leaves the machine's control and timing
//      state untouched — a half-restored interrupt controller is a machine that faults for reasons
//      no diagnostic can name, and refusing before any mutation is the only version of this that is
//      safe to retry.
//
// THE TITLE REFUSAL. A title with native owners says so by returning a NativeStatePort (below); a
// title without them returns null and this owner writes no title section. Loading compares the
// FILE's title section against the RUNNING title's port and refuses in all three mismatch
// directions — a section this title does not own, a section with the wrong name, and a title that
// owns native state the file does not carry. There is no "load it anyway": a title whose native
// state is missing desyncs on the first field, silently, which is worse than a refusal.
//
// THE INVALIDATION IS NOT OPTIONAL. Restoring 2 MB of RAM and the scratchpad replaces executable
// bytes behind Lightrec's back, so every block translated from them is now stale and reachable. One
// notification per restored span, source `Savestate`, is reported to the single invalidation owner
// after the bytes are visible. The dynarec's translated-block counters CONTINUE across a load and
// the invalidation count rises — a run that has loaded a state has translated less than its
// instruction count implies, and that is visible rather than silent.
#pragma once

#include "state_file.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Core;
class Game;

namespace psx::state {

// The title-owned half. A title whose native owners (frame driver, field scheduler, boot-sequence
// state, picker session) live outside guest RAM implements this against ITS OWN version and stores
// the result in the file's `title` section.
//
// `sectionName` must be STABLE for as long as the title's native state layout is: it is what the
// framework compares a loaded file's title section against, and renaming it is a format change.
// `version` is the title's own layout version; a load refuses a file whose version it does not
// implement, because a title that cannot decode its own older state must say so rather than load
// half of it.
class NativeStatePort {
public:
  virtual ~NativeStatePort() = default;
  [[nodiscard]] virtual const char *sectionName() const = 0;
  [[nodiscard]] virtual std::uint32_t version() const = 0;
  // Both return false with `error` filled in. A save that fails is never written, and a load that
  // fails restores NOTHING of the title's — the title owns its own atomicity here, because only the
  // title knows which of its fields are safe to have half-restored.
  virtual bool save(class BlobWriter &out, std::string &error) const = 0;
  virtual bool load(class BlobReader &in, std::string &error) = 0;
};

// The version of the envelope the framework writes AROUND a title's own payload — the two fields
// ahead of the title's bytes: this value, then the title's `version()`. It is the framework's own
// format, so it lives in this header rather than in the .cpp, and a test building a title section by
// hand uses this constant instead of a literal that would silently keep testing the wrong thing if
// the envelope ever changed.
inline constexpr std::uint8_t kTitleEnvelopeVersion = 1;

// What one save or load actually did, with the denominators a caller needs to read the number
// rather than assume what it covers.
struct StateOutcome {
  std::vector<std::string> sections;    // every section the operation wrote or read, in file order
  std::size_t bytes = 0;                // total image size
  std::uint32_t fields = 0;             // the display-field count at the moment of the operation
  std::uint32_t invalidationRanges = 0; // spans reported to the invalidation owner
  bool loaded = false;
};

class MachineState {
public:
  // Takes the Game explicitly; there is no process-global active machine to reach for.
  explicit MachineState(Game &game) : mGame(game) {}

  // Serialize the machine. Returns nullopt with `error` naming the device that refused.
  std::optional<std::vector<std::uint8_t>> capture(std::string &error);
  // Restore `image`. Refuses — by name, with nothing mutated — on a malformed file, an unknown
  // section, a device that refuses its own bytes, or a title/native-state mismatch.
  std::optional<StateOutcome> restore(std::span<const std::uint8_t> image, std::string &error);

  // The title's own port, or null when this title has no native owners.
  static NativeStatePort *titlePort(Game &game);

private:
  Game &mGame;
};

// Write `image` to `path`, and read it back from `path`. Both report failure by name.
bool saveToFile(Core &core, const std::string &path, std::string &error);
std::optional<StateOutcome> loadFromFile(Core &core, const std::string &path, std::string &error);

} // namespace psx::state