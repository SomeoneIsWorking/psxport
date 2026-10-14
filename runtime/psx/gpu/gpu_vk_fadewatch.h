// gpu_vk_fadewatch — the `debug fadewatch` present tap: the fade composited on each present.
#pragma once

class Core;
struct FadeState;

// Logs only when the fade or the presented rect `sx/sy/w/h` changes, and only on the `fadewatch` channel.
void gpu_vk_fadewatch_present(const FadeState &fade, int sx, int sy, int w, int h);
