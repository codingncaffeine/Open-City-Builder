// The data-view map window's services on the native application (sc4::IWinMapViewHost). The generated stub this replaces
// answered every call neutrally: no message server (so cSC4WinMapView::Init failed) and FilterUICommand false (so every
// button of the Data Views panel was vetoed before the view saw it).
//   PARTIAL: CollectHighlights (the nine per-view simulator queries), the marked-cell view (SC4MarkedCellView is not
//   recovered), the highlight effects and the pothole decal answer "nothing"; the tutorial
//   command filter always passes (no tutorial runs natively).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/WinFont.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/CameraControl.h"
#include "sc4/Exemplar.h"
#include "sc4/Render3D.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SLString.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/WinMapView.h"
#include "sc4/WinMiniMap.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_MAPVIEWDEBUG") != nullptr;
    return on;
}

// DisplayLegend's exemplar: {0x6534284a, 0x690f693f, view + 0x4a0b6800}; colours 0x4a0b47f2, label string ids 0x4a0b47f3, title
// string id 0x4a0b47f4.
constexpr std::uint32_t kExemplarType = 0x6534284a, kViewExemplarGroup = 0x690f693f, kViewExemplarBase = 0x4a0b6800;
constexpr std::uint32_t kLegendColors = 0x4a0b47f2, kLegendLabels = 0x4a0b47f3, kLegendTitle = 0x4a0b47f4;
constexpr std::uint32_t kLegendFontStyle = 0xe9c86b5f;

// The map panel's child 0x4203 is a cSC4WinMiniMap; its iid 0xca318385 view is cISC4WinMiniMap. The port's colour-sink seam
// names its calls by the vslots it saw: 0x40 = SetFlags(mask, value), 0x30 = SetDefaultZoneColorToTrans, 0x38 = SetZoneColor.
class MiniMapSink final : public IWinMapViewColorSink {
public:
    cISC4WinMiniMap* map = nullptr;   // borrowed: the panel owns the window
    void SetBaseColor(std::uint32_t mask, std::uint32_t value) override { map->SetFlags(mask, value); }
    void ResetColors() override { map->SetDefaultZoneColorToTrans(); }
    void SetColor(int index, std::uint32_t rgb) override { map->SetZoneColor(static_cast<std::uint32_t>(index), rgb); }
};

class Host final : public IWinMapViewHost {
public:
    std::uint32_t NowMilliseconds() override {
        using namespace std::chrono;
        return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
    cIGZMessageServer2* MessageServer2() override { return SL::spMS2; }
    // SL::ATM() vslots 0x28 / 0x30 / 0x40: AddTickableObject, RemoveTickableObject (any priority), SetNewPriority.
    void AddAnimationTarget(cIGZMessageTarget2* target) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, 0);
    }
    void RemoveAnimationTarget(cIGZMessageTarget2* target) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, 0xffffffffu);
    }
    void SetAnimationNotifications(cIGZMessageTarget2* target, std::uint32_t flags) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->SetNewPriority(target, flags);
    }
    void CollectHighlights(std::uint32_t, std::vector<WinMapHighlight>&) override {}
    IWinMapViewMarkedCells* CreateMarkedCells() override { return nullptr; }
    bool AddMarkedCells(IWinMapViewMarkedCells*) override { return false; }
    bool RemoveMarkedCells(IWinMapViewMarkedCells*) override { return false; }
    bool CreateEffect(const cRZString&, IWinMapViewEffect**) override { return false; }
    bool LoadLegendData(std::uint32_t view, std::vector<std::uint32_t>& colors, std::vector<std::uint32_t>& labelStrings,
                        std::uint32_t& titleString) override {
        void* resource = nullptr;
        const cGZPersistResourceKey key{kExemplarType, kViewExemplarGroup, view + kViewExemplarBase};
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &resource, 0, nullptr) || !resource) {
            if (Trace()) std::fprintf(stderr, "map view: no legend exemplar for view 0x%x\n", view);
            return false;
        }
        auto* exemplar = static_cast<cISCResExemplar*>(resource);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        cSCPropertyHelp::GetPropertyValue(holder, kLegendColors, colors);
        cSCPropertyHelp::GetPropertyValue(holder, kLegendLabels, labelStrings);
        cSCPropertyHelp::GetPropertyValue(holder, kLegendTitle, titleString);
        exemplar->Release();
        if (Trace()) std::fprintf(stderr, "map view: legend 0x%x: %zu colours, %zu labels, title 0x%08x\n", view, colors.size(), labelStrings.size(), titleString);
        return true;
    }
    void LoadString(std::uint32_t group, std::uint32_t instance, cRZString& out) override {
        out = cSLString(group, instance, true);
    }
    cIGZFont* LegendFont() override {
        gz::IWinFontSource* source = gz::WinFontSource();
        gz::IWinFont* font = source ? source->Font(kLegendFontStyle) : nullptr;
        return font ? font->AsFont() : nullptr;
    }
    IWinMapViewColorSink* ViewColorSink(cIGZWin* parent) override {
        void* raw = nullptr;
        if (!parent || !parent->GetChildAsRecursive(0x4203, kMiniMapIID, &raw) || !raw) return nullptr;
        sink_.map = static_cast<cISC4WinMiniMap*>(raw);
        sink_.map->Release();   // borrowed for the caller's few calls; the window keeps its own reference
        return &sink_;
    }
    bool OccupantLotPosition(sc4::cISC4Occupant*, float[3]) override { return false; }
    bool IsTransportationCell(int x, int z) override {
        return x >= 0 && z >= 0 && x < CitySizeXInCells() && z < CitySizeZInCells() && NativeNetworkTypesAt(x, z) != 0;
    }
    int CitySizeXInCells() override { cSC4City* city = HostCity(); return city ? static_cast<int>(city->CellCountX()) : 0; }
    int CitySizeZInCells() override { cSC4City* city = HostCity(); return city ? static_cast<int>(city->CellCountZ()) : 0; }
    cIGZBuffer* CreatePotholeDecal() override { return nullptr; }
    int MapRotation() override {
        cSC43DRender* renderer = CityRenderer();
        cSC4CameraControl* camera = renderer ? renderer->GetCameraControl() : nullptr;
        return camera ? camera->Rotation() : 0;
    }
    // SL::spCommandServer vslot 0x98: the city view handles the commands natively (as NativeView3DWinHost::ExecuteCommand).
    void SendCommand(std::uint32_t command, std::uint32_t, std::uint32_t) override {
        if (cSC4View3DWin* view = CityView3DWin()) view->ProcessViewCommand(command);
    }
    bool FilterUICommand(const cGZMessage&) override { return true; }

private:
    MiniMapSink sink_;
};

}  // namespace

void InstallWinMapViewHost() {
    static Host host;
    sc4::SetWinMapViewHost(&host);
}

// cSC4WinMapView::InitWindow on the map window the city view made from its script (NativeView3DWinHost::InitMapView).
void NativeInitMapView(cIGZWin* window) {
    if (auto* map = dynamic_cast<sc4::cSC4WinMapView*>(window)) {
        map->InitWindow();
        if (Trace()) std::fprintf(stderr, "map view: InitWindow on %p\n", static_cast<void*>(map));
    } else if (Trace()) {
        std::fprintf(stderr, "map view: InitMapView got %p, not a cSC4WinMapView\n", static_cast<void*>(window));
    }
}

}  // namespace native
