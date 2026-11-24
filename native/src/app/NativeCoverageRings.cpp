// The city side of the view's coverage rings (sc4/CoverageRadiusEffectHelper): the police, fire, hospital and school
// buildings with their coverage radius, a building's lot centre and the ring effects.
#include <list>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeEffects.h"
#include "sc4/CoverageRadiusEffectHelper.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/Lot.h"
#include "sc4/Occupant.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/VisualEffect.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"

namespace native {
sc4::IPoliceOccupant* NativePoliceStation(sc4::cISC4Occupant* occupant);   // app/NativeSimHosts.cpp
sc4::IFireOccupant* NativeFireStation(sc4::cISC4Occupant* occupant);
namespace {
using namespace sc4;

template <class T> T* Comp(CityComponent component) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(component) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = dynamic_cast<T*>(serializable);
    serializable->Release();
    return out;
}
// the building a simulator's record stands for (one reference), or null
sc4::cISC4Occupant* OccupantOf(cIGZUnknown* object) {
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_cISC4Occupant, &raw) && raw ? static_cast<sc4::cISC4Occupant*>(raw) : nullptr;
}

class RingEffect final : public ICoverageRingEffect {
public:
    explicit RingEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    ~RingEffect() override { effect_->Release(); }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() override { return effect_->IsStarted(); }
    // the world placement goes in the effect's source transform (as the place-building tool's effects)
    void SetEffectTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }

private:
    cSC4VisualEffect* effect_;
};

struct Host final : ICoverageRadiusHost {
    bool IsPoliceStation(sc4::cISC4Occupant* occupant) override {
        IPoliceOccupant* station = occupant && Comp<cSC4PoliceSimulator>(CityComponent::PoliceSimulator) ? NativePoliceStation(occupant) : nullptr;
        return station && cSC4PoliceSimulator::IsPoliceStation(station);
    }
    bool IsFireStation(sc4::cISC4Occupant* occupant) override {
        auto* fire = Comp<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
        IFireOccupant* station = fire && occupant ? NativeFireStation(occupant) : nullptr;
        return station && fire->IsFireStation(station);
    }
    bool Buildings(int category, float share, std::vector<Building>& out) override {
        out.clear();
        const SC4Percentage percent(share);
        if (category == 0) {
            auto* police = Comp<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
            if (!police) return false;
            std::list<IProtectionStation*> stations;
            police->GetPoliceStations(stations);
            for (IProtectionStation* station : stations) {
                Building building;
                building.occupant = OccupantOf(station->Object());
                if (!building.occupant) continue;
                float covered = 0.0f;
                if (auto* occupant = dynamic_cast<IPoliceOccupant*>(station))
                    building.hasRadius = police->GetOccupantCoverage(occupant, percent, covered, building.radius);
                out.push_back(building);
            }
            return true;
        }
        if (category == 1) {
            auto* fire = Comp<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
            if (!fire) return false;
            std::list<IFireOccupant*> stations;
            fire->GetFireStations(stations);
            for (IFireOccupant* station : stations) {
                Building building;
                building.occupant = station ? OccupantOf(station->Object()) : nullptr;
                if (!building.occupant) continue;
                float covered = 0.0f;
                building.hasRadius = fire->GetOccupantCoverage(station, percent, covered, building.radius);
                out.push_back(building);
            }
            return true;
        }
        auto* residential = Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
        if (!residential) return false;
        cSC4ResidentialSimulator::OccupantList list;   // one reference each
        if (category == 2) residential->GetHospitalBuildings(list, nullptr);
        else residential->GetSchoolBuildings(list, nullptr);
        for (IResidentialOccupant* resident : list) {
            Building building;
            building.occupant = resident ? OccupantOf(resident->Object()) : nullptr;
            if (building.occupant) {
                float distance = 0.0f;
                building.hasRadius = residential->GetOccupantCoverage(resident, share, distance, building.radius);
                out.push_back(building);
            }
            if (resident) resident->Release();
        }
        return true;
    }
    bool LotCenter(sc4::cISC4Occupant* occupant, float center[3]) override {
        auto* lots = Comp<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper);   // SL::spLotManager
        cSC4Lot* lot = lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
        if (!lot) return false;
        lot->GetCenter(center);
        return true;
    }
    ICoverageRingEffect* CreateEffect(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        return effect ? new RingEffect(effect) : nullptr;
    }
} g_host;
}  // namespace

void InstallCoverageRingHost() { SetCoverageRadiusHost(&g_host); }

}  // namespace native
