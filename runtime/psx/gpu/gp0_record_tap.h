// gp0_record_tap.h — appends each GP0 command the GPU device executes to the FrameRecord.
#pragma once

#include "frame_record.h"
#include "texture_feedback.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace psx::gpu {

// What the device's command sequencer is doing, as gpu.c's InCmd says.
enum class DeviceCommandState : std::uint8_t { None, PolyLine, Quad, Upload, Read };

// The device after it consumed one GP0 word: the state a just-executed command drew with.
struct DeviceProbe {
  DeviceCommandState command = DeviceCommandState::None;
  unsigned fifoDepth = 0;
  unsigned dispatched = 0;  // commands the device dispatched while consuming the word
  bool wordDropped = false; // the device discarded the word on a full FIFO
  present::RecordDrawState state;
  bool texDisable = false; // gpu.c TexDisable
  bool spriteFlipX = false;
  bool spriteFlipY = false;
  std::uint32_t clutTag = 0;           // gpu.c CLUT_Cache_VB
  std::span<const std::uint16_t> clut; // gpu.c CLUT_Cache, 256 entries
  std::span<const std::uint16_t> vram; // the device's VRAM, 1024x512
};

// Frames the GP0 word stream exactly as gpu.c's ProcessFIFO does, decodes each command with
// Gp0Command, and checks after every word that the device agrees. Any disagreement marks the
// record incomplete until the device and the tap are both idle again.
class Gp0RecordTap {
public:
  Gp0RecordTap();

  // `packetKey` is the emission key of the OT node the word belongs to, if any.
  void onGp0(std::uint32_t word,
             std::uint32_t sourceAddress,
             const std::optional<present::RecordKey> &packetKey,
             const DeviceProbe &probe);
  // The next words belong to a new OT node; `part` numbering restarts.
  void beginPacket();
  // The walk reached `slot` of a table walked high to low when `descending`, or left every named
  // table (nullopt).
  void enterSlot(const std::optional<present::OtSlot> &slot, bool descending);
  // A device step that consumed no GP0 word: GP1, GPUREAD, GPUSTAT.
  void onDeviceStep(const DeviceProbe &probe);
  // GP1(00): gpu.c GPU_SoftReset, which also invalidates the texture cache.
  void onSoftReset(const DeviceProbe &probe);
  // GP1(00) or GP1(01): the device dropped its FIFO and any command in progress.
  void onCommandReset(const DeviceProbe &probe);
  // The device's whole state was replaced; nothing recorded so far describes it.
  void onStateReplaced(const DeviceProbe &probe);

  // Seals the current record and starts the next.
  present::FrameRecord seal();
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
  std::uint32_t captureClut(const DeviceProbe &probe);
  void verify(const DeviceProbe &probe);
  void desync();
  void resetFraming();
  // The key for the next primitive of the current packet: its packet key with the next part.
  std::optional<present::RecordKey> nextPrimitiveKey();

  std::uint64_t nextSequence_ = 1; // 0 is the presenter's empty record
  present::FrameRecord record_;
  bool synced_ = true;

  Mode mode_ = Mode::Command;
  std::vector<std::uint32_t> words_;
  unsigned need_ = 0;
  std::uint32_t packetSource_ = 0;
  std::optional<present::RecordKey> packetKey_;
  std::optional<present::RecordKey> lastKey_; // the last primitive key stamped in this packet
  std::optional<present::OtSlot> slot_;
  unsigned expectedDispatches_ = 0; // by the word being processed

  // A quad's first triangle, kept for its second.
  std::uint32_t quadOpcode_ = 0;
  // The previous point of a poly-line.
  present::RecordVertex polyLinePoint_{};
  std::uint32_t polyLineOpcode_ = 0;
  std::uint32_t polyLineSource_ = 0;
  std::optional<present::RecordKey> polyLineKey_;

  present::VramUpload upload_{};
  std::uint32_t uploadRemaining_ = 0;
  std::vector<std::uint16_t> uploadPixels_;

  TextureFeedback feedback_;

  std::uint32_t clutTag_ = 0xFFFFFFFFu;
  std::uint32_t clutOffset_ = present::kNoClut;
};

} // namespace psx::gpu
