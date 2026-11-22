// The city situation manager's view of the recovered automata and occupants (the sc4/CitySituationEntities.h seams): in
// the original the manager calls cSC4Automaton / cISC4Occupant / cSC4AutomataManager / cSC4OccupantManager directly; here
// small views forward each slot to the engine object's method at the same vtable offset.
//   - an automaton's view is attached to the automaton (the companion slot of its managers' view, deleted with it) and
//     shares the automaton's reference count; its occupant view is part of it;
//   - any other occupant gets one view per object while it is referenced (the view holds the occupant);
//   - the vehicle, aircraft and pedestrian managers and the occupant manager get one view each.
// A translation unit of its own because the automata headers do not combine with the situation manager's.
#include <cmath>
#include "platform/Unwired.h"
#include <cstdint>
#include <map>
#include <unordered_set>
#include <vector>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "rz/cRZString.h"
#include "sc4/AutomataManager.h"
#include "sc4/Automaton.h"
#include "sc4/AutomataDriveParams.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/CitySituationEntities.h"
#include "sc4/CommonIIDs.h"
#include "sc4/OccupantGroupTemplate.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4City.h"

namespace native {
const sc4::tAutomataPlayerDriveParams* NativePlayerDriveParams(sc4::cSC4Automaton* automaton);   // NativePlayerDrive.cpp

namespace {
using namespace sc4;

class SituationAutomatonView;
SituationAutomatonView* ViewOfAutomaton(cSC4Automaton* automaton);

// The property holder of an engine occupant.
class PropertyView final : public ISituationPropertyHolder {
public:
    explicit PropertyView(cISC4Occupant& occupant) : o_(occupant) {}
    bool HasProperty(std::uint32_t id) override {
        cISCPropertyHolder* holder = o_.AsPropertyHolder();
        return holder && holder->HasProperty(id);
    }
    bool GetPropertyU32(std::uint32_t id, std::uint32_t& value) override {
        cISCPropertyHolder* holder = o_.AsPropertyHolder();
        return holder && cSCPropertyHelp::GetPropertyValue(holder, id, value);
    }

private:
    cISC4Occupant& o_;
};

// The cISC4Occupant slots the manager uses. The reference count is supplied by the owner (the automaton, or the plain
// occupant view itself).
class OccupantSlots : public ISituationOccupant {
public:
    explicit OccupantSlots(cISC4Occupant& occupant) : o_(occupant), properties_(occupant) {}
    cISC4Occupant& Engine() const { return o_; }
    bool QueryInterface(std::uint32_t iid, void** out) override { return o_.QueryInterface(iid, out); }
    ISituationPropertyHolder* PropertyHolder() override { return &properties_; }      // 0x30
    std::uint32_t OccupantType() override { return o_.GetType(); }                     // 0x38
    void Position(float xyz[3]) override { o_.GetPosition(xyz); }                       // 0x40
    void SetPosition(const float xyz[3]) override { o_.SetPosition(xyz); }              // 0x48
    bool IsInGroup(std::uint32_t group) override { return o_.IsOccupantGroup(group); }  // 0xb0
    void SetGroup(std::uint32_t group) override { o_.AddOccupantGroup(group); }         // 0xb8
    ISituationAutomaton* AsAutomaton() override;                                        // QueryInterface(0xa9b40f05)
    // Recovered-partial: the network occupant queries feed the coverage missions, which are not recovered.
    bool IsNetworkOccupant() override {
        SC4_UNWIRED("cSC4CitySituation occupant: the network occupant queries (coverage missions) are not recovered (never a network piece)");
        return false;
    }
    bool NetworkFlagsMatch(std::uint32_t) override { return false; }
    void NetworkCell(int& x, int& z) override { x = z = 0; }

private:
    cISC4Occupant& o_;
    PropertyView properties_;
};

// ---- the automaton --------------------------------------------------------------------------------------------------------
class SituationAutomatonView final : public ISituationAutomaton, public cSC4AutomatonExtension {
public:
    explicit SituationAutomatonView(cSC4Automaton& automaton) : a_(automaton), occupant_(automaton, *this) {}
    cSC4Automaton& Engine() const { return a_; }

    bool QueryInterface(std::uint32_t iid, void** out) override { return a_.QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return a_.AddRef(); }
    std::uint32_t Release() override { return a_.Release(); }

    ISituationOccupant* Occupant() override { return &occupant_; }                               // 0x30
    void Position(float xyz[3]) override { a_.GetPosition(xyz); }                                // 0x38
    void Remove(bool fade) override { a_.Remove(fade); }                                         // 0x68
    void GetOrientation(float xyz[3]) override { a_.GetOrientation(xyz); }                       // 0x78
    void SetFlag(std::uint32_t flag, bool on) override { a_.SetAutomataFlag(flag, on); }         // 0xf0
    bool CheckFlag(std::uint32_t flag) override { return a_.CheckAutomataFlag(flag); }           // 0xf8
    void AddListener(cIGZMessageTarget2* target) override { a_.AddCallback(target); }            // 0x138
    void RemoveListener(cIGZMessageTarget2* target) override { a_.RemoveCallback(target); }      // 0x140
    // 0x1c0: the pursuit state's target. PARTIAL: the recovered automaton states keep no chase target natively.
    ISituationAutomaton* StateTarget() override {
        SC4_UNWIRED("cSC4CitySituation automaton: the pursuit state's chase target (vslot 0x1c0) is not kept natively");
        return nullptr;
    }
    std::uint32_t GetBehavior() override { return a_.GetBehavior(); }                            // 0x1d8
    void StartSFX(std::uint32_t sfx) override { a_.StartSFX(sfx); }                              // 0x1e8
    // 0x218 -> cSC4OccupantGroupTemplate::GetSituationIndicatorImageId: "csi_image" of the group or of its parents.
    std::uint32_t SituationIndicatorImageId() override {
        for (const cSC4OccupantGroupTemplate* group = OccupantGroupTemplateOf(a_.GetAutomataGroup()); group; group = group->parent)
            if (group->csiImage != 0) return group->csiImage;
        return 0;
    }
    void RegisterInGameDataRegistry() override { a_.RegisterInGameDataRegistry(); }              // 0x220
    bool PlayerDriveMaxSpeed(float& speed) override {
        const tAutomataPlayerDriveParams* params = NativePlayerDriveParams(&a_);
        if (!params) return false;
        speed = params->F(0x8);
        return true;
    }

private:
    // The automaton's own occupant view: its reference count is the automaton's.
    class OwnOccupant final : public OccupantSlots {
    public:
        OwnOccupant(cSC4Automaton& automaton, SituationAutomatonView& owner) : OccupantSlots(automaton), owner_(owner) {}
        std::uint32_t AddRef() override { return owner_.AddRef(); }
        std::uint32_t Release() override { return owner_.Release(); }
        ISituationAutomaton* AsAutomaton() override { return &owner_; }

    private:
        SituationAutomatonView& owner_;
    };

    cSC4Automaton& a_;
    OwnOccupant occupant_;
};

SituationAutomatonView* ViewOfAutomaton(cSC4Automaton* automaton) {
    if (!automaton) return nullptr;
    if (auto* view = dynamic_cast<SituationAutomatonView*>(AutomatonCompanion(automaton))) return view;
    auto* view = new SituationAutomatonView(*automaton);
    SetAutomatonCompanion(automaton, view);
    return view;
}

// ---- any other occupant -----------------------------------------------------------------------------------------------------
class PlainOccupantView;
std::map<cISC4Occupant*, PlainOccupantView*> g_plainOccupants;

class PlainOccupantView final : public OccupantSlots {
public:
    explicit PlainOccupantView(cISC4Occupant& occupant) : OccupantSlots(occupant) { occupant.AddRef(); }
    ~PlainOccupantView() { Engine().Release(); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        g_plainOccupants.erase(&Engine());
        delete this;
        return 0;
    }

private:
    std::uint32_t refs_ = 0;
};

ISituationAutomaton* OccupantSlots::AsAutomaton() {
    auto* automaton = dynamic_cast<cSC4Automaton*>(&o_);
    return automaton ? ViewOfAutomaton(automaton) : nullptr;
}

// The canonical view of an engine occupant (the automaton's own occupant view for an automaton); not counted.
ISituationOccupant* ViewOfOccupant(cISC4Occupant* occupant) {
    if (!occupant) return nullptr;
    if (auto* automaton = dynamic_cast<cSC4Automaton*>(occupant)) return ViewOfAutomaton(automaton)->Occupant();
    auto it = g_plainOccupants.find(occupant);
    if (it != g_plainOccupants.end()) return it->second;
    auto* view = new PlainOccupantView(*occupant);
    g_plainOccupants[occupant] = view;
    return view;
}

// ---- the automata managers --------------------------------------------------------------------------------------------------
class ManagerView final : public ISituationAutomataManager {
public:
    explicit ManagerView(CityComponent component) : component_(component) {}
    cSC4AutomataManager* Engine() const {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(component_) : nullptr;
        return object ? dynamic_cast<cSC4AutomataManager*>(object) : nullptr;
    }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }    // the city owns the managers; the views live for the process
    std::uint32_t Release() override { return 1; }
    void Pause() override { if (cSC4AutomataManager* m = Engine()) m->Pause(); }                     // 0x48
    void Resume() override { if (cSC4AutomataManager* m = Engine()) m->Resume(); }                   // 0x50
    bool HandlesGroup(std::uint32_t group) override { cSC4AutomataManager* m = Engine(); return m && m->CanCreateGroup(group); }   // 0xf0
    void AlertGroup(float radius, std::uint32_t group, ISituationAutomaton* target, bool all) override {   // 0xb0
        cSC4AutomataManager* m = Engine();
        auto* view = dynamic_cast<SituationAutomatonView*>(target);
        if (m && view) m->ChaseAutomaton(group, radius, AutomatonView(&view->Engine()), all);
    }
    // 0x100: the engine set holds one reference per automaton; the situation set keeps that reference until 0x108.
    void AppendInstancesOfGroup(std::uint32_t group, std::unordered_set<ISituationAutomaton*>& out) override {
        cSC4AutomataManager* m = Engine();
        if (!m) return;
        std::unordered_set<IAutomataAutomaton*> engine;
        m->GetInstancesOfGroup(group, engine);
        for (IAutomataAutomaton* automaton : engine) {
            SituationAutomatonView* view = ViewOfAutomaton(AutomatonOf(automaton));
            if (view && out.insert(view).second) view->AddRef();
        }
        m->ReleaseAutomatonSet(engine);
    }
    void ReleaseInstances(std::unordered_set<ISituationAutomaton*>& list) override {   // 0x108
        for (ISituationAutomaton* automaton : list)
            if (automaton) automaton->Release();
        list.clear();
    }
    bool FindSpawnPosition(float minDistance, float maxDistance, std::uint32_t group, const float from[3], float spawnPosition[3],
                           std::uint32_t flags) override {   // 0x80
        cSC4AutomataManager* m = Engine();
        if (!m) return false;
        const cS3DVector3 origin{from[0], from[1], from[2]};
        cS3DVector3 where{from[0], from[1], from[2]};
        if (!m->GetRandomCreationPoint(group, origin, minDistance, maxDistance, where, flags, nullptr)) return false;
        spawnPosition[0] = where.x;
        spawnPosition[1] = where.y;
        spawnPosition[2] = where.z;
        return true;
    }
    ISituationAutomaton* CreateAutomaton(std::uint32_t group, const float position[3], std::uint32_t flags) override {   // 0x78
        cSC4AutomataManager* m = Engine();
        if (!m) return nullptr;
        IAutomataAutomaton* made = m->CreateAutomaton(group, cS3DVector3{position[0], position[1], position[2]}, flags,
                                                      cGZPersistResourceKey{}, nullptr, 0xff, nullptr);
        SituationAutomatonView* view = made ? ViewOfAutomaton(AutomatonOf(made)) : nullptr;
        if (!view) return nullptr;
        view->AddRef();   // the caller's reference (the manager keeps its own)
        return view;
    }

private:
    CityComponent component_;
};

ManagerView g_vehicles(CityComponent::VehicleManager), g_aircraft(CityComponent::AircraftManager),
    g_pedestrians(CityComponent::PedestrianManager), g_watercraft(CityComponent::WatercraftManager);

// cSC4OccupantFilterByGroupAndDistance (i386 0x007fca00): the group filter, and with a minimum distance the occupants
// closer than it (measured at the center's height) are left out.
class GroupAndDistanceFilter final : public cSC4OccupantGroupOccupantFilter {
public:
    GroupAndDistanceFilter(const std::vector<std::uint32_t>& groups, const float center[3], float minDistanceSquared)
        : cSC4OccupantGroupOccupantFilter(groups), minDistanceSquared_(minDistanceSquared) {
        center_[0] = center[0];
        center_[1] = center[1];
        center_[2] = center[2];
    }
    bool IsOccupantIncluded(cISC4Occupant* occupant) override {
        if (!cSC4OccupantGroupOccupantFilter::IsOccupantIncluded(occupant)) return false;
        if (minDistanceSquared_ > 0.0f) {
            float position[3] = {0, 0, 0};
            occupant->GetPosition(position);
            position[1] = center_[1];
            const float dx = position[0] - center_[0], dy = position[1] - center_[1], dz = position[2] - center_[2];
            if (dx * dx + dy * dy + dz * dz < minDistanceSquared_) return false;
        }
        return true;
    }

private:
    float minDistanceSquared_;
    float center_[3];
};

class OccupantManagerView final : public ISituationOccupantManager {
public:
    static cISC4OccupantManager* Engine() {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
        auto* manager = static_cast<cISC4OccupantManager*>(raw);
        manager->Release();   // the city keeps the manager
        return manager;
    }
    // vslot 0x128 GetOccupantsByBBox(list, min, max, filter, count); a null box is the whole city. Each view comes with one
    // reference, which ReleaseOccupants returns.
    std::vector<ISituationOccupant*> OccupantsInBox(const float boxMin[3], const float boxMax[3], const std::vector<std::uint32_t>& groups,
                                                    const float center[3], float minDistanceSquared, std::uint32_t maxCount) override {
        std::vector<ISituationOccupant*> out;
        cISC4OccupantManager* manager = Engine();
        if (!manager) return out;
        auto* filter = new GroupAndDistanceFilter(groups, center, minDistanceSquared);
        filter->AddRef();
        OccupantList list;
        manager->GetOccupantsByBBox(list, boxMin, boxMax, filter, maxCount);
        for (cISC4Occupant* occupant : list) {
            ISituationOccupant* view = ViewOfOccupant(occupant);
            if (!view) continue;
            view->AddRef();
            out.push_back(view);
        }
        manager->ReleaseOccupantList(list);
        filter->Release();
        return out;
    }
    std::vector<ISituationOccupant*> NetworkOccupantsInBox(const float*, const float*, std::uint32_t, const float*, float) override {
        SC4_UNWIRED("cSC4CitySituation: the network occupants in a box (coverage missions) are not answered");
        return {};   // Recovered-partial: only the coverage missions ask for network occupants.
    }
    void ReleaseOccupants(std::vector<ISituationOccupant*>& list) override {   // vslot 0x180
        for (ISituationOccupant* occupant : list)
            if (occupant) occupant->Release();
        list.clear();
    }
    std::vector<ISituationOccupant*> VisibleOccupantsOfGroup(std::uint32_t, bool) override { return {}; }
} g_occupants;

}  // namespace

// ---- what the situation host hands out ------------------------------------------------------------------------------------
// The occupant of an occupant-manager message: an automaton's own view, or the view a reference already keeps; an
// occupant nothing in the manager refers to answers null (only automata and referenced targets matter to the messages).
sc4::ISituationOccupant* SituationOccupantOf(void* engineOccupant) {
    auto* occupant = static_cast<cISC4Occupant*>(engineOccupant);
    if (!occupant) return nullptr;
    if (auto* automaton = dynamic_cast<cSC4Automaton*>(occupant)) return ViewOfAutomaton(automaton)->Occupant();
    auto it = g_plainOccupants.find(occupant);
    return it != g_plainOccupants.end() ? it->second : nullptr;
}
// The view of any engine occupant, made when it has none yet (not counted).
sc4::ISituationOccupant* SituationViewOfOccupant(void* engineOccupant) { return ViewOfOccupant(static_cast<cISC4Occupant*>(engineOccupant)); }
sc4::ISituationAutomaton* SituationAutomatonOf(void* engineAutomaton) {
    auto* automaton = static_cast<cSC4Automaton*>(engineAutomaton);
    return ViewOfAutomaton(automaton);
}
int SituationAutomataManagerCount() { return 4; }
// The original's order (Init): vehicle, aircraft, pedestrian, watercraft. A manager that is not a cSC4AutomataManager answers
// through its view as an empty one.
sc4::ISituationAutomataManager* SituationAutomataManager(int index) {
    switch (index) {
        case 0: return &g_vehicles;
        case 1: return &g_aircraft;
        case 2: return &g_pedestrians;
        case 3: return &g_watercraft;
        default: return nullptr;
    }
}
sc4::ISituationOccupantManager* SituationOccupantManager() { return OccupantManagerView::Engine() ? &g_occupants : nullptr; }
// The engine automaton behind a situation view (null for anything else).
sc4::cSC4Automaton* SituationEngineAutomaton(sc4::ISituationAutomaton* automaton) {
    auto* view = dynamic_cast<SituationAutomatonView*>(automaton);
    return view ? &view->Engine() : nullptr;
}
// The engine occupant behind a situation occupant view (null for anything else).
cIGZUnknown* SituationEngineOccupant(sc4::ISituationOccupant* occupant) {
    auto* view = dynamic_cast<OccupantSlots*>(occupant);
    return view ? static_cast<cIGZUnknown*>(&view->Engine()) : nullptr;
}
// cSC4AutomataManager::EnterPlayerDriveView(automaton, target) (the vehicle manager's).
void SituationEnterPlayerDriveView(sc4::ISituationAutomaton* automaton, cIGZMessageTarget2* target) {
    cSC4AutomataManager* vehicles = g_vehicles.Engine();
    cSC4Automaton* engine = SituationEngineAutomaton(automaton);
    if (vehicles && engine) vehicles->EnterPlayerDriveView(AutomatonView(engine), target);
}
// SL::spCity vslot 0x180 (the automata script system) -> GetAutomataGroup(id): the template's source building group,
// else its parents' (cSC4OccupantGroupTemplate::GetSourceBuildingGroupId, i386 0x0063b8f0).
std::uint32_t SituationSourceBuildingGroup(std::uint32_t automataGroup) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::AutomataScriptSystem) : nullptr;
    auto* scripts = object ? dynamic_cast<cSC4AutomataScriptSystem*>(object) : nullptr;
    void* raw = nullptr;
    if (!scripts && object && object->QueryInterface(kGZIID_PlainObject, &raw) && raw) scripts = static_cast<cSC4AutomataScriptSystem*>(raw);
    const cSC4OccupantGroupTemplate* group = scripts ? scripts->GetAutomataGroup(automataGroup) : nullptr;
    for (; group; group = group->parent)
        if (group->sourceBuilding != 0) return group->sourceBuilding;
    return 0;
}

}  // namespace native
