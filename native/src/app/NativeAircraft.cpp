// The aircraft in a native city: the creators of the three aircraft classes for the serializable factory (a saved or newly
// created automaton of class 0x8ba98eda cSC4Helicopter, 0xcbeb4f62 cSC4Airplane or 0xc96e75dc cSC4Aircraft), and the real
// hosts they and their manager reach the city through:
//   * IAircraftHost (sc4/Aircraft.h): the terrain, the lot manager, the city, the effects manager, the occupant manager's
//     box queries, the simulator, the disaster layer, the view and cSC4AircraftManager's runway list and random stream;
//   * IHelicopterHost (sc4/Helicopter.h): the rotor effect collections, the obstacle queries, the renderer's terrain picks,
//     the city cell geometry, the automata controller manager, the traffic accident location and the crime hotspot;
//   * IAircraftManagerHost (sc4/AircraftManager.h): the lot manager, the prop orientation, the police / fire protection
//     simulators and the aircraft constructors.
// The original's AEMS audio events (the player-drive sounds) have no audio event system in the port: they are the same
// documented no-op the other automata post through (IAutomatonHost::PostAudioEvent).
// The renderer header first: it declares the global cISC4OccupantFilter its own declarations name.
#include "sc4/Render3D.h"

#include <cstdio>
#include <list>
#include <string>
#include <vector>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeEffects.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/AircraftManager.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/AutomataManager.h"
#include "sc4/AutomataTuningParams.h"
#include "sc4/Airplane.h"
#include "sc4/Blimp.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/EffectMaps.h"
#include "sc4/EffectsManager.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/Helicopter.h"
#include "sc4/Lot.h"
#include "sc4/ModelRegEffects.h"
#include "sc4/OccupantFilters.h"
#include "sc4/OccupantGroupTemplate.h"
#include "sc4/OccupantManager.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PropOccupantInterface.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/Simulator.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/VisualEffect.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::cSC4VisualEffect* NativeRegEffectVisual(sc4::IRegEffect* effect);              // app/NativeModelRegEffects.cpp
sc4::IAutomatonEffects* NativeNewModelRegEffects();
const sc4::RegPointMap* NativeAutomatonModelRegPoints(sc4::IAutomatonModel* model);   // app/NativeAutomata.cpp
bool NativeDisasterOngoing();                                                        // app/NativeRiot.cpp
int NativeViewZoom();                                                                // app/NativeAutomataManagerHost.cpp
void NativeVisibleTerrainRect(float out[4]);                                          // app/NativeAutomataManagerHost.cpp
bool NativeProjectToViewport(const float* xyz, float out[3]);                           // app/NativeAutomataManagerHost.cpp
sc4::cSC4CrimeSimulator* NativeCrimeSimulator();                                     // app/NativeCrime.cpp
sc4::IPoliceOccupant* NativePoliceStation(sc4::cISC4Occupant* occupant);             // app/NativeSimHosts.cpp
sc4::IFireOccupant* NativeFireStation(sc4::cISC4Occupant* occupant);

namespace {
using namespace sc4;

template <class T> T* CityComponentAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
template <class T> T* SerializableComponent(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
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
cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();   // the city keeps it
    return manager;
}
cSC4ZoneDeveloper* Lots() { return SerializableComponent<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4Simulator* Simulator() { return SerializableComponent<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator); }
cSC4AircraftManager* Manager() { return CityComponentAs<cSC4AircraftManager>(CityComponent::AircraftManager); }
cSC4PoliceSimulator* PoliceSim() { return CityComponentAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator); }
cSC4FireProtectionSimulator* FireSim() { return CityComponentAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator); }
cSC4EffectsManager* Effects() { return dynamic_cast<cSC4EffectsManager*>(SL::spEffectsManager); }
cSC4AutomataControllerManager* Controllers() { return CityComponentAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager); }

// A visual effect of the effects manager, as the aircraft drive it (the slots IAirplaneEffect models: Start, Stop,
// IsStarted, SetEffectTransform, SetSourceTransform).
class AirplaneEffect final : public IAirplaneEffect {
public:
    explicit AirplaneEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ > 0) return;
        effect_->Release();
        delete this;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(bool fade) override { effect_->Stop(fade ? 1 : 0); }
    bool IsPlaying() override { return effect_->IsStarted(); }
    void SetTransform(const cS3DTransform& transform) override { effect_->SetEffectTransform(transform); }
    // vslot 0x40 SetSourceTransform with the position as a translation-only transform.
    void SetPosition(const cS3DVector3& position) override {
        cS3DTransform source;
        source.flags = 1;
        source.changeCounter = 1;
        source.translation[0] = position.x;
        source.translation[1] = position.y;
        source.translation[2] = position.z;
        effect_->SetSourceTransform(source);
    }
    cSC4VisualEffect* Visual() const { return effect_; }

private:
    cSC4VisualEffect* effect_;
    int refs_ = 1;
};

// The visual effect behind an aircraft effect pointer: one of ours (CreateEffect), or a registered effect of the model
// (the aircraft look those up through cSC4ModelRegEffects::FindRegEffect and keep them as IAirplaneEffect).
cSC4VisualEffect* VisualOf(IAirplaneEffect* effect) {
    if (!effect) return nullptr;
    if (auto* mine = dynamic_cast<AirplaneEffect*>(effect)) return mine->Visual();
    return NativeRegEffectVisual(reinterpret_cast<IRegEffect*>(effect));
}

// The two rotor effect collections of a helicopter: cSC4ModelRegEffects objects.
class HelicopterEffects final : public IHelicopterEffects {
public:
    HelicopterEffects() : effects_(static_cast<cSC4ModelRegEffects*>(NativeNewModelRegEffects())) {}
    ~HelicopterEffects() override { delete effects_; }
    void SetModel(IAutomatonModel* model) override { effects_->SetModel(model); }
    bool Init() override { return effects_->Init(); }
    void Update() override { effects_->UpdatePlacement(); }
    void StartAll(int transition) override { effects_->StartAllEffects(transition); }
    void StopAll(int transition) override { effects_->StopAllEffects(transition); }
    bool StartNamed(const cRZString& effect, int transition, bool loop) override { return effects_->StartNamedEffects(effect, transition, loop); }
    void StopNamed(const cRZString& effect, int transition) override { effects_->StopNamed(effect, transition); }
    // The helicopter passes (registration point, effect name): the effect rides on the point, not started.
    bool AddRegEffect(const cRZString& point, const cRZString& effect) override {
        return effects_->AddRegEffect(std::string(point.ToChar()), std::string(effect.ToChar())) != nullptr;
    }
    void Shutdown() override { effects_->Shutdown(); }

private:
    cSC4ModelRegEffects* effects_;
};

// An occupant the helicopter's obstacle queries found (one reference on the occupant).
class Obstacle final : public IHelicopterObstacle {
public:
    explicit Obstacle(cISC4Occupant* occupant) : occupant_(occupant) { occupant_->AddRef(); }
    ~Obstacle() override { occupant_->Release(); }
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ <= 0) delete this;
    }
    void GetBounds(cS3DVector3& minCorner, cS3DVector3& maxCorner) override {
        float lo[3] = {0.0f, 0.0f, 0.0f}, hi[3] = {0.0f, 0.0f, 0.0f};
        occupant_->GetBoundingBox(lo, hi);
        minCorner = cS3DVector3{lo[0], lo[1], lo[2]};
        maxCorner = cS3DVector3{hi[0], hi[1], hi[2]};
    }

private:
    cISC4Occupant* occupant_;
    int refs_ = 1;
};

// The first occupant of a box query (occupant manager vslot 0x120 by type / 0x128 by filter); the list is released.
bool FirstInBox(const float* lo, const float* hi, std::uint32_t type, sc4::cISC4OccupantFilter* filter, cS3DVector3* position,
                float* boxMin, float* boxMax) {
    cISC4OccupantManager* occupants = Occupants();
    if (!occupants) return false;
    OccupantList found;
    const bool any = filter ? occupants->GetOccupantsByBBox(found, lo, hi, filter, 0xffffffffu)
                            : occupants->GetOccupantsByBBox(found, lo, hi, type, 0xffffffffu);
    bool hit = false;
    if (any && !found.empty() && found.front()) {
        cISC4Occupant* first = found.front();
        float p[3] = {0.0f, 0.0f, 0.0f};
        first->GetPosition(p);
        if (position) *position = cS3DVector3{p[0], p[1], p[2]};
        if (boxMin && boxMax) first->GetBoundingBox(boxMin, boxMax);
        hit = true;
    }
    occupants->ReleaseOccupantList(found);
    return hit;
}

class AircraftHost final : public IAircraftHost {
public:
    bool TerrainPointInBounds(float x, float z) override { cSTETerrain* t = Terrain(); return t && t->LocationIsInBounds(x, z); }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float WaterHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetWaterLevel(x, z) : 0.0f; }
    float DefaultAltitude() override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }       // GetSeaLevel
    float AltitudeClamp() override { cSTETerrain* t = Terrain(); return t ? t->MaxHeight() : 0.0f; }           // MaxHeight
    float TallestBuildingAltitude() override { cSC4EffectsManager* e = Effects(); return e ? e->GetTallestBuildingAltitude() : 0.0f; }
    void GetRunwayList(const cS3DVector3& position, std::vector<cS3DVector3>& out) override {
        out.clear();
        if (cSC4AircraftManager* manager = Manager()) manager->GetRunwayList(position, out);
    }
    std::uint32_t RandomWeightedChoice(std::uint32_t count, const double* weights) override {
        return cSC4AircraftManager::RandomWeightedChoice(count, weights);
    }
    const void* LotAt(const cS3DVector3& position) override {
        cSC4ZoneDeveloper* lots = Lots();
        return lots ? static_cast<const void*>(lots->GetLot(position)) : nullptr;
    }
    float OpacityFadeSeconds() override { return AutomataTuning().params.f134; }
    bool CellHasWater(std::uint32_t cellX, std::uint32_t cellZ) override { cSTETerrain* t = Terrain(); return t && t->CellHasWater(cellX, cellZ); }
    bool FindCollider(AircraftColliderQuery query, const float* boxMin, const float* boxMax, AircraftCollider& hit) override {
        sc4::cISC4OccupantFilter* filter = nullptr;
        std::uint32_t type = 0x278128a0;
        if (query == AircraftColliderQuery::kAircraft) {
            cSC4AircraftManager* manager = Manager();
            filter = manager ? static_cast<sc4::cISC4OccupantFilter*>(manager->BuildingPropFilter()) : nullptr;
            if (!filter) return false;
        } else if (query == AircraftColliderQuery::kType_c772bf98) {
            type = 0xc772bf98;
        }
        return FirstInBox(boxMin, boxMax, type, filter, &hit.position, hit.boxMin, hit.boxMax);
    }
    // The lot under the point; a lot with flag bit 0 answers its own altitude (cISC4Lot vslots 0x60 GetFlags, 0x80 GetAltitude).
    bool LotAltitudeAt(const cS3DVector3& position, float* altitude) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots ? lots->GetLot(position) : nullptr;
        if (!lot || (lot->GetFlags() & 1) == 0) return false;
        if (altitude) *altitude = lot->GetAltitude();
        return true;
    }
    float CitySizeX() override { cSC4City* c = HostCity(); return c ? c->SizeX() : 0.0f; }
    float CitySizeZ() override { cSC4City* c = HostCity(); return c ? c->SizeZ() : 0.0f; }
    int CityCellCountX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellCountZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    void WorldToCell(float x, float z, std::int32_t& cellX, std::int32_t& cellZ) override {
        cellX = cellZ = 0;
        if (cISC4OccupantManager* occupants = Occupants()) {
            int cx = 0, cz = 0;
            occupants->PositionToOccupantManagerCell(x, z, cx, cz);
            cellX = cx;
            cellZ = cz;
        }
    }
    bool FindFireOccupant(const cS3DVector3& minCorner, const cS3DVector3& maxCorner, sc4::cISC4OccupantFilter* filter,
                          cS3DVector3& position) override {
        const float lo[3] = {minCorner.x, minCorner.y, minCorner.z}, hi[3] = {maxCorner.x, maxCorner.y, maxCorner.z};
        return filter && FirstInBox(lo, hi, 0, filter, &position, nullptr, nullptr);
    }
    double RandomDoubleRange(double lo, double hi) override { return cSC4AircraftManager::RandomDoubleRange(lo, hi); }
    // The group template's +0xe0 / +0xe8 parameter blocks (the script system's airplane / helicopter / player-drive tables).
    const float* FindDriveParameters(IAutomatonGroupTemplate* group) override {
        cSC4OccupantGroupTemplate* t = OccupantGroupTemplateOf(group);
        return t && t->driveParams ? t->driveParams->v.data() : nullptr;
    }
    const float* FindPlayerDriveParameters(IAutomatonGroupTemplate* group) override {
        cSC4OccupantGroupTemplate* t = OccupantGroupTemplateOf(group);
        return t && t->playerDriveParams ? reinterpret_cast<const float*>(t->playerDriveParams->w.data()) : nullptr;
    }
    IAirplaneEffect* CreateEffect(const cRZString& name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name.ToChar());
        return effect ? new AirplaneEffect(effect) : nullptr;
    }
    // The first frame's translation of the model's registration point.
    bool FindRegPointTransform(IAutomatonModel* model, const cRZString& name, cS3DVector3& out) override {
        const RegPointMap* points = NativeAutomatonModelRegPoints(model);
        if (!points) return false;
        const auto it = points->find(name.ToChar());
        if (it == points->end() || it->second.empty()) return false;
        out = cS3DVector3{it->second[0].translation[0], it->second[0].translation[1], it->second[0].translation[2]};
        return true;
    }
    // SL::spEffectsManager GetEffectMaps -> MapLerpedHeight(map 1, x, z) (the water effect map).
    float EffectsWaterHeight(float x, float z) override {
        cSC4EffectsManager* e = Effects();
        cSC4EffectMaps* maps = e ? e->GetEffectMaps() : nullptr;
        // A map that never read its terrain has no vertices (and no grid to sample).
        return maps && maps->MapVertsX() > 0 ? maps->MapLerpedHeight(1, x, z) : TerrainHeight(x, z);
    }
    bool DisasterActive() override { return NativeDisasterOngoing(); }   // cISC4DisasterLayer::IsADisasterOngoing
    float DisasterSpeedMultiplier() override { return AutomataTuning().params.f1dc; }
    bool SimulatorPaused() override { cSC4Simulator* s = Simulator(); return s && s->IsPaused(); }
    bool SimulatorAdvancing() override { cSC4Simulator* s = Simulator(); return s && s->IsEmergencyPaused(); }   // vslot 0x88
    int ViewZoom() override { return NativeViewZoom(); }
    void PlayAudioEvent(std::uint32_t) override {}
    void StopAudioEvent(std::uint32_t) override {}
} g_aircraftHost;

class HelicopterHost final : public IHelicopterHost {
public:
    IHelicopterEffects* CreateEffects() override { return new HelicopterEffects(); }
    void DestroyEffects(IHelicopterEffects* effects) override { delete effects; }
    // cISC4VisualEffect vslot 0x78 GetSourceTransform.
    void GetEffectTransform(IAirplaneEffect* effect, cS3DTransform& transform) override {
        if (cSC4VisualEffect* visual = VisualOf(effect)) visual->GetSourceTransform(transform);
    }
    // Occupant manager vslot 0xe0 GetFirstOccupantByPosition (one reference, which the caller releases).
    IHelicopterObstacle* FindObstacleAt(float x, float z, std::uint32_t type) override {
        cISC4OccupantManager* occupants = Occupants();
        cISC4Occupant* found = nullptr;
        if (!occupants || !occupants->GetFirstOccupantByPosition(found, x, z, type) || !found) return nullptr;
        auto* obstacle = new Obstacle(found);
        found->Release();
        return obstacle;
    }
    // The first occupant of a box query; it stays valid until EndObstacleQuery releases the list.
    IHelicopterObstacle* FindObstacleInBox(const cS3DVector3& minCorner, const cS3DVector3& maxCorner, std::uint32_t type,
                                           sc4::cISC4OccupantFilter* filter) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return nullptr;
        const float lo[3] = {minCorner.x, minCorner.y, minCorner.z}, hi[3] = {maxCorner.x, maxCorner.y, maxCorner.z};
        OccupantList found;
        const bool any = filter ? occupants->GetOccupantsByBBox(found, lo, hi, filter, 0xffffffffu)
                                : occupants->GetOccupantsByBBox(found, lo, hi, type, 0xffffffffu);
        Obstacle* first = any && !found.empty() && found.front() ? new Obstacle(found.front()) : nullptr;
        occupants->ReleaseOccupantList(found);
        if (first) held_.push_back(first);
        return first;
    }
    void EndObstacleQuery() override {
        for (Obstacle* obstacle : held_) obstacle->Release();
        held_.clear();
    }
    const void* LotAtPoint(const cS3DVector3& point) override { return g_aircraftHost.LotAt(point); }
    // cSC43DRender::PickTerrain by ray (vslot 0x1e0).
    bool RendererSpotlightCone(const cS3DVector3& origin, const cS3DVector3& direction, cS3DVector3& target) override {
        cSC43DRender* render = CityRenderer();
        float hit[3] = {0.0f, 0.0f, 0.0f};
        if (!render || !render->PickTerrain(&origin.x, &direction.x, hit, true, false)) return false;
        target = cS3DVector3{hit[0], hit[1], hit[2]};
        return true;
    }
    // cSC43DRender::PickTerrain by viewport point (vslot 0x1e8).
    bool RendererSpotlightHit(float x, float z, cS3DVector3& target) override {
        cSC43DRender* render = CityRenderer();
        float hit[3] = {0.0f, 0.0f, 0.0f};
        if (!render || !render->PickTerrain(static_cast<int>(x), static_cast<int>(z), hit, true, false)) return false;
        target = cS3DVector3{hit[0], hit[1], hit[2]};
        return true;
    }
    float CityCellSizeX() override { cSC4City* c = HostCity(); return c ? c->CellWidthX() : 16.0f; }
    float CityCellSizeZ() override { cSC4City* c = HostCity(); return c ? c->CellWidthZ() : 16.0f; }
    // cSC4City::CellCenterToPosition(int, int) with the cell the caller passes.
    void CityCellCentre(float x, float z, float& outX, float& outZ) override {
        outX = outZ = 0.0f;
        if (cSC4City* c = HostCity()) c->CellCenterToPosition(static_cast<int>(x), static_cast<int>(z), outX, outZ);
    }
    // city vslot 0x178 is the automata controller manager: dispatch state 5 adds the "riot_police_repulsor" controller to
    // the helicopter (AddControllerFromTemplateName, vslot 0x28); any other state removes its controllers (vslot 0x70).
    void DispatchManagerReport(cISC4Occupant* occupant, int state) override {
        cSC4AutomataControllerManager* controllers = Controllers();
        if (!controllers || !occupant) return;
        if (state == 5) controllers->AddControllerFromTemplateName(occupant, cRZString("riot_police_repulsor"), nullptr);
        else controllers->RemoveControllers(occupant);
    }
    // cSC4TrafficEffectManager::GetMostRecentAccidentLocation (the vehicle manager's) as the centre of that cell.
    bool TrafficAccidentLocation(cS3DVector3& position) override {
        auto* vehicles = CityComponentAs<cSC4TrafficAutomataManager>(CityComponent::VehicleManager);
        cSC4City* city = HostCity();
        int cellX = 0, cellZ = 0;
        if (!vehicles || !city || !vehicles->EffectManager().GetMostRecentAccidentLocation(cellX, cellZ)) return false;
        position = cS3DVector3{0.0f, 0.0f, 0.0f};
        city->CellCenterToPosition(cellX, cellZ, position.x, position.z);
        return true;
    }
    void* NearestCrimeSim(const cS3DVector3& position, float radius) override {
        cSC4CrimeSimulator* simulator = NativeCrimeSimulator();
        return simulator ? simulator->GetNearestCrimeSim(&position.x, radius) : nullptr;
    }
    void* NearestCrimeProp(const cS3DVector3& position, float radius) override {
        cSC4CrimeSimulator* simulator = NativeCrimeSimulator();
        return simulator ? simulator->GetNearestCrimeProp(&position.x, radius) : nullptr;
    }
    void HoldCriminal(void* object) override {
        auto* automaton = object ? dynamic_cast<cSC4Automaton*>(static_cast<cIGZUnknown*>(object)) : nullptr;
        if (!automaton) return;
        automaton->SetOpacity(0.0f, 0.2f);
        automaton->PushState(7, nullptr, 5000.0f, true, 0, 0, nullptr);
    }
    float FadeSeconds() override { return AutomataTuning().params.f134; }
    void SetCrimePropState(void* prop, int state) override {
        if (prop) static_cast<cISC4PropOccupant*>(prop)->SetState(static_cast<std::uint32_t>(state));
    }
    // Crime simulator vslot 0x40 GetCrimeHotspot: the hotspot's cell (x, z), which the helicopter turns into a cell centre.
    bool CrimeAt(const cS3DVector3&, float, cS3DVector3& crime) override {
        cSC4CrimeSimulator* simulator = NativeCrimeSimulator();
        int x = 0, z = 0;
        if (!simulator || !simulator->GetCrimeHotspot(nullptr, &x, &z)) return false;
        crime = cS3DVector3{static_cast<float>(x), 0.0f, static_cast<float>(z)};
        return true;
    }

private:
    std::vector<Obstacle*> held_;
} g_helicopterHost;

class AircraftManagerHostImpl final : public IAircraftManagerHost {
public:
    std::int32_t PropOrientation(cISC4Occupant* occupant) override {
        void* raw = nullptr;
        if (!occupant || !occupant->QueryInterface(0xe9793a65, &raw) || !raw) return 0;
        auto* prop = static_cast<cISC4PropOccupant*>(raw);
        const std::int32_t orientation = prop->GetOrientation();
        prop->Release();
        return orientation;
    }
    const void* LotAt(const cS3DVector3& position) override { return g_aircraftHost.LotAt(position); }
    bool StationLotOccupants(IAutomataOccupant* station, std::vector<cISC4Occupant*>& out) override {
        cISC4Occupant* occupant = CityOccupantOf(station);
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = occupant && lots ? lots->GetOccupantLot(occupant) : nullptr;
        if (!lot) return false;
        std::vector<cIGZUnknown*> onLot;
        lot->GetLotOccupants(onLot);   // borrowed: the occupant manager holds them
        for (cIGZUnknown* object : onLot) out.push_back(static_cast<cISC4Occupant*>(object));
        return true;
    }
    bool IsPoliceStation(IAutomataOccupant* station) override {
        cISC4Occupant* occupant = CityOccupantOf(station);
        IPoliceOccupant* police = occupant ? NativePoliceStation(occupant) : nullptr;
        return PoliceSim() && police && cSC4PoliceSimulator::IsPoliceStation(police);
    }
    float PoliceStationEfficiency(IAutomataOccupant* station) override {
        cSC4PoliceSimulator* police = PoliceSim();
        cISC4Occupant* occupant = CityOccupantOf(station);
        return police && occupant ? police->GetStationEfficiency(NativePoliceStation(occupant)) : 1.0f;
    }
    bool IsFireStation(IAutomataOccupant* station) override {
        cSC4FireProtectionSimulator* fire = FireSim();
        cISC4Occupant* occupant = CityOccupantOf(station);
        IFireOccupant* view = occupant ? NativeFireStation(occupant) : nullptr;
        return fire && view && fire->IsFireStation(view);
    }
    float FireStationEfficiency(IAutomataOccupant* station) override {
        cSC4FireProtectionSimulator* fire = FireSim();
        cISC4Occupant* occupant = CityOccupantOf(station);
        return fire && occupant ? fire->GetStationEfficiency(NativeFireStation(occupant)) : 1.0f;
    }
    bool TerrainPointInBounds(float x, float z) override { return g_aircraftHost.TerrainPointInBounds(x, z); }
    float TerrainHeight(float x, float z) override { return g_aircraftHost.TerrainHeight(x, z); }
    float DefaultAltitude() override { return g_aircraftHost.DefaultAltitude(); }
    void SetDestination(IAutomataAutomaton* automaton, const cS3DVector3& destination) override {
        if (cSC4Automaton* object = AutomatonOf(automaton)) object->SetDestination(&destination.x);
    }
    void SetStationEfficiency(IAutomataAutomaton* automaton, float efficiency) override {
        if (auto* aircraft = dynamic_cast<cSC4Aircraft*>(AutomatonOf(automaton))) aircraft->SetStationEfficiency(efficiency);
    }
    IAutomataAutomaton* NewHelicopter() override { return Wrap(new cSC4Helicopter(AutomatonHost(), &g_aircraftHost, &g_helicopterHost)); }
    IAutomataAutomaton* NewBlimp() override { return Wrap(new cSC4Blimp(AutomatonHost(), &g_aircraftHost)); }
    cSC4BuildingPropFilter* NewBuildingPropFilter() override {
        auto* filter = new cSC4BuildingPropFilter();
        filter->AddRef();
        return filter;
    }
    void ReleaseBuildingPropFilter(cSC4BuildingPropFilter* filter) override {
        if (filter) filter->Release();
    }

private:
    static IAutomataAutomaton* Wrap(cSC4Aircraft* aircraft) {
        aircraft->AddRef();
        IAutomataAutomaton* view = AutomatonView(aircraft);
        if (!view) aircraft->Release();
        return view;   // the constructor's reference
    }
} g_managerHost;

template <class T, class... Hosts>
cIGZSerializable* NewAircraft(Hosts*... hosts) {
    auto* object = new T(AutomatonHost(), hosts...);
    object->AddRef();
    return object;
}

}  // namespace

void InstallAircraftHosts() {
    SetAircraftHost(&g_aircraftHost);
    SetHelicopterHost(&g_helicopterHost);
    SetAircraftManagerHost(&g_managerHost);
}

// The serializable factory's aircraft classes (cSC4COMSerializer creates them by class id, the automata manager by the class
// id of the automaton's group template). A blimp has no class id of its own: it saves as cSC4Aircraft.
cIGZSerializable* NativeCreateAircraftSerializable(std::uint32_t classId) {
    switch (classId) {
        case kGZCLSID_cSC4Helicopter: return NewAircraft<cSC4Helicopter>(static_cast<IAircraftHost*>(&g_aircraftHost), static_cast<IHelicopterHost*>(&g_helicopterHost));
        case kGZCLSID_cSC4Airplane: return NewAircraft<cSC4Airplane>(static_cast<IAircraftHost*>(&g_aircraftHost));
        case kGZCLSID_cSC4Aircraft: return NewAircraft<cSC4Aircraft>(static_cast<IAircraftHost*>(&g_aircraftHost));
        default: return nullptr;
    }
}

// Kit command `aircraft`: the aircraft manager's airports and runway markers, then every aircraft with its class, group,
// state and position.
void NativeAircraftReport() {
    cSC4AircraftManager* manager = Manager();
    if (!manager) {
        std::printf("aircraft: no aircraft manager\n");
        return;
    }
    std::printf("aircraft: %zu airports, %zu runway markers, %d automata (max %u, min zoom %d)\n", manager->AirportCount(),
                manager->RunwayMarkers().size(), manager->GetAutomataCount(), manager->MaxCount(), manager->MinZoom());
    float view[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    NativeVisibleTerrainRect(view);
    std::printf("aircraft: view rect (%.0f, %.0f)..(%.0f, %.0f) zoom %d\n", view[0], view[1], view[2], view[3], NativeViewZoom());
    cSC4EffectsManager* effects = Effects();
    cSC4EffectMaps* maps = effects ? effects->GetEffectMaps() : nullptr;
    std::printf("aircraft: effect maps %d x %d vertices\n", maps ? maps->MapVertsX() : -1, maps ? maps->MapVertsZ() : -1);
    for (const auto& entry : manager->Airports()) {
        float p[3] = {0.0f, 0.0f, 0.0f};
        if (entry.first) entry.first->GetPosition(p);
        cSC4Automaton* automaton = AutomatonOf(entry.second);
        std::printf("aircraft: airport occupant %p type %08x at (%.0f, %.0f, %.0f) -> automaton %p\n", static_cast<void*>(entry.first),
                    entry.first ? entry.first->GetType() : 0u, p[0], p[1], p[2], static_cast<void*>(automaton));
    }
    for (const AircraftRunwayMarker& marker : manager->RunwayMarkers())
        std::printf("aircraft: runway %u marker at (%.0f, %.0f, %.0f)\n", marker.flag, marker.x, marker.y, marker.z);
    // The city's occupants that carry one of the three properties the manager reacts to.
    cISC4OccupantManager* occupants = Occupants();
    cSC4City* city = HostCity();
    if (occupants && city) {
        const float lo[3] = {0.0f, -1.0e9f, 0.0f}, hi[3] = {city->SizeX(), 1.0e9f, city->SizeZ()};
        OccupantList all;
        int counts[3] = {0, 0, 0};
        if (occupants->GetOccupantsByBBox(all, lo, hi, static_cast<sc4::cISC4OccupantFilter*>(nullptr), 0xffffffffu)) {
            for (cISC4Occupant* occupant : all) {
                cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
                if (!holder) continue;
                const std::uint32_t ids[3] = {kAircraftPropertyHelipad, kAircraftPropertyAirport, kAircraftPropertyRunway};
                for (int i = 0; i < 3; ++i) {
                    if (!holder->HasProperty(ids[i])) continue;
                    ++counts[i];
                    float p[3] = {0.0f, 0.0f, 0.0f};
                    occupant->GetPosition(p);
                    std::uint8_t value = 0;
                    cSCPropertyHelp::GetPropertyValue(holder, ids[i], value);
                    std::printf("aircraft: occupant %p property %08x = %u at (%.0f, %.0f, %.0f)\n", static_cast<void*>(occupant), ids[i], value, p[0], p[1], p[2]);
                }
            }
        }
        const std::size_t total = all.size();
        occupants->ReleaseOccupantList(all);
        std::printf("aircraft: city occupants with helipad %d, airport %d, runway %d properties (of %zu)\n", counts[0], counts[1], counts[2], total);
    }
    for (IAutomataAutomaton* view : manager->Automata()) {
        cSC4Automaton* automaton = AutomatonOf(view);
        if (!automaton) continue;
        float p[3] = {0.0f, 0.0f, 0.0f};
        automaton->GetPosition(p);
        const tAutomataState* state = automaton->GetState();
        IAutomatonGroupTemplate* group = automaton->GetAutomataGroup();
        float screen[3] = {-1.0f, -1.0f, 0.0f};
        const bool projected = NativeProjectToViewport(p, screen);
        std::printf("aircraft: %p class %08x group %04x state %u models %zu opacity %.2f at (%.1f, %.1f, %.1f) screen %s%.0f,%.0f\n",
                    static_cast<void*>(automaton), automaton->GetGZCLSID(), group ? group->GroupId() : 0u, state ? state->stateId : 0u,
                    automaton->Models().size(), automaton->GetOpacity(), p[0], p[1], p[2], projected ? "" : "(none) ", screen[0], screen[1]);
    }
}

}  // namespace native
