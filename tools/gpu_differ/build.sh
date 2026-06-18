#!/usr/bin/env bash
# Build the "ours" GP0-replay harness (links our renderer gpu_native.c standalone).
# Output: scratch/bin/replay_ours
set -eu
cd "$(dirname "$0")/../.."
CC="${CC:-cc}"
mkdir -p scratch/bin
$CC -O2 -g -w -Iruntime/recomp -Iengine \
  tools/gpu_differ/replay_ours.c runtime/recomp/gpu_native.c engine/native_dl.c \
  -o scratch/bin/replay_ours
echo "[build] scratch/bin/replay_ours"
