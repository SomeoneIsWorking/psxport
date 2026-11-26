#include "draw_list_report.h"

#include "core.h"
#include "image_identity.h"
#include "owner_ledger.h"

#include <lucent/log.h>
#include <lucent/text.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <string>
#include <type_traits>

namespace psx::debug {

namespace {

using psx::present::DrawPrimitive;
using psx::present::PrimitiveKind;
using psx::present::SinkPlacement;

std::string hex(std::uint32_t value) {
  return lucent::text::json_quote(lucent::format("0x{:08X}", value));
}

const char *kindName(PrimitiveKind kind) {
  switch (kind) {
  case PrimitiveKind::Polygon:
    return "polygon";
  case PrimitiveKind::Line:
    return "line";
  case PrimitiveKind::Sprite:
    return "sprite";
  }
  return "?";
}

// A box of VRAM points as fractions of the screenshot, or null without a placement.
class ScreenBox {
public:
  explicit ScreenBox(const std::optional<SinkPlacement> &placement) : placement_(placement) {}

  void add(float x, float y) {
    x0_ = std::min(x0_, x);
    y0_ = std::min(y0_, y);
    x1_ = std::max(x1_, x);
    y1_ = std::max(y1_, y);
  }

  std::string json() const {
    if (!placement_ || placement_->columns <= 0 || placement_->rows <= 0 || placement_->sink_w <= 0 ||
        placement_->sink_h <= 0 || x0_ > x1_) {
      return "null";
    }
    const SinkPlacement &p = *placement_;
    const auto across = [&p](float x) {
      return (static_cast<float>(p.viewport.x) +
              (x - static_cast<float>(p.x)) * static_cast<float>(p.viewport.w) / static_cast<float>(p.columns)) /
             static_cast<float>(p.sink_w);
    };
    const auto down = [&p](float y) {
      return (static_cast<float>(p.viewport.y) +
              (y - static_cast<float>(p.y)) * static_cast<float>(p.viewport.h) / static_cast<float>(p.rows)) /
             static_cast<float>(p.sink_h);
    };
    return lucent::format("[{:.4f},{:.4f},{:.4f},{:.4f}]", across(x0_), down(y0_), across(x1_), down(y1_));
  }

private:
  std::optional<SinkPlacement> placement_;
  float x0_ = std::numeric_limits<float>::max();
  float y0_ = std::numeric_limits<float>::max();
  float x1_ = std::numeric_limits<float>::lowest();
  float y1_ = std::numeric_limits<float>::lowest();
};

std::string writerJson(Core &core, std::uint32_t owner) {
  const psx::present::GuestWriter *writer = core.owners.writer(owner);
  if (writer == nullptr) {
    return "null";
  }
  std::string image;
  if (const std::optional<cpu::ImageDescription> described = core.imageCatalog().describe(writer->image)) {
    image = described->name;
  }
  return lucent::format("{{\"kind\":{},\"function\":{},\"block_pc\":{},\"dispatched\":{},\"image\":{}}}",
                        lucent::text::json_quote(psx::present::writerKindName(writer->kind)),
                        hex(writer->function),
                        hex(writer->blockPc),
                        hex(writer->dispatched),
                        lucent::text::json_quote(image));
}

std::string
primitiveJson(Core &core, std::size_t index, const DrawPrimitive &p, const std::optional<SinkPlacement> &placement) {
  ScreenBox box(placement);
  std::string vertices;
  for (int i = 0; i < p.vertexCount; i++) {
    const psx::present::RecordVertex &v = p.vertices.at(static_cast<std::size_t>(i));
    const float x = static_cast<float>(v.x) + v.subX;
    const float y = static_cast<float>(v.y) + v.subY;
    box.add(x, y);
    vertices += lucent::format("{}{{\"x\":{},\"y\":{},\"sub\":[{:.3f},{:.3f}],\"uv\":[{},{}],\"rgb\":[{},{},{}],"
                               "\"depth\":{:.1f}}}",
                               i == 0 ? "" : ",",
                               v.x,
                               v.y,
                               v.subX,
                               v.subY,
                               v.u,
                               v.v,
                               v.r,
                               v.g,
                               v.b,
                               v.depth);
  }
  if (p.kind == PrimitiveKind::Sprite) {
    box.add(static_cast<float>(p.vertices[0].x + p.width), static_cast<float>(p.vertices[0].y + p.height));
  }
  const std::string slot = p.slot ? lucent::format("[{},{}]", p.slot->table, p.slot->index) : std::string{"null"};
  return lucent::format(
      "{{\"i\":{},\"type\":{},\"cmd\":\"0x{:02X}\",\"screen\":{},\"vertices\":[{}],\"size\":[{},{}],\"textured\":{},"
      "\"semi\":{},\"blend\":{},\"tpage\":[{},{}],\"texmode\":{},\"clut\":\"0x{:04X}\",\"offset\":[{},{}],"
      "\"clip\":[{},{},{},{}],\"ui\":{},\"ot\":{},\"site\":{},\"packet\":{},\"source\":{},\"writer\":{}}}",
      index,
      lucent::text::json_quote(kindName(p.kind)),
      p.command,
      box.json(),
      vertices,
      p.width,
      p.height,
      p.textured,
      p.semiTransparent,
      p.state.blendMode,
      p.state.texPageX,
      p.state.texPageY,
      p.state.texMode,
      p.clutWord,
      p.state.offsetX,
      p.state.offsetY,
      p.state.clipX0,
      p.state.clipY0,
      p.state.clipX1,
      p.state.clipY1,
      p.ui,
      slot,
      hex(p.site),
      hex(p.packet),
      hex(p.sourceAddress),
      writerJson(core, p.owner));
}

std::string entryJson(Core &core,
                      std::size_t index,
                      const psx::present::RecordEntry &entry,
                      const std::optional<SinkPlacement> &placement) {
  return std::visit(
      [&](const auto &e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, DrawPrimitive>) {
          return primitiveJson(core, index, e, placement);
        } else if constexpr (std::is_same_v<T, psx::present::VramFill>) {
          ScreenBox box(placement);
          box.add(static_cast<float>(e.x), static_cast<float>(e.y));
          box.add(static_cast<float>(e.x + e.width), static_cast<float>(e.y + e.height));
          return lucent::format(
              "{{\"i\":{},\"type\":\"fill\",\"screen\":{},\"rect\":[{},{},{},{}],\"value\":\"0x{:04X}\"}}",
              index,
              box.json(),
              e.x,
              e.y,
              e.width,
              e.height,
              e.value);
        } else if constexpr (std::is_same_v<T, psx::present::VramCopy>) {
          ScreenBox box(placement);
          box.add(static_cast<float>(e.dstX), static_cast<float>(e.dstY));
          box.add(static_cast<float>(e.dstX + e.width), static_cast<float>(e.dstY + e.height));
          return lucent::format("{{\"i\":{},\"type\":\"copy\",\"screen\":{},\"from\":[{},{}],\"rect\":[{},{},{},{}]}}",
                                index,
                                box.json(),
                                e.srcX,
                                e.srcY,
                                e.dstX,
                                e.dstY,
                                e.width,
                                e.height);
        } else if constexpr (std::is_same_v<T, psx::present::VramUpload>) {
          return lucent::format("{{\"i\":{},\"type\":\"upload\",\"rect\":[{},{},{},{}],\"source\":{}}}",
                                index,
                                e.x,
                                e.y,
                                e.width,
                                e.height,
                                hex(e.sourceAddress));
        } else {
          return lucent::format("{{\"i\":{},\"type\":\"readback\",\"read\":[{},{},{},{}]}}",
                                index,
                                e.readX,
                                e.readY,
                                e.readWidth,
                                e.readHeight);
        }
      },
      entry);
}

std::string headerJson(std::uint64_t sequence, std::size_t entries, const std::optional<SinkPlacement> &placement) {
  const std::string placed =
      placement ? lucent::format("{{\"sink\":[{},{}],\"viewport\":[{},{},{},{}],\"vram\":[{},{},{},{}]}}",
                                 placement->sink_w,
                                 placement->sink_h,
                                 placement->viewport.x,
                                 placement->viewport.y,
                                 placement->viewport.w,
                                 placement->viewport.h,
                                 placement->x,
                                 placement->y,
                                 placement->columns,
                                 placement->rows)
                : std::string{"null"};
  return lucent::format(
      "{{\"record\":{},\"entries\":{},\"placement\":{},\"notes\":{}}}",
      sequence,
      entries,
      placed,
      lucent::text::json_quote(
          "One line per record entry in draw order. screen is [x0,y0,x1,y1] as fractions of screen.png, the units of "
          "report.json marks; vertex x/y are VRAM pixels after the draw offset, sub their place past the whole pixel "
          "above native scale; vram is the columns and rows the viewport shows; writer is the guest code that "
          "stored the packet's first word."));
}

} // namespace

void DrawListReport::capture(const psx::present::FrameRecord &record) {
  const std::span<const psx::present::RecordEntry> entries = record.entries();
  entries_.assign(entries.begin(), entries.end());
  sequence_ = record.sequence();
  captured_ = true;
}

bool DrawListReport::write(Core &core, const std::filesystem::path &path) const {
  std::ofstream out(path, std::ios::trunc);
  out << headerJson(sequence_, entries_.size(), placement_) << '\n';
  for (std::size_t i = 0; i < entries_.size(); i++) {
    out << entryJson(core, i, entries_[i], placement_) << '\n';
  }
  return static_cast<bool>(out);
}

void DrawListReport::clear() {
  entries_.clear();
  sequence_ = 0;
  placement_.reset();
  captured_ = false;
}

} // namespace psx::debug
