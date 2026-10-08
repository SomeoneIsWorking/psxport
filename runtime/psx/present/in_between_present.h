// in_between_present.h — the renderer operation that presents a synthesized in-between frame.
#pragma once

class Core;

// Present the accumulated intermediate batch and reset it for the following real pass. This does
// not advance the logic-frame counter or run real-frame diagnostics.
void gpu_present_in_between(Core *core);
