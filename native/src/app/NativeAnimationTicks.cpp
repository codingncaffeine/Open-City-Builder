// The animation tick manager on the native application (cSC4AnimationTickManager, SL::ATM()). The original creates it through GZCOM in
// cSC4WinRegionScreen::InitApplicationServices (0x100238972) and, when none exists, in the city view's start-up (cSC4View3DWin::DoMessage
// @ 0x1007865fe); Init adds it to the framework, which is where SL::ATM() finds it. The region screen ticks it from GZPaint (its "frame
// service" EndFrame, vslot 0x48 = DoAnimationTick); the city view's per-frame call is not recovered, so in a city the native frame loop
// ticks it.
#include "app/NativeAnimationTicks.h"

#include <cstdio>
#include <cstdlib>
#include <SDL.h>

#include "app/NativeBoot.h"
#include "gz/GZCOM.h"
#include "gz/GZFramework.h"
#include "platform/Unwired.h"
#include "sc4/AnimationTickManager.h"

namespace native {
namespace {
sc4::cSC4AnimationTickManager* g_atm = nullptr;
}  // namespace

sc4::cSC4AnimationTickManager* AnimationTickManager() {
    if (!g_atm) {
        sc4::SetAnimationTickManagerFramework(BootFramework());
        void* raw = gzcom::GZCOM_CreatecSC4AnimationTickManager();   // GZCOM()->CreateInstance(0xa9c73857, 0x86ad10ed)
        if (!raw) return nullptr;
        auto* atm = static_cast<sc4::cSC4AnimationTickManager*>(static_cast<sc4::cISC4AnimationTickManager*>(raw));
        atm->AddRef();
        g_atm = atm;
    }
    return g_atm;
}

bool StartAnimationTicks() {
    sc4::cSC4AnimationTickManager* atm = AnimationTickManager();
    if (std::getenv("SC4_SIMDEBUG")) { static int n = 0; if (n++ < 3) std::fprintf(stderr, "atm: start (sdl %u)\n", SDL_GetTicks()); }
    return atm && atm->Init();
}

void StopAnimationTicks() {
    if (g_atm) g_atm->Shutdown();
}

namespace { std::uint64_t g_presented = 0; }
void NoteFramePresented() { ++g_presented; }

std::uint32_t TickAnimation() {
    if (!g_atm) return 0;
    const std::uint32_t result = g_atm->DoAnimationTick();
    static const bool trace = std::getenv("SC4_SIMDEBUG") != nullptr;
    if (trace) {   // the tick pace, once per 5 s: a tick that follows the last one by less than the manager's 2 ms floor is handed 2 ms
        static std::uint64_t ticks = 0, short_ = 0, presentedBefore = 0;
        static std::uint32_t last = SDL_GetTicks();
        static std::uint64_t lastNs = SDL_GetTicksNS();
        const std::uint64_t nowNs = SDL_GetTicksNS();
        if (nowNs - lastNs < 2000000u) ++short_;
        lastNs = nowNs;
        ++ticks;
        const std::uint32_t now = SDL_GetTicks();
        if (now - last >= 5000) {
            std::fprintf(stderr, "atm: pace %llu ticks in %u ms (%.1f ms per tick), %llu under the 2 ms floor, %llu frames presented\n",
                         static_cast<unsigned long long>(ticks), now - last, double(now - last) / double(ticks),
                         static_cast<unsigned long long>(short_), static_cast<unsigned long long>(g_presented - presentedBefore));
            presentedBefore = g_presented;
            ticks = short_ = 0;
            last = now;
        }
    }
    if (trace && g_atm->TickCount() % 240 == 0)
        std::fprintf(stderr, "atm: sdl %u tick %u at %u us, objects %zu/%zu/%zu/%zu, %.1f ticks/s\n", SDL_GetTicks(), g_atm->TickCount(), g_atm->GetCurrentTickTime(), g_atm->BucketSize(0),
                     g_atm->BucketSize(1), g_atm->BucketSize(2), g_atm->BucketSize(3), g_atm->TicksPerSecond());
    return result;
}

// A cIGZMessageServer2 view of the manager for the seams that model SL::ATM() as one (its vslots 0x28 / 0x30, AddTickableObject and
// RemoveTickableObject, line up with AddNotification / RemoveNotification): the "message type" those callers pass is the priority.
class AnimationTickServer final : public cIGZMessageServer2 {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { SC4_UNWIRED("ATM as message server: MessageSend"); return false; }
    bool MessagePost(cIGZMessage2*, bool) override { SC4_UNWIRED("ATM as message server: MessagePost"); return false; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t priority) override {
        sc4::cSC4AnimationTickManager* atm = AnimationTickManager();
        return atm && atm->AddTickableObject(target, priority);
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t priority) override {
        sc4::cSC4AnimationTickManager* atm = AnimationTickManager();
        return atm && atm->RemoveTickableObject(target, priority);
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { TickAnimation(); return true; }   // vslot 0x48 = DoAnimationTick
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

cIGZMessageServer2* AnimationTickServerView() {
    static AnimationTickServer server;
    return &server;
}

}  // namespace native
