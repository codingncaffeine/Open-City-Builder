// cSC4MySimAgentSimulator: the five sims plus the special one, the forwarding of every city event, the panel bookkeeping and the console command.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/MySimAgentSimulator.h"

#include "fake_occupant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct SimBase : cISC4MySim {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int refs = 0;
    void MoveIn(std::uint8_t, cRZString&, bool, std::uint8_t, cRZString&, std::uint32_t, cRZString&) override {}
    cISC4Advisor* AsAdvisor() override { return {}; }
    bool FindNewResidence() override { return {}; }
    bool ChangeResidence(cISC4BuildingOccupant* building) override { return {}; }
    bool TestForResidence(cISC4BuildingOccupant* building, bool checkFilter, bool checkWealth) override { return {}; }
    bool ChangeJob(cISC4BuildingOccupant* building) override { return {}; }
    bool DeleteFromGame() override { return {}; }
    std::uint8_t GetSlot() const override { return {}; }
    cISC4BuildingOccupant* GetResidence() const override { return {}; }
    cISC4BuildingOccupant* GetJob() const override { return {}; }
    void SetResidence(cISC4BuildingOccupant* building) override {}
    bool GetLocation(float* xyz) const override { return {}; }
    cGZPersistResourceKey GetVehicleKey() const override { return cGZPersistResourceKey{}; }
    std::uint8_t GetSign() const override { return {}; }
    std::uint8_t GetAge() const override { return {}; }
    bool IsMale() const override { return {}; }
    int GetWealth() const override { return {}; }
    bool IsActive() const override { return {}; }
    const cRZString& GetBio() const override { static cRZString s; return s; }
    std::uint32_t GetBitmapID() const override { return {}; }
    const cRZString& GetBitmapFile() const override { static cRZString s; return s; }
    bool GetBitmapBuffer(cIGZBuffer** out) const override { return {}; }
    const cRZString& GetHomeName() const override { static cRZString s; return s; }
    const cRZString& GetJobName() const override { static cRZString s; return s; }
    const cRZString& GetJobTitle() const override { static cRZString s; return s; }
    bool GetEQAsText(cRZString& out) override { return {}; }
    bool GetHQAsText(cRZString& out) override { return {}; }
    void Find() override {}
    void ShowResidence() override {}
    void HideResidence() override {}
    void ShowLocation() override {}
    void HideLocation() override {}
    void SetPylonVisibility(bool visible) override {}
    void UpdateResidence() override {}
    void LeaveForWork() override {}
    void ReturnHome() override {}
    void AnnounceMoveIn() override {}
    void DoScriptCommand(int command) override {}
    void DoBuildingAdded(cISC4BuildingOccupant* building) override {}
    void DoBuildingChanged(cISC4BuildingOccupant* building) override {}
    void DoBuildingRemoved(cISC4BuildingOccupant* building, std::uint32_t data) override {}
    void DoFireAdded(float x, float z) override {}
    void DoPipeBurst(float x, float z) override {}
    void DoTornado(float x, float z) override {}
    void DoDisaster(std::uint32_t disaster, int x, int z) override {}
    void DoNeighborConnectionRemoved(int cellX, int cellZ) override {}
    int DistanceToClosest(std::uint32_t occupantGroup) override { return {}; }
    std::uint32_t DistanceToClosestSpecial(std::uint32_t buildingType) override { return {}; }
    float LocalSchoolGrade(std::uint32_t) override { return {}; }
    float LocalHospitalGrade(std::uint32_t) override { return {}; }
    void PlayNightSound() override {}
    bool GetCurrentScreenLocationXY(int* x, int* y) override { return {}; }
    void GetJobTitle(cISC4BuildingOccupant* building, cIGZString& out) override {}
    bool RequestToPlaySound() override { return {}; }
    bool SetCar(cISCResExemplar* exemplar) override { return {}; }
    bool SetPedestrian(cISCResExemplar* exemplar) override { return {}; }
    std::uint32_t GetModelButtonBitmapID(bool pedestrian) const override { return {}; }
    cISC4MySimBalloonAdvice* GetNextBalloonAdvice() override { return {}; }
    cISC4MySimBalloonAdvice* GetNextBalloonEventAdvice() override { return {}; }
    cISC4MySimBalloonAdvice* GetCurrentBalloonAdvice() const override { return {}; }
    cISC4MySimBalloonAdvice* GetCurrentBalloonEventAdvice() const override { return {}; }
    bool CanUDriveIt() override { return {}; }
    bool EnterUDriveIt() override { return {}; }
    bool ExitUDriveIt() override { return {}; }
    bool CreateDispatchTool() override { return {}; }
    bool ActivateLuaContext() override { return {}; }
};
struct Sim final : SimBase {
    std::vector<std::string> calls;
    std::uint8_t slot = 0;
    bool active = false;
    cISC4BuildingOccupant* residence = nullptr;
    float lastX = 0, lastZ = 0;
    std::uint32_t lastData = 99;
    int lastCommand = -1;
    std::uint32_t lastDisaster = 0;
    int lastX2 = 0, lastZ2 = 0;
    std::uint8_t moveSlot = 0;
    std::uint8_t GetSlot() const override { return slot; }
    bool IsActive() const override { return active; }
    cISC4BuildingOccupant* GetResidence() const override { return residence; }
    void MoveIn(std::uint8_t s, cRZString&, bool, std::uint8_t, cRZString&, std::uint32_t, cRZString&) override { moveSlot = s; calls.push_back("movein"); }
    void ShowResidence() override { calls.push_back("showres"); }
    void HideResidence() override { calls.push_back("hideres"); }
    void ShowLocation() override { calls.push_back("showloc"); }
    void HideLocation() override { calls.push_back("hideloc"); }
    void SetPylonVisibility(bool v) override { calls.push_back(v ? "pylon1" : "pylon0"); }
    void UpdateResidence() override { calls.push_back("update"); }
    void LeaveForWork() override { calls.push_back("work"); }
    void ReturnHome() override { calls.push_back("home"); }
    void PlayNightSound() override { calls.push_back("night"); }
    void DoScriptCommand(int c) override { lastCommand = c; calls.push_back("script"); }
    void DoBuildingAdded(cISC4BuildingOccupant*) override { calls.push_back("added"); }
    void DoBuildingChanged(cISC4BuildingOccupant*) override { calls.push_back("changed"); }
    void DoBuildingRemoved(cISC4BuildingOccupant*, std::uint32_t d) override { lastData = d; calls.push_back("removed"); }
    void DoFireAdded(float x, float z) override { lastX = x; lastZ = z; calls.push_back("fire"); }
    void DoPipeBurst(float x, float z) override { lastX = x; lastZ = z; calls.push_back("pipe"); }
    void DoTornado(float x, float z) override { lastX = x; lastZ = z; calls.push_back("tornado"); }
    void DoDisaster(std::uint32_t d, int x, int z) override { lastDisaster = d; lastX2 = x; lastZ2 = z; calls.push_back("disaster"); }
    void DoNeighborConnectionRemoved(int x, int z) override { lastX2 = x; lastZ2 = z; calls.push_back("neighbor"); }
};

struct Server final : cIGZMessageServer2 {
    std::vector<std::uint32_t> added, removed, posted;
    std::vector<std::uint32_t> postedData;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* m, bool) override {
        posted.push_back(m->GetType());
        postedData.push_back(static_cast<std::uint32_t>(static_cast<cIGZMessage2Standard*>(m)->GetData1()));
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t t) override { added.push_back(t); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t t) override { removed.push_back(t); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};
struct Host final : IMySimAgentHost {
    std::vector<MySimAdvisorEntry> advisors;
    std::map<cIGZUnknown*, cISC4MySim*> sims;
    Server server;
    bool riot = false;
    int registered = 0, unregistered = 0;
    void Advisors(std::vector<MySimAdvisorEntry>& out) override { out = advisors; }
    cISC4MySim* AsMySim(cIGZUnknown* object) override {
        auto it = sims.find(object);
        if (it == sims.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    cIGZMessageServer2* Messages() override { return &server; }
    void RegisterGameData(cSC4MySimAgentSimulator&) override { ++registered; }
    void UnregisterGameData(cSC4MySimAgentSimulator&) override { ++unregistered; }
    bool RiotInProgress() override { return riot; }
    void CellCenter(int x, int z, float& ox, float& oz) override { ox = x * 16.0f + 8.0f; oz = z * 16.0f + 8.0f; }
};
struct Advisor final : cIGZUnknown {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};
}  // namespace

int main() {
    Host host;
    Sim sims[6];
    Advisor advisors[6];
    for (int i = 0; i < 6; ++i) { sims[i].slot = static_cast<std::uint8_t>(i + 1); host.sims[&advisors[i]] = &sims[i]; }
    // the advisor of type 0x1e comes in the middle of the key order but takes the last slot
    host.advisors = {{&advisors[0], 1}, {&advisors[1], 2}, {&advisors[5], 0x1e}, {&advisors[2], 3}, {&advisors[3], 4}, {&advisors[4], 5}};
    auto* agent = new cSC4MySimAgentSimulator(&host);
    agent->AddRef();
    CHECK(agent->GetGZCLSID() == 0x29e37dec && agent->SimCount() == 6);
    void* out = nullptr;
    CHECK(agent->QueryInterface(0xa9e37ebb, &out) && out == static_cast<cIGZSerializable*>(agent));
    agent->Release();
    CHECK(agent->QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(agent));
    agent->Release();
    CHECK(agent->QueryInterface(0xe4fda3d4, &out));
    agent->Release();
    CHECK(!agent->QueryInterface(1234, &out));

    CHECK(agent->Init() && agent->Initialized() && host.registered == 1 && host.server.added.size() == 11);
    CHECK(host.server.added.front() == 0x99ef1142 && host.server.added.back() == 0xea0d163e);
    CHECK(agent->Init() && host.registered == 1);                          // a second Init does nothing
    CHECK(agent->Sim(0) == &sims[0] && agent->Sim(1) == &sims[1] && agent->Sim(2) == &sims[2] && agent->Sim(4) == &sims[4] && agent->Sim(5) == &sims[5]);
    CHECK(sims[5].refs == 1 && sims[0].refs == 1);                         // one reference each (the query's own is dropped)

    // the stream is one version number
    PortMemoryOStream o;
    CHECK(agent->Write(o) && o.bytes().size() == 2);
    PortMemoryIStream in(o.bytes().data(), o.bytes().size());
    CHECK(agent->Read(in));
    const std::uint8_t bad[2] = {2, 0};
    PortMemoryIStream badIn(bad, 2);
    CHECK(!agent->Read(badIn));

    // buildings: only a building view is forwarded, to the five ordinary sims
    sc4_test::FakeOccupant building;
    building.iface = 0x87dda3a9;
    sc4_test::FakeOccupant other;
    agent->DoBuildingAdded(&other);
    CHECK(sims[0].calls.empty());
    agent->DoBuildingAdded(&building);
    CHECK(sims[0].calls == std::vector<std::string>{"added"} && sims[4].calls.size() == 1 && sims[5].calls.empty());
    agent->DoBuildingChanged(&building);
    CHECK(sims[3].calls.back() == "changed");
    sims[1].calls.clear();
    host.riot = false;
    agent->DoBuildingRemoved(&building, 4);
    CHECK(sims[1].lastData == 0);                                          // no disaster running yet
    cRZMessage2Standard* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(0xca1b1e64);
    message->SetData2(10); message->SetData3(20); message->SetData4(7);
    agent->DoMessage(message);
    CHECK(agent->DisasterType() == 7 && sims[2].lastDisaster == 7 && sims[2].lastX2 == 10 && sims[2].lastZ2 == 20 && sims[5].lastDisaster == 0);
    agent->DoBuildingRemoved(&building, 4);
    CHECK(sims[1].lastData == 7);                                          // the disaster type travels with the removal
    host.riot = true;
    agent->DoBuildingRemoved(&building, 4);
    CHECK(sims[1].lastData == 0);                                          // not during a riot
    host.riot = false;
    agent->DoBuildingRemoved(&building, 3);
    CHECK(sims[1].lastData == 0);

    // the hour: 8 work, 17 home, 22 night; other hours nothing
    for (auto& s : sims) s.calls.clear();
    agent->DoHourChange(8); agent->DoHourChange(0x11); agent->DoHourChange(0x16); agent->DoHourChange(9);
    CHECK(sims[0].calls == (std::vector<std::string>{"work", "home", "night"}) && sims[5].calls.empty());

    // pipe bursts and tornadoes arrive as cells and are forwarded as the cell centre
    agent->DoPipeBurst(2, 3);
    CHECK(sims[0].lastX == 40.0f && sims[0].lastZ == 56.0f && sims[0].calls.back() == "pipe");
    agent->DoTornado(1, 1);
    CHECK(sims[4].lastX == 24.0f && sims[4].calls.back() == "tornado");

    // the messages
    for (auto& s : sims) s.calls.clear();
    message->SetType(0x99ef1142);
    message->SetData3(0x278128a0);
    message->SetVoid1(&building);
    agent->DoMessage(message);
    CHECK(sims[0].calls == std::vector<std::string>{"added"});
    message->SetData3(0x69d25c5f);
    agent->DoMessage(message);
    CHECK(sims[0].calls.back() == "fire" && sims[0].calls.size() == 2);
    message->SetData3(99);
    agent->DoMessage(message);
    CHECK(sims[0].calls.size() == 2);                                      // another occupant kind: ignored
    message->SetType(0x99ef1143);
    message->SetData3(0x278128a0);
    message->SetData4(0);
    agent->DoMessage(message);
    CHECK(sims[0].calls.back() == "removed");
    message->SetType(0x99ef1145);
    agent->DoMessage(message);
    CHECK(sims[0].calls.back() == "changed");
    message->SetType(0xa6b79602);
    agent->DoMessage(message);
    CHECK(sims[0].calls.back() == "update" && sims[5].calls.empty());
    message->SetType(0xea0d163e);
    message->SetData2(5); message->SetData3(6);
    agent->DoMessage(message);
    CHECK(sims[1].calls.back() == "neighbor" && sims[1].lastX2 == 5 && sims[1].lastZ2 == 6);
    message->SetType(0xea1f0dcd);
    message->SetData1(0); message->SetData2(0);
    agent->DoMessage(message);
    CHECK(sims[1].calls.back() == "pipe" && sims[1].lastX == 8.0f);
    message->SetType(0x44778230);
    message->SetData2(8);
    agent->DoMessage(message);
    CHECK(sims[1].calls.back() == "work");
    message->SetType(0x8a09fa6c);
    message->SetData1(42);
    Advisor holder;
    message->SetIGZUnknown(&holder);
    agent->DoMessage(message);                                             // the object does not answer 0x0a1da42f: nothing
    CHECK(sims[0].lastCommand == -1);
    message->SetType(0x1234);
    CHECK(agent->DoMessage(message));                                      // unknown messages are accepted and ignored

    // the sim panel bookkeeping
    agent->SetActiveUIPanelSim(3);
    CHECK(agent->ActivePanelIndex() == 2);
    int index = -1;
    CHECK(agent->GetActiveUIPanelSim(index) == &sims[2] && index == 2);
    agent->SetActiveUIPanelSim(99);
    CHECK(agent->ActivePanelIndex() == -1 && agent->GetActiveUIPanelSim(index) == nullptr);
    sims[5].slot = 6;
    agent->SetActiveUIPanelSim(6);
    CHECK(agent->ActivePanelIndex() == -1);                                // the special sim is not searched

    // move in: the first idle sim takes the newcomer
    cRZString name("a"), bio("b"), file("c");
    sims[0].active = true;
    agent->MoveIn(9, name, true, 3, bio, 77, file);
    CHECK(sims[0].moveSlot == 0 && sims[1].moveSlot == 9);
    for (auto& s : sims) s.active = true;
    sims[1].moveSlot = 0;
    agent->MoveIn(8, name, true, 3, bio, 77, file);
    CHECK(sims[1].moveSlot == 0 && sims[4].moveSlot == 0);                 // nobody idle: nothing happens

    // who lives where
    sims[2].residence = reinterpret_cast<cISC4BuildingOccupant*>(&building);
    CHECK(agent->MySimLivesHere(reinterpret_cast<cISC4BuildingOccupant*>(&building)) == &sims[2]);
    CHECK(agent->MySimLivesHere(reinterpret_cast<cISC4BuildingOccupant*>(&other)) == nullptr);

    // tracking: the tracked sim shows its residence and location, the rest hide theirs; all six are visited
    for (auto& s : sims) s.calls.clear();
    agent->TrackLocation(&sims[1]);
    CHECK(sims[1].calls == (std::vector<std::string>{"showres", "showloc", "pylon1"}));
    CHECK(sims[0].calls == (std::vector<std::string>{"hideres", "hideloc", "pylon0"}) && sims[5].calls.size() == 3);
    for (auto& s : sims) s.calls.clear();
    agent->TrackLocation(nullptr);
    CHECK(sims[3].calls.front() == "showres");
    for (auto& s : sims) s.calls.clear();
    agent->HideLocation();
    CHECK(sims[5].calls == (std::vector<std::string>{"hideres", "hideloc", "pylon0"}));

    // the console command: -print / -newjob:X / -newhouse:X / -dispatch become messages
    auto console = [&](const char* line) {
        message->SetType(0x230e27ac);
        message->SetData1(0xeb6f3c97);
        cRZString text(line);
        message->SetVoid2(&text);
        host.server.posted.clear();
        host.server.postedData.clear();
        agent->DoMessage(message);
    };
    console("-print:1");
    CHECK(host.server.posted == std::vector<std::uint32_t>{0x0b6f3e27} && host.server.postedData[0] == 0);   // (the print message carries no data)
    console("-print");
    CHECK(host.server.posted.empty());                                     // the value-reading overload needs "-print:value"
    console("-newjob:1f");
    CHECK(host.server.posted == std::vector<std::uint32_t>{0xab6f3e2d} && host.server.postedData[0] == 0x1f);
    console("-newhouse:a0");
    CHECK(host.server.posted == std::vector<std::uint32_t>{0x4b6f3ebf} && host.server.postedData[0] == 0xa0);
    console("-dispatch");
    CHECK(host.server.posted == std::vector<std::uint32_t>{0x0bb04126} && host.server.postedData[0] == 0);
    console("-other");
    CHECK(host.server.posted.empty());
    message->SetType(0x230e27ac);
    message->SetData1(5);
    host.server.posted.clear();
    agent->DoMessage(message);
    CHECK(host.server.posted.empty());                                     // another tag: ignored
    message->Release();

    // shutdown unregisters everything and lets go of the sims
    CHECK(agent->Shutdown() && host.unregistered == 1 && host.server.removed.size() == 11 && agent->Sim(0) == nullptr && sims[0].refs == 0);
    CHECK(agent->Shutdown());
    agent->Release();
    std::puts(failures ? "sc4_mysimagent: FAILED" : "sc4_mysimagent ok");
    return failures != 0;
}
