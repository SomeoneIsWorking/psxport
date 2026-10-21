# 0167: a tool run cannot measure present pacing

Status: fixed. Owner: `runtime/psx/render/render_window.cpp` `RenderWindow::create`, `render_device.cpp`.

## Observed
Every agent run set `PSXPORT_VK_HEADLESS=1` (or left `PSXPORT_VK_WINDOW` unset), so `RenderWindow::create`
returned without a window, `RenderState::showPresentImage` never ran and `GpuPerf::presented` was never
stamped: `[playcheck] presents: intervals=0`. Judder the user sees could not be measured by a tool.

## Cause
`render_window.cpp:RenderWindow::create` had a windowless leg: no window, no swapchain, the present image
built and never shown. A plain `SDL_WINDOW_HIDDEN` window is no better: SDL_GPU's
`VULKAN_INTERNAL_AcquireSwapchainTexture` (SDL 3.4) returns true with a null texture for a hidden window
before asking Vulkan. Run under `SDL_VIDEO_DRIVER=x11`: `[sinkidle] sink idle, no swapchain image`, empty
SDL error, no block, `intervals=0`.

## Fix
One window path. `PSXPORT_SHOW_WINDOW` (replaces `PSXPORT_VK_WINDOW` and `PSXPORT_VK_HEADLESS`) only
decides visibility. Unshown on Wayland, the window is a roleless surface
(`SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN`): SDL counts it as shown, the compositor never
maps it, and it is claimed and presented like the player's (MAILBOX). On other video drivers it is
`SDL_WINDOW_HIDDEN` and reports no intervals, as above.

## Proof
Spyro 1, Clang `build/agent`, `PSXPORT_SHOW_WINDOW=0` read from `/proc/<pid>/environ`, no `PSXPORT_NOPACE`,
shipping settings (fps60), `warp 10` then `run 3000` on the REPL:
`[render] window 2400x1800 pixels roleless, never mapped (video driver wayland)`, `swapchain present mode:
MAILBOX`, `[warp] level 10 started from the title screen at f601`, `[playcheck] presents: intervals=5559
p50=16.75ms p99=17.75ms worst=63.84ms vsync=16.67ms missed=6 (0.11%) longest_miss_run=1`, `records:
presents=3000 in_betweens=2560`. KWin's window list (`org.kde.KWin /WindowsRunner krunner1.Match`) named no
Spyro or psxport window in 17 samples over the run, and named the terminal window as a control.
