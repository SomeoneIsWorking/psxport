#pragma once

#include <stddef.h>
#include <vector>

struct RqItem;

// Encode PSX AddPrim head-insertion order into raster-distinct depths for the selected keyed faces.
// Returns the number of selected faces that belonged to a multi-face OT bucket.
size_t rq_apply_ot_lifo_depths(RqItem *items, std::vector<int> selected);
