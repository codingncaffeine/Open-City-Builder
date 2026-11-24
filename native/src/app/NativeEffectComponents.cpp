// The effect components that act on the city, in a native city: the scrubber (demolish / burn / contaminate / extinguish under
// an effect), the attractor (automata controllers that follow an effect), the sequence (effects played one after the other)
// and the terrain brush of the brush component. Their classes reach the city through four seams; this file is the city side
// of each, and registers the three classes with the component descriptions. Installed when the effects manager comes up
// (NativeEffects.cpp). Trace: SC4_FXCOMPDEBUG=1.
#include "app/NativeEffectComponents.h"

#include <cstdio>
#include <cstdlib>

#include "app/NativeCity.h"
#include "app/NativeDemolition.h"
#include "app/NativeEffects.h"
#include "platform/Unwired.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "rz/cRZString.h"
#include "sc4/AttractorEffect.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/Demolition.h"
#include "sc4/Disaster.h"
#include "sc4/DisasterManagers.h"
#include "sc4/DummyAutoOccupant.h"
#include "sc4/EffectComponents.h"
#include "sc4/EffectMaps.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/ScrubberEffect.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::ISequenceHost* NativeSequenceHost();   // app/NativeSequenceEffects.cpp (the effect class and the disaster headers clash)

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_FXCOMPDEBUG") != nullptr;
    return on;
}

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
cSC424HourClock* Clock() {
    if (auto* clock = CityAs<cSC424HourClock>(CityComponent::Clock24Hour)) return clock;
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Clock24Hour) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
    return static_cast<cSC424HourClock*>(raw);
}
cSTETerrain* Terrain() { return CityAs<cSTETerrain>(CityComponent::Terrain); }
cISC4OccupantManager* Occupants() { return CityAs<cISC4OccupantManager>(CityComponent::OccupantManager); }
cSC4AutomataControllerManager* Controllers() { return CityAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager); }

// ---- the scrubber's city ----------------------------------------------------------------------------------------------------

bool Answers(cISC4Occupant* occupant, std::uint32_t iid) {
    void* raw = nullptr;
    if (!occupant->QueryInterface(iid, &raw) || !raw) return false;
    static_cast<cIGZUnknown*>(raw)->Release();
    return true;
}

// cScrubberFilter over the demolition's occupants.
struct ScrubberDemolishFilter final : IDemolishFilter {
    std::uint32_t flags = 0;
    float minSize = 0.0f, maxSize = 0.0f;
    int spared = 0, taken = 0;
    bool IsOccupantIncluded(IDemolishable* demolishable) override {
        cISC4Occupant* occupant = DemolitionOccupant(demolishable);
        if (!occupant) return false;
        ScrubberOccupant info;
        info.signpost = Answers(occupant, 0x4b44fbe2);
        info.textDecal = Answers(occupant, 0x2b3b7d86);
        info.network = dynamic_cast<cSC4NetworkOccupant*>(occupant) != nullptr || Answers(occupant, 0xa821ef94);
        info.type = occupant->GetType();
        occupant->GetBoundingBox(info.boxMin, info.boxMax);
        const bool included = ScrubberFilterIncludes(flags, minSize, maxSize, info);
        ++(included ? taken : spared);
        return included;
    }
    bool IsOccupantTypeIncluded(std::uint32_t) override { return true; }
};

class ScrubberHost final : public IScrubberHost {
public:
    bool HasSimulator() override { return CitySimulatorObject() != nullptr; }
    int CityCellCountX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CityCellCountZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    void DemolishRegion(const std::int32_t rect[4], std::uint32_t flags, bool option, std::uint32_t filterFlags, float minSize, float maxSize) override {
        DemolitionRegion region;
        region.bounds = DemolitionCellRect{rect[0], rect[1], rect[2], rect[3]};
        ScrubberDemolishFilter filter;
        filter.flags = filterFlags;
        filter.minSize = minSize;
        filter.maxSize = maxSize;
        const bool ok = native::DemolishRegion(true, region, 0, flags, option, nullptr, nullptr, 0, 0, &filter);
        if (Trace())
            std::fprintf(stderr, "scrubber: demolish %d,%d..%d,%d flags %x option %d filter %x -> %d, %d occupants taken, %d spared\n", rect[0], rect[1], rect[2],
                         rect[3], flags, option, filterFlags, ok, filter.taken, filter.spared);
    }
    void Burn(std::int32_t cellX, std::int32_t cellZ, std::uint32_t radius) override {
        cSC4DisasterManagerFire* fire = cSC4DisasterManagerFire::Instance();
        if (!fire) return;
        const bool lit = fire->IgniteCellRadius(static_cast<std::uint32_t>(cellX), static_cast<std::uint32_t>(cellZ), 0, radius);
        fire->SetCause(kScrubberFireCause);
        if (Trace()) std::fprintf(stderr, "scrubber: burn %d,%d radius %u -> %d\n", cellX, cellZ, radius, lit);
    }
    void Extinguish(std::int32_t cellX, std::int32_t cellZ, std::int32_t amount) override {
        if (cSC4DisasterManagerFire* fire = cSC4DisasterManagerFire::Instance())
            fire->ExtinguishCell(static_cast<std::uint32_t>(cellX), static_cast<std::uint32_t>(cellZ), amount);
    }
    void SpillToxic(std::int32_t cellX, std::int32_t cellZ) override {
        auto* layer = CityAs<cISC4DisasterLayer>(CityComponent::DisasterLayer);
        cISC4DisasterManager* toxic = layer ? layer->GetDisasterManager(kDisasterManagerIdToxic) : nullptr;
        if (toxic) toxic->AttemptDisasterStart(cellX, cellZ);
        if (Trace()) std::fprintf(stderr, "scrubber: toxic spill at %d,%d -> %s\n", cellX, cellZ, toxic ? "asked" : "no manager");
    }
    void PostScrubMessage(std::uint32_t type, std::uintptr_t data, std::int32_t cellX, std::int32_t cellZ) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard;
        message->AddRef();
        message->SetType(type);
        message->SetVoid1(reinterpret_cast<void*>(data));
        message->SetData2(static_cast<std::uint32_t>(cellX));
        message->SetData3(static_cast<std::uint32_t>(cellZ));
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    // spEffectsManager QI 0x0a5eaa1a, vslot 0x24 AddRectToMap(map, rect, value, spread) (i386 AddBlobToMap 0x003e16f2)
    void AddRectToEffectMap(std::uint32_t map, const std::int32_t rect[4], float value, std::uint32_t spread) override {
        if (sc4::cSC4EffectMaps* maps = EffectMaps())
            maps->AddRectToMap(map, sc4::SC4CellRect{rect[0], rect[1], rect[2], rect[3]}, value, spread);
    }
    void SimulatorPause(bool pause) override {
        if (cSC4Simulator* simulator = CitySimulatorObject()) pause ? simulator->Pause() : simulator->Resume();
    }
    void SimulatorHiddenPause(bool pause) override {
        if (cSC4Simulator* simulator = CitySimulatorObject()) pause ? simulator->HiddenPause() : simulator->HiddenResume();
    }
    void ClockHiddenPause(bool pause) override {
        if (cSC424HourClock* clock = Clock()) clock->HiddenPause(pause);
    }
} g_scrubberHost;

// ---- the attractor's city ---------------------------------------------------------------------------------------------------

class AttractorHost final : public IAttractorHost {
public:
    bool HasOccupantManager() override { return Occupants() != nullptr; }
    void* CreateAnchor() override {
        auto* anchor = new cSC4DummyOccupant();
        anchor->AddRef();
        anchor->Init();
        if (cISC4OccupantManager* occupants = Occupants()) occupants->InsertOccupant(anchor, 0);
        return anchor;
    }
    void* AddController(void* anchor, const char* templateName) override {
        cSC4AutomataControllerManager* controllers = Controllers();
        cSC4AutomataController* made = controllers ? controllers->AddControllerFromTemplateName(Anchor(anchor), cRZString(templateName), nullptr) : nullptr;
        if (made) made->AddRef();
        if (Trace()) std::fprintf(stderr, "attractor: controller %s -> %s\n", templateName, made ? "added" : "none");
        return made;
    }
    void RemoveController(void* handle) override {
        auto* controller = static_cast<cSC4AutomataController*>(handle);
        if (!controller) return;
        if (cSC4AutomataControllerManager* controllers = Controllers()) controllers->RemoveController(controller);
        controller->Release();
    }
    void AddGroupControllers(const char* group, void* anchor) override {
        cSC4AutomataControllerManager* controllers = Controllers();
        const bool added = controllers && controllers->AddControllersFromOccupantGroup(cRZString(group), Anchor(anchor), nullptr);
        if (Trace()) std::fprintf(stderr, "attractor: group %s -> %s\n", group, added ? "controllers added" : "none");
    }
    void RemoveGroupControllers(const char* group, void* anchor) override {
        if (cSC4AutomataControllerManager* controllers = Controllers()) controllers->RemoveControllersFromOccupantGroup(cRZString(group), Anchor(anchor));
    }
    void MoveAnchor(void* handle, const float xyz[3], bool inManager) override {
        cSC4DummyOccupant* anchor = Anchor(handle);
        if (!anchor) return;
        anchor->SetPosition(xyz);
        if (inManager)
            if (cISC4OccupantManager* occupants = Occupants()) occupants->MoveOccupant(anchor, false);
        if (cSC4AutomataControllerManager* controllers = Controllers()) controllers->MoveControllers(anchor);
    }
    void DestroyAnchor(void* handle) override {
        cSC4DummyOccupant* anchor = Anchor(handle);
        if (!anchor) return;
        if (cISC4OccupantManager* occupants = Occupants()) occupants->RemoveOccupant(anchor, false, 0);
        anchor->Shutdown();
        anchor->Release();
    }
private:
    static cSC4DummyOccupant* Anchor(void* handle) { return static_cast<cSC4DummyOccupant*>(handle); }
} g_attractorHost;

// ---- the brush component's terrain ------------------------------------------------------------------------------------------

class EffectTerrainBrush final : public ITerrainBrush {
public:
    void BeginBrush() override { if (cSTETerrain* terrain = Terrain()) terrain->BeginLongTerrainOperation(); }
    void EndBrush() override { if (cSTETerrain* terrain = Terrain()) terrain->EndLongTerrainOperation(); }
    void ApplyBrush(std::uint32_t brushId, const TerrainBrushParams& params) override {
        cSTETerrain* terrain = Terrain();
        if (!terrain) return;
        TerrainBrushParameters p;
        p.previousX = params.from.x; p.previousZ = params.from.y;
        p.currentX = params.to.x; p.currentZ = params.to.y;
        p.zoom = static_cast<std::int32_t>(params.u24);
        p.valid = params.last != 0;
        p.strengthScale = params.randomA;
        p.radiusScale = params.randomB;
        p.redisplayFlags = static_cast<std::uint32_t>(params.mode);
        p.levelAltitude = params.amount;
        const bool applied = terrain->ApplyBrush(brushId, p);
        if (Trace() && (++strokes_ <= 4 || strokes_ % 100 == 0))
            std::fprintf(stderr, "effect brush: %#x stroke %d at (%.0f, %.0f) strength %.2f width %.2f zoom %d -> %s, cells %d,%d..%d,%d\n", brushId, strokes_, p.currentX,
                         p.currentZ, p.strengthScale, p.radiusScale, p.zoom, applied ? "applied" : "no such brush", p.rect.left, p.rect.top, p.rect.right, p.rect.bottom);
    }
    bool PositionInCity(float x, float z) override { cSC4City* city = HostCity(); return city && city->LocationIsInBounds(x, z); }
    double RandomRange(double low, double high) override { return random_.RandomDoubleRangeUniform(low, high); }
private:
    cRZRandom random_{0xffffffffu};
    int strokes_ = 0;
} g_terrainBrush;

}  // namespace

void InstallEffectComponentHosts() {
    SetScrubberHost(&g_scrubberHost);
    SetAttractorHost(&g_attractorHost);
    SetSequenceHost(NativeSequenceHost());
    SetTerrainBrush(&g_terrainBrush);
    SetEffectComponentFactory(EffectComponentType::Scrubber, [](cISC4ComponentDescription* description) -> cISC4VisualEffectComponent* {
        return new cSC4ScrubberEffect(static_cast<cSC4ScrubberDescription*>(description));
    });
    SetEffectComponentFactory(EffectComponentType::Attractor, [](cISC4ComponentDescription* description) -> cISC4VisualEffectComponent* {
        return new cSC4AttractorEffect(static_cast<cSC4AttractorDescription*>(description));
    });
    SetEffectComponentFactory(EffectComponentType::Sequence, [](cISC4ComponentDescription* description) -> cISC4VisualEffectComponent* {
        return new cSC4SequenceEffect(static_cast<cSC4SequenceDescription*>(description));
    });
}

}  // namespace native
