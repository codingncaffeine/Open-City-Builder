// The flammability simulator's host in a native city (it ran on the city probe's neutral binding: no agent, a city of zero cells,
// no fires). Its header cannot be combined with the lot, building and disaster headers, so what it asks of those comes from
// NativeDisasters.cpp.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativePlumbing.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4FlammabilitySimulator.h"

namespace native {
const sc4::cISCPropertyHolder* NativeHeldExemplar(const cGZPersistResourceKey& key);   // app/NativeDisasters.cpp
bool NativeTutorialRunning();
bool NativeRandomLotBuildingCell(int& x, int& z);
int NativeLotZoneTypeOfOccupant(sc4::cISC4Occupant* occupant);
sc4::cISC4Occupant* NativeLotMessageBuilding(cIGZUnknown* unknown);
void NativeStartRandomFire(int x, int z, bool onRandomLot, int cause);
float NativeOrdinanceFactor(std::uint32_t property, std::uint32_t index);
int NativeCurrentSeason();
int NativeZoneTypeAt(int x, int z);
void NativeRecordHistory(std::uint32_t id, std::int32_t value);

namespace {
using namespace sc4;


cSC4FlammabilitySimulator* Flammability() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cSC4FlammabilitySimulator*>(city->GetComponent(CityComponent::FlammabilitySimulator)) : nullptr;
}

class FlammabilityOccupant final : public ISimOccupant {
public:
    explicit FlammabilityOccupant(cISC4Occupant* occupant) : occupant_(occupant) { occupant_->AddRef(); }
    ~FlammabilityOccupant() override { occupant_->Release(); }
    std::uint32_t GetOccupantType() override { return occupant_->GetType(); }
    cISCPropertyHolder* GetPropertyHolder() override { return occupant_->AsPropertyHolder(); }
    SC4Rect GetBoundingRect() override {
        SC4CellRect cells{};
        occupant_->GetBoundingCityCells(cells);
        return SC4Rect{cells.x0, cells.z0, cells.x1, cells.z1};
    }
    bool HasProperty(std::uint32_t id) override {
        cISCPropertyHolder* holder = occupant_->AsPropertyHolder();
        return holder && holder->HasProperty(id);
    }
    cISC4Occupant* Occupant() const { return occupant_; }
private:
    cISC4Occupant* occupant_;
};

// The simulator's message target: the city simulator's daily tick and the six notifications of the original's Init.
class FlammabilityTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cIGZMessageTarget2) { *out = nullptr; return false; }
        *out = static_cast<cIGZMessageTarget2*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* message) override {
        cSC4FlammabilitySimulator* sim = Flammability();
        if (!sim) return true;
        const std::uint32_t type = message->GetType();
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        switch (type) {
            case kFlammabilityMsgOccupantAdded:
            case kFlammabilityMsgOccupantRemoved:
            case kFlammabilityMsgOccupantChanged:
                if (auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1())) {
                    FlammabilityOccupant adapter(occupant);
                    sim->OnMessage(type, &adapter);
                }
                break;
            case kFlammabilityMsgOccupantMoved: {
                // The message carries a lot (iid 0xe818fced); its building is the occupant to refresh.
                if (cISC4Occupant* occupant = NativeLotMessageBuilding(standard->GetIGZUnknown())) {
                    {
                        FlammabilityOccupant adapter(occupant);
                        sim->OnMessage(type, &adapter);
                    }
                    occupant->Release();
                }
                break;
            }
            default:
                sim->OnMessage(type, nullptr);
                if (type == kMessageDailyTick) {
                    static const bool trace = std::getenv("SC4_FIREDEBUG") != nullptr;
                    static int days = 0;
                    if (trace && ++days % 30 == 0)
                        std::fprintf(stderr, "flammability: %d daily passes, average %u, %zu hotspots\n", days, sim->GetAverageFlammability(),
                                     sim->GetHotspots().size());
                }
                break;
        }
        return true;
    }
} g_flammabilityTarget;

class FlammabilityHost final : public IFlammabilityHost {
public:
    int CitySizeXInCells() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CitySizeZInCells() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    bool RandomDisastersBlocked() override { return NativeTutorialRunning(); }
    bool PickRandomLotCell(int& x, int& z) override { return NativeRandomLotBuildingCell(x, z); }
    void StartFire(int x, int z, bool onRandomLot, int cause) override { NativeStartRandomFire(x, z, onRandomLot, cause); }
    float GetOrdinanceFlameResistance() override { return NativeOrdinanceFactor(kOrdinanceFlameResistance, 0); }
    int GetCurrentWeather() override { return NativeCurrentSeason(); }   // weather simulator vslot 0x38
    bool HasWaterAt(int x, int z) override { return NativeCellWatered(x, z); }
    int GetZoneTypeAt(int x, int z) override { return NativeZoneTypeAt(x, z); }
    int GetLotZoneTypeOfOccupant(ISimOccupant* occupant) override {
        auto* adapter = static_cast<FlammabilityOccupant*>(occupant);
        return adapter ? NativeLotZoneTypeOfOccupant(adapter->Occupant()) : -1;
    }
    std::vector<ISimOccupant*> GatherOccupants(const SC4Rect& rect, const std::uint32_t* types, int typeCount) override {
        gathered_.clear();
        std::vector<ISimOccupant*> out;
        cSC4City* city = HostCity();
        auto* manager = city ? dynamic_cast<cISC4OccupantManager*>(city->GetComponent(CityComponent::OccupantManager)) : nullptr;
        if (!manager) return out;
        const int min[2] = {rect.x0, rect.z0}, max[2] = {rect.x1, rect.z1};
        for (int i = 0; i < typeCount; ++i) {
            OccupantList list;
            manager->GetOccupantsByStandardCityCells(list, min, max, types[i], 0xffffffffu);
            for (cISC4Occupant* occupant : list) {
                gathered_.push_back(std::make_unique<FlammabilityOccupant>(occupant));
                out.push_back(gathered_.back().get());
                occupant->Release();
            }
        }
        return out;
    }
    cISCPropertyHolder* GetSimulatorTunables(const cGZPersistResourceKey& key) override {
        return const_cast<cISCPropertyHolder*>(NativeHeldExemplar(key));
    }
    void PostFlammabilityMessage() override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMessageFlammabilityUpdated);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    void AddHistoryValue(std::uint32_t historyId, std::uint8_t value) override {
        NativeRecordHistory(historyId, value);
    }
    // simulator vslot 0x4c AddAgent(target, 4, name, 2) and the six notifications of Init (0x0008f2ac)
    void RegisterSimulator(const char* name) override {
        AddCitySimulatorAgent(&g_flammabilityTarget, 4, name, 2);
        if (!SL::spMS2) return;
        for (std::uint32_t type : kNotifications) SL::spMS2->AddNotification(&g_flammabilityTarget, type);
    }
    void UnregisterSimulator() override {
        RemoveCitySimulatorAgent(&g_flammabilityTarget);
        if (!SL::spMS2) return;
        for (std::uint32_t type : kNotifications) SL::spMS2->RemoveNotification(&g_flammabilityTarget, type);
        gathered_.clear();
    }

private:
    static constexpr std::uint32_t kNotifications[6] = {kFlammabilityMsgOccupantAdded,   kFlammabilityMsgOccupantRemoved,
                                                        kFlammabilityMsgOccupantChanged, kFlammabilityMsgOrdinanceChanged,
                                                        kFlammabilityMsgWeatherChanged,  kFlammabilityMsgOccupantMoved};
    std::vector<std::unique_ptr<FlammabilityOccupant>> gathered_;   // the last GatherOccupants answer (the caller only reads it)
} g_flammabilityHost;

}  // namespace

// Set in gzcom::GetSimHosts() after the probe bindings (NativeSimHosts.cpp).
sc4::IFlammabilityHost* NativeFlammabilityHost() { return &g_flammabilityHost; }

}  // namespace native
