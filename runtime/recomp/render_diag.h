// class RenderDiag — per-object DIAG SCOPE tracking on one Core's render walk.
//
// The native render walk sets an object scope around each per-object dispatch so downstream code (the
// fps60 billboard recorder, render-queue node tagging, the sil_bbox_log geometry diag) can identify
// which live object emitted a given prim. Two independent scopes:
//
//   currentNode()      = the entity NODE the walk is currently rendering (opened via beginObject(node),
//                        closed via endObject()). 0 outside an object scope — terrain/static/background
//                        prims emit with no per-object identity and are correctly unlabeled.
//   currentGeomblk()   = the GEOMBLK CMD RECORD currently being submitted by Render::gt3gt4 (or the per-
//                        entity cmd tag when the entity list is submitted directly). Used by the
//                        sil_bbox_log diag to attribute a rasterized poly back to its source record.
//
// Per-Core state so SBS / dualcore run two cores without one core's render walk leaking scope into the
// other's diag (was the process-globals g_dbg_render_node / g_dbg_cur_geomblk; deglobalize-game 2026-07-02).
// Reached as `core->rsub.diag`.
#pragma once
#include <stdint.h>

class RenderDiag {
public:
  // Per-object walk scope. beginObject sets the current object's node pointer; endObject clears it.
  // Callers pair these around a per-object dispatch; nesting is not supported (the walk itself doesn't nest).
  void beginObject(uint32_t node) { mNode = node; }
  void endObject()                { mNode = 0; }
  uint32_t currentNode() const    { return mNode; }

  // Geomblk-cmd scope. Set at the top of Render::gt3gt4 (with the geomblk chunk it is submitting) and by
  // the per-entity list walk (with each entity's cmd record). Downstream diag reads it via currentGeomblk().
  void setGeomblk(uint32_t cmd)   { mGeomblk = cmd; }
  uint32_t currentGeomblk() const { return mGeomblk; }

private:
  uint32_t mNode    = 0;
  uint32_t mGeomblk = 0;
};
