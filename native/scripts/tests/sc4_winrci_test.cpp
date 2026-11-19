// cSC4WinRCI: the demand-channel sums, the change notification and the bar rectangles. The expected rectangles are the
// original's arithmetic (single-precision log ratio, double-precision scaling, truncation) worked out by hand below.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "gz/DrawContext.h"
#include "gz/RZDraw.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/DemandSimulator.h"
#include "sc4/WinRCI.h"
#include "sc4/cSC4Demand.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
using namespace gz;

namespace {
struct Ctx final : cIGZDrawContext {
    std::vector<std::string> log;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void PushTransform() override {} void ClipReset() override {} void Clip(const cRZRect&) override {}
    void ClipAbsolute(const cRZRect&) override {} void Offset(const cRZPoint&) override {}
    void OffsetAbsolute(const cRZPoint&) override {} void PopTransform() override {} void PushState() override {}
    void PopState() override {} cRZRect GetCanvasArea() override { return {}; } cRZRect GetClipRect() override { return {}; }
    cRZPoint GetOrigin() override { return {}; } int GetTransformStackDepth() override { return 0; }
    bool IsTargetAttached() override { return true; } void SetTarget(IDrawTargetBuffer*) override {}
    cIGZDrawEngine* GetEngine() override { return nullptr; } void ResetToDefaultState() override {}
    void SetColor(std::uint32_t rgb) override { log.push_back("color " + std::to_string(rgb)); }
    void SetColor(std::uint32_t, std::uint32_t) override {} void SetAlphaBlend(int) override {}
    void SetBltModulate(bool) override {} void SetAntialiasing(bool) override {} void SetLineThickness(float) override {}
    void ClearGradient() override {} void SetGradientTransform(const float (*)[4], float, float) override {}
    void ClearBlt() override {} void SetBlt(cIGZUnknown*, float, float, float, float) override {}
    void ClearBltTiling() override {} void SetBltTiling(const cRZRect&) override {} void Clear() override {}
    void Point(int, int) override {} void Line(int, int, int, int) override {}
    void Fill(const cRZRect& r) override { log.push_back("fill " + std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) + "," + std::to_string(r.bottom)); }
    void Ellipse(int, int, int, int) override {} void FillEllipse(int, int, int, int) override {}
    void Blt(IDrawTargetBuffer*, const cRZRect&, const cRZRect&) override {}
    void BltTiled(IDrawTargetBuffer*, const cRZRect&, const cRZRect&, int, int) override {}
    void Flush() override {} bool IsSuspended() override { return false; } void Suspend() override {} void Resume() override {}
    void Draw(eDrawingOp, const float*, int) override {} void DrawNoTransform(eDrawingOp, const float*, int) override {}
    bool Lock(cGZBufferSurfaceDesc&, bool) override { return true; } void Unlock() override {}
};

struct Bar final : cSC4WinRCI {
    using cSC4WinRCI::cSC4WinRCI;
    void SetCtx(cIGZDrawContext* c) { drawContext_ = c; }
};

struct Sim final : cISC4DemandSimulator {
    std::vector<std::pair<std::uint32_t, cSC4Demand*>> demands;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> asked;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    std::uint32_t GetSimulatorType() override { return 0; }
    cSC4Demand* GetDemand(std::uint32_t id, std::uint32_t offset) override {
        asked.push_back({id, offset});
        for (auto& d : demands) if (d.first == id) return d.second;
        return nullptr;
    }
    void UpdateOccupantEffects(std::uint32_t, const SC4Percentage&, const SC4Percentage&) override {}
    void CalculateJobsPerUnitOfDemand(float*, std::uint32_t, std::uint32_t) override {}
    std::int64_t GetJobsBySensus(std::uint32_t) override { return 0; }
    float GetNeutralTaxRate() override { return 0; }
    void GetLocalPopulationSummary(std::map<std::uint32_t, int>&) const override {}
    bool AddDemandToSimulator(cSC4Demand*) override { return true; }
};

struct Server final : cIGZMessageServer2 {
    std::vector<std::string> log;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t t) override { log.push_back("add " + std::to_string(t)); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t t) override { log.push_back("remove " + std::to_string(t)); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct Host final : IWinRCIHost {
    Sim* sim = nullptr;
    Server* server = nullptr;
    cISC4DemandSimulator* DemandSimulator() override { return sim; }
    cIGZMessageServer2* MessageServer() override { return server; }
};

void MakeDemand(cSC4Demand& d, float min, float max, float value) {
    d.SetActiveDemandMax(max);
    d.SetActiveDemandMin(min);
    d.SetDemandValue(value);
    d.CalculateActiveDemand(false);   // economy, tax and cap are 0: the value is demand clamped to [min, max]
}

// The original's arithmetic for one rectangle edge, written independently of the port's code.
int Edge(float ratio, int middle, bool plusOne, bool subtract) {
    const double half = middle * 0.5;
    const double scaled = static_cast<double>(ratio) * half + 0.5;
    return subtract ? static_cast<int>(static_cast<double>(middle) - scaled)
                    : static_cast<int>(scaled + static_cast<double>(middle + (plusOne ? 1 : 0)));
}
}  // namespace

int main() {
    FakeMgr mgr; FakeWinGraphics graphics; Ctx ctx;
    SetWinGraphics(&graphics);
    Sim sim; Server server; Host host;
    host.sim = &sim; host.server = &server;
    SetWinRCIHost(&host);

    cSC4Demand a, b, c;
    MakeDemand(a, -600, 1000, 300);    // active 300
    MakeDemand(b, -200, 500, 150);     // active 150
    MakeDemand(c, -100, 100, -60);     // active -60
    CHECK(a.QueryActiveDemandValue() == 300.0f && c.QueryActiveDemandValue() == -60.0f);
    sim.demands = {{0x1101, &a}, {0x1102, &b}, {0x1103, &c}};

    auto* bar = new Bar(&mgr);
    bar->AddRef();
    bar->SetArea(0, 0, 100, 20);

    // ---- views
    void* p = nullptr;
    CHECK(bar->QueryInterface(1, &p) && p == static_cast<cISC4WinRCI*>(bar));
    bar->Release();
    CHECK(bar->QueryInterface(kGZIID_cISC4WinRCI, &p) && p == static_cast<cISC4WinRCI*>(bar));
    bar->Release();
    CHECK(bar->QueryInterface(0x22ba0121, &p) && p == static_cast<cIGZWin*>(bar));
    bar->Release();
    CHECK(bar->QueryInterface(kGZIID_cIGZMessageTarget2, &p) && p == static_cast<cIGZMessageTarget2*>(bar));
    bar->Release();
    CHECK(!bar->QueryInterface(0x1234, &p));
    CHECK(bar->AsIGZWin() == static_cast<cIGZWin*>(bar));

    // ---- Init / Shutdown listen for the demand message
    CHECK(bar->Init());
    CHECK(server.log.size() == 1 && server.log[0] == "add " + std::to_string(kWinRCIDemandChangedMessage));
    CHECK(bar->Init() && server.log.size() == 1);   // already initialised
    bar->SetCtx(&ctx);

    // ---- SetDemandType sums the channels (unknown and non-positive ids add nothing)
    const std::uint32_t ids[] = {0x1101, 0x1102, 0, 0x9999};
    bar->SetDemandType(ids, 4);
    CHECK(bar->Count() == 4);
    CHECK(bar->MinimumSum() == -800 && bar->MaximumSum() == 1500 && bar->ValueSum() == 450);
    CHECK(sim.asked.size() >= 3 && sim.asked.back().second == 0x20000);

    // ---- horizontal bar: wider than tall, positive value, limit = the maximum sum
    ctx.log.clear();
    bar->SetColor(0x00ff00);
    CHECK(bar->GZPaint());
    {
        const float ratio = 450.0f / 1500.0f + std::log(450.0f) / std::log(1500.0f);
        const int middle = 50;
        // the port orders the corners (the original fills right-to-left and relies on the window clip)
        const std::string expect = "fill " + std::to_string(middle + 1) + ",0," + std::to_string(Edge(ratio, middle, true, false)) + ",20";
        CHECK(ctx.log.size() == 2 && ctx.log[0] == "color 65280");
        CHECK(ctx.log.size() == 2 && ctx.log[1] == expect);
        if (ctx.log.size() == 2 && ctx.log[1] != expect) std::printf("  got %s want %s\n", ctx.log[1].c_str(), expect.c_str());
    }

    // ---- vertical bar, positive: taller than wide; the bar grows upwards from the middle
    bar->SetArea(0, 0, 20, 100);
    ctx.log.clear();
    CHECK(bar->GZPaint());
    {
        const float ratio = 450.0f / 1500.0f + std::log(450.0f) / std::log(1500.0f);
        const std::string expect = "fill 0," + std::to_string(Edge(ratio, 50, false, true)) + ",20,50";
        CHECK(ctx.log.size() == 2 && ctx.log[1] == expect);
        if (ctx.log.size() == 2 && ctx.log[1] != expect) std::printf("  got %s want %s\n", ctx.log[1].c_str(), expect.c_str());
    }

    // ---- negative value (channel 3 only): limit = the minimum sum, grows downwards / to the left
    const std::uint32_t negative[] = {0x1103};
    bar->SetDemandType(negative, 1);
    CHECK(bar->MinimumSum() == -100 && bar->MaximumSum() == 100 && bar->ValueSum() == -60);
    ctx.log.clear();
    CHECK(bar->GZPaint());
    {
        const float ratio = -60.0f / -100.0f + std::log(60.0f) / std::log(100.0f);
        const std::string expect = "fill 0," + std::to_string(51) + ",20," + std::to_string(Edge(ratio, 50, true, false));
        CHECK(ctx.log.size() == 2 && ctx.log[1] == expect);
        if (ctx.log.size() == 2 && ctx.log[1] != expect) std::printf("  got %s want %s\n", ctx.log[1].c_str(), expect.c_str());
    }
    bar->SetArea(0, 0, 100, 20);
    ctx.log.clear();
    CHECK(bar->GZPaint());
    {
        const float ratio = -60.0f / -100.0f + std::log(60.0f) / std::log(100.0f);
        const std::string expect = "fill " + std::to_string(Edge(ratio, 50, false, true)) + ",0,50,20";
        CHECK(ctx.log.size() == 2 && ctx.log[1] == expect);
        if (ctx.log.size() == 2 && ctx.log[1] != expect) std::printf("  got %s want %s\n", ctx.log[1].c_str(), expect.c_str());
    }

    // ---- a value of 0 sets the colour and draws nothing; a square window takes the vertical branch
    bar->SetDemandType(nullptr, 0);
    ctx.log.clear();
    CHECK(bar->GZPaint() && ctx.log.size() == 1 && ctx.log[0] == "color 65280");

    // ---- the demand-changed message: only a listed id repaints, and only when a sum changed
    bar->SetDemandType(ids, 2);
    mgr.valid.insert(bar);
    cRZMessage2Standard message;
    message.SetType(kWinRCIDemandChangedMessage);
    message.SetData1(0x1101);
    a.SetDemandValue(400);
    a.CalculateActiveDemand(false);
    CHECK(!bar->DoMessage(&message));
    CHECK(bar->ValueSum() == 550);
    message.SetData1(0x7777);   // not one of ours: nothing is recomputed
    b.SetDemandValue(0);
    b.CalculateActiveDemand(false);
    CHECK(!bar->DoMessage(&message) && bar->ValueSum() == 550);
    message.SetType(0x1);
    message.SetData1(0x1102);
    CHECK(!bar->DoMessage(&message) && bar->ValueSum() == 550);

    // ---- RethinkBar without a demand simulator reports no change and keeps the sums
    host.sim = nullptr;
    CHECK(!bar->RethinkBar() && bar->ValueSum() == 550);
    host.sim = &sim;

    // ---- an over-long channel list keeps the 16 that fit (WINRCI-1)
    std::uint32_t many[20];
    for (auto& v : many) v = 0x1101;
    bar->SetDemandType(many, 20);
    CHECK(bar->Count() == 16);

    // ---- Shutdown stops listening
    server.log.clear();
    CHECK(bar->Shutdown());
    CHECK(server.log.size() == 1 && server.log[0] == "remove " + std::to_string(kWinRCIDemandChangedMessage));

    SetWinRCIHost(nullptr);
    bar->Release();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("sc4_winrci: ok\n");
    return 0;
}
