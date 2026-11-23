// The boats in a native city: the watercraft manager component, the boat class for the serializable factory, and the real
// hosts both reach the city through.
//   * The component (class id 0xa98f9d28) is a cSC4AutomataManager, as the original cSC4WatercraftManager is (constructor i386
//     0x00385da2: exemplar group 0x19, default group 0x4412, fallback class 0x096e75f5 = cSC4Watercraft). The recovered
//     watercraft logic (sc4/WatercraftManager.*) runs on it: its inherited half (IWatercraftManagerBase) is the component's own
//     base class, so the boats live in the base's automaton list, tick and save with it, and every overridden entry point of
//     the original (Init, Shutdown, DoMessage, the tick, Read/Write, AddAutomaton, RemoveAutomaton, CreateAutomaton,
//     GetRandomCreationPoint, HandleTuningParametersChanged) goes to the recovered class.
//   * IWatercraftManagerHost: the message server, the city's cell geometry, the zone developer's lots, the terrain's sea level,
//     the surface water, the prop facing, the traffic network map's visible rect, the automata tuning exemplar, the city file's
//     object references, the traffic simulator's ferry lines and water routes, and the neighbors simulator's connections.
//   * IWatercraftHost (the boats): the manager's queries, the city, the terrain (sea level, the cell's highest surface, the
//     altitude, the lerped normal), the surface water's navigability, the lots, the compressed route reader and the effects
//     manager's named effects; the generic automaton services forward to the automata host every automaton uses.
#include "sc4/Render3D.h"

#include <bit>
#include <cstdio>
#include <list>
#include <vector>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeEffects.h"
#include "app/NativeLotSerial.h"
#include "gz/cGZPersistResourceManager.h"
#include "platform/Unwired.h"
#include "sc4/AutomataManager.h"
#include "sc4/CommonIIDs.h"
#include "sc4/CompressedRouteReader.h"
#include "sc4/DBSegment.h"
#include "sc4/Lot.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/PropOccupantInterface.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SurfaceWater.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/VisualEffect.h"
#include "sc4/Watercraft.h"
#include "sc4/WatercraftManager.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_WATERCRAFTDEBUG") != nullptr;
    return on;
}

template <class T> T* Serializable(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
template <class T> T* Concrete(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    return object ? dynamic_cast<T*>(object) : nullptr;
}
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
cSTESurfaceWater* Water() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::SurfaceWater) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISTESurfaceWater, &raw) || !raw) return nullptr;
    auto* water = static_cast<cSTESurfaceWater*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return water;
}
cSC4ZoneDeveloper* Lots() { return Serializable<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4TrafficSimulator* Traffic() { return Serializable<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator); }
cSC4NeighborsSimulator* Neighbors() { return Serializable<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator, kGZCLSID_cSC4NeighborsSimulator); }
cSC4TrafficNetworkMap* NetworkMap() { return Concrete<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork); }

cSC4Watercraft* WatercraftOf(IAutomataAutomaton* view) { return view ? dynamic_cast<cSC4Watercraft*>(AutomatonOf(view)) : nullptr; }

class WatercraftComponent;
WatercraftComponent* g_component = nullptr;   // the city's (one city at a time)

// ---- the body-of-water map (SL::spSurfaceWater vslot 0x48 GetBodyOfWaterMap, its cell value) -------------------------------------
class BodyOfWaterMap final : public IWatercraftBodyOfWater {
public:
    std::int16_t BodyOfWaterAt(int x, int z) override {
        cSTESurfaceWater* water = Water();
        cSC4SimGrid<std::uint16_t>* bodies = water ? water->GetBodyOfWaterMap() : nullptr;
        if (!bodies || x < 0 || z < 0 || x >= bodies->GetTractCountX() << bodies->GetTractShift() ||
            z >= bodies->GetTractCountZ() << bodies->GetTractShift())
            return 0;
        return static_cast<std::int16_t>(bodies->GetCellValue(x, z));
    }
} g_bodies;

// ---- the manager's host ------------------------------------------------------------------------------------------------------------
class ManagerHost final : public IWatercraftManagerHost {
public:
    bool AddMessageNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        return SL::spMS2 && SL::spMS2->AddNotification(target, type);
    }
    bool RemoveMessageNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        return SL::spMS2 && SL::spMS2->RemoveNotification(target, type);
    }
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = cellZ = -1;
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
    }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override {   // city vslot 0x308 (i386 0x184): the cell centre
        x = z = 0.0f;
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
    }
    bool CellInCity(int cellX, int cellZ) override {
        cSC4City* city = HostCity();
        return city && cellX >= 0 && cellZ >= 0 && cellX < city->CellCountX() && cellZ < city->CellCountZ();
    }
    bool PointInCity(float x, float z) override {
        cSC4City* city = HostCity();
        return city && city->LocationIsInBounds(x, z);
    }
    void CityCellCount(int& sizeX, int& sizeZ) override {
        cSC4City* city = HostCity();
        sizeX = city ? city->CellCountX() : 0;
        sizeZ = city ? city->CellCountZ() : 0;
    }
    float CellSize() override { return 16.0f; }
    cSC4Lot* LotAtPosition(const float* xyz) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots ? lots->GetLot(cS3DVector3{xyz[0], xyz[1], xyz[2]}) : nullptr;
        if (lot) lot->AddRef();
        return lot;
    }
    cSC4Lot* LotAtCell(int cellX, int cellZ) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots ? lots->GetLot(cellX, cellZ, false) : nullptr;
        if (lot) lot->AddRef();
        return lot;
    }
    float TerrainHeight(float, float) override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }   // i386 0x74
    IWatercraftBodyOfWater* BodyOfWater() override { return &g_bodies; }
    bool IsWaterCell(int cellX, int cellZ, int radius) override {
        cSTESurfaceWater* water = Water();
        return water && water->IsNavigable(cellX, cellZ, radius, nullptr);
    }
    bool PropOccupantFacing(cISC4Occupant* occupant, int& facing) override {
        void* raw = nullptr;
        if (!occupant || !occupant->QueryInterface(0xe9793a65, &raw) || !raw) return false;
        auto* prop = static_cast<cISC4PropOccupant*>(raw);
        facing = prop->GetOrientation();
        prop->Release();
        return true;
    }
    void VisibleCellRect(int rect[4]) override {   // SL::spTrafficNetworkMap vslot 0x88 GetVisibleCellRect
        rect[0] = rect[1] = rect[2] = rect[3] = 0;
        if (cSC4TrafficNetworkMap* map = NetworkMap()) {
            const SC4RectInt32 visible = map->GetVisibleCellRect();
            rect[0] = visible.x0;
            rect[1] = visible.z0;
            rect[2] = visible.x1;
            rect[3] = visible.z1;
        }
    }
    cISCResExemplar* GetAutomataTuningExemplar() override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0xa998d30b, 1}, 0xa52160f5, &raw, 0, nullptr)) return nullptr;
        return static_cast<cISCResExemplar*>(raw);
    }
    bool WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) override { return SC4WriteGZSerializable(stream, object); }
    cIGZUnknown* ReadSerializable(cIGZIStream& stream, std::uint32_t iid) override {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, iid, &raw, false);
        return static_cast<cIGZUnknown*>(raw);
    }
    bool WriteLot(cIGZOStream& stream, cSC4Lot* lot) override { return SC4WriteGZSerializable(stream, LotSerializable(lot)); }
    // SC4ReadGZSerializable(stream, 0xe818fced): the plain lot's adapter answers its serializable view and the plain object, so the
    // read asks for the former; one reference on the lot.
    cSC4Lot* ReadLot(cIGZIStream& stream) override {
        cIGZUnknown* adapter = nullptr;
        SC4ReadGZSerializable(stream, 0xe4fda3d4, reinterpret_cast<void**>(&adapter), false);
        void* plain = nullptr;
        if (!adapter || !adapter->QueryInterface(kGZIID_PlainObject, &plain) || !plain) {
            if (adapter) adapter->Release();
            return nullptr;
        }
        auto* lot = static_cast<cSC4Lot*>(plain);
        lot->AddRef();
        adapter->Release();
        return lot;
    }
    cSC4Watercraft* CreateWatercraft(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags, const cGZPersistResourceKey& key,
                                     cIGZMessageTarget2* listener, std::uint8_t argument, const cS3DVector3* velocity) override;
    // The traffic simulator's lines in use, with their lots (the simulator keeps the lots' cells).
    void FerryRoutesInUse(std::list<FerryRouteInfo>& out) override {
        out.clear();
        cSC4TrafficSimulator* traffic = Traffic();
        cSC4ZoneDeveloper* lots = Lots();
        if (!traffic || !lots) return;
        std::list<cSC4TrafficSimulator::FerryRouteInfo> inUse;
        traffic->GetFerryRoutesInUse(inUse);
        for (const auto& line : inUse) {
            FerryRouteInfo info;
            info.fromLot = lots->GetLot(line.lotAX, line.lotAZ, false);
            if (line.lotBX >= 0) info.toLot = lots->GetLot(line.lotBX, line.lotBZ, false);
            info.state = static_cast<std::int32_t>(line.type);
            info.f14 = line.abLastA;
            info.f16 = line.abLastB;
            info.f18 = line.baLastA;
            info.f1a = line.baLastB;
            info.f1c = line.neighbor ? 1 : 0;
            info.f1e = static_cast<std::int16_t>(line.neighborX);
            info.f20 = static_cast<std::int16_t>(line.neighborZ);
            info.route = line.routeAB;
            info.route2 = line.routeBA;
            if (Trace())
                std::fprintf(stderr, "watercraft: line in use from %d,%d to %d,%d type %u neighbor %d routes %zu/%zu lots %p %p\n", line.lotAX,
                             line.lotAZ, line.lotBX, line.lotBZ, line.type, line.neighbor ? 1 : 0, line.routeAB.size(), line.routeBA.size(),
                             static_cast<void*>(info.fromLot.get()), static_cast<void*>(info.toLot.get()));
            out.push_back(std::move(info));
        }
    }
    bool GetWaterRoute(int x0, int z0, int x1, int z1, std::vector<std::uint8_t>& route) override {
        cSC4TrafficSimulator* traffic = Traffic();
        return traffic && traffic->GetWaterRoute(x0, z0, x1, z1, route);
    }
    float SeaLevel() override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    void NeighborConnectionCells(std::uint32_t type, std::vector<std::pair<int, int>>& out) override {
        out.clear();
        cSC4NeighborsSimulator* neighbors = Neighbors();
        if (!neighbors) return;
        ConnectionList connections;
        neighbors->GetConnectionsOfType(type, connections);
        for (const auto& connection : connections) {
            if (!connection) continue;
            int x = 0, z = 0;
            connection->GetConnectionCell(x, z);
            out.emplace_back(x, z);
        }
    }
} g_managerHost;

// ---- the component ---------------------------------------------------------------------------------------------------------------
class InheritedHalf final : public IWatercraftManagerBase {
public:
    explicit InheritedHalf(WatercraftComponent& owner) : o_(owner) {}
    bool Initialized() const override;
    bool Init() override;
    bool Shutdown() override;
    bool DoMessage(cIGZMessage2* message) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    bool ReadInternal(cIGZIStream& stream, std::uint16_t version) override;
    void DoAnimationTick(std::uint32_t data, float milliseconds) override;
    bool CalculateSimSpeedModifier() override;
    bool SimSpeedPaused() const override { return cSC4AutomataManager::SimSpeedPaused(); }
    bool IsDrivingMode() const override;
    bool CanCreateGroup(std::uint32_t group) const override;
    void SetMaxCount(int count) override;
    void SetMinZoom(int zoom) override;
    void DestroyAutomata() override;
    bool PostCityInitPending() const override;
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;
    cRZRandomSuper& Random() override;
    float TuningMaxCountField() const override { return std::bit_cast<float>(AutomataTuning().f218a8); }
    float RetireOpacityDuration() const override { return AutomataTuning().f219b4; }
    bool AddToBase(cSC4Watercraft* automaton) override;
    bool RemoveFromBase(cSC4Watercraft* automaton, bool force) override;
    bool BaseBoats(std::vector<cSC4Watercraft*>& out) override;

private:
    WatercraftComponent& o_;
};

class WatercraftComponent final : public cSC4AutomataManager {
public:
    WatercraftComponent() : half_(*this) {
        SetExemplarGroup(0x19);
        SetDefaultGroup(0x4412);
        SetFallbackGroup(kWatercraftGZCLSID);
        manager_ = new cSC4WatercraftManager();
        manager_->AddRef();   // the component's reference (the message server may hold more)
        manager_->SetBase(&half_);
        manager_->SetHost(&g_managerHost);
        g_component = this;
    }
    ~WatercraftComponent() override {
        if (g_component == this) g_component = nullptr;
        manager_->SetBase(nullptr);
        manager_->Release();
    }
    cSC4WatercraftManager& Manager() { return *manager_; }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZCLSID_cSC4WatercraftManager) {
            *out = static_cast<cISC4AutomataManager*>(this);
            AddRef();
            return true;
        }
        return cSC4AutomataManager::QueryInterface(iid, out);
    }
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4WatercraftManager; }
    bool Init() override {
        g_component = this;
        return manager_->Init();
    }
    bool Shutdown() override { return manager_->Shutdown(); }
    bool DoMessage(cIGZMessage2* message) override { return manager_->DoMessage(message); }
    bool DoMessage(cGZMessage& message) override { return cSC4AutomataManager::DoMessage(message); }
    void DoAnimationTick(std::uint32_t data, float milliseconds) override { manager_->DoAnimationTick(data, milliseconds); }
    bool HandleTuningParametersChanged() override { return manager_->HandleTuningParametersChanged(); }
    bool Write(cIGZOStream& stream) const override { return manager_->Write(stream); }
    bool Read(cIGZIStream& stream) override { return manager_->Read(stream); }
    // AddAutomaton(null) makes a boat (i386 0x003802b6: a default cSC4Watercraft); the boat goes through the recovered class, whose
    // inherited half puts it in this base's list.
    bool AddAutomaton(IAutomataAutomaton* automaton) override {
        cSC4Watercraft* boat = automaton ? WatercraftOf(automaton) : NewBoat();
        if (!boat) return automaton ? cSC4AutomataManager::AddAutomaton(automaton) : false;
        const bool added = manager_->AddAutomaton(boat);
        if (!automaton) boat->Release();   // the base list holds its own reference
        return added;
    }
    bool RemoveAutomaton(IAutomataAutomaton* automaton, bool force) override {
        cSC4Watercraft* boat = WatercraftOf(automaton);
        return boat ? manager_->RemoveAutomaton(boat, force) : cSC4AutomataManager::RemoveAutomaton(automaton, force);
    }
    IAutomataAutomaton* CreateAutomaton(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                        const cGZPersistResourceKey& key, cIGZMessageTarget2* listener, std::uint8_t argument,
                                        const cS3DVector3* velocity) override {
        cSC4Watercraft* boat = manager_->CreateAutomaton(group, position, flags, key, listener, argument, velocity);
        return boat ? AutomatonView(static_cast<cSC4Automaton*>(boat)) : nullptr;
    }
    bool GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& origin, float minDistance, float maxDistance, cS3DVector3& out,
                                std::uint32_t flags, const cS3DVector3* velocity) override {
        return manager_->GetRandomCreationPoint(group, origin, minDistance, maxDistance, out, flags, const_cast<cS3DVector3*>(velocity));
    }
    // The base's own creation (the recovered class's CreateAutomaton ends here through its host).
    cSC4Watercraft* BaseCreate(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags, const cGZPersistResourceKey& key,
                               cIGZMessageTarget2* listener, std::uint8_t argument, const cS3DVector3* velocity) {
        return WatercraftOf(cSC4AutomataManager::CreateAutomaton(group, position, flags, key, listener, argument, velocity));
    }
    static cSC4Watercraft* NewBoat();

private:
    friend class InheritedHalf;
    cSC4WatercraftManager* manager_ = nullptr;
    InheritedHalf half_;
};

bool InheritedHalf::Initialized() const { return o_.Initialized(); }
bool InheritedHalf::Init() { return o_.cSC4AutomataManager::Init(); }
bool InheritedHalf::Shutdown() { return o_.cSC4AutomataManager::Shutdown(); }
bool InheritedHalf::DoMessage(cIGZMessage2* message) { return o_.cSC4AutomataManager::DoMessage(message); }
bool InheritedHalf::Write(cIGZOStream& stream) const { return o_.cSC4AutomataManager::Write(stream); }
bool InheritedHalf::Read(cIGZIStream& stream) { return o_.cSC4AutomataManager::Read(stream); }
bool InheritedHalf::ReadInternal(cIGZIStream& stream, std::uint16_t version) { return o_.ReadInternal(stream, version); }
void InheritedHalf::DoAnimationTick(std::uint32_t data, float milliseconds) { o_.cSC4AutomataManager::DoAnimationTick(data, milliseconds); }
bool InheritedHalf::CalculateSimSpeedModifier() { return o_.CalculateSimSpeedModifier(); }
bool InheritedHalf::IsDrivingMode() const { return o_.cSC4AutomataManager::IsDrivingMode(); }
bool InheritedHalf::CanCreateGroup(std::uint32_t group) const { return o_.cSC4AutomataManager::CanCreateGroup(group); }
void InheritedHalf::SetMaxCount(int count) { o_.cSC4AutomataManager::SetMaxCount(count); }
void InheritedHalf::SetMinZoom(int zoom) { o_.SetMinZoom(zoom); }
void InheritedHalf::DestroyAutomata() { o_.cSC4AutomataManager::DestroyAutomata(); }
bool InheritedHalf::PostCityInitPending() const { return o_.PostCityInitPending(); }
bool InheritedHalf::QueryInterface(std::uint32_t iid, void** out) { return o_.cSC4AutomataManager::QueryInterface(iid, out); }
std::uint32_t InheritedHalf::AddRef() { return o_.AddRef(); }
std::uint32_t InheritedHalf::Release() { return o_.Release(); }
cRZRandomSuper& InheritedHalf::Random() { return o_.Random(); }
bool InheritedHalf::AddToBase(cSC4Watercraft* automaton) {
    IAutomataAutomaton* view = automaton ? AutomatonView(static_cast<cSC4Automaton*>(automaton)) : nullptr;
    if (!view) return false;
    return o_.cSC4AutomataManager::AddAutomaton(view);
}
bool InheritedHalf::RemoveFromBase(cSC4Watercraft* automaton, bool force) {
    IAutomataAutomaton* view = automaton ? AutomatonView(static_cast<cSC4Automaton*>(automaton)) : nullptr;
    if (!view) return false;
    return o_.cSC4AutomataManager::RemoveAutomaton(view, force);
}
bool InheritedHalf::BaseBoats(std::vector<cSC4Watercraft*>& out) {
    for (IAutomataAutomaton* view : o_.Automata())
        if (cSC4Watercraft* boat = WatercraftOf(view)) out.push_back(boat);
    return true;
}

cSC4Watercraft* ManagerHost::CreateWatercraft(std::uint32_t group, const cS3DVector3& position, std::uint32_t flags,
                                              const cGZPersistResourceKey& key, cIGZMessageTarget2* listener, std::uint8_t argument,
                                              const cS3DVector3* velocity) {
    cSC4Watercraft* boat = g_component ? g_component->BaseCreate(group, position, flags, key, listener, argument, velocity) : nullptr;
    if (Trace())
        std::fprintf(stderr, "watercraft: create group 0x%x at (%.1f, %.1f, %.1f) -> %p\n", group, position.x, position.y, position.z,
                     static_cast<void*>(boat));
    return boat;
}

// ---- the boats' host ---------------------------------------------------------------------------------------------------------------
// The compressed route reader behind a boat's route (cSC4CompressedRouteReader, embedded twice in the original boat).
class RouteReader final : public IWatercraftRouteReader {
public:
    void SetData(const std::vector<std::uint8_t>& data) override { reader_.SetData(&data); }
    bool GetNext(std::int16_t& x, std::int16_t& z) override {
        CompressedRouteCell cell;
        if (!reader_.GetNext(cell)) return false;
        x = cell.x;
        z = cell.z;
        return true;
    }

private:
    cSC4CompressedRouteReader reader_;
};

// A named effect of the effects manager as the boat drives its six (the sinking explosion, the grounding, the splash...). The
// port's visual effects have no paused state: pausing stops the effect, resuming a stopped effect starts it again.
class NamedAnimation final : public IWatercraftAnimation {
public:
    explicit NamedAnimation(cSC4VisualEffect* effect) : effect_(effect) {}
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ > 0) return;
        effect_->Release();
        delete this;
    }
    void Play(bool) override { effect_->Start(0); }
    void SetPaused(bool paused) override {
        if (paused) effect_->Stop(0);
        else if (!effect_->IsStarted()) effect_->Start(0);
    }
    bool IsPlaying() const override { return effect_->IsStarted(); }
    void SetTransform(const cS3DTransform& transform) override { effect_->SetEffectTransform(transform); }

private:
    cSC4VisualEffect* effect_;
    int refs_ = 1;
};

class BoatHost final : public IWatercraftHost {
public:
    IAutomatonHost* A() { return AutomatonHost(); }
    // ---- IAutomatonHost: the services every automaton uses
    IAutomatonModel* CreateModel(const cGZPersistResourceKey& key) override { return A()->CreateModel(key); }
    IAutomatonModel* CreateAnimatedModel(const cGZPersistResourceKey& key) override { return A()->CreateAnimatedModel(key); }
    void DestroyModel(IAutomatonModel* model) override { A()->DestroyModel(model); }
    IAutomatonEffects* CreateEffects() override { return A()->CreateEffects(); }
    void DestroyEffects(IAutomatonEffects* effects) override { A()->DestroyEffects(effects); }
    bool AddModelToRenderer(IAutomatonModel* model) override { return A()->AddModelToRenderer(model); }
    bool RemoveModelFromRenderer(IAutomatonModel* model) override { return A()->RemoveModelFromRenderer(model); }
    void MoveModelInRenderer(IAutomatonModel* model) override { A()->MoveModelInRenderer(model); }
    void ChangeModelInRenderer(IAutomatonModel* model, int change) override { A()->ChangeModelInRenderer(model, change); }
    bool InsertOccupant(cISC4Occupant* occupant) override { return A()->InsertOccupant(occupant); }
    void RemoveOccupant(cISC4Occupant* occupant) override { A()->RemoveOccupant(occupant); }
    bool RefreshOccupant(cISC4Occupant* occupant) override { return A()->RefreshOccupant(occupant); }
    bool HasCity() override { return A()->HasCity(); }
    float CitySizeX() override { return A()->CitySizeX(); }
    float CitySizeZ() override { return A()->CitySizeZ(); }
    // The boat's heights are the sea level (terrain i386 0x74 GetSeaLevel, the only height query of the boat code).
    float TerrainHeight(float, float) override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    bool ConstrainVectorToNetworkTile(const cS3DVector3& from, cS3DVector3& point, std::uint32_t flags) override {
        return A()->ConstrainVectorToNetworkTile(from, point, flags);
    }
    float RoadAltitude(float x, float z) override { return A()->RoadAltitude(x, z); }
    bool NetworkAt(int cellX, int cellZ) override { return A()->NetworkAt(cellX, cellZ); }
    bool LotAt(int cellX, int cellZ) override { return A()->LotAt(cellX, cellZ); }
    void PlaceOnNetwork(const cS3DVector3& from, cS3DVector3& to, std::uint32_t mask) override { A()->PlaceOnNetwork(from, to, mask); }
    cSC4AutomataController* FindAttraction(cISC4Occupant* occupant, cS3DVector3* attraction, float* radius, float* distance) override {
        return A()->FindAttraction(occupant, attraction, radius, distance);
    }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t id) override { return A()->FindAutomataGroup(id); }
    bool AnimationUpdateAllowed() override { return A()->AnimationUpdateAllowed(); }
    float SimSpeedModifier() override { return A()->SimSpeedModifier(); }
    float MaxSpeedUnits() override { return A()->MaxSpeedUnits(); }
    float FadeDurationSeconds() override { return A()->FadeDurationSeconds(); }
    void PostMessage(cIGZMessage2* message) override { A()->PostMessage(message); }
    std::uint32_t GenerateSoundInstanceId() override { return A()->GenerateSoundInstanceId(); }
    void StopSoundLoop(std::uint32_t handle) override { A()->StopSoundLoop(handle); }
    bool SoundSystemAvailable() override { return A()->SoundSystemAvailable(); }
    void StartSoundLoop(std::uint32_t soundId, std::uint32_t handle) override { A()->StartSoundLoop(soundId, handle); }
    void MoveSoundLoop(std::uint32_t handle, std::uint32_t x, std::uint32_t y, std::uint32_t z) override { A()->MoveSoundLoop(handle, x, y, z); }
    std::uint32_t ViewZoom() override { return A()->ViewZoom(); }
    bool IsDrivingMode() override { return A()->IsDrivingMode(); }
    void PostAudioEvent(std::uint32_t event) override { A()->PostAudioEvent(event); }
    void OccupantManagerCell(float x, float z, std::int32_t& cellX, std::int32_t& cellZ) override { A()->OccupantManagerCell(x, z, cellX, cellZ); }
    std::uint32_t RandomBelow(std::uint32_t range) override { return A()->RandomBelow(range); }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t hi) override { return A()->RandomInt(lo, hi); }
    double RandomDouble(double lo, double hi) override { return A()->RandomDouble(lo, hi); }
    double RandomUnit() override { return A()->RandomUnit(); }

    // ---- cSC4WatercraftManager
    cSC4WatercraftManager* M() { return g_component ? &g_component->Manager() : nullptr; }
    cSC4Watercraft* LookForBoatsAtPosition(const float* xyz, cSC4Watercraft* exclude) override {
        return M() ? M()->LookForBoatsAtPosition(cS3DVector3{xyz[0], xyz[1], xyz[2]}, exclude) : nullptr;
    }
    // The boat asks about its own simulated position (the original passes it through; the out position is not written).
    cSC4Watercraft* CollisionDetection(float radius, cSC4Watercraft* self, float*) override {
        return M() && self ? M()->CollisionDetection(self, self->SimulatedPosition(), radius) : nullptr;
    }
    bool RequestPermissionToDock(cSC4Lot* lot, cSC4Watercraft* boat) override { return M() && M()->RequestPermissionToDock(lot, boat); }
    bool RequestPermissionToWait(cSC4Lot* lot, cSC4Watercraft* boat) override { return M() && M()->RequestPermissionToWait(lot, boat); }
    void DoneDocking(cSC4Lot* lot, cSC4Watercraft* boat) override { if (M()) M()->DoneDocking(lot, boat); }
    void DoneQueueing(cSC4Lot* lot, cSC4Watercraft* boat) override { if (M()) M()->DoneQueueing(lot, boat); }
    // Manager vslot 0x178 (i386 0xbc): the inherited cSC4AutomataManager::IsAutomatonOnscreen.
    bool CanDockNow(cSC4Watercraft* boat) override {
        IAutomataAutomaton* view = boat ? AutomatonView(static_cast<cSC4Automaton*>(boat)) : nullptr;
        return g_component && view && g_component->IsAutomatonOnscreen(view);
    }
    void FleetCounts(std::int32_t& docks, std::int32_t& queues) override {
        docks = queues = 0;
        if (M()) M()->FleetCounts(docks, queues);
    }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { return SL::spMS2 && SL::spMS2->AddNotification(target, type); }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        return SL::spMS2 && SL::spMS2->RemoveNotification(target, type);
    }
    // ---- city
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override { g_managerHost.WorldToCell(x, z, cellX, cellZ); }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override { g_managerHost.CellToWorld(cellX, cellZ, x, z); }
    bool CellInCity(int cellX, int cellZ) override { return g_managerHost.CellInCity(cellX, cellZ); }
    bool PointInCity(float x, float z) override { return g_managerHost.PointInCity(x, z); }
    void CityCellCount(int& sizeX, int& sizeZ) override { g_managerHost.CityCellCount(sizeX, sizeZ); }
    float CityExtent() override { cSC4City* city = HostCity(); return city ? city->SizeX() : 0.0f; }   // vslot 0x2d8
    // ---- terrain: i386 0x48 GetMaxSurfaceLevel (cell), 0x6c GetLerpedNormal, 0x30 GetAltitude
    float TerrainHeightAtCell(int cellX, int cellZ) override {
        cSTETerrain* t = Terrain();
        return t && cellX >= 0 && cellZ >= 0 ? t->GetMaxSurfaceLevel(std::uint32_t(cellX), std::uint32_t(cellZ)) : 0.0f;
    }
    void TerrainNormal(float x, float z, cS3DVector3& out) override {
        cSTETerrain* t = Terrain();
        if (!t) return;
        TerrainVector3 normal{};
        t->GetLerpedNormal(x, z, normal);
        out = cS3DVector3{normal.x, normal.y, normal.z};
    }
    float WaterLevel(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    // ---- surface water: i386 0x34 HasDraftClearance, 0x1c / 0x20 IsNavigable (cell / position)
    bool IsWaterCell(int cellX, int cellZ) override { cSTESurfaceWater* w = Water(); return w && w->HasDraftClearance(cellX, cellZ); }
    bool IsWaterAtCell(int cellX, int cellZ, int boatSize) override {
        cSTESurfaceWater* w = Water();
        return w && w->IsNavigable(cellX, cellZ, boatSize, nullptr);
    }
    bool IsWaterAtPosition(float x, float z, int mask, cSC4Lot* lot) override {
        cSTESurfaceWater* w = Water();
        return w && w->IsNavigable(cS3DVector3{x, 0.0f, z}, mask, lot);
    }
    // ---- the zone developer's lots (borrowed)
    cSC4Lot* LotAtCell(int cellX, int cellZ) override { cSC4ZoneDeveloper* l = Lots(); return l ? l->GetLot(cellX, cellZ, false) : nullptr; }
    cSC4Lot* LotAtPosition(const float* xyz) override { cSC4ZoneDeveloper* l = Lots(); return l ? l->GetLot(cS3DVector3{xyz[0], xyz[1], xyz[2]}) : nullptr; }
    IWatercraftRouteReader* CreateRouteReader() override { return new RouteReader(); }
    IWatercraftAnimation* CreateNamedAnimation(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        return effect ? new NamedAnimation(effect) : nullptr;
    }
} g_boatHost;

cSC4Watercraft* WatercraftComponent::NewBoat() {
    auto* boat = new cSC4Watercraft(&g_boatHost);
    boat->AddRef();
    return boat;
}

}  // namespace

// The serializable factory's watercraft classes: the manager component (0xa98f9d28) and the boat (0x096e75f5).
cIGZSerializable* NativeCreateWatercraftSerializable(std::uint32_t classId) {
    if (classId == kGZCLSID_cSC4WatercraftManager) {
        auto* component = new WatercraftComponent();
        component->AddRef();
        return component;
    }
    if (classId == kWatercraftGZCLSID) {
        cSC4Watercraft* boat = WatercraftComponent::NewBoat();
        return boat;
    }
    return nullptr;
}

// Kit command `watercraft`: the manager's terminals, marinas and boats.
void NativeWatercraftReport() {
    if (!g_component) {
        std::printf("watercraft: no watercraft manager\n");
        return;
    }
    cSC4WatercraftManager& m = g_component->Manager();
    std::printf("watercraft: %d terminals, %d marina buildings, %d marina lots, %d ambient boats, %d automata (max %u)\n", m.TerminalCount(),
                m.MarinaOccupantCount(), m.MarinaLotCount(), m.AmbientBoatCount(), g_component->GetAutomataCount(), g_component->MaxCount());
    for (const auto& entry : m.Terminals()) {
        int x = -1, z = -1;
        if (entry.first) entry.first->GetLocation(x, z);
        std::printf("watercraft: terminal lot %d,%d condition %u dock %p wait %p routes %zu\n", x, z, entry.first ? entry.first->GetCondition() : 0u,
                    static_cast<void*>(entry.second.dockBoat.get()), static_cast<void*>(entry.second.waitBoat.get()), entry.second.routes.size());
        for (const FerryRouteInformation& route : entry.second.routes) {
            int tx = -1, tz = -1;
            if (route.route.toLot) route.route.toLot->GetLocation(tx, tz);
            std::printf("watercraft: route to %d,%d type %d neighbor %u boat %p at-station %d\n", tx, tz, route.route.state, route.route.f1c,
                        static_cast<void*>(route.boat.get()), route.atStation ? 1 : 0);
        }
    }
    for (IAutomataAutomaton* view : g_component->Automata()) {
        cSC4Watercraft* boat = WatercraftOf(view);
        if (!boat) continue;
        float p[3] = {0.0f, 0.0f, 0.0f};
        boat->GetPosition(p);
        IAutomatonGroupTemplate* group = boat->GetAutomataGroup();
        std::printf("watercraft: boat %p group %04x behavior %x models %zu opacity %.2f at (%.1f, %.1f, %.1f) lookahead %zu docking %d\n",
                    static_cast<void*>(boat), group ? group->GroupId() : 0u, boat->GetBehavior(), boat->Models().size(), boat->GetOpacity(), p[0],
                    p[1], p[2], boat->Lookahead().size(), boat->DockingActive() ? 1 : 0);
    }
}

}  // namespace native
