// NativeGraphics — the game's own 2-D draw path running on the native Metal driver. The screen buffer's draw context uses the
// recovered hardware engine (cGZDrawContextRenderToHardware) with the recovered hardware half (cGZDrawContextRenderToSimGL),
// which issues cIGZGDriver calls into cMetalGDriver; windows with a private buffer (winflag_pbuff) draw into CPU buffers with
// the recovered software engine and are shown on the screen by a hardware blit (buffer -> texture cache -> textured quads), which
// is what the shipped game does. Nothing here is a software presentation: the frame is whatever the driver holds.
//
// Not game code (there is no original counterpart): the seams the recovered classes expect (IWinGraphics, IDrawContextHost,
// IBufferHost, ISimGLDrawServices) implemented over the driver.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "gz/Buffer.h"
#include "gz/DrawContext.h"
#include "gz/DrawContextAsBuffer.h"
#include "gz/DrawContextRenderToHardware.h"
#include "gz/DrawContextRenderToSimGL.h"
#include "gz/Win.h"
#include "render/gd/IGZGDriver.h"

namespace native {

// RZ::GS() / the draw system as the hardware half reaches them.
class DriverServices final : public ISimGLDrawServices {
public:
    explicit DriverServices(gd::cIGZGDriver* driver) : driver_(driver) {}
    void RegisterBufferCache(cIGZBufferHardwareCache*) override {}
    void UnregisterBufferCache(cIGZBufferHardwareCache*) override {}
    gd::cIGZGDriver* GetGDriver() override { return driver_; }
    void DriverStateChanged() override {}
    void ScreenDirty() override {}
    bool TranslucencyAllowed() override { return true; }
    float TranslucencyThreshold() override { return 0.5f; }
    std::uint32_t BufferCacheClock() override { return clock_; }
    void Tick() { ++clock_; }

private:
    gd::cIGZGDriver* driver_;
    std::uint32_t clock_ = 0;
};

// An IWinBuffer over a cGZBuffer (XRGB8888) with its own draw context. The screen buffer's context draws through the driver;
// a private buffer's context draws into its pixels.
class Buffer final : public IWinBuffer {
public:
    Buffer(int width, int height, bool onScreen);
    ~Buffer();
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }

    const cRZRect* GetBounds() const override { return buffer_->GetBufferArea(); }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return buffer_->ConvertRGBValueToNative(r, g, b); }
    void NativeToRGB(std::uint32_t native, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { buffer_->ConvertNativeValueToRGB(native, r, g, b); }
    void* GetDrawContext() override { return asBuffer_->GetDrawContext(); }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    bool Fill(std::uint32_t color, const cRZRect& rect) override;
    void SetTransparency(std::uint32_t color) override { buffer_->SetTransparency(color); }
    bool GetTransparency() override { return buffer_->GetTransparency(); }
    bool IsPixelTransparent(int x, int y) override { return buffer_->IsPixelTransparent(std::uint32_t(x), std::uint32_t(y)); }
    bool GetTransparentColor(std::uint32_t& color) override { return buffer_->GetTransparentColor(color); }
    void RemoveTransparency() override { buffer_->RemoveTransparency(); }
    gz::IDrawTargetBuffer* AsDrawSource() override { return buffer_; }
    bool Blt(IWinBuffer* source, const cRZRect& sourceRect, const cRZRect& destRect) override;

    cIGZBufferDrawTarget* QueryDrawTarget() override { return asBuffer_; }
    cIGZBuffer* AsBuffer() { return asBuffer_; }
    cIGZBuffer* AsGZBuffer() override { return asBuffer_; }
    cGZBuffer* Pixels() { return buffer_; }
    bool OnScreen() const { return onScreen_; }
    // every live buffer (debug dumps of private buffers)
    static const std::vector<Buffer*>& All();

private:
    int refs_ = 1;
    bool onScreen_;
    cGZBuffer* buffer_ = nullptr;
    gz::cGZDrawContext* context_ = nullptr;
    gz::cGZDrawContextAsBuffer* asBuffer_ = nullptr;
};

class Graphics final : public IWinGraphics, public gz::IDrawContextHost, public IBufferHost {
public:
    Graphics(gd::cIGZGDriver* driver, int width, int height);
    ~Graphics();

    Buffer* Primary() { return primary_.get(); }
    DriverServices& Services() { return services_; }

    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return primary_->MakeNativeColor(r, g, b); }
    void NativeToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { primary_->NativeToRGB(n, r, g, b); }
    IWinBuffer* PrimaryBuffer() override { return primary_.get(); }
    bool DrawSystemActive() override { return true; }
    IWinBuffer* DrawSystemCreateBuffer(int width, int height) override { return new Buffer(width, height, false); }
    IWinBuffer* GraphicsCreateBuffer() override { return nullptr; }
    bool GetPrivateBufferKeyColor(std::uint32_t& nativeColor) override { nativeColor = primary_->MakeNativeColor(0xff, 0x00, 0xff); return true; }

    gz::cIGZDrawEngine* CreateEngine(gz::IDrawTargetBuffer* target) override;

    bool HasGraphicsSystem() override { return true; }
    cGZBufferColorType DisplayColorType() override { return {kBufferColorARGB8888, 32}; }
    bool QueryTextureBitCount(std::uint32_t& bits) override { bits = 32; return true; }

private:
    gd::cIGZGDriver* driver_;
    DriverServices services_;
    std::unique_ptr<Buffer> primary_;
};

}  // namespace native
