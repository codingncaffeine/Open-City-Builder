// The toxic waste occupants' city services (sc4::IToxicHost): the "toxic_disaster_fx" visual effect by name, the cell's centre and
// the terrain height there. Before it, a spill stood at the world origin and showed nothing.
#include <cstdint>
#include <cstdio>

#include "app/NativeCity.h"
#include "app/NativeEffects.h"
#include "sc4/OccupantManager.h"
#include "sc4/ToxicOccupant.h"
#include "sc4/VisualEffect.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

class ToxicEffect final : public IToxicEffect {
public:
    explicit ToxicEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    void Release() override {
        effect_->Release();
        delete this;
    }
    void Start(int flags) override { effect_->Start(flags); }
    void Stop(int flags) override { effect_->Stop(flags); }
    void SetTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
private:
    cSC4VisualEffect* effect_;
};

class ToxicHost final : public IToxicHost {
public:
    IToxicEffect* CreateEffect(const char* name) override {   // effects manager vslot 0x30 (by name)
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        return effect ? new ToxicEffect(effect) : nullptr;
    }
    void CellToWorld(int cellX, int cellZ, float& x, float& z) override {   // city vslot 0x308 (i386 0x184 CellCenterToPosition)
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
        else { x = float(cellX) * 16.0f + 8.0f; z = float(cellZ) * 16.0f + 8.0f; }
    }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }   // terrain vslot 0x60
} g_toxicHost;

}  // namespace

void InstallToxicHost() { SetToxicHost(&g_toxicHost); }

// Kit command `toxics` (the spill itself: NativeToxicStart in app/NativeDisasters.cpp): every toxic waste occupant, where it stands and whether its effect runs.
void NativeToxicReport() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) { std::printf("toxics: no occupant manager\n"); return; }
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    const int lo[2] = {0, 0}, hi[2] = {city->CellCountX() - 1, city->CellCountZ() - 1};
    OccupantList list;
    manager->GetOccupantsByStandardCityCells(list, lo, hi, kOccupantTypeToxic, 0xffffffffu);
    std::printf("toxics: %zu occupants\n", list.size());
    int shown = 0;
    for (cISC4Occupant* occupant : list) {
        if (auto* toxic = dynamic_cast<cSC4ToxicOccupant*>(occupant); toxic && shown++ < 12) {
            float p[3] = {};
            toxic->GetPosition(p);
            std::printf("toxics: cell %d,%d at %.0f,%.1f,%.0f amount %u hosed %u effect %d\n", toxic->GetCellX(), toxic->GetCellZ(), p[0], p[1], p[2],
                        toxic->GetAmount(), toxic->GetHosed(), toxic->IsActive() ? 1 : 0);
        }
        occupant->Release();
    }
    manager->Release();
}

}  // namespace native
