// cGZCanvasW32: native events become input messages; clamping, keypad mapping, character events, input gating,
// video-mode matching on window creation.
#include <cstdio>
#include <vector>

#include "gz/CanvasW32.h"
#include "render/gd/GDriverBase.h"
#include "render/gd/VideoModes.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Sent { std::uint32_t type, event, a, b, c; };
struct Server final : cIGZMessageServer2 {
    std::vector<Sent> sent; std::vector<std::pair<char, std::uint32_t>> notes;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2* m) override {
        Sent s{m->GetType(), 0, 0, 0, 0};
        void* p = nullptr;
        if (m->QueryInterface(gz::kGZIID_cIGZMouseMessage, &p)) { auto* x = static_cast<gz::cIGZMouseMessage*>(p); s.event = x->EventType(); s.a = x->Position().x; s.b = x->Position().y; s.c = x->KeyFlags() | (static_cast<std::uint32_t>(x->WheelDelta()) << 16); x->Release(); }
        else if (m->QueryInterface(gz::kGZIID_cIGZKeyMessage, &p)) { auto* x = static_cast<gz::cIGZKeyMessage*>(p); s.event = x->EventType(); s.a = x->Key(); s.b = x->KeyFlags(); s.c = x->RepeatCount(); x->Release(); }
        else if (m->QueryInterface(gz::kGZIID_cIGZCanvasMessage, &p)) { auto* x = static_cast<gz::cIGZCanvasMessage*>(p); s.event = x->EventType(); x->Release(); }
        sent.push_back(s);
        return true;
    }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t t) override { notes.push_back({'+', t}); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t t) override { notes.push_back({'-', t}); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};
struct Driver final : gd::cGDriverBase {
    int mode = -2; void* window = nullptr; bool vsync = false;
    int CountVideoModes() override { return 3; }
    bool GetVideoModeInfo(std::uint32_t i, gd::sGDMode& m) override {
        m = {}; m.width = i == 2 ? 800 : 640; m.height = i == 2 ? 600 : 480; m.bitsPerPixel = 32; m.flags[0] = i == 1 ? 1 : 0; return true;
    }
    bool SetVideoMode(int m, void* w, bool, bool v) override { mode = m; window = w; vsync = v; return true; }
    int GetError() override { return 0; }
};
struct Graphics final : IGraphicSystemControl {
    Driver driver; bool fullScreen = false;
    DisplayMetrics GetScreenMetrics() override { return {640, 480, 32}; }
    void PreInitSetDesiredGameResolution(const DisplayMetrics&) override {}
    void PreInitSetWindowedMode(bool) override {}
    void DisableScreenBuffer() override {}
    void EnableCursorEmulation(bool) override {}
    void EnablePageFlipping(bool) override {}
    bool IsPageFlippingEnabled() override { return true; }
    void SetDefaultDriverClassID(std::uint32_t) override {}
    bool CanvasReady() override { return true; }
    DisplayMetrics GetGameMetrics() override { return {640, 480, 32}; }
    gd::cIGZGDriver* GetGDriver() override { return &driver; }
    int GetGDriverCount() override { return 1; }
    gd::cIGZGDriver* GetGDriverByIndex(int) override { return &driver; }
    void SetGDriver(gd::cIGZGDriver*) override {}
    void SetForceNoTranslucency(bool, float) override {}
    cIGZBuffer* CreateBuffer() override { return nullptr; }
    void RenderBufferToScreen(cIGZBuffer*, const cRZRect&, const cRZRect&) override {}
    void LazyFlush() override {}
    cRZRect CanvasRect() override { return cRZRect{0, 0, 800, 600}; }
    bool IsFullScreenMode() override { return fullScreen; }
    std::uint32_t GetScreenPixelFormatBits() override { return 32; }
};
}  // namespace

int main() {
    Server server; Graphics graphics;
    auto* canvas = new cGZCanvasW32(); canvas->AddRef();
    canvas->SetMessageServers(nullptr, &server);
    canvas->SetGraphicSystem(&graphics);
    CHECK(canvas->GetServiceID() == 0xa3460deb && canvas->GetServicePriority() == 0x1312cf);
    CHECK(canvas->Init() && canvas->IsInitialized());
    CHECK(canvas->GetW() == 800 && canvas->GetH() == 600);
    CHECK(graphics.driver.mode == 2 && graphics.driver.vsync);                       // the 800x600 windowed 32-bit mode matched
    CHECK(server.sent.size() == 2 && server.sent[0].event == 8 && server.sent[1].event == 0);   // window created, canvas initialised

    server.sent.clear();
    canvas->OnMouseMove(-5, 9999, 0);                                              // clamped into the canvas
    CHECK(server.sent.size() == 1 && server.sent[0].event == 0xd && server.sent[0].a == 0 && server.sent[0].b == 599);
    canvas->OnLButtonDown(10, 20, 0);
    canvas->OnRButtonUp(900, 20, 0);
    CHECK(server.sent[1].event == 7 && server.sent[1].a == 10 && server.sent[2].event == 11 && server.sent[2].a == 799);

    // keys: keypad digits are reported as the digit row; Enter is a character plus a key-down
    server.sent.clear();
    canvas->OnKeyDown(0x61, 1, 0);
    CHECK(server.sent.size() == 1 && server.sent[0].event == 5 && server.sent[0].a == 0x31);
    server.sent.clear();
    canvas->OnKeyDown(0x0d, 1, 0);
    CHECK(server.sent.size() == 2 && server.sent[0].event == 4 && server.sent[1].event == 5 && server.sent[0].a == 0x0d);
    server.sent.clear();
    canvas->OnKeyDown(0x0d, 1, 0x1000);                                            // extended: key-down only
    CHECK(server.sent.size() == 1 && server.sent[0].event == 5);
    server.sent.clear();
    canvas->OnKeyDown(0xe5, 1, 0); canvas->OnKeyUp(0xe5, 1, 0);                    // IME process key: ignored
    CHECK(server.sent.empty());
    canvas->OnKeyUp(0x41, 1, 0); CHECK(server.sent.size() == 1 && server.sent[0].event == 6);

    // input generation counting
    server.sent.clear(); server.notes.clear();
    canvas->SetInputGenerationEnabled(1, false);
    CHECK(!canvas->GetInputGenerationEnabled(1) && canvas->GetInputGenerationEnabled(2));
    canvas->OnKeyUp(0x41, 1, 0); CHECK(server.sent.empty());
    canvas->SetInputGenerationEnabled(2, false); canvas->SetInputGenerationEnabled(2, false);
    CHECK(server.notes.size() == 1 && server.notes[0].first == '+');                // only the first disable hooks the server
    canvas->OnMouseMove(1, 1, 0); CHECK(server.sent.empty());
    canvas->SetInputGenerationEnabled(2, true); CHECK(server.notes.size() == 1);
    canvas->SetInputGenerationEnabled(2, true); CHECK(server.notes.size() == 2 && server.notes[1].first == '-');
    canvas->SetInputGenerationEnabled(1, true);
    CHECK(canvas->GetInputGenerationEnabled(1) && canvas->GetInputGenerationEnabled(2));

    // activation, paint, shutdown
    server.sent.clear();
    canvas->OnActivate(1, nullptr, 0); canvas->OnActivate(0, nullptr, 1);
    CHECK(server.sent[0].event == 3 && server.sent[1].event == 2);
    canvas->ForceFullSystemRedraw(); CHECK(server.sent.back().event == 4);
    server.sent.clear();
    CHECK(canvas->Shutdown() && !canvas->IsInitialized());
    CHECK(server.sent.size() == 2 && server.sent[0].event == 1 && server.sent[1].event == 9 && graphics.driver.mode == -1);
    canvas->Release();
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}
