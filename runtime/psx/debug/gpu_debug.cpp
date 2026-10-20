// gpu_debug.cpp — see gpu_debug.h. Each view reads guest RAM and the device; none mutates either.
#include "gpu_debug.h"

#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "gp0_command.h"
#include "ordering_table.h"

#include <lucent/log.h>

#include <cstdlib>
#include <string_view>

namespace {

template <typename Producer> void logDiagnosticLines(std::string_view channel, Producer &&producer) {
  char *buffer = nullptr;
  size_t size = 0;
  FILE *stream = open_memstream(&buffer, &size);
  if (!stream) {
    lucent::error("gpu-diag", "could not allocate the {} diagnostic stream", channel);
    return;
  }
  producer(stream);
  fclose(stream);

  const std::string_view output(buffer ? buffer : "", size);
  size_t begin = 0;
  while (begin < output.size()) {
    const size_t end = output.find('\n', begin);
    const size_t count = end == std::string_view::npos ? output.size() - begin : end - begin;
    lucent::log(lucent::Level::Info, channel, output.substr(begin, count));
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  free(buffer);
}

void writeScene(Core &core, FILE *out, uint32_t madr) {
  using psx::gpu::Gp0Command;
  const int frame = core.game->display.frame();
  int npoly = 0;
  int nrect = 0;
  int nline = 0;
  int nfill = 0;
  int ncopy = 0;
  int nup = 0;
  int nenv = 0;
  fprintf(out, "[scene] f%d OT@0x%08X — classified display list:\n", frame, psx::gpu::guestAddressOf(madr));
  psx::gpu::OrderingTableCursor cursor(core, madr);
  do {
    const psx::gpu::OtNode &node = cursor.node();
    const unsigned n = node.gp0WordCount();
    const auto word = [&core, &node, n](unsigned index) {
      return index < n ? core.mem_r32(node.gp0WordRamOffset(index)) : 0u;
    };
    for (unsigned i = 0; i < n;) {
      const uint32_t c = word(i);
      const Gp0Command command(c);
      const uint32_t w1 = word(i + 1);
      const uint32_t w2 = word(i + 2);
      if (command.opcode() == psx::gpu::Gp0Opcode::FillRect) {
        nfill++;
        fprintf(out,
                "  FILL rgb=(%d,%d,%d) at(%d,%d) %dx%d\n",
                c & 0xFF,
                (c >> 8) & 0xFF,
                (c >> 16) & 0xFF,
                w1 & 0x3FF,
                (w1 >> 16) & 0x1FF,
                w2 & 0x3FF,
                (w2 >> 16) & 0x1FF);
      } else if (command.isVramCopy()) {
        ncopy++;
        const uint32_t w3 = word(i + 3);
        fprintf(out,
                "  COPY src(%d,%d)->dst(%d,%d) %dx%d [reflection/fade]\n",
                w1 & 0x3FF,
                (w1 >> 16) & 0x1FF,
                w2 & 0x3FF,
                (w2 >> 16) & 0x1FF,
                w3 & 0x3FF,
                (w3 >> 16) & 0x1FF);
      } else if (command.isCpuToVramUpload()) {
        nup++;
      } else if (command.isDrawEnvironmentCommand()) {
        nenv++;
      } else if (command.isPolygon()) {
        npoly++;
        if (command.flags().semiTransparent && !command.flags().textured) {
          fprintf(
              out, "  POLY semi flat rgb=(%d,%d,%d) [fade/overlay?]\n", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
        }
      } else if (command.isRectangleOrSprite()) {
        nrect++;
      } else if (command.isLineOrPolyLine()) {
        nline++;
        break; // a poly-line runs to its terminator
      }
      i += Gp0Command::packetWordCount(c);
    }
  } while (cursor.advance());
  fprintf(out,
          "[scene] f%d totals: poly=%d rect=%d line=%d fill=%d vramcopy=%d upload=%d env=%d\n",
          frame,
          npoly,
          nrect,
          nline,
          nfill,
          ncopy,
          nup,
          nenv);
}

} // namespace

namespace psx::debug {

void logScene(Core &core, uint32_t madr) {
  logDiagnosticLines("scene", [&core, madr](FILE *output) {
    writeScene(core, output, madr);
  });
}

void dumpScene(Core &core, FILE *out) {
  writeScene(core, out, core.rsub.lastOrderingTable);
}

void dumpDisplay(Core &core, FILE *out) {
  const psx::gpu::DisplayArea area = core.gpuDevice().displayArea();
  const psx::gpu::VramPoint draw = core.gpuDevice().drawAreaOrigin();
  fprintf(out,
          "[disp] f%d display VRAM rect = (%d,%d) %dx%d %s%s\n",
          core.game->display.frame(),
          area.x,
          area.y,
          area.width,
          area.height,
          area.pal ? "PAL" : "NTSC",
          area.depth24 ? "  24-BIT" : "");
  fprintf(out, "  draw area origin = (%d,%d)\n", draw.x, draw.y);
}

} // namespace psx::debug
