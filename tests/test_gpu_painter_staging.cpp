#include "testutil.h"
#include "gpu_vk_internal.h"

static void draw(GpuVkState& g, int mode, int u0) {
  int x[3]={0,8,0}, y[3]={0,0,8}, u[3]={u0,u0+1,u0}, v[3]={0,0,1};
  unsigned char c[3]={128,128,128}; float d[3]={.2f,.4f,.6f}; g.s_vd=d;
  g.draw_tritri(x,y,u,v,c,c,c,64,0,mode,0,16,0,0,0,0,0,0,0,1023,511);
}

static void test_separate_ordered_object_ranges(void) {
  GpuVkState g;
  CHECK(g.painter_begin(10)); draw(g,0,1); draw(g,2,7); CHECK(g.painter_end());
  CHECK(g.painter_begin(20)); draw(g,1,13); CHECK(g.painter_end());
  int ordinary=-1,painter=-1,ranges=-1; g.painter_staging_stats(&ordinary,&painter,&ranges);
  CHECK_EQ(ordinary,0); CHECK_EQ(painter,9); CHECK_EQ(ranges,2);
  CHECK_EQ(g.s_painter_object[0],10); CHECK_EQ(g.s_painter_first[0],0); CHECK_EQ(g.s_painter_count[0],6);
  CHECK_EQ(g.s_painter_object[1],20); CHECK_EQ(g.s_painter_first[1],6); CHECK_EQ(g.s_painter_count[1],3);
}

static void test_ordinary_stays_ordinary(void) {
  GpuVkState g; draw(g,0,1);
  int ordinary=-1,painter=-1,ranges=-1; g.painter_staging_stats(&ordinary,&painter,&ranges);
  CHECK_EQ(ordinary,3); CHECK_EQ(painter,0); CHECK_EQ(ranges,0);
  CHECK(!g.painter_begin(0)); CHECK(!g.painter_end());
}

int main(void){ RUN(separate_ordered_object_ranges); RUN(ordinary_stays_ordinary); return pt_summary(); }
