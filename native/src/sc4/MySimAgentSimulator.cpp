// cSC4MySimAgentSimulator — see sc4/MySimAgentSimulator.h.
#include "sc4/MySimAgentSimulator.h"

#include "rz/RZStringUtils.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CommonIIDs.h"

namespace sc4 {

namespace {
// QueryInterface(0x87dda3a9) on the occupant: the building view, one reference, or null for anything that is not a building.
cISC4BuildingOccupant* AsBuilding(cISC4Occupant* occupant) {
    void* raw = nullptr;
    if (!occupant || !occupant->QueryInterface(kMySimBuildingGroup, &raw) || !raw) return nullptr;
    return static_cast<cISC4BuildingOccupant*>(raw);
}
}  // namespace

cSC4MySimAgentSimulator::cSC4MySimAgentSimulator(IMySimAgentHost* host) : host_(host) {}

cSC4MySimAgentSimulator::~cSC4MySimAgentSimulator() {
    for (cISC4MySim*& sim : sims_) {
        if (sim) { cISC4MySim* old = sim; sim = nullptr; old->Release(); }
    }
}

// 0x1007aba6a: the primary interface, the message target and the serializable view.
bool cSC4MySimAgentSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    *out = nullptr;
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4MySimAgentSimulator) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cIGZSerializable) *out = static_cast<cIGZSerializable*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4MySimAgentSimulator::Release() {
    if (refCount_ > 1) return --refCount_;
    Shutdown();
    delete this;
    return 0;
}

void cSC4MySimAgentSimulator::ForEachSim(int count, const std::function<void(cISC4MySim&)>& each) const {
    for (int i = 0; i < count; ++i)
        if (sims_[i]) each(*sims_[i]);
}

int cSC4MySimAgentSimulator::SimCount() const { return kMySimAgentSlots; }

// 0x1007abba8: the city's advisors that are MySims fill the slots in key order; the advisor of type 0x1e takes the last one.
bool cSC4MySimAgentSimulator::Init() {
    if (initialized_) return true;
    initialized_ = true;
    std::vector<MySimAdvisorEntry> advisors;
    host_->Advisors(advisors);
    int next = 0;
    for (const MySimAdvisorEntry& entry : advisors) {
        cISC4MySim* sim = host_->AsMySim(entry.object);
        if (!sim) continue;
        cISC4MySim** slot = (entry.type == kMySimAdvisorTypeSpecial) ? &sims_[kMySimAgentSlots - 1] : &sims_[next++];
        if (*slot != sim) {
            cISC4MySim* old = *slot;
            sim->AddRef();
            *slot = sim;
            if (old) old->Release();
        }
        sim->Release();                            // the query's own reference
    }
    if (!serverAdded_ && host_->Messages()) {
        serverAdded_ = true;
        for (std::uint32_t id : {0x99ef1142u, 0x99ef1145u, 0x99ef1143u, 0xa6b79602u, 0xa6b79612u, 0x44778230u, 0x8a09fa6cu, 0xca1b1e64u,
                                 0xea1f0dcdu, 0x0a36f1e9u, 0xea0d163eu})
            host_->Messages()->AddNotification(static_cast<cIGZMessageTarget2*>(this), id);
    }
    host_->RegisterGameData(*this);
    return true;
}

// 0x1007ac1f8.
bool cSC4MySimAgentSimulator::Shutdown() {
    if (!initialized_) return true;
    host_->UnregisterGameData(*this);
    initialized_ = false;
    if (serverAdded_ && host_->Messages()) {
        for (std::uint32_t id : {0x99ef1142u, 0x99ef1145u, 0x99ef1143u, 0xa6b79602u, 0xa6b79612u, 0x44778230u, 0x8a09fa6cu, 0xca1b1e64u,
                                 0xea1f0dcdu, 0x0a36f1e9u, 0xea0d163eu})
            host_->Messages()->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), id);
    }
    serverAdded_ = false;
    for (cISC4MySim*& sim : sims_) {
        if (sim) { cISC4MySim* old = sim; sim = nullptr; old->Release(); }
    }
    return true;
}

// 0x1007ac3f6 / 0x1007ac44e: only a version number (1).
bool cSC4MySimAgentSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    return !stream.GetError();
}
bool cSC4MySimAgentSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return version == 1 && !stream.GetError();
}

// 0x1007acc0e: a new building reaches the five ordinary sims (they ask it for homes and jobs).
void cSC4MySimAgentSimulator::DoBuildingAdded(cISC4Occupant* building) {
    cISC4BuildingOccupant* view = AsBuilding(building);
    if (!view) return;
    ForEachSim(kMySimAgentOrdinarySlots, [&](cISC4MySim& sim) { sim.DoBuildingAdded(view); });
    reinterpret_cast<cIGZUnknown*>(view)->Release();
}
void cSC4MySimAgentSimulator::DoBuildingChanged(cISC4Occupant* building) {
    cISC4BuildingOccupant* view = AsBuilding(building);
    if (!view) return;
    ForEachSim(kMySimAgentOrdinarySlots, [&](cISC4MySim& sim) { sim.DoBuildingChanged(view); });
    reinterpret_cast<cIGZUnknown*>(view)->Release();
}
// 0x1007acdac: a building that went away in a disaster (data 4) while no riot is on carries the disaster type along.
void cSC4MySimAgentSimulator::DoBuildingRemoved(cISC4Occupant* building, std::uint32_t data) {
    std::uint32_t disaster = 0;
    if (data == kMySimDisasterEnd && !host_->RiotInProgress()) disaster = disasterType_;
    cISC4BuildingOccupant* view = AsBuilding(building);
    if (!view) return;
    ForEachSim(kMySimAgentOrdinarySlots, [&](cISC4MySim& sim) { sim.DoBuildingRemoved(view, disaster); });
    reinterpret_cast<cIGZUnknown*>(view)->Release();
}
void cSC4MySimAgentSimulator::DoFireAdded(cISC4Occupant* fire) {
    float position[3] = {0, 0, 0};
    fire->GetPosition(position);
    ForEachSim(kMySimAgentSlots - 1, [&](cISC4MySim& sim) { sim.DoFireAdded(position[0], position[2]); });
}
// 0x1007ace74: 8 o'clock the sims leave for work, 17 o'clock they come home, 22 o'clock the night sound.
void cSC4MySimAgentSimulator::DoHourChange(std::uint32_t hour) {
    if (hour == kMySimHourNight) ForEachSim(kMySimAgentOrdinarySlots, [](cISC4MySim& sim) { sim.PlayNightSound(); });
    else if (hour == kMySimHourReturnHome) ForEachSim(kMySimAgentOrdinarySlots, [](cISC4MySim& sim) { sim.ReturnHome(); });
    else if (hour == kMySimHourLeaveForWork) ForEachSim(kMySimAgentOrdinarySlots, [](cISC4MySim& sim) { sim.LeaveForWork(); });
}
void cSC4MySimAgentSimulator::DoDisasterStarted(cIGZMessage2Standard* message) {
    const int x = static_cast<int>(message->GetData2());
    const int z = static_cast<int>(message->GetData3());
    disasterType_ = static_cast<std::uint32_t>(message->GetData4());
    ForEachSim(kMySimAgentOrdinarySlots, [&](cISC4MySim& sim) { sim.DoDisaster(disasterType_, x, z); });
}
void cSC4MySimAgentSimulator::DoPipeBurst(int cellX, int cellZ) {
    float x = 0, z = 0;
    host_->CellCenter(cellX, cellZ, x, z);
    ForEachSim(kMySimAgentOrdinarySlots, [&](cISC4MySim& sim) { sim.DoPipeBurst(x, z); });
}
void cSC4MySimAgentSimulator::DoTornado(int cellX, int cellZ) {
    float x = 0, z = 0;
    host_->CellCenter(cellX, cellZ, x, z);
    ForEachSim(kMySimAgentOrdinarySlots, [&](cISC4MySim& sim) { sim.DoTornado(x, z); });
}

// 0x1007ad144: the sim whose slot number matches, or -1.
void cSC4MySimAgentSimulator::SetActiveUIPanelSim(int slot) {
    for (int i = 0; i < kMySimAgentOrdinarySlots; ++i) {
        if (sims_[i] && static_cast<int>(sims_[i]->GetSlot()) == slot) { activePanel_ = i; return; }
    }
    activePanel_ = -1;
}
bool cSC4MySimAgentSimulator::GetActiveMySims(std::vector<cRZAutoRefCount<cISC4MySim>>& out) const {
    out.clear();
    for (int i = 0; i < kMySimAgentOrdinarySlots; ++i) {
        if (sims_[i] && sims_[i]->IsActive()) out.emplace_back(sims_[i]);
    }
    return !out.empty();
}
cISC4MySim* cSC4MySimAgentSimulator::GetActiveUIPanelSim(int& slot) {
    if (activePanel_ < 0 || activePanel_ >= kMySimAgentSlots) return nullptr;
    slot = activePanel_;
    return sims_[activePanel_];
}
// 0x1007ad1c2: the first idle sim takes the newcomer.
void cSC4MySimAgentSimulator::MoveIn(std::uint8_t slot, cRZString& name, bool male, std::uint8_t zodiac, cRZString& bio,
                                      std::uint32_t bitmapId, cRZString& bitmapFile) {
    for (int i = 0; i < kMySimAgentOrdinarySlots; ++i) {
        if (sims_[i] && !sims_[i]->IsActive()) {
            sims_[i]->MoveIn(slot, name, male, zodiac, bio, bitmapId, bitmapFile);
            return;
        }
    }
}
// 0x1007ad24c: the tracked sim shows its residence and location; every other sim hides them.
void cSC4MySimAgentSimulator::TrackLocation(cISC4MySim* tracked) {
    for (int i = 0; i < kMySimAgentSlots; ++i) {
        cISC4MySim* sim = sims_[i];
        if (!sim) continue;
        if (tracked == nullptr || sim == tracked) {
            sim->ShowResidence();
            sim->ShowLocation();
            sim->SetPylonVisibility(true);
        } else {
            sim->HideResidence();
            sim->HideLocation();
            sim->SetPylonVisibility(false);
        }
    }
}
void cSC4MySimAgentSimulator::HideLocation() {
    ForEachSim(kMySimAgentSlots, [](cISC4MySim& sim) { sim.HideResidence(); sim.HideLocation(); sim.SetPylonVisibility(false); });
}
cISC4MySim* cSC4MySimAgentSimulator::MySimLivesHere(cISC4BuildingOccupant* building) {
    for (int i = 0; i < kMySimAgentOrdinarySlots; ++i)
        if (sims_[i] && sims_[i]->GetResidence() == building) return sims_[i];
    return nullptr;
}

// 0x1007ac4d2.
bool cSC4MySimAgentSimulator::DoMessage(cIGZMessage2* base) {
    auto* message = static_cast<cIGZMessage2Standard*>(base);
    const std::uint32_t type = message->GetType();
    switch (type) {
        case kMySimMsgScriptCommand: {
            cIGZUnknown* object = message->GetIGZUnknown();
            void* raw = nullptr;
            if (object && object->QueryInterface(kGZIID_cISC4MySimLink, &raw) && raw) {
                auto* sim = static_cast<cISC4MySim*>(raw);
                sim->DoScriptCommand(static_cast<int>(message->GetData1()));
                sim->Release();
            }
            return true;
        }
        case kMySimMsgOccupantAdded: {
            const std::uint64_t kind = message->GetData3();
            auto* occupant = static_cast<cISC4Occupant*>(message->GetVoid1());
            if (kind == kMySimOccupantTypeFire) DoFireAdded(occupant);
            else if (kind == kMySimOccupantTypeBuilding) DoBuildingAdded(occupant);
            return true;
        }
        case kMySimMsgOccupantRemoved:
            if (message->GetData3() == kMySimOccupantTypeBuilding)
                DoBuildingRemoved(static_cast<cISC4Occupant*>(message->GetVoid1()), static_cast<std::uint32_t>(message->GetData4()));
            return true;
        case kMySimMsgOccupantChanged:
            if (message->GetData3() == kMySimOccupantTypeBuilding) DoBuildingChanged(static_cast<cISC4Occupant*>(message->GetVoid1()));
            return true;
        case kMySimMsgResidenceA:
        case kMySimMsgResidenceB:
            ForEachSim(kMySimAgentOrdinarySlots, [](cISC4MySim& sim) { sim.UpdateResidence(); });
            return true;
        case kMySimMsgNeighborConnection: {
            const int x = static_cast<int>(message->GetData2()), z = static_cast<int>(message->GetData3());
            ForEachSim(kMySimAgentOrdinarySlots, [&](cISC4MySim& sim) { sim.DoNeighborConnectionRemoved(x, z); });
            return true;
        }
        case kMySimMsgTornado:
            DoTornado(static_cast<int>(message->GetData2()), static_cast<int>(message->GetData3()));
            return true;
        case kMySimMsgDisasterStarted:
            DoDisasterStarted(message);
            return true;
        case kMySimMsgPipeBurst:
            DoPipeBurst(static_cast<int>(message->GetData1()), static_cast<int>(message->GetData2()));
            return true;
        case kMySimMsgHour:
            DoHourChange(static_cast<std::uint32_t>(message->GetData2()));
            return true;
        case kMySimMsgConsole: {
            if (static_cast<std::uint32_t>(message->GetData1()) != kMySimConsoleCommandTag) return true;
            auto* text = static_cast<cIGZString*>(message->GetVoid2());
            cRZCmdLine commandLine(text->ToChar());
            cRZString value;
            const auto post = [&](std::uint32_t outType, bool withValue) {
                auto* out = new cRZMessage2Standard();
                out->AddRef();
                out->SetType(outType);
                if (withValue) out->SetData1(RZAtoUint32HexA(value.ToChar()));
                if (host_->Messages()) host_->Messages()->MessagePost(out, false);
                out->Release();
            };
            if (commandLine.IsSwitchPresent(cRZString("print"), value, true)) post(kMySimOutPrint, false);
            else if (commandLine.IsSwitchPresent(cRZString("newjob"), value, true)) post(kMySimOutNewJob, true);
            else if (commandLine.IsSwitchPresent(cRZString("newhouse"), value, true)) post(kMySimOutNewHouse, true);
            else if (commandLine.IsSwitchPresent(cRZString("dispatch"))) post(kMySimOutDispatch, true);
            return true;
        }
        default: return true;
    }
}

}  // namespace sc4
