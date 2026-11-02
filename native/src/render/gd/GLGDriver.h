// cGLGDriver — the Linux implementation of cIGZGDriver, on OpenGL 3.3 core. It is cMetalGDriver's twin: the same state
// mirror (cGDriverBase), texture table, stage records and shader logic (DX7Emulator.metal ported to GLSL), drawing into a
// persistent frame target (colour + depth) that Present() copies to the window. Only the state combinations the Metal
// driver implements are implemented; anything else is counted in UnsupportedCalls().
//
// The caller owns the GL context: it must be current on this thread for every call (the frame loop's thread).
#pragma once
#include <memory>
#include <vector>

#include "render/gd/GDriverBase.h"
#include "render/gd/IBackingStore.h"
#include "render/gd/VideoModes.h"

namespace gd {

// Clear mask bits of the shipped driver (not GL's): colour 0x4000, depth 0x1000, stencil 0x2000.
constexpr std::uint32_t kClearColor = 0x4000, kClearDepth = 0x1000, kClearStencil = 0x2000;

class cGLGDriver final : public cGDriverBase {
public:
    // Needs a current OpenGL 3.3 core context. `width` x `height` is the game's (logical) resolution, the one every
    // rectangle the game passes is in; the frame target is `pixelWidth` x `pixelHeight` (0: the same), and the driver
    // scales viewports, scissors, clears and backing-store copies onto it, so geometry renders at the full pixel size.
    static std::unique_ptr<cGLGDriver> Create(std::uint32_t width, std::uint32_t height, std::uint32_t pixelWidth = 0,
                                              std::uint32_t pixelHeight = 0, IModeSource* modes = nullptr);
    std::uint32_t PixelWidth() const;
    std::uint32_t PixelHeight() const;
    ~cGLGDriver() override;

    // ---- presentation / test hooks (not part of cIGZGDriver) ----
    // frame target -> the default framebuffer's rectangle (x, y from its bottom-left, in pixels); the caller swaps
    void Present(int x, int y, int width, int height);
    bool ReadBack(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);   // top-left origin, pixels
    IBackingStore* BackingStore();
    std::uint32_t DrawCalls() const;
    std::uint32_t PipelineCount() const;
    // The GL context is going away (the window closes, or the process exits with windows still alive): texture and target
    // deletes after this only forget their objects. A late delete through a dead context crashed at exit on NVIDIA.
    void DetachContext();

    // ---- cIGZGDriver ----
    void Clear(std::uint32_t mask) override;
    void ClearColor(float r, float g, float b, float a) override;
    void ColorMask(bool enable) override;                             // a flush-only no-op in the shipped driver
    void DepthFunc(eGDTestFunc func) override;
    void DepthMask(bool enable) override;
    void BlendFunc(eGDBlend src, eGDBlend dst) override;
    void AlphaFunc(eGDTestFunc func, float ref) override;
    void Enable(eGDDriverState state) override;
    void Disable(eGDDriverState state) override;
    void MatrixMode(eGDMatrixTarget target) override;
    void LoadMatrix(const float* m) override;
    void LoadIdentity() override;
    void InterleavedArrays(eGDVertexFormat format, int stride, const void* data) override;
    void DrawArrays(eGDPrimType mode, int first, int count) override;
    void DrawElements(eGDPrimType mode, int count, eGDType indexType, const void* indices) override;
    void Flush() override;
    void SetViewport() override;
    void SetViewport(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) override;
    void GetViewport(std::uint32_t out[4]) override;
    int VertexFormatStride(eGDVertexFormat format) const override;
    int GetError() override;
    int CountVideoModes() override;
    bool GetVideoModeInfo(std::uint32_t index, sGDMode& mode) override;
    bool GetVideoModeInfo(sGDMode& mode) override;
    bool SetVideoMode(int mode, void* window, bool windowed, bool vsync) override;

    // textures (handle semantics of the shipped driver: see TextureTable.h)
    void GenTextures(int count, std::uint32_t* ids) override;
    void DeleteTextures(int count, const std::uint32_t* ids) override;
    bool IsTexture(std::uint32_t texture) override;
    void BindTexture(eGDTextureTarget target, std::uint32_t texture) override;
    void TexParameter(eGDTextureTarget target, eGDTextureParamType param, eGDTextureParam value) override;
    void TexEnv(eGDTextureEnvTarget target, eGDTextureEnvParamType param, eGDTextureEnvModeParam mode) override;
    void TexEnv(eGDTextureEnvTarget target, eGDTextureEnvParamType param, const float* values) override;
    void SetCombiner(const cGDCombiner& combiner, std::uint32_t stage) override;
    void TexStage(std::uint32_t stage) override;
    void TexStageCoord(eGDTexCoordSource source) override;
    void TexStageMatrix(const float* m, std::uint32_t a, std::uint32_t b, eGDTexMatFlags flags) override;
    void ColorMultiplier(float r, float g, float b) override;
    void AlphaMultiplier(float a) override;
    void SetTexture(std::uint32_t texture, std::uint32_t stage) override;
    std::uint32_t GetTexture(std::uint32_t stage) const override;
    std::uint32_t CreateTexture(eGDInternalTextureFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t mipLevels, eGDTextureHintFlags hint) override;
    void LoadTextureLevel(std::uint32_t texture, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, eGDTextureFormat format, eGDType type, std::uint32_t rowLength, const void* pixels) override;

private:
    cGLGDriver();
    void MarkStage(bool enabled);
    void Encode(unsigned primitive, const std::uint8_t* vertices, int vertexCount, const std::uint16_t* indices, int indexCount);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gd
