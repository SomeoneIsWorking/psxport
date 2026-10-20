#include "pad_frame_probes.h"

#include "core.h"
#include "game.h"
#include "gpu_debug.h"
#include "snapshot.h"

#include <lucent/log.h>

#include <cstdio>
#include <string>

namespace psx::debug {

void PadFrameProbes::configure() {
  configured_ = true;
  if (const std::optional<std::vector<psx::config::GuestPoke>> pokes = psx::config::guest_poke()) {
    pokes_ = *pokes;
  } else {
    lucent::error("poke",
                  "PSXPORT_GUEST_POKE={} REFUSED: expected <hexaddr>:<hexval>[:1|2|4] separated by commas; nothing is poked",
                  psx::config::guest_poke_text());
  }
  for (const psx::config::GuestPoke &poke : pokes_) {
    lucent::info("poke", "every frame: [{:08X}] = {:X} ({} byte(s))", poke.address, poke.value, poke.width);
  }
  shots_ = FrameSchedule(psx::config::pad_shot_frames());
  dumps_ = FrameSchedule(psx::config::pad_dump_frames());
}

void PadFrameProbes::service(Game &game, std::uint32_t padFrame) {
  if (!configured_) {
    configure();
  }
  Core &core = game.core;
  if (shots_.contains(padFrame)) {
    const std::string path = "scratch/screenshots/padshot_" + std::to_string(padFrame) + ".ppm";
    game.display.shot(path.c_str());
    lucent::info("padrec", "shot at replay-frame {} -> {}", padFrame, path);
  }
  if (dumps_.contains(padFrame)) {
    const std::string stem = "scratch/bin/pad";
    Snapshotter::writeRam(core, stem + "ram_" + std::to_string(padFrame) + ".bin");
    const std::string scene = stem + "scene_" + std::to_string(padFrame) + ".txt";
    if (std::FILE *out = std::fopen(scene.c_str(), "w")) {
      dumpScene(core, out);
      std::fclose(out);
    }
  }
  // Written at the frame tick, so a comparison's oracle leg sees them too.
  for (const psx::config::GuestPoke &poke : pokes_) {
    if (poke.width == 1) {
      core.mem_w8(poke.address, static_cast<std::uint8_t>(poke.value));
    } else if (poke.width == 2) {
      core.mem_w16(poke.address, static_cast<std::uint16_t>(poke.value));
    } else {
      core.mem_w32(poke.address, poke.value);
    }
  }
}

} // namespace psx::debug
