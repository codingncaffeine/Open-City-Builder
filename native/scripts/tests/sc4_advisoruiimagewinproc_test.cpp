// cSC4AdvisorUIImageWinProc: the mood mapping, the head models chosen by mood (and the llama fallback), the tick
// registration, the loop / switch rule of the animation tick and the head placement. The paint path needs the renderer
// and the driver; the host answers null for both, so only its placement half runs here.
#include <cstdio>
#include <set>
#include <vector>

#include "s3d/ModelInstance.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorUIImageWinProc.h"
#include "sc4/DrawContext.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { \
    std::printf("FAIL line %d: %s != %s\n", __LINE__, #a, #b); ++failures; } } while (0)

// The paint path's draw-context calls (DrawContext.cpp is not linked: the host gives no renderer, so they never run).
void SC4DrawContext::SetCamera(s3d::cS3DCamera*) {}
void SC4DrawContext::SetDefaultRenderStateUnilaterally() {}
void SC4DrawContext::SetDepthOffset(int) {}

namespace {

using namespace sc4;

// A head animation: `frames` frames, one frame per tick.
struct FakeModel final : s3d::cIS3DModelInstanceAnimated {
    explicit FakeModel(std::uint32_t id, int frames) : id(id), frames(frames) {}
    std::uint32_t id;
    int frames;
    int frame = 5;
    int refs = 1;
    bool selfLit = false;
    cS3DVector3 position{};
    cS3DTransform transform{};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(refs > 0 ? --refs : 0); }   // the test owns the objects
    cIGZUnknown* GetOwner() override { return nullptr; }
    void SetOwner(cIGZUnknown*) override {}
    void* GetCustomData() override { return nullptr; }
    void SetCustomData(void*) override {}
    void GetBounds(cS3DBoundingBox&) override {}
    void GetWorldBounds(cS3DBoundingBox&) override {}
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& t) override { transform = t; }
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { return 1; }
    int GetProperties() override { return 0; }
    void SetProperties(int) override {}
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return {}; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes&) override {}
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
    s3d::cIS3DModelInstance* AsModelInstance() override { return this; }
    int SetPosition(const cS3DVector3& p) override { position = p; return 1; }
    int SetOrientation(const cS3DVector3&) override { return 1; }
    int SetOrientation(const cS3DVector3&, const cS3DVector3&) override { return 1; }
    void ModelPointToWorldPoint(cS3DVector3&) override {}
    void ModelPointToWorldPoint(float*) override {}
    void ModelVectorToWorldVector(cS3DVector3&) override {}
    void ModelVectorToWorldVector(float*) override {}
    int SetZoomAndRotation(int, int) override { return 1; }
    int SetFrameset(int) override { return 1; }
    int SetFrame(int f) override { frame = f; return 1; }
    int GetFrame() const override { return frame; }
    int GetFrameCount() const override { return frames; }
    int IncrementFrame() override { frame = (frame + 1) % frames; return 1; }
    int SetAutoFrameRate(float) override { return 1; }
    int DoAutoFrameUpdate(float) override { frame = (frame + 1) % frames; return 1; }
    void SetPlayMode(int) override {}
    const void* GetAnimInfo() const override { return nullptr; }
    int SetOpacity(bool, float) override { return 1; }
    int SetHighlight(int) override { return 1; }
    int SetSelfIlluminated(bool on) override { selfLit = on; return 1; }
};

struct FakeAdvisor final : cISC4Advisor {
    int refs = 0;
    cRZString dummy_;
    AdviceMap advices;
    std::vector<int> asked;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return std::uint32_t(++refs); }
    std::uint32_t Release() override { return std::uint32_t(refs > 0 ? --refs : 0); }
    int GetID() const override { return 7; }
    cIGZString& GetPersonalName() override { return dummy_; }
    const AdviceMap& GetCurrentAdvices() const override { return advices; }
    AdviceMap& GetCurrentAdvices() override { return advices; }
    bool Init(int) override { return true; }
    bool Shutdown() override { return true; }
    const cIGZString& GetName() const override { return dummy_; }
    void SetName(const cIGZString&) override {}
    void SetPersonalName(const cIGZString&) override {}
    cISC4Advice* GetAdviceByGUID(int) const override { return nullptr; }
    cISC4Advice* GetAdviceBySessionID(int) const override { return nullptr; }
    bool Simulate(int) override { return true; }
    int GetHeadAnimCount(bool) override { return 8; }
    bool SetHeadAnim(bool, int) override { return true; }
    int GetHeadAnimID(int index) override { asked.push_back(index); return 0x1000 + index; }
    bool PushEvent(int, cISC4AdviceSubject*) override { return false; }
    bool CommandWouldTrigger(std::uint32_t) override { return false; }
    bool RetireAdvice(cISC4Advice*) override { return false; }
};

struct Host final : IAdvisorUIImageHost {
    FakeAdvisor advisor;
    bool llama = false;
    std::set<std::uint32_t> missing;               // instances CreateModel answers null for
    std::vector<cGZPersistResourceKey> asked;
    std::vector<FakeModel*> made;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    ~Host() { for (FakeModel* m : made) delete m; }
    cISC4Advisor* Advisor(int id) override { return id == 7 ? &advisor : nullptr; }
    bool LlamaCheatActive() override { return llama; }
    s3d::cIS3DModelInstanceAnimated* CreateModel(const cGZPersistResourceKey& key) override {
        asked.push_back(key);
        if (missing.count(key.instance)) return nullptr;
        auto* m = new FakeModel(key.instance, 3);
        made.push_back(m);
        return m;
    }
    void AddTickable(cIGZMessageTarget2* t, std::uint32_t p) override { added.push_back({t, p}); }
    void RemoveTickable(cIGZMessageTarget2* t, std::uint32_t p) override { removed.push_back({t, p}); }
    bool MainWindowSize(std::uint16_t& w, std::uint16_t& h) override { w = 800; h = 600; return true; }
    cISC43DRender* Renderer() override { return nullptr; }
    gd::cIGZGDriver* Driver() override { return nullptr; }
};

void TestMoods() {
    Host host;
    SetAdvisorUIImageHost(&host);
    auto* proc = new cSC4AdvisorUIImageWinProc(7);
    proc->AddRef();
    // 0 -> 3, 1 -> 2, 3 -> 0, 5 -> 1, anything else unchanged
    proc->ForceMood(0); CHECK_EQ(proc->Mood(), 3);
    proc->ForceMood(1); CHECK_EQ(proc->Mood(), 2);
    proc->ForceMood(3); CHECK_EQ(proc->Mood(), 0);
    proc->ForceMood(5); CHECK_EQ(proc->Mood(), 1);
    proc->ForceMood(2); CHECK_EQ(proc->Mood(), 2);
    proc->ForceMood(4); CHECK_EQ(proc->Mood(), 4);
    CHECK(host.asked.empty());   // disabled: no models are loaded on a mood change
    proc->Release();
    SetAdvisorUIImageHost(nullptr);
}

void TestModelsAndTicks() {
    Host host;
    SetAdvisorUIImageHost(&host);
    auto* proc = new cSC4AdvisorUIImageWinProc(7);
    proc->AddRef();
    proc->ForceMood(1);   // -> mood 2: talking head slot 2, second animation slot 6
    proc->Enable(true);
    CHECK_EQ(host.added.size(), std::size_t(1));
    CHECK(host.added[0].first == static_cast<cIGZMessageTarget2*>(proc) && host.added[0].second == 0xc0u);
    CHECK_EQ(proc->ModelIdA(), 0x1002u);
    CHECK_EQ(proc->ModelIdB(), 0x1006u);
    CHECK_EQ(host.asked.size(), std::size_t(2));
    CHECK(host.asked[0].type == 0x5ad0e817u && host.asked[0].group == 0xbadb57f1u && host.asked[0].instance == 0x1002u);
    CHECK(host.asked[1].instance == 0x1006u);
    auto* a = static_cast<FakeModel*>(proc->ModelA());
    auto* b = static_cast<FakeModel*>(proc->ModelB());
    CHECK(a && b && a->selfLit && b->selfLit);
    CHECK(proc->Current() == proc->ModelA());
    CHECK_EQ(a->frame, 0);   // A starts from its first frame
    proc->Enable(true);      // already enabled: nothing happens
    CHECK_EQ(host.added.size(), std::size_t(1));
    proc->ForceMood(1);      // the same mood: no reload
    CHECK_EQ(host.asked.size(), std::size_t(2));

    // Ticks: A has 3 frames. The first wrap restarts A and draws the loop count from the RNG ([1, 3); the seed 0xffffffff
    // means "from the clock", so either value can come).
    proc->DoAnimationTick(16.0f);   // frame 1
    proc->DoAnimationTick(16.0f);   // frame 2
    CHECK_EQ(proc->LoopTarget(), 0);
    proc->DoAnimationTick(16.0f);   // wraps to 0
    CHECK_EQ(proc->LoopCount(), 1);
    const int target = proc->LoopTarget();
    CHECK(target == 1 || target == 2);
    CHECK(proc->Current() == proc->ModelA());
    // Next wraps count loops; B starts when the count reaches the target (a target of 1 is never reached: A loops on).
    for (int i = 0; i < 3; ++i) proc->DoAnimationTick(16.0f);
    if (target == 2) {
        CHECK(proc->Current() == proc->ModelB());
        CHECK_EQ(proc->LoopCount(), 0);
        CHECK_EQ(b->frame, 0);
        for (int i = 0; i < 3; ++i) proc->DoAnimationTick(16.0f);   // B wraps: back to A with a new count
        CHECK(proc->Current() == proc->ModelA());
        CHECK_EQ(proc->LoopCount(), 1);
    } else {
        CHECK(proc->Current() == proc->ModelA());
        CHECK_EQ(proc->LoopCount(), 2);
    }

    // The placement: (x, y, z) and the uniform scale with its flag; a scale of 1 clears the flag.
    proc->PlaceHead(-50.0f, -4.0f, 0.0f, 102.0f);
    auto* current = static_cast<FakeModel*>(proc->Current());
    CHECK(current->position.x == -50.0f && current->position.y == -4.0f && current->position.z == 0.0f);
    CHECK(current->transform.scale == 102.0f && (current->transform.flags & 2) != 0);
    const std::uint8_t counter = current->transform.changeCounter;
    proc->PlaceHead(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK((current->transform.flags & 2) == 0);
    CHECK_EQ(current->transform.changeCounter, std::uint8_t(counter + 1));

    // Disabled: off the tick list, models dropped.
    proc->Enable(false);
    CHECK_EQ(host.removed.size(), std::size_t(1));
    CHECK(proc->ModelA() == nullptr && proc->ModelB() == nullptr && proc->ModelIdA() == 0u);
    proc->Release();
    SetAdvisorUIImageHost(nullptr);
}

void TestLlama() {
    Host host;
    host.llama = true;
    host.missing.insert(0x1003 + 0x100);   // mood 3's talking head has no llama model
    SetAdvisorUIImageHost(&host);
    auto* proc = new cSC4AdvisorUIImageWinProc(7);
    proc->AddRef();
    proc->ForceMood(0);   // -> mood 3
    proc->Enable(true);
    // A: the llama key first, then the normal one; B: the llama key exists
    CHECK_EQ(host.asked.size(), std::size_t(3));
    CHECK(host.asked.size() == 3 && host.asked[0].instance == 0x1103u && host.asked[1].instance == 0x1003u &&
          host.asked[2].instance == 0x1107u);
    CHECK(proc->ModelA() && static_cast<FakeModel*>(proc->ModelA())->id == 0x1003u);
    CHECK(proc->ModelB() && static_cast<FakeModel*>(proc->ModelB())->id == 0x1107u);
    proc->Enable(false);
    proc->Release();
    SetAdvisorUIImageHost(nullptr);
}

}  // namespace

int main() {
    TestMoods();
    TestModelsAndTicks();
    TestLlama();
    std::printf(failures ? "FAILED\n" : "sc4_advisoruiimagewinproc: all passed\n");
    return failures ? 1 : 0;
}
