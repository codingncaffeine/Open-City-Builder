// The query tool's services on the native application (sc4::IQueryHost). Only the generated stub was installed, so the
// recovered cSC4ViewInputControlQuery picked nothing and showed nothing.
//   PARTIAL: the occupant filters accept any occupant with a model (the original builds one cSC4OccupantFilter subclass per
//   query kind); the view's mouse tips, the coverage ring, the path view, following an automaton, stoplight props, the
//   legend window proc, the abandon reason, fire / police station figures, neighbor connections, transit stations and
//   the per-cell rating maps answer "nothing" (the rows that read them stay empty).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include "app/NativeCatalogUi.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativePlumbing.h"
#include "app/NativePower.h"
#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "app/NativeZoneDeveloperHost.h"
#include "s3d/ModelInstance.h"
#include "sc4/Lot.h"
#include "sc4/Render3D.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SC4Utilities.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlQuery.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"

namespace native {
namespace {
using namespace sc4;

constexpr std::uint32_t kIIDOccupant = 0x26bb767e, kUIScriptGroup = 0x96a006b0;

bool Trace() {
    static const bool on = std::getenv("SC4_QUERYDEBUG") != nullptr;
    return on;
}

sc4::cISC4Occupant* AsOccupant(cIGZUnknown* object) {   // one reference
    void* raw = nullptr;
    return object && object->QueryInterface(kIIDOccupant, &raw) ? static_cast<sc4::cISC4Occupant*>(raw) : nullptr;
}

void OccupantCell(sc4::cISC4Occupant* occupant, int& x, int& z) {
    float p[3] = {};
    occupant->GetPosition(p);
    x = static_cast<int>(p[0] / 16.0f);
    z = static_cast<int>(p[2] / 16.0f);
}

// cISC4View3DWin as the tool reads it.
struct QueryView final : IQueryView {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    static cSC4View3DWin* View() { return CityView3DWin(); }
    void EndInput(int) override { if (cSC4View3DWin* v = View()) v->RemoveCurrentViewInputControl(false); }
    cSC4ViewInputControl* TopControl() override { cSC4View3DWin* v = View(); return v ? v->GetCurrentViewInputControl() : nullptr; }
    cIGZWin* ViewWindow() override { return View(); }
    bool PickTerrain(int x, int y, float out[3], bool flag) override { cSC4View3DWin* v = View(); return v && v->PickTerrain(x, y, out, flag); }
    bool ShowCoordinates() override { return false; }
    void SetTip(std::uint32_t, int, const cIGZString*, const cIGZString*, int) override {}
    void ClearTip(std::uint32_t) override {}
    void RecalcCoverage() override {}
} g_view;

struct AnyFilter final : IOccupantFilter {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs) return refs;
        delete this;
        return 0;
    }
    bool IsValid(const sc4::cISC4Occupant*) override { return true; }
    std::uint32_t refs = 1;
};

struct PickResult final : IPickResult {
    explicit PickResult(cIGZUnknown* object) : object_(object) {}
    ~PickResult() { if (object_) object_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_) return refs_;
        delete this;
        return 0;
    }
    cIGZUnknown* GetObject() override { return object_; }

private:
    cIGZUnknown* object_;   // one reference (the occupant)
    std::uint32_t refs_ = 1;
};

struct QueryRenderer final : IQueryRenderer {
    IOccupantFilter* CreateFilter(int kind) override {
        if (Trace()) std::fprintf(stderr, "query: filter %d\n", kind);
        return new AnyFilter();
    }
    IPickResult* Pick(int x, int y, IOccupantFilter* filter) override {
        s3d::cIS3DModelInstance* model = nullptr;
        cSC43DRender* render = CityRenderer();
        if (Trace()) std::fprintf(stderr, "query: pick %d,%d renderer %p\n", x, y, static_cast<void*>(render));
        if (!render || !render->Pick(x, y, nullptr, model) || !model) return nullptr;
        sc4::cISC4Occupant* occupant = AsOccupant(model->GetOwner());
        if (!occupant) return nullptr;
        if (filter && !filter->IsValid(occupant)) {
            occupant->Release();
            return nullptr;
        }
        if (Trace()) std::fprintf(stderr, "query: pick %d,%d -> occupant %p type %08x\n", x, y, static_cast<void*>(occupant), occupant->GetType());
        return new PickResult(occupant);
    }
    void AddLayer(cIGZUnknown*) override {}
    void RemoveLayer(cIGZUnknown*) override {}
} g_renderer;

struct QueryCity final : IQueryCity {
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
        else cellX = cellZ = 0;
    }
    int CellCountX() override { cSC4City* c = HostCity(); return c ? static_cast<int>(c->CellCountX()) : 0; }
    int CellCountZ() override { cSC4City* c = HostCity(); return c ? static_cast<int>(c->CellCountZ()) : 0; }
    void FollowAutomaton(cIGZUnknown*) override {}
    void SelectStoplightProp(cIGZUnknown*) override {}
} g_city;

// cSC4Lot as the query reads it. The lot manager hands lots out borrowed (the tool AddRefs some and reads others bare), so
// each lot keeps one wrapper for the session and its counting is a no-op.
struct QueryLot final : IQueryLot {
    explicit QueryLot(cSC4Lot* lot) : lot_(lot) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void GetOccupants(std::vector<sc4::cISC4Occupant*>& out) override {
        std::vector<cIGZUnknown*> objects;
        lot_->GetLotOccupants(objects);
        for (cIGZUnknown* object : objects) {
            // borrowed, as the original's list (the lot holds its occupants; the tool AddRefs the one it keeps)
            if (sc4::cISC4Occupant* occupant = AsOccupant(object)) {
                out.push_back(occupant);
                occupant->Release();
            }
        }
    }
    void GetCellRect(SC4CellRect& rect) override {
        std::int32_t r[4] = {};
        lot_->GetBoundingRect(r);
        rect = SC4CellRect{r[0], r[1], r[2], r[3]};
    }
    int GetState() override { return lot_->GetState(); }
    bool IsHistorical() override { return lot_->IsHistorical(); }
    void SetHistorical(bool on) override { lot_->SetHistorical(on); }
    int GetAbandonReason() override { return 0; }
    int GetOccupantWealth() override { return lot_->GetOccupantWealth(); }
    std::uint32_t GetBuildingType(bool flag) override { return lot_->GetBuildingType(flag); }
    std::uint16_t GetCapacity(int wealth) override { return lot_->GetCapacityForWealth(static_cast<std::uint32_t>(wealth)); }
    float GetOccupancy() override { return lot_->GetOccupancy(); }
    void GetCenter(float* xyz) override { lot_->GetCenter(xyz); }
    bool GetNeedsPower() override { return lot_->GetNeedsPower(); }
    bool GetHasPower() override { return lot_->GetHasPower(); }
    sc4::cISC4Occupant* GetBuildingOccupant() override {   // borrowed: the lot holds its building
        cIGZUnknown* building = lot_->GetBuilding();
        sc4::cISC4Occupant* occupant = AsOccupant(building);
        if (occupant) occupant->Release();
        if (Trace()) {
            std::uint32_t queryUi = 0;
            const bool has = occupant && occupant->AsPropertyHolder() &&
                             cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), 0x2a499f85, queryUi);
            std::fprintf(stderr, "query: lot building %p occupant %p query ui %s %08x\n", static_cast<void*>(building),
                         static_cast<void*>(occupant), has ? "yes" : "no", queryUi);
        }
        return occupant;
    }

private:
    cSC4Lot* lot_;
};

QueryLot* WrapLot(cSC4Lot* lot) {
    // never destroyed: the query tool can still ask during the view's shutdown, after static destructors ran
    static auto* wrappers = new std::unordered_map<cSC4Lot*, QueryLot*>();
    QueryLot*& wrapper = (*wrappers)[lot];
    if (!wrapper) wrapper = new QueryLot(lot);
    return wrapper;
}

struct QueryLots final : IQueryLotManager {
    IQueryLot* GetLotForOccupant(sc4::cISC4Occupant* occupant) override {
        if (!occupant) return nullptr;
        int x = 0, z = 0;
        OccupantCell(occupant, x, z);
        return GetLotAtCell(x, z, 0);
    }
    IQueryLot* GetLotAtCell(int cellX, int cellZ, int) override {
        cSC4Lot* lot = NativeLotAtCell(cellX, cellZ, false);
        if (Trace()) std::fprintf(stderr, "query: lot at %d,%d -> %p\n", cellX, cellZ, static_cast<void*>(lot));
        return lot ? WrapLot(lot) : nullptr;
    }
} g_lots;

struct QueryPower final : IQueryPowerSim {
    bool IsPowered(sc4::cISC4Occupant* occupant) override {
        int x = 0, z = 0;
        if (!occupant) return false;
        OccupantCell(occupant, x, z);
        return NativeCellPowered(x, z);
    }
} g_power;

struct QueryPlumbing final : IQueryPlumbingSim {
    bool IsWatered(sc4::cISC4Occupant* occupant) override {
        int x = 0, z = 0;
        if (!occupant) return false;
        OccupantCell(occupant, x, z);
        return NativeCellWatered(x, z);
    }
} g_plumbing;

struct QueryAura final : IQueryAuraSim {
    std::int8_t GetAura(int, int) override { return 0; }
} g_aura;

struct QuerySimulator final : IQuerySimulator {
    std::uint32_t FrameCount() override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::Simulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return 0;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        auto* simulator = serializable->GetGZCLSID() == kGZCLSID_cSC4Simulator ? static_cast<cSC4Simulator*>(serializable) : nullptr;
        serializable->Release();
        return simulator ? simulator->Counter(0) : 0;
    }
} g_simulator;

struct QueryResidential final : IQueryResidentialSim {
    void SetQueryCell(int, int) override {}
} g_residential;

struct QueryFire final : IQueryFireSim {
    float StationEffectiveness(sc4::cISC4Occupant*) override { return 0.0f; }
    bool StationFunding(sc4::cISC4Occupant*, int*, float*) override { return false; }
    void SetStationFunding(sc4::cISC4Occupant*, float) override {}
    bool DispatchInfo(sc4::cISC4Occupant*, std::uint32_t*, std::uint32_t*, std::uint32_t*) override { return false; }
} g_fire;

struct QueryPolice final : IQueryPoliceSim {
    float StationEffectiveness(sc4::cISC4Occupant*) override { return 0.0f; }
    int ArrestCount(sc4::cISC4Occupant*) override { return 0; }
    int CrimeCount(sc4::cISC4Occupant*) override { return 0; }
    bool StationFunding(sc4::cISC4Occupant*, int*, float*) override { return false; }
    void SetStationFunding(sc4::cISC4Occupant*, float) override {}
    bool DispatchInfo(sc4::cISC4Occupant*, std::uint32_t*, std::uint32_t*, std::uint32_t*) override { return false; }
    float JailEfficiency() override { return 0.0f; }
    int JailCapacity(sc4::cISC4Occupant*) override { return 0; }
    std::int64_t JailMaintenance(sc4::cISC4Occupant*) override { return 0; }
} g_police;

struct QueryBudget final : IQueryBudgetSim {
    bool OccupantValueCost(cISCPropertyHolder*, std::uint32_t, std::int64_t&) override { return false; }
} g_budget;

struct QueryNeighbors final : IQueryNeighborsSim {
    bool IsValidConnectionCell(int, int) override { return false; }
    bool FindConnection(std::uint32_t, int, int) override { return false; }
    IQueryNeighborCity* FindCityOnOtherSide(int, int) override { return nullptr; }
} g_neighbors;

struct QueryTraffic final : IQueryTrafficSim {
    std::uint8_t CongestionCell(int, int) override { return 0; }
} g_traffic;

// cSC4NetworkOccupant as IQueryNetworkOccupant (the game's network occupant answers iid 0xa821ef94 with its own interface).
struct NetworkPiece final : IQueryNetworkOccupant {
    explicit NetworkPiece(cSC4NetworkOccupant* network) : network_(network) { network_->AddRef(); }
    ~NetworkPiece() { network_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_) return refs_;
        delete this;
        return 0;
    }
    bool IsOfType(std::uint32_t type) const override { return network_->IsOfType(type); }

private:
    cSC4NetworkOccupant* network_;
    std::uint32_t refs_ = 1;
};

template <class T> T* Serializable(CityComponent component, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(component) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? dynamic_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

class Host final : public IQueryHost {
public:
    IQueryNetworkOccupant* NetworkOccupant(sc4::cISC4Occupant* occupant) override {
        auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        return network ? new NetworkPiece(network) : nullptr;
    }
    std::uint32_t ZoneTypeAt(int x, int z) override {
        cSC4ZoneManager* zones = Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId);
        return zones ? static_cast<std::uint32_t>(zones->GetZoneType(x, z)) : 0xff;
    }
    int RunModalDialog(cIGZWin* dialog) override {
        WinMgr* manager = ActiveWinMgr();
        const int result = manager ? manager->RunPopup(dialog) : -1;
        if (Trace()) std::fprintf(stderr, "query: dialog %p closed with %d\n", static_cast<void*>(dialog), result);
        return result;
    }
    void EndModalDialog(cIGZWin* dialog, std::uint32_t result) override {
        if (WinMgr* manager = ActiveWinMgr()) manager->EndDialog(0xf, dialog, result);
    }
    // PARTIAL: there is no window location saver natively; the dialog opens centered over the game window each time.
    void RestoreDialogPlace(cIGZWin* dialog) override {
        WinMgr* manager = ActiveWinMgr();
        cIGZWin* main = manager ? manager->GetMainWindow() : nullptr;
        if (main) dialog->GZWinMoveTo((main->GetW() - dialog->GetW()) / 2, (main->GetH() - dialog->GetH()) / 2);
    }
    void CloseDialog(cIGZWin* dialog) override {
        WinMgr* manager = ActiveWinMgr();
        if (!manager) return;
        manager->DestroyWindow(dialog);
        if (cSC4View3DWin* view = CityView3DWin()) manager->GZSetFocus(view);
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IQueryView* QueryView() override { return &g_view; }
    IQueryRenderer* Renderer() override { return &g_renderer; }
    IQueryCity* City() override { return &g_city; }
    IQueryLotManager* LotManager() override { return &g_lots; }
    IQueryPowerSim* PowerSim() override { return &g_power; }
    IQueryPlumbingSim* PlumbingSim() override { return &g_plumbing; }
    IQueryAuraSim* AuraSim() override { return &g_aura; }
    IQuerySimulator* Simulator() override { return &g_simulator; }
    IQueryResidentialSim* ResidentialSim() override { return &g_residential; }
    IQueryFireSim* FireSim() override { return &g_fire; }
    IQueryPoliceSim* PoliceSim() override { return &g_police; }
    IQueryBudgetSim* BudgetSim() override { return &g_budget; }
    IQueryNeighborsSim* NeighborsSim() override { return &g_neighbors; }
    IQueryTrafficSim* TrafficSim() override { return &g_traffic; }
    IQueryPathView* CreatePathView() override { return nullptr; }
    cISC4WinCalloutBox* CreateCalloutBox() override { return native::CreateCalloutBox(); }
    cIGZWin* CreateWindowFromScript(std::uint32_t instance, cIGZWin* parent, std::uint32_t id) override {
        if (!parent) {   // SC4CreateWindowFromScript puts a parentless window on the main window
            WinMgr* manager = ActiveWinMgr();
            parent = manager ? manager->GetMainWindow() : nullptr;
        }
        cIGZWin* window = native::CreateWindowFromScript(cGZPersistResourceKey{0, kUIScriptGroup, instance}, parent, id);
        if (Trace()) std::fprintf(stderr, "query: script window %08x -> %p\n", instance, static_cast<void*>(window));
        return window;
    }
    void AttachLegendWinProc(cIGZWin*) override {}
    void DetachWinProc(cIGZWin*) override {}
    bool IsKeyDown(std::uint32_t) override { return false; }
    bool IsDebugMode() override { return false; }
    void FormatMoneyFloat(double value, cRZString& out) override { SC4MakeMoneyString(static_cast<std::int64_t>(value), out); }
    void LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override { LoadLocalizedString(group, instance, out); }
};

}  // namespace

void InstallQueryHost() {
    static Host host;
    sc4::SetQueryHost(&host);
}

}  // namespace native
