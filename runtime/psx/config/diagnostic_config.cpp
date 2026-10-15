// diagnostic_config.cpp — see diagnostic_config.h.
#include "diagnostic_config.h"

#include "config_var.h"

namespace psx::config {
namespace {

TextVar cv_cpu_store_watch("PSXPORT_CW",
                           "",
                           "log host-side stores into a physical range: \"lo,hi\" (hex)",
                           /*persistable=*/false);
IntVar cv_cpu_store_watch_limit("PSXPORT_CW_MAX", 64, "PSXPORT_CW hits logged (0 = unlimited)", /*persistable=*/false);
BoolVar
    cv_cpu_store_watch_backtrace("PSXPORT_CW_BT", false, "host backtrace per PSXPORT_CW hit", /*persistable=*/false);
TextVar cv_guest_store_watch("PSXPORT_WWATCH",
                             "",
                             "log the guest PC of stores into a KSEG range: \"lo,hi\" (hex)",
                             /*persistable=*/false);
BoolVar cv_guest_store_watch_registers("PSXPORT_WWATCH_GPR",
                                       false,
                                       "the whole GPR file per PSXPORT_WWATCH hit",
                                       /*persistable=*/false);
BoolVar cv_guest_store_watch_backtrace("PSXPORT_WWATCH_BT",
                                       false,
                                       "host backtrace per PSXPORT_WWATCH hit",
                                       /*persistable=*/false);
BoolVar cv_spu_dma_log("PSXPORT_SPUDMA", false, "log RAM-to-SPU DMA transfers", /*persistable=*/false);
TextVar
    cv_snapshot_frames("PSXPORT_SNAP_AT", "", "machine snapshot at these frames: comma list", /*persistable=*/false);
TextVar cv_snapshot_every("PSXPORT_SNAP_EVERY", "", "machine snapshot every N frames", /*persistable=*/false);
TextVar cv_snapshot_max("PSXPORT_SNAP_MAX", "", "cap on scheduled machine snapshots", /*persistable=*/false);
IntVar cv_present_dump_first_fence("PSXPORT_FPS60_DUMP_FROM",
                                   0,
                                   "first presentation fence the fps60dump channel writes",
                                   /*persistable=*/false);
BoolVar cv_gate_presentation("PSXPORT_GATE_PRESENTATION",
                             false,
                             "fail the frame when the presentation ledger does not reconcile",
                             /*persistable=*/false);
TextVar cv_present_shot_frames("PSXPORT_PRESENT_SHOT_AT",
                               "",
                               "screenshot the presented image at these present indices: comma list",
                               /*persistable=*/false);
TextVar cv_gpu_ram_dump("PSXPORT_GRAMDUMP", "", "write guest RAM at \"frame:path\"", /*persistable=*/false);
TextVar cv_gpu_dump_dir("PSXPORT_GPU_DUMP",
                        "",
                        "write a PPM of every presented frame into this directory",
                        /*persistable=*/false);
TextVar cv_gpu_shot_frames("PSXPORT_SHOT_AT",
                           "",
                           "screenshot the native frame at these frames: comma list",
                           /*persistable=*/false);
IntVar cv_gpu_scene_dump_frame("PSXPORT_SCENEDUMP",
                               -1,
                               "log the ordering-table scene at this frame (-1 = off)",
                               /*persistable=*/false);
BoolVar
    cv_gpu_validation("PSXPORT_GPU_DEBUG", false, "create the SDL GPU device with validation", /*persistable=*/false);
BoolVar cv_gpu_fault_override("PSXPORT_GPU_FAULT_OVERRIDE",
                              false,
                              "start despite a recorded GPU fault",
                              /*persistable=*/false);
TextVar cv_present_sink("PSXPORT_PRESENT_SINK", "", "headless present sink size \"WxH\"", /*persistable=*/false);
TextVar cv_movie_fps("PSXPORT_FMV_FPS", "", "0 plays movies uncapped", /*persistable=*/false);
IntVar cv_movie_max_frames("PSXPORT_FMV_MAXFRAMES", 0, "movie frames to play (0 = all)", /*persistable=*/false);
BoolVar cv_movie_dc_only("PSXPORT_FMV_DCONLY",
                         false,
                         "decode movie blocks from DC only (diagnostic)",
                         /*persistable=*/false);
BoolVar cv_movie_row_major("PSXPORT_FMV_ROWMAJOR",
                           false,
                           "place movie blocks row-major (A/B diagnostic)",
                           /*persistable=*/false);
BoolVar cv_movie_dump_codes("PSXPORT_FMV_DUMPCODES", false, "log decoded movie run-level codes", /*persistable=*/false);
TextVar cv_wav_capture_path("PSXPORT_WAV", "", "write the mixed audio to this WAV file", /*persistable=*/false);
TextVar cv_spu_write_backtrace("PSXPORT_SPUBT",
                               "",
                               "host backtrace on writes to this SPU register offset (hex)",
                               /*persistable=*/false);
TextVar cv_guest_poke("PSXPORT_GUEST_POKE",
                      "",
                      "guest writes \"addr:val[:width],...\" (hex) applied each frame",
                      /*persistable=*/false);
TextVar cv_forced_buttons("PSXPORT_FORCE_BUTTONS", "", "pulse this active-low pad mask (hex)", /*persistable=*/false);
TextVar cv_forced_hold("PSXPORT_FORCE_HOLD",
                       "",
                       "hold this active-low pad mask (hex) from PSXPORT_FORCE_HOLD_AT",
                       /*persistable=*/false);
TextVar cv_forced_hold_at("PSXPORT_FORCE_HOLD_AT", "", "frame PSXPORT_FORCE_HOLD starts", /*persistable=*/false);
IntVar
    cv_forced_stop_at("PSXPORT_FORCE_STOP_AT", -1, "frame all forced input stops (-1 = never)", /*persistable=*/false);
TextVar
    cv_pad_shot_frames("PSXPORT_PAD_SHOT_AT", "", "screenshot at these pad frames: comma list", /*persistable=*/false);
TextVar
    cv_pad_dump_frames("PSXPORT_PAD_DUMP_AT", "", "dump RAM at these pad frames: comma list", /*persistable=*/false);
TextVar cv_pad_trace("PSXPORT_PAD_TRACE", "", "trace pad frames \"lo-hi\" or up to \"hi\"", /*persistable=*/false);
BoolVar cv_no_host_pads("PSXPORT_PAD_NOPAD", false, "open no host controllers", /*persistable=*/false);
BoolVar cv_memory_census("PSXPORT_MEMCENSUS", false, "count host allocations by call site", /*persistable=*/false);
TextVar cv_memory_census_path("PSXPORT_MEMCENSUS_OUT",
                              "",
                              "host allocation census output (default scratch/raw/memcensus.txt)",
                              /*persistable=*/false);

} // namespace

std::string cpu_store_watch() {
  return cv_cpu_store_watch.get();
}

long cpu_store_watch_limit() {
  return cv_cpu_store_watch_limit.get();
}

bool cpu_store_watch_backtrace() {
  return cv_cpu_store_watch_backtrace.get();
}

std::string guest_store_watch() {
  return cv_guest_store_watch.get();
}

bool guest_store_watch_registers() {
  return cv_guest_store_watch_registers.get();
}

bool guest_store_watch_backtrace() {
  return cv_guest_store_watch_backtrace.get();
}

bool spu_dma_log() {
  return cv_spu_dma_log.get();
}

std::string snapshot_frames() {
  return cv_snapshot_frames.get();
}

std::string snapshot_every() {
  return cv_snapshot_every.get();
}

std::string snapshot_max() {
  return cv_snapshot_max.get();
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

std::string guest_poke() {
  return cv_guest_poke.get();
}

std::string forced_buttons() {
  return cv_forced_buttons.get();
}

std::string forced_hold() {
  return cv_forced_hold.get();
}

std::string forced_hold_at() {
  return cv_forced_hold_at.get();
}

long forced_stop_at() {
  return cv_forced_stop_at.get();
}

std::string pad_shot_frames() {
  return cv_pad_shot_frames.get();
}

std::string pad_dump_frames() {
  return cv_pad_dump_frames.get();
}

std::string pad_trace() {
  return cv_pad_trace.get();
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
