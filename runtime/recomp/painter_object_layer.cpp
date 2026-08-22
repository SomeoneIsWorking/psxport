#include "painter_object_layer.h"

#include "render_queue.h"

#include <algorithm>
#include <array>

namespace {

PainterObjectRefusal validateFace(const RqItem &item) {
  if (item.layer != RQ_WORLD) {
    return PainterObjectRefusal::NonWorld;
  }
  if (item.order_mode != RQ_OM_DEPTH) {
    return PainterObjectRefusal::NonDepth;
  }
  if ((item.nv != 3 && item.nv != 4) || item.mode < 0 || item.mode > 3 ||
      (item.semi && (item.tp_blend < 0 || item.tp_blend > 3))) {
    return PainterObjectRefusal::UnsupportedMaterial;
  }
  return PainterObjectRefusal::None;
}

} // namespace

PainterObjectAdmission
RenderQueue::preflightPainterObject(PainterObjectId object, size_t new_faces, PainterObjectLimits limits) const {
  PainterObjectAdmission admission;
  admission.queued_items = consumed ? 0 : (size_t)n;
  auto refuse = [&](PainterObjectAdmissionRefusal why, size_t item = SIZE_MAX) {
    admission.refusal = why;
    admission.refusal_item = item;
    return admission;
  };
  if (!object) {
    return refuse(PainterObjectAdmissionRefusal::InvalidObjectId);
  }
  if (!new_faces) {
    return refuse(PainterObjectAdmissionRefusal::Empty);
  }
  if (mPainterScopeDepth) {
    return refuse(PainterObjectAdmissionRefusal::ActiveScope);
  }
  if (mPainterInvalidId) {
    return refuse(PainterObjectAdmissionRefusal::InvalidLifecycle);
  }
  if (!limits.max_objects || limits.max_objects > 256) {
    return refuse(PainterObjectAdmissionRefusal::TooManyObjects);
  }
  if (!limits.max_faces || new_faces > limits.max_faces) {
    return refuse(PainterObjectAdmissionRefusal::TooManyFaces);
  }
  if (new_faces > (size_t)RQ_MAX - admission.queued_items) {
    return refuse(PainterObjectAdmissionRefusal::QueueCapacity);
  }

  std::array<PainterObjectId, 256> ids{};
  size_t id_count = 0;
  for (size_t i = 0; i < admission.queued_items; ++i) {
    const RqItem &item = items[i];
    if (!item.painter_object) {
      continue;
    }
    if (item.painter_object == object) {
      return refuse(PainterObjectAdmissionRefusal::DuplicateObject, i);
    }
    if (validateFace(item) != PainterObjectRefusal::None) {
      return refuse(PainterObjectAdmissionRefusal::InvalidExistingFace, i);
    }
    ++admission.existing_faces;
    if (std::find(ids.begin(), ids.begin() + (ptrdiff_t)id_count, item.painter_object) ==
        ids.begin() + (ptrdiff_t)id_count) {
      if (id_count == limits.max_objects) {
        return refuse(PainterObjectAdmissionRefusal::TooManyObjects, i);
      }
      ids[id_count++] = item.painter_object;
    }
  }
  admission.existing_objects = id_count;
  if (id_count >= limits.max_objects) {
    return refuse(PainterObjectAdmissionRefusal::TooManyObjects);
  }
  if (new_faces > limits.max_faces - admission.existing_faces) {
    return refuse(PainterObjectAdmissionRefusal::TooManyFaces);
  }
  return admission;
}

PainterObjectPlan RenderQueue::buildPainterObjectPlan(PainterObjectLimits limits) const {
  PainterObjectPlan plan;
  PainterObjectStats &out = plan.stats;
  auto refuse = [&](PainterObjectRefusal why, size_t item = SIZE_MAX) -> PainterObjectPlan {
    out.refusal = why;
    out.refusal_item = item;
    plan.ordinary_items.clear();
    plan.commands.clear();
    plan.objects.clear();
    out.partitioned_items = 0;
    return plan;
  };
  if (mPainterScopeDepth != 0) {
    return refuse(PainterObjectRefusal::ActiveScope);
  }
  if (mPainterInvalidId) {
    return refuse(PainterObjectRefusal::InvalidObjectId);
  }
  if (limits.max_objects == 0 || limits.max_faces == 0 || limits.max_objects > 256) {
    return refuse(limits.max_objects > 256 ? PainterObjectRefusal::TooManyObjects : PainterObjectRefusal::TooManyFaces);
  }
  for (int i = 0; i < n; ++i) {
    const RqItem &item = items[i];
    ++out.items_scanned;
    if (i && (items[i - 1].layer > item.layer || (items[i - 1].layer == item.layer && items[i - 1].seq > item.seq))) {
      return refuse(PainterObjectRefusal::UnsortedQueue, (size_t)i);
    }
    if (!item.painter_object) {
      plan.ordinary_items.push_back((size_t)i);
      continue;
    }
    if (++out.grouped_faces > limits.max_faces) {
      return refuse(PainterObjectRefusal::TooManyFaces, (size_t)i);
    }
    if (const PainterObjectRefusal why = validateFace(item); why != PainterObjectRefusal::None) {
      return refuse(why, (size_t)i);
    }
  }
  if (!out.grouped_faces) {
    return refuse(PainterObjectRefusal::Empty);
  }

  // First-seen object order is deterministic. Material and blend state remain command metadata, never
  // partition keys, so authored textured/untextured and opaque/semitransparent interleaving survives.
  std::vector<PainterObjectId> ids;
  for (int i = 0; i < n; ++i) {
    if (items[i].painter_object && std::find(ids.begin(), ids.end(), items[i].painter_object) == ids.end()) {
      if (ids.size() == limits.max_objects) {
        return refuse(PainterObjectRefusal::TooManyObjects, (size_t)i);
      }
      ids.push_back(items[i].painter_object);
    }
  }
  out.objects = ids.size();
  for (PainterObjectId id : ids) {
    PainterObjectRange range{id, plan.commands.size(), 0};
    std::vector<size_t> members;
    for (int i = 0; i < n; ++i) {
      if (items[i].painter_object == id) {
        members.push_back((size_t)i);
      }
    }
    std::stable_sort(members.begin(), members.end(), [&](size_t left, size_t right) {
      return items[left].seq < items[right].seq;
    });
    for (size_t i : members) {
      const RqItem &item = items[i];
      plan.commands.push_back({i,
                               id,
                               item.seq,
                               item.mode == 3 ? PainterMaterial::Untextured : PainterMaterial::Textured,
                               item.shade_gouraud != 0,
                               item.dither != 0,
                               item.semi != 0,
                               (uint8_t)item.tp_blend});
    }
    range.command_count = members.size();
    plan.objects.push_back(range);
  }
  out.partitioned_items = plan.ordinary_items.size() + plan.commands.size();
  if (out.partitioned_items != (size_t)n) {
    return refuse(PainterObjectRefusal::TooManyFaces);
  }
  return plan;
}
