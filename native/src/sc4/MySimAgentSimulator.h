// cSC4MySimAgentSimulator — the MySim agent simulator (vtable 0x100aef860, class id 0x29e37dec, interface 0xa9e37ebb, 0x60 bytes): the one
// object that owns the five MySims of the city (plus the extra one of advisor type 0x1e), forwards every city event to them and answers the
// Lua queries get_sim / get_count.
//
// Layout: +0x18 initialised, +0x20 vector<cRZAutoRefCount<cISC4MySim>> (6 entries), +0x38 the running disaster's type, +0x48 counted city,
// +0x50 counted message server, +0x58 index of the sim whose panel is open (-1). Three views: the primary interface, +8 the message
// target (iid 0x452294aa) and +0x10 the serializable view (iid 0xe4fda3d4).
//
// The sims, the city's advisor map, the message server, the game data registry and the command line are behind IMySimAgentHost.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "sc4/MySim.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4MySimAgentSimulator = 0x29e37dec;
constexpr std::uint32_t kGZIID_cISC4MySimAgentSimulator = 0xa9e37ebb;
constexpr int kMySimAgentSlots = 6;                    // five by order of discovery, the last one is the advisor type 0x1e
constexpr int kMySimAgentOrdinarySlots = 5;
constexpr int kMySimAdvisorTypeSpecial = 0x1e;
constexpr std::uint32_t kMySimOccupantTypeBuilding = 0x278128a0, kMySimOccupantTypeFire = 0x69d25c5f;   // message Data3 of occupant added / removed
constexpr std::uint32_t kMySimBuildingGroup = 0x87dda3a9;                          // occupant group asked of a building
constexpr std::uint32_t kMySimMsgScriptCommand = 0x8a09fa6c, kMySimMsgOccupantAdded = 0x99ef1142, kMySimMsgOccupantRemoved = 0x99ef1143,
                        kMySimMsgOccupantChanged = 0x99ef1145, kMySimMsgResidenceA = 0xa6b79602, kMySimMsgResidenceB = 0xa6b79612,
                        kMySimMsgHour = 0x44778230, kMySimMsgTornado = 0x0a36f1e9, kMySimMsgDisasterStarted = 0xca1b1e64,
                        kMySimMsgPipeBurst = 0xea1f0dcd, kMySimMsgNeighborConnection = 0xea0d163e, kMySimMsgConsole = 0x230e27ac;
constexpr std::uint32_t kMySimConsoleCommandTag = 0xeb6f3c97;                      // Data1 of the console message this handler reads
constexpr std::uint32_t kMySimOutPrint = 0x0b6f3e27, kMySimOutNewJob = 0xab6f3e2d, kMySimOutNewHouse = 0x4b6f3ebf, kMySimOutDispatch = 0x0bb04126;
constexpr std::uint32_t kMySimHourReturnHome = 0x11, kMySimHourLeaveForWork = 8, kMySimHourNight = 0x16;
constexpr std::uint32_t kMySimDisasterEnd = 4;

struct MySimAdvisorEntry {
    cIGZUnknown* object;       // the advisor (answers cISC4MySim through AsMySim)
    int type;                  // cISC4Advisor::GetID
};

class cSC4MySimAgentSimulator;

class IMySimAgentHost {
public:
    virtual ~IMySimAgentHost() = default;
    virtual void Advisors(std::vector<MySimAdvisorEntry>& out) = 0;            // spCity vslot 0x288 -> vslot 0x40: the advisors in key order
    virtual cISC4MySim* AsMySim(cIGZUnknown* object) = 0;                     // QueryInterface(0x0a1da42f), counted
    virtual cIGZMessageServer2* Messages() = 0;                                // SL::spMS2
    virtual void RegisterGameData(cSC4MySimAgentSimulator& agent) = 0;         // the registry's AddValue("get_sim"), AddValue("get_count")
    virtual void UnregisterGameData(cSC4MySimAgentSimulator& agent) = 0;       // the registry's vslot 0x30(0)
    virtual bool RiotInProgress() = 0;                                         // spCity vslot 0x240 -> vslot 0x50
    virtual void CellCenter(int cellX, int cellZ, float& x, float& z) = 0;     // spCity vslot 0x308
};

class cSC4MySimAgentSimulator : public cIGZSerializable, public cIGZMessageTarget2 {
public:
    explicit cSC4MySimAgentSimulator(IMySimAgentHost* host);                  // @ 0x1007ab82e
    ~cSC4MySimAgentSimulator();                                               // @ 0x1007ab99c

    bool QueryInterface(std::uint32_t iid, void** out) override;              // @ 0x1007aba6a
    std::uint32_t AddRef() override { return ++refCount_; }                   // @ 0x1007abad0
    std::uint32_t Release() override;                                         // @ 0x1007abafa

    bool Init();                                                              // 0x18 @ 0x1007abba8
    bool Shutdown();                                                          // 0x20 @ 0x1007ac1f8
    bool Write(cIGZOStream& stream) const override;                           // @ 0x1007ac3f6
    bool Read(cIGZIStream& stream) override;                                  // @ 0x1007ac44e
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4MySimAgentSimulator; }   // @ 0x1007ad384
    bool DoMessage(cIGZMessage2* message) override;                           // @ 0x1007ac4d2

    void DoBuildingAdded(cISC4Occupant* building);                            // @ 0x1007acc0e
    void DoFireAdded(cISC4Occupant* fire);                                    // @ 0x1007acca2
    void DoBuildingChanged(cISC4Occupant* building);                          // @ 0x1007acd18
    void DoBuildingRemoved(cISC4Occupant* building, std::uint32_t data);      // @ 0x1007acdac
    void DoHourChange(std::uint32_t hour);                                    // @ 0x1007ace74
    void DoDisasterStarted(cIGZMessage2Standard* message);                    // @ 0x1007acefa
    void DoPipeBurst(int cellX, int cellZ);                                   // @ 0x1007acf76
    void DoTornado(int cellX, int cellZ);                                     // @ 0x1007acfce
    void SetActiveUIPanelSim(int slot);                                       // @ 0x1007ad144
    cISC4MySim* GetActiveUIPanelSim(int& slot);                               // @ 0x1007ad190 (null when no panel is open)
    void MoveIn(std::uint8_t slot, cRZString& name, bool male, std::uint8_t zodiac, cRZString& bio, std::uint32_t bitmapId, cRZString& bitmapFile);   // @ 0x1007ad1c2
    void TrackLocation(cISC4MySim* sim);                                      // @ 0x1007ad24c
    void HideLocation();                                                      // @ 0x1007ad2ce
    cISC4MySim* MySimLivesHere(cISC4BuildingOccupant* building);              // @ 0x1007ad324
    void CancelMoveIn() {}                                                    // @ 0x1007ad37a (returns at once)
    // i386 0x0054defa: the active sims of the five ordinary slots (one reference each); true when there is one.
    bool GetActiveMySims(std::vector<cRZAutoRefCount<cISC4MySim>>& out) const;

    cISC4MySim* Sim(int slot) const { return sims_[slot]; }
    int ActivePanelIndex() const { return activePanel_; }
    std::uint32_t DisasterType() const { return disasterType_; }
    bool Initialized() const { return initialized_; }
    int SimCount() const;                                                     // the Lua get_count: entries of the vector (6)

private:
    void ForEachSim(int count, const std::function<void(cISC4MySim&)>& each) const;

    IMySimAgentHost* host_ = nullptr;
    bool initialized_ = false;                     // +0x18
    std::uint32_t refCount_ = 0;
    cISC4MySim* sims_[kMySimAgentSlots] = {};      // +0x20 vector, counted
    std::uint32_t disasterType_ = 0;               // +0x38
    int activePanel_ = -1;                         // +0x58
    bool serverAdded_ = false;                     // +0x50 (counted message server)
};

}  // namespace sc4
