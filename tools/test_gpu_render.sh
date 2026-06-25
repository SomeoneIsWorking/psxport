#!/usr/bin/env bash
# Renderer regression test (TDD) for the SDL_GPU backend (gpu_gpu.cpp).
#
# Runs the headless self-test (PSXPORT_GPU_SELFTEST=1, GpuVkState::tritest) which renders a known VRAM
# pattern through the REAL present pipeline into an offscreen RGBA8 target and asserts:
#   - ORIENTATION: VRAM row 0 (top) lands at the TOP of the output (guards the swapchain Y-flip — the
#     "rendering upside down" regression).
#   - 1555 UNPACK: present.frag decodes PSX 1555 → RGB correctly (red marker → red, blue → blue).
# No disc/game is loaded (the test runs at boot before load_exe, then exits 0=PASS / 1=FAIL).
#
# Usage: tools/test_gpu_render.sh   (builds gpu_gpu.cpp first, then runs the self-test)
set -eu
cd "$(dirname "$0")/.."

tools/build_port.sh runtime/recomp/gpu_gpu.cpp >/dev/null 2>&1 || { echo "[test_gpu_render] build failed" >&2; exit 2; }

# The self-test only needs the GPU device; pass a dummy argv[1] (boot calls the test before load_exe).
out="$(PSXPORT_GPU_SELFTEST=1 PSXPORT_VK_HEADLESS=1 ./scratch/bin/tomba2_port /dev/null 2>&1 || true)"
echo "$out" | grep -E '\[gpu_selftest\]' || true

if echo "$out" | grep -q '\[gpu_selftest\].*PASS'; then
  echo "[test_gpu_render] PASS"
  exit 0
else
  echo "[test_gpu_render] FAIL" >&2
  exit 1
fi
