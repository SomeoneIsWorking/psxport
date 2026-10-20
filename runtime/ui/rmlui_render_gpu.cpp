// rmlui_render_gpu.cpp — see rmlui_render_gpu.h.
#include "rmlui_render_gpu.h"
#include "render_device.h"

#include <RmlUi/Core/Vertex.h>
#include <SDL3_image/SDL_image.h>
#include <cstring>
#include <vector>

#include "psxport_generated/render_shaders.h"
#include <lucent/log.h>

namespace {

struct RmlUbo {
  float uTranslate[2];
  float uViewport[2];
};

constexpr psx::render::ShaderCode kRmlVertex{spv_g_rml_vert, spv_g_rml_vert_len, {0, 1, 0}};
constexpr psx::render::ShaderCode kRmlFragment{spv_g_rml_frag, spv_g_rml_frag_len, {1, 0, 0}};

} // namespace

// Uploads are submitted, not waited on: a later submit that draws them is ordered after the copy.
SDL_GPUBuffer *RmlRenderInterfaceGpu::UploadBuffer(std::span<const std::byte> bytes, SDL_GPUBufferUsageFlags usage) {
  const auto size = static_cast<uint32_t>(bytes.size());
  if (!size) {
    return nullptr;
  }
  SDL_GPUBufferCreateInfo bi = {};
  bi.usage = usage;
  bi.size = size;
  SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(mDevice.handle(), &bi);
  if (!buf) {
    return nullptr;
  }
  SDL_GPUTransferBuffer *xfer = mDevice.makeTransferBuffer(SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size);
  void *p = SDL_MapGPUTransferBuffer(mDevice.handle(), xfer, false);
  std::memcpy(p, bytes.data(), size);
  SDL_UnmapGPUTransferBuffer(mDevice.handle(), xfer);
  SDL_GPUCommandBuffer *cmd = mDevice.acquire();
  SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTransferBufferLocation src = {};
  src.transfer_buffer = xfer;
  SDL_GPUBufferRegion dst = {};
  dst.buffer = buf;
  dst.size = size;
  SDL_UploadToGPUBuffer(cp, &src, &dst, false);
  SDL_EndGPUCopyPass(cp);
  mDevice.submit(cmd, "rmlui buffer upload");
  mDevice.release(xfer);
  return buf;
}

RmlRenderInterfaceGpu::RmlRenderInterfaceGpu(psx::render::RenderDevice &device) : mDevice(device) {
  SDL_GPUVertexBufferDescription vbd = {};
  vbd.slot = 0;
  vbd.pitch = (Uint32)sizeof(Rml::Vertex);
  vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
  SDL_GPUVertexAttribute attrs[3] = {};
  attrs[0].location = 0;
  attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[0].offset = (Uint32)offsetof(Rml::Vertex, position);
  attrs[1].location = 1;
  attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
  attrs[1].offset = (Uint32)offsetof(Rml::Vertex, colour);
  attrs[2].location = 2;
  attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  attrs[2].offset = (Uint32)offsetof(Rml::Vertex, tex_coord);
  SDL_GPUVertexInputState input = {};
  input.vertex_buffer_descriptions = &vbd;
  input.num_vertex_buffers = 1;
  input.vertex_attributes = attrs;
  input.num_vertex_attributes = 3;

  SDL_GPUColorTargetBlendState blend = {};
  blend.enable_blend = true;
  blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
  blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

  mPipe = mDevice.makePipeline(kRmlVertex, kRmlFragment, mDevice.sinkFormat(), &blend, &input);
  const std::uint8_t white[4] = {255, 255, 255, 255};
  mWhiteTex = mDevice.makeFilledTexture({1, 1}, white);
}

void RmlRenderInterfaceGpu::BeginFrame(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h) {
  mCmd = cmd;
  mRp = rp;
  mWinW = win_w;
  mWinH = win_h;
  mScissorEnabled = false;
  mScissor = {0, 0, win_w, win_h};
  mActive = true;
}

Rml::CompiledGeometryHandle RmlRenderInterfaceGpu::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                                   Rml::Span<const int> indices) {
  if (vertices.empty() || indices.empty()) {
    return 0;
  }
  Geometry g;
  g.vbo = UploadBuffer(std::as_bytes(std::span(vertices.data(), vertices.size())), SDL_GPU_BUFFERUSAGE_VERTEX);
  g.ibo = UploadBuffer(std::as_bytes(std::span(indices.data(), indices.size())), SDL_GPU_BUFFERUSAGE_INDEX);
  g.indexCount = (uint32_t)indices.size();
  if (!g.vbo || !g.ibo) {
    mDevice.release(g.vbo);
    mDevice.release(g.ibo);
    return 0;
  }
  Rml::CompiledGeometryHandle h = mNextGeometry++;
  mGeometries[h] = g;
  return h;
}

void RmlRenderInterfaceGpu::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                           Rml::Vector2f translation,
                                           Rml::TextureHandle texture) {
  if (!mActive || !mRp) {
    return;
  }
  auto it = mGeometries.find(geometry);
  if (it == mGeometries.end()) {
    return;
  }
  const Geometry &g = it->second;
  if (!g.indexCount) {
    return;
  }

  RmlUbo ubo;
  ubo.uTranslate[0] = translation.x;
  ubo.uTranslate[1] = translation.y;
  ubo.uViewport[0] = (float)mWinW;
  ubo.uViewport[1] = (float)mWinH;
  SDL_PushGPUVertexUniformData(mCmd, 0, &ubo, sizeof ubo);

  SDL_GPUViewport vp = {0.0f, 0.0f, (float)mWinW, (float)mWinH, 0.0f, 1.0f};
  SDL_Rect sc = mScissorEnabled ? mScissor : (SDL_Rect){0, 0, mWinW, mWinH};
  if (sc.w < 0) {
    sc.w = 0;
  }
  if (sc.h < 0) {
    sc.h = 0;
  }

  SDL_GPUTexture *view = mWhiteTex;
  if (texture) {
    auto t = mTextures.find(texture);
    if (t != mTextures.end()) {
      view = t->second;
    }
  }

  SDL_BindGPUGraphicsPipeline(mRp, mPipe);
  SDL_SetGPUViewport(mRp, &vp);
  SDL_SetGPUScissor(mRp, &sc);
  SDL_GPUBufferBinding vb = {};
  vb.buffer = g.vbo;
  vb.offset = 0;
  SDL_BindGPUVertexBuffers(mRp, 0, &vb, 1);
  SDL_GPUBufferBinding ib = {};
  ib.buffer = g.ibo;
  ib.offset = 0;
  SDL_BindGPUIndexBuffer(mRp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  SDL_GPUTextureSamplerBinding tsb = {};
  tsb.texture = view;
  tsb.sampler = mDevice.linearSampler();
  SDL_BindGPUFragmentSamplers(mRp, 0, &tsb, 1);
  SDL_DrawGPUIndexedPrimitives(mRp, g.indexCount, 1, 0, 0, 0);
}

void RmlRenderInterfaceGpu::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
  auto it = mGeometries.find(geometry);
  if (it == mGeometries.end()) {
    return;
  }
  mDevice.release(it->second.vbo);
  mDevice.release(it->second.ibo);
  mGeometries.erase(it);
}

// An <img> file (a bug report's captured frames): decoded by SDL_image, converted to the RGBA byte order
// GenerateTexture uploads, and premultiplied, because RmlUi 6 blends every texture as premultiplied alpha.
Rml::TextureHandle RmlRenderInterfaceGpu::LoadTexture(Rml::Vector2i &texture_dimensions, const Rml::String &source) {
  SDL_Surface *loaded = IMG_Load(source.c_str());
  if (!loaded) {
    lucent::error("rmlui", "cannot load image {}: {}", source, SDL_GetError());
    return 0;
  }
  SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(loaded);
  if (!rgba) {
    lucent::error("rmlui", "cannot convert image {} to RGBA: {}", source, SDL_GetError());
    return 0;
  }
  const int w = rgba->w;
  const int h = rgba->h;
  std::vector<Rml::byte> pixels(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    const auto *row = static_cast<const Rml::byte *>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch;
    Rml::byte *out = pixels.data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w * 4; x += 4) {
      const unsigned alpha = row[x + 3];
      out[x + 0] = static_cast<Rml::byte>(row[x + 0] * alpha / 255);
      out[x + 1] = static_cast<Rml::byte>(row[x + 1] * alpha / 255);
      out[x + 2] = static_cast<Rml::byte>(row[x + 2] * alpha / 255);
      out[x + 3] = static_cast<Rml::byte>(alpha);
    }
  }
  SDL_DestroySurface(rgba);
  texture_dimensions = {w, h};
  return GenerateTexture({pixels.data(), pixels.size()}, texture_dimensions);
}

Rml::TextureHandle RmlRenderInterfaceGpu::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                          Rml::Vector2i source_dimensions) {
  if (source_dimensions.x < 1 || source_dimensions.y < 1) {
    return 0;
  }
  SDL_GPUTexture *tex =
      mDevice.makeFilledTexture({source_dimensions.x, source_dimensions.y}, {source.data(), source.size()});
  Rml::TextureHandle h = mNextTexture++;
  mTextures[h] = tex;
  return h;
}

void RmlRenderInterfaceGpu::ReleaseTexture(Rml::TextureHandle texture) {
  auto it = mTextures.find(texture);
  if (it == mTextures.end()) {
    return;
  }
  mDevice.release(it->second);
  mTextures.erase(it);
}

void RmlRenderInterfaceGpu::EnableScissorRegion(bool enable) {
  mScissorEnabled = enable;
}

void RmlRenderInterfaceGpu::SetScissorRegion(Rml::Rectanglei region) {
  mScissor.x = region.Left();
  mScissor.y = region.Top();
  mScissor.w = region.Width() < 0 ? 0 : region.Width();
  mScissor.h = region.Height() < 0 ? 0 : region.Height();
}

RmlRenderInterfaceGpu::~RmlRenderInterfaceGpu() {
  for (auto &[h, g] : mGeometries) {
    mDevice.release(g.vbo);
    mDevice.release(g.ibo);
  }
  for (auto &[h, t] : mTextures) {
    mDevice.release(t);
  }
  mDevice.release(mWhiteTex);
  mDevice.release(mPipe);
}
