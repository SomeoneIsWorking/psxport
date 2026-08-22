// Deterministic PlayStation CD drive timing.
//
// Executed guest instructions and delivered display fields advance one authoritative per-Game
// emulated CPU clock. The register-level CDC schedules sector events in that domain, so host load,
// debugger stops, host pacing, and rendering speed cannot change guest-visible callback ordering.
#pragma once

#include "emulated_time.h"

#include <cstdint>

class Core;

int cd_drive_sectors_per_second(uint8_t mode);
uint64_t cd_drive_sector_period_cpu_ticks(uint8_t mode);

extern "C" void rec_guest_instruction_ticks(Core *core, uint32_t ticks);
