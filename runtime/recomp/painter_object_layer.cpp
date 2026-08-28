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

bool painterReplayBefore(const PainterReplayOrder &leftOrder, const PainterReplayOrder &rightOrder) {
  const PainterReplayKey &left = leftOrder.key;
  const PainterReplayKey &right = rightOrder.key;
  if (left.ot_bin != right.ot_bin) {
    return left.ot_bin > right.ot_bin;
  }
  if (left.link_ordinal != right.link_ordinal) {
    return left.link_ordinal > right.link_ordinal;
  }
  return left.chain_suborder < right.chain_suborder;
}

namespace {

bool sameReplayKey(const PainterReplayKey &left, const PainterReplayKey &right) {
  return left.ot_bin == right.ot_bin && left.link_ordinal == right.link_ordinal &&
         left.chain_suborder == right.chain_suborder;
}

} // namespace

PainterObjectAdmission RenderQueue::preflightPainterObject(PainterObjectId object,
                                                           size_t new_faces,
                                                           PainterReplayDomainId replay_domain,
                                                           PainterObjectLimits limits) const {
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
      if (replay_domain && item.layer == RQ_WORLD) {
        return refuse(PainterObjectAdmissionRefusal::UnorderedWorldMix, i);
      }
      continue;
    }
    if (item.painter_object == object) {
      return refuse(PainterObjectAdmissionRefusal::DuplicateObject, i);
    }
    if (validateFace(item) != PainterObjectRefusal::None) {
      return refuse(PainterObjectAdmissionRefusal::InvalidExistingFace, i);
    }
    if (item.painter_replay.authored() != (replay_domain != 0)) {
      return refuse(PainterObjectAdmissionRefusal::MixedReplayPolicy, i);
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

PainterObjectPlan planPainterItemStream(std::span<const RqItem *const> stream, PainterObjectLimits limits) {
  PainterObjectPlan plan;
  PainterObjectStats &out = plan.stats;
  auto refuse = [&](PainterObjectRefusal why, size_t item = SIZE_MAX) -> PainterObjectPlan {
    out.refusal = why;
    out.refusal_item = item;
    plan.ordinary_items.clear();
    plan.ordinary_items_after_ranges.clear();
    plan.commands.clear();
    plan.ranges.clear();
    plan.presentation_ranks.clear();
    out.partitioned_items = 0;
    return plan;
  };
  if (limits.max_objects == 0 || limits.max_faces == 0 || limits.max_objects > 256) {
    return refuse(limits.max_objects > 256 ? PainterObjectRefusal::TooManyObjects : PainterObjectRefusal::TooManyFaces);
  }
  bool hasAuthored = false;
  bool hasIsolated = false;
  size_t firstOrdinaryWorld = SIZE_MAX;
  const uint32_t flushOrdinal = stream.empty() ? 0 : stream.front()->flush_ordinal;
  for (size_t i = 0; i < stream.size(); ++i) {
    const RqItem &item = *stream[i];
    ++out.items_scanned;
    if (item.flush_ordinal != flushOrdinal) {
      return refuse(PainterObjectRefusal::MixedFlushEpoch, i);
    }
    const RqItem *previous = i ? stream[i - 1] : nullptr;
    if (previous && (previous->layer > item.layer || (previous->layer == item.layer && previous->seq > item.seq))) {
      return refuse(PainterObjectRefusal::UnsortedQueue, i);
    }
    if (!item.painter_object) {
      plan.ordinary_items.push_back((size_t)i);
      if (item.layer == RQ_WORLD && firstOrdinaryWorld == SIZE_MAX) {
        firstOrdinaryWorld = i;
      }
      continue;
    }
    if (++out.grouped_faces > limits.max_faces) {
      return refuse(PainterObjectRefusal::TooManyFaces, i);
    }
    if (const PainterObjectRefusal why = validateFace(item); why != PainterObjectRefusal::None) {
      return refuse(why, i);
    }
    hasAuthored |= item.painter_replay.authored();
    hasIsolated |= !item.painter_replay.authored();
  }
  if (!out.grouped_faces) {
    return refuse(PainterObjectRefusal::Empty);
  }
  if (hasAuthored && hasIsolated) {
    return refuse(PainterObjectRefusal::MixedReplayPolicy);
  }
  if (hasAuthored && firstOrdinaryWorld != SIZE_MAX) {
    plan.ordinary_items.clear();
    size_t lastGrouped = 0;
    bool haveGrouped = false;
    for (size_t i = 0; i < stream.size(); ++i) {
      if (stream[i]->painter_object) {
        lastGrouped = i;
        haveGrouped = true;
      }
    }
    for (size_t i = 0; i < stream.size(); ++i) {
      const RqItem &item = *stream[i];
      if (item.painter_object) {
        continue;
      }
      if (item.layer == RQ_WORLD) {
        // An ordinary world primitive cannot participate in a painter replay. It is admissible only
        // when it is already a trailing item; moving an interleaved primitive around the replay
        // would change the guest's paint order, so retain the refusal for that case.
        if (!haveGrouped || i < lastGrouped) {
          return refuse(PainterObjectRefusal::UnorderedWorldMix, i);
        }
        plan.ordinary_items_after_ranges.push_back(i);
      } else {
        plan.ordinary_items.push_back(i);
      }
    }
  }

  std::vector<uint32_t> sequences;
  sequences.reserve(stream.size());
  for (const RqItem *item : stream) {
    sequences.push_back(item->seq);
  }
  std::sort(sequences.begin(), sequences.end());
  sequences.erase(std::unique(sequences.begin(), sequences.end()), sequences.end());
  plan.presentation_ranks.reserve(stream.size());
  for (const RqItem *item : stream) {
    const auto rank = std::lower_bound(sequences.begin(), sequences.end(), item->seq);
    plan.presentation_ranks.push_back((uint32_t)std::distance(sequences.begin(), rank));
  }

  // First-seen object order is deterministic. Material and blend state remain command metadata, never
  // partition keys, so authored textured/untextured and opaque/semitransparent interleaving survives.
  std::vector<PainterObjectId> ids;
  for (size_t i = 0; i < stream.size(); ++i) {
    const RqItem &item = *stream[i];
    if (item.painter_object && std::find(ids.begin(), ids.end(), item.painter_object) == ids.end()) {
      if (ids.size() == limits.max_objects) {
        return refuse(PainterObjectRefusal::TooManyObjects, i);
      }
      ids.push_back(item.painter_object);
    }
  }
  out.objects = ids.size();
  if (hasIsolated) {
    for (PainterObjectId id : ids) {
      PainterPlaybackRange range{PainterPlaybackKind::IsolatedObject, id, plan.commands.size(), 0};
      std::vector<size_t> members;
      for (size_t i = 0; i < stream.size(); ++i) {
        if (stream[i]->painter_object == id) {
          members.push_back(i);
        }
      }
      std::stable_sort(members.begin(), members.end(), [&](size_t left, size_t right) {
        return stream[left]->seq < stream[right]->seq;
      });
      for (size_t i : members) {
        const RqItem &item = *stream[i];
        plan.commands.push_back({i,
                                 id,
                                 item.seq,
                                 item.mode == 3 ? PainterMaterial::Untextured : PainterMaterial::Textured,
                                 item.shade_gouraud != 0,
                                 item.dither != 0,
                                 item.semi != 0,
                                 (uint8_t)item.tp_blend,
                                 item.painter_replay});
      }
      range.command_count = members.size();
      plan.ranges.push_back(range);
    }
  } else {
    std::vector<PainterReplayDomainId> domains;
    std::vector<std::pair<PainterObjectId, PainterReplayDomainId>> objectDomains;
    for (size_t i = 0; i < stream.size(); ++i) {
      const RqItem &item = *stream[i];
      if (!item.painter_object) {
        continue;
      }
      const PainterReplayDomainId domain = item.painter_replay.domain;
      if (std::find(domains.begin(), domains.end(), domain) == domains.end()) {
        domains.push_back(domain);
      }
      const auto object = std::find_if(objectDomains.begin(), objectDomains.end(), [&](const auto &entry) {
        return entry.first == item.painter_object;
      });
      if (object != objectDomains.end() && object->second != domain) {
        return refuse(PainterObjectRefusal::ObjectInMultipleDomains, i);
      }
      if (object == objectDomains.end()) {
        objectDomains.push_back({item.painter_object, domain});
      }
    }
    out.authored_domains = domains.size();
    for (PainterReplayDomainId domain : domains) {
      PainterPlaybackRange range{PainterPlaybackKind::AuthoredDomain, domain, plan.commands.size(), 0};
      std::vector<size_t> members;
      for (size_t i = 0; i < stream.size(); ++i) {
        if (stream[i]->painter_object && stream[i]->painter_replay.domain == domain) {
          members.push_back(i);
        }
      }
      std::stable_sort(members.begin(), members.end(), [&](size_t left, size_t right) {
        return painterReplayBefore(stream[left]->painter_replay, stream[right]->painter_replay);
      });
      for (size_t i = 1; i < members.size(); ++i) {
        if (sameReplayKey(stream[members[i - 1]]->painter_replay.key, stream[members[i]]->painter_replay.key)) {
          return refuse(PainterObjectRefusal::DuplicateReplayKey, members[i]);
        }
      }
      for (size_t i : members) {
        const RqItem &item = *stream[i];
        plan.commands.push_back({i,
                                 item.painter_object,
                                 item.seq,
                                 item.mode == 3 ? PainterMaterial::Untextured : PainterMaterial::Textured,
                                 item.shade_gouraud != 0,
                                 item.dither != 0,
                                 item.semi != 0,
                                 (uint8_t)item.tp_blend,
                                 item.painter_replay});
      }
      range.command_count = members.size();
      plan.ranges.push_back(range);
    }
  }
  out.partitioned_items = plan.ordinary_items.size() + plan.ordinary_items_after_ranges.size() + plan.commands.size();
  if (out.partitioned_items != stream.size()) {
    return refuse(PainterObjectRefusal::TooManyFaces);
  }
  return plan;
}

PainterObjectPlan RenderQueue::buildPainterObjectPlan(PainterObjectLimits limits) const {
  PainterObjectPlan refused;
  if (mPainterScopeDepth != 0) {
    refused.stats.refusal = PainterObjectRefusal::ActiveScope;
    return refused;
  }
  if (mPainterInvalidId) {
    refused.stats.refusal = PainterObjectRefusal::InvalidObjectId;
    return refused;
  }
  std::vector<const RqItem *> stream;
  stream.reserve((size_t)n);
  for (int i = 0; i < n; ++i) {
    stream.push_back(&items[i]);
  }
  return planPainterItemStream(stream, limits);
}
