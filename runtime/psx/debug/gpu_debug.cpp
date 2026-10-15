// gpu_debug.cpp — see gpu_debug.h. Each view reads guest RAM and the device; none mutates either.
#include "gpu_debug.h"

#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "ot_attr.h" // OtAttr::Span — packet->submitter attribution

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

int gp0CommandLength(uint8_t op) {
  if (op >= 0x20 && op <= 0x3F) {
    int nv = (op & 0x08) ? 4 : 3, per = 1 + ((op & 0x04) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
    return 1 + nv * per - ((op & 0x10) ? 1 : 0);
  }
  if (op >= 0x40 && op <= 0x5F) {
    return 0;
  }
  if (op >= 0x60 && op <= 0x7F) {
    int t = (op & 0x04) ? 1 : 0, sz = (op >> 3) & 3;
    return 1 + 1 + t + (sz == 0 ? 1 : 0);
  }
  if (op == 0x02) {
    return 3;
  }
  if (op >= 0x80 && op <= 0x9F) {
    return 4;
  }
  if (op >= 0xA0 && op <= 0xDF) {
    return 3;
  }
  return 1;
}
void writeScene(Core &core, FILE *out, uint32_t madr) {
  const int frame = core.game->display.frame();
  uint32_t addr = madr & 0x1FFFFC;
  int npoly = 0, nrect = 0, nline = 0, nfill = 0, ncopy = 0, nup = 0, nenv = 0;
  fprintf(out, "[scene] f%d OT@0x%08X — classified display list:\n", frame, 0x80000000u | addr);
  for (int g = 0; g < 0x10000; g++) {
    uint32_t hdr = core.mem_r32(addr);
    int n = static_cast<int>(hdr >> 24);
    int i = 0;
    while (i < n) {
      uint32_t c = core.mem_r32(addr + 4 + i * 4);
      uint8_t op = c >> 24;
      int len = gp0CommandLength(op);
      if (len <= 0) {
        break;
      }
      uint32_t w1 = (i + 1 < n) ? core.mem_r32(addr + 4 + (i + 1) * 4) : 0;
      uint32_t w2 = (i + 2 < n) ? core.mem_r32(addr + 4 + (i + 2) * 4) : 0;
      if (op == 0x02) {
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
      } else if (op >= 0x80 && op <= 0x9F) {
        ncopy++;
        uint32_t w3 = (i + 3 < n) ? core.mem_r32(addr + 4 + (i + 3) * 4) : 0;
        fprintf(out,
                "  COPY src(%d,%d)->dst(%d,%d) %dx%d [reflection/fade]\n",
                w1 & 0x3FF,
                (w1 >> 16) & 0x1FF,
                w2 & 0x3FF,
                (w2 >> 16) & 0x1FF,
                w3 & 0x3FF,
                (w3 >> 16) & 0x1FF);
      } else if (op >= 0xA0 && op <= 0xBF) {
        nup++;
      } else if (op >= 0xE1 && op <= 0xE6) {
        nenv++;
      } else if (op >= 0x20 && op <= 0x3F) {
        npoly++;
        if (((op >> 1) & 1) && !((op >> 2) & 1)) {
          fprintf(
              out, "  POLY semi flat rgb=(%d,%d,%d) [fade/overlay?]\n", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
        }
      } else if (op >= 0x60 && op <= 0x7F) {
        nrect++;
      } else if (op >= 0x40 && op <= 0x5F) {
        nline++;
        break;
      }
      i += len;
    }
    uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) {
      break;
    }
    addr = next & 0x1FFFFC;
  }
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

// WHO SUBMITTED THIS FRAME'S GEOMETRY — the packet->submitter question, on the debug server.
//
// The REPL has had `otattr` for a long time and it was UNREACHABLE where the question is usually asked:
// the REPL blocks the frame loop, so it cannot attach to a live window or to a long resumed session,
// which is exactly where "what draws that thing that is missing" comes up. Same gap `renderpath` had.
//
// Two forms, both read-only:
//   otattr            aggregate the CURRENT ordering table by submitter fn
//   otattr <addr>     attribute one packet address
//
// It reports its DENOMINATORS because the attribution is not total: spans are recorded as the guest
// STORES packets, so a packet whose store the span table missed (or which was written before the table
// was armed) is UNATTRIBUTED, and a table that overflowed says so. An aggregate with no denominator
// would read as "these are all the submitters" when it means "these are the ones I could name".
void dumpOtAttribution(Core &core, FILE *out, uint32_t oneAddr) {
  OtAttr &oa = core.rsub.otAttr;

  if (oneAddr) {
    OtAttr::Span sp{};
    if (oa.lookupStore(oneAddr, &sp)) {
      fprintf(out,
              "[otattr] 0x%08X <- fn=0x%08X caller=0x%08X claimed=0x%08X "
              "(span [0x%08X,0x%08X))\n",
              oneAddr | 0x80000000u,
              sp.fn,
              sp.caller,
              sp.claimed,
              sp.lo,
              sp.hi);
    } else {
      fprintf(out,
              "[otattr] 0x%08X — NO SPAN COVERS IT. That is 'not recorded', NOT 'nobody wrote it': "
              "%d spans this frame%s.\n",
              oneAddr | 0x80000000u,
              oa.spanCount(),
              oa.spanOverflow() ? " (TABLE OVERFLOWED — attribution is incomplete)" : "");
    }
    return;
  }

  // Aggregate the current OT. Same link walk gpu_scene_dump uses; read-only, no gpu_gp0 side effects.
  struct Row {
    uint32_t fn;
    int packets;
  };
  Row rows[64];
  int nrows = 0;
  int nodes = 0, attributed = 0, unattributed = 0, rowsDropped = 0;
  uint32_t addr = core.rsub.lastOrderingTable & 0x1FFFFC;
  for (int guard = 0; guard < 0x10000; guard++) {
    const uint32_t hdr = core.mem_r32(addr);
    const unsigned n = hdr >> 24;
    if (n) {
      nodes++;
      OtAttr::Span sp{};
      if (oa.lookupStore(addr + 4, &sp)) {
        attributed++;
        int i = 0;
        for (; i < nrows; i++) {
          if (rows[i].fn == sp.fn) {
            rows[i].packets++;
            break;
          }
        }
        if (i == nrows) {
          if (nrows < 64) {
            rows[nrows++] = {sp.fn, 1};
          } else {
            rowsDropped++;
          }
        }
      } else {
        unattributed++;
      }
    }
    const uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) {
      break;
    }
    addr = next & 0x1FFFFC;
  }
  // Biggest first — the ranking is the point when the question is "what draws most of this scene".
  for (int i = 0; i < nrows; i++) {
    for (int j = i + 1; j < nrows; j++) {
      if (rows[j].packets > rows[i].packets) {
        Row t = rows[i];
        rows[i] = rows[j];
        rows[j] = t;
      }
    }
  }
  fprintf(out,
          "[otattr] f%d OT@0x%08X: %d drawing nodes, %d attributed, %d UNATTRIBUTED"
          " (%d spans%s)\n",
          core.game->display.frame(),
          0x80000000u | core.rsub.lastOrderingTable,
          nodes,
          attributed,
          unattributed,
          oa.spanCount(),
          oa.spanOverflow() ? ", TABLE OVERFLOWED" : "");
  for (int i = 0; i < nrows; i++) {
    fprintf(out, "  fn=0x%08X  %d packet(s)\n", rows[i].fn, rows[i].packets);
  }
  if (rowsDropped) {
    fprintf(out, "  (+%d distinct fn(s) past the 64-row cap — NOT listed)\n", rowsDropped);
  }
  if (!nodes) {
    fprintf(out, "  the ordering table this frame has no drawing nodes at all.\n");
  }
}

} // namespace psx::debug
