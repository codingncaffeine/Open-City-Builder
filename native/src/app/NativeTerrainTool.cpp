// The god-mode terrain tools on the native application: the terrain brush tool's services
// (cSC4ViewInputControlTerrainScrubber) and the city view's terrain editor (SL::spTerrain as cSC4View3DWin calls it:
// the default raise / lower brushes, flatten, shore cliffs, raising or lowering the whole map).
#include "app/NativeTerrainTool.h"

#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativeWinMgr.h"
#include "app/NativeZoneTool.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlTerrainScrubber.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

#include <SDL3/SDL.h>

namespace native {
namespace {

sc4::cSTETerrain* Terrain() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

sc4::cSC4BudgetSimulator* Budget() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::BudgetSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* out = serializable->GetGZCLSID() == sc4::kGZCLSID_cSC4BudgetSimulator ? static_cast<sc4::cSC4BudgetSimulator*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

struct ScrubberHost final : sc4::ITerrainScrubberHost {
    sc4::cISC4View3DWin* View() override { return CityView3DWin(); }
    sc4::cSTETerrain* Terrain() override { return native::Terrain(); }
    bool BudgetPresent() override { return Budget() != nullptr; }
    std::int64_t Funds() override { sc4::cSC4BudgetSimulator* b = Budget(); return b ? b->GetTotalFunds() : 0; }
    bool WithdrawFunds(std::int64_t amount) override { sc4::cSC4BudgetSimulator* b = Budget(); return b && b->WithdrawFunds(amount); }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    void StartTick(cIGZMessageTarget2* target, std::uint32_t interval) override { StartToolTick(target, interval); }
    void StopTick(cIGZMessageTarget2* target, std::uint32_t) override { StopToolTick(target); }
    bool CursorPosition(int& x, int& y) override {
        WinMgr* manager = ActiveWinMgr();
        sc4::cSC4View3DWin* view = CityView3DWin();
        if (!manager || !view) return false;
        manager->GetCursorPos(static_cast<cIGZWin*>(static_cast<cGZWin*>(view)), x, y);
        return true;
    }
    bool CapsLockOn() override { return (SDL_GetModState() & SDL_KMOD_CAPS) != 0; }
    bool ViewportPointOnPlane(int x, int y, float altitude, float out[3]) override {
        sc4::cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* v = render ? render->GetViewUtilities() : nullptr;
        float origin[3], direction[3];
        if (!v || !v->ViewportPositionToRay(x, y, origin, direction) || direction[1] == 0.0f) return false;
        const float t = (altitude - origin[1]) / direction[1];
        out[0] = origin[0] + direction[0] * t;
        out[1] = altitude;
        out[2] = origin[2] + direction[2] * t;
        v->ClipWorldPositionToCityPosition(out);
        return true;
    }
    sc4::cISCPropertyHolder* TuningExemplar() override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0x88cd66e9, 1}, 0xa52160f5, &resource, 0, nullptr) || !resource)
            return nullptr;
        return static_cast<sc4::cISCResExemplar*>(resource)->AsISCPropertyHolder();
    }
};

// SL::spTerrain's slots as the view calls them (cSTETerrain vtable, x86-64 offsets).
struct TerrainEditor final : sc4::ITerrainEditor {
    float TerrainHeight(float x, float z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float TerrainHeightAt(float x, float z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetWaterLevel(x, z) : 0.0f; }
    void SetTerrainType(int type) override { if (sc4::cSTETerrain* t = Terrain()) t->SetTerrainType(type); }
    int TerrainType() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->TerrainType()) : 0; }
    void CreateFlatAreas(bool smoothen) override { if (sc4::cSTETerrain* t = Terrain()) t->CreateFlatAreas(smoothen); }
    void CreateShoreCliffs(bool redisplay) override { if (sc4::cSTETerrain* t = Terrain()) t->CreateShoreCliffs(redisplay); }
    float DefaultRaiseLowerAmount() override { return sc4::cSTETerrain::GetDefaultTerrainRaiseLowerAmount(); }
    void ChangeTerrainLevel(bool raise, float amount) override { if (sc4::cSTETerrain* t = Terrain()) t->ChangeTerrainLevel(raise, amount); }
    bool TerrainTypeForBrush(int brush, std::uint32_t& out) override {
        sc4::cSTETerrain* t = Terrain();
        return t && t->GetBrushExemplarID(brush, out);
    }
};

ScrubberHost g_scrubberHost;
TerrainEditor g_terrainEditor;

}  // namespace

void InstallTerrainTools() { sc4::SetTerrainScrubberHost(&g_scrubberHost); }

sc4::ITerrainEditor* CityTerrainEditor() { return Terrain() ? &g_terrainEditor : nullptr; }

sc4::cSC4ViewInputControl* CreateTerrainScrubberTool(std::uint32_t brushType, bool freeBrush) {
    auto* tool = new sc4::cSC4ViewInputControlTerrainScrubber(brushType, freeBrush);
    tool->AddRef();
    return tool;
}

}  // namespace native
