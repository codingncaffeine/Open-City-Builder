// Tests of the cGZWin rendering pipeline: private buffers, the fade effect and its temporary window, PlotComposite
// and PlotPresent, with fake buffers / draw contexts / window manager.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "gz/Buffer.h"
#include "gz/DrawContext.h"
#include "gz/Win.h"
#include "rz/cRZVariant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

std::vector<std::string> g_log;
void Log(const std::string& s) { g_log.push_back(s); }
std::string Joined() { std::string r; for (auto& s : g_log) { r += s; r += ';'; } return r; }

struct FakeMgr final : cIGZWinMgr {
    std::set<cIGZWin*> valid;
    std::vector<cIGZWin*> plotStrategy, destroyed;
    cIGZWin* mainWin = nullptr;
    int plots = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZWin* GetMainWindow() override { return mainWin; }
    void AppendToPlotStrategy(cIGZWin* w) override { plotStrategy.push_back(w); }
    void Plot() override { ++plots; }
    void PostMessageToTarget(cIGZWin*, const cGZMessage&) override {}
    void AddWindowToValidList(cIGZWin* w) override { valid.insert(w); }
    void RemoveWindowFromValidList(cIGZWin* w) override { valid.erase(w); }
    void CleanUpWindowReferences(cIGZWin*) override {}
    bool DestroyWindow(cIGZWin* w) override { destroyed.push_back(w); return true; }
    bool IsWindowValid(cIGZWin* w) override { return valid.count(w) != 0; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void SetCurrentMouseWin(cIGZWin*) override {}
    cIGZWin* GZGetCapture() override { return nullptr; }
    void GZSetCapture(cIGZWin*) override {}
    void GZReleaseCapture(cIGZWin*) override {}
    cIGZCursorManager* GetCursorManager() override { return nullptr; }
    cIGZWin* GZGetFocus() override { return nullptr; }
    void GZSetFocus(cIGZWin*) override {}
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
};

// A recording draw context.
struct FakeCtx final : gz::cIGZDrawContext {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { Log("ctx.AddRef"); return ++refs; }
    std::uint32_t Release() override { Log("ctx.Release"); return --refs; }
    void PushTransform() override { Log("PushTransform"); }
    void ClipReset() override { Log("ClipReset"); }
    void Clip(const cRZRect&) override {}
    void ClipAbsolute(const cRZRect& r) override { Log("ClipAbsolute" + std::to_string(r.left) + "," + std::to_string(r.top)); }
    void Offset(const cRZPoint&) override {}
    void OffsetAbsolute(const cRZPoint& p) override { Log("OffsetAbsolute" + std::to_string(p.x) + "," + std::to_string(p.y)); }
    void PopTransform() override { Log("PopTransform"); }
    void PushState() override { Log("PushState"); }
    void PopState() override { Log("PopState"); }
    cRZRect GetCanvasArea() override { return {}; }
    cRZRect GetClipRect() override { return {}; }
    cRZPoint GetOrigin() override { return {}; }
    int GetTransformStackDepth() override { Log("Depth"); return 0; }
    bool IsTargetAttached() override { return true; }
    void SetTarget(gz::IDrawTargetBuffer*) override {}
    gz::cIGZDrawEngine* GetEngine() override { return nullptr; }
    void ResetToDefaultState() override { Log("Reset"); }
    void SetColor(std::uint32_t rgb) override { char b[32]; std::snprintf(b, sizeof b, "Color%08x", rgb); Log(b); }
    void SetColor(std::uint32_t, std::uint32_t) override {}
    void SetAlphaBlend(int blend) override { Log("Alpha" + std::to_string(blend)); }
    void SetBltModulate(bool on) override { Log(std::string("Modulate") + (on ? "1" : "0")); }
    void SetAntialiasing(bool) override {}
    void SetLineThickness(float) override {}
    void ClearGradient() override {}
    void SetGradientTransform(const float (*)[4], float, float) override {}
    void ClearBlt() override {}
    void SetBlt(cIGZUnknown*, float, float, float, float) override {}
    void ClearBltTiling() override {}
    void SetBltTiling(const cRZRect&) override {}
    void Clear() override {}
    void Point(int, int) override {}
    void Line(int, int, int, int) override {}
    void Fill(const cRZRect&) override {}
    void Ellipse(int, int, int, int) override {}
    void FillEllipse(int, int, int, int) override {}
    void Blt(gz::IDrawTargetBuffer* src, const cRZRect& s, const cRZRect& d) override {
        Log("Blt" + std::string(src ? "src" : "null") + "(" + std::to_string(s.left) + "," + std::to_string(s.top) + "," + std::to_string(s.right) +
            "," + std::to_string(s.bottom) + ")->(" + std::to_string(d.left) + "," + std::to_string(d.top) + ")");
    }
    void BltTiled(gz::IDrawTargetBuffer*, const cRZRect&, const cRZRect&, int, int) override {}
    void Flush() override {}
    bool IsSuspended() override { return false; }
    void Suspend() override { Log("Suspend"); }
    void Resume() override { Log("Resume"); }
    void Draw(gz::eDrawingOp, const float*, int) override {}
    void DrawNoTransform(gz::eDrawingOp, const float*, int) override {}
    bool Lock(gz::cGZBufferSurfaceDesc&, bool) override { return false; }
    void Unlock() override {}
};

struct FakeDrawTarget final : cIGZBufferDrawTarget {
    FakeCtx* ctx;
    explicit FakeDrawTarget(FakeCtx* c) : ctx(c) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    cIGZBuffer* AsIGZBuffer() override { return nullptr; }
    gz::cIGZDrawContext* GetDrawContext() override { return ctx; }
    gz::cIGZDrawContext* LockContext() override { Log("LockContext"); return ctx; }
    void UnlockContext() override { Log("UnlockContext"); }
    void Init(gz::cIGZDrawContext*, cIGZBuffer*) override {}
    void ResetSize() override {}
    void SetBlitSource(cIGZBuffer*) override {}
};

struct FakeBuffer final : IWinBuffer {
    int refs = 1;
    cRZRect bounds{0, 0, 0, 0};
    bool ready = true, initOk = true, transparency = false, locked = false, lockOk = true;
    std::uint32_t transparentColor = 0, keyColor = 0;
    int shutdowns = 0, fadeAlpha = -1;
    std::set<std::pair<int, int>> transparentPixels;
    FakeDrawTarget* drawTarget = nullptr;
    bool hasShade = false;
    std::uint8_t sr = 0, sg = 0, sb = 0, sa = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    const cRZRect* GetBounds() const override { return &bounds; }
    std::uint32_t MakeNativeColor(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return drawTarget ? drawTarget->ctx : nullptr; }
    bool SetFadeAlpha(std::uint8_t a) override { fadeAlpha = a; return true; }
    bool Init(int w, int h) override { Log("Init" + std::to_string(w) + "x" + std::to_string(h)); bounds = {0, 0, w, h}; return initOk; }
    void Shutdown() override { ++shutdowns; Log("Shutdown"); }
    bool Lock(std::uint32_t f) override { char b[32]; std::snprintf(b, sizeof b, "Lock%x", f); Log(b); locked = lockOk; return lockOk; }
    void Unlock(std::uint32_t f) override { char b[32]; std::snprintf(b, sizeof b, "Unlock%x", f); Log(b); locked = false; }
    bool IsReady() override { return ready; }
    bool Fill(std::uint32_t c, const cRZRect& r) override { char b[64]; std::snprintf(b, sizeof b, "Fill%x(%d,%d,%d,%d)", c, r.left, r.top, r.right, r.bottom); Log(b); return true; }
    void SetTransparency(std::uint32_t c) override { keyColor = c; transparency = true; Log("SetTransparency"); }
    bool GetTransparency() override { return transparency; }
    bool IsPixelTransparent(int x, int y) override { return transparentPixels.count({x, y}) != 0; }
    bool GetTransparentColor(std::uint32_t& c) override { if (!transparency) return false; c = transparentColor; return true; }
    void RemoveTransparency() override { transparency = false; Log("RemoveTransparency"); }
    bool Blt(IWinBuffer* src, const cRZRect& s, const cRZRect& d) override {
        Log(std::string("BufBlt") + (src ? "src" : "null") + "(" + std::to_string(s.left) + "," + std::to_string(s.top) + "," + std::to_string(s.right) + "," +
            std::to_string(s.bottom) + ")->(" + std::to_string(d.left) + "," + std::to_string(d.top) + "," + std::to_string(d.right) + "," + std::to_string(d.bottom) + ")");
        return true;
    }
    bool GetShadeColor(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a) override {
        if (!hasShade) return false;
        r = sr; g = sg; b = sb; a = sa; return true;
    }
    cIGZBufferDrawTarget* QueryDrawTarget() override { return drawTarget; }
    gz::IDrawTargetBuffer* AsDrawSource() override { return reinterpret_cast<gz::IDrawTargetBuffer*>(this); }
};

struct FakeGfx final : IWinGraphics {
    bool drawSystem = false;
    std::vector<FakeBuffer*> created;
    std::vector<FakeBuffer*> retired;   // buffers of finished blocks, freed with the fake
    void Retire() { retired.insert(retired.end(), created.begin(), created.end()); created.clear(); }
    ~FakeGfx() { Retire(); for (FakeBuffer* b : retired) delete b; }
    FakeBuffer* primary = nullptr;
    bool initOk = true, keyAvailable = true;
    std::uint32_t MakeNativeColor(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    IWinBuffer* PrimaryBuffer() override { return primary; }
    bool DrawSystemActive() override { return drawSystem; }
    IWinBuffer* DrawSystemCreateBuffer(int w, int h) override {
        auto* b = new FakeBuffer;
        b->bounds = {0, 0, w, h};
        created.push_back(b);
        Log("DSCreate" + std::to_string(w) + "x" + std::to_string(h));
        return b;
    }
    IWinBuffer* GraphicsCreateBuffer() override {
        auto* b = new FakeBuffer;
        b->initOk = initOk;
        created.push_back(b);
        Log("GSCreate");
        return b;
    }
    bool GetPrivateBufferKeyColor(std::uint32_t& c) override { if (!keyAvailable) return false; c = 0x00ff00ff; return true; }
};

std::uint32_t g_now = 0;
std::uint32_t Clock() { return g_now; }

struct TestWin : cGZWin {
    using cGZWin::cGZWin;
    int plotted = 0;
    int painted = 0;
    bool Plot() override { ++plotted; Log("plot" + std::to_string(GetID())); return true; }
    bool GZPaint() override { ++painted; Log("paint" + std::to_string(GetID())); return true; }
    using cGZWin::fade_advance;
    using cGZWin::fade_in_progress;
    using cGZWin::fill_private_buffer_with_transparent_color;
    using cGZWin::set_fade_state;
    using cGZWin::set_private_buffer_transparency;
    void InjectBuffer(IWinBuffer* b) { privateBuffer_ = b; }
    IWinBuffer* Buffer() { return privateBuffer_; }
    void SetInvalid(bool v) { invalid_ = v; }
    bool Invalid() const { return invalid_; }
    void SetBlt(const cRZRect& r) { bltArea_ = r; }
    void SetDraw(const cRZRect& r) { drawArea_ = r; }
    void SetAccum(const cRZRect& r) { accumulatedArea_ = r; }
    void SetCtx(void* c) { drawContext_ = c; }
    void SetDrawBuffer(IWinBuffer* b) { drawBuffer_ = b; }
};

struct Filter final : cIGZWinMessageFilter {
    int refs = 1; bool consume = false; int seen = 0; std::uint32_t lastType = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cIGZWin*, cGZMessage& m) override { ++seen; lastType = m.type; return consume; }
};

// Releases a root window made with AddRef: removing a child drops the list's reference (the last one for these
// windows), so the root can then be destroyed without the fake manager having to.
void Dispose(cGZWin* root) {
    std::vector<cIGZWin*> kids;
    root->EnumChildren(kGZIID_cIGZWin, [](cIGZWin*, std::uint32_t, void* child, void* ctx) {
        static_cast<std::vector<cIGZWin*>*>(ctx)->push_back(static_cast<cIGZWin*>(child));
        return true;
    }, &kids);
    for (cIGZWin* k : kids) root->ChildRemove(k);
    root->Release();
}

}  // namespace

int main() {
    FakeMgr mgr;
    FakeGfx gfx;
    SetWinGraphics(&gfx);
    SetWinClock(Clock);

    // ---- create_private_buffer ----
    {
        auto* w = new TestWin(&mgr);
        w->SetArea(0, 0, 100, 50);
        g_log.clear();
        CHECK(w->create_private_buffer(100, 50));
        CHECK(gfx.created.size() == 1 && Joined() == "GSCreate;Init100x50;");
        FakeBuffer* b = gfx.created[0];
        CHECK(w->GetPrivateBuffer() == b);
        g_log.clear();
        CHECK(w->create_private_buffer(30, 20));              // existing buffer: shut down, re-initialised in place
        CHECK(Joined() == "Shutdown;Init30x20;" && gfx.created.size() == 1 && b->bounds.right == 30);
        b->initOk = false;
        CHECK(!w->create_private_buffer(10, 10));            // init fails: buffer dropped
        CHECK(w->GetPrivateBuffer() == nullptr && b->refs == 0);
        CHECK(!w->create_private_buffer(0, 0));                // nothing to create
        gfx.initOk = false;
        CHECK(!w->create_private_buffer(10, 10) && w->GetPrivateBuffer() == nullptr);
        gfx.initOk = true;
        // draw-system path: the service makes the buffer
        gfx.drawSystem = true;
        g_log.clear();
        CHECK(w->create_private_buffer(64, 32));
        CHECK(Joined() == "DSCreate64x32;");
        auto* ds = static_cast<FakeBuffer*>(w->GetPrivateBuffer());
        g_log.clear();
        CHECK(w->create_private_buffer(80, 40));
        CHECK(Joined() == "Shutdown;DSCreate80x40;" && ds->refs == 0 && w->GetPrivateBuffer() != ds);
        gfx.drawSystem = false;
        CHECK(!w->create_private_buffer(-5, 0));
        w->Release();
        gfx.Retire();
    }

    // ---- PrivateBuffer enable / disable ----
    {
        auto* root = new TestWin(&mgr);
        root->SetArea(0, 0, 640, 480);
        root->AddRef();
        auto* w = new TestWin(&mgr);
        w->SetArea(10, 10, 110, 60);
        root->ChildAdd(w);
        g_log.clear();
        gfx.Retire();
        CHECK(w->PrivateBuffer(true));
        CHECK(w->GetFlag(kWinPrivateBuffer) && gfx.created.size() == 1);
        FakeBuffer* b = gfx.created[0];
        CHECK(b->bounds.right == 100 && b->bounds.bottom == 50);
        CHECK(w->GetBufferToDrawTo() == b);                    // draws into its own buffer now
        CHECK(w->GetAreaToDrawTo().right == 100 && w->GetAreaToDrawTo().left == 0);
        CHECK(w->PrivateBuffer(true) && gfx.created.size() == 1);   // right size already
        w->SetArea(10, 10, 210, 60);                           // resize recreates (Shutdown + Init in place)
        CHECK(gfx.created.size() == 1 && b->bounds.right == 200);
        // transparent windows key the buffer on creation
        w->SetFlag(kWinPrivateBufferTransparent, true);
        w->PrivateBuffer(false);
        CHECK(w->GetPrivateBuffer() == nullptr && b->refs == 0 && b->shutdowns >= 2);
        w->SetFlag(kWinPrivateBufferTransparent, true);
        CHECK(w->PrivateBuffer(true));
        FakeBuffer* b2 = static_cast<FakeBuffer*>(w->GetPrivateBuffer());
        CHECK(b2->transparency && b2->keyColor == 0x00ff00ff);
        // a buffer that is not ready is not shut down
        b2->ready = false;
        int before = b2->shutdowns;
        CHECK(w->PrivateBuffer(false) && b2->shutdowns == before && b2->refs == 0);
        // invisible ancestor: reports success, makes no buffer, drops an existing one
        gfx.Retire();
        root->SetFlag(kWinVisible, false);
        CHECK(w->PrivateBuffer(true));
        CHECK(w->GetPrivateBuffer() == nullptr && gfx.created.empty());
        root->SetFlag(kWinVisible, true);
        // failing creation reports failure but keeps the flag
        w->PrivateBuffer(false);
        gfx.initOk = false;
        w->SetFlag(kWinPrivateBufferTransparent, false);
        CHECK(!w->PrivateBuffer(true) && w->GetFlag(kWinPrivateBuffer));
        gfx.initOk = true;
        gfx.Retire();
        Dispose(root);
    }

    // ---- transparency helpers ----
    {
        auto* w = new TestWin(&mgr);
        w->SetArea(0, 0, 20, 10);
        CHECK(!w->set_private_buffer_transparency(true));      // no buffer
        auto* b = new FakeBuffer;
        b->bounds = {0, 0, 20, 10};
        w->InjectBuffer(b);
        CHECK(w->set_private_buffer_transparency(true) && b->transparency && b->keyColor == 0x00ff00ff);
        gfx.keyAvailable = false;
        CHECK(!w->set_private_buffer_transparency(true));      // window class manager missing
        gfx.keyAvailable = true;
        CHECK(w->set_private_buffer_transparency(false) && !b->transparency);

        // hit testing
        b->transparentPixels.insert({3, 4});
        CHECK(!w->is_point_transparent(3, 4));                 // no colour key: opaque everywhere
        b->transparency = true;
        g_log.clear();
        CHECK(w->is_point_transparent(3, 4) && !w->is_point_transparent(5, 5));
        CHECK(Joined() == "Lock800;Unlock800;Lock800;Unlock800;");
        b->lockOk = false;
        CHECK(!w->is_point_transparent(3, 4));
        b->lockOk = true;

        // fill with the transparent colour
        b->transparentColor = 0x123456;
        w->SetDraw({1, 2, 18, 9});
        g_log.clear();
        CHECK(w->fill_private_buffer_with_transparent_color());
        CHECK(Joined() == "Lock8001;Fill123456(1,2,18,9);Unlock8001;");
        b->transparency = false;
        g_log.clear();
        CHECK(w->fill_private_buffer_with_transparent_color());
        CHECK(Joined() == "Lock8001;Unlock8001;");
        b->lockOk = false;
        CHECK(!w->fill_private_buffer_with_transparent_color());
        w->InjectBuffer(nullptr);
        CHECK(!w->fill_private_buffer_with_transparent_color());
        b->Release();
        w->Release();
        delete b;
    }

    // ---- fade effect ----
    {
        auto* w = new TestWin(&mgr);
        w->SetArea(0, 0, 20, 10);
        auto* b = new FakeBuffer;
        w->InjectBuffer(b);
        w->SetFlag(kWinFade, true);                              // without a private buffer flag the fade state is not kept
        CHECK(!w->fade_in_progress());
        w->SetFlag(0x10000, true);                               // plain flag bit (no buffer work: ancestors absent)
        g_now = 1000;
        w->set_fade_state(0, 500);                               // fade in over 500 ms starting now
        CHECK(w->fade_in_progress() && b->fadeAlpha == 0);
        g_now = 1250;
        w->fade_advance();                                       // 250 ms of 500: alpha 127.5 -> 127
        CHECK(b->fadeAlpha == 127 && w->fade_in_progress());
        g_now = 1400;
        w->fade_advance();                                       // 204
        CHECK(b->fadeAlpha == 204);
        g_now = 1600;
        w->fade_advance();                                       // past the end: clamps to 255 and stops
        CHECK(b->fadeAlpha == 255 && !w->fade_in_progress());
        w->fade_advance();                                       // period 0: no more changes
        CHECK(b->fadeAlpha == 255);
        // fade out: negative period
        g_now = 2000;
        w->set_fade_state(0xff, -400);
        g_now = 2100;
        w->fade_advance();                                       // 255 - 100*255/400 = 191.25
        CHECK(b->fadeAlpha == 191);
        g_now = 2500;
        w->fade_advance();                                       // below 0: alpha 0 and the fade ends
        CHECK(b->fadeAlpha == 0 && !w->fade_in_progress());
        b->Release();
        w->InjectBuffer(nullptr);
        w->Init();   // so the destructor's Shutdown frees the fade parameters the window stores
        w->Release();
        delete b;
    }

    // ---- fade-out hands the buffer to a temporary window ----
    {
        auto* root = new TestWin(&mgr);
        root->SetArea(0, 0, 640, 480);
        root->AddRef();
        auto* w = new TestWin(&mgr);
        w->SetID(7);
        w->SetArea(40, 50, 140, 90);
        root->ChildAdd(w);
        w->SetFadeEffectPeriod(300, 600);
        w->SetFlag(kWinFade, true);
        CHECK(w->PrivateBuffer(true));
        FakeBuffer* b = static_cast<FakeBuffer*>(w->GetPrivateBuffer());
        b->refs = 1;
        w->SetFlag(0x400000, true);                              // "was drawn" marker set by PlotComposite
        const std::uint32_t kids = root->GetChildCount();
        int shutdownsBefore = b->shutdowns;
        g_now = 5000;
        CHECK(w->PrivateBuffer(false));
        CHECK(root->GetChildCount() == kids + 1);                // the temporary window
        CHECK(w->GetPrivateBuffer() == nullptr);
        CHECK(b->shutdowns == shutdownsBefore);                  // not shut down: the tmp window owns it
        CHECK(b->refs == 1);                                     // +1 for the thief, -1 for the window
        CHECK(!w->GetFlag(0x400000));
        cIGZWin* tmp = nullptr;
        root->EnumChildren(kGZIID_cIGZWin, [](cIGZWin* , std::uint32_t id, void* child, void* ctx) {
            if (id != 7) *static_cast<cIGZWin**>(ctx) = static_cast<cIGZWin*>(child);
            return true;
        }, &tmp);
        CHECK(tmp != nullptr && tmp->GetPrivateBuffer() == b);
        if (tmp) {
            CHECK(tmp->GetL() == 40 && tmp->GetT() == 50 && tmp->GetW() == 100 && tmp->GetH() == 40);
            CHECK(tmp->GetFlag(kWinFlag4) && tmp->GetFlag(kWinFade) && tmp->GetFlag(kWinPrivateBuffer));
            CHECK(tmp->GetFlag(kWinPrivateBufferTransparent) && tmp->GetFlag(kWinFlag4));   // 0x230021 forced on
            CHECK(static_cast<cGZWin*>(tmp)->GetBufferToDrawTo() == b);
            cIGZVariant* v = nullptr;
            CHECK(tmp->GetParam(0xe6e998fe, &v));                // marker
            if (v) v->Release();
            CHECK(tmp->GetParam(0xe6e998fd, &v) && v && v->AsSint32() == -600);   // fades out over the 600 ms
            if (v) v->Release();
            CHECK(tmp->GetParam(0xe6e998fb, &v) && v && v->AsUint8() == 0xff);
            if (v) v->Release();
            CHECK(b->fadeAlpha == 255);
            // while fading it plots (here: the base Plot, then invalidates); afterwards it is destroyed
            CHECK(tmp->Plot());
            CHECK(mgr.destroyed.empty());
            g_now = 5000 + 700;
            CHECK(tmp->Plot());                                  // this plot ends the fade (clamps to 0)
            CHECK(b->fadeAlpha == 0 && mgr.destroyed.empty());
            CHECK(tmp->Plot());                                  // fade over: asks the manager to destroy it
            CHECK(mgr.destroyed.size() == 1 && mgr.destroyed[0] == tmp);
        }
        gfx.Retire();
        Dispose(root);
    }

    // ---- fade-out without a parent attaches to the main window ----
    {
        auto* main = new TestWin(&mgr);
        main->SetArea(0, 0, 640, 480);
        main->AddRef();
        mgr.mainWin = main;
        auto* w = new TestWin(&mgr);
        w->SetArea(5, 6, 55, 46);
        w->SetFlag(kWinFade, true);
        w->SetFlag(kWinPrivateBuffer, true);
        auto* b = new FakeBuffer;
        b->bounds = {0, 0, 50, 40};
        w->InjectBuffer(b);
        w->SetFlag(0x400000, true);
        w->SetFlag(kWinPrivateBuffer, false);   // PrivateBuffer(false) through the flag path
        CHECK(main->GetChildCount() == 1);
        mgr.mainWin = nullptr;
        cIGZWin* fadeTmp = nullptr;   // the temporary fade window ended up as the main window's child
        main->EnumChildren(kGZIID_cIGZWin, [](cIGZWin*, std::uint32_t, void* child, void* ctx) {
            *static_cast<cIGZWin**>(ctx) = static_cast<cIGZWin*>(child);
            return true;
        }, &fadeTmp);
        if (fadeTmp) main->ChildRemove(fadeTmp);   // the list held its last reference
        w->Release();
        main->Release();
        delete b;
    }

    // ---- PlotComposite ----
    {
        auto* root = new TestWin(&mgr);
        root->SetArea(0, 0, 640, 480);
        root->AddRef();
        auto* c1 = new TestWin(&mgr); c1->SetID(1); c1->SetArea(0, 0, 100, 100);
        auto* c2 = new TestWin(&mgr); c2->SetID(2); c2->SetArea(200, 0, 300, 100);
        auto* c3 = new TestWin(&mgr); c3->SetID(3); c3->SetArea(50, 50, 150, 150);
        auto* c4 = new TestWin(&mgr); c4->SetID(4); c4->SetArea(400, 400, 500, 500);
        root->ChildAdd(c1); root->ChildAdd(c2); root->ChildAdd(c3); root->ChildAdd(c4);   // front to back: 4 3 2 1
        c4->SetFlag(kWinVisible, false);
        // plots back to front
        g_log.clear();
        CHECK(root->PlotComposite());
        CHECK(Joined() == "paint0;plot1;plot2;plot3;");
        CHECK(root->painted == 1 && !root->Invalid() == false);   // no private buffer: still invalid
        // an invisible window does nothing
        root->SetFlag(kWinVisible, false);
        g_log.clear();
        CHECK(!root->PlotComposite() && g_log.empty());
        root->SetFlag(kWinVisible, true);
        // a delayed-plot (desktop) child pulls overlapping siblings into the manager's plot strategy
        c1->SetFlag(kWinDesktop, true);          // the back-most child is plotted first
        g_log.clear();
        mgr.plotStrategy.clear();
        CHECK(root->PlotComposite());
        // c1 (desktop) deferred; c2 does not overlap c1 -> plotted; c3 (50..150) overlaps c1 -> deferred
        CHECK(Joined() == "paint0;plot2;");
        CHECK(mgr.plotStrategy.size() == 2 && mgr.plotStrategy[0] == c1 && mgr.plotStrategy[1] == c3);
        CHECK(c1->GetFlag(kWinPlotted) && c3->GetFlag(kWinPlotted) && !c2->GetFlag(kWinPlotted));
        c1->SetFlag(kWinDesktop, false);
        c1->SetFlag(kWinPlotted, false);
        c3->SetFlag(kWinPlotted, false);
        // a desktop window defers every child
        root->SetFlag(kWinDesktop, true);
        mgr.plotStrategy.clear();
        g_log.clear();
        CHECK(root->PlotComposite());
        CHECK(Joined() == "paint0;" && mgr.plotStrategy.size() == 4);   // invisible c4 is deferred too (no visibility test)
        root->SetFlag(kWinDesktop, false);
        for (auto* c : {c1, c2, c3, c4}) c->SetFlag(kWinPlotted, false);
        // filters see the paint message; a consuming filter stops the paint
        Filter f;
        root->AddMessageFilter(&f);
        g_log.clear();
        CHECK(root->PlotComposite());
        CHECK(f.seen == 1 && f.lastType == 0x16 && Joined() == "paint0;plot1;plot2;plot3;");
        f.consume = true;
        g_log.clear();
        CHECK(root->PlotComposite() && g_log.empty());
        f.consume = false;
        // not invalid: only the delayed plot windows are collected
        root->SetInvalid(false);
        g_log.clear();
        mgr.plotStrategy.clear();
        CHECK(root->PlotComposite() && g_log.empty());
        root->RemoveMessageFilter(&f);
        root->SetInvalid(true);

        // private buffer: validity is cleared (child of a parent) and the draw context bracket is emitted
        FakeCtx ctx;
        root->SetCtx(&ctx);
        root->SetAccum({10, 20, 110, 70});
        root->SetFlag(kWinFlag4, true);
        g_log.clear();
        CHECK(root->PlotComposite());
        CHECK(Joined() == "ctx.AddRef;PushTransform;OffsetAbsolute10,20;ClipAbsolute10,20;ClipReset;Depth;PushState;Reset;Alpha1;Modulate1;Color"
                          "ffffffff;Resume;paint0;Suspend;PopState;PopTransform;ctx.Release;plot1;plot2;plot3;");
        // shade colour only without a private buffer
        root->SetFlag(kWinUseShadeColor, true);
        root->SetShadeColor(0x80402010);
        g_log.clear();
        root->PlotComposite();
        CHECK(Joined().find("Color80402010;") != std::string::npos);
        root->SetCtx(nullptr);

        // with a private buffer: filled with the key colour, marked drawn, parent's invalid flag cleared
        auto* kid = new TestWin(&mgr);
        kid->SetID(20);
        kid->SetArea(0, 0, 10, 10);
        root->ChildAdd(kid);
        auto* kb = new FakeBuffer;
        kb->transparency = true; kb->transparentColor = 0xabcdef;
        kid->InjectBuffer(kb);
        kid->SetFlag(kWinPrivateBufferTransparent, true);
        kid->SetFlag(0x10000, true);
        kid->SetFlag(0x40000, true);     // the fill needs both 0x20000 and 0x40000 (flags & 0x60000 == 0x60000)
        kid->SetDraw({0, 0, 10, 10});
        g_log.clear();
        CHECK(kid->PlotComposite());
        CHECK(!kid->Invalid() && kid->GetFlag(0x400000));
        if (Joined() != "Lock8001;Fillabcdef(0,0,10,10);Unlock8001;paint20;") std::printf("got %s\n", Joined().c_str());
        CHECK(Joined() == "Lock8001;Fillabcdef(0,0,10,10);Unlock8001;paint20;");
        kb->Release();
        kid->InjectBuffer(nullptr);
        Dispose(root);
        delete kb;
    }

    // ---- PlotPresent ----
    {
        auto* root = new TestWin(&mgr);
        root->SetArea(0, 0, 640, 480);
        root->AddRef();
        auto* w = new TestWin(&mgr);
        w->SetArea(30, 40, 130, 90);
        root->ChildAdd(w);
        auto* own = new FakeBuffer;
        w->InjectBuffer(own);
        w->SetBlt({30, 40, 130, 90});
        w->SetDraw({0, 0, 100, 50});
        auto* screen = new FakeBuffer;
        gfx.primary = screen;
        root->SetDrawBuffer(screen);
        // no private buffer: nothing to do
        auto* plain = new TestWin(&mgr);
        CHECK(plain->PlotPresent());
        // plain locked blit into the parent's draw buffer
        g_log.clear();
        CHECK(w->PlotPresent());
        CHECK(Joined() == "Lock8010;BufBltsrc(0,0,100,50)->(30,40,130,90);Unlock8010;");
        // plotted-later windows blit at their absolute position into the screen
        w->SetFlag(kWinPlotted, true);
        g_log.clear();
        CHECK(w->PlotPresent());
        CHECK(Joined() == "Lock8010;BufBltsrc(0,0,100,50)->(30,40,130,90);Unlock8010;" && !w->GetFlag(kWinPlotted));
        // through a draw context
        FakeCtx ctx;
        FakeDrawTarget target(&ctx);
        screen->drawTarget = &target;
        g_log.clear();
        CHECK(w->PlotPresent());
        CHECK(Joined() == "LockContext;ClipReset;OffsetAbsolute0,0;PushState;Alpha0;Modulate0;Colorffffffff;Bltsrc(0,0,100,50)->(30,40);PopState;UnlockContext;");
        w->SetFlag(kWinFlag4, true);
        w->SetFlag(kWinUseShadeColor, true);
        w->SetShadeColor(0xff102030);
        own->hasShade = true; own->sr = 1; own->sg = 2; own->sb = 3; own->sa = 0x40;
        g_log.clear();
        w->PlotPresent();
        CHECK(Joined().find("Alpha1;Modulate1;Colorff102030;Color40102030;") != std::string::npos);
        w->SetFlag(kWinUseShadeColor, false);
        g_log.clear();
        w->PlotPresent();
        CHECK(Joined().find("Colorffffffff;Color40010203;") != std::string::npos);
        // Plot = composite then present, then the manager gets its turn (no own buffer to draw to)
        g_log.clear();
        int plots = mgr.plots;
        w->Plot();
        CHECK(Joined().find("paint0;") == std::string::npos);   // TestWin::Plot is overridden: sanity only
        (void)plots;
        screen->drawTarget = nullptr;
        own->Release(); screen->Release();
        plain->Release();
        Dispose(root);
        gfx.primary = nullptr;
        delete own; delete screen;
    }

    SetWinGraphics(nullptr);
    SetWinClock(nullptr);
    if (failures == 0) std::printf("ok\n");
    return failures == 0 ? 0 : 1;
}
