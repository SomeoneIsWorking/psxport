#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Explicit reset-derived CPU window, not an exact console-state import. Strict register file plus
// complete RAM/scratch images; rejects pending execution/device state instead of discarding it.
int oracle_boundary_import(const char *registers_path, const char *ram_path, const char *scratch_path);

#ifdef __cplusplus
}
#endif
