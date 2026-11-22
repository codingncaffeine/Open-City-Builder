// The disasters in a native city: the real IDisasterHost (the layer's and the managers' city services), the instance factory,
// the fire instance's services (IFireInstanceHost), the flame effects of cSC4FireOccupant and the flammability simulator's
// host (the simulator that starts the random fires). Installed after the stub hosts (NativeCityHosts.cpp).
#include <cstdio>
#include "platform/Unwired.h"
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeDemolition.h"
#include "app/NativeEffects.h"
#include "app/NativePlumbing.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/AuraSimulator.h"
#include "sc4/Demolition.h"
#include "sc4/Disaster.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/DisasterInstanceEarthquake.h"
#include "sc4/DisasterInstanceFire.h"
#include "sc4/DisasterInstanceRiot.h"
#include "sc4/DisasterInstanceVolcano.h"
#include "sc4/DisasterManagers.h"
#include "sc4/Exemplar.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/PipeBurstInstance.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/Lot.h"
#include "sc4/Ordinance.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TutorialSystem.h"
#include "sc4/WeatherSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"
#include "tools/city_probe_plain.h"
#include "app/NativeDisasterTools.h"
#include "app/NativeVolcanoLava.h"

namespace native {
const sc4::cSC4SimGrid<std::uint8_t>* NativeFlammabilityGrid();   // app/NativeAdvisorFlammability.cpp
void NativeMoveCameraTo(float x, float y, float z);                // app/NativeCitySystems.cpp
int NativeCurrentViewLevel();
void NativeStartEffectOnOccupant(const char* name, sc4::cISC4Occupant* occupant);   // app/NativeFireEffects.cpp
void NativeStartEffectAt(const char* name, const float position[3]);
void InstallFireServices();
void InstallRiotHost();   // app/NativeRiot.cpp
void NativeRiotCityClosed();
bool NativeFireSkip(const char* what);
void NativeKillFloraOccupant(sc4::cISC4Occupant* occupant);   // app/NativeFlora.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_FIREDEBUG") != nullptr;
    return on;
}

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
cISC4DisasterLayer* Layer() { return CityAs<cISC4DisasterLayer>(CityComponent::DisasterLayer); }
cISC4OccupantManager* OccupantManager() { return CityAs<cISC4OccupantManager>(CityComponent::OccupantManager); }
cSC4ZoneDeveloper* ZoneDeveloper() { return CityAs<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper); }
cSTETerrain* Terrain() { return CityAs<cSTETerrain>(CityComponent::Terrain); }

cSC4Lot* LotOf(cISC4Occupant* occupant) {
    cSC4ZoneDeveloper* lots = ZoneDeveloper();
    return lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
}

const cISCPropertyHolder* HeldExemplar(const cGZPersistResourceKey& key) {
    static std::map<std::uint32_t, cISCResExemplar*> held;   // by instance: the tuning exemplars live for the program
    auto it = held.find(key.instance);
    if (it == held.end()) {
        void* raw = nullptr;
        cISCResExemplar* exemplar =
            (SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) && raw) ? static_cast<cISCResExemplar*>(raw) : nullptr;
        it = held.emplace(key.instance, exemplar).first;
    }
    return it->second ? it->second->AsISCPropertyHolder() : nullptr;
}

void PostCityMessage(std::uint32_t type) {
    if (!SL::spMS2) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    SL::spMS2->MessagePost(message, false);
    message->Release();
}

// ---------------------------------------------------------------------------------------------- the disaster host

class DisasterHostImpl final : public IDisasterHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    // SL::ATM(): the pipe burst (priority 0xc0) and the toxic cloud (0xff) take their ticks from the animation tick manager.
    cIGZMessageServer2* NotificationServer() override { return AnimationTickServerView(); }
    cISC4DisasterLayer* DisasterLayer() override { return Layer(); }
    cISC4OccupantManager* Occupants() override { return OccupantManager(); }
    // The automata are not in the native occupant manager: one that still has its in-city flag counts.
    bool IsOccupantInCity(cISC4Occupant* occupant) override {
        if (IDisasterHost::IsOccupantInCity(occupant)) return true;
        void* automaton = nullptr;
        if (!occupant || !occupant->QueryInterface(0xa9b40f05, &automaton) || !automaton) return false;
        occupant->Release();
        return occupant->IsFlagSet(1);
    }
    bool CityEstablished() override { cSC4City* city = HostCity(); return city && city->GetEstablished(); }
    void EmergencyPause() override { if (cSC4Simulator* s = CitySimulatorObject()) s->EmergencyPause(); }
    void EmergencyResume() override { if (cSC4Simulator* s = CitySimulatorObject()) s->EmergencyResume(); }
    void DisableSave() override { if (cSC4City* city = HostCity()) city->DisableSave(); }
    void EnableSave() override { if (cSC4City* city = HostCity()) city->EnableSave(); }
    std::uint32_t FireDispatchCount() override {
        auto* fire = CityAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
        return fire ? static_cast<std::uint32_t>(fire->GetTotalDispatchCount()) : 0;
    }
    std::uint32_t PoliceDispatchCount() override {
        auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
        return police ? static_cast<std::uint32_t>(police->GetTotalDispatchCount()) : 0;
    }
    bool PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        city->PositionToCell(x, z, cellX, cellZ);
        return true;
    }
    bool CellCenterToPosition(int cellX, int cellZ, float& x, float& z) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        city->CellCenterToPosition(cellX, cellZ, x, z);
        return true;
    }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    int CityCellCountX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CityCellCountZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    void MoveCamera(float x, float y, float z, int zoomLevel) override {
        NativeMoveCameraTo(x, y, z);
        (void)zoomLevel;   // PARTIAL: the jump keeps the player's zoom
        SC4_UNWIRED("cSC4DisasterManager camera jump: the zoom level of the jump is not applied (the player's zoom stays)");
    }
    bool AutoJumpToDisasters() override { return SL::spPreferences && SL::spPreferences[0xf08] != 0; }
    cISC4DisasterManager* CreateManager(std::uint32_t classId) override {
        cISC4DisasterManager* manager = nullptr;
        switch (classId) {
            case kGZCLSID_cSC4DisasterManagerVolcano: manager = new cSC4DisasterManagerVolcano(); break;
            case kGZCLSID_cSC4DisasterManagerFire: manager = new cSC4DisasterManagerFire(); break;
            case kGZCLSID_cSC4DisasterManagerEarthquake: manager = new cSC4DisasterManagerEarthquake(); break;
            case kGZCLSID_cSC4DisasterManagerPipeBurst: manager = new cSC4DisasterManagerPipeBurst(); break;
            case kGZCLSID_cSC4DisasterManagerRiot: manager = new cSC4DisasterManagerRiot(); break;
            case kGZCLSID_cSC4DisasterManagerEffect: manager = new cSC4DisasterManagerEffect(); break;
            case kGZCLSID_cSC4DisasterManagerToxic: manager = new cSC4DisasterManagerToxic(); break;
            default: return nullptr;
        }
        manager->AddRef();
        return manager;
    }
} g_disasterHost;

// ------------------------------------------------------------------------------------------ the earthquake's city

bool QuakeTrace() {
    static const bool on = std::getenv("SC4_DISASTERDEBUG") != nullptr;
    return on;
}

class EarthquakeHost final : public IEarthquakeHost {
public:
    float CitySizeX() override { cSC4City* c = HostCity(); return c ? float(c->CellCountX()) * 16.0f : 0.0f; }
    float CitySizeZ() override { cSC4City* c = HostCity(); return c ? float(c->CellCountZ()) * 16.0f : 0.0f; }
    float Altitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    void ApplyBrush(std::uint32_t brush, EarthquakeBrush& b) override {
        cSTETerrain* terrain = Terrain();
        if (!terrain) return;
        TerrainBrushParameters p;
        p.previousX = b.previousX; p.previousZ = b.previousZ; p.currentX = b.currentX; p.currentZ = b.currentZ;
        p.zoom = b.zoom; p.valid = b.valid; p.strengthScale = b.strengthScale; p.radiusScale = b.radiusScale;
        p.redisplayFlags = b.redisplayFlags; p.levelAltitude = b.levelAltitude;
        p.rect = TerrainRect{b.rect[0], b.rect[1], b.rect[2], b.rect[3]};
        const float before = terrain->GetAltitude(b.currentX, b.currentZ);
        const bool applied = terrain->ApplyBrush(brush, p);
        const float after = terrain->GetAltitude(b.currentX, b.currentZ);
        if (after != before) ++moved_;
        b.valid = p.valid;
        b.rect[0] = p.rect.left; b.rect[1] = p.rect.top; b.rect[2] = p.rect.right; b.rect[3] = p.rect.bottom;
        ++brushes_;
        if (QuakeTrace() && (brushes_ <= 4 || brushes_ % 50 == 0))
            std::fprintf(stderr, "earthquake: brush %#x #%d (%.0f, %.0f) -> (%.0f, %.0f) strength %.3f width %.3f -> %s, cells %d,%d..%d,%d\n", brush, brushes_,
                         b.previousX, b.previousZ, b.currentX, b.currentZ, b.strengthScale, b.radiusScale, applied ? "applied" : "refused", b.rect[0], b.rect[1],
                         b.rect[2], b.rect[3]);
        if (QuakeTrace() && (brushes_ <= 4 || brushes_ % 50 == 0))
            std::fprintf(stderr, "earthquake:   altitude there %.3f -> %.3f, valid %d, %d strokes moved the ground so far\n", before, after, p.valid, moved_);
    }
    void RedisplayTerrain(const std::int32_t rect[4]) override {
        if (cSTETerrain* t = Terrain()) t->RedisplayTerrain(true, true, TerrainRect{rect[0], rect[1], rect[2], rect[3]}, 3);
    }
    void StartTextureUpdates() override { if (cSTETerrain* t = Terrain()) t->StartAccumulatingTextureUpdates(); }
    void StopTextureUpdates() override { if (cSTETerrain* t = Terrain()) t->StopAccumulatingTextureUpdates(); }
    void BeginLongTerrainOperation() override {
        if (cSTETerrain* t = Terrain()) t->BeginLongTerrainOperation();
        if (!QuakeTrace()) return;
        for (std::uint32_t id : {kEarthquakeBrushA, kEarthquakeBrushB}) {   // what the two brushes are made of
            const auto& map = cSTETerrain::BrushDescriptions();
            const auto it = map.find(id);
            if (it == map.end()) { std::fprintf(stderr, "earthquake: brush %#x: no description\n", id); continue; }
            std::fprintf(stderr, "earthquake: brush %#x: rotate %d, %zu components\n", id, it->second.rotateToDrag, it->second.components.size());
            for (const TerrainBrushComponent& c : it->second.components) {
                std::fprintf(stderr, "earthquake:   component %08x type %d texture %u decals %u, radius", c.exemplarID, c.type, c.textureValue, c.maxDecals);
                for (float v : c.radiusPerZoom) std::fprintf(stderr, " %.2f", v);
                std::fprintf(stderr, ", strength");
                for (float v : c.strengthPerZoom) std::fprintf(stderr, " %.3f", v);
                std::fprintf(stderr, "\n");
            }
        }
    }
    void EndLongTerrainOperation() override {
        if (cSTETerrain* t = Terrain()) t->EndLongTerrainOperation();
        if (QuakeTrace()) std::fprintf(stderr, "earthquake: deformation over after %d brush strokes\n", brushes_);
        brushes_ = 0;
    }
    void* CreateEffect(const char* name, const float xyz[3]) override { return NativeCreateEffectAtPoint(name, xyz); }
    void MoveEffect(void* effect, const float xyz[3]) override { NativeMoveEffectTo(effect, xyz); }
    void ReleaseEffect(void* effect) override { NativeReleaseEffectHandle(effect); }
    void* AddRepulsor(const char* templateName, const float xyz[3]) override {
        auto* controllers = CityAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager);
        cSC4AutomataController* made = controllers ? controllers->AddControllerFromTemplateName(nullptr, cRZString(templateName), xyz) : nullptr;
        if (made) made->AddRef();
        if (QuakeTrace()) std::fprintf(stderr, "earthquake: repulsor %s at (%.0f, %.0f) -> %s\n", templateName, xyz[0], xyz[2], made ? "added" : "none");
        return made;
    }
    void RemoveRepulsor(void* handle) override {
        auto* controller = static_cast<cSC4AutomataController*>(handle);
        if (!controller) return;
        if (auto* controllers = CityAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager)) controllers->RemoveController(controller);
        controller->Release();
    }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, priority);
    }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, priority);
    }
    double RandomRange(double lo, double hi) override { return random_.RandomDoubleRangeUniform(lo, hi); }
    const cISCPropertyHolder* TuningExemplar() override { return HeldExemplar(kEarthquakeTuningKey); }
private:
    cRZRandom random_{0xffffffffu};
    int brushes_ = 0, moved_ = 0;
};
EarthquakeHost g_earthquakeHost;

// ------------------------------------------------------------------------------------------ the volcano's city

class VolcanoHostImpl final : public IVolcanoHost {
public:
    float CitySizeX() override { cSC4City* c = HostCity(); return c ? float(c->CellCountX()) * 16.0f : 0.0f; }
    float CitySizeZ() override { cSC4City* c = HostCity(); return c ? float(c->CellCountZ()) * 16.0f : 0.0f; }
    float Altitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    void* CreateEffect(const char* name, const float xyz[3], std::uint32_t finishMessage, std::uint32_t finishData) override {
        void* effect = NativeCreateEffectWithFinishMessage(name, xyz, finishMessage, finishData);
        if (QuakeTrace()) std::fprintf(stderr, "volcano: effect %s at (%.0f, %.0f, %.0f) -> %s\n", name, xyz[0], xyz[1], xyz[2], effect ? "started" : "none");
        return effect;
    }
    void StopEffect(void* effect) override {
        NativeStopEffectHandle(effect);
        if (QuakeTrace()) std::fprintf(stderr, "volcano: effect stopped, the volcano is over\n");
    }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, priority);
    }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, priority);
    }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    const cISCPropertyHolder* TuningExemplar() override { return HeldExemplar(kVolcanoTuningKey); }
    IVolcanoLava* CreateLava() override {
        IVolcanoLava* lava = CreateVolcanoLava();
        if (QuakeTrace()) std::fprintf(stderr, "volcano: lava -> %s\n", lava ? "made" : "none");
        return lava;
    }
};
VolcanoHostImpl g_volcanoHost;

// ------------------------------------------------------------------------------------------ pipe bursts

// The city's watered cells as the burst's pipe filter reads them (city vslot 0x200 = the plumbing simulator, IsCellWatered at
// i386 +0x2c). The simulator is looked up per question, so the view outlives a city change safely.
class PlumbingGridView final : public IPlumbingGrid {
public:
    bool QueryInterface(std::uint32_t, void** out) override { *out = nullptr; return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        delete this;
        return 0;
    }
    bool IsWatered(int x, int z) override {
        cSC4PlumbingSimulator* plumbing = NativePlumbingSimulator();
        return plumbing && plumbing->IsCellWatered(x, z);
    }
private:
    std::uint32_t refs_ = 1;
};

// A named effect as the burst holds it: made when the instance starts it, at the place SetTransform gave (translation in
// matrix[9..11]). The effect itself lives behind app/NativeDisasterTools.h's handle (sc4/VisualEffect.h cannot be included here).
class PipeEffect final : public sc4::cISC4VisualEffect {
public:
    explicit PipeEffect(const char* name) : name_(name) {}
    bool QueryInterface(std::uint32_t, void** out) override { *out = nullptr; return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        NativeReleaseEffectHandle(handle_);
        delete this;
        return 0;
    }
    void SetTransform(const float* matrix) override {
        position_[0] = matrix[9]; position_[1] = matrix[10]; position_[2] = matrix[11];
        if (handle_) NativeMoveEffectTo(handle_, position_);
    }
    void Start(int) override {
        if (!handle_) handle_ = NativeCreateEffectAtPoint(name_, position_);
    }
    void Stop(int flags) override { NativeStopEffectOnly(handle_, flags); }
    bool IsPlaying() override { return handle_ != nullptr; }
    void SetCompletionMessage(std::uint32_t, std::uint32_t) override {}
    void SetIntParameter(int, const void*, int) override {}
    void SetPointerParameter(int, const void*, int) override {}
private:
    const char* name_;   // the instance's string literals
    void* handle_ = nullptr;
    float position_[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t refs_ = 1;
};

// cSC4DisasterInstancePipeBurst's city (Mac i386 0x00340ec0..0x003422ce).
class PipeBurstHostImpl final : public IPipeBurstHost {
public:
    // city vslot 0x154 = the effects manager, its vslot 0x18 by name
    bool CreateEffect(const char* name, cISC4VisualEffect** out) override {
        *out = nullptr;
        if (!name || !HasNamedEffect(name)) return false;
        *out = new PipeEffect(name);
        if (QuakeTrace()) std::fprintf(stderr, "pipe burst: effect \"%s\" -> made\n", name);
        return true;
    }
    float TerrainCellSize() override { return cSTETerrain::CellWidth(); }   // terrain i386 +0x18
    // 0x003414b4: a tBrushParameters at the cell's corner (previous = current), zoom 0, valid, scales 1, redisplay flags 3,
    // then terrain i386 +0xe4 ApplyBrush(0x14, params).
    void AddSinkHole(int brush, const SinkHoleDesc& desc) override {
        cSTETerrain* terrain = Terrain();
        if (!terrain) return;
        TerrainBrushParameters p;
        p.previousX = p.currentX = desc.x;
        p.previousZ = p.currentZ = desc.z;
        p.zoom = 0;
        p.valid = desc.flag;
        p.strengthScale = desc.scaleX;
        p.radiusScale = desc.scaleZ;
        p.redisplayFlags = static_cast<std::uint32_t>(desc.mode);
        p.levelAltitude = desc.strength;
        const float before = terrain->GetAltitude(desc.x, desc.z);
        const bool applied = terrain->ApplyBrush(static_cast<std::uint32_t>(brush), p);
        if (QuakeTrace())
            std::fprintf(stderr, "pipe burst: sink hole %#x at (%.0f, %.0f) -> %s, altitude %.2f -> %.2f\n", brush, desc.x, desc.z,
                         applied ? "applied" : "refused", before, terrain->GetAltitude(desc.x, desc.z));
    }
    bool HasDemolition() override { return NativeHasDemolition(); }
    // SL::spDemolition i386 +0x20: DemolishOccupant(commit, occupant, privilege 0, flags 0x302, no cost, false, no list)
    void DemolishPipe(cISC4Occupant* occupant) override {
        const std::uint32_t flags = occupant ? occupant->GetFlags() : 0;
        const bool removal = occupant && occupant->IsFlagSet(1);
        float position[3] = {0.0f, 0.0f, 0.0f};
        if (occupant) occupant->GetPosition(position);
        const bool done = DemolishOccupant(true, occupant, 0x302, nullptr);
        if (QuakeTrace()) {
            struct PipesOnly final : cSC4OccupantFilter {
                bool IsOccupantIncluded(cISC4Occupant* o) override {
                    void* view = nullptr;
                    if (!o->QueryInterface(kGZIID_PortPipeOccupantView, &view) || !view) return false;
                    static_cast<cIGZUnknown*>(view)->Release();
                    return true;
                }
            };
            auto* pipes = new PipesOnly();
            pipes->AddRef();
            int left = 0;
            if (cISC4OccupantManager* occupants = OccupantManager()) {
                cISC4Occupant* found = nullptr;
                if (occupants->GetFirstOccupantByStandardCityCell(found, int(position[0] / 16.0f), int(position[2] / 16.0f), pipes) && found) {
                    left = 1;
                    found->Release();
                }
            }
            pipes->Release();
            std::fprintf(stderr, "pipe burst: demolish pipe (flags %#x, removal flag %d) -> %d, a pipe still in its cell %d\n", flags,
                         removal ? 1 : 0, done ? 1 : 0, left);
        }
    }
    int ViewLevel() override { return NativeCurrentViewLevel(); }   // SL::spView3D i386 +0x58
    // 0x00340fb0: the disaster tuning exemplar {0x6534284a, 0x29b6c670, 3}: leak seconds before the burst (min, max) and the zoom
    bool ReadTuning(float& minSeconds, float& maxSeconds, int& goToZoomLevel) override {
        const cISCPropertyHolder* tuning = HeldExemplar(cGZPersistResourceKey{0x6534284a, 0x29b6c670, 3});
        if (!tuning) return false;
        cSCPropertyHelp::GetPropertyValue(tuning, 0x4a08c600, minSeconds);
        cSCPropertyHelp::GetPropertyValue(tuning, 0x4a08c601, maxSeconds);
        std::int32_t zoom = goToZoomLevel;
        cSCPropertyHelp::GetPropertyValue(tuning, 0xca821b8c, zoom);
        goToZoomLevel = zoom;
        if (QuakeTrace()) std::fprintf(stderr, "pipe burst: tuning %.1f..%.1f s, zoom %d\n", minSeconds, maxSeconds, goToZoomLevel);
        return true;
    }
};
PipeBurstHostImpl g_pipeBurstHost;

// ------------------------------------------------------------------------------------------ the instance factory

class InstanceFactory final : public IDisasterInstanceFactory {
public:
    cISC4DisasterInstanceFire* CreateFire() override { return new cSC4DisasterInstanceFire(); }
    cISC4DisasterInstanceVolcano* CreateVolcano() override { return new cSC4DisasterInstanceVolcano(); }
    cISC4DisasterInstanceRiot* CreateRiot() override { return new cSC4DisasterInstanceRiot(); }
    cISC4DisasterInstancePipeBurst* CreatePipeBurst() override { return new cSC4DisasterInstancePipeBurst(); }
    cISC4DisasterInstanceEarthquake* CreateEarthquake() override { return new cSC4DisasterInstanceEarthquake(); }
    IPlumbingGrid* PlumbingGrid() override { return new PlumbingGridView(); }   // one reference
    // Mac i386 0x0033a55c: how many riots may run at once and how many leaders one may have
    bool ReadRiotTuning(int& maxInstances, int& maxLeaders) override {
        const cISCPropertyHolder* tuning = HeldExemplar(cGZPersistResourceKey{0x6534284a, kRiotTuningGroup, kRiotTuningInstance});
        if (!tuning) return false;
        std::uint32_t instances = static_cast<std::uint32_t>(maxInstances), leaders = static_cast<std::uint32_t>(maxLeaders);
        cSCPropertyHelp::GetPropertyValue(tuning, 0xcadb6b3e, instances);
        cSCPropertyHelp::GetPropertyValue(tuning, 0x2adb6b6d, leaders);
        maxInstances = static_cast<int>(instances);
        maxLeaders = static_cast<int>(leaders);
        return true;
    }
} g_factory;

// -------------------------------------------------------------------------------------- the fire instance's services

class FireInstanceHostImpl final : public IFireInstanceHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cISC4DisasterLayer* DisasterLayer() override { return Layer(); }
    cISC4OccupantManager* Occupants() override { return OccupantManager(); }
    bool HasCity() override { return HostCity() != nullptr; }
    bool HasSimulator() override { return CitySimulatorObject() != nullptr; }
    bool SimulatorPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsPaused(); }
    bool SimulatorEmergencyPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsEmergencyPaused(); }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    int CellCountX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CellCountZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    float CellWidthX() override { cSC4City* city = HostCity(); return city ? city->CellWidthX() : 16.0f; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
    }
    void CellCenterToPosition(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
    }
    bool TutorialRunning() override;
    std::uint8_t Flammability(int x, int z) override {
        const cSC4SimGrid<std::uint8_t>* grid = NativeFlammabilityGrid();
        if (!grid || x < 0 || z < 0) return 0;
        if (!grid->TractIsInBounds(x >> grid->GetTractShift(), z >> grid->GetTractShift())) return 0;
        return grid->GetCellValue(x, z);
    }
    std::uint8_t FireProtection(int x, int z, int width, int height) override {
        auto* fire = CityAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
        const cSC4SimGrid<std::uint8_t>* grid = fire ? fire->GetFireProtectionGrid() : nullptr;
        return grid ? static_cast<std::uint8_t>(grid->GetAverageValueInCellRect(x, z, width, height)) : 0;
    }
    void AmbientWind(float& x, float& z) override {
        x = z = 0.0f;
        auto* weather = CityAs<cSC4WeatherSimulator>(CityComponent::WeatherSimulator);
        if (!weather) return;
        float direction[2] = {};
        weather->GetAmbientWind(direction);
        x = direction[0];
        z = direction[1];
    }
    void AddTransientAuraEffect(float x, float z, int effect) override {
        if (auto* aura = CityAs<cISC4AuraSimulator>(CityComponent::AuraSimulator))
            aura->AddTransientEffect(x, z, static_cast<TransientAuraEffect>(effect));
    }
    void StartEffectOnOccupant(const char* name, cISC4Occupant* occupant) override { NativeStartEffectOnOccupant(name, occupant); }
    void StartEffectAt(const char* name, const float position[3]) override { NativeStartEffectAt(name, position); }
    void KillFloraOccupant(cISC4Occupant* occupant) override {
        NativeKillFloraOccupant(occupant);
    }
    bool OccupantLotRect(cISC4Occupant* occupant, SC4CellRect& rect) override {
        cSC4Lot* lot = LotOf(occupant);
        if (!lot) return false;
        std::int32_t r[4] = {};
        lot->GetBoundingRect(r);
        rect.x0 = r[0];
        rect.z0 = r[1];
        rect.x1 = r[2];
        rect.z1 = r[3];
        return true;
    }
    void SetOccupantLotCondition(cISC4Occupant* occupant, int condition) override {
        if (NativeFireSkip("condition")) return;
        if (cSC4Lot* lot = LotOf(occupant)) lot->SetCondition(static_cast<std::uint32_t>(condition));
    }
    void DemolishRegion(const SC4CellRect& rect, std::uint32_t flags) override {
        DemolitionRegion region;
        region.bounds = {rect.x0, rect.z0, rect.x1, rect.z1};
        std::int64_t cost = 0;
        native::DemolishRegion(true, region, 0, flags, false, &cost);
        if (Trace()) std::fprintf(stderr, "fire: demolished %d,%d..%d,%d flags %x\n", rect.x0, rect.z0, rect.x1, rect.z1, flags);
    }
    void AddRadiation(const float position[3], float radius, float amount) override {
        if (auto* pollution = CityAs<cSC4PollutionSimulator>(CityComponent::PollutionSimulator))
            pollution->AddRadiationToCity(position, radius, amount);
    }
    void RecordHistory(std::uint32_t id, std::int32_t value) override {
        if (auto* history = CityAs<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse)) history->RecordHistoryData(id, value);
    }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, priority);
    }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, priority);
    }
    int CurrentViewLevel() override { return NativeCurrentViewLevel(); }
    const cISCPropertyHolder* TuningExemplar() override { return HeldExemplar(kTuningKey); }
    bool LoadTuningCurve(cSC4ResponseCurve& curve, std::uint32_t property) override {
        const cISCPropertyHolder* tuning = TuningExemplar();
        std::vector<float> points;
        if (!tuning || !cSCPropertyHelp::GetPropertyValue(tuning, property, points) || points.size() < 2) return false;
        return curve.SetResponseCurveData(points.data(), points.size());
    }

private:
    static constexpr cGZPersistResourceKey kTuningKey{0x6534284a, 0x29b6c670, 0xc9b6b73d};
} g_fireHost;

}  // namespace

bool NativeTutorialRunning();
bool FireInstanceHostImpl::TutorialRunning() { return NativeTutorialRunning(); }

void InstallDisasterHosts() {
    sc4::SetDisasterHost(&g_disasterHost);
    sc4::SetDisasterInstanceFactory(&g_factory);
    sc4::SetFireInstanceHost(&g_fireHost);
    InstallFireServices();
    InstallRiotHost();
    sc4::SetEarthquakeHost(&g_earthquakeHost);
    sc4::SetVolcanoHost(&g_volcanoHost);
    sc4::SetPipeBurstHost(&g_pipeBurstHost);
}

// ---- what the flammability simulator's host (app/NativeFlammabilityHost.cpp) asks of the city: its header cannot be combined with
// the lot and disaster headers.
const sc4::cISCPropertyHolder* NativeHeldExemplar(const cGZPersistResourceKey& key) { return HeldExemplar(key); }
bool NativeTutorialRunning() {
    auto* tutorial = CityAs<sc4::cSC4TutorialSystem>(sc4::CityComponent::TutorialSystem);
    return tutorial && tutorial->IsTutorial();
}
// lot manager vslot 0x34 (a random lot) -> its building's position as a cell
bool NativeRandomLotBuildingCell(int& x, int& z) {
    sc4::cSC4ZoneDeveloper* lots = ZoneDeveloper();
    sc4::cSC4Lot* lot = lots ? lots->GetRandomLot() : nullptr;
    cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
    void* view = nullptr;
    if (!building || !building->QueryInterface(sc4::kGZIID_cISC4Occupant, &view) || !view) return false;
    auto* occupant = static_cast<sc4::cISC4Occupant*>(view);
    float position[3] = {};
    occupant->GetPosition(position);
    occupant->Release();
    HostCity()->PositionToCell(position[0], position[2], x, z);
    return true;
}
int NativeLotZoneTypeOfOccupant(sc4::cISC4Occupant* occupant) {
    sc4::cSC4Lot* lot = LotOf(occupant);
    return lot ? lot->GetZoneType() : -1;
}
// The building of the lot a "lot changed" message carries (iid 0xe818fced), one reference; null when it has none.
sc4::cISC4Occupant* NativeLotMessageBuilding(cIGZUnknown* unknown) {
    void* plain = nullptr;   // the message's object answers the plain lot (app/NativeZoneDeveloperHost.cpp PostLotMessage)
    sc4::cSC4Lot* lot = unknown && unknown->QueryInterface(sc4::kGZIID_PlainObject, &plain) ? static_cast<sc4::cSC4Lot*>(plain) : nullptr;
    cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
    void* view = nullptr;
    if (!building || !building->QueryInterface(sc4::kGZIID_cISC4Occupant, &view)) return nullptr;
    return static_cast<sc4::cISC4Occupant*>(view);
}
// disaster layer -> the fire manager's interface: IgniteCell(x, z, roll on a random lot, false, 0), then SetCause
void NativeStartRandomFire(int x, int z, bool onRandomLot, int cause) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    sc4::cISC4DisasterManagerFire* fire = nullptr;
    if (!manager || !manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerFire, reinterpret_cast<void**>(&fire)) || !fire) return;
    const bool lit = fire->IgniteCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), onRandomLot, false, 0);
    fire->SetCause(static_cast<std::uint32_t>(cause));
    fire->Release();
    if (Trace()) std::fprintf(stderr, "fire: random start at cell %d,%d (%s) -> %s\n", x, z, onRandomLot ? "random lot" : "hotspot", lit ? "lit" : "nothing lit");
}
float NativeOrdinanceFactor(std::uint32_t property, std::uint32_t index) {
    auto* ordinances = CityAs<sc4::cSC4OrdinanceSimulator>(sc4::CityComponent::OrdinanceSimulator);
    return ordinances ? ordinances->GetCumulativeFactor(property, index) : 1.0f;
}
int NativeCurrentSeason() {
    auto* weather = CityAs<sc4::cSC4WeatherSimulator>(sc4::CityComponent::WeatherSimulator);
    return weather ? static_cast<int>(weather->GetCurrentSeason()) : 0;
}
int NativeZoneTypeAt(int x, int z) {
    auto* zones = CityAs<sc4::cSC4ZoneManager>(sc4::CityComponent::ZoneManager);
    return zones ? zones->GetZoneType(x, z) : 0;
}
void NativeRecordHistory(std::uint32_t id, std::int32_t value) {
    if (auto* history = CityAs<sc4::cSC4HistoryWarehouse>(sc4::CityComponent::HistoryWarehouse)) history->RecordHistoryData(id, value);
}

// The disaster layer's part of the city's component lifecycle (cSC4City::Init / Shutdown slot 0x18 / 0x20).
bool CityComponentInit_cSC4DisasterLayer(cIGZUnknown* object, bool init) {
    auto* layer = dynamic_cast<sc4::cSC4DisasterLayer*>(object);
    if (!layer) return false;
    if (init) {
        layer->Init();
    } else {
        layer->Shutdown();
        NativeRiotCityClosed();   // the riot host's views of the old city's road tiles
    }
    return true;
}

// The fires burning in the city (the fire engines' host: fire manager -> fire instance -> GetFireCount).
int NativeFireCount() {
    sc4::cISC4DisasterLayer* layer = Layer();
    auto* manager = dynamic_cast<sc4::cSC4DisasterManagerFire*>(layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr);
    sc4::cISC4DisasterInstanceFire* fire = manager ? manager->GetFireInstance() : nullptr;
    if (!fire) return 0;
    const int count = fire->GetFireCount();
    fire->Release();
    return count;
}

// A crime of the arson kind (crime simulator, Mac i386 0x00172cea): the fire manager's IgniteOccupant, then SetCause(2).
void NativeStartArson(sc4::cISC4Occupant* building) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    sc4::cISC4DisasterManagerFire* fire = nullptr;
    if (!building || !manager || !manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerFire, reinterpret_cast<void**>(&fire)) || !fire) return;
    const bool lit = fire->IgniteOccupant(building);
    fire->SetCause(2);
    fire->Release();
    if (Trace()) std::fprintf(stderr, "fire: arson -> %s\n", lit ? "lit" : "nothing lit");
}

// The view's "go to the next disaster" button and key.
void GoToNextDisaster() {
    if (sc4::cISC4DisasterLayer* layer = Layer()) layer->GoToNextDisaster();
}

// Kit command `ignite X Z [force]`: sets the occupants of a city cell alight through the fire manager (the torch's call).
// ---- what the disaster menu's tools ask (app/NativeDisasterTools.h) ----------------------------------------------------------

namespace {
// The tools' effect as sc4/Disaster.h's interface.
class EffectFaceAdapter final : public sc4::cISC4VisualEffect {
public:
    explicit EffectFaceAdapter(IDisasterEffectFace* face) : face_(face) { face_->Retain(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        face_->Drop();
        delete this;
        return 0;
    }
    void Start(int flags) override { face_->Start(flags); }
    void Stop(int flags) override { face_->Stop(flags); }
    bool IsPlaying() override { return face_->IsPlaying(); }
    void SetCompletionMessage(std::uint32_t message, std::uint32_t userValue) override { face_->SetCompletionMessage(message, userValue); }
    void SetIntParameter(int parameter, const void* data, int count) override { face_->SetIntParameter(parameter, data, count); }
    void SetPointerParameter(int parameter, const void* data, int count) override { face_->SetPointerParameter(parameter, data, count); }
private:
    IDisasterEffectFace* face_;
    std::uint32_t refs_ = 1;
};

sc4::cISC4DisasterManagerFire* FireManagerFace() {   // one reference, or null
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    sc4::cISC4DisasterManagerFire* fire = nullptr;
    if (!manager || !manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerFire, reinterpret_cast<void**>(&fire))) return nullptr;
    return fire;
}
}  // namespace

bool NativeHasDisasterManager(std::uint32_t id) {
    sc4::cISC4DisasterLayer* layer = Layer();
    return layer && layer->GetDisasterManager(id) != nullptr;
}

void NativePipeBurstStart(int x, int z) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdPipeBurst) : nullptr;
    const bool started = manager && (x < 0 ? manager->AttemptDisasterStart() : manager->AttemptDisasterStart(x, z));
    std::printf("pipe burst: start at %d,%d -> %s\n", x, z, !manager ? "no manager" : started ? "started" : "no watered pipe");
}

void NativePipeBurstReport() {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdPipeBurst) : nullptr;
    if (!manager) { std::printf("pipe bursts: no manager\n"); return; }
    std::printf("pipe bursts: %zu instances\n", manager->GetInstances().size());
    for (sc4::cISC4DisasterInstance* instance : manager->GetInstances()) {
        auto* burst = dynamic_cast<sc4::cSC4DisasterInstancePipeBurst*>(instance);
        if (!burst) continue;
        std::printf("pipe bursts:   %zu leaking, %zu waiting, view level %d\n", burst->Leaks().size(), burst->Waiting().size(),
                    burst->ViewLevelValue());
        for (const auto& leak : burst->Leaks())
            std::printf("pipe bursts:     leak at %d,%d, %u of %u us, effect %s\n", leak.cellX, leak.cellZ, leak.elapsed, leak.burstTime,
                        leak.effect ? "yes" : "none");
    }
}

bool NativeAttemptDisasterStart(std::uint32_t id, int x, int z) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(id) : nullptr;
    return manager && manager->AttemptDisasterStart(x, z);
}

bool NativeTorchIgnite(int cellX, int cellZ, bool again, bool force, std::uint32_t value) {
    sc4::cISC4DisasterManagerFire* fire = FireManagerFace();
    if (!fire) return false;
    const bool lit = fire->IgniteCell(static_cast<std::uint32_t>(cellX), static_cast<std::uint32_t>(cellZ), again, force, value);
    fire->Release();
    return lit;
}

void NativeSetFireCause(std::uint32_t cause) {
    if (sc4::cISC4DisasterManagerFire* fire = FireManagerFace()) {
        fire->SetCause(cause);
        fire->Release();
    }
}

cIGZUnknown* NativeStartDisasterEffect(std::uint32_t managerId, IDisasterEffectFace* effect, const float position[3], std::uint32_t id) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(managerId) : nullptr;
    sc4::cISC4DisasterManagerEffect* effects = nullptr;
    auto* adapter = new EffectFaceAdapter(effect);
    cIGZUnknown* instance = nullptr;
    if (manager && manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerEffect, reinterpret_cast<void**>(&effects)) && effects) {
        cS3DVector3 at;
        at.x = position[0]; at.y = position[1]; at.z = position[2];
        if (sc4::cISC4DisasterInstance* made = effects->StartDisasterEffect(adapter, at, id)) {
            made->AddRef();   // the tool's reference
            instance = made;
        }
        effects->Release();
    }
    adapter->Release();   // the instance keeps its own
    return instance;
}

void NativeStopDisasterEffect(cIGZUnknown* instance) {
    if (auto* effect = dynamic_cast<sc4::cSC4DisasterInstanceEffect*>(instance)) effect->Stop();
}

void NativeIgniteCell(int x, int z, bool force) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    sc4::cISC4DisasterManagerFire* fire = nullptr;
    if (!manager || !manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerFire, reinterpret_cast<void**>(&fire)) || !fire) {
        std::printf("ignite: no fire manager\n");
        return;
    }
    const bool lit = fire->IgniteCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), false, force, 0);
    fire->Release();
    std::printf("ignite %d,%d -> %s\n", x, z, lit ? "lit" : "nothing lit");
}

// Kit command `fires`: the fire instance's state, one line per fire.
void NativeFireReport() {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    auto* fireManager = dynamic_cast<sc4::cSC4DisasterManagerFire*>(manager);
    sc4::cISC4DisasterInstanceFire* view = fireManager ? fireManager->GetFireInstance() : nullptr;
    auto* fire = dynamic_cast<sc4::cSC4DisasterInstanceFire*>(view);
    if (!fire) {
        std::printf("fires: no fire instance (layer %p manager %p)\n", static_cast<void*>(layer), static_cast<void*>(manager));
        if (view) view->Release();
        return;
    }
    std::printf("fires: %d burning, phase %d, severity %u, cause %u, damaged %d, toast %zu, ongoing %d\n", fire->GetFireCount(),
                fire->GetPhase(), fire->Severity(), fire->GetCause(), fire->GetDamagedOccupantsCount(), fire->ToastCount(),
                layer->IsADisasterOngoing() ? 1 : 0);
    for (sc4::cSC4FireOccupant* f : fire->Fires()) {
        float p[3] = {};
        f->GetPosition(p);
        std::printf("  fire at %.0f,%.0f,%.0f target type %08x stage %u/%u counter %u hosed %u flames %zu family %08x\n", p[0], p[1], p[2],
                    f->GetTarget() ? f->GetTarget()->GetType() : 0, f->GetStage(), f->GetMaxStage(), f->GetCounter(), f->GetHosed(),
                    f->Flames().size(), f->GetFamily());
    }
    view->Release();
}

// Kit command `toxic X Z`: the toxic disaster manager starts a spill at a cell (what a crashed toxic truck's scrubber asks for).
void NativeToxicStart(int cellX, int cellZ) {
    cSC4City* city = HostCity();
    auto* layer = city ? dynamic_cast<cISC4DisasterLayer*>(city->GetComponent(CityComponent::DisasterLayer)) : nullptr;
    cISC4DisasterManager* toxic = layer ? layer->GetDisasterManager(kDisasterManagerIdToxic) : nullptr;
    const bool started = toxic && toxic->AttemptDisasterStart(cellX, cellZ);
    std::printf("toxic %d,%d: %s\n", cellX, cellZ, !toxic ? "no manager" : started ? "started" : "refused");
}

}  // namespace native
