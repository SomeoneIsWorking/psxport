# Renderer backend: Vulkan → SDL3 GPU API (one PC-native render API)

**Decision (user, 2026-06-25):** the sole renderer is the **SDL3 GPU API** (`SDL_gpu.h`), PC-native.
- macOS / iOS → **native Metal** (NO MoltenVK — this was the original bug: the SBS two-pane VK composite
  rendered black on MoltenVK).
- Android / Linux / Windows → Vulkan (Win also D3D12).
- ONE stack for window + input + audio + GPU (SDL3), replacing SDL2 + Vulkan + the SDL window.

## Why SDL_GPU
**SDL_GPU maps ~1:1 onto the EXISTING `runtime/recomp/gpu_vk.cpp`** (the renderer that already WORKS on
Linux/AMD and in single-core on the user's Mac via MoltenVK — only the SBS composite was MoltenVK-black).
That makes it a clean translation, and on the Mac it runs native Metal so the original problem is gone.
(An OpenGL-backed presenter was tried first and abandoned — DEAD END: OpenGL is deprecated on macOS /
absent on iOS, and its immediate-mode batch fought a custom integer-VRAM PSX rasterizer. Do not retry it.)

## Asset reuse (big)
- **Shaders:** reuse `runtime/recomp/shaders_vk/*.{vert,frag}` as-is. `tools/gen_vk_shaders.sh` already
  compiles them GLSL→SPIR-V→`gpu_vk_shaders.h` (embedded). SDL_GPU's Vulkan backend consumes SPIR-V
  directly (`SDL_CreateGPUShader` with `SDL_GPU_SHADERFORMAT_SPIRV`). For Metal (Mac): transpile the same
  SPIR-V via **SDL_shadercross** (offline or at load) — add for the Mac build.
- **Renderer design:** `gpu_vk.cpp` is the behavioral reference — VRAM R16_UINT image, the tri/tritex/semi
  pipelines + CLUT decode, depth bands, present (region+letterbox+fade), headless readback, dirty-rects.

## VK → SDL_GPU object mapping
| Vulkan (gpu_vk.cpp) | SDL3 GPU |
|---|---|
| VkInstance/VkDevice/VkQueue | `SDL_GPUDevice` (`SDL_CreateGPUDevice`, claim window) |
| SDL2 VkSurface + swapchain | `SDL_ClaimWindowForGPUDevice` + `SDL_AcquireGPUSwapchainTexture` |
| VkImage s_tex (R16_UINT VRAM, color targets, depth) | `SDL_GPUTexture` (R16_UINT sampler tex; RGBA/color-target; D32 depth) |
| staging buffers + vkCmdCopy | `SDL_GPUTransferBuffer` + `SDL_UploadToGPUTexture` / `SDL_DownloadFromGPUTexture` |
| VkBuffer vertex buffers | `SDL_GPUBuffer` (vertex) + transfer upload |
| VkPipeline (tri/tritex/semi/present/shadow) | `SDL_GPUGraphicsPipeline` (blend states incl. the 4 PSX semi modes via blend factors) |
| vkCmdBeginRenderPass | `SDL_BeginGPURenderPass` (color+depth targets) / `SDL_BeginGPUCopyPass` |
| push constants | `SDL_PushGPUVertexUniformData` / `...FragmentUniformData` |
| readback (vk_readback_to_rb) | `SDL_DownloadFromGPUTexture` + `SDL_MapGPUTransferBuffer` |

## SDL2 → SDL3 migration surface
- `pkg-config sdl2` → `sdl3` (installed: 3.4.10; `SDL3/SDL_gpu.h` present; `glslc`/`glslang` present).
- Audio sink `spu_audio.c` (SDL2 audio → SDL3 `SDL_AudioStream` / new callback API).
- Window + input: SDL3 owns the window; `SDL_GPU` claims it. `pad_input.cpp pad_poll_sdl` → SDL3 input
  (`SDL_GetKeyboardState`, gamepad API renamed `SDL_Gamepad*`). Event/quit poll.
- `-DPSXPORT_SDL` stays. memcard/other SDL uses → SDL3 equivalents.

## Phasing (this is the plan the swarm executes)
1. **Module + build:** new `runtime/recomp/gpu_vk.cpp` providing EVERY `gpu_vk_*`/`gpu_*` entry point from
   `gpu_vk.h` on SDL_GPU. SDL2→SDL3 in CFLAGS/link. Reuse `gpu_vk_shaders.h` (SPIR-V). Build green.
2. **Single-core game renders:** VRAM 2D + native 3D (depth) + present + fade + `shot`. Verify on Linux
   (Vulkan backend) + USER eyeballs Mac (Metal).
3. **SBS:** two offscreen targets composited side-by-side (native Metal → no MoltenVK black).
4. **Delete:** `gpu_vk.cpp` + SDL2 once Pass 2/3 verified.
5. Shadows/SSAO (enhancements) port or defer.

## Status
- [x] Feasibility: SDL3 3.4.10 + SDL_gpu.h + glslc present on the linux box
- [x] **Pass 1 — gpu_vk.cpp on SDL_GPU + SDL3 migration + build green + 2D screens render.**
  - `runtime/recomp/gpu_vk.cpp`: device/window/swapchain on SDL_GPU; VRAM R16_UINT sampler texture
    uploaded each present; present pass (sample VRAM 1555 → swapchain, letterbox 4:3 + fade); fullscreen
    IMAGE present (SCEA/FMV); headless VRAM readback (`shot`). All `gpu_vk_*` symbols provided.
  - shaders: `runtime/recomp/shaders_gpu/{present,image}.{vert,frag}` (SDL_GPU binding convention —
    frag samplers set=2, frag uniform buffers set=3) → `gpu_vk_shaders.h` via `tools/gen_gpu_shaders.sh`.
  - SDL2→SDL3: `pad_input.cpp` (SDL_Gamepad + bool keyboard), `spu_audio.c` + `native_fmv.cpp`
    (SDL_AudioStream), `gpu_native.cpp` (retired the SDL_Renderer SW window — GPU path is THE present).
  - RmlUi overlay dropped (`rmlui_overlay_stub.cpp`); SBS two-pane present is now SDL_GPU (`sbs_present_sdl.cpp` + `gpu_vk_present_sbs2`). Build wires
    `sdl3` (no sdl2/vulkan/GL); `ldd` shows only `libSDL3.so`. Verified WINDOWED on Linux/Vulkan-backend:
    the intro FMV renders correctly (scratch/screenshots/window_grab*.png). gpu_vk.cpp kept for reference.
  - Diagnostics (tracked): `PSXPORT_GPU_TRACE` (per-present src occupancy + disp region; readback nz),
    `PSXPORT_GPU_DEBUG` (SDL_GPU device validation — slows boot, can trip the watchdog).
  - NOTE: 3D `draw_*` are STOPGAP no-ops (Pass 2). Headless `shot` can read the BACK buffer (display
    page x=320 vs the sampled 0,0) at some frames; the WINDOWED present samples the live front buffer
    correctly — that quirk is pre-existing display-page behavior, not a renderer bug.
- [ ] Pass 2 — native 3D raster (draw_tri/tritri/semi) on an R16_UINT color target with IN-SHADER semi
  blend against a VRAM snapshot (no HW blend / no format alias) + a real depth buffer + the 3 ordering
  bands. This is the bulk of gpu_vk.cpp's tri/tritex/semi pipelines re-expressed; rewrite tri/tritex
  fragments to OUTPUT the packed 1555 uint (not the A1R5G5B5 swizzle).
- [ ] Pass 3 — SBS two-target compose (native Metal → no MoltenVK black).
- [ ] delete VK (gpu_vk.cpp) + SDL2 once Pass 2/3 verified.
- [ ] Mac Metal shaders via SDL_shadercross.
