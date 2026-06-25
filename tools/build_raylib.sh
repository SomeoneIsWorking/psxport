#!/usr/bin/env bash
# Build the vendored raylib (vendor/raylib) ONCE into a static library. The SBS two-pane debugger
# (PSXPORT_SBS=1) presents through raylib (OpenGL) instead of the Vulkan swapchain, because MoltenVK
# (macOS) silently rendered the two-pane VK composite black while OpenGL is solid there. Output:
#   build/raylib/raylib/libraylib.a
# Idempotent: re-running is a no-op once the lib exists.
#
# Requires: cmake, a C compiler, and (linux) X11 + GL dev headers. raylib 5.5, GLFW desktop backend.
set -eu
cd "$(dirname "$0")/.."

SRC=vendor/raylib
OUT=build/raylib
LIB="$(find "$OUT" -name 'libraylib.a' 2>/dev/null | head -1 || true)"

[ -n "$LIB" ] && { echo "[build_raylib] up to date ($LIB)"; exit 0; }

command -v cmake >/dev/null || { echo "[build_raylib] cmake not found" >&2; exit 1; }

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Static raylib only (no examples). GLFW desktop backend (OpenGL 3.3). raylib owns NO audio here
# (the port pumps its own SPU audio), so leave raylib's miniaudio in but unused — harmless.
cmake --fresh -S "$SRC" -B "$OUT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DPLATFORM=Desktop \
  >/dev/null

cmake --build "$OUT" -j "$JOBS" --target raylib >/dev/null

echo "[build_raylib] built:"
find "$OUT" -name 'libraylib.a' -printf '  %p\n' 2>/dev/null || true
