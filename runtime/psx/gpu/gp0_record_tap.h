// gp0_record_tap.h — appends each GP0 command the GPU device executes to the FrameRecord.
#pragma once

#include "frame_record.h"
#include "gp0_draw_state.h"
#include "gp0_primitive_decode.h"
#include "owner_ledger.h"
#include "texture_feedback.h"
#include "vertex_places.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psx::gpu {

// What the device's command sequencer is doing, as gpu.c's InCmd says.
enum class DeviceCommandState : std::uint8_t { None, PolyLine, Quad, Upload, Read };

// The device after it consumed one GP0 word: the state a just-executed command drew with.
struct DeviceProbe {
  DeviceCommandState command = DeviceCommandState::None;
  unsigned fifoDepth = 0;
  unsigned dispatched = 0;             // commands the device dispatched while consuming the word
  bool wordDropped = false;            // the device discarded the word on a full FIFO
  Gp0DrawRegisters draw;               // checked against the tap's own decode
  std::uint32_t clutTag = 0;           // gpu.c CLUT_Cache_VB
  std::span<const std::uint16_t> clut; // gpu.c CLUT_Cache, 256 entries
  std::span<const std::uint16_t> vram; // the device's VRAM, 1024x512
};

// Where a GP0 word came from: its guest address (0 when it came from no packet) and the writer of the packet it
// belongs to.
struct Gp0Origin {
  std::uint32_t sourceAddress = 0;
  std::uint32_t packet = 0; // guest address of the ordering-table packet carrying the word, 0 when none
  present::OwnerHandle owner = present::kNoOwner;
  std::optional<cpu::NotedPlace> place; // where a draw site stored this word's vertex (VertexPlaces)
};

// Frames the GP0 word stream exactly as gpu.c's ProcessFIFO does, decodes each command with
// Gp0Command, and checks after every word that the device agrees. A framing disagreement marks the
// record incomplete until the device and the tap are both idle again; a draw environment that differs
// from the device's is a fault.
// The tap as a machine state carries it: the next sequences and, when the tap stood between packets with its record
// complete, the record in progress and the texture writes it tracks.
struct TapResume {
  std::uint64_t recordSequence = 0;
  std::uint64_t nextSequence = 1;
  std::optional<present::FrameRecord> record;
  TextureFeedback feedback;
};

class Gp0RecordTap {
public:
  Gp0RecordTap();

  void onGp0(std::uint32_t word, const Gp0Origin &origin, const DeviceProbe &probe);
  void noteDrawEntry(std::uint32_t field) {
    record_.markDrawEntry(field);
  }
  // Places a host read-back declaration at this point of the record.
  void declareReadBack(const present::ReadBackTexture &texture) {
    record_.append(texture);
  }
  // The walk reached `slot` of a table walked high to low when `descending`, under `state`, or left
  // every named table (nullopt).
  void enterSlot(const std::optional<present::OtSlot> &slot, bool descending, const present::RecordDrawState &state);
  // One DrawOTag walk from `head` starts under `state`, or ends.
  void beginChain(std::uint32_t head, const present::RecordDrawState &state) {
    record_.beginChain(head, state);
  }
  void endChain() {
    record_.endChain();
  }
  // A device step that consumed no GP0 word: GP1, GPUREAD, GPUSTAT.
  void onDeviceStep(const DeviceProbe &probe);
  // GP1(00): gpu.c GPU_SoftReset, which also invalidates the texture cache.
  void onSoftReset(const DeviceProbe &probe);
  // GP1(00) or GP1(01): the device dropped its FIFO and any command in progress.
  void onCommandReset(const DeviceProbe &probe);
  // The device's whole state was replaced; nothing recorded so far describes it.
  void onStateReplaced(const DeviceProbe &probe);
  void save(psx::platform::BlobWriter &out) const;
  static bool read(psx::platform::BlobReader &in, TapResume &resume, std::string &error);
  // After onStateReplaced: the saved sequences, and the saved record in progress when the device is idle.
  void resume(TapResume &&resume);

  // Seals the current record into `into` and starts the next in `into`'s old storage.
  void seal(present::FrameRecord &into);
  // True when the device executed work since the last seal.
  bool hasPendingWork() const;

private:
  enum class Mode : std::uint8_t { Command, QuadTail, PolyLine, Upload, Read };

  void executeCommand(const DeviceProbe &probe);
  void recordPolygon(const DeviceProbe &probe);
  void finishQuad(const DeviceProbe &probe);
  void recordLineSegment(const present::RecordVertex &from,
                         const present::RecordVertex &to,
                         bool gouraud,
                         bool semi,
                         const DeviceProbe &probe);
  void recordSprite(const DeviceProbe &probe);
  // Settles the last primitive: tracks what it wrote and keeps the device's pixels when its texture reads
  // depended on writes the rasterizer's snapshot cannot order.
  void settleLast(const DeviceProbe &probe);
  void consumeUploadWord(std::uint32_t word);
  void consumePolyLineWord(std::uint32_t word, const DeviceProbe &probe);
  void resyncIfIdle(const DeviceProbe &probe);
  // The CLUT `clutWord` selects, as the device sampled it.
  std::uint32_t captureClut(std::uint16_t clutWord, const DeviceProbe &probe);
  void verify(const DeviceProbe &probe);
  void checkDrawState(const DeviceProbe &probe) const;
  void desync();
  void resetFraming();
  // The packet's writer and command onto `primitive`, with its slot.
  void stampPacket(present::DrawPrimitive &primitive);
  // Keeps the place of the word about to join words_.
  void notePlace(const Gp0Origin &origin);
  // Vertex `v` of `primitive` decoded from words_[position], as written, placed where it was stored.
  void placeVertex(present::DrawPrimitive &primitive, int v, present::RecordVertex vertex, std::size_t position) const;
  // words_ with the place of each.
  [[nodiscard]] PacketView placedWords() const;
  // The draw site that stored every placed word of words_, 0 when they differ or none is placed.
  [[nodiscard]] std::uint32_t placingSite() const;
  // The draw site that stored words_[i], 0 when it is not placed.
  [[nodiscard]] std::uint32_t wordSite(std::size_t i) const {
    return i < places_.size() && places_[i] ? placingSites_[i] : 0u;
  }

  std::uint64_t nextSequence_ = 1; // 0 is the presenter's empty record
  present::FrameRecord record_;
  bool synced_ = true;

  Mode mode_ = Mode::Command;
  std::vector<std::uint32_t> words_;
  std::array<gte::WordPlace, 16> places_{};      // of words_ by index
  std::array<std::uint32_t, 16> placingSites_{}; // the draw site that stored each, 0 for none
  unsigned need_ = 0;
  std::uint32_t packetSource_ = 0;
  std::uint32_t packetNode_ = 0;
  present::OwnerHandle packetOwner_ = present::kNoOwner;
  std::uint8_t packetCommand_ = 0;
  std::optional<present::OtSlot> slot_;
  unsigned expectedDispatches_ = 0; // by the word being processed

  // A quad's first triangle, kept for its second.
  std::uint32_t quadOpcode_ = 0;
  // The previous point of a poly-line.
  present::RecordVertex polyLinePoint_{};
  std::uint32_t polyLineSite_ = 0;      // the segment being recorded's
  std::uint32_t polyLinePointSite_ = 0; // the last point's
  std::uint32_t polyLineOpcode_ = 0;
  std::uint32_t polyLineSource_ = 0;
  present::OwnerHandle polyLineOwner_ = present::kNoOwner;
  std::uint32_t polyLineNode_ = 0;

  present::VramUpload upload_{};
  std::uint32_t uploadRemaining_ = 0;
  std::vector<std::uint16_t> uploadPixels_;
  // A settled draw's pixels, reused across draws.
  std::vector<std::uint16_t> settledPixels_;

  TextureFeedback feedback_;
  Gp0DrawState drawState_;
  std::uint32_t lastCommand_ = 0; // the last command word executed

  std::uint32_t clutTag_ = 0xFFFFFFFFu;
  std::uint32_t clutOffset_ = present::kNoClut;
};

} // namespace psx::gpu
