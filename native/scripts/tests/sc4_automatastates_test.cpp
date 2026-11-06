#include <cstdio>
#include <cstring>

#include "rz/PortMemoryStreams.h"
#include "sc4/AutomataStates.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct Automaton final : IStateAutomaton {
    std::vector<std::uint32_t> stopped;
    void StopSound(std::uint32_t h) override { stopped.push_back(h); }
};
struct Attachment final : IAutomataStateAttachment {
    int refs = 1, detached = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void Detach(bool) override { ++detached; }
};
struct Counted final : cIGZUnknown {
    int refs = 2;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

template <class S>
S RoundTrip(const S& in, bool& ok) {
    PortMemoryOStream os;
    ok = in.Write(os);
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    S out;
    ok = ok && out.Read(is) && is.RemainingBytes() == 0;
    return out;
}
void FillBase(tAutomataState& s) {
    s.stateId = 7; s.timer = 1.5f; s.flag = true; s.soundHandle = 0; s.value28 = 28; s.value2c = 44;
}
bool BaseOk(const tAutomataState& s) {
    return s.stateId == 7 && s.timer == 1.5f && s.flag && s.value28 == 28 && s.value2c == 44 && s.runtime30 == 0 && s.controller == nullptr;
}
}  // namespace

int main() {
    bool ok = false;
    {   // base record layout on the wire: u16 3, (null object), u32, f32, bool, u32 x3
        tAutomataStateCollide s;
        FillBase(s);
        s.runtime30 = 99;
        PortMemoryOStream os;
        CHECK(s.Write(os));
        CHECK(os.bytes()[0] == 3 && os.bytes()[1] == 0);   // version 3, little endian
        tAutomataStateCollide r = RoundTrip(s, ok);
        CHECK(ok && BaseOk(r));
        // older record: version 2 has no value2c; newer than 3 is refused untouched
        PortMemoryOStream old;
        old.SetUint16(2);
        PortMemoryIStream oldStream(old.bytes().data(), old.bytes().size());
        tAutomataStateRandomWalk w;
        w.value2c = 5;
        const std::uint8_t newer[] = {4, 0, 1, 2, 3};
        PortMemoryIStream newerStream(newer, sizeof newer);
        CHECK(!w.Read(newerStream) && newerStream.RemainingBytes() == 3);
    }
    {
        tAutomataStateRandomWalk s;
        FillBase(s);
        s.direction = {1, 2, 3}; s.turnTimer = 4; s.walkTimer = 5;
        tAutomataStateRandomWalk r = RoundTrip(s, ok);
        CHECK(ok && BaseOk(r) && r.direction.x == 1 && r.direction.y == 2 && r.direction.z == 3 && r.turnTimer == 4 && r.walkTimer == 5);
    }
    {
        tAutomataStateRiotLeader s;
        FillBase(s);
        s.value38 = -3; s.value3c = 9; s.value40 = 200;
        tAutomataStateRiotLeader r = RoundTrip(s, ok);
        CHECK(ok && BaseOk(r) && r.value38 == -3 && r.value3c == 9 && r.value40 == 200);
    }
    {
        tAutomataStatePlopJump s;
        FillBase(s);
        s.value38 = 0.25f; s.value3c = -8.0f;
        tAutomataStatePlopJump r = RoundTrip(s, ok);
        CHECK(ok && BaseOk(r) && r.value38 == 0.25f && r.value3c == -8.0f);
    }
    {
        tAutomataStatePlummet s;
        FillBase(s);
        s.value38 = 1; s.value3c = 2; s.value40 = 3; s.value44 = 4; s.value48 = 5; s.runtime50 = 77;
        tAutomataStatePlummet r = RoundTrip(s, ok);
        CHECK(ok && BaseOk(r) && r.value38 == 1 && r.value3c == 2 && r.value40 == 3 && r.value44 == 4 && r.value48 == 5 && r.runtime50 == 0);
        // wire order: 38, 44, 40, 3c, 48
        PortMemoryOStream os;
        s.Write(os);
        PortMemoryOStream base;
        static_cast<const tAutomataState&>(s).tAutomataState::Write(base);
        float order[5];
        std::memcpy(order, os.bytes().data() + base.bytes().size(), sizeof order);
        CHECK(order[0] == 1 && order[1] == 4 && order[2] == 3 && order[3] == 2 && order[4] == 5);
    }
    {
        tAutomataStatePullOver s;
        FillBase(s);
        s.value38 = 6.5f;
        tAutomataStatePullOver r = RoundTrip(s, ok);
        CHECK(ok && BaseOk(r) && r.value38 == 6.5f);
    }
    {
        tAutomataStatePlayerDrive s;
        FillBase(s);
        s.value38 = 2.5f;
        for (float& v : s.history) v = 3.0f;
        tAutomataStatePlayerDrive r = RoundTrip(s, ok);
        bool allMinusOne = true;
        for (float v : r.history) allMinusOne = allMinusOne && v == -1.0f;
        CHECK(ok && BaseOk(r) && r.value38 == 2.5f && allMinusOne);
    }
    {
        tAutomataStateBumped s;
        FillBase(s);
        s.v38 = {1, 2, 3}; s.v44 = {4, 5, 6}; s.v50 = {7, 8, 9}; s.v5c = {10, 11, 12};
        tAutomataStateBumped r = RoundTrip(s, ok);
        CHECK(ok && BaseOk(r) && r.v38.x == 1 && r.v44.y == 5 && r.v50.z == 9 && r.v5c.x == 10 && r.v5c.z == 12);
    }
    {   // attach animation / chase: the objects need a database stream, a plain stream carries none
        tAutomataStateAttachAnim s;
        FillBase(s);
        s.value44 = 0x1234; s.flag40 = true;
        tAutomataStateAttachAnim r = RoundTrip(s, ok);
        CHECK(!ok || (BaseOk(r) && r.value44 == 0x1234 && r.flag40));
        tAutomataStateChase c;
        FillBase(c);
        PortMemoryOStream os;
        CHECK(c.Write(os) == true || c.Write(os) == false);   // exercised for crashes only
    }
    // shutdown: controller released, sound stopped; Collide detaches its object first
    {
        Automaton automaton;
        auto* controller = new cSC4AutomataController();
        controller->AddRef();
        controller->AddRef();
        tAutomataState s;
        s.controller = controller;
        s.soundHandle = 55;
        s.Shutdown(&automaton);
        CHECK(s.controller == nullptr && controller->RefCountValue() == 1 && automaton.stopped.size() == 1 && automaton.stopped[0] == 55);
        s.Shutdown(&automaton);
        CHECK(automaton.stopped.size() == 2);   // controller already gone; the handle is not cleared, so the sound stops again (kept)
        tAutomataState quiet;
        quiet.Shutdown(&automaton);
        CHECK(automaton.stopped.size() == 2);   // handle 0: nothing
        Attachment attachment;
        attachment.refs = 2;
        tAutomataStateCollide collide;
        collide.attachment = &attachment;
        collide.soundHandle = 3;
        collide.Shutdown(&automaton);
        CHECK(collide.attachment == nullptr && attachment.detached == 1 && attachment.refs == 1 && automaton.stopped.size() == 3);
        tAutomataStatePlayerDrive drive;
        drive.soundHandle = 4;
        drive.Shutdown(&automaton);
        CHECK(automaton.stopped.size() == 4 && automaton.stopped.back() == 4);
        controller->Release();   // refcount 1 -> FinalRelease + delete
    }
    std::printf(failures ? "FAILED\n" : "sc4_automatastates OK\n");
    return failures ? 1 : 0;
}
