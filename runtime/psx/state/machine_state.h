// psx::state::MachineState: one resumable whole-machine state. Sections are captured in
// section::kRequired order and restored in reverse with nothing mutated before every check passes;
// a title with native owners adds its section through NativeStatePort, and a mismatch either way refuses.
#pragma once

#include "blob_codec.h"
#include "state_file.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Core;
class Game;

namespace psx::state {

// A title's native state, kept in the file's `title` section; renaming sectionName() is a format change.
class NativeStatePort {
public:
  virtual ~NativeStatePort() = default;
  [[nodiscard]] virtual const char *sectionName() const = 0;
  [[nodiscard]] virtual std::uint32_t version() const = 0;
  // False with `error` set; a failed load restores nothing of the title's.
  virtual bool save(psx::platform::BlobWriter &out, std::string &error) const = 0;
  virtual bool load(psx::platform::BlobReader &in, std::string &error) = 0;
  // After every section is restored; `load` runs before RAM, so state tied to guest bytes is staged there and adopted here.
  virtual void restored(Core &) {}
};

// What one load did.
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

  // The title's own port for this machine's Core, or null when this title has no native owners.
  static NativeStatePort *titlePort(Game &game);

private:
  Game &mGame;
};

// The only save and load paths; each refuses an empty path and logs its summary.
bool saveToFile(Core &core, const std::string &path, std::string &error);
std::optional<StateOutcome> loadFromFile(Core &core, const std::string &path, std::string &error);

} // namespace psx::state