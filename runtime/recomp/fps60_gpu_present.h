// fps60_gpu_present.h — the renderer operation exclusive to a synthesized temporal pass.
#pragma once

class Core;

// Present the accumulated intermediate batch and reset it for the following real pass. This does
// not advance the logic-frame counter or run real-frame diagnostics.
void gpu_fps60_present_pass(Core *core);
