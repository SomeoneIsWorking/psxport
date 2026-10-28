# SDL3 GPU renderer contract

SDL3 owns the product window, input, audio, and GPU integration. The renderer uses SDL's native
backend for each platform: Metal on Apple platforms and Vulkan or D3D12 where SDL supports them. The
product does not carry a second windowing or graphics stack.

## Ownership

- `runtime/psx/render/render_device.*` (`RenderDevice`), `render_window.*` and `render_submit.cpp` own the SDL GPU
  device, window claim, swapchain, samplers, pipelines and submits; `render_present.cpp` (`RenderState`) one
  Game's present to the sink; `record_rasterizer.*` the VRAM image, canvases and record passes;
  `render_readback.cpp` readback.
- A device-lost or fence-timeout fault latches the renderer off for the rest of that process (`enabled()` turns
  false, so no later present acquires or submits; work submitted to a lost device never retires) and is written
  to `gpu_fault` in the user data directory (`~/.config/psxport/` on Linux). The window keeps its last picture
  while the guest, audio and input run on. Every later process logs the record as a warning until the file is
  deleted, and starts normally: the kernel resets the ring, a lost device is reported to innocent contexts
  too, and the marker cannot tell which process hung the GPU (`journalctl -k` names it).
- `runtime/psx/shaders_gpu/` owns first-party GLSL sources. `tools/gen_gpu_shaders.py` compiles and
  embeds them in a consumer-owned build directory; no shader header is written to the source tree.
- `docs/presentation.md` is the renderer's contract: what each render path presents and how.
- `RenderState::buildPanes` (`runtime/psx/render/render_panes.cpp`) draws a host screen's panes into its own present image,
  sampling other Games' completed present images without sharing per-Core renderer state.

## Behavioral requirements

- Preserve the 16-bit PSX VRAM representation, CLUT decode, semitransparency modes, primitive order,
  display-region selection, letterboxing, fades, and bounded readback.
- A tool run and a player run use the same window, swapchain and rendering pipeline; only the window's
  visibility differs (`PSXPORT_SHOW_WINDOW`, `presentation.md`).
- Build-owned generated headers are namespaced by consumer so configuring or cleaning one build
  cannot affect another.
- Apple shader delivery uses SDL_shadercross from the same authored shader sources; platform-specific
  shader copies are not permitted.

## Remaining capability gaps

The authoritative status is `docs/project-state.md`. At present, native 3D parity and Apple Metal
shader delivery still require complete verification. A renderer capability is not verified by a
device-creation check, an internal trace, or a single boot/FMV scene; representative interactive
rendering and the declared platform matrix are required.
