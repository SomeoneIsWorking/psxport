// render_substrate.h — RenderSubstrate: per-Core host-only render state. Holds no guest memory.
#pragma once
#include "ot_attr.h"
#include "picture_announce.h"
#include "producer_census.h"
#include "proj_params.h"
#include "render_mode.h"

#include <cstdint>

namespace psx::present {

class RenderSubstrate {
public:
  RenderMode mode;
  OtAttr otAttr; // OT/GTE submission attribution (`debug otattr`; ot_attr.h)
  // The per-run producer table, fed from each sealed frame record.
  psx::debug::ProducerCensus census;
  ProjParams projParams; // the projection the game stated through libgte
  // The picture geometry last announced for this Core (picture_announce.h).
  psx::picture::Geometry announcedPicture;
  // The guest address of the last ordering table DMA2 walked (the scene and attribution dumps read it).
  std::uint32_t lastOrderingTable = 0;
};

} // namespace psx::present
