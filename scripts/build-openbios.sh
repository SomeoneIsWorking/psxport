#!/bin/sh
# Builds the psxport fastboot OpenBIOS (FASTBOOT=1: no shell, boots the disc
# executable directly) from a sparse clone of pcsx-redux (MIT).
# Requires: gcc-mips64-linux-gnu binutils-mips64-linux-gnu
set -eu
repo="$(cd "$(dirname "$0")/.." && pwd)"
src="$repo/vendor/openbios-src"
if [ ! -d "$src" ]; then
  git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/grumpycoders/pcsx-redux.git "$src"
  git -C "$src" sparse-checkout set src/mips third_party/EASTL/include
  git -C "$src" submodule update --init --depth 1 third_party/uC-sdk
  git -C "$src" apply "$repo/patches/openbios/0001-psxport-boot-logs.patch"
fi
make -C "$src/src/mips/openbios" FASTBOOT=1 PREFIX=mips64-linux-gnu \
  FORMAT=elf32-tradlittlemips -j"$(nproc)"
cp "$src/src/mips/openbios/openbios.bin" "$repo/bios/openbios-fast.bin"
echo "built: $repo/bios/openbios-fast.bin"
