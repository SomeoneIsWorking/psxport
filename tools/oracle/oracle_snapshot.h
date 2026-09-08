#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Versioned diagnostic snapshots contain only the isolated oracle's modeled CPU/IRQ/DPCR state.
// They are not console savestates: CD, GPU, SPU, SIO, timers and DMA channels are absent.
// Restore validates every field before changing any live state. Unsupported-device taint survives.
int oracle_snapshot_save(const char *path);
int oracle_snapshot_load(const char *path);

// The shim owns its clock and sticky stop/device-provenance state. This visitor exposes those
// actual fields to the snapshot owner rather than maintaining a second shadow representation.
typedef void (*OracleSnapshotFieldVisitor)(
    void *context, const char *name, void *data, size_t bytes, unsigned element_bytes);
int oracle_snapshot_visit_shim(OracleSnapshotFieldVisitor visitor, void *context, int restoring);

#ifdef __cplusplus
}
#endif
