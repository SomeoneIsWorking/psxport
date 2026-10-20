// rmlui_render_gpu.h — RmlUi's render interface on the RenderDevice: indexed textured draws recorded
// into the sink pass the present hands BeginFrame. No depth, premultiplied-alpha blend.
#pragma once
#include <RmlUi/Core/RenderInterface.h>
#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace psx::render {
class RenderDevice;
} // namespace psx::render

class RmlRenderInterfaceGpu final : public Rml::RenderInterface {
public:
  explicit RmlRenderInterfaceGpu(psx::render::RenderDevice &device);
  ~RmlRenderInterfaceGpu() override;
  RmlRenderInterfaceGpu(const RmlRenderInterfaceGpu &) = delete;
  RmlRenderInterfaceGpu &operator=(const RmlRenderInterfaceGpu &) = delete;

  // This frame's command buffer, open pass and full target size; RenderGeometry records into the pass.
  void BeginFrame(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int win_w, int win_h);
  void EndFrame() {
    mActive = false;
  }

  Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                              Rml::Span<const int> indices) override;
  void
  RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
  void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
  Rml::TextureHandle LoadTexture(Rml::Vector2i &texture_dimensions, const Rml::String &source) override;
  Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
  void ReleaseTexture(Rml::TextureHandle texture) override;
  void EnableScissorRegion(bool enable) override;
  void SetScissorRegion(Rml::Rectanglei region) override;

private:
  struct Geometry {
    SDL_GPUBuffer *vbo = nullptr;
    SDL_GPUBuffer *ibo = nullptr;
    uint32_t indexCount = 0;
  };

  SDL_GPUBuffer *UploadBuffer(std::span<const std::byte> bytes, SDL_GPUBufferUsageFlags usage);

  psx::render::RenderDevice &mDevice;
  SDL_GPUGraphicsPipeline *mPipe = nullptr;

  SDL_GPUCommandBuffer *mCmd = nullptr;
  SDL_GPURenderPass *mRp = nullptr;
  int mWinW = 0;
  int mWinH = 0;
  bool mActive = false;
  bool mScissorEnabled = false;
  SDL_Rect mScissor = {};

  std::unordered_map<Rml::CompiledGeometryHandle, Geometry> mGeometries;
  std::unordered_map<Rml::TextureHandle, SDL_GPUTexture *> mTextures;
  Rml::CompiledGeometryHandle mNextGeometry = 1;
  Rml::TextureHandle mNextTexture = 1;
  SDL_GPUTexture *mWhiteTex = nullptr;
};
