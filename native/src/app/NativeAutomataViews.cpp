// The automata managers' view of the recovered automata: one view per automaton, attached to it (cSC4Automaton::SetExtension),
// forwarding each IAutomataAutomaton / ITrafficAutomataAutomaton slot to the automaton's method at the same original vtable
// offset (the offsets are the interface's comments in sc4/AutomataManager.h and TrafficAutomataManager.h).
#include "app/NativeAutomataViews.h"

#include <map>
#include <memory>
#include <set>

#include "rz/cRZVariant.h"
#include "sc4/AutomataManager.h"
#include "sc4/Automaton.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Exemplar.h"
#include "sc4/OccupantGroupTemplate.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/TrafficAutomaton.h"

namespace native {
namespace {
using namespace sc4;

// ---- script group templates ----------------------------------------------------------------------------------------------
// One template seen by both halves: the managers' IAutomataGroupTemplate and the automaton's IAutomatonGroupTemplate
// (SetAutomataGroup / PostInit). Their Parent() slots return different types, so each half gets its own small view.
struct GroupPair;
GroupPair* PairFor(cSC4OccupantGroupTemplate* group);

class ManagerGroupView final : public IAutomataGroupTemplate {
public:
    explicit ManagerGroupView(cSC4OccupantGroupTemplate* group) : group_(group) {}
    std::uint32_t GroupId() const override { return group_->groupId; }
    std::uint32_t ClassId() const override { return group_->GetClassID(); }
    cRZVariant& GroupIdVariant() override { return group_->GetGroupIDVariant(); }
    IAutomataGroupTemplate* Parent() const override;
    std::uint32_t SourceBuilding() const override { return group_->sourceBuilding; }
    bool IsOccupantGroup(std::uint32_t group) const override { return group_->IsOccupantGroup(group); }
    cSC4OccupantGroupTemplate* group_;
};

class AutomatonGroupView final : public IAutomatonGroupTemplate {
public:
    explicit AutomatonGroupView(cSC4OccupantGroupTemplate* group) : group_(group) {}
    std::uint32_t GroupId() const override { return group_->groupId; }
    IAutomatonGroupTemplate* Parent() const override;
    float SpeedMin() const override { return group_->kphMin; }
    float SpeedMax() const override { return group_->kphMax; }
    void GetGroupHierarchyDepth() override { (void)group_->GetGroupHierarchyDepth(); }
    cSC4OccupantGroupTemplate* group_;
};

struct GroupPair {
    explicit GroupPair(cSC4OccupantGroupTemplate* group) : manager(group), automaton(group) {}
    ManagerGroupView manager;
    AutomatonGroupView automaton;
};

std::map<cSC4OccupantGroupTemplate*, std::unique_ptr<GroupPair>> g_groups;   // the script system keeps its templates for the process
std::map<const IAutomataGroupTemplate*, GroupPair*> g_byManagerView;

GroupPair* PairFor(cSC4OccupantGroupTemplate* group) {
    if (!group) return nullptr;
    auto& slot = g_groups[group];
    if (!slot) {
        slot = std::make_unique<GroupPair>(group);
        g_byManagerView[&slot->manager] = slot.get();
    }
    return slot.get();
}
IAutomataGroupTemplate* ManagerGroupView::Parent() const { GroupPair* p = PairFor(group_->parent); return p ? &p->manager : nullptr; }
IAutomatonGroupTemplate* AutomatonGroupView::Parent() const { GroupPair* p = PairFor(group_->parent); return p ? &p->automaton : nullptr; }

// ---- occupant views (any city occupant; an automaton's own is a member of its view) ---------------------------------------
class OccupantView final : public IAutomataOccupant {
public:
    explicit OccupantView(cISC4Occupant& occupant) : a_(occupant) {}
    bool QueryInterface(std::uint32_t iid, void** out) override { return a_.QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return a_.AddRef(); }
    std::uint32_t Release() override { return a_.Release(); }
    bool Init() override { return a_.Init(); }
    bool Shutdown() override { return a_.Shutdown(); }
    bool IsInitialized() override { return a_.IsInitialized(); }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return a_.AsPropertyHolder(); }
    std::uint32_t GetType() override { return a_.GetType(); }
    void GetPosition(float* xyz) override { a_.GetPosition(xyz); }
    void GetBoundingBox(float* minXyz, float* maxXyz) override { a_.GetBoundingBox(minXyz, maxXyz); }
    void GetBoundingCityCells(tCellRect& rect) override {
        SC4CellRect cells;
        a_.GetBoundingCityCells(cells);
        rect.x0 = cells.x0;
        rect.z0 = cells.z0;
        rect.x1 = cells.x1;
        rect.z1 = cells.z1;
    }
    bool IsOccupantGroup(std::uint32_t group) override { return a_.IsOccupantGroup(group); }
    bool GetOccupantGroups(std::set<std::uint32_t>& groups) override { return a_.GetOccupantGroups(groups); }
    std::uint32_t GetFlags() override { return a_.GetFlags(); }

private:
    cISC4Occupant& a_;
};
std::map<cISC4Occupant*, std::unique_ptr<OccupantView>> g_occupants;   // stateless pass-throughs, per address

class BuildingView final : public IAutomataBuildingOccupant {
public:
    explicit BuildingView(cISC4BuildingOccupant* building) : b_(building) {}
    bool QueryInterface(std::uint32_t iid, void** out) override { return b_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return b_->AddRef(); }
    std::uint32_t Release() override { return b_->Release(); }
    IAutomataOccupant* AsOccupant() const override { return CityOccupantView(b_->AsOccupant()); }
    const float& GetCompletionPercent() override { return b_->GetCompletionPercent(); }
private:
    cISC4BuildingOccupant* b_;
};
std::map<cISC4BuildingOccupant*, std::unique_ptr<BuildingView>> g_buildings;

// ---- the automaton view --------------------------------------------------------------------------------------------------
class AutomatonViewImpl final : public ITrafficAutomataAutomaton, public cSC4AutomatonExtension {
public:
    explicit AutomatonViewImpl(cSC4Automaton& automaton)
        : a_(automaton), traffic_(dynamic_cast<cSC4TrafficAutomaton*>(&automaton)), occupant_(automaton) {}
    cSC4Automaton& Automaton() const { return a_; }
    cSC4TrafficAutomaton* Traffic() const { return traffic_; }

    // cIGZUnknown: the automaton's
    bool QueryInterface(std::uint32_t iid, void** out) override { return a_.QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return a_.AddRef(); }
    std::uint32_t Release() override { return a_.Release(); }

    // IAutomataAutomaton (the cSC4Automaton primary vtable offsets)
    bool Init() override { return a_.Init(); }                                                       // 0x18
    bool Shutdown() override { return a_.Shutdown(); }                                               // 0x20
    IAutomataOccupant* Occupant() override { return &occupant_; }                                    // 0x30
    void GetPosition(float* xyz) override { a_.GetPosition(xyz); }                                   // 0x38
    void SetPosition(const float* xyz) override { a_.SetPosition(xyz); }                             // 0x40
    void SetVisible(bool visible) override { a_.SetVisible(visible); }                               // 0x48
    bool Remove(bool force) override { a_.Remove(force); return true; }                              // 0x68
    void SetOrientation(const float* direction) override { a_.SetOrientation(direction); }          // 0x70
    void GetOrientation(float* direction) override { a_.GetOrientation(direction); }                 // 0x78
    float GetRadius() const override { return a_.GetRadius(); }                                      // 0x88
    void SetAutomataFlag(std::uint32_t flag, bool on) override { a_.SetAutomataFlag(flag, on); }     // 0xf0
    bool CheckAutomataFlag(std::uint32_t flag) override { return a_.CheckAutomataFlag(flag); }       // 0xf8
    float GetOpacity() const override { return a_.GetOpacity(); }                                    // 0x100
    void SetOpacity(float opacity, float seconds) override { a_.SetOpacity(opacity, seconds); }      // 0x108
    void AddCallback(cIGZMessageTarget2* target) override { a_.AddCallback(target); }                // 0x138
    void* GetState() override { return a_.GetState(); }                                              // 0x1c0
    void* FindState(std::uint32_t state) override { return a_.FindState(state); }                    // 0x1c8
    bool PushState(std::uint32_t state, void* controller, float timer, bool flag, std::uint32_t a, std::uint32_t b,
                   const void* behavior) override {                                                  // 0x1a0
        return a_.PushState(state, static_cast<cSC4AutomataController*>(controller), timer, flag, a, b,
                            static_cast<const tBehaviorTemplate*>(behavior));
    }
    std::uint8_t GetPriority() override { return a_.GetPriority(); }                                 // 0x1f8
    void SetPriority(std::uint8_t priority) override { a_.SetPriority(priority); }                   // 0x200
    std::uint32_t GetAutomataGroup() override {                                                      // 0x218
        IAutomatonGroupTemplate* group = a_.GetAutomataGroup();
        return group ? group->GroupId() : 0;
    }
    bool DoAnimationTick(std::uint32_t data, float ms) override { return a_.DoAnimationTick(data, ms); }   // 0x278
    void SetZoomAndRotation(int zoom, int rotation) override { a_.SetZoomAndRotation(zoom, rotation); }   // 0x288
    void HandleRemoveController(void* controller) override {                                         // 0x2c0
        a_.HandleRemoveController(static_cast<cSC4AutomataController*>(controller));
    }
    bool GetMarkedForRemove() override { return a_.GetMarkedForRemove(); }                           // 0x2e0
    void SetMarkedForRemove(bool marked) override { a_.SetMarkedForRemove(marked); }                 // 0x300
    bool PostInit() override { return a_.PostInit(); }                                               // 0x360
    void SendCallbackMessage(std::uint32_t event, std::uint32_t data) override { a_.SendCallbackMessage(event, data); }   // 0x368
    bool ShouldCollide(IAutomataAutomaton* other) override {                                         // 0x280
        cSC4Automaton* peer = AutomatonOf(other);
        return peer && a_.ShouldCollide(peer);
    }
    bool PlaceInOccupantManager(bool place) override { return a_.PlaceInOccupantManager(place); }   // 0x268
    void PlaceInRenderer(bool place) override { a_.PlaceInRenderer(place); }                         // 0x270
    bool IsPlacedInOccupantManager() override { return a_.IsFlagSet(kAutomatonOccupantInManager); }
    bool IsPlacedInRenderer() override { return a_.IsFlagSet(kAutomatonOccupantInRenderer); }
    void StopEffectsAndClearModel() override {
        if (IAutomatonEffects* effects = a_.Effects()) {
            effects->StopAll(false);
            effects->SetModel(nullptr);
        }
    }
    // The pursuit state's target (state record +0x38): PARTIAL, no pursuit natively yet.
    IAutomataAutomaton* StateTarget() override { return nullptr; }
    void SetStateTarget(IAutomataAutomaton*) override {}
    void SetPropertyVariant(std::uint32_t id, cRZVariant& value, bool replace) override {
        if (sc4::cISCPropertyHolder* holder = a_.AsPropertyHolder()) holder->AddProperty(id, &value, replace);
    }
    void SetExemplar(cISCResExemplar* exemplar) override { a_.SetDefaultExemplar(exemplar); }
    void AddModelResourceKey(const cGZPersistResourceKey& key) override { a_.AddModelResourceKey(key); }
    void SetAutomataGroup(IAutomataGroupTemplate* group) override {                                  // 0x330
        auto it = g_byManagerView.find(group);
        a_.SetAutomataGroup(it != g_byManagerView.end() ? &it->second->automaton : nullptr);
    }

    // ITrafficAutomataAutomaton (the cSC4TrafficAutomaton slots; a non-traffic automaton answers neutrally)
    std::uint32_t PopulateLayer() const override { return traffic_ ? traffic_->PopulateLayer() : 0; }
    void SetPopulateLayer(std::uint32_t layer) override { if (traffic_) traffic_->SetPopulateLayer(layer); }
    void SetCellLifespan(std::uint8_t lifespan) override { if (traffic_) traffic_->SetCellLifespan(lifespan); }
    bool StartTrip(ITrafficNetworkMapOccupant* occupant, std::uint32_t key, bool param3) override {   // 0x450
        auto* target = dynamic_cast<ITrafficAutomatonOccupant*>(occupant);
        return traffic_ && target && traffic_->StartTrip(target, key, param3);
    }
    void SetPathToOccupant(ITrafficNetworkMapOccupant* occupant, std::uint32_t key, const void* path, bool keep) override {   // 0x458
        auto* target = dynamic_cast<ITrafficAutomatonOccupant*>(occupant);
        if (traffic_ && target) traffic_->SetPath(target, key, static_cast<const std::vector<cS3DVector3>*>(path), keep);
    }
    void HandleNetworkOccupantRemoved() override { if (traffic_) traffic_->HandleNetworkOccupantRemoved(); }   // 0x470
    std::uint32_t GetPathKey() const override { return traffic_ ? traffic_->GetPathKey() : 0xffff; }           // 0x478
    ITrafficNetworkMapOccupant* GetNetworkOccupant() const override { return traffic_ ? traffic_->GetNetworkOccupant() : nullptr; }   // 0x480
    bool IsTrain() const override { return traffic_ && traffic_->IsTrain(); }                                   // 0x4a0
    bool GetDetectCollisions() const override { return traffic_ && traffic_->GetDetectCollisions(); }           // 0x4e8
    IAutomataAutomaton* FindLeader() override {                                                                  // 0x4d8
        cSC4Automaton* leader = traffic_ ? traffic_->FindLeader() : nullptr;
        return leader ? AutomatonView(leader) : nullptr;
    }
    IAutomataAutomaton* Leader() const override {
        cSC4Automaton* leader = a_.GetLeader();
        return leader ? AutomatonView(leader) : nullptr;
    }
    void SetLeader(IAutomataAutomaton* leader) override { a_.SetLeader(AutomatonOf(leader)); }
    void HandleDispatched(const cS3DVector3& position, cISC4Occupant* source) override { a_.HandleDispatched(position, source); }   // 0x2b8
    std::deque<RouteData>* RouteDeque() override { return traffic_ ? &traffic_->Route() : nullptr; }                          // +0x368
    bool BeginRoute(bool onlyStart) override { return traffic_ && traffic_->BeginRoute(onlyStart); }                         // 0x4a8
    void ReturnToDefaultSpeed() override { if (traffic_) traffic_->ReturnToDefaultSpeed(); }                                  // 0x538

private:
    cSC4Automaton& a_;
    cSC4TrafficAutomaton* traffic_;
    OccupantView occupant_;
};

}  // namespace

IAutomataAutomaton* AutomatonView(cSC4Automaton* automaton) {
    if (!automaton) return nullptr;
    if (auto* view = dynamic_cast<AutomatonViewImpl*>(automaton->Extension())) return view;
    auto* view = new AutomatonViewImpl(*automaton);
    automaton->SetExtension(view);
    return view;
}

IAutomataAutomaton* AutomatonView(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xa9b40f05, &raw) || !raw) return nullptr;   // cISC4Automaton: the primary view
    auto* automaton = static_cast<cSC4Automaton*>(raw);   // cSC4Automaton::QueryInterface hands out `this` for its primary iid
    automaton->Release();
    return AutomatonView(automaton);
}

cSC4Automaton* AutomatonOf(IAutomataAutomaton* view) {
    auto* impl = dynamic_cast<AutomatonViewImpl*>(view);
    return impl ? &impl->Automaton() : nullptr;
}

ITrafficAutomataAutomaton* TrafficAutomatonView(IAutomataAutomaton* view) {
    auto* impl = dynamic_cast<AutomatonViewImpl*>(view);
    return impl && impl->Traffic() ? impl : nullptr;
}

IAutomataOccupant* CityOccupantView(cISC4Occupant* occupant) {
    if (!occupant) return nullptr;
    if (auto* automaton = dynamic_cast<cSC4Automaton*>(occupant)) {
        auto* view = static_cast<AutomatonViewImpl*>(AutomatonView(automaton));
        return view ? view->Occupant() : nullptr;
    }
    auto& slot = g_occupants[occupant];
    if (!slot) slot = std::make_unique<OccupantView>(*occupant);
    return slot.get();
}

IAutomataBuildingOccupant* CityBuildingView(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return nullptr;
    auto* building = static_cast<cISC4BuildingOccupant*>(raw);
    building->Release();   // the caller's object keeps it
    auto& slot = g_buildings[building];
    if (!slot) slot = std::make_unique<BuildingView>(building);
    return slot.get();
}

IAutomataGroupTemplate* GroupTemplateView(cSC4OccupantGroupTemplate* group) {
    GroupPair* pair = PairFor(group);
    return pair ? &pair->manager : nullptr;
}

IAutomatonGroupTemplate* AutomatonGroupTemplateView(const IAutomataGroupTemplate* managerView) {
    auto it = g_byManagerView.find(managerView);
    return it != g_byManagerView.end() ? &it->second->automaton : nullptr;
}

}  // namespace native
