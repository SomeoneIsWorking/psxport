#!/usr/bin/env bash
# Build the FMV decode compare harness (tools/fmv_compare).
#   ./tools/fmv_compare/build.sh && scratch/bin/fmv_compare <lba> <size> <frame#>
# Disc image is resolved by disc.c (PSXPORT_TOMBA2_DISC / PSXPORT_DISC / .env).
# Example (LOGO.STR @ LBA 11491, 2621440 bytes, frame 40):
#   PSXPORT_TOMBA2_DISC=... scratch/bin/fmv_compare 11491 2621440 40
set -eu
cd "$(dirname "$0")/../.."

RT=runtime/recomp
CHDR_DIR=build/vendor/beetle-psx/deps/libchdr
[ -d "$CHDR_DIR" ] || { echo "build libchdr first (run.sh builds it)"; exit 1; }
find_a(){ find "$CHDR_DIR" -name "$1" 2>/dev/null | head -1; }
CHD_LIBS="$(find_a libchdr-static.a) $(find_a libchdr-lzma.a) $(find_a libminiz.a) $(find_a libzstd.a)"

INC="-I$RT -Ivendor/beetle-psx/deps/libchdr/include"
CFLAGS="-O2 -w $INC"
mkdir -p scratch/bin scratch/obj
MED=vendor/beetle-psx/mednafen
INC="$INC -I$MED -I$MED/psx -Ivendor/beetle-psx/libretro-common/include -Ivendor/beetle-psx"
CFLAGS="-O2 -w $INC"
cc $CFLAGS -c tools/fmv_compare/fmv_compare.c -o scratch/obj/fmv_compare.o
cc $CFLAGS -c $RT/native_fmv.c   -o scratch/obj/fmv_native_fmv.o
cc $CFLAGS -c $RT/disc.c         -o scratch/obj/fmv_disc.o
# real mednafen MDEC (oracle) + adapter, so the synthetic table test exercises the true IDCT.
cc $CFLAGS -c $RT/mdec_beetle.c  -o scratch/obj/fmv_mdec_beetle.o
cc $CFLAGS -c $MED/psx/mdec.c    -o scratch/obj/fmv_mdec.o
cc scratch/obj/fmv_compare.o scratch/obj/fmv_native_fmv.o scratch/obj/fmv_disc.o \
   scratch/obj/fmv_mdec_beetle.o scratch/obj/fmv_mdec.o \
   $CHD_LIBS -lm -o scratch/bin/fmv_compare
echo "built scratch/bin/fmv_compare"
