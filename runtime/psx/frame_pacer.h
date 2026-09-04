// frame_pacer.h — shared display-field cadence, guest-time delivery, and optional host sleep.
#pragma once

class Core;

void gpu_pace_frame(Core *core);
void gpu_pace_subframe(Core *core, int parts);
void gpu_pace_subframe_fields(Core *core, int guestFields, int parts);

// The display field rate decoded from the standard the guest programmed through GP1(0x08).
unsigned gpu_field_rate_millihz(Core *core);
