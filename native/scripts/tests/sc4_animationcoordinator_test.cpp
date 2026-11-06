// cSC4AnimationCoordinator (i386 0x00284b64..0x00285dda): which animatable props tick, against a fake city.
#include <cstdio>
#include <map>
#include <set>

#include "rz/cRZMessage2Standard.h"
#include "sc4/AnimationCoordinator.h"
#include "sc4/PropOccupant.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
class Server final : public cIGZMessageServer2 {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t type) override { types.insert(type); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override { types.erase(type); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
    std::set<std::uint32_t> types;
};

class Host final : public IAnimationCoordinatorHost {
public:
    cIGZMessageServer2* Messages() override { return &server; }
    bool AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override { ticking.insert(target); lastPriority = priority; return true; }
    bool RemoveTickable(cIGZMessageTarget2* target, std::uint32_t) override { ticking.erase(target); return true; }
    bool IsOccupantPowered(cISC4Occupant*) override { return powered; }
    bool OccupantFunding(cISC4Occupant*, std::uint32_t purpose, float& out) override { askedPurpose = purpose; out = funding; return hasFunding; }
    bool OccupantLotCells(cISC4Occupant*, int out[4]) override { for (int i = 0; i < 4; ++i) out[i] = lot[i]; return true; }
    int ViewZoom() override { return zoom; }
    void VisibleTerrainRect(float out[4]) override { for (int i = 0; i < 4; ++i) out[i] = view[i]; }
    int CityCellsX() override { return 64; }        // 4 x 4 occupant sets
    int CityDetail() override { return detail; }
    int OccupantModelClass(cISC4Occupant* occupant) override { auto it = classes.find(occupant); return it == classes.end() ? 0 : it->second; }

    Server server;
    std::set<cIGZMessageTarget2*> ticking;
    std::map<cISC4Occupant*, int> classes;
    std::uint32_t lastPriority = 0, askedPurpose = 0;
    bool powered = true, hasFunding = true;
    float funding = 1.0f;
    int zoom = 4, detail = 3;
    float view[4] = {0.0f, 0.0f, 511.0f, 511.0f};   // sets 0..1 x 0..1
    int lot[4] = {0, 0, 63, 63};
};

constexpr int kSprite = static_cast<int>(0xea123be1u);
constexpr int kTurning = static_cast<int>(0xaa123bf9u);

cSC4PropOccupant* MakeProp(Host& host, float x, float z, int modelClass) {
    auto* prop = new cSC4PropOccupant();
    prop->AddRef();
    const float position[3] = {x, 0.0f, z};
    prop->SetPosition(position);
    host.classes[static_cast<cISC4Occupant*>(prop)] = modelClass;
    return prop;
}
bool Ticks(Host& host, cSC4PropOccupant* prop) {
    return host.ticking.count(static_cast<cIGZMessageTarget2*>(prop)) != 0 && prop->IsFlagSet(0x20);
}
bool Idle(Host& host, cSC4PropOccupant* prop) {
    return host.ticking.count(static_cast<cIGZMessageTarget2*>(prop)) == 0 && !prop->IsFlagSet(0x20);
}
void Send(cSC4AnimationCoordinator& c, std::uint32_t type, void* v1 = nullptr, void* v2 = nullptr) {
    cRZMessage2Standard message;
    message.AddRef();
    message.SetType(type);
    message.SetVoid1(v1);
    message.SetVoid2(v2);
    c.DoMessage(&message);
}
}  // namespace

int main() {
    Host host;
    auto* coordinator = new cSC4AnimationCoordinator(&host);
    coordinator->AddRef();
    coordinator->Init();
    CHECK(coordinator->SetsPerSide() == 4);
    CHECK(host.server.types.size() == 5 && host.server.types.count(0xa6b79612) && host.server.types.count(0x8a07ae00));
    int rect[4];
    coordinator->VisibleSetRect(rect);
    CHECK(rect[0] == 0 && rect[1] == 0 && rect[2] == 1 && rect[3] == 1);

    // a static model class is not the coordinator's
    cSC4PropOccupant* plain = MakeProp(host, 10.0f, 10.0f, 0);
    coordinator->SetOccupantAnimation(plain, true);
    CHECK(coordinator->HeldCount() == 0 && Idle(host, plain));

    // an animated prop in a visible set ticks at priority 0xc0; the reference is held
    cSC4PropOccupant* near = MakeProp(host, 300.0f, 100.0f, kSprite);
    const std::uint32_t refsBefore = near->AddRef() - 1;
    near->Release();
    coordinator->SetOccupantAnimation(near, true);
    CHECK(coordinator->HeldCount() == 1 && Ticks(host, near) && host.lastPriority == 0xc0);
    CHECK(near->AddRef() - 1 == refsBefore + 1);
    near->Release();
    coordinator->SetOccupantAnimation(near, true);   // the same occupant twice: held once
    CHECK(coordinator->HeldCount() == 1);

    // the set is position / 256, floored: 767.9 is set 2 (outside), 512 too
    cSC4PropOccupant* far = MakeProp(host, 767.9f, 10.0f, kTurning);
    coordinator->SetOccupantAnimation(far, true);
    CHECK(coordinator->HeldCount() == 2 && Idle(host, far));
    cSC4PropOccupant* edge = MakeProp(host, 511.9f, 511.9f, kSprite);
    coordinator->SetOccupantAnimation(edge, true);
    CHECK(Ticks(host, edge));

    // 0x4a70d491 on the exemplar keeps a prop out
    cSC4PropOccupant* excluded = MakeProp(host, 20.0f, 20.0f, kSprite);
    cSCPropertyHelp::SetPropertyValue(excluded->AsPropertyHolder(), 0x4a70d491, true);
    coordinator->SetOccupantAnimation(excluded, true);
    CHECK(coordinator->HeldCount() == 3 && Idle(host, excluded));

    // the camera moves: the sets entering the view tick, the ones leaving stop
    host.view[0] = 512.0f; host.view[2] = 1023.0f;   // sets 2..3 x 0..1
    Send(*coordinator, 0xa6b79612);
    coordinator->VisibleSetRect(rect);
    CHECK(rect[0] == 2 && rect[2] == 3);
    CHECK(Ticks(host, far) && Idle(host, near) && Idle(host, edge));
    host.view[0] = 0.0f; host.view[2] = 1023.0f;     // everything
    Send(*coordinator, 0xa6b79612);
    CHECK(Ticks(host, far) && Ticks(host, near) && Ticks(host, edge));

    // zooming out below 2 stops everything; a rotation above it refreshes; zooming back starts them again
    int before[2] = {4, 0}, after[2] = {1, 0};
    host.zoom = 1;
    Send(*coordinator, 0xa6b79602, before, after);
    CHECK(Idle(host, near) && Idle(host, far) && Idle(host, edge));
    before[0] = 1; after[0] = 3;
    host.zoom = 3;
    Send(*coordinator, 0xa6b79602, before, after);
    CHECK(Ticks(host, near) && Ticks(host, far));

    // power: an exemplar asking for power (0xea5393ed) needs a powered lot
    cSC4PropOccupant* powered = MakeProp(host, 40.0f, 40.0f, kSprite);
    cSCPropertyHelp::SetPropertyValue(powered->AsPropertyHolder(), 0xea5393ed, true);
    host.powered = false;
    coordinator->SetOccupantAnimation(powered, true);
    CHECK(Idle(host, powered));
    host.powered = true;
    const int cells[4] = {0, 0, 15, 15};
    {
        cRZMessage2Standard message;   // 0x8a07ae00: the cell block 0..15 (set 0) refreshes
        message.AddRef();
        message.SetType(0x8a07ae00);
        message.SetData1(cells[0]); message.SetData2(cells[1]); message.SetData3(cells[2]); message.SetData4(cells[3]);
        coordinator->DoMessage(&message);
    }
    CHECK(Ticks(host, powered));

    // funding: 0x8a5f88e3 = {purpose, least percent}; 79 % < 80 % stops, 80 % plays, no budget item = 0 %
    cSC4PropOccupant* funded = MakeProp(host, 60.0f, 60.0f, kSprite);
    cSCPropertyHelp::SetPropertyValue(funded->AsPropertyHolder(), 0x8a5f88e3, std::vector<std::uint32_t>{7u, 80u});
    host.funding = 0.79f;
    coordinator->SetOccupantAnimation(funded, true);
    CHECK(Idle(host, funded) && host.askedPurpose == 7);
    host.funding = 0.80f;
    coordinator->SetOccupantAnimation(funded, true);
    CHECK(Ticks(host, funded));
    host.hasFunding = false;
    host.funding = 5.0f;
    coordinator->SetOccupantAnimation(funded, true);
    CHECK(Idle(host, funded));
    host.hasFunding = true;

    // the city detail preference (re-read on 0x231bbf91 once something is held): below 2 nothing ticks
    host.detail = 1;
    Send(*coordinator, 0x231bbf91);
    CHECK(!coordinator->DetailAllows() && Idle(host, near) && Idle(host, far) && Idle(host, powered));
    host.detail = 2;
    Send(*coordinator, 0x231bbf91);
    CHECK(coordinator->DetailAllows() && Ticks(host, near));

    // hiding a prop: out of the tick manager, the reference given back
    coordinator->SetOccupantAnimation(near, false);
    CHECK(Idle(host, near) && near->AddRef() - 1 == refsBefore);
    near->Release();
    const int held = coordinator->HeldCount();
    coordinator->SetOccupantAnimation(near, false);   // twice: nothing changes
    CHECK(coordinator->HeldCount() == held);

    coordinator->Shutdown();
    CHECK(host.server.types.empty());
    coordinator->Release();
    for (cSC4PropOccupant* p : {plain, near, far, edge, excluded, powered, funded}) p->Release();
    std::puts(failures ? "sc4_animationcoordinator: FAILED" : "sc4_animationcoordinator ok");
    return failures != 0;
}
