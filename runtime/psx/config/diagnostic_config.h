// diagnostic_config.h — the opt-in diagnostic knobs: probes, dumps, watches and forced input.
// Each is a non-persistable CVar in diagnostic_config.cpp; consumers read only these typed values.
// A text knob's format belongs to its one consumer; empty means unset.
#pragma once

#include <string>

namespace psx::config {

// ── guest memory watches (core/mem.cpp) ──────────────────────────────────────────────────────────
std::string cpu_store_watch();      // PSXPORT_CW: "lo,hi" physical range
long cpu_store_watch_limit();       // PSXPORT_CW_MAX: hits logged (0 = unlimited)
bool cpu_store_watch_backtrace();   // PSXPORT_CW_BT
std::string guest_store_watch();    // PSXPORT_WWATCH: "lo,hi" KSEG range
bool guest_store_watch_registers(); // PSXPORT_WWATCH_GPR
bool guest_store_watch_backtrace(); // PSXPORT_WWATCH_BT
bool spu_dma_log();                 // PSXPORT_SPUDMA

// ── machine snapshots (core/snapshot.cpp) ────────────────────────────────────────────────────────
std::string snapshot_frames(); // PSXPORT_SNAP_AT: comma list
std::string snapshot_every();  // PSXPORT_SNAP_EVERY
std::string snapshot_max();    // PSXPORT_SNAP_MAX

// ── presentation (frame/frame_presenter.cpp, gpu/) ──────────────────────────────────────────────
long present_dump_first_fence();   // PSXPORT_FPS60_DUMP_FROM
bool gate_presentation();          // PSXPORT_GATE_PRESENTATION
std::string present_shot_frames(); // PSXPORT_PRESENT_SHOT_AT: comma list of present indices

// ── frame captures (present/display_presenter.cpp, debug/gpu_debug.cpp) ──────────────────────────
std::string gpu_ram_dump();    // PSXPORT_GRAMDUMP: "frame:path"
std::string gpu_dump_dir();    // PSXPORT_GPU_DUMP
std::string gpu_shot_frames(); // PSXPORT_SHOT_AT: comma list of frames
long gpu_scene_dump_frame();   // PSXPORT_SCENEDUMP (-1 = off)

// ── SDL GPU device (gpu/gpu_vk*.cpp) ─────────────────────────────────────────────────────────────
bool gpu_validation();      // PSXPORT_GPU_DEBUG
bool gpu_fault_override();  // PSXPORT_GPU_FAULT_OVERRIDE
std::string present_sink(); // PSXPORT_PRESENT_SINK: "WxH"

// ── movies (movie/) ──────────────────────────────────────────────────────────────────────────────
std::string movie_fps(); // PSXPORT_FMV_FPS: "0" uncaps
long movie_max_frames(); // PSXPORT_FMV_MAXFRAMES (0 = all)
bool movie_dc_only();    // PSXPORT_FMV_DCONLY
bool movie_row_major();  // PSXPORT_FMV_ROWMAJOR
bool movie_dump_codes(); // PSXPORT_FMV_DUMPCODES

// ── audio (audio/) ───────────────────────────────────────────────────────────────────────────────
std::string wav_capture_path();    // PSXPORT_WAV
std::string spu_write_backtrace(); // PSXPORT_SPUBT: hex register offset

// ── pad (input/) ─────────────────────────────────────────────────────────────────────────────────
std::string guest_poke();      // PSXPORT_GUEST_POKE: "addr:val[:width],..."
std::string forced_buttons();  // PSXPORT_FORCE_BUTTONS: hex mask
std::string forced_hold();     // PSXPORT_FORCE_HOLD: hex mask
std::string forced_hold_at();  // PSXPORT_FORCE_HOLD_AT
long forced_stop_at();         // PSXPORT_FORCE_STOP_AT (-1 = never)
std::string pad_shot_frames(); // PSXPORT_PAD_SHOT_AT: comma list
std::string pad_dump_frames(); // PSXPORT_PAD_DUMP_AT: comma list
std::string pad_trace();       // PSXPORT_PAD_TRACE: "lo-hi" or "hi"
bool no_host_pads();           // PSXPORT_PAD_NOPAD

// ── host allocation census (debug/memcensus.cpp) ────────────────────────────────────────────────
bool memory_census();             // PSXPORT_MEMCENSUS
std::string memory_census_path(); // PSXPORT_MEMCENSUS_OUT

} // namespace psx::config
