// The concrete advisors' statistics pass (IAdvisorConcreteHost::UpdateSimulatorStatistics): the `game` variables each
// advisor publishes from the simulators (OpenSC4 advisors.md 7.3-7.8). The recovered advisors route their whole
// simulator walk through this one seam; each block below is the variable table of one advisor.
//
// PARTIAL: g_road_funding_p / g_masstransit_funding_p / g_power_funding_p (their departments are not named here yet), the HQ occupant minimum-funding / grade locals and tract extremes, the school / health strike chances, the
// traffic station utilizations and connection congestion, the port counts by size and stage, the neighbour-deal
// "can buy / sell" proposals, the power-plant / water-building locals and the regional commodity imports / exports.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <string>
#include <vector>

#include "app/NativeCity.h"
#include "sc4/AdviceSubject.h"
#include "sc4/AdvisorConcrete.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/AirportDeveloper.h"
#include "sc4/AuraSimulator.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/DemandSimulator.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/LandfillDeveloper.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/PowerSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/SeaportDeveloper.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4Demand.h"
#include "sc4/cSC4ZoneManager.h"

namespace native {
// NativeAdvisors.cpp (the flammability header clashes with the fire headers here): the hotspot and the average.
bool AdvisorFlammability(std::uint8_t* hotspot, int* x, int* z, std::uint8_t* average);
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

IAdvisorConcreteHost* H() { return AdvisorConcreteHost(); }
void G(const char* name, double value) { H()->SetGlobal(name, value); }
float X100(float v) { return v * 100.0f; }   // "x100": rounded to float32, then widened (advisors.md 7.1)

// A subject at a city cell (one reference, handed to SetLocal and released here).
void LocalAtCell(const char* name, double value, int x, int z) {
    cISC4AdviceSubject* subject = nullptr;
    if (H()->CreateAdviceSubject(&subject) && subject) subject->SetLocation(static_cast<float>(x), static_cast<float>(z));
    H()->SetLocal(name, value, subject);
    if (subject) subject->Release();
}
void LocalNone(const char* name, double value) { H()->SetLocal(name, value, nullptr); }

float DepartmentFunding(std::uint32_t id) {
    auto* b = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
    cSC4DepartmentBudget* d = b ? b->GetDepartmentBudget(id) : nullptr;
    return d ? d->GetFundingPercentage() : 0.0f;
}

// ---- CityPlanning (7.3)
void CityPlanning() {
    if (auto* aura = Comp<cSC4AuraSimulator>(CityComponent::AuraSimulator)) {
        G("ga_mayor_rating", static_cast<double>(static_cast<std::int8_t>(aura->GetMayorRating())));
        AuraPoint p;
        const int low = static_cast<std::int8_t>(aura->GetMinMayorRating(p));
        LocalAtCell("l_mayor_rating_l", low, p.x, p.z);
        const int high = static_cast<std::int8_t>(aura->GetMaxMayorRating(p));
        LocalAtCell("l_mayor_rating_h", high, p.x, p.z);
    }
    if (auto* demand = Comp<cSC4DemandSimulator>(CityComponent::DemandSimulator)) {
        auto supply = [&](std::uint32_t id, std::uint32_t offset) {
            cSC4Demand* d = demand->GetDemand(id, offset);
            return d ? d->QuerySupplyValue() : 0.0f;
        };
        for (std::uint32_t region : {0u, 0x20000u}) {
            const char* scope = region ? "region" : "city";
            const float r = supply(0x1001, region), c = supply(0x3001, region), i = supply(0x4001, region);
            char name[64];
            std::snprintf(name, sizeof name, "g_%s_r_population", scope); G(name, r);
            std::snprintf(name, sizeof name, "g_%s_c_population", scope); G(name, c);
            std::snprintf(name, sizeof name, "g_%s_i_population", scope); G(name, i);
            std::snprintf(name, sizeof name, "g_%s_rci_population", scope); G(name, (i + c) + r);
            std::snprintf(name, sizeof name, "g_%s_workforce_population", scope); G(name, supply(0x2100, region));
        }
        G("g_city_r1_population", supply(0x1011, 0));
        G("g_city_r2_population", supply(0x1021, 0));
        G("g_city_r3_population", supply(0x1031, 0));
        static const struct { const char* name; std::uint32_t key; } kJobs[] = {
            {"g_city_ir_population", 0x4101}, {"g_city_id_population", 0x4201}, {"g_city_im_population", 0x4301},
            {"g_city_iht_population", 0x4401}, {"g_city_co3_population", 0x3331}, {"g_city_co2_population", 0x3321},
            {"g_city_cs1_population", 0x3111}, {"g_city_cs2_population", 0x3121}, {"g_city_cs3_population", 0x3131}};
        for (const auto& j : kJobs) G(j.name, static_cast<double>(static_cast<std::uint32_t>(demand->GetJobsBySensus(j.key))));
        auto region = [&](std::uint32_t node) { return demand->GetDemand(node, 0x20000); };
        static const struct { const char* tag; std::uint32_t node; } kNodes[] = {
            {"r1", 0x1010}, {"r2", 0x1020}, {"r3", 0x1030}, {"co2", 0x3320}, {"co3", 0x3330},
            {"id", 0x4200}, {"im", 0x4300}, {"iht", 0x4400}, {"workforce", 0x2000}};
        for (const auto& n : kNodes) {
            cSC4Demand* d = region(n.node);
            char name[64];
            if (std::string(n.tag).size() <= 3 && n.node < 0x4000) {   // r1..r3, co2, co3 publish their demand
                std::snprintf(name, sizeof name, "g_%s_demand", n.tag);
                G(name, d ? d->QueryDemandValue() : 0.0f);
            }
            std::snprintf(name, sizeof name, "g_%s_active_demand", n.tag);
            G(name, d ? d->QueryActiveDemandValue() : 0.0f);
            if (n.node != 0x2000) {
                std::snprintf(name, sizeof name, "g_current_%s_cap", n.tag);
                G(name, X100(d ? d->GetDemandCap() : 0.0f));
            }
        }
        cSC4Demand* workforce = region(0x2000);
        G("g_workforce_demand", workforce ? workforce->QueryDemandValue() : 0.0f);
        G("g_tax_rate_neutral", demand->GetNeutralTaxRate());
    }
    if (auto* zones = Comp<cSC4ZoneManager>(CityComponent::ZoneManager)) {
        static const char* const kZones[] = {"g_num_rzone_ld_tiles", "g_num_rzone_md_tiles", "g_num_rzone_hd_tiles",
                                             "g_num_czone_ld_tiles", "g_num_czone_md_tiles", "g_num_czone_hd_tiles",
                                             "g_num_izone_r_tiles",  "g_num_izone_l_tiles",  "g_num_izone_h_tiles"};
        for (int t = 1; t <= 9; ++t) G(kZones[t - 1], zones->GetZoneCount(t));
    }
    if (auto* neighbors = Comp<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator)) {
        ConnectionList list;
        static const struct { const char* name; std::uint32_t type; } kConn[] = {
            {"g_num_rail_neighbors", 1}, {"g_num_road_neighbors", 2}, {"g_num_avenue_neighbors", 8}};
        for (const auto& c : kConn) { list.clear(); neighbors->GetConnectionsOfType(c.type, list); G(c.name, static_cast<double>(list.size())); }
        std::list<cRZAutoRefCount<cISC4RegionalCity>> cities;
        neighbors->GetConnectedCities(cities, false);
        G("g_num_cities_connected", static_cast<double>(cities.size()));
        cities.clear();
        neighbors->GetAdjacentCities(cities, false);
        G("g_num_cities_adjacent", static_cast<double>(cities.size()));
    }
    if (auto* seaports = Comp<cSC4SeaportDeveloper>(CityComponent::SeaportDeveloper)) G("g_city_has_sea", seaports->CityHasSea() ? 1.0 : 0.0);
    if (cSC4City* city = HostCity()) G("g_city_tile_size", city->SizeX() * city->SizeZ());
}

// ---- HQ occupant variables (7.5)
void HQ() {
    auto* res = Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
    if (!res) return;
    auto count = [&](bool school, std::initializer_list<std::uint32_t> groups) {
        std::vector<std::uint32_t> g(groups);
        cSC4ResidentialSimulator::OccupantList list;
        if (school) res->GetSchoolBuildings(list, &g); else res->GetHospitalBuildings(list, &g);
        const double n = static_cast<double>(list.size());
        for (auto* o : list) if (o) o->Release();
        return n;
    };
    G("g_num_hospitals", count(false, {0x1513}));
    G("g_num_clinics", count(false, {0x1512}));
    const double schools = count(true, {0x150f, 0x1510, 0x1514, 0x1504});
    G("g_num_schools", schools);
    G("g_num_museums", count(true, {0x1506}));
    G("g_num_libraries", count(true, {0x1505}));
    G("g_num_educational_buildings", schools);
    G("g_num_elem_schools", count(true, {0x150f}));
    G("g_num_high_schools", count(true, {0x1510}));
    G("g_num_colleges", count(true, {0x1504}));
    const float population = static_cast<float>(res->GetPopulation());
    auto totals = [&](bool school, std::initializer_list<std::uint32_t> groups, ResidentialQueryData& t) {
        std::vector<std::uint32_t> g(groups);
        return school ? res->GetSchoolSystemTotals(t, &g) : res->GetHospitalSystemTotals(t, &g);
    };
    ResidentialQueryData health;
    const int healthCount = totals(false, {0x1513, 0x1512}, health);
    G("g_health_coverage_p", (static_cast<float>(health.served) / (population + 1.0f)) * 100.0f);
    G("ga_health_grade", healthCount > 0 ? (health.grade / static_cast<float>(healthCount)) * 100.0f : 0.0f);
    ResidentialQueryData school;
    const int schoolCount = totals(true, {0x150f, 0x1510, 0x1514, 0x1504}, school);
    G("g_education_coverage_p", (static_cast<float>(school.served) / (population + 1.0f)) * 100.0f);
    G("ga_school_grade", schoolCount > 0 ? (school.grade / static_cast<float>(schoolCount)) * 100.0f : 0.0f);
    ResidentialQueryData museum, library;
    const int museums = totals(true, {0x1506}, museum), libraries = totals(true, {0x1505}, library);
    G("ga_museum_grade", museums > 0 ? (museum.grade / static_cast<float>(museums)) * 100.0f : 0.0f);
    G("ga_library_grade", libraries > 0 ? (library.grade / static_cast<float>(libraries)) * 100.0f : 0.0f);
    G("g_num_museum_served", museum.served);
    G("g_num_library_served", library.served);
}

// ---- Safety (7.6)
void Safety() {
    auto* fire = Comp<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
    auto* police = Comp<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
    auto* crime = Comp<cSC4CrimeSimulator>(CityComponent::CrimeSimulator);
    auto* res = Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
    if (fire) G("g_fire_strike_chance", X100(fire->GetChanceOfStrike()));
    if (res) G("g_population", res->GetPopulation());
    std::uint8_t v = 0; int x = 0, z = 0;
    std::uint8_t average = 0;
    if (AdvisorFlammability(&v, &x, &z, &average)) {
        LocalAtCell("l_flammability_h", v, x, z);
        G("ga_flammability", average);
    }
    if (crime) {
        G("ga_crime", crime->GetAverageCrime());
        crime->GetCrimeHotspot(&v, &x, &z);   // the same v, x, z, not zeroed again (quirk kept)
        LocalAtCell("l_crime_h", v, x, z);
        G("g_crime_commited_count", static_cast<double>(static_cast<std::uint32_t>(crime->GetMonthlyCrimeCount())));
    }
    if (police) {
        G("g_inmate_count", police->GetJailPopulation());
        G("g_criminal_count", police->GetCriminalCount());
        G("g_arrest_count", police->GetArrestCount());
        G("g_police_strike_chance", X100(police->GetChanceOfStrike()));
        G("g_police_station_count", static_cast<double>(police->GetPoliceStationCount()));
        G("g_jail_count", static_cast<double>(police->GetJailCount()));
        G("ga_police_coverage", police->GetAverageCoverage());
        G("g_police_coverage_p", X100(police->GetPercentCoverage()));
        G("g_jail_capacity", police->GetJailCapacity());
    }
    G("g_school_strike_chance", 0.0);   // PARTIAL: residential +0x28 / +0x2c
    G("g_health_strike_chance", 0.0);
    G("g_fire_funding_p", X100(DepartmentFunding(0x28f55a9f)));
    G("g_police_funding_p", X100(DepartmentFunding(0xa2963983)));
    if (fire) {
        G("g_fire_station_count", static_cast<double>(fire->GetFireStationCount()));
        G("ga_fire_coverage", fire->GetAverageCoverage());
        G("g_fire_coverage_p", X100(fire->GetPercentCoverage()));
        if (fire->GetFireStationCount() == 0) LocalNone("l_fire_funding_p", 10000.0);
    }
    if (police && police->GetPoliceStationCount() == 0) LocalNone("l_police_funding_p", 10000.0);
    LocalNone("l_fire_station_no_roads", 0.0);
    LocalNone("l_police_no_roads", 0.0);
}

// ---- Traffic (7.7)
void Traffic() {
    auto* traffic = Comp<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (traffic) {
        const auto& rows = traffic->PublishedStats();
        static const struct { const char* net; int row; } kNets[] = {
            {"road", 0}, {"rail", 1}, {"street", 3}, {"avenue", 6}, {"subway", 7}, {"elevated", 8}, {"monorail", 9}, {"bridge", 13}};
        for (const auto& n : kNets) {
            const auto& row = rows[static_cast<std::size_t>(n.row)];
            char name[64];
            std::snprintf(name, sizeof name, "ga_%s_congestion", n.net); G(name, X100(row.average));
            std::snprintf(name, sizeof name, "g_%s_congestion_tile_count", n.net); G(name, row.congested);
            std::snprintf(name, sizeof name, "l_%s_congestion_h", n.net); LocalAtCell(name, X100(row.max), row.maxX, row.maxZ);
        }
        static const struct { const char* net; int type; } kTiles[] = {
            {"road", 0}, {"rail", 1}, {"highway", 2}, {"street", 3}, {"avenue", 6}, {"subway", 7},
            {"elevated_rail", 8}, {"monorail", 9}, {"dirtroad", 11}, {"groundhighway", 12}};
        for (const auto& t : kTiles) {
            char name[64];
            std::snprintf(name, sizeof name, "g_%s_tile_count", t.net);
            G(name, traffic->CellCount(t.type));
        }
    }
    if (auto* airports = Comp<cSC4AirportDeveloper>(CityComponent::AirportDeveloper)) {
        G("ga_airport_efficiency", X100(airports->GetAvgEfficiency()));
        G("g_airport_count", airports->GetNumPorts(0xe, 0x1f));
    }
    if (auto* seaports = Comp<cSC4SeaportDeveloper>(CityComponent::SeaportDeveloper)) {
        G("ga_seaport_efficiency", X100(seaports->GetAvgEfficiency()));
        G("g_seaport_count", seaports->GetNumPorts(0xe, 0x1f));
    }
}

// ---- Utility (7.8)
void Utility() {
    if (auto* power = Comp<cSC4PowerSimulator>(CityComponent::PowerSimulator)) {
        G("g_power_production_capacity", power->GetMonthlyProduction());
        G("g_power_consumed", power->GetMonthlyConsumption());
        G("g_overworked_power_plant_count", power->GetOverworkedPowerPlantCount());
        G("g_unpowered_building_count", power->GetUnpoweredConsumerCount());
        G("g_power_plant_count", power->GetPowerPlantCount());
        static const struct { const char* name; std::uint32_t type; } kTypes[] = {
            {"g_power_plant_count_coal", 1}, {"g_power_plant_count_gas", 3}, {"g_power_plant_count_oil", 6},
            {"g_power_plant_count_wind", 9}, {"g_power_plant_count_solar", 7}, {"g_power_plant_count_nuclear", 5},
            {"g_power_plant_count_waste", 8}, {"g_power_plant_count_fusion", 2}};
        for (const auto& t : kTypes) G(t.name, power->GetPowerPlantCount(t.type));
        G("g_aged_power_plant_count", power->GetAgedPowerPlantCount());
        G("g_power_pole_count", power->GetPowerPoleCount());
    }
    G("g_water_funding_p", X100(DepartmentFunding(0xaa4014b4)));
    G("g_pollution_funding_p", X100(DepartmentFunding(0x6a42a8f5)));
    if (auto* water = Comp<cSC4PlumbingSimulator>(CityComponent::PlumbingSimulator)) {
        G("g_water_production_capacity", static_cast<std::int32_t>(water->GetWaterProductionCapacity()));
        G("g_water_consumed", static_cast<std::int32_t>(water->GetWaterConsumed()));
        G("g_water_imported", static_cast<double>(water->GetWaterImported()));
        G("g_water_exported", static_cast<double>(water->GetWaterExported()));
        G("g_water_source_count", water->GetNumWaterSources());
        G("g_water_pipe_count", water->GetNumPipes());
        G("g_water_distressed_pipe_count", water->GetNumDistressedPipes());
        G("g_water_burst_pipe_count", water->GetNumBurstPipes());
        G("g_watered_building_count", water->GetNumWateredBuildings());
        G("g_unwatered_building_count", water->GetNumUnwateredBuildings());
        G("g_water_pollution_pump_shutdown_count", water->GetNumPollutedWaterSources());
    }
    if (auto* pollution = Comp<cSC4PollutionSimulator>(CityComponent::PollutionSimulator)) {
        G("g_incinerator_capacity_daily", static_cast<std::uint32_t>(pollution->GetMonthlyIncineratorCapacity() * 30u));
        G("g_waste_to_energy_capacity_daily", static_cast<std::uint32_t>(pollution->GetMonthlyWasteToEnergyCapacity() * 30u));
        G("g_recycling_center_effect_p", X100(pollution->GetRecyclingCenterEffect()));
        auto nonNeg = [](int v) { return v < 0 ? 0 : v; };
        G("ga_air_pollution", nonNeg(pollution->GetAverageAirValue()));
        G("ga_water_pollution", nonNeg(pollution->GetAverageWaterValue()));
        G("ga_garbage_pollution", nonNeg(pollution->GetAverageGarbageValue()));
        G("g_uncollected_garbage", static_cast<int>(pollution->GetTotalGarbageUncollected()));
        G("g_garbage_produced", static_cast<double>(pollution->GetGarbageProducedPrevMonth()));
        G("g_garbage_to_energy", static_cast<double>(pollution->GetGarbageConvertedToEnergyPrevMonth()));
        G("g_garbage_to_landfill", static_cast<double>(pollution->GetGarbageLandfilledPrevMonth()));
        G("g_garbage_imported", static_cast<double>(pollution->GetGarbageImportedPrevMonth()));
        G("g_garbage_exported", static_cast<double>(pollution->GetGarbageExportedPrevMonth()));
        int x = 0, z = 0, v = 0;
        pollution->GetWorstAirTract(x, z, v); LocalAtCell("l_air_pollution_h", nonNeg(v), x, z);
        pollution->GetWorstWaterTract(x, z, v); LocalAtCell("l_water_pollution_h", nonNeg(v), x, z);
        pollution->GetWorstGarbageTract(x, z, v); LocalAtCell("l_uncollected_garbage_h", nonNeg(v), x, z);
    }
    if (auto* landfill = Comp<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper)) {
        std::uint32_t used = 0, capacity = 0;
        landfill->GetLandfillUsage(used, capacity);
        G("g_landfill_capacity", capacity);
        G("g_available_landfill_capacity", capacity - used);
    }
    if (auto* neighbors = Comp<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator)) {
        static const struct { const char* name; std::uint32_t type; } kDeals[] = {
            {"g_nd_count_buy_water", 0x4773ffe2}, {"g_nd_count_sell_water", 0x4773ffe3}, {"g_nd_count_buy_power", 0x4773ffe0},
            {"g_nd_count_sell_power", 0x4773ffe1}, {"g_nd_count_export_garbage", 0x4773ffe4}, {"g_nd_count_import_garbage", 0x4773ffe5}};
        for (const auto& d : kDeals) {
            DealList deals;
            neighbors->FindDeals(d.type, deals);
            G(d.name, static_cast<double>(deals.size()));
        }
    }
}

}  // namespace

void UpdateAdvisorStatistics(std::uint32_t advisorClassId) {
    if (!H()) return;
    switch (advisorClassId) {
        case kGZCLSID_cSC4AdvisorCityPlanning: CityPlanning(); break;
        case kGZCLSID_cSC4AdvisorHQ: HQ(); break;
        case kGZCLSID_cSC4AdvisorSafety: Safety(); break;
        case kGZCLSID_cSC4AdvisorTraffic: Traffic(); break;
        case kGZCLSID_cSC4AdvisorUtility: Utility(); break;
        default: break;
    }
}

}  // namespace native
