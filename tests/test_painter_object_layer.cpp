#include "render_queue.h"

#include "mods.h" // FACE_ORDER_DEPTH — the ordering mode the rule is driven with
#include "testutil.h"
#include <array>
#include <memory>

static std::unique_ptr<RenderQueue> make_queue(void) {
  auto q = std::make_unique<RenderQueue>();
  q->n = 0;
  q->seq = 0;
  q->consumed = 0;
  return q;
}
static void add(RenderQueue &q,
                PainterObjectId id,
                uint32_t seq,
                int mode,
                int semi = 0,
                uint8_t flags = 0,
                int gouraud = 0,
                int dither = 0,
                PainterReplayOrder replay = {}) {
  RqItem &it = q.items[q.n++];
  it = RqItem{};
  it.painter_object = id;
  it.painter_flags = flags;
  it.seq = seq;
  it.draw_seq = seq; // what push() does; these fixtures build items directly
  it.layer = RQ_WORLD;
  it.order_mode = RQ_OM_DEPTH;
  it.nv = 3;
  it.mode = mode;
  it.semi = semi;
  it.shade_gouraud = (uint8_t)gouraud;
  it.dither = (uint8_t)dither;
  it.painter_replay = replay;
}

static PainterReplayOrder replay(uint32_t domain, uint16_t bin, uint32_t link, uint32_t suborder) {
  return {.domain = domain, .key = {.ot_bin = bin, .link_ordinal = link, .chain_suborder = suborder}};
}

static void test_interleaved_materials_and_ties(void) {
  auto q = make_queue();
  add(*q, 0, 4, 3);
  add(*q, 7, 30, 0);
  q->items[1].tp_x = 64;
  q->items[1].clut_x = 16;
  add(*q, 7, 10, 3);
  add(*q, 7, 20, 1);
  q->items[3].tp_x = 128;
  q->items[3].clut_x = 32;
  add(*q, 0, 5, 0);
  q->sortQueue();
  PainterObjectPlan p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK_EQ(p.commands.size(), 3);
  CHECK_EQ((int)p.commands[0].material, (int)PainterMaterial::Untextured);
  q = make_queue();
  add(*q, 7, 30, 0);
  add(*q, 7, 10, 2);
  add(*q, 7, 20, 1);
  q->sortQueue();
  p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK_EQ(p.commands.size(), 3);
  CHECK_EQ(p.commands[0].seq, 10);
  CHECK_EQ(p.commands[1].seq, 20);
  CHECK_EQ(p.commands[2].seq, 30);
  q = make_queue();
  add(*q, 9, 5, 0);
  add(*q, 9, 5, 1);
  p = q->buildPainterObjectPlan();
  CHECK_EQ(p.commands[0].item_index, 0);
  CHECK_EQ(p.commands[1].item_index, 1);
}

static void test_multiple_objects_are_contiguous(void) {
  auto q = make_queue();
  add(*q, 2, 30, 0);
  add(*q, 0, 15, 3);
  add(*q, 1, 20, 2);
  add(*q, 2, 10, 1);
  add(*q, 0, 25, 0);
  add(*q, 1, 40, 0);
  q->sortQueue();
  PainterObjectPlan p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK_EQ(p.stats.items_scanned, 6);
  CHECK_EQ(p.stats.grouped_faces, 4);
  CHECK_EQ(p.stats.objects, 2);
  CHECK_EQ(p.stats.partitioned_items, 6);
  CHECK_EQ((int)p.ranges[0].kind, (int)PainterPlaybackKind::IsolatedObject);
  CHECK_EQ(p.ranges[0].identity, 2);
  CHECK_EQ(p.ranges[0].command_count, 2);
  CHECK_EQ(p.commands[0].seq, 10);
  CHECK_EQ(p.commands[1].seq, 30);
  CHECK_EQ(p.ranges[1].identity, 1);
  CHECK_EQ(p.commands[2].seq, 20);
  CHECK_EQ(p.commands[3].seq, 40);
}

static void test_authored_domain_merges_objects_in_guest_replay_order(void) {
  auto q = make_queue();
  add(*q, 10, 0, 0, 0, 0, 0, 0, replay(77, 8, 1, 0));
  add(*q, 20, 1, 1, 1, 0, 0, 0, replay(77, 9, 0, 0));
  q->items[1].tp_blend = 2;
  add(*q, 10, 2, 3, 0, 0, 0, 0, replay(77, 8, 2, 1));
  add(*q, 30, 3, 2, 0, 0, 0, 0, replay(77, 8, 2, 0));
  PainterObjectPlan p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK_EQ(p.stats.objects, 3);
  CHECK_EQ(p.stats.authored_domains, 1);
  CHECK_EQ(p.ranges.size(), 1);
  CHECK_EQ((int)p.ranges[0].kind, (int)PainterPlaybackKind::AuthoredDomain);
  CHECK_EQ(p.ranges[0].identity, 77);
  CHECK_EQ(p.ranges[0].command_count, 4);
  CHECK_EQ(p.commands[0].object, 20);
  CHECK(p.commands[0].semi_transparent);
  CHECK_EQ(p.commands[0].blend_mode, 2);
  CHECK_EQ(p.commands[1].object, 30);
  CHECK_EQ(p.commands[2].object, 10);
  CHECK_EQ(p.commands[3].object, 10);
  CHECK_EQ(p.commands[1].replay.key.chain_suborder, 0);
  CHECK_EQ(p.commands[2].replay.key.chain_suborder, 1);
}

static void test_presentation_stream_plans_across_captured_queues(void) {
  auto captured = make_queue();
  auto rerendered = make_queue();
  add(*captured, 10, 0, 0, 0, 0, 0, 0, replay(77, 8, 1, 0));
  add(*rerendered, 20, 1, 0, 0, 0, 0, 0, replay(77, 9, 0, 0));
  add(*captured, 30, 2, 0, 0, 0, 0, 0, replay(77, 8, 2, 0));
  captured->items[0].seq = 2650;
  rerendered->items[0].seq = 2652;
  captured->items[1].seq = 2654;
  const std::array<const RqItem *, 3> present = {&captured->items[0], &rerendered->items[0], &captured->items[1]};
  const PainterObjectPlan plan = planPainterItemStream(present);
  CHECK(plan.accepted());
  CHECK_EQ(plan.ranges.size(), 1);
  CHECK_EQ(plan.ranges[0].command_count, 3);
  CHECK_EQ(plan.commands[0].object, 20);
  CHECK_EQ(plan.commands[1].object, 30);
  CHECK_EQ(plan.commands[2].object, 10);
  CHECK_EQ(plan.commands[0].item_index, 1);
  CHECK_EQ(plan.presentation_ranks[0], 0);
  CHECK_EQ(plan.presentation_ranks[1], 1);
  CHECK_EQ(plan.presentation_ranks[2], 2);

  rerendered->items[0].flush_ordinal = 1;
  const PainterObjectPlan mixedFlush = planPainterItemStream(present);
  CHECK_EQ((int)mixedFlush.stats.refusal, (int)PainterObjectRefusal::MixedFlushEpoch);
  CHECK_EQ(mixedFlush.stats.refusal_item, 1);
}

static void test_authored_domain_refuses_incomplete_or_ambiguous_order(void) {
  auto q = make_queue();
  add(*q, 1, 0, 0, 0, 0, 0, 0, replay(7, 9, 1, 0));
  add(*q, 2, 1, 0, 0, 0, 0, 0, replay(7, 9, 1, 0));
  PainterObjectPlan p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::DuplicateReplayKey);

  q = make_queue();
  add(*q, 0, 0, 0);
  add(*q, 1, 1, 0, 0, 0, 0, 0, replay(7, 9, 1, 0));
  p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::UnorderedWorldMix);
  CHECK_EQ(p.stats.refusal_item, 0);

  q = make_queue();
  add(*q, 1, 0, 0, 0, 0, 0, 0, replay(7, 9, 1, 0));
  add(*q, 0, 1, 0);
  q->items[1].nv = 2;
  p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK_EQ(p.ordinary_items_after_ranges.size(), 1);
  CHECK_EQ(p.ordinary_items_after_ranges[0], 1);

  q = make_queue();
  add(*q, 1, 0, 0, 0, 0, 0, 0, replay(7, 9, 1, 0));
  add(*q, 0, 1, 3);
  q->items[1].nv = 4;
  p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK_EQ(p.ordinary_items_after_ranges.size(), 1);
  CHECK_EQ(p.ordinary_items_after_ranges[0], 1);

  q = make_queue();
  add(*q, 1, 0, 0, 0, 0, 0, 0, replay(7, 9, 1, 0));
  add(*q, 2, 1, 0);
  p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::MixedReplayPolicy);

  q = make_queue();
  add(*q, 1, 0, 0, 0, 0, 0, 0, replay(7, 9, 1, 0));
  add(*q, 1, 1, 0, 0, 0, 0, 0, replay(8, 9, 2, 0));
  p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::ObjectInMultipleDomains);
}

static void test_authored_admission_refuses_unknown_world_mix(void) {
  auto q = make_queue();
  add(*q, 0, 0, 3);
  PainterObjectAdmission admission = q->preflightPainterObject(8, 1, 77);
  CHECK_EQ((int)admission.refusal, (int)PainterObjectAdmissionRefusal::UnorderedWorldMix);
  q = make_queue();
  add(*q, 4, 0, 3);
  admission = q->preflightPainterObject(8, 1, 77);
  CHECK_EQ((int)admission.refusal, (int)PainterObjectAdmissionRefusal::MixedReplayPolicy);
}

static void test_atomic_admission_accepts_existing_semitransparency(void) {
  auto q = make_queue();
  add(*q, 4, 0, 1, 1);
  q->items[0].tp_blend = 3;
  add(*q, 0, 1, 3);
  const PainterObjectAdmission accepted = q->preflightPainterObject(8, 2);
  CHECK(accepted.accepted());
  CHECK_EQ(accepted.queued_items, 2);
  CHECK_EQ(accepted.existing_objects, 1);
  CHECK_EQ(accepted.existing_faces, 1);

  PainterObjectAdmission refused = q->preflightPainterObject(4, 1);
  CHECK_EQ((int)refused.refusal, (int)PainterObjectAdmissionRefusal::DuplicateObject);
  CHECK_EQ(refused.refusal_item, 0);

  q->items[0].tp_blend = 4;
  refused = q->preflightPainterObject(8, 1);
  CHECK_EQ((int)refused.refusal, (int)PainterObjectAdmissionRefusal::InvalidExistingFace);
  CHECK_EQ(refused.refusal_item, 0);
}

static void test_batch_admission_is_atomic(void) {
  auto q = make_queue();
  add(*q, 4, 0, 1, 1);
  q->items[0].tp_blend = 3;
  const std::array<PainterObjectBatchEntry, 2> entries = {{{8, 2, 0}, {9, 3, 0}}};
  const int queued_before = q->n;
  const uint32_t first_seq_before = q->items[0].seq;
  const PainterObjectAdmission accepted = q->preflightPainterObjectBatch(entries);
  CHECK(accepted.accepted());
  CHECK_EQ(accepted.queued_items, 1);
  CHECK_EQ(accepted.existing_objects, 1);
  CHECK_EQ(accepted.existing_faces, 1);
  CHECK_EQ(q->n, queued_before);
  CHECK_EQ(q->items[0].seq, first_seq_before);

  const std::array<PainterObjectBatchEntry, 2> duplicate = {{{8, 1, 0}, {8, 1, 0}}};
  const PainterObjectAdmission refused = q->preflightPainterObjectBatch(duplicate);
  CHECK_EQ((int)refused.refusal, (int)PainterObjectAdmissionRefusal::DuplicateObject);
  CHECK_EQ(refused.refusal_item, 1);
  CHECK_EQ(q->n, queued_before);
  CHECK_EQ(q->items[0].seq, first_seq_before);
}

static void test_batch_admission_preserves_refusal_semantics(void) {
  auto q = make_queue();
  add(*q, 0, 0, 3);
  const std::array<PainterObjectBatchEntry, 2> authored = {{{8, 1, 77}, {9, 1, 77}}};
  PainterObjectAdmission refused = q->preflightPainterObjectBatch(authored);
  CHECK_EQ((int)refused.refusal, (int)PainterObjectAdmissionRefusal::UnorderedWorldMix);
  CHECK_EQ(refused.refusal_item, 0);

  q = make_queue();
  add(*q, 4, 0, 3);
  q->items[0].painter_replay = replay(77, 1, 0, 0);
  const std::array<PainterObjectBatchEntry, 2> isolated = {{{8, 1, 0}, {9, 1, 0}}};
  refused = q->preflightPainterObjectBatch(isolated);
  CHECK_EQ((int)refused.refusal, (int)PainterObjectAdmissionRefusal::MixedReplayPolicy);
  CHECK_EQ(refused.refusal_item, 0);

  q = make_queue();
  const std::array<PainterObjectBatchEntry, 2> too_many = {{{8, 1, 0}, {9, 1, 0}}};
  PainterObjectLimits limits;
  limits.max_objects = 1;
  refused = q->preflightPainterObjectBatch(too_many, limits);
  CHECK_EQ((int)refused.refusal, (int)PainterObjectAdmissionRefusal::TooManyObjects);
  CHECK_EQ(refused.refusal_item, 1);
}

static void test_refusals_and_denominators(void) {
  auto q = make_queue();
  PainterObjectPlan p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::Empty);
  CHECK_EQ(p.stats.items_scanned, 0);
  add(*q, 1, 0, 0, 1);
  q->items[0].tp_blend = 2;
  p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK_EQ(p.stats.grouped_faces, 1);
  CHECK(p.commands[0].semi_transparent);
  CHECK_EQ(p.commands[0].blend_mode, 2);
  q = make_queue();
  add(*q, 1, 0, 3, 0, PAINTER_OBJECT_DITHER, 1, 1);
  p = q->buildPainterObjectPlan();
  CHECK(p.accepted());
  CHECK(p.commands[0].shade_gouraud);
  CHECK(p.commands[0].dither);
  q = make_queue();
  add(*q, 1, 0, 4);
  p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::UnsupportedMaterial);
  CHECK_EQ(p.stats.items_scanned, 1);
  q = make_queue();
  for (uint32_t i = 0; i < 557; i++) {
    add(*q, 1, i, 3);
  }
  PainterObjectLimits measured;
  measured.max_faces = 557;
  p = q->buildPainterObjectPlan(measured);
  CHECK(p.accepted());
  add(*q, 1, 557, 3);
  p = q->buildPainterObjectPlan(measured);
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::TooManyFaces);
  CHECK_EQ(p.stats.items_scanned, 558);
  q = make_queue();
  add(*q, 1, 0, 0);
  add(*q, 1, 1, 0);
  PainterObjectLimits lf;
  lf.max_faces = 1;
  p = q->buildPainterObjectPlan(lf);
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::TooManyFaces);
  CHECK_EQ(p.stats.items_scanned, 2);
  q = make_queue();
  add(*q, 1, 0, 0);
  add(*q, 2, 1, 0);
  PainterObjectLimits lo;
  lo.max_objects = 1;
  p = q->buildPainterObjectPlan(lo);
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::TooManyObjects);
  CHECK_EQ(p.stats.grouped_faces, 2);
  q = make_queue();
  {
    RenderQueue::PainterObjectScope active(*q, 8);
    p = q->buildPainterObjectPlan();
    CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::ActiveScope);
    CHECK_EQ(p.stats.items_scanned, 0);
  }
  q->reset();
  CHECK_EQ(q->mPainterObject, 0);
  CHECK_EQ(q->n, 0);
  {
    RenderQueue::PainterObjectScope invalid(*q, 0);
  }
  p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::InvalidObjectId);
  CHECK_EQ(p.commands.size(), 0);
  q = make_queue();
  add(*q, 1, 2, 0);
  add(*q, 1, 1, 0);
  p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::UnsortedQueue);
  CHECK_EQ(p.stats.items_scanned, 2);
  CHECK_EQ(p.commands.size(), 0);
}

static void test_painter_depth_is_not_key_flattened(void) {
  auto q = make_queue();
  add(*q, 3, 0, 0);
  add(*q, 3, 1, 0);
  add(*q, 0, 2, 0);
  add(*q, 0, 3, 0);
  for (int i = 0; i < 4; i++) {
    q->items[i].dbg_node = 0x80001000;
    q->items[i].sort_key = i & 1;
    // Shipping key_to_ord is strictly decreasing with the bucket key. Keep this fixture valid so
    // the key-order resolver can distinguish a production invariant failure from the painter test.
    q->items[i].key_ord = .223f - .100f * (float)(i & 1);
    q->items[i].has_xyf = 1;
    q->items[i].nv = 4;
    for (int k = 0; k < 4; k++) {
      q->items[i].xsf[k] = (k == 0 || k == 3) ? 0.f : 10.f;
      q->items[i].ysf[k] = (k < 2) ? 0.f : 10.f;
      q->items[i].depth[k] = (i & 1) ? .9f : .1f;
    }
  }
  float p0 = q->items[0].depth[0], p1 = q->items[1].depth[0];
  q->resolveKeyOrderFaces(0, "test", FACE_ORDER_DEPTH);
  CHECK(q->items[0].depth[0] == p0);
  CHECK(q->items[1].depth[0] == p1);
  CHECK(q->items[2].depth[0] == q->items[2].key_ord || q->items[3].depth[0] == q->items[3].key_ord);
}

static void test_lazy_reset_preserves_scopes(void) {
  auto q = make_queue();
  q->consumed = 1;
  {
    RenderQueue::PainterObjectScope outer(*q, 11);
    RqItem *a = q->push();
    CHECK_EQ(a->painter_object, 11);
    CHECK_EQ(q->mPainterScopeDepth, 1);
    {
      RenderQueue::PainterObjectScope inner(*q, 22);
      RqItem *b = q->push();
      CHECK_EQ(b->painter_object, 22);
      CHECK_EQ(q->mPainterScopeDepth, 2);
    }
    RqItem *c = q->push();
    CHECK_EQ(c->painter_object, 11);
    CHECK_EQ(q->mPainterScopeDepth, 1);
  }
  CHECK_EQ(q->mPainterScopeDepth, 0);
  CHECK_EQ(q->n, 3);
}

int main(void) {
  RUN(interleaved_materials_and_ties);
  RUN(multiple_objects_are_contiguous);
  RUN(authored_domain_merges_objects_in_guest_replay_order);
  RUN(presentation_stream_plans_across_captured_queues);
  RUN(authored_domain_refuses_incomplete_or_ambiguous_order);
  RUN(authored_admission_refuses_unknown_world_mix);
  RUN(atomic_admission_accepts_existing_semitransparency);
  RUN(batch_admission_is_atomic);
  RUN(batch_admission_preserves_refusal_semantics);
  RUN(refusals_and_denominators);
  RUN(painter_depth_is_not_key_flattened);
  RUN(lazy_reset_preserves_scopes);
  return pt_summary();
}
