// RmlUi render interface on SDL_GPU — see rmlui_render_gpu.h. Mirrors the former Vulkan backend's model
// (one-shot, fence-waited geometry/texture uploads + per-draw vertex UBO + indexed draw) on the SDL_GPU
// API, recording into the port's present render pass.
#include "rmlui_render_gpu.h"
#include <RmlUi/Core/Vertex.h>
#include <cstring>
#include <cstdio>

#include "gpu_vk_shaders.h"   // generated SPIR-V: spv_g_rml_vert / spv_g_rml_frag
#include "cfg.h"

namespace {
struct RmlUbo { float uTranslate[2]; float uViewport[2]; };

// One-shot copy submit, fence-waited: the uploaded resource is ready before any later present pass reads
// it (RmlUi compiles geometry/textures rarely — only when content changes — so the wait is not hot).
void OneShotWait(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd) {
  SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (f) { SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); }
}
} // namespace

SDL_GPUBuffer* RmlRenderInterfaceGpu::UploadBuffer(const void* data, uint32_t size, SDL_GPUBufferUsageFlags usage) {
  if (!size) return nullptr;
  SDL_GPUBufferCreateInfo bi = {}; bi.usage = usage; bi.size = size;
  SDL_GPUBuffer* buf = SDL_CreateGPUBuffer(mDev, &bi);
  if (!buf) return nullptr;
  SDL_GPUTransferBufferCreateInfo ti = {}; ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size = size;
  SDL_GPUTransferBuffer* xfer = SDL_CreateGPUTransferBuffer(mDev, &ti);
  void* p = SDL_MapGPUTransferBuffer(mDev, xfer, false);
  memcpy(p, data, size);
  SDL_UnmapGPUTransferBuffer(mDev, xfer);
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(mDev);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTransferBufferLocation src = {}; src.transfer_buffer = xfer; src.offset = 0;
  SDL_GPUBufferRegion dst = {}; dst.buffer = buf; dst.offset = 0; dst.size = size;
  SDL_UploadToGPUBuffer(cp, &src, &dst, false);
  SDL_EndGPUCopyPass(cp);
  OneShotWait(mDev, cmd);
  SDL_ReleaseGPUTransferBuffer(mDev, xfer);
  return buf;
}

SDL_GPUTexture* RmlRenderInterfaceGpu::UploadTexture(const void* rgba, int w, int h) {
  if (w < 1) w = 1; if (h < 1) h = 1;
  SDL_GPUTextureCreateInfo ti = {};
  ti.type = SDL_GPU_TEXTURETYPE_2D; ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width = (Uint32)w; ti.height = (Uint32)h;
  ti.layer_count_or_depth = 1; ti.num_levels = 1;
  SDL_GPUTexture* tex = SDL_CreateGPUTexture(mDev, &ti);
  if (!tex) return nullptr;
  const Uint32 size = (Uint32)w * h * 4;
  SDL_GPUTransferBufferCreateInfo bi = {}; bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; bi.size = size;
  SDL_GPUTransferBuffer* xfer = SDL_CreateGPUTransferBuffer(mDev, &bi);
  void* p = SDL_MapGPUTransferBuffer(mDev, xfer, false);
  if (rgba) memcpy(p, rgba, size); else memset(p, 0xFF, size);
  SDL_UnmapGPUTransferBuffer(mDev, xfer);
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(mDev);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src = {}; src.transfer_buffer = xfer; src.pixels_per_row = (Uint32)w; src.rows_per_layer = (Uint32)h;
  SDL_GPUTextureRegion dst = {}; dst.texture = tex; dst.w = (Uint32)w; dst.h = (Uint32)h; dst.d = 1;
  SDL_UploadToGPUTexture(cp, &src, &dst, false);
  SDL_EndGPUCopyPass(cp);
  OneShotWait(mDev, cmd);
  SDL_ReleaseGPUTransferBuffer(mDev, xfer);
  return tex;
}

bool RmlRenderInterfaceGpu::Init(SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt) {
  mDev = dev; mSwapFmt = swap_fmt;
  SDL_GPUShaderCreateInfo vci = {};
  vci.code = (const Uint8*)spv_g_rml_vert; vci.code_size = spv_g_rml_vert_len; vci.entrypoint = "main";
  vci.format = SDL_GPU_SHADERFORMAT_SPIRV; vci.stage = SDL_GPU_SHADERSTAGE_VERTEX; vci.num_uniform_buffers = 1;
  SDL_GPUShader* vs = SDL_CreateGPUShader(mDev, &vci);
  SDL_GPUShaderCreateInfo fci = {};
  fci.code = (const Uint8*)spv_g_rml_frag; fci.code_size = spv_g_rml_frag_len; fci.entrypoint = "main";
  fci.format = SDL_GPU_SHADERFORMAT_SPIRV; fci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; fci.num_samplers = 1;
  SDL_GPUShader* fs = SDL_CreateGPUShader(mDev, &fci);
  if (!vs || !fs) { cfg_loge("rmlgpu", "shader create failed: %s", SDL_GetError()); return false; }

  SDL_GPUVertexBufferDescription vbd = {}; vbd.slot = 0; vbd.pitch = (Uint32)sizeof(Rml::Vertex);
  vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  SDL_GPUVertexAttribute attrs[3] = {};
  attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;     attrs[0].offset = (Uint32)offsetof(Rml::Vertex, position);
  attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM; attrs[1].offset = (Uint32)offsetof(Rml::Vertex, colour);
  attrs[2].location = 2; attrs[2].buffer_slot = 0; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;     attrs[2].offset = (Uint32)offsetof(Rml::Vertex, tex_coord);

  SDL_GPUColorTargetDescription ct = {}; ct.format = mSwapFmt;
  ct.blend_state.enable_blend = true;
  ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

  SDL_GPUGraphicsPipelineCreateInfo gp = {};
  gp.vertex_shader = vs; gp.fragment_shader = fs;
  gp.vertex_input_state.vertex_buffer_descriptions = &vbd; gp.vertex_input_state.num_vertex_buffers = 1;
  gp.vertex_input_state.vertex_attributes = attrs; gp.vertex_input_state.num_vertex_attributes = 3;
  gp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  gp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  gp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  gp.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  gp.target_info.color_target_descriptions = &ct; gp.target_info.num_color_targets = 1;
  mPipe = SDL_CreateGPUGraphicsPipeline(mDev, &gp);
  SDL_ReleaseGPUShader(mDev, vs); SDL_ReleaseGPUShader(mDev, fs);
  if (!mPipe) { cfg_loge("rmlgpu", "pipeline create failed: %s", SDL_GetError()); return false; }

  SDL_GPUSamplerCreateInfo si = {};
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR; si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  mSampler = SDL_CreateGPUSampler(mDev, &si);
  if (!mSampler) { cfg_loge("rmlgpu", "sampler create failed: %s", SDL_GetError()); return false; }

  const unsigned char white[4] = { 255, 255, 255, 255 };
  mWhiteTex = UploadTexture(white, 1, 1);
  mReady = (mWhiteTex != nullptr);
  if (mReady) cfg_logi("rmlgpu", "render interface ready (swap fmt %d)", (int)mSwapFmt);
  return mReady;
}

void RmlRenderInterfaceGpu::BeginFrame(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h) {
  mCmd = cmd; mRp = rp; mWinW = win_w; mWinH = win_h;
  mScissorEnabled = false; mScissor = { 0, 0, win_w, win_h };
  mActive = mReady;
}

Rml::CompiledGeometryHandle RmlRenderInterfaceGpu::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                                   Rml::Span<const int> indices) {
  if (!mReady || vertices.empty() || indices.empty()) return 0;
  Geometry g;
  g.vbo = UploadBuffer(vertices.data(), (uint32_t)(vertices.size() * sizeof(Rml::Vertex)), SDL_GPU_BUFFERUSAGE_VERTEX);
  g.ibo = UploadBuffer(indices.data(), (uint32_t)(indices.size() * sizeof(int)), SDL_GPU_BUFFERUSAGE_INDEX);
  g.indexCount = (uint32_t)indices.size();
  if (!g.vbo || !g.ibo) { if (g.vbo) SDL_ReleaseGPUBuffer(mDev, g.vbo); if (g.ibo) SDL_ReleaseGPUBuffer(mDev, g.ibo); return 0; }
  Rml::CompiledGeometryHandle h = mNextGeometry++;
  mGeometries[h] = g;
  return h;
}

void RmlRenderInterfaceGpu::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                           Rml::TextureHandle texture) {
  if (!mActive || !mRp) return;
  auto it = mGeometries.find(geometry);
  if (it == mGeometries.end()) return;
  const Geometry& g = it->second;
  if (!g.indexCount) return;

  RmlUbo ubo;
  ubo.uTranslate[0] = translation.x; ubo.uTranslate[1] = translation.y;
  ubo.uViewport[0] = (float)mWinW; ubo.uViewport[1] = (float)mWinH;
  SDL_PushGPUVertexUniformData(mCmd, 0, &ubo, sizeof ubo);

  SDL_GPUViewport vp = { 0.0f, 0.0f, (float)mWinW, (float)mWinH, 0.0f, 1.0f };
  SDL_Rect sc = mScissorEnabled ? mScissor : (SDL_Rect){ 0, 0, mWinW, mWinH };
  if (sc.w < 0) sc.w = 0; if (sc.h < 0) sc.h = 0;

  SDL_GPUTexture* view = mWhiteTex;
  if (texture) { auto t = mTextures.find(texture); if (t != mTextures.end()) view = t->second.tex; }

  SDL_BindGPUGraphicsPipeline(mRp, mPipe);
  SDL_SetGPUViewport(mRp, &vp);
  SDL_SetGPUScissor(mRp, &sc);
  SDL_GPUBufferBinding vb = {}; vb.buffer = g.vbo; vb.offset = 0;
  SDL_BindGPUVertexBuffers(mRp, 0, &vb, 1);
  SDL_GPUBufferBinding ib = {}; ib.buffer = g.ibo; ib.offset = 0;
  SDL_BindGPUIndexBuffer(mRp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  SDL_GPUTextureSamplerBinding tsb = {}; tsb.texture = view; tsb.sampler = mSampler;
  SDL_BindGPUFragmentSamplers(mRp, 0, &tsb, 1);
  SDL_DrawGPUIndexedPrimitives(mRp, g.indexCount, 1, 0, 0, 0);
}

void RmlRenderInterfaceGpu::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
  auto it = mGeometries.find(geometry);
  if (it == mGeometries.end()) return;
  // The device is idle between present frames at the granularity RmlUi releases geometry (menu rebuilds),
  // and Shutdown waits; release immediately.
  if (it->second.vbo) SDL_ReleaseGPUBuffer(mDev, it->second.vbo);
  if (it->second.ibo) SDL_ReleaseGPUBuffer(mDev, it->second.ibo);
  mGeometries.erase(it);
}

Rml::TextureHandle RmlRenderInterfaceGpu::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) {
  (void)texture_dimensions; (void)source;   // the menu uses only generated (font) textures, no <img> files
  return 0;
}

Rml::TextureHandle RmlRenderInterfaceGpu::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) {
  if (!mDev) return 0;
  SDL_GPUTexture* tex = UploadTexture(source.data(), source_dimensions.x, source_dimensions.y);
  if (!tex) return 0;
  Rml::TextureHandle h = mNextTexture++;
  mTextures[h] = Texture{ tex };
  return h;
}

void RmlRenderInterfaceGpu::ReleaseTexture(Rml::TextureHandle texture) {
  auto it = mTextures.find(texture);
  if (it == mTextures.end()) return;
  if (it->second.tex) SDL_ReleaseGPUTexture(mDev, it->second.tex);
  mTextures.erase(it);
}

void RmlRenderInterfaceGpu::EnableScissorRegion(bool enable) { mScissorEnabled = enable; }

void RmlRenderInterfaceGpu::SetScissorRegion(Rml::Rectanglei region) {
  mScissor.x = region.Left(); mScissor.y = region.Top();
  mScissor.w = region.Width() < 0 ? 0 : region.Width();
  mScissor.h = region.Height() < 0 ? 0 : region.Height();
}

void RmlRenderInterfaceGpu::Shutdown() {
  if (!mDev) return;
  SDL_WaitForGPUIdle(mDev);
  for (auto& [h, g] : mGeometries) { if (g.vbo) SDL_ReleaseGPUBuffer(mDev, g.vbo); if (g.ibo) SDL_ReleaseGPUBuffer(mDev, g.ibo); }
  mGeometries.clear();
  for (auto& [h, t] : mTextures) if (t.tex) SDL_ReleaseGPUTexture(mDev, t.tex);
  mTextures.clear();
  if (mWhiteTex) { SDL_ReleaseGPUTexture(mDev, mWhiteTex); mWhiteTex = nullptr; }
  if (mSampler) { SDL_ReleaseGPUSampler(mDev, mSampler); mSampler = nullptr; }
  if (mPipe) { SDL_ReleaseGPUGraphicsPipeline(mDev, mPipe); mPipe = nullptr; }
  mReady = false; mDev = nullptr;
}
