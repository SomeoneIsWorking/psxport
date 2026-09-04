# SDL3 GPU renderer contract

SDL3 owns the product window, input, audio, and GPU integration. The renderer uses SDL's native
backend for each platform: Metal on Apple platforms and Vulkan or D3D12 where SDL supports them. The
product does not carry a second windowing or graphics stack.

## Ownership

- `runtime/psx/gpu_vk.cpp` owns the SDL GPU device, window claim, swapchain, VRAM textures, transfer
  buffers, pipelines, presentation, readback, and dirty-region synchronization.
- `runtime/psx/shaders_gpu/` owns first-party GLSL sources. `tools/gen_gpu_shaders.py` compiles and
  embeds them in a consumer-owned build directory; no shader header is written to the source tree.
- `runtime/psx/gpu_native.cpp` and the render-queue owners provide primitive and presentation work
  through the renderer's narrow interfaces.
- `runtime/psx/sbs_pane_layout.h` owns side-by-side pane geometry. Each Game renders independently;
  composition consumes the completed panes without sharing per-Core renderer state.

## Behavioral requirements

- Preserve the 16-bit PSX VRAM representation, CLUT decode, semitransparency modes, primitive order,
  depth policy, display-region selection, letterboxing, fades, and bounded readback.
- Headless and windowed execution use the same rendering pipeline. Only the final sink differs.
- Build-owned generated headers are namespaced by consumer so configuring or cleaning one build
  cannot affect another.
- Apple shader delivery uses SDL_shadercross from the same authored shader sources; platform-specific
  shader copies are not permitted.

## Remaining capability gaps

The authoritative status is `docs/project-state.md`. At present, native 3D parity and Apple Metal
shader delivery still require complete verification. A renderer capability is not verified by a
device-creation check, an internal trace, or a single boot/FMV scene; representative interactive
rendering and the declared platform matrix are required.
