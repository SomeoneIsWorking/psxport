#!/usr/bin/env bash
# Build the standalone offline .STR exporter (tools/fmv_export). Links the SAME decode code the
# runtime player runs — fmv_decode.cpp + mdec_beetle.c + vendored mednafen mdec.c — plus lucent,
# cfg, and static libchdr (the .a the port build uses). No game/SDL deps.
set -e
cd "$(dirname "$0")/../.."

RE=runtime/recomp
MED=vendor/beetle-psx/mednafen
OBJ=scratch/obj/fmv_export
mkdir -p "$OBJ"

INC="-I$RE -Ivendor/beetle-psx/deps/libchdr/include -Ivendor/lucent/include"
INC="$INC -I$MED -I$MED/psx -Ivendor/beetle-psx -Ivendor/beetle-psx/libretro-common/include"

find_a(){ find build -name "$1" 2>/dev/null | head -1; }
CHD_LIBS="$(find_a libchdr-static.a) $(find_a libchdr-lzma.a) $(find_a libminiz.a)"
ZSTD_A="$(find_a libzstd.a)"; [ -n "$ZSTD_A" ] && CHD_LIBS="$CHD_LIBS $ZSTD_A" || CHD_LIBS="$CHD_LIBS $(pkg-config --libs libzstd 2>/dev/null || echo -lzstd)"
[ -n "$(find_a libchdr-static.a)" ] || { echo "libchdr not built — run the game's cmake configure once" >&2; exit 1; }

# .c TUs MUST be compiled as C (cc), else the C-linkage mdec_* symbols get C++-mangled and the
# extern "C" declarations in c_subsys.h can't find them. .cpp TUs are C++ (lucent needs C++20).
cc  -O2 -w $INC -c "$MED/psx/mdec.c"      -o "$OBJ/mdec.o"
cc  -O2 -w $INC -c "$RE/mdec_beetle.c"    -o "$OBJ/mdec_beetle.o"
g++ -std=c++20 -O2 $INC -c "$RE/fmv_decode.cpp"  -o "$OBJ/fmv_decode.o"
g++ -std=c++20 -O2 $INC -c "$RE/cfg.cpp"         -o "$OBJ/cfg.o"
g++ -std=c++20 -O2 $INC -c vendor/lucent/src/config.cpp -o "$OBJ/lucent_config.o"
g++ -std=c++20 -O2 $INC -c vendor/lucent/src/log.cpp    -o "$OBJ/lucent_log.o"
g++ -std=c++20 -O2 $INC -c tools/fmv_export/fmv_export.cpp -o "$OBJ/fmv_export.o"

g++ -o tools/fmv_export/fmv_export \
  "$OBJ/fmv_export.o" "$OBJ/fmv_decode.o" "$OBJ/cfg.o" \
  "$OBJ/lucent_config.o" "$OBJ/lucent_log.o" \
  "$OBJ/mdec_beetle.o" "$OBJ/mdec.o" \
  $CHD_LIBS -lpthread -lz -lm
echo "built tools/fmv_export/fmv_export"

# TDD suite: links the SAME decode objects, pins the decode against the Python
# oracle's goldens (all-white drain-completeness, flat-color blocks, LOGO frames).
g++ -std=c++20 -O2 $INC -c tools/fmv_export/test_fmv_decode.cpp -o "$OBJ/test_fmv_decode.o"
g++ -o tools/fmv_export/test_fmv_decode \
  "$OBJ/test_fmv_decode.o" "$OBJ/fmv_decode.o" "$OBJ/cfg.o" \
  "$OBJ/lucent_config.o" "$OBJ/lucent_log.o" \
  "$OBJ/mdec_beetle.o" "$OBJ/mdec.o" \
  $CHD_LIBS -lpthread -lz -lm -lcrypto
echo "built tools/fmv_export/test_fmv_decode"
