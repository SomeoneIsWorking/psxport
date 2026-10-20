// The opt-in diagnostic knobs: probes, dumps, watches and forced input, each a run-only knob read through these.
#pragma once

#include "guest_address_range.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace psx::config {

// PSXPORT_CW: log stores into "lo,hi" (hex); _MAX caps the hits logged (0 unlimited), _GPR adds registers, _BT a backtrace.
struct StoreWatchConfig {
  std::string text;
  std::optional<GuestAddressRange> range;
  std::uint32_t limit = 0;
  bool registers = false;
  bool backtrace = false;
};
StoreWatchConfig store_watch();

// PSXPORT_SNAP_AT (field list), PSXPORT_SNAP_EVERY (fields, 0 off), PSXPORT_SNAP_MAX (captures per run).
struct SnapshotSchedule {
  std::vector<std::uint32_t> at;
  std::uint32_t every = 0;
  std::uint32_t max = 0;
};
SnapshotSchedule snapshot_schedule();

long present_dump_first_fence();   // PSXPORT_FPS60_DUMP_FROM
bool gate_presentation();          // PSXPORT_GATE_PRESENTATION
std::string present_shot_frames(); // PSXPORT_PRESENT_SHOT_AT: comma list of present indices

std::string gpu_ram_dump();    // PSXPORT_GRAMDUMP: "frame:path"
std::string gpu_dump_dir();    // PSXPORT_GPU_DUMP
std::string gpu_shot_frames(); // PSXPORT_SHOT_AT: comma list of frames
long gpu_scene_dump_frame();   // PSXPORT_SCENEDUMP (-1 off)

bool gpu_validation();      // PSXPORT_GPU_DEBUG
bool gpu_fault_override();  // PSXPORT_GPU_FAULT_OVERRIDE
std::string present_sink(); // PSXPORT_PRESENT_SINK: "WxH"

std::string movie_fps(); // PSXPORT_FMV_FPS: "0" uncaps
long movie_max_frames(); // PSXPORT_FMV_MAXFRAMES (0 all)
bool movie_dc_only();    // PSXPORT_FMV_DCONLY
bool movie_row_major();  // PSXPORT_FMV_ROWMAJOR
bool movie_dump_codes(); // PSXPORT_FMV_DUMPCODES

std::string wav_capture_path();    // PSXPORT_WAV
std::string spu_write_backtrace(); // PSXPORT_SPUBT: hex register offset

// PSXPORT_GUEST_POKE: "addr:val[:1|2|4],..." (hex) rewritten every frame; nullopt if any entry is malformed.
struct GuestPoke {
  std::uint32_t address = 0;
  std::uint32_t value = 0;
  std::uint32_t width = 1;
};
std::optional<std::vector<GuestPoke>> guest_poke();
std::string guest_poke_text();

// PSXPORT_FORCE_BUTTONS pulses, PSXPORT_FORCE_HOLD holds from _HOLD_AT, all stop at _STOP_AT (-1 never); active-low hex.
struct ForcedInput {
  std::optional<std::uint16_t> pulse;
  std::optional<std::uint16_t> hold;
  std::uint32_t holdAt = 0;
  long stopAt = -1;
};
ForcedInput forced_input();

std::vector<std::uint32_t> pad_shot_frames(); // PSXPORT_PAD_SHOT_AT: pad frames
std::vector<std::uint32_t> pad_dump_frames(); // PSXPORT_PAD_DUMP_AT: pad frames
bool no_host_pads();                          // PSXPORT_PAD_NOPAD

bool memory_census();             // PSXPORT_MEMCENSUS
std::string memory_census_path(); // PSXPORT_MEMCENSUS_OUT

} // namespace psx::config
