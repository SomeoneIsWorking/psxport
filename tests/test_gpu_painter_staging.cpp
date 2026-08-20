#include "testutil.h"
#include "gpu_vk_internal.h"

static void draw(GpuVkState& g, int mode, int u0) {
  int x[3]={0,8,0}, y[3]={0,0,8}, u[3]={u0,u0+1,u0}, v[3]={0,0,1};
  unsigned char c[3]={128,128,128}; float d[3]={.2f,.4f,.6f}; g.s_vd=d;
  g.draw_tritri(x,y,u,v,c,c,c,64,0,mode,0,16,0,0,0,0,0,0,0,1023,511);
}
static void draw_untextured(GpuVkState& g, bool gouraud, bool dither) {
  float d[3]={.2f,.4f,.6f}; g.s_vd=d; g.s_painter_item_gouraud=gouraud; g.s_painter_item_dither=dither;
  // Full-canvas clip: psxport 7782da9c gave draw_tri a guest draw-area rect (dax0,day0,dax1,day1)
  // and did not update this call, which stopped the file compiling — and with it the ENTIRE suite,
  // since one unbuildable test aborts the build and leaves every other test "Not Run". This test is
  // about painter STAGING, not clipping, so the clip must not be able to reject the triangle.
  g.draw_tri(0,0,10,20,30, 8,0,40,50,60, 0,8,70,80,90, 0,0,1023,511);
}

static void test_separate_ordered_object_ranges(void) {
  GpuVkState g;
  CHECK(g.painter_begin(10)); draw(g,0,1); draw_untextured(g,true,true); draw(g,2,7); CHECK(g.painter_end());
  CHECK(g.painter_begin(20)); draw(g,1,13); CHECK(g.painter_end());
  int ordinary=-1,painter=-1,ranges=-1; g.painter_staging_stats(&ordinary,&painter,&ranges);
  CHECK_EQ(ordinary,0); CHECK_EQ(painter,12); CHECK_EQ(ranges,2);
  CHECK_EQ(g.s_painter_object[0],10); CHECK_EQ(g.s_painter_first[0],0); CHECK_EQ(g.s_painter_count[0],3);
  CHECK_EQ(g.s_painter_cmd_material[0],1); CHECK_EQ(g.s_painter_cmd_material[1],0); CHECK_EQ(g.s_painter_cmd_material[2],1);
  CHECK_EQ(g.s_painter_cmd_gouraud[1],1); CHECK_EQ(g.s_painter_cmd_dither[1],1);
  CHECK_EQ(g.s_painter_object[1],20); CHECK_EQ(g.s_painter_first[1],3); CHECK_EQ(g.s_painter_count[1],1);
}

static void test_ordinary_stays_ordinary(void) {
  GpuVkState g; draw(g,0,1);
  int ordinary=-1,painter=-1,ranges=-1; g.painter_staging_stats(&ordinary,&painter,&ranges);
  CHECK_EQ(ordinary,3); CHECK_EQ(painter,0); CHECK_EQ(ranges,0);
  CHECK(!g.painter_begin(0)); CHECK(!g.painter_end());
}

int main(void){ RUN(separate_ordered_object_ranges); RUN(ordinary_stays_ordinary); return pt_summary(); }
