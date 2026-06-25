#pragma once
// RmlUi render interface for the Tomba2 port's Vulkan present pass. Records the menu's 2D geometry
// into an EXTERNALLY-PROVIDED command buffer + render pass (the port's swapchain present pass in
// gpu_gpu.cpp), so the menu draws on top of the game's frame. This is NOT the stock RmlUi VK backend
// (RmlUi_Renderer_VK.cpp) — that one owns its own device/swapchain/present and is wrong for a renderer
// that already has all of those. Adapted from soh3d's RmlRenderInterfaceVk (same record-into-host-pass
// model), but using the port's pre-compiled SPIR-V (no runtime glslang dependency) and the port's own
// device handles passed in at init.

#include <RmlUi/Core/RenderInterface.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>

class RmlRenderInterfaceVk : public Rml::RenderInterface {
public:
    RmlRenderInterfaceVk();
    ~RmlRenderInterfaceVk() override;

    // One-time setup: build the pipeline/sampler/UBO rings against the port's device + present pass.
    // `frames_in_flight` is how many distinct swapchain frames the present loop cycles through (the
    // UBO ring is per-frame so a draw isn't overwritten while still in flight).
    bool Init(VkPhysicalDevice phys, VkDevice dev, uint32_t qfam, VkQueue queue,
              VkRenderPass present_rpass, uint32_t frames_in_flight);
    void Shutdown();

    // Per-frame: tell the interface which swapchain frame + command buffer + viewport it is recording
    // into THIS frame, just before Rml::Context::Render(). Resets that frame's UBO ring + desc pool.
    void BeginFrame(VkCommandBuffer cmd, uint32_t frame_index, VkViewport viewport, VkRect2D full_scissor);
    void EndFrame();

    // --- Rml::RenderInterface ---
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
    struct Geometry {
        VkBuffer vbo = VK_NULL_HANDLE; VkDeviceMemory vboMem = VK_NULL_HANDLE;
        VkBuffer ibo = VK_NULL_HANDLE; VkDeviceMemory iboMem = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
    };
    struct Texture {
        VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    };
    struct Ring {
        VkBuffer ubo = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* mapped = nullptr;
        VkDeviceSize capacity = 0; VkDeviceSize offset = 0; VkDescriptorPool pool = VK_NULL_HANDLE;
    };
    struct PendingFree { uint64_t freeAtFrame = 0; Geometry geo; Texture tex; };

    uint32_t FindMemType(uint32_t bits, VkMemoryPropertyFlags want) const;
    void MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                    VkBuffer& buf, VkDeviceMemory& mem, void** mappedOut);
    void DestroyGeometry(Geometry& g);
    void DestroyTexture(Texture& t);
    void ProcessPendingFrees();

    bool mReady = false;
    bool mActive = false;
    uint64_t mFrameCounter = 0;

    VkPhysicalDevice mPhys = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    uint32_t mQfam = 0;
    VkQueue mQueue = VK_NULL_HANDLE;
    VkRenderPass mRenderPass = VK_NULL_HANDLE;
    uint32_t mFramesInFlight = 0;

    VkCommandPool mCmdPool = VK_NULL_HANDLE;   // one-shot transfers (geometry/texture uploads)
    VkCommandBuffer mCmd = VK_NULL_HANDLE;     // current frame's record target (from BeginFrame)
    VkViewport mViewport{};
    VkRect2D mFullScissor{};
    uint32_t mFrameIndex = 0;

    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mPipeLayout = VK_NULL_HANDLE;
    VkShaderModule mVs = VK_NULL_HANDLE, mFs = VK_NULL_HANDLE;
    VkPipeline mPipeline = VK_NULL_HANDLE;
    VkSampler mSampler = VK_NULL_HANDLE;
    Texture mWhiteTex{};
    VkDeviceSize mUboStride = 0;
    std::vector<Ring> mRings;

    bool mScissorEnabled = false;
    VkRect2D mScissor{};

    std::unordered_map<Rml::CompiledGeometryHandle, Geometry> mGeometries;
    std::unordered_map<Rml::TextureHandle, Texture> mTextures;
    Rml::CompiledGeometryHandle mNextGeometry = 1;
    Rml::TextureHandle mNextTexture = 1;
    std::vector<PendingFree> mPendingFrees;
};
