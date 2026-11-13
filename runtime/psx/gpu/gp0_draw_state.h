// gp0_draw_state.h — the draw environment gpu.c keeps (E1-E6, a textured polygon's texpage), decoded from command
// words.
#pragma once

#include "frame_record.h"
#include "projected_vertex.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace psx::gpu {

// The device registers a primitive drawn now is drawn under.
struct Gp0DrawRegisters {
  present::RecordDrawState state;
  bool texDisable = false;        // E1 bit 11, taken only while GP1(09) allows it
  bool texDisableAllowed = false; // GP1(09)
  bool spriteFlipX = false;       // E1 bit 12
  bool spriteFlipY = false;       // E1 bit 13
  bool drawToDisplay = false;     // E1 bit 10, gpu.c dfe
  // The row parity an interlaced display leaves undrawn this field (gpu.c LineSkipTest), -1 when it draws both.
  int displaySkipParity = -1;

  bool operator==(const Gp0DrawRegisters &) const = default;
};

std::string describe(const Gp0DrawRegisters &registers);

// gpu.c's draw environment as command words set it. Dither (E1 bit 9) is not kept: no record op dithers.
class Gp0DrawState {
public:
  // gpu.c GPU_Power.
  Gp0DrawState() = default;
  explicit Gp0DrawState(const Gp0DrawRegisters &seed);
  // The environment a record noted; the E1 bits and GP1 state it does not hold start clear, and the skipped row
  // parity it holds is the display's.
  explicit Gp0DrawState(const present::RecordDrawState &atStart);

  // One command as gpu.c dispatches it: E1-E6 set the environment, a textured polygon its texpage; others no-op.
  void execute(std::span<const std::uint32_t> command);
  // GP1(00), gpu.c GPU_SoftReset.
  void softReset();
  // The GP1 side: GP1(09) and the display's interlaced field.
  void setControl(bool texDisableAllowed, int displaySkipParity);

  [[nodiscard]] const Gp0DrawRegisters &registers() const {
    return registers_;
  }
  [[nodiscard]] const present::RecordDrawState &state() const {
    return registers_.state;
  }
  // A polygon or line vertex as written to where it draws: past the draw offset.
  void offset(present::RecordVertex &vertex) const;
  // `primitive`, decoded as written, drawn now: the environment's state, its vertices past the draw offset (a
  // sprite's wrapped to 11 bits as gpu.c does) and a textured sprite's flip.
  void apply(present::DrawPrimitive &primitive) const;

private:
  void setTexPage(std::uint16_t attribute);
  void updateSkipParity();

  Gp0DrawRegisters registers_;
};

// One packet of a captured chain: its tag's guest address, its `count` GP0 words from `offset` in the capture and the
// function that placed its vertex words (EmitMemory::placingSite).
struct CapturedPacket {
  std::uint32_t address = 0;
  std::uint32_t offset = 0;
  std::uint32_t count = 0;
  std::uint32_t site = 0;
};

// Each command of each packet as the device runs it from `start`: environment commands set the state, and every
// polygon, line, sprite, fill and copy goes into `into` under the state then in effect, vertices past the draw
// offset, `sourceAddress` its first word's guest address, `packet` and `site` its packet's, no slot, key or owner, and
// its CLUT
// unresolved
// (`clutWord` only). `places`, one per word or empty, gives each vertex the place stored with its word. A poly-line
// ends its packet. False, having decoded the commands before it, at any other command or one its packet ends inside.
bool decodePackets(std::span<const std::uint32_t> words,
                   std::span<const gte::WordPlace> places,
                   std::span<const CapturedPacket> packets,
                   const present::RecordDrawState &start,
                   std::vector<present::RecordEntry> &into);

} // namespace psx::gpu
