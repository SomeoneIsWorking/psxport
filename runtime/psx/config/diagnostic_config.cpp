#include "diagnostic_config.h"

#include "config_var.h"
#include "number_text.h"

#include <lucent/log.h>

namespace psx::config {
namespace {

Var<std::string> cv_store_watch("PSXPORT_CW", "", kRunOnly);
Var<long> cv_store_watch_limit("PSXPORT_CW_MAX", 64, kRunOnly);
Var<bool> cv_store_watch_registers("PSXPORT_CW_GPR", false, kRunOnly);
Var<bool> cv_store_watch_backtrace("PSXPORT_CW_BT", false, kRunOnly);
Var<std::string> cv_snapshot_at("PSXPORT_SNAP_AT", "", kRunOnly);
Var<long> cv_snapshot_every("PSXPORT_SNAP_EVERY", 0, kRunOnly);
Var<long> cv_snapshot_max("PSXPORT_SNAP_MAX", 8, kRunOnly);
Var<long> cv_present_dump_first_fence("PSXPORT_FPS60_DUMP_FROM", 0, kRunOnly);
Var<bool> cv_gate_presentation("PSXPORT_GATE_PRESENTATION", false, kRunOnly);
Var<std::string> cv_present_shot_frames("PSXPORT_PRESENT_SHOT_AT", "", kRunOnly);
Var<std::string> cv_gpu_ram_dump("PSXPORT_GRAMDUMP", "", kRunOnly);
Var<std::string> cv_gpu_dump_dir("PSXPORT_GPU_DUMP", "", kRunOnly);
Var<std::string> cv_gpu_shot_frames("PSXPORT_SHOT_AT", "", kRunOnly);
Var<long> cv_gpu_scene_dump_frame("PSXPORT_SCENEDUMP", -1, kRunOnly);
Var<bool> cv_gpu_validation("PSXPORT_GPU_DEBUG", false, kRunOnly);
Var<bool> cv_gpu_fault_override("PSXPORT_GPU_FAULT_OVERRIDE", false, kRunOnly);
Var<std::string> cv_present_sink("PSXPORT_PRESENT_SINK", "", kRunOnly);
Var<std::string> cv_movie_fps("PSXPORT_FMV_FPS", "", kRunOnly);
Var<long> cv_movie_max_frames("PSXPORT_FMV_MAXFRAMES", 0, kRunOnly);
Var<bool> cv_movie_dc_only("PSXPORT_FMV_DCONLY", false, kRunOnly);
Var<bool> cv_movie_row_major("PSXPORT_FMV_ROWMAJOR", false, kRunOnly);
Var<bool> cv_movie_dump_codes("PSXPORT_FMV_DUMPCODES", false, kRunOnly);
Var<std::string> cv_wav_capture_path("PSXPORT_WAV", "", kRunOnly);
Var<std::string> cv_spu_write_backtrace("PSXPORT_SPUBT", "", kRunOnly);
Var<std::string> cv_guest_poke("PSXPORT_GUEST_POKE", "", kRunOnly);
Var<std::string> cv_forced_buttons("PSXPORT_FORCE_BUTTONS", "", kRunOnly);
Var<std::string> cv_forced_hold("PSXPORT_FORCE_HOLD", "", kRunOnly);
Var<long> cv_forced_hold_at("PSXPORT_FORCE_HOLD_AT", 0, kRunOnly);
Var<long> cv_forced_stop_at("PSXPORT_FORCE_STOP_AT", -1, kRunOnly);
Var<std::string> cv_pad_shot_frames("PSXPORT_PAD_SHOT_AT", "", kRunOnly);
Var<std::string> cv_pad_dump_frames("PSXPORT_PAD_DUMP_AT", "", kRunOnly);
Var<bool> cv_no_host_pads("PSXPORT_PAD_NOPAD", false, kRunOnly);
Var<bool> cv_memory_census("PSXPORT_MEMCENSUS", false, kRunOnly);
Var<std::string> cv_memory_census_path("PSXPORT_MEMCENSUS_OUT", "", kRunOnly);

const Registration kRegistration{
    cv_store_watch,       cv_store_watch_limit,    cv_store_watch_registers, cv_store_watch_backtrace,
    cv_snapshot_at,       cv_snapshot_every,       cv_snapshot_max,          cv_present_dump_first_fence,
    cv_gate_presentation, cv_present_shot_frames,  cv_gpu_ram_dump,          cv_gpu_dump_dir,
    cv_gpu_shot_frames,   cv_gpu_scene_dump_frame, cv_gpu_validation,        cv_gpu_fault_override,
    cv_present_sink,      cv_movie_fps,            cv_movie_max_frames,      cv_movie_dc_only,
    cv_movie_row_major,   cv_movie_dump_codes,     cv_wav_capture_path,      cv_spu_write_backtrace,
    cv_guest_poke,        cv_forced_buttons,       cv_forced_hold,           cv_forced_hold_at,
    cv_forced_stop_at,    cv_pad_shot_frames,      cv_pad_dump_frames,       cv_no_host_pads,
    cv_memory_census,     cv_memory_census_path,
};

// A malformed list arms nothing, and says so.
std::vector<std::uint32_t> frameList(const Var<std::string> &var) {
  std::optional<std::vector<std::uint32_t>> frames = parseUnsignedList(var.get(), 0);
  if (!frames) {
    lucent::error("cfg", "{}='{}' is not a list of frame numbers; nothing is scheduled", var.name(), var.get());
    return {};
  }
  return std::move(*frames);
}

std::optional<std::uint16_t> buttonMask(const Var<std::string> &var) {
  if (var.get().empty()) {
    return std::nullopt;
  }
  const std::optional<std::uint32_t> mask = parseUnsigned(var.get(), 16);
  if (!mask || *mask > 0xFFFFu) {
    lucent::error("cfg", "{}='{}' is not a hex pad mask; ignored", var.name(), var.get());
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(*mask);
}

std::uint32_t nonNegative(const Var<long> &var) {
  return var.get() > 0 ? static_cast<std::uint32_t>(var.get()) : 0u;
}

std::optional<GuestPoke> parsePoke(std::string_view entry) {
  const std::vector<std::string_view> fields = splitList(entry, ":");
  if (fields.size() < 2 || fields.size() > 3) {
    return std::nullopt;
  }
  const std::optional<std::uint32_t> address = parseUnsigned(fields[0], 16);
  const std::optional<std::uint32_t> value = parseUnsigned(fields[1], 16);
  const std::optional<std::uint32_t> width = fields.size() == 3 ? parseUnsigned(fields[2], 10) : 1u;
  if (!address || !value || !width || (*width != 1 && *width != 2 && *width != 4)) {
    return std::nullopt;
  }
  return GuestPoke{*address, *value, *width};
}

} // namespace

StoreWatchConfig store_watch() {
  StoreWatchConfig config{cv_store_watch.get(),
                          std::nullopt,
                          nonNegative(cv_store_watch_limit),
                          cv_store_watch_registers.get(),
                          cv_store_watch_backtrace.get()};
  if (const std::optional<NumberPair> pair = parsePair(config.text, kHexRange)) {
    config.range = GuestAddressRange{pair->first, pair->second};
  }
  return config;
}

SnapshotSchedule snapshot_schedule() {
  return {frameList(cv_snapshot_at), nonNegative(cv_snapshot_every), nonNegative(cv_snapshot_max)};
}

long present_dump_first_fence() {
  return cv_present_dump_first_fence.get();
}

bool gate_presentation() {
  return cv_gate_presentation.get();
}

std::string present_shot_frames() {
  return cv_present_shot_frames.get();
}

std::string gpu_ram_dump() {
  return cv_gpu_ram_dump.get();
}

std::string gpu_dump_dir() {
  return cv_gpu_dump_dir.get();
}

std::string gpu_shot_frames() {
  return cv_gpu_shot_frames.get();
}

long gpu_scene_dump_frame() {
  return cv_gpu_scene_dump_frame.get();
}

bool gpu_validation() {
  return cv_gpu_validation.get();
}

bool gpu_fault_override() {
  return cv_gpu_fault_override.get();
}

std::string present_sink() {
  return cv_present_sink.get();
}

std::string movie_fps() {
  return cv_movie_fps.get();
}

long movie_max_frames() {
  return cv_movie_max_frames.get();
}

bool movie_dc_only() {
  return cv_movie_dc_only.get();
}

bool movie_row_major() {
  return cv_movie_row_major.get();
}

bool movie_dump_codes() {
  return cv_movie_dump_codes.get();
}

std::string wav_capture_path() {
  return cv_wav_capture_path.get();
}

std::string spu_write_backtrace() {
  return cv_spu_write_backtrace.get();
}

std::optional<std::vector<GuestPoke>> guest_poke() {
  std::vector<GuestPoke> pokes;
  for (const std::string_view entry : splitList(cv_guest_poke.get(), ",")) {
    const std::optional<GuestPoke> poke = parsePoke(entry);
    if (!poke) {
      return std::nullopt;
    }
    pokes.push_back(*poke);
  }
  return pokes;
}

std::string guest_poke_text() {
  return cv_guest_poke.get();
}

ForcedInput forced_input() {
  return {buttonMask(cv_forced_buttons),
          buttonMask(cv_forced_hold),
          nonNegative(cv_forced_hold_at),
          cv_forced_stop_at.get()};
}

std::vector<std::uint32_t> pad_shot_frames() {
  return frameList(cv_pad_shot_frames);
}

std::vector<std::uint32_t> pad_dump_frames() {
  return frameList(cv_pad_dump_frames);
}

bool no_host_pads() {
  return cv_no_host_pads.get();
}

bool memory_census() {
  return cv_memory_census.get();
}

std::string memory_census_path() {
  return cv_memory_census_path.get();
}

} // namespace psx::config
