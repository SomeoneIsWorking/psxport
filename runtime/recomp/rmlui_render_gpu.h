#ifndef PSXPORT_RMLUI_RENDER_GPU_H
#define PSXPORT_RMLUI_RENDER_GPU_H
// RmlUi render interface on SDL_GPU (replaces the former Vulkan RmlRenderInterfaceVk). Draws the menu's
// 2D textured/coloured geometry into the port's existing SDL_GPU present render pass: gpu_vk.cpp's
// present() hands BeginFrame() the live command buffer + render pass + window size, RmlUi's
// Context::Render() then calls RenderGeometry which records indexed draws into that pass. Geometry and
// font textures upload through one-shot SDL_GPU copy command buffers (fence-waited) so they are ready
// before the present pass samples them. No depth, premultiplied-alpha blend, dynamic viewport/scissor.
#include <RmlUi/Core/RenderInterface.h>
#include <SDL3/SDL.h>
#include <unordered_map>
#include <cstdint>

class RmlRenderInterfaceGpu final : public Rml::RenderInterface {
public:
    RmlRenderInterfaceGpu() = default;
    ~RmlRenderInterfaceGpu() override { Shutdown(); }

    // Create the pipeline/sampler/white texture on the port's SDL_GPU device for the given swapchain
    // colour format. Returns false (overlay disabled) on failure.
    bool Init(SDL_GPUDevice* dev, SDL_GPUTextureFormat swap_fmt);
    void Shutdown();
    bool Ready() const { return mReady; }

    // Bind this frame's present command buffer + render pass + full window pixel size. RenderGeometry
    // records into `rp`; the per-draw vertex UBO is pushed onto `cmd`.
    void BeginFrame(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* rp, int win_w, int win_h);
    void EndFrame() { mActive = false; }

    // ---- Rml::RenderInterface ----
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    struct Geometry { SDL_GPUBuffer* vbo = nullptr; SDL_GPUBuffer* ibo = nullptr; uint32_t indexCount = 0; };
    struct Texture  { SDL_GPUTexture* tex = nullptr; };

    SDL_GPUBuffer* UploadBuffer(const void* data, uint32_t size, SDL_GPUBufferUsageFlags usage);
    SDL_GPUTexture* UploadTexture(const void* rgba, int w, int h);

    SDL_GPUDevice*             mDev = nullptr;
    SDL_GPUTextureFormat       mSwapFmt = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline*   mPipe = nullptr;
    SDL_GPUSampler*            mSampler = nullptr;
    bool                       mReady = false;

    // per-frame
    SDL_GPUCommandBuffer*      mCmd = nullptr;
    SDL_GPURenderPass*         mRp = nullptr;
    int                        mWinW = 0, mWinH = 0;
    bool                       mActive = false;
    bool                       mScissorEnabled = false;
    SDL_Rect                   mScissor = {};

    std::unordered_map<Rml::CompiledGeometryHandle, Geometry> mGeometries;
    std::unordered_map<Rml::TextureHandle, Texture>           mTextures;
    Rml::CompiledGeometryHandle mNextGeometry = 1;
    Rml::TextureHandle          mNextTexture = 1;
    SDL_GPUTexture*             mWhiteTex = nullptr;
};

#endif
