// IAuraHost: the city the mayor rating simulator (cSC4AuraSimulator) reads. Each service is the city component the original
// reaches through its SL:: global; the slot comments name the i386 call (Mac i386 vtables, re/names/mac/vtables.tsv).
// Two of the interface's names are older guesses: "EQCandidateMap" / "HQCandidateMap" are the residential simulator's
// school / hospital average grade maps (i386 vslots 0x9c / 0xa0), "TrafficDensity" is the trip length map (0x34) and
// "TrafficCongestion" the congestion map (0x28).
#include <cstdint>
#include <list>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeGameData.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/AuraSimulator.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/DBSegment.h"
#include "sc4/Disaster.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/Lot.h"
#include "sc4/Occupant.h"
#include "sc4/Ordinance.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"

namespace native {
const sc4::cISCPropertyHolder* NativeSimulatorExemplar(std::uint32_t instance);   // app/NativeSimHosts.cpp
std::uint64_t NativeNowMicroseconds();                                             // app/NativeSimHosts.cpp
sc4::cSC4CrimeSimulator* NativeCrimeSimulator();                                   // app/NativeCrime.cpp

namespace {
using namespace sc4;

template <class T> T* Comp(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = dynamic_cast<T*>(serializable);
    serializable->Release();
    return out;
}
class DisasterStarter final : public IAuraDisasterApi {
public:
    std::uint32_t type = 0;
    // the riot manager's AttemptDisasterStart(cellX, cellZ) (disaster manager vslot 0x38)
    void StartAtCell(int cellX, int cellZ) override {
        auto* layer = Comp<cISC4DisasterLayer>(CityComponent::DisasterLayer);
        if (cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(type) : nullptr) manager->AttemptDisasterStart(cellX, cellZ);
    }
};

class CityApi final : public IAuraCityApi {
public:
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {   // i386 0x17c
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
    }
    float CellSizeXMeters() const override { cSC4City* city = HostCity(); return city ? city->CellWidthX() : 16.0f; }
    float CellSizeZMeters() const override { cSC4City* city = HostCity(); return city ? city->CellWidthZ() : 16.0f; }
    IAuraDisasterApi* Disaster(std::uint32_t type) override {   // GetDisasterLayer (i386 0x120) -> GetDisasterManager(type)
        disaster_.type = type;
        return &disaster_;
    }
    void RemoveMessageTarget(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }   // GetSimulator -> remove
private:
    DisasterStarter disaster_;
};

// cSC4SafeListIterator<cISC4Lot*>: the zone developer's lot list as it was when the pass began (each lot referenced), a lot
// no longer standing at its own cell (deleted since) is skipped, as the original's safe list drops removed lots.
class SafeLotIterator final : public IAuraLotIterator {
public:
    void Restart() override {
        lots_.clear();
        auto* lots = Comp<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper);
        if (lots)
            for (cSC4Lot* lot : lots->Lots()) lots_.emplace_back(lot);
        at_ = lots_.begin();
    }
    bool AtEnd() override { return at_ == lots_.end(); }
    void Advance() override { if (at_ != lots_.end()) ++at_; }
    cSC4Lot* Lot() override {
        if (at_ == lots_.end()) return nullptr;
        cSC4Lot* lot = at_->get();
        std::int32_t rect[4] = {0, 0, 0, 0};
        lot->GetBoundingRect(rect);
        return NativeLotAtCell(rect[0], rect[1], false) == lot ? lot : nullptr;
    }
    void Release() override { delete this; }
private:
    std::list<cRZAutoRefCount<cSC4Lot>> lots_;
    std::list<cRZAutoRefCount<cSC4Lot>>::iterator at_ = lots_.end();
};

class LotManagerApi final : public IAuraLotManagerApi {
public:
    cSC4Lot* LotAtCell(int cellX, int cellZ, int) override { return NativeLotAtCell(cellX, cellZ, false); }   // vslot 0x28
    IAuraLotIterator* LotIterator() override {
        auto* iterator = new SafeLotIterator();
        iterator->Restart();
        return iterator;
    }
    int LotCount() override {
        auto* lots = Comp<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper);
        return lots ? lots->GetLotCount() : 0;
    }
};

class BudgetApi final : public IAuraBudgetApi {
public:
    float DepartmentFunding(std::uint32_t departmentId) override {   // GetDepartmentBudget (i386 0x6c) -> funding
        auto* budget = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(departmentId) : nullptr;
        return department ? department->GetFundingPercentage() : 0.0f;
    }
    bool IsDepartmentFunded(cISCPropertyHolder* holder, std::uint32_t purpose) override {   // GetBudgetItemForPurpose (0xcc)
        auto* budget = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        BudgetItem item;
        return budget && holder && budget->GetBudgetItemForPurpose(holder, purpose, item);
    }
    float TaxRate(std::uint32_t economyType) override {   // GetTaxRate (0x90)
        auto* budget = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        return budget ? budget->GetTaxRate(static_cast<int>(economyType)) : 0.0f;
    }
};

class ResidentialApi final : public IAuraResidentialApi {
public:
    static cSC4ResidentialSimulator* Sim() { return Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator); }
    cSC4SimGrid<float>* EQCandidateMap() override { auto* r = Sim(); return r ? r->GetGradeMap(true) : nullptr; }    // school grades
    cSC4SimGrid<float>* HQCandidateMap() override { auto* r = Sim(); return r ? r->GetGradeMap(false) : nullptr; }   // hospital grades
    void PopulationGrids(cSC4SimGrid<std::uint16_t>*& wealth1, cSC4SimGrid<std::uint16_t>*& wealth2,
                         cSC4SimGrid<std::uint16_t>*& wealth3) override {   // GetPopulationGrids (0x64), counted
        cSC4ResidentialSimulator* r = Sim();
        wealth1 = r ? r->GetPopulationGrid(0) : nullptr;
        wealth2 = r ? r->GetPopulationGrid(1) : nullptr;
        wealth3 = r ? r->GetPopulationGrid(2) : nullptr;
        for (cSC4SimGrid<std::uint16_t>* grid : {wealth1, wealth2, wealth3})   // one reference each: the caller releases them
            if (grid) grid->AddRef();
    }
    void EndSchoolStrike() override { if (auto* r = Sim()) r->EndSchoolStrike(); }   // 0x20
    void EndHealthStrike() override { if (auto* r = Sim()) r->EndHealthStrike(); }   // 0x24
};

class CrimeGrid final : public IAuraGridApi {
public:
    cSC4SimGrid<std::uint8_t>* Uint8Map() override {   // GetCrimeGrid (0x14)
        cSC4CrimeSimulator* crime = NativeCrimeSimulator();   // a plain object behind its adapter
        return crime ? const_cast<cSC4SimGrid<std::uint8_t>*>(crime->GetCrimeGrid()) : nullptr;
    }
};
class TrafficGrid final : public IAuraGridApi {
public:
    explicit TrafficGrid(bool tripLength) : tripLength_(tripLength) {}
    cSC4SimGrid<std::uint8_t>* Uint8Map() override {
        auto* traffic = Comp<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
        if (!traffic) return nullptr;
        return tripLength_ ? traffic->GetTripLengthMap() : traffic->GetCongestionMap();
    }
private:
    bool tripLength_;
};

class PollutionApi final : public IAuraPollutionApi {
public:
    cSC4SimGrid<std::int16_t>* TypeMap(int type) override {   // GetPollutionGrid(type) (0x14)
        auto* pollution = Comp<cSC4PollutionSimulator>(CityComponent::PollutionSimulator);
        return pollution ? pollution->GetPollutionGrid(static_cast<PollutionType>(type)) : nullptr;
    }
};

class OrdinanceApi final : public IAuraOrdinanceApi {
public:
    int OrdinanceValue(std::uint32_t property, int index) override {   // GetCumulativeOffset (0x60)
        auto* ordinances = Comp<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator);
        return ordinances ? ordinances->GetCumulativeOffset(property, static_cast<std::uint32_t>(index)) : 0;
    }
};

class LandValueApi final : public IAuraLandValueApi {
public:
    int LandValueAtCell(int, int) override { return 0; }   // the simulator never asks (kept for the interface)
};

class HistoryApi final : public IAuraHistoryApi {
public:
    void AddHistory(std::uint32_t historyId, std::int64_t value) override {
        if (auto* history = Comp<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse))
            history->RecordHistoryData(historyId, static_cast<int>(value));
    }
};

class StrikeApi final : public IAuraStrikeApi {
public:
    explicit StrikeApi(bool police) : police_(police) {}
    void EndStrike() override {   // police i386 0x34 / fire 0x38
        if (police_) {
            if (auto* police = Comp<cSC4PoliceSimulator>(CityComponent::PoliceSimulator)) police->EndStrike();
        } else if (auto* fire = Comp<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator)) {
            fire->EndStrike();
        }
    }
private:
    bool police_;
};

class Service final : public IAuraService {};

class AuraHost final : public IAuraHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IAuraCityApi* City() override { return HostCity() ? &city_ : nullptr; }
    IAuraLotManagerApi* LotManager() override { return Comp<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper) ? &lots_ : nullptr; }
    IAuraBudgetApi* Budget() override { return Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator) ? &budget_ : nullptr; }
    IAuraResidentialApi* Residential() override { return ResidentialApi::Sim() ? &residential_ : nullptr; }
    IAuraGridApi* Crime() override { return &crime_; }
    IAuraGridApi* TrafficDensity() override { return &tripLength_; }
    IAuraGridApi* TrafficCongestion() override { return &congestion_; }
    IAuraPollutionApi* Pollution() override { return &pollution_; }
    IAuraOrdinanceApi* Ordinance() override { return &ordinance_; }
    IAuraLandValueApi* LandValue() override { return &landValue_; }
    IAuraHistoryApi* History() override { return &history_; }
    IAuraStrikeApi* PoliceStrike() override { return &policeStrike_; }
    IAuraStrikeApi* FireStrike() override { return &fireStrike_; }
    IAuraService* BuildingDevelopment() override { return &buildingDevelopment_; }
    IAuraService* Simulator() override { return CitySimulatorPresent() ? &simulator_ : nullptr; }
    std::int32_t SimDateNumber() override {   // simulator vslot 0x40 -> the day number
        cSC4Simulator* simulator = CitySimulatorObject();
        return simulator ? static_cast<std::int32_t>(simulator->GetSimDateNumber()) : 0;
    }
    void AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override {
        AddCitySimulatorAgent(target, interval, name, flags);
    }
    bool SimulatorPresent() override { return CitySimulatorPresent(); }
    bool GetTuningProperties(cISCPropertyHolder** out) override {   // {0x6534284a, 0xe7e2c2db, 0xaa023079}
        *out = const_cast<cISCPropertyHolder*>(NativeSimulatorExemplar(0xaa023079));
        return *out != nullptr;
    }
    cISC4GameDataRegistry* GameDataRegistry() override { return NativeGameDataRegistry(); }
    cISC4Occupant* BuildingOccupant(cSC4Lot* lot) override {   // the lot's building as an occupant, not counted
        cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
        void* view = nullptr;
        if (!building || !building->QueryInterface(kGZIID_cISC4Occupant, &view) || !view) return nullptr;
        auto* occupant = static_cast<cISC4Occupant*>(view);
        occupant->Release();
        return occupant;
    }
    std::uint64_t BeginSlice(std::uint32_t milliseconds) override { return NativeNowMicroseconds() + std::uint64_t{milliseconds} * 1000u; }
    bool SliceExpired(std::uint64_t token) override { return NativeNowMicroseconds() >= token; }
    void WriteGrid(cIGZOStream& stream, cIGZSerializable* grid) override { SC4WriteGZSerializable(stream, grid); }
    // The read answers the grid interface (0xc9bd939a, the grid's first base); the simulator wants the serializable view, a
    // different sub-object (the stub cast the pointer as it came, so every saved aura grid failed the type check and was dropped).
    cIGZSerializable* ReadGrid(cIGZIStream& stream) override {
        void* out = nullptr;
        if (!SC4ReadGZSerializable(stream, 0xc9bd939a, &out, false) || !out) return nullptr;
        auto* grid = static_cast<cIGZUnknown*>(out);
        void* view = nullptr;
        const bool ok = grid->QueryInterface(0xe4fda3d4, &view) && view;
        grid->Release();   // the view's reference replaces the read's
        return ok ? static_cast<cIGZSerializable*>(view) : nullptr;
    }

private:
    CityApi city_;
    LotManagerApi lots_;
    BudgetApi budget_;
    ResidentialApi residential_;
    CrimeGrid crime_;
    TrafficGrid tripLength_{true};
    TrafficGrid congestion_{false};
    PollutionApi pollution_;
    OrdinanceApi ordinance_;
    LandValueApi landValue_;
    HistoryApi history_;
    StrikeApi policeStrike_{true};
    StrikeApi fireStrike_{false};
    Service buildingDevelopment_;
    Service simulator_;
} g_aura;

}  // namespace

void InstallAuraHost() { sc4::SetAuraHost(&g_aura); }

}  // namespace native
