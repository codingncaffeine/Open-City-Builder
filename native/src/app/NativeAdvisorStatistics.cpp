// The concrete advisors' statistics pass (IAdvisorConcreteHost::UpdateSimulatorStatistics): the `game` variables each
// advisor publishes from the simulators (OpenSC4 advisors.md 7.3-7.8). The recovered advisors route their whole
// simulator walk through this one seam; each block below is the variable table of one advisor.
//
#include <cstring>
#include "platform/Unwired.h"
#include <cstdint>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <string>
#include <vector>

#include "app/NativeRegion.h"
#include "app/NativeCity.h"
#include "sc4/RegionalCity.h"
#include "sc4/Lot.h"
#include "sc4/ZoneDeveloper.h"
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

namespace sc4 { class cSC4CrimeSimulator; }
namespace native { sc4::cSC4CrimeSimulator* NativeCrimeSimulator(); }   // app/NativeCrime.cpp

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
void G(const char* name, double value) {
    static const char* const trace = std::getenv("SC4_ADVISORSTATDEBUG");   // the named globals as the advisors receive them
    if (trace && std::strstr(name, trace)) std::fprintf(stderr, "advisor stat: %s = %g\n", name, value);
    H()->SetGlobal(name, value);
}
float X100(float v) { return v * 100.0f; }   // "x100": rounded to float32, then widened (advisors.md 7.1)

// A subject at a city cell (one reference, handed to SetLocal and released here).
void LocalAtCell(const char* name, double value, int x, int z) {
    cISC4AdviceSubject* subject = nullptr;
    if (H()->CreateAdviceSubject(&subject) && subject) subject->SetLocation(static_cast<float>(x), static_cast<float>(z));
    H()->SetLocal(name, value, subject);
    if (subject) subject->Release();
}
void LocalNone(const char* name, double value) { H()->SetLocal(name, value, nullptr); }
// A subject naming an object (subject vslot 0x1c SetObject with the object's QueryInterface(1)); none without one.
void LocalAtObject(const char* name, double value, cIGZUnknown* object) {
    cISC4AdviceSubject* subject = nullptr;
    if (object && H()->CreateAdviceSubject(&subject) && subject) subject->SetObject(object);
    H()->SetLocal(name, value, subject);
    if (subject) subject->Release();
}

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
    // cSC4AdvisorHQ::update_occupant_variables (i386 0x00332f24): each list walked back to front; the lowest staff funding
    // fraction (residential vslot 0x50, the list's purpose) x 100 and the lowest grade x 100 (query data, vslot 0x6c / 0x68),
    // strict <, from FLT_MAX, each with the building as its subject.
    auto lowest = [&](bool school, std::initializer_list<std::uint32_t> groups, const char* fundingName, const char* gradeName) {
        std::vector<std::uint32_t> g(groups);
        cSC4ResidentialSimulator::OccupantList list;
        if (school) res->GetSchoolBuildings(list, &g); else res->GetHospitalBuildings(list, &g);
        float minFunding = FLT_MAX, minGrade = FLT_MAX;
        IResidentialOccupant* fundingAt = nullptr;
        IResidentialOccupant* gradeAt = nullptr;
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            IResidentialOccupant* o = *it;
            if (!o) continue;
            float staff = 0.0f, vehicle = 0.0f;
            res->GetOccupantFundingPercentages(o, staff, vehicle, school);
            if (staff < minFunding) { minFunding = staff; fundingAt = o; }
            ResidentialQueryData q;
            if (school) res->GetSchoolQueryData(o, q); else res->GetHospitalQueryData(o, q);
            const float grade = q.grade * 100.0f;
            if (grade < minGrade) { minGrade = grade; gradeAt = o; }
        }
        LocalAtObject(fundingName, minFunding * 100.0f, fundingAt ? fundingAt->Object() : nullptr);
        LocalAtObject(gradeName, minGrade, gradeAt ? gradeAt->Object() : nullptr);
        for (auto* o : list) if (o) o->Release();
    };
    lowest(false, {0x1513}, "l_hospital_funding_l", "l_hospital_grade_l");
    lowest(false, {0x1512}, "l_clinic_funding_l", "l_clinic_grade_l");
    lowest(true, {0x150f, 0x1510, 0x1514, 0x1504}, "l_school_funding_l", "l_school_grade_l");
    lowest(true, {0x1506}, "l_museum_funding_l", "l_museum_grade_l");
    lowest(true, {0x1505}, "l_library_funding_l", "l_library_grade_l");
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
    cSC4CrimeSimulator* crime = NativeCrimeSimulator();   // a plain object: a cast from the serializable view never finds it
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
    if (auto* res = Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator)) {
        G("g_school_strike_chance", X100(res->ChanceOfSchoolStrike()));   // residential vslot 0x28
        G("g_health_strike_chance", X100(res->ChanceOfHealthStrike()));   // vslot 0x2c
    } else {
        G("g_school_strike_chance", 0.0);
        G("g_health_strike_chance", 0.0);
    }
    G("g_fire_funding_p", X100(DepartmentFunding(0x28f55a9f)));
    G("g_police_funding_p", X100(DepartmentFunding(0xa2963983)));
    // cSC4AdvisorSafety::update_occupant_variables (i386): the stations front to back; the lowest funding fraction (strict <,
    // from 100) x 100 with that station as subject, and the first station whose lot exists without road access (lot vslot
    // 0x70 HasRoadAccess) as l_X_no_roads = 1.
    auto* lots = Comp<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper);
    auto noRoads = [&](cIGZUnknown* object) {
        void* raw = nullptr;
        if (!lots || !object || !object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return false;
        auto* occupant = static_cast<cISC4Occupant*>(raw);
        cSC4Lot* lot = lots->GetOccupantLot(occupant);   // spLotManager vslot 0x38
        occupant->Release();
        return lot && !lot->HasRoadAccess();
    };
    if (fire) {
        G("g_fire_station_count", static_cast<double>(fire->GetFireStationCount()));
        G("ga_fire_coverage", fire->GetAverageCoverage());
        G("g_fire_coverage_p", X100(fire->GetPercentCoverage()));
        std::list<IFireOccupant*> stations;
        fire->GetFireStations(stations);   // vslot 0x58
        float lowest = 100.0f;
        cIGZUnknown* lowestAt = nullptr;
        cIGZUnknown* noRoadsAt = nullptr;
        for (IFireOccupant* station : stations) {
            if (!station) continue;
            int cost = 0;
            SC4Percentage fraction{};
            fire->GetStationFunding(station, &cost, &fraction);   // vslot 0x20
            if (fraction.value < lowest) { lowest = fraction.value; lowestAt = station->Object(); }
            if (!noRoadsAt && noRoads(station->Object())) noRoadsAt = station->Object();
        }
        LocalAtObject("l_fire_funding_p", X100(lowest), lowestAt);
        LocalAtObject("l_fire_station_no_roads", noRoadsAt ? 1.0 : 0.0, noRoadsAt);
    }
    if (police) {
        std::list<IProtectionStation*> stations;
        police->GetPoliceStations(stations);   // vslot 0x54
        float lowest = 100.0f;
        cIGZUnknown* lowestAt = nullptr;
        cIGZUnknown* noRoadsAt = nullptr;
        for (IProtectionStation* station : stations) {
            auto* officer = dynamic_cast<IPoliceOccupant*>(station);
            if (!officer) continue;
            int cost = 0;
            SC4Percentage fraction{};
            police->GetStationFunding(officer, &cost, &fraction);   // vslot 0x1c
            if (fraction.value < lowest) { lowest = fraction.value; lowestAt = station->Object(); }
            if (!noRoadsAt && noRoads(station->Object())) noRoadsAt = station->Object();
        }
        LocalAtObject("l_police_funding_p", X100(lowest), lowestAt);
        LocalAtObject("l_police_no_roads", noRoadsAt ? 1.0 : 0.0, noRoadsAt);
    }
}

// ---- Traffic (7.7)
void Traffic() {
    auto* traffic = Comp<cSC4TrafficSimulator>(CityComponent::TrafficSimulator);
    if (traffic) {
        // cSC4AdvisorTraffic::update_variables (i386 0x00335d98): the statistics block's funding, strike and freight fields.
        G("g_road_funding_p", X100(traffic->StatsRoadFunding()));
        G("g_masstransit_funding_p", X100(traffic->StatsMassTransitFunding()));
        G("g_transit_strike", traffic->StrikeActiveFlag() ? 1.0 : 0.0);
        G("ga_freight_trip_length", static_cast<double>(traffic->StatsFreightTripLength()));
        // The statistics' station block (i386 stats +0x138: bus, train, subway, elevated, monorail, freight depots, rapid-transit
        // transitions; count u16, average, cell, worst) is copied each cycle from a working block that the original resets
        // (WorkOnSimCycle) and never fills, so every station value it publishes is 0, at cell (0, 0). The pothole count
        // just before it (+0x134) is ApplyRoadFunding's.
        static const char* const kStations[] = {"bus", "train", "subway", "elevated", "monorail"};
        for (const char* kind : kStations) {
            char name[64];
            std::snprintf(name, sizeof name, "g_%s_station_count", kind); G(name, 0.0);
            std::snprintf(name, sizeof name, "ga_%s_station_utilization_p", kind); G(name, 0.0);
            std::snprintf(name, sizeof name, "l_%s_station_utilization_pl", kind); LocalAtCell(name, 0.0, 0, 0);
        }
        G("g_train_freight_depot_count", 0.0);
        G("g_num_rapidtransit_transitions", 0.0);
        G("g_pothole_count", static_cast<double>(static_cast<std::uint16_t>(traffic->StatRoadPotholes())));
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
    // The first port lot (from the back) without road access, as a local at the lot's cell; else 0 with no subject.
    auto noRoadAccess = [](const char* name, std::vector<cSC4Lot*>& lots) {
        for (auto it = lots.rbegin(); it != lots.rend(); ++it) {
            cSC4Lot* lot = *it;
            if (lot && !lot->HasRoadAccess()) {   // lot vslot 0x70
                int x = 0, z = 0;
                lot->GetLocation(x, z);            // vslot 0x54
                LocalAtCell(name, 1.0, x, z);
                return;
            }
        }
        LocalNone(name, 0.0);
    };
    if (auto* airports = Comp<cSC4AirportDeveloper>(CityComponent::AirportDeveloper)) {
        G("ga_airport_efficiency", X100(airports->GetAvgEfficiency()));
        std::vector<cSC4Lot*> lots;
        airports->GetAllPortLots(lots);   // vslot 0x20
        noRoadAccess("l_airport_no_road_access", lots);
        // update_occupant_variables: GetNumPorts(size, stage) with sizes 2 small, 4 medium, 8 large (0xe all) and stages
        // 1 / 2 / 4 (0x1f all).
        G("g_airport_count", airports->GetNumPorts(0xe, 0x1f));
        G("g_medium_airport_count", airports->GetNumPorts(4, 0x1f));
        G("g_small_airport_count", airports->GetNumPorts(2, 0x1f));
        G("g_large_airport_count", airports->GetNumPorts(8, 0x1f));
        static const struct { const char* name; int size, stage; } kStages[] = {
            {"g_medium_airport_s1_count", 4, 1}, {"g_small_airport_s1_count", 2, 1}, {"g_large_airport_s1_count", 8, 1},
            {"g_medium_airport_s2_count", 4, 2}, {"g_small_airport_s2_count", 2, 2}, {"g_large_airport_s2_count", 8, 2},
            {"g_medium_airport_s3_count", 4, 4}, {"g_small_airport_s3_count", 2, 4}, {"g_large_airport_s3_count", 8, 4}};
        for (const auto& s : kStages) G(s.name, airports->GetNumPorts(s.size, s.stage));
    }
    if (auto* seaports = Comp<cSC4SeaportDeveloper>(CityComponent::SeaportDeveloper)) {
        G("ga_seaport_efficiency", X100(seaports->GetAvgEfficiency()));
        std::vector<cSC4Lot*> lots;
        seaports->GetAllPortLots(lots);
        noRoadAccess("l_seaport_no_road_access", lots);
        G("g_seaport_count", seaports->GetNumPorts(0xe, 0x1f));
        G("g_seaport_s1_count", seaports->GetNumPorts(0xe, 1));
        G("g_seaport_s2_count", seaports->GetNumPorts(0xe, 2));
        G("g_seaport_s3_count", seaports->GetNumPorts(0xe, 4));
        G("g_seaport_volume", static_cast<double>(static_cast<std::uint32_t>(seaports->GetNumSeaportTrips())));   // vslot 0x34
    }
    // The highest congestion (strict >, from 0) at the inner cell of each road, then each avenue, neighbor connection; the
    // avenue pass keeps the road pass's cell when no avenue cell is congested (as the original).
    auto* neighbors = Comp<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator);
    cSC4SimGrid<std::uint8_t>* congestion = traffic ? traffic->GetCongestionMap() : nullptr;
    if (neighbors) {
        int atX = 0, atZ = 0;
        auto worst = [&](std::uint32_t type, const char* name) {
            ConnectionList list;
            neighbors->GetConnectionsOfType(type, list);   // vslot 0x44
            std::uint8_t highest = 0;
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                int x = 0, z = 0;
                if (!*it || !(*it)->GetConnectionInnerCell(x, z) || !congestion) continue;
                const std::uint8_t value = congestion->GetCellValue(x, z);
                if (highest < value) { highest = value; atX = x; atZ = z; }
            }
            LocalAtCell(name, static_cast<double>(highest), atX, atZ);
        };
        worst(2, "l_road_connection_congestion_h");
        worst(8, "l_avenue_connection_congestion_h");
        ConnectionList rail;
        neighbors->GetConnectionsOfType(1, rail);
        G("g_rail_neighbor_connection_count", static_cast<double>(rail.size()));
    }
}

// The least funded and the most worn plant (FindPowerPlants / FindWaterBuildings: funding from 100%, wear from 0), as locals
// with the building as subject; the references the search hands out are dropped here.
void PlantExtremes(const char* fundingName, const char* ageName, cISC4Occupant* leastFunded, float funding, cISC4Occupant* mostWorn,
                   int wear) {
    LocalAtObject(fundingName, X100(funding), leastFunded);
    LocalAtObject(ageName, static_cast<double>(wear), mostWorn);
    if (leastFunded) leastFunded->Release();
    if (mostWorn) mostWorn->Release();
}

// ---- Utility (7.8)
void Utility() {
    // cSC4AdvisorUtility::update_variables (i386 0x0032f11a): the regional city's commodity imports / exports (vslots 0xe0 /
    // 0xd8; 1 power, 2 water), the power department's funding and the power strike (never: IsOnStrike / GetChanceOfStrike
    // answer false / 0).
    auto* region = dynamic_cast<cSC4RegionalCity*>(NativeCurrentRegionalCity());
    G("g_power_imported", region ? region->GetImported(1) : 0.0);
    G("g_power_exported", region ? region->GetExported(1) : 0.0);
    if (auto* power = Comp<cSC4PowerSimulator>(CityComponent::PowerSimulator)) {
        G("g_power_funding_p", X100(power->GetFundingPercent()));
        G("g_power_strike", cSC4PowerSimulator::IsOnStrike() ? 1.0 : 0.0);
        G("g_power_strike_chance", X100(cSC4PowerSimulator::GetChanceOfStrike()));
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
        cISC4Occupant* leastFunded = nullptr;
        cISC4Occupant* mostWorn = nullptr;
        SC4Percentage funding(1.0f);
        int wear = 0;
        power->FindPowerPlants(&leastFunded, &funding, &mostWorn, &wear);   // vslot 0x78
        PlantExtremes("l_power_plant_funding_pl", "l_power_plant_age_h", leastFunded, funding.value, mostWorn, wear);
    }
    G("g_water_funding_p", X100(DepartmentFunding(0xaa4014b4)));
    G("g_pollution_funding_p", X100(DepartmentFunding(0x6a42a8f5)));
    if (auto* water = Comp<cSC4PlumbingSimulator>(CityComponent::PlumbingSimulator)) {
        G("g_water_production_capacity", static_cast<std::int32_t>(water->GetWaterProductionCapacity()));
        G("g_water_consumed", static_cast<std::int32_t>(water->GetWaterConsumed()));
        G("g_water_imported", region ? region->GetImported(2) : 0.0);
        G("g_water_exported", region ? region->GetExported(2) : 0.0);
        G("g_water_source_count", water->GetNumWaterSources());
        G("g_water_pipe_count", water->GetNumPipes());
        G("g_water_distressed_pipe_count", water->GetNumDistressedPipes());
        G("g_water_burst_pipe_count", water->GetNumBurstPipes());
        G("g_watered_building_count", water->GetNumWateredBuildings());
        G("g_unwatered_building_count", water->GetNumUnwateredBuildings());
        G("g_water_pollution_pump_shutdown_count", water->GetNumPollutedWaterSources());
        cISC4Occupant* leastFunded = nullptr;
        cISC4Occupant* mostWorn = nullptr;
        SC4Percentage funding(1.0f);
        int wear = 0;
        water->FindWaterBuildings(&leastFunded, &funding, &mostWorn, &wear);   // vslot 0x6c
        PlantExtremes("l_water_building_funding_pl", "l_water_building_age_h", leastFunded, funding.value, mostWorn, wear);
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
        G("g_garbage_recycled", static_cast<double>(pollution->GetGarbageRecycledPrevMonth()));   // vslot 0x98
        G("g_incinerator_count", pollution->GetIncineratorCount());                              // 0xa8
        G("g_waste_to_energy_building_count", pollution->GetWasteToEnergyBuildingCount());       // 0xac
        G("g_water_treatment_plant_count", pollution->GetWaterTreatmentPlantCount());            // 0xb4
        G("g_recycling_center_count", pollution->GetRecyclingCenterCount());                     // 0xb0
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
        // update_neighbor_deal_variables (i386 0x0032d9f2): which deals have a connection (the connections back to front until
        // all three are seen: type 5 water, 4 power, 1 / 2 garbage by rail or road) and which the city could propose
        // (ProposeDeals(list, false), the proposals back to front: buy / export, sell / import).
        ConnectionList connections;
        neighbors->GetAllConnections(connections);
        unsigned present = 0;
        for (auto it = connections.rbegin(); it != connections.rend() && present != 7; ++it) {
            if (!*it) continue;
            const std::uint32_t type = (*it)->GetType();
            if (type == 4) present |= 2;
            else if (type == 5) present |= 1;
            else if (type - 1 < 2) present |= 4;
        }
        G("g_nd_connection_present_water", (present & 1) ? 1.0 : 0.0);
        G("g_nd_connection_present_power", (present & 2) ? 1.0 : 0.0);
        G("g_nd_connection_present_garbage", (present & 4) ? 1.0 : 0.0);
        DealList proposals;
        neighbors->ProposeDeals(proposals, false);
        unsigned buy = 0, sell = 0;
        for (auto it = proposals.rbegin(); it != proposals.rend() && buy != 7 && sell != 7; ++it) {
            if (!*it) continue;
            switch ((*it)->GetType()) {
                case 0x4773ffe0: buy |= 2; break;
                case 0x4773ffe1: sell |= 2; break;
                case 0x4773ffe2: buy |= 1; break;
                case 0x4773ffe3: sell |= 1; break;
                case 0x4773ffe4: buy |= 4; break;
                case 0x4773ffe5: sell |= 4; break;
                default: break;
            }
        }
        G("g_nd_can_buy_water", (buy & 1) ? 1.0 : 0.0);
        G("g_nd_can_buy_power", (buy & 2) ? 1.0 : 0.0);
        G("g_nd_can_export_garbage", (buy & 4) ? 1.0 : 0.0);
        G("g_nd_can_sell_water", (sell & 1) ? 1.0 : 0.0);
        G("g_nd_can_sell_power", (sell & 2) ? 1.0 : 0.0);
        G("g_nd_can_import_garbage", (sell & 4) ? 1.0 : 0.0);
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
