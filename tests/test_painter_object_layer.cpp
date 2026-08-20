#include "render_queue.h"

#include "mods.h" // FACE_ORDER_DEPTH — the ordering mode the rule is driven with
#include "testutil.h"
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
                int dither = 0) {
  RqItem &it = q.items[q.n++];
  it = RqItem{};
  it.painter_object = id;
  it.painter_flags = flags;
  it.seq = seq;
  it.layer = RQ_WORLD;
  it.order_mode = RQ_OM_DEPTH;
  it.nv = 3;
  it.mode = mode;
  it.semi = semi;
  it.shade_gouraud = (uint8_t)gouraud;
  it.dither = (uint8_t)dither;
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
  CHECK_EQ(p.objects[0].object, 2);
  CHECK_EQ(p.objects[0].command_count, 2);
  CHECK_EQ(p.commands[0].seq, 10);
  CHECK_EQ(p.commands[1].seq, 30);
  CHECK_EQ(p.objects[1].object, 1);
  CHECK_EQ(p.commands[2].seq, 20);
  CHECK_EQ(p.commands[3].seq, 40);
}

static void test_refusals_and_denominators(void) {
  auto q = make_queue();
  PainterObjectPlan p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::Empty);
  CHECK_EQ(p.stats.items_scanned, 0);
  add(*q, 1, 0, 0, 1);
  p = q->buildPainterObjectPlan();
  CHECK_EQ((int)p.stats.refusal, (int)PainterObjectRefusal::SemiTransparent);
  CHECK_EQ(p.stats.grouped_faces, 1);
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
  RUN(refusals_and_denominators);
  RUN(painter_depth_is_not_key_flattened);
  RUN(lazy_reset_preserves_scopes);
  return pt_summary();
}
