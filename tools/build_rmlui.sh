#!/usr/bin/env bash
# Build the vendored RmlUi (vendor/rmlui) ONCE into a static library, so the port links librmlui.a +
# system freetype instead of compiling all of RmlUi Core per-file with raw g++ (heavy + slow). Output:
#   build/rmlui/.../librmlui.a            (Core; the rmlui_core target's OUTPUT_NAME is "rmlui")
#   build/rmlui/.../librmlui_debugger.a   (visual debugger; F1 in the overlay)
# Idempotent: re-running is a no-op once the Core lib exists.
#
# Requires: cmake, a C++17 compiler, and FreeType 2 dev (pkg-config freetype2). RmlUi 6.2.
set -eu
cd "$(dirname "$0")/.."

SRC=vendor/rmlui
OUT=build/rmlui
# Marker: the Core static lib. RmlUi 6.2's `rmlui_core` target sets OUTPUT_NAME "rmlui", so the file is
# librmlui.a (NOT librmlui_core.a). Its exact path varies by generator, so just check that it exists.
LIB="$(find "$OUT" -name 'librmlui.a' 2>/dev/null | head -1 || true)"

[ -n "$LIB" ] && { echo "[build_rmlui] up to date ($LIB)"; exit 0; }

command -v cmake >/dev/null || { echo "[build_rmlui] cmake not found" >&2; exit 1; }
pkg-config --exists freetype2 || { echo "[build_rmlui] freetype2 dev not found (dnf install freetype-devel)" >&2; exit 1; }

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Static Core + Debugger only. No samples/Lua/SVG/Lottie. FreeType font engine ON (default).
# --fresh: ignore any stale CMakeCache from an earlier (possibly failed) configure, so a re-run after a
# vendored-CMakeLists fix doesn't reuse a poisoned build tree.
cmake --fresh -S "$SRC" -B "$OUT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DRMLUI_SAMPLES=OFF \
  -DRMLUI_LUA_BINDINGS=OFF \
  -DRMLUI_SVG_PLUGIN=OFF \
  -DRMLUI_LOTTIE_PLUGIN=OFF \
  -DRMLUI_FONT_ENGINE=freetype \
  >/dev/null

# Build the two libraries we link (target names: rmlui_core, rmlui_debugger).
cmake --build "$OUT" -j "$JOBS" --target rmlui_core rmlui_debugger >/dev/null

echo "[build_rmlui] built:"
find "$OUT" -name 'librmlui*.a' -printf '  %p\n' 2>/dev/null || true
