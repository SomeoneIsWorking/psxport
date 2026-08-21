// Deterministic PlayStation CD drive timing.
//
// The static recompiler advances the authoritative per-Game guest CPU cycle counter from the
// instructions it executes. The register-level CDC schedules sector events in that domain, so host
// load, debugger stops and rendering speed cannot change guest-visible command/callback ordering.
#pragma once

#include <cstdint>

class Core;

constexpr uint32_t kNominalPsxCpuHz = 33'868'800u;

int cd_drive_sectors_per_second(uint8_t mode);
uint64_t cd_drive_sector_period_instruction_ticks(uint8_t mode);

extern "C" void rec_guest_instruction_ticks(Core *core, uint32_t ticks);
