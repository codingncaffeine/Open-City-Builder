// The region screen's cloud shadows and map scrolling on the native application: the services of the recovered
// cSC4WinRegionClouds and cSC4WinRegionScroll.
#include "app/NativeRegionExtras.h"

#include <cstdio>
#include <cstdlib>

#include "app/NativeModelMaker.h"
#include "app/NativeUi.h"
#include "render/gd/IGZGDriver.h"
#include "render/gd/VideoModes.h"
#include "sc4/WinRegionClouds.h"
#include "sc4/WinRegionScroll.h"
#include "sc4/WinRegionScreen.h"
#include "gz/Buffer.h"
#include "gz/WinBufferOfGZBuffer.h"
#include "gz/WinBMP.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinCtrlFactory.h"
#include "sc4/SLBuffer.h"

namespace native {
namespace {

gd::cIGZGDriver* g_driver = nullptr;
std::uint8_t* (*g_preferences)() = nullptr;

// SL::spGS2 vslot 0x18 is the graphics driver itself: the cloud code's vslot numbers are the driver's slots
// (0x160 Enable, 0x168 Disable, 0xa8 BlendFunc, 0x198 TexStage, 0x1a0 TexStageCoord, 0x1a8 TexStageMatrix,
// 0x28 InterleavedArrays, 0x1d0 SetTexture, 0xe8 TexParameter, 0xd8 TexEnv, 0x18 DrawArrays), so each call forwards 1:1.
class CloudsDevice final : public sc4::IRegionCloudsDevice {
public:
    void SetState168(int value) override { g_driver->Disable(static_cast<gd::eGDDriverState>(value)); }
    void SetState160(int value) override { g_driver->Enable(static_cast<gd::eGDDriverState>(value)); }
    void SetBlend(int source, int destination) override { g_driver->BlendFunc(source, destination); }
    void SetState198(int value) override { g_driver->TexStage(static_cast<std::uint32_t>(value)); }
    void SetState1a0(int value) override { g_driver->TexStageCoord(static_cast<gd::eGDTexCoordSource>(value)); }
    void SetState1a8(int a, int b, int c, int d) override {
        g_driver->TexStageMatrix(reinterpret_cast<const float*>(static_cast<std::intptr_t>(a)), static_cast<std::uint32_t>(b),
                                 static_cast<std::uint32_t>(c), static_cast<gd::eGDTexMatFlags>(d));
    }
    // Called once per paint, before the clouds draw: the clouds are in screen coordinates, so the pass takes the whole
    // screen. The driver still holds the viewport and projection of whichever window drew last (the draw context sets
    // them per clip rectangle), which squeezed the clouds into that window's corner of the screen.
    void SetVertices(int format, int offset, const sc4::CloudVertex* vertices) override {
        gd::sGDMode mode{};
        g_driver->GetVideoModeInfo(mode);
        if (mode.width > 0 && mode.height > 0) {
            g_driver->SetViewport();
            // cGZDrawContextRenderToHardware::UpdateTransform's orthographic projection for the full screen (y down)
            const float sx = 2.0f / float(mode.width), sy = -2.0f / float(mode.height);
            const float projection[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, 0, 0, -1.0f, 1.0f, 0.5f, 1.0f};
            g_driver->MatrixMode(1);
            g_driver->LoadMatrix(projection);
            g_driver->MatrixMode(0);
            g_driver->LoadIdentity();
        }
        g_driver->InterleavedArrays(format, offset, vertices);
    }
    void SetTexture(std::uint32_t handle, int stage) override { g_driver->SetTexture(handle, static_cast<std::uint32_t>(stage)); }
    void SetTextureState(int stage, int state, int value) override { g_driver->TexParameter(stage, state, value); }
    void SetSamplerState(int stage, int state, int value) override { g_driver->TexEnv(stage, state, static_cast<gd::eGDTextureEnvModeParam>(value)); }
    void DrawPrimitive(int type, int first, int count) override { g_driver->DrawArrays(type, first, count); }
};

class CloudTexture final : public sc4::IRegionCloudTexture {
public:
    explicit CloudTexture(std::uint32_t handle) : handle_(handle) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }
    std::uint32_t TextureHandle() override { return handle_; }

private:
    std::uint32_t handle_;
    std::uint32_t refs_ = 1;
};

class CloudsHost final : public sc4::IRegionCloudsHost {
public:
    sc4::IRegionCloudsDevice* Device() override { return g_driver ? &device_ : nullptr; }
    sc4::IRegionCloudTexture* GetTexture(std::uint32_t id) override {
        const unsigned handle = FshTextureId(0x1abe787d, id);   // the clouds' FSH group
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "region: cloud texture %08x -> %u\n", id, handle);
        return handle ? new CloudTexture(handle) : nullptr;
    }
    bool HasTextureFactory() override { return g_driver != nullptr; }

private:
    CloudsDevice device_;
};

class ScrollHost final : public sc4::IRegionScrollHost {
public:
    bool EdgeScrollEnabled() override {
        std::uint8_t* preferences = g_preferences ? g_preferences() : nullptr;
        return preferences && preferences[0xf09] != 0;
    }
    bool CursorInGameWindow() override { return true; }
    // cSLBuffer(0x856ddbac, 0x46a006b0, 0x094ac89a, 1, 0): the origin marker picture, one reference for the caller.
    IWinBuffer* LoadScrollOriginImage() override {
        cSLBuffer picture(0x856ddbac, 0x46a006b0, 0x094ac89a, 1, false);
        if (!picture.Load() || !picture.GetBuffer()) return nullptr;
        return new WinBufferOfGZBuffer(picture.GetBuffer());
    }
    // RZ::WCM() CreateWinBMP(0x48e945b4, image, fit): the marker window, one reference for the caller.
    cIGZWin* CreateImageWindow(IWinBuffer* image) override {
        gz::cIGZWinCtrlMgr* controls = gz::WinCtrlMgr();
        cIGZBuffer* buffer = image ? image->AsGZBuffer() : nullptr;
        gz::cIGZWinBMP* bmp = controls && buffer ? controls->CreateWinBMP(0x48e945b4, buffer, true) : nullptr;
        return bmp ? bmp->AsIGZWin() : nullptr;   // the bitmap control's reference passes to the caller
    }
};

CloudsHost g_clouds;
ScrollHost g_scroll;

}  // namespace

void InstallRegionExtras(gd::cIGZGDriver* driver, std::uint8_t* (*preferences)()) {
    g_driver = driver;
    g_preferences = preferences;
    InstallModelResources(driver);   // the cloud textures are driver textures from the same FSH loader
    sc4::SetRegionCloudsHost(&g_clouds);
    sc4::SetRegionScrollHost(&g_scroll);
}

}  // namespace native
