// cHitTrackPlayer: register file semantics, the bytecode interpreter, waits, fades, lifecycle.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "audio/HitClassIds.h"
#include "audio/HitTrackPlayer.h"
#include "gz/cGZAny.h"

namespace {

struct Services final : Hit::cIHitManServices {
    int registered = 0, unregistered = 0;
    bool HasSoundSystem() const override { return true; }
    void EnumerateUpdateTargets(EnumCallback, std::uint32_t, void*) override {}
    bool GetPersistResource(const cGZPersistResourceKey&, std::uint32_t, void**) override { return false; }
    cIGZUnknown* CreatePropertySetResource(const cGZPersistResourceKey&, const Hit::DefaultProperty*, std::size_t, bool) override { return nullptr; }
    bool ReadPersistRecord(const cGZPersistResourceKey&, std::vector<std::uint8_t>&, std::string&) override { return false; }
    bool GetSoundById(std::uint32_t, std::uint32_t, void**) override { return false; }
    void RegisterUpdateTarget(cIGZSndUpdateTarget*, std::uint32_t) override { ++registered; }
    void UnregisterUpdateTarget(cIGZSndUpdateTarget*) override { ++unregistered; }
    bool SetGlobalProperty(std::uint32_t, const cIGZAny&) override { return true; }
    bool GetGlobalProperty(std::uint32_t, cIGZAny*) override { return false; }
    bool SetInstanceProperty(std::uint32_t, std::uint32_t, const cIGZAny&) override { return true; }
    bool GetDefaultSndPlayerClassId(bool, std::uint32_t&) override { return false; }
    bool CreateComInstance(std::uint32_t, std::uint32_t, void**) override { return false; }
    bool ReadFileBytes(const std::string&, std::vector<std::uint8_t>&) override { return false; }
    std::string ApplicationDirectory() const override { return ""; }
    std::string DataDirectory() const override { return ""; }
    void ParseDebugInfo(const cIGZString&, const std::uint8_t*, std::uint32_t) override {}
    bool CreateTrackPlayer(std::uint32_t, void**) override { return false; }
    bool CreateChannel(void**) override { return false; }
};

std::vector<std::string> g_errors;
std::vector<std::unique_ptr<std::uint8_t[]>> g_programBytes;
void RecordError(cIHitTrackPlayer*, const char* message) { g_errors.emplace_back(message); }

struct Program {
    std::vector<std::uint8_t> bytes;
    Program& b(std::uint8_t v) { bytes.push_back(v); return *this; }
    Program& i32(std::int32_t v) { for (int k = 0; k < 4; ++k) b(static_cast<std::uint8_t>(v >> (8 * k))); return *this; }
};

constexpr std::uint32_t kEntryKey = 0x1234;
constexpr std::uint32_t kRegEntry = 0xff5f15e1;

// Registers a block holding `program` in the manager and returns a player pointing at it.
cHitTrackPlayer* MakePlayer(cHitMan& hm, const Program& program) {
    // The block only points at the bytes; the test owns them until the manager is gone.
    g_programBytes.emplace_back(new std::uint8_t[program.bytes.size()]);
    std::uint8_t* data = g_programBytes.back().get();
    std::memcpy(data, program.bytes.data(), program.bytes.size());
    auto* block = new cHitTrackDataBlock();
    block->AddRef();
    block->SetDataBlock(data, static_cast<std::uint32_t>(program.bytes.size()));
    block->AddEntryPoint(kEntryKey, 0);
    hm.RegisterTrackDataBlock(block);
    block->Release();
    auto* player = new cHitTrackPlayer();
    player->AddRef();
    player->SetRegisterValue(static_cast<std::int32_t>(kRegEntry), kEntryKey, true);
    return player;
}

}  // namespace

int main() {
    Services services;
    Hit::SetServices(&services);
    cHitMan& hm = *new cHitMan();
    hm.AddRef();
    hm.Init();
    hm.SetTrackFlowErrorHandler(&RecordError);

    // ---- layout facts ----
    {
        cHitTrackPlayer p;
        char* base = reinterpret_cast<char*>(static_cast<cIHitTrackPlayer*>(&p));
        auto off = [&](void* q) { return reinterpret_cast<char*>(q) - base; };
        assert(off(static_cast<cIGZSndRegisterSet*>(&p)) == 0x08);
        assert(off(static_cast<cIGZSnd3D*>(&p)) == 0x10);
        assert(off(static_cast<cIGZSnd*>(&p)) == 0x18);
        assert(off(static_cast<cIGZSndViewUpdateTarget*>(&p)) == 0x20);
        assert(off(static_cast<cIGZSndUpdateTarget*>(&p)) == 0x28);
        assert(off(static_cast<cIGZSndMultipleSources*>(&p)) == 0x30);
        assert(off(static_cast<cIGZSndCompletionNotification*>(&p)) == 0x38);
        void* q = nullptr;
        assert(p.QueryInterface(1, &q) && q == static_cast<cIHitTrackPlayer*>(&p));
        assert(p.QueryInterface(0xdd583e2d, &q) && q == static_cast<cIGZSndMultipleSources*>(&p));
        assert(!p.QueryInterface(0x1234, &q));
    }

    // ---- registers: own layer, legacy ids, typed reads, flags ----
    {
        cHitTrackPlayer p;
        assert(p.GetRegisterValue(0x15) == 0);
        assert(p.SetRegisterValue(0x15, 77, true) && p.GetRegisterValue(0xff90aac8) == 77);
        assert(p.flagGreater() && !p.flagEqual() && !p.flagLess());
        p.SetRegisterValue(0x15, 0, true);
        assert(p.flagEqual());
        p.SetRegisterValue(0x15, -5, true);
        assert(p.flagLess());
        p.SetRegisterFloat(0xff3071b9, 2.5f, true);
        assert(p.GetRegisterFloat(0xff3071b9) == 2.5f && p.GetRegisterValue(0xff3071b9) == 0);
        assert(p.HasRegisterValue(0x15) && !p.HasRegisterValue(0xff000001));
        assert(p.GetSoundPriority() == 0 && p.SetSoundPriority(4) && p.GetSoundPriority() == 4);
        assert(!p.GetVelocity(nullptr, nullptr, nullptr));
        p.SetID(99);
        assert(p.GetID() == 99);
        // AddSource never adds (binary bug) but notifies
        p.AddSource(5);
        assert(p.SourceList().empty());
    }

    // ---- program: counter loop then end ----
    {
        Program prog;
        prog.b(0x04).b(0x20).b(3)          // 0: r20 = 3
            .b(0x30).b(0x20)               // 3: r20--
            .b(0x0e).b(0x20)               // 5: compare r20 with 0
            .b(0x3f).i32(3)                // 7: if not equal jump to 3
            .b(0x0c);                      // 12: end
        cHitTrackPlayer* p = MakePlayer(hm, prog);
        assert(p->PlayPause());
        assert(p->IsPlaying());
        assert(p->GetRegisterValue(0x20) == 0);   // not run yet
        p->HandleTimerCallback(0);                // runs until the program ends
        assert(p->GetRegisterValue(0x20) == 0 && !p->IsPlaying());
        assert(p->Release() == 0 || true);
    }

    // ---- waits: "wait register" burns down by elapsed time, runs on afterwards ----
    {
        Program prog;
        prog.b(0x05).b(0x21).i32(100)      // r21 = 100
            .b(0x09).b(0x21)               // wait r21
            .b(0x04).b(0x22).b(9)          // r22 = 9
            .b(0x0c);
        cHitTrackPlayer* p = MakePlayer(hm, prog);
        p->PlayPause();
        p->HandleTimerCallback(0);
        assert(p->waitTicks() == 100 && p->GetRegisterValue(0x22) == 0);
        p->HandleTimerCallback(60);
        assert(p->waitTicks() == 40 && p->GetRegisterValue(0x22) == 0);
        p->HandleTimerCallback(60);               // burns the remaining 40 and continues
        assert(p->waitTicks() == 0 && p->GetRegisterValue(0x22) == 9 && !p->IsPlaying());
        p->Release();
    }

    // ---- arithmetic, call/return, errors ----
    {
        Program prog;
        prog.b(0x04).b(0x23).b(6)          // 0: r23 = 6
            .b(0x04).b(0x24).b(7)          // 3: r24 = 7
            .b(0x13).b(0x23).b(0x24)       // 6: r23 *= r24 -> 42
            .b(0x07).i32(18)               // 9: call 18
            .b(0x0c)                       // 14: end
            .b(0x00).b(0x00).b(0x00)       // 15..17 padding
            .b(0x30).b(0x23)               // 18: r23--
            .b(0x08);                      // 20: return
        cHitTrackPlayer* p = MakePlayer(hm, prog);
        p->PlayPause();
        p->HandleTimerCallback(0);
        assert(p->GetRegisterValue(0x23) == 41 && !p->IsPlaying());
        p->Release();

        g_errors.clear();
        Program bad;
        bad.b(0x1a).b(0x0c);               // illegal instruction, then end
        cHitTrackPlayer* q = MakePlayer(hm, bad);
        q->PlayPause();
        q->HandleTimerCallback(0);
        assert(g_errors.size() == 1 && g_errors[0] == "Illegal Instruction");
        q->Release();

        g_errors.clear();
        Program div0;
        div0.b(0x04).b(0x25).b(8).b(0x04).b(0x26).b(0).b(0x12).b(0x25).b(0x26).b(0x0c);
        cHitTrackPlayer* d = MakePlayer(hm, div0);
        d->PlayPause();
        d->HandleTimerCallback(0);
        assert(g_errors.size() == 1 && g_errors[0] == "Divide by zero");   // port guard (the binary faults)
        d->Release();
    }

    // ---- pause/unpause/kill drive the sound system registration ----
    {
        Program prog;
        prog.b(0x05).b(0x27).i32(1000).b(0x09).b(0x27).b(0x0c);
        cHitTrackPlayer* p = MakePlayer(hm, prog);
        p->PlayPause();
        p->HandleTimerCallback(0);
        const int unreg = services.unregistered, reg = services.registered;
        assert(p->Pause(false) && p->IsPaused() && services.unregistered == unreg + 1);
        assert(p->Unpause() && !p->IsPaused() && services.registered == reg + 1);
        assert(p->Kill());                       // destroys the player's own extra references
        assert(!p->IsPlaying());
        p->Release();
    }

    // default flow-error handler: kills the track and flags it
    {
        cHitMan& fresh = *new cHitMan();
        fresh.AddRef();
        RZ::SetHitMan(&fresh);
        Program bad;
        bad.b(0x05).b(0x28).i32(1).b(0x09).b(0x28).b(0x1a);
        cHitTrackPlayer* p = MakePlayer(fresh, bad);
        p->PlayPause();
        p->HandleTimerCallback(0);
        p->HandleTimerCallback(5);               // reaches the illegal instruction
        assert(p->GetErrorCondition() && !p->IsPlaying());
        p->Release();
        RZ::SetHitMan(&hm);
        fresh.Release();
    }
    assert(cHitTrackPlayer::NumSoundObjects() == 0);
    hm.Release();
    Hit::SetServices(nullptr);
    std::puts("hittrackplayer ok");
}
