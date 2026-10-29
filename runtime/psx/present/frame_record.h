// frame_record.h — one logic frame's GPU work.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace psx::present {

// Producer identity carried by a primitive.
struct RecordKey {
  std::uint32_t producer = 0;
  std::uint32_t object = 0;
  std::uint32_t element = 0; // producer-named part
  std::uint32_t part = 0;
  std::uint32_t serial = 0; // the object scope that bound the packet; names the state it saved, not the object

  bool operator==(const RecordKey &) const = default;
  // The key without the scope serial: the object a packet belongs to, equal across frames and respawns of the scope.
  [[nodiscard]] RecordKey identity() const {
    RecordKey key = *this;
    key.serial = 0;
    return key;
  }
};

struct RecordKeyHash {
  std::size_t operator()(const RecordKey &key) const {
    std::size_t h = key.producer;
    h = h * 1000003u ^ key.object;
    h = h * 1000003u ^ key.element;
    h = h * 1000003u ^ key.part;
    return h;
  }
};

// A bucket of a title-named ordering table: the table's id and the bucket's index in it.
struct OtSlot {
  std::uint16_t table = 0;
  std::uint32_t index = 0;

  bool operator==(const OtSlot &) const = default;
};

// Draw environment a primitive executed under, read from the device after it ran.
struct RecordDrawState {
  // Draw area, inclusive corners.
  int clipX0 = 0;
  int clipY0 = 0;
  int clipX1 = 0;
  int clipY1 = 0;
  int offsetX = 0;
  int offsetY = 0;
  int texPageX = 0; // halfwords
  int texPageY = 0;
  int texMode = 0;   // 4bpp/8bpp/15bpp mode
  int blendMode = 0; // blend: 0 avg, 1 add, 2 sub, 3 add-quarter
  // Texture window, the raw 5-bit E2 fields.
  int windowMaskX = 0;
  int windowMaskY = 0;
  int windowOffsetX = 0;
  int windowOffsetY = 0;
  bool maskSet = false;
  bool maskCheck = false;
  // Interlaced drawing: -1 draws every row
  int skipRowParity = -1;

  bool operator==(const RecordDrawState &) const = default;
};

struct RecordVertex {
  int x = 0; // VRAM position after draw offset
  int y = 0;
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t u = 0;
  std::uint8_t v = 0;
  // Polygons and lines: where the vertex lies past x/y above native scale, under two pixels either way.
  float subX = 0.0f;
  float subY = 0.0f;

  bool operator==(const RecordVertex &) const = default;
};

enum class PrimitiveKind : std::uint8_t { Polygon, Line, Sprite };

inline constexpr std::uint32_t kNoClut = 0xFFFFFFFFu;

struct DrawPrimitive {
  PrimitiveKind kind = PrimitiveKind::Polygon;
  int vertexCount = 0; // polygon 3 or 4, line 2, sprite 1
  bool textured = false;
  bool gouraud = false;
  bool semiTransparent = false;
  bool modulate = false; // texel is multiplied by the vertex colour
  bool flipX = false;    // sprites
  bool flipY = false;
  int width = 0; // sprites
  int height = 0;
  std::array<RecordVertex, 4> vertices{};
  // Index into FrameRecord::clutPool() of the 16 or 256 CLUT entries the device sampled through.
  std::uint32_t clutOffset = kNoClut;
  std::uint16_t clutWord = 0; // the command's raw CLUT attribute, when a CLUT was sampled
  RecordDrawState state;
  std::uint32_t sourceAddress = 0; // guest address of the packet's first word, 0 when not from RAM
  std::optional<RecordKey> key;
  std::optional<OtSlot> slot; // the bucket the OT walk was in when it reached the packet
  // OwnerLedger handles: who owns the packet's first command word (or the port it came through), and an owner of
  // the other kind (scope against writer) that stored one of its later words.
  std::uint32_t owner = 0;
  std::uint32_t mixedOwner = 0;
  std::uint8_t command = 0; // the packet's GP0 command byte

  bool operator==(const DrawPrimitive &) const = default;
};

// GP0(02): x and width already 16-aligned; fills ignore the draw area and mask bits.
struct VramFill {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::uint16_t value = 0;
  int skipRowParity = -1;

  bool operator==(const VramFill &) const = default;
};

// GP0(80): coordinates wrap at the VRAM edges.
struct VramCopy {
  int srcX = 0;
  int srcY = 0;
  int dstX = 0;
  int dstY = 0;
  int width = 0;
  int height = 0;
  bool maskSet = false;
  bool maskCheck = false;

  bool operator==(const VramCopy &) const = default;
};

// GP0(A0): pixels are FrameRecord::uploadPixels()[pixelOffset, pixelOffset + width * height).
struct VramUpload {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool maskSet = false;
  bool maskCheck = false;
  std::uint32_t pixelOffset = 0;
  std::uint32_t sourceAddress = 0;
  std::optional<RecordKey> key; // set when the pixels bake a keyed primitive (texture feedback)
  std::uint32_t owner = 0;      // OwnerLedger handle of the upload's source or port

  bool operator==(const VramUpload &) const = default;
};

// Host-only (GpuDevice::declareReadBack): `page` holds read's pixels as indices (r*red + g*green + b*blue) >> shift.
struct ReadBackTexture {
  int readX = 0;
  int readY = 0;
  int readWidth = 0;
  int readHeight = 0;
  int pageX = 0;
  int pageY = 0;
  int pageWidth = 0;
  int pageHeight = 0;
  int texMode = 0;
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
  std::uint8_t shift = 0;

  bool operator==(const ReadBackTexture &) const = default;
};

using RecordEntry = std::variant<DrawPrimitive, VramFill, VramCopy, VramUpload, ReadBackTexture>;

// The OT walk reached the head of `slot` when the record held `entry` entries, with the device under
// `state`. `descending` says the walk visits the slot's table from high bucket indices to low.
struct SlotStart {
  OtSlot slot;
  std::size_t entry = 0;
  bool descending = true;
  RecordDrawState state;

  bool operator==(const SlotStart &) const = default;
};

// The OT walk left `table` when the record held `entry` entries, with the device under `state`.
struct TableExit {
  std::uint16_t table = 0;
  std::size_t entry = 0;
  RecordDrawState state;

  bool operator==(const TableExit &) const = default;
};

class FrameRecord {
public:
  // The record's storage; past either the record is marked incomplete and never replayed.
  static constexpr std::size_t kMaxEntries = 262144;
  static constexpr std::size_t kMaxPoolHalfwords = std::size_t{8} * 1024u * 1024u;

  FrameRecord() = default;
  explicit FrameRecord(std::uint64_t sequence, bool complete) : sequence_(sequence), complete_(complete) {}
  // Empty, as FrameRecord(sequence, complete), in this record's storage.
  void reset(std::uint64_t sequence, bool complete);

  std::uint64_t sequence() const {
    return sequence_;
  }
  // False when the device executed work this record does not describe; its end state is then only in device VRAM.
  bool complete() const {
    return complete_;
  }
  std::span<const RecordEntry> entries() const {
    return entries_;
  }
  std::span<RecordEntry> entries() {
    return entries_;
  }
  std::span<const std::uint16_t> clutPool() const {
    return clutPool_;
  }
  std::span<const std::uint16_t> uploadPixels() const {
    return uploadPixels_;
  }
  // Bucket heads in walk order.
  std::span<const SlotStart> slotStarts() const {
    return slotStarts_;
  }
  // Table exits in walk order.
  std::span<const TableExit> tableExits() const {
    return tableExits_;
  }
  std::span<const std::uint16_t> clut(const DrawPrimitive &primitive) const;
  std::span<const std::uint16_t> pixels(const VramUpload &upload) const;
  bool empty() const {
    return entries_.empty();
  }

  // The display field the record's first draw reached the GPU in: the logic frame's draw entry, which spaces
  // its picture from the one before.
  std::optional<std::uint32_t> drawEntryField() const {
    return drawEntryField_;
  }
  void markDrawEntry(std::uint32_t field) {
    if (!drawEntryField_) {
      drawEntryField_ = field;
    }
  }

  // The row parity (of VRAM y) the record's last drawing entry leaves undrawn, set when the device
  // rasterizes one field per frame (480-line interlace, E1 bit 10 clear); the other parity is the field
  // just drawn. Empty when it draws every row or draws nothing.
  std::optional<int> undrawnRowParity() const;

  void append(const RecordEntry &entry);
  // The most recent entry, or null.
  RecordEntry *last();
  void dropLast();
  // Returns the pool offset of `entries`, or kNoClut when the pool is full.
  std::uint32_t appendClut(std::span<const std::uint16_t> entries);
  // Appends an upload with its pixels; sets upload.pixelOffset.
  void appendUpload(VramUpload upload, std::span<const std::uint16_t> pixels);
  void markIncomplete();
  void beginSlot(OtSlot slot, bool descending, const RecordDrawState &state);
  void exitTable(std::uint16_t table, const RecordDrawState &state);
  // Keeps the pools, which `entries` index as this record's did; drops the slot starts and table exits.
  void replaceEntries(std::span<const RecordEntry> entries);
  // Becomes `other` without its entries, slot starts and table exits, in this record's storage.
  void assignPools(const FrameRecord &other);

  bool operator==(const FrameRecord &) const = default;

private:
  std::uint64_t sequence_ = 0;
  bool complete_ = true;
  std::vector<RecordEntry> entries_;
  std::vector<std::uint16_t> clutPool_;
  std::vector<std::uint16_t> uploadPixels_;
  std::vector<SlotStart> slotStarts_;
  std::vector<TableExit> tableExits_;
  std::optional<std::uint32_t> drawEntryField_;
};

} // namespace psx::present
