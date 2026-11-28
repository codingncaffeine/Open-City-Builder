// A game picture (cIGZBuffer, the cSLBuffer / LoadBuffer result) as the window system's buffer (IWinBuffer): bounds, colour
// conversion, transparency and the buffer itself for the controls that draw it. Not in the original, where both are one class.
#pragma once
#include "gz/Buffer.h"
#include "gz/Win.h"

class WinBufferOfGZBuffer final : public IWinBuffer {
public:
    explicit WinBufferOfGZBuffer(cIGZBuffer* buffer) : buffer_(buffer) { buffer_->AddRef(); }   // one reference for the caller
    ~WinBufferOfGZBuffer() { buffer_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) {
            delete this;
            return 0;
        }
        return refs_;
    }
    const cRZRect* GetBounds() const override { return buffer_->GetBufferArea(); }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return buffer_->ConvertRGBValueToNative(r, g, b); }
    void NativeToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { buffer_->ConvertNativeValueToRGB(n, r, g, b); }
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    void SetTransparency(std::uint32_t color) override { buffer_->SetTransparency(color); }
    cIGZBuffer* AsGZBuffer() override { return buffer_; }
    gz::IDrawTargetBuffer* AsDrawSource() override { return buffer_; }

private:
    int refs_ = 1;
    cIGZBuffer* buffer_;
};
