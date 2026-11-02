// cSC4GrowthDeveloper — see GrowthDeveloper.h for the layout, the three COM views, the map-update state
// machine and the list of quirks.
//
// Every call outside the object goes through IGrowthDeveloperHost (one method per original call, with its
// slot). The grids (cSC4SimGrid<T>), the response curves, the property bag and the lot are used directly:
// all of them are recovered. The comments name the original vtable slot of every external call.

#include "sc4/GrowthDeveloper.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

#include "rz/cRZRandom.h"
#include "sc4/ProtectionSimulator.h"   // AddRadiusEffect
#include "sc4/SimGridOps.h"            // ProcessTwoGridsWithCallback / AddCurveToShort

namespace sc4 {

namespace {

IGrowthDeveloperHost* g_host = nullptr;

// Property ids of the growth-developer ini exemplar (0xe7e2c2db 6534284a / 0xe8da7677).
constexpr std::uint32_t kPropertySubType = 0x21;              // -> +0x170 (the developer type)
constexpr std::uint32_t kPropertyType = 0x87cd6300;           // -> +0x174 (1/2/3)
constexpr std::uint32_t kPropertyDemandType = 0x87cd6301;     // the source demand channel type
constexpr std::uint32_t kPropertyTractSize = 0x87cd6310;      // -> +0x74
constexpr std::uint32_t kPropertyWealth = 0x87cd6330;         // -> +0x6c
constexpr std::uint32_t kPropertyPurpose = 0x87cd6332;        // -> +0x68
constexpr std::uint32_t kPropertyProfileFlags = 0x87cd6333;   // -> +0x75/+0x76
constexpr std::uint32_t kPropertyUseTotalLandValue = 0x87cd633f;   // -> +0x77
constexpr std::uint32_t kPropertyLandValueCurve = 0x87cd6340;      // -> +0x78
constexpr std::uint32_t kPropertySlopeCurve = 0x87cd6398;          // -> +0x80
constexpr std::uint32_t kPropertyResidentialCurve1 = 0x87cd6343;   // -> +0xa8
constexpr std::uint32_t kPropertyResidentialCurve2 = 0x87cd6344;   // -> +0xb0
constexpr std::uint32_t kPropertyResidentialCurve3 = 0x87cd6345;   // -> +0xb8
constexpr std::uint32_t kPropertyCommercialCurve3320 = 0x87cd6391;  // -> +0xc0
constexpr std::uint32_t kPropertyCommercialCurve3330 = 0x87cd6392;  // -> +0xc8
constexpr std::uint32_t kPropertyAuraCurve70 = 0x87cd6341;          // -> +0xa0
constexpr std::uint32_t kPropertyPollutionCurveFirst = 0x87cd634b;  // -> +0x88[0..3]
constexpr std::uint32_t kPropertyResidentialMap138Curve = 0x87cd6348;   // -> +0xd0
constexpr std::uint32_t kPropertyResidentialMap140Curve = 0x87cd6349;   // -> +0xd8
constexpr std::uint32_t kPropertyCrimeCurve = 0x87cd634a;               // -> +0xe0
constexpr std::uint32_t kPropertyTrafficCurve = 0x87cd6347;             // -> +0xe8
constexpr std::uint32_t kPropertyTrafficMap48Curve = 0x87cd6346;        // -> +0xf0
constexpr std::uint32_t kPropertyAuraCurve78 = 0x87cd6399;              // -> +0xf8
constexpr std::uint32_t kPropertyAuraCurve68 = 0x87cd6397;              // -> +0x100
constexpr std::uint32_t kPropertyDesirabilityMapValue = 0x87cd6357;     // -> +0x108
constexpr std::uint32_t kPropertyDesirabilityThreshold = 0x87cd6355;    // -> +0x10c
constexpr std::uint32_t kPropertyDeclineThreshold = 0x87cd6354;         // -> +0x110
constexpr std::uint32_t kPropertyAddPopulationToDemand = 0x87cd6356;    // -> +0x114
constexpr std::uint32_t kPropertyStyleCurveCount = 0x87cd6360;          // -> +0x118 size
constexpr std::uint32_t kPropertyStyleCurveFirst = 0x87cd6361;          // -> +0x118[i]
constexpr std::uint32_t kPropertyParkCap = 0x87cd6372;                  // -> +0x133
constexpr std::uint32_t kPropertyWaterCap = 0x87cd6370;                 // -> +0x131
constexpr std::uint32_t kPropertyFireCap = 0x87cd6371;                  // -> +0x132
constexpr std::uint32_t kPropertyAbandonmentThresholds = 0x87cd6380;    // -> +0x138
constexpr std::uint32_t kPropertyMonopolyUse = 0x87cd6381;              // -> +0x150
constexpr std::uint32_t kPropertyStylePercentages = 0x87cd63a0;         // -> +0x158
constexpr std::uint32_t kPropertyPowerCap = 0x87cd6373;                 // -> +0x130
constexpr std::uint32_t kPropertySubjectiveThresholdLow = 0xcab1ce3a;   // -> +0x218
constexpr std::uint32_t kPropertySubjectiveThresholdHigh = 0xcab1ce3c;  // -> +0x230

// Messages (the simulator intervals the class registers for in Init). The lot state message id is the one
// Lot.h already declares (0xe8bda2e7).
constexpr std::uint32_t kMessageMaxCapacityRect = 0x07699455;
constexpr std::uint32_t kMessageDayTick = 0x66956814;      // simulator interval 4 (day)
constexpr std::uint32_t kMessageIdleTick = 0x6695681a;     // simulator interval 10 (idle time)
constexpr int kSimulatorDayInterval = 4;
constexpr int kSimulatorIdleInterval = 10;
constexpr std::uint32_t kSimulatorAgentFlags = 2;
constexpr const char* kSimulatorAgentName = "cSC4GrowthDeveloper";
constexpr std::uint32_t kDayTickBudgetMilliseconds = 5;

// The landmark/aura properties of a special building occupant (public building exemplars).
constexpr std::uint32_t kPropertyLandmarkTypes = 0x4a5f7c4b;    // one uint32 per entry
constexpr std::uint32_t kPropertyLandmarkEffect = 0x4a5f7315;   // three floats per entry

// cISC4BuildingDevelopmentSimulator::DeveloperType values the demand branch and the style shuffle test.
constexpr std::uint32_t kDeveloperTypeCommercialOfficeMedium = 0x3320;
constexpr std::uint32_t kDeveloperTypeCommercialOfficeHigh = 0x3330;
constexpr std::uint32_t kDemandTypeIndustrial = 0x4001;
constexpr std::uint32_t kDemandTypeRci = 0x3001;
constexpr std::uint32_t kDemandTypeNeighborShare = 0x3301;
constexpr std::uint32_t kDemandTypeResidential = 0x1001;
constexpr std::uint32_t kDemandFlagsSource = 0x20000;
constexpr std::uint32_t kDemandFlagsNeighbor = 0x10000;

// The float -> int16 conversion the original does by adding 12582912.0f (0x4b400000) and keeping the low
// 16 bits of the float. The values here are always in [0, 65535], where a plain truncation is identical.
inline std::uint16_t TruncateToUint16(float value) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(value));
}

// The payload accessors of a message (the original calls them through the message's own vtable).
cIGZMessage2Standard& StandardMessage(cIGZMessage2* message) {
    return *static_cast<cIGZMessage2Standard*>(message);
}

// The desirability map the growth-list comparator reads (gpDesirabilityMap @ 0x100b7...).
const std::vector<std::vector<std::int16_t>>* g_desirabilityMap = nullptr;
// The seed mask of the randomize comparator (g_CompareRandomize_seedMask).
std::uint32_t g_compareSeedMask = 0;

// The growth list is sorted by desirability, highest first (the original compares b < a).
bool CompareByDesirability(const GrowthItem& a, const GrowthItem& b) {
    const auto& map = *g_desirabilityMap;
    return map[static_cast<std::size_t>(b.x)][static_cast<std::size_t>(b.z)] <
           map[static_cast<std::size_t>(a.x)][static_cast<std::size_t>(a.z)];
}

// The decline list of an industrial developer is shuffled: a per-item seed of z * 0x100000 + x * 0x10,
// XORed with a mask drawn once from the global random, and the two fields compared by their first draw.
bool CompareByRandom(const GrowthItem& a, const GrowthItem& b) {
    cRZRandom randomA(static_cast<std::uint32_t>(a.z * 0x100000 + a.x * 0x10) ^ g_compareSeedMask);
    const std::uint32_t valueA = randomA.RandomUint32Uniform();
    cRZRandom randomB(static_cast<std::uint32_t>(b.z * 0x100000 + b.x * 0x10) ^ g_compareSeedMask);
    const std::uint32_t valueB = randomB.RandomUint32Uniform();
    return valueB < valueA;
}

// The landmark pass body (DoIdle stage 0xf): the occupant's 0x4a5f7c4b array lists the developer sub types
// it boosts; the first entry that matches this developer's sub type contributes the three floats of the
// parallel 0x4a5f7315 array (radius in city cells / tract size, peak, edge) as a radius effect on the
// desirability map. Returns whether an effect was applied.
bool ApplyLandmarkEffect(IGrowthDeveloperHost* host, std::int32_t subType, cSC4SimGrid<std::int16_t>& grid,
                         cISC4Occupant* occupant) {
    if (!host || !occupant) return false;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();          // occupant vslot 0x30
    if (!holder) return false;
    cISCProperty* types = holder->GetProperty(kPropertyLandmarkTypes);  // holder vslot 0x28
    if (!types) return false;
    const cIGZVariant* typesValue = types->GetPropertyValue();          // property vslot 0x30
    const std::uint32_t* typeArray = typesValue->RefUint32();           // variant slot 0x358
    const std::uint32_t typeCount = typesValue->GetCount();             // variant slot 0x20
    for (std::uint32_t i = 0; i < typeCount; ++i) {
        if (static_cast<std::int32_t>(typeArray[i]) != subType) continue;
        cISCProperty* effect = holder->GetProperty(kPropertyLandmarkEffect);
        if (!effect) return false;
        const cIGZVariant* effectValue = effect->GetPropertyValue();
        const float* values = effectValue->RefFloat32();                // variant slot 0x3a8
        const float radius = values[i * 3];
        const float peak = values[i * 3 + 1];
        const float edge = values[i * 3 + 2];
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);                                // occupant vslot 0x40
        int cellX = 0, cellZ = 0;
        if (IGrowthCityApi* city = host->City()) city->PositionToCell(position[0], position[2], cellX, cellZ);
        const int shift = grid.GetTractShift();                         // grid slot 0x58
        cellX >>= shift;
        cellZ >>= shift;
        AddRadiusEffect(grid.GetGridData(), grid.GetTractCountX(), grid.GetTractCountZ(), cellX, cellZ,
                        radius / static_cast<float>(grid.GetTractSize()), peak, edge);
        return true;
    }
    return false;
}

}  // namespace

IGrowthDeveloperHost* GrowthDeveloperHost() { return g_host; }
void SetGrowthDeveloperHost(IGrowthDeveloperHost* host) { g_host = host; }

// The two comparators, exported (see GrowthDeveloper.h).
bool GrowthDeveloperCompareDesirability(const GrowthItem& a, const GrowthItem& b) { return CompareByDesirability(a, b); }
bool GrowthDeveloperCompareRandomize(const GrowthItem& a, const GrowthItem& b) { return CompareByRandom(a, b); }

//==================================================================================================
// construction / COM
//==================================================================================================

cSC4GrowthDeveloper::cSC4GrowthDeveloper() {
    // +0x18 initialised byte, +0x1c reference count and every service pointer at +0x20..+0x73 are zero;
    // +0x74 is the tract size and +0x108..+0x110 three thresholds (all 0x50). The two style flags at
    // +0x131/+0x132 default to 3, +0x133 to 6 and +0x130 (power) to 0; +0x150 is 1.
    pollutionCurves_.resize(4, nullptr);
    specialCursor_ = specialBuildings_.begin();
}

cSC4GrowthDeveloper::~cSC4GrowthDeveloper() {
    // The original frees the vector storages, clears the occupant list, destroys both maps and the style
    // curve vector, and deletes the four pollution curve pointer slots - all of which the members do here.
    // It does *not* free the response curves themselves (only Shutdown does) nor the map-update cursor, so
    // an object destroyed without Init/Shutdown leaks them; kept (0x10015638c).
}

bool cSC4GrowthDeveloper::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (static_cast<std::int32_t>(iid) < 1) {
        if (iid == kGZIID_cISC4GrowthDeveloperSerializable) {
            *out = static_cast<cIGZSerializable*>(this);
        } else if (iid == kGZIID_cISC4GrowthDeveloper) {
            *out = static_cast<cISC4GrowthDeveloper*>(this);
        } else {
            return false;
        }
    } else if (iid == 1) {
        *out = static_cast<cISC4GrowthDeveloper*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4GrowthDeveloper::AddRef() { return ++refCount_; }

std::uint32_t cSC4GrowthDeveloper::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

//==================================================================================================
// maps
//==================================================================================================

cSC4SimGrid<std::uint8_t>* cSC4GrowthDeveloper::MakeUint8Map(std::uint32_t instanceId) {
    auto* map = new cSC4SimGrid<std::uint8_t>();
    map->Init();                                    // grid slot 0x18 (the byte at +0x10 of the original)
    map->AddRef();                                  // reference count 1
    map->SetTractSize(tractSize_);                   // grid slot 0x48
    map->SetInstanceID(instanceId);                  // grid slot 0x30
    return map;
}

cSC4SimGrid<std::int16_t>* cSC4GrowthDeveloper::MakeInt16Map(std::uint32_t instanceId) {
    auto* map = new cSC4SimGrid<std::int16_t>();
    map->Init();
    map->AddRef();
    map->SetTractSize(tractSize_);
    map->SetInstanceID(instanceId);
    return map;
}

cSC4SimGrid<std::uint16_t>* cSC4GrowthDeveloper::MakeUint16Map(std::uint32_t instanceId) {
    auto* map = new cSC4SimGrid<std::uint16_t>();
    map->Init();
    map->AddRef();
    map->SetTractSize(tractSize_);
    map->SetInstanceID(instanceId);
    return map;
}

// @ 0x100157dc0. The six maps are re-laid and the map-update cursor gets the new tract bounds; the state,
// x/z and step of the cursor are left alone (the constructor's start = INT_MIN / max = INT_MAX sentinels are
// only replaced here, so a tract size that never changes leaves the driver loop unbounded in the original).
void cSC4GrowthDeveloper::SetTractSize(std::uint8_t size) {
    if (tractSize_ == size) return;
    tractSize_ = size;
    if (zoneCellCountMap_) zoneCellCountMap_->SetTractSize(size);
    if (desirabilityWorkMap_) desirabilityWorkMap_->SetTractSize(size);
    if (desirabilityMap_) desirabilityMap_->SetTractSize(size);
    if (populationMap_) populationMap_->SetTractSize(size);
    if (capacityMap_) capacityMap_->SetTractSize(size);
    if (maxCapacityMap_) maxCapacityMap_->SetTractSize(size);
    if (cursor_ && desirabilityWorkMap_) {
        const int countX = desirabilityWorkMap_->GetTractCountX();
        const int countZ = desirabilityWorkMap_->GetTractCountZ();
        cursor_->startX = 0;            // the original writes the two start fields at once
        cursor_->startZ = 0;
        cursor_->maxX = countX - 1;
        cursor_->maxZ = countZ - 1;
        if (countX < 1) cursor_->maxX = 0;
        if (countZ < 1) cursor_->maxZ = 0;
    }
}

//==================================================================================================
// Init / Shutdown
//==================================================================================================

// @ 0x100156620. One reference on each service; the grids are created here and the tunables are read from
// the ini exemplar last (the exemplar may change the tract size, which re-lays the maps).
bool cSC4GrowthDeveloper::Init(cISCPropertyHolder* exemplar) {
    if (initialized_) return true;
    if (!g_host || !exemplar) return false;      // the original dereferences the exemplar unchecked

    if (!city_) {
        city_ = g_host->City();
        if (!city_) return false;
        city_->AddRef();
    }
    if (!buildingDevelopment_) {
        buildingDevelopment_ = g_host->BuildingDevelopment();
        if (!buildingDevelopment_) return false;
        buildingDevelopment_->AddRef();
    }
    if (!occupantManager_) {
        occupantManager_ = city_->OccupantManager();     // city slot 0x110
        if (!occupantManager_) return false;
        occupantManager_->AddRef();
    }
    if (!zoneManager_) {
        zoneManager_ = g_host->ZoneManager();
        if (!zoneManager_) return false;
        zoneManager_->AddRef();
    }
    if (!landValue_) {
        landValue_ = g_host->LandValue();
        if (!landValue_) return false;
        landValue_->AddRef();
    }
    if (!traffic_) {
        traffic_ = g_host->Traffic();
        if (!traffic_) return false;
        traffic_->AddRef();
    }
    exemplar_ = exemplar;
    exemplar_->AddRef();

    std::uint32_t subType = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertySubType, subType))
        subType_ = static_cast<std::int32_t>(subType);
    const std::uint32_t base = static_cast<std::uint32_t>(subType_);
    if (!zoneCellCountMap_) zoneCellCountMap_ = MakeUint8Map(base + 0x1000000);
    if (!desirabilityWorkMap_) desirabilityWorkMap_ = MakeInt16Map(base + 0x2100000);
    if (!desirabilityMap_) desirabilityMap_ = MakeInt16Map(base + 0x2000000);
    if (!populationMap_) populationMap_ = MakeInt16Map(base + 0x3000000);
    if (!capacityMap_) capacityMap_ = MakeUint16Map(base + 0x5000000);
    if (!maxCapacityMap_) maxCapacityMap_ = MakeUint16Map(base + 0x6000000);
    mapUpdateStage_ = 0;
    if (!cursor_) {
        // start = INT_MIN, max = INT_MAX until SetTractSize bounds them (see the quirk above).
        cursor_ = new GrowthMapUpdateCursor();
        cursor_->state = 0;
        cursor_->x = 0;
        cursor_->z = 0;
        cursor_->step = 1;
        cursor_->startX = static_cast<std::int32_t>(0x80000000u);
        cursor_->startZ = static_cast<std::int32_t>(0x80000000u);
        cursor_->maxX = 0x7fffffff;
        cursor_->maxZ = 0x7fffffff;
    }
    ReadSimulatorTunableValues();

    if (!messageServer_) {
        messageServer_ = g_host->MessageServer();
        if (!messageServer_) return false;
        messageServer_->AddRef();
        messageServer_->AddNotification(this, kMessageLotStateChanged);   // message server slot 0x28
        messageServer_->AddNotification(this, kMessageMaxCapacityRect);
    }
    if (!g_host->SimulatorPresent()) return false;
    // simulator slot 0x98: the agent name is a cRZString in the original.
    g_host->AddSimulatorAgent(this, kSimulatorDayInterval, kSimulatorAgentName, kSimulatorAgentFlags);
    g_host->AddSimulatorAgent(this, kSimulatorIdleInterval, kSimulatorAgentName, kSimulatorAgentFlags);
    initialized_ = true;
    return true;
}

// @ 0x1001578b6. Releases everything Init acquired (in the original's order) and frees the response curves.
bool cSC4GrowthDeveloper::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (g_host) {
        if (IGrowthCityApi* city = g_host->City()) {
            // city slot 0x190 -> the developer registry's slot 0xa8: forget this developer's message view.
            city->RemoveDeveloper(static_cast<cIGZMessageTarget2*>(this));
        }
    }
    if (messageServer_) {
        messageServer_->RemoveNotification(this, kMessageLotStateChanged);   // slot 0x30
        messageServer_->RemoveNotification(this, kMessageMaxCapacityRect);
        messageServer_->Release();
        messageServer_ = nullptr;
    }
    delete cursor_;
    cursor_ = nullptr;
    auto releaseMap = [](auto*& map) {
        if (!map) return;
        map->Shutdown();      // grid slot 0x20
        map->Release();       // grid slot 0x10
        map = nullptr;
    };
    releaseMap(zoneCellCountMap_);
    releaseMap(desirabilityWorkMap_);
    releaseMap(desirabilityMap_);
    releaseMap(populationMap_);
    releaseMap(capacityMap_);
    releaseMap(maxCapacityMap_);
    if (exemplar_) {
        exemplar_->Release();
        exemplar_ = nullptr;
    }
    if (sourceDemand_) {
        sourceDemand_->Release();
        sourceDemand_ = nullptr;
    }
    if (buildingDevelopment_) {
        buildingDevelopment_->Release();
        buildingDevelopment_ = nullptr;
    }
    if (landValue_) {
        landValue_->Release();
        landValue_ = nullptr;
    }
    if (traffic_) {
        traffic_->Release();
        traffic_ = nullptr;
    }
    if (zoneManager_) {
        zoneManager_->Release();
        zoneManager_ = nullptr;
    }
    if (occupantManager_) {
        occupantManager_->Release();
        occupantManager_ = nullptr;
    }
    if (city_) {
        city_->Release();
        city_ = nullptr;
    }
    for (cSC4ResponseCurve*& curve : pollutionCurves_) DeleteCurve(curve);
    DeleteCurve(landValueCurve_);
    DeleteCurve(slopeCurve_);
    DeleteCurve(auraCurve70_);
    DeleteCurve(residentialCurve1_);
    DeleteCurve(residentialCurve2_);
    DeleteCurve(residentialCurve3_);
    DeleteCurve(commercialCurve3320_);
    DeleteCurve(commercialCurve3330_);
    DeleteCurve(residentialMap138Curve_);
    DeleteCurve(residentialMap140Curve_);
    DeleteCurve(crimeCurve_);
    DeleteCurve(trafficCurve_);
    DeleteCurve(trafficMap48Curve_);
    DeleteCurve(auraCurve78_);
    DeleteCurve(auraCurve68_);
    return true;
}

//==================================================================================================
// tunables
//==================================================================================================

void cSC4GrowthDeveloper::DeleteCurve(cSC4ResponseCurve*& curve) {
    delete curve;      // the original deletes the curve's point vector, then the curve
    curve = nullptr;
}

// @ 0x100156c0c (bodies of the curve property reads: cSC4ResponseCurve::SetResponseCurveData(property)).
void cSC4GrowthDeveloper::ReadCurveProperty(std::uint32_t propertyId, cSC4ResponseCurve*& curve) {
    cISCProperty* property = exemplar_ ? exemplar_->GetProperty(propertyId) : nullptr;   // holder slot 0x28
    if (!property) {
        DeleteCurve(curve);
        return;
    }
    if (!curve) curve = new cSC4ResponseCurve();
    const cIGZVariant* value = property->GetPropertyValue();          // property slot 0x30
    curve->SetResponseCurveData(value->RefFloat32(), value->GetCount());   // variant slots 0x3a8 / 0x20
}

// @ 0x100156c0c. Reads every growth-developer property from the ini exemplar; a property that is absent keeps
// the constructor default (or the previous value when this is a re-read).
void cSC4GrowthDeveloper::ReadSimulatorTunableValues() {
    if (!exemplar_ || !g_host) return;
    std::uint32_t u32 = 0;
    std::uint8_t u8 = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertySubType, u32))
        subType_ = static_cast<std::int32_t>(u32);
    if (cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyType, u8)) type_ = u8;
    if (cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyDemandType, u32)) {
        if (sourceDemand_) {
            sourceDemand_->Release();
            sourceDemand_ = nullptr;
        }
        // demand simulator slot 0x30: the channel of that demand type, owned by the simulator.
        sourceDemand_ = g_host->DemandSimulator() ? g_host->DemandSimulator()->GetChannel(u32, kDemandFlagsSource)
                                                  : nullptr;
        if (sourceDemand_) sourceDemand_->AddRef();
    }
    if (cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyTractSize, u8)) SetTractSize(u8);
    if (cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyWealth, u8)) wealth_ = u8;
    if (cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyPurpose, u8)) purpose_ = u8;
    if (cISCProperty* property = exemplar_->GetProperty(kPropertyProfileFlags)) {
        // The value is a two-byte array: the construction profile's two flag bytes.
        const std::uint8_t* flags = property->GetPropertyValue()->RefUint8();     // variant slot 0x318
        constructionFlag10_ = flags[0];
        constructionFlag11_ = flags[1];
    }
    useTotalLandValue_ = false;
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyUseTotalLandValue, useTotalLandValue_);
    ReadCurveProperty(kPropertyLandValueCurve, landValueCurve_);
    ReadCurveProperty(kPropertySlopeCurve, slopeCurve_);
    ReadCurveProperty(kPropertyResidentialCurve1, residentialCurve1_);
    ReadCurveProperty(kPropertyResidentialCurve2, residentialCurve2_);
    ReadCurveProperty(kPropertyResidentialCurve3, residentialCurve3_);
    ReadCurveProperty(kPropertyCommercialCurve3320, commercialCurve3320_);
    ReadCurveProperty(kPropertyCommercialCurve3330, commercialCurve3330_);
    ReadCurveProperty(kPropertyAuraCurve70, auraCurve70_);
    for (std::uint32_t i = 0; i < 4; ++i)
        ReadCurveProperty(kPropertyPollutionCurveFirst + i, pollutionCurves_[i]);
    ReadCurveProperty(kPropertyResidentialMap138Curve, residentialMap138Curve_);
    ReadCurveProperty(kPropertyResidentialMap140Curve, residentialMap140Curve_);
    ReadCurveProperty(kPropertyCrimeCurve, crimeCurve_);
    ReadCurveProperty(kPropertyTrafficCurve, trafficCurve_);
    ReadCurveProperty(kPropertyTrafficMap48Curve, trafficMap48Curve_);
    ReadCurveProperty(kPropertyAuraCurve78, auraCurve78_);
    ReadCurveProperty(kPropertyAuraCurve68, auraCurve68_);

    desirabilityMapValue_ = 0x50;
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyDesirabilityMapValue, desirabilityMapValue_);
    desirabilityThreshold_ = 0x50;
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyDesirabilityThreshold, desirabilityThreshold_);
    if (!cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyDeclineThreshold, declineThreshold_))
        declineThreshold_ = desirabilityThreshold_;
    addPopulationToDemand_ = false;
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyAddPopulationToDemand, addPopulationToDemand_);

    styleCurves_.clear();
    std::uint8_t styleCount = 0;
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyStyleCurveCount, styleCount);
    styleCurves_.resize(styleCount);
    for (std::uint32_t i = 0; i < styleCount; ++i) {
        cISCProperty* property = exemplar_->GetProperty(kPropertyStyleCurveFirst + i);
        if (!property) {
            // A missing style curve becomes a single (0, 0) point, not an empty curve.
            const std::vector<::SC4Point> point = {{0.0f, 0.0f}};
            styleCurves_[i].SetResponseCurveData(point);
        } else {
            const cIGZVariant* value = property->GetPropertyValue();
            styleCurves_[i].SetResponseCurveData(value->RefFloat32(), value->GetCount());
        }
    }
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyPowerCap, powerSupplyCap_);
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyWaterCap, waterSupplyCap_);
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyFireCap, fireCoverageCap_);
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyParkCap, parkCap_);

    abandonmentThresholds_[kGrowthDensityLow] = 0.2f;      // 0x3e4ccccd
    abandonmentThresholds_[kGrowthDensityMedium] = 0.2f;
    abandonmentThresholds_[kGrowthDensityHigh] = 0.2f;
    if (cISCProperty* property = exemplar_->GetProperty(kPropertyAbandonmentThresholds)) {
        const float* thresholds = property->GetPropertyValue()->RefFloat32();
        abandonmentThresholds_[kGrowthDensityLow] = thresholds[0];
        abandonmentThresholds_[kGrowthDensityMedium] = thresholds[1];
        abandonmentThresholds_[kGrowthDensityHigh] = thresholds[2];
    }
    std::uint8_t monopoly = 1;
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertyMonopolyUse, monopoly);
    monopolyUse_ = monopoly;

    stylePercentages_.clear();
    if (cISCProperty* property = exemplar_->GetProperty(kPropertyStylePercentages)) {
        // Interleaved (style id, percentage * 100) uint32 pairs.
        const cIGZVariant* value = property->GetPropertyValue();
        const std::uint32_t* data = value->RefUint32();               // variant slot 0x358
        const std::uint32_t count = value->GetCount();                // variant slot 0x20
        for (std::uint32_t i = 0; i < count; i += 2)
            stylePercentages_[data[i]] = SC4Percentage(static_cast<float>(data[i + 1]) * 0.01f, true);
    }
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertySubjectiveThresholdLow, subjectiveThresholdLow_);
    cSCPropertyHelp::GetPropertyValue(exemplar_, kPropertySubjectiveThresholdHigh, subjectiveThresholdHigh_);
}

//==================================================================================================
// messages / population
//==================================================================================================

// @ 0x100157fd6.
bool cSC4GrowthDeveloper::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();      // message slot 0x20
    cIGZMessage2Standard& payload = StandardMessage(message);
    if (type == kMessageLotStateChanged) {
        // The message carries the cISC4Lot in its unknown slot (0x100); the host unwraps it.
        auto* lot = g_host->MessageLot(message);
        if (!lot) return true;
        const std::uint32_t oldState = static_cast<std::uint32_t>(payload.GetData1());   // 0x48
        const std::uint32_t newState = static_cast<std::uint32_t>(payload.GetData2());   // 0x70
        const std::uint32_t population = static_cast<std::uint32_t>(payload.GetData3()); // 0x98
        const std::int32_t wealth = static_cast<std::int32_t>(payload.GetData4());       // 0xc0
        if (wealth_ != wealth) return true;
        const bool hadCapacity = (oldState & 0xfe) == 2;
        const bool hasCapacity = (newState & 0xfe) == 2;
        if (hadCapacity == hasCapacity) return true;
        if (population == 0) {
            // Kept: the original reads the building type and throws the result away.
            (void)lot->GetBuildingType(true);
        } else {
            UpdatePopulationCount(lot, population, hasCapacity);
        }
    } else if (type == kMessageMaxCapacityRect) {
        auto* rect = static_cast<const SC4Rect<int>*>(payload.GetVoid2());   // slot 0x78
        if (rect) UpdateMaxCapacity(*rect);
    } else if (type == kMessageDayTick) {
        DoIdle(kDayTickBudgetMilliseconds);
        const std::uint32_t stage = static_cast<std::uint32_t>(mapUpdateStage_);
        if (stage != 0) {
            const std::uint32_t elapsed =
                static_cast<std::uint32_t>(g_host->SimDateNumber() - mapUpdateStartDate_);
            if (stage < elapsed) return false;
        }
    } else if (type == kMessageIdleTick) {
        DoIdle(static_cast<std::uint32_t>(payload.GetData3()));          // slot 0x98
    }
    return true;
}

// @ 0x100158140 (i386 0x000fd6c6). Spreads the lot's capacity delta over the cells of its bounding rectangle (one share
// per cell, the remainder on the lot's own cell) in the capacity map, then adds the delta to the total capacity. The
// message's data3 is the lot's building type: its profile's purpose must be this developer's (the port looked up the
// developer's own type, whose profile is empty, so no building ever added capacity).
void cSC4GrowthDeveloper::UpdatePopulationCount(cSC4Lot* lot, std::uint32_t buildingType, bool add) {
    if (!lot || !buildingDevelopment_ || !capacityMap_) return;
    const BuildingProfile& profile = buildingDevelopment_->GetBuildingProfile(buildingType);   // BDS vslot 0x9c
    if (purpose_ != static_cast<std::int32_t>(profile.purpose)) return;
    const std::uint16_t capacity = lot->GetCapacity(static_cast<std::uint32_t>(subType_), true);   // lot slot 0x198
    std::int32_t delta = -static_cast<std::int32_t>(capacity);
    if (add) delta = static_cast<std::int32_t>(capacity);
    std::uint8_t sizeX = 0, sizeZ = 0;
    lot->GetSize(sizeX, sizeZ);                                      // lot slot 0xb0
    const std::int32_t cellCount = static_cast<std::int32_t>(sizeZ) * static_cast<std::int32_t>(sizeX);
    if (cellCount == 0) return;                                      // the original divides by it
    const std::int32_t perCell = delta / cellCount;
    const std::int32_t remainder = delta - cellCount * perCell;
    std::int32_t rect[4] = {0, 0, 0, 0};
    lot->GetBoundingRect(rect);                                      // lot slot 0x70
    std::int32_t x0 = rect[0], z0 = rect[1], x1 = rect[2], z1 = rect[3];
    if (x1 < x0) x1 = x0;
    if (z1 < z0) z1 = z0;
    auto& data = capacityMap_->GetGridData();                        // grid slot 0xd8
    const int shift = capacityMap_->GetTractShift();                 // grid slot 0x58
    for (std::int32_t x = x0; x <= x1; ++x) {
        for (std::int32_t z = z0; z <= z1; ++z) {
            const int tx = x >> shift;
            const int tz = z >> shift;
            const std::int32_t value = data[static_cast<std::size_t>(tx)][static_cast<std::size_t>(tz)] + perCell;
            if (value < 0) {
                // Kept: the original asks the lot configuration for its id and discards it.
                if (cISC4LotConfigurationRef* configuration = lot->GetLotConfiguration())
                    (void)configuration->GetID();
                data[static_cast<std::size_t>(tx)][static_cast<std::size_t>(tz)] = 0;
            } else {
                data[static_cast<std::size_t>(tx)][static_cast<std::size_t>(tz)] =
                    static_cast<std::uint16_t>(value);
            }
        }
    }
    // The remainder goes to the lot's own cell; the original does not check that it is inside the map, the
    // port skips the write when it is not.
    std::int32_t cellX = 0, cellZ = 0;
    lot->GetLocation(cellX, cellZ);                                  // lot slot 0xa8
    cellX >>= shift;
    cellZ >>= shift;
    if (cellX >= 0 && cellZ >= 0 && static_cast<std::size_t>(cellX) < data.size() &&
        static_cast<std::size_t>(cellZ) < data[static_cast<std::size_t>(cellX)].size()) {
        const std::int32_t value =
            data[static_cast<std::size_t>(cellX)][static_cast<std::size_t>(cellZ)] + remainder;
        if (value < 0) {
            if (cISC4LotConfigurationRef* configuration = lot->GetLotConfiguration())
                (void)configuration->GetID();
            data[static_cast<std::size_t>(cellX)][static_cast<std::size_t>(cellZ)] = 0;
        } else {
            data[static_cast<std::size_t>(cellX)][static_cast<std::size_t>(cellZ)] =
                static_cast<std::uint16_t>(value);
        }
    }
    totalCapacity_ += delta;
}

//==================================================================================================
// the map update
//==================================================================================================

// @ 0x10015837c. One step of the twenty-stage map update. The original also constructs, starts and destroys
// a cRZTimer it never reads; that profiling leftover is not ported.
void cSC4GrowthDeveloper::DoIdle(std::uint32_t budgetMilliseconds) {
    if (!g_host) return;
    switch (mapUpdateStage_) {
    case 1:
        if (initialized_) {
            mapUpdateStartDate_ = g_host->SimDateNumber();       // simulator slot 0x40
            mapUpdateStage_ = 2;
        }
        break;
    case 2: {
        // The desirability map starts at the configured value.
        if (!desirabilityWorkMap_) {
            mapUpdateStage_ = 3;    // the port skips a stage whose map is missing instead of crashing
            break;
        }
        auto& data = desirabilityWorkMap_->GetGridData();
        const std::uint16_t value = static_cast<std::uint16_t>(desirabilityMapValue_);
        for (int x = 0; x < desirabilityWorkMap_->GetTractCountX(); ++x)
            for (int z = 0; z < desirabilityWorkMap_->GetTractCountZ(); ++z)
                data[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = value;
        mapUpdateStage_ = 3;
        break;
    }
    case 3:
        if (landValueCurve_ && landValue_) {
            cSC4SimGrid<std::uint8_t>* map = useTotalLandValue_ ? landValue_->TotalLandValueMap()
                                                                : landValue_->IntrinsicLandValueMap();
            if (map)
                ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(*desirabilityWorkMap_, *map,
                                                                        AddCurveToShort<std::uint8_t>,
                                                                        landValueCurve_);
        }
        mapUpdateStage_ = 4;
        break;
    case 4:
        if (slopeCurve_ && landValue_) {
            if (cSC4SimGrid<float>* map = landValue_->SlopeMap())
                ProcessTwoGridsWithCallback<std::int16_t, float>(*desirabilityWorkMap_, *map,
                                                                 AddCurveToShort<float>, slopeCurve_);
        }
        mapUpdateStage_ = 5;
        break;
    case 5:
        if (g_host->Residential()) {
            if (residentialCurve1_) {
                if (auto* map = g_host->Residential()->WealthMap(1))
                    ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(
                        *desirabilityWorkMap_, *map, AddCurveToShort<std::uint8_t>, residentialCurve1_);
            }
            if (residentialCurve2_) {
                if (auto* map = g_host->Residential()->WealthMap(2))
                    ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(
                        *desirabilityWorkMap_, *map, AddCurveToShort<std::uint8_t>, residentialCurve2_);
            }
            if (residentialCurve3_) {
                if (auto* map = g_host->Residential()->WealthMap(3))
                    ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(
                        *desirabilityWorkMap_, *map, AddCurveToShort<std::uint8_t>, residentialCurve3_);
            }
        }
        mapUpdateStage_ = 6;
        break;
    case 6:
        if (g_host->Commercial()) {
            if (commercialCurve3320_) {
                if (auto* map = g_host->Commercial()->DeveloperMap(kDeveloperTypeCommercialOfficeMedium))
                    ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(
                        *desirabilityWorkMap_, *map, AddCurveToShort<std::uint8_t>, commercialCurve3320_);
            }
            if (commercialCurve3330_) {
                if (auto* map = g_host->Commercial()->DeveloperMap(kDeveloperTypeCommercialOfficeHigh))
                    ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(
                        *desirabilityWorkMap_, *map, AddCurveToShort<std::uint8_t>, commercialCurve3330_);
            }
        }
        mapUpdateStage_ = 7;
        break;
    case 7:
        if (auraCurve70_ && g_host->Aura()) {
            if (auto* map = g_host->Aura()->Int16MapA())
                ProcessTwoGridsWithCallback<std::int16_t, std::int16_t>(
                    *desirabilityWorkMap_, *map, AddCurveToShort<std::int16_t>, auraCurve70_);
        }
        mapUpdateStage_ = 8;
        break;
    case 8:
        if (g_host->Pollution()) {
            for (std::uint32_t i = 0; i < 4; ++i) {
                if (!pollutionCurves_[i]) continue;
                if (auto* map = g_host->Pollution()->TypeMap(static_cast<int>(i)))
                    ProcessTwoGridsWithCallback<std::int16_t, std::int16_t>(
                        *desirabilityWorkMap_, *map, AddCurveToShort<std::int16_t>, pollutionCurves_[i]);
            }
        }
        mapUpdateStage_ = 9;
        break;
    case 9:
        if (residentialMap138Curve_ && g_host->Residential()) {
            if (auto* map = g_host->Residential()->Map138())
                ProcessTwoGridsWithCallback<std::int16_t, float>(*desirabilityWorkMap_, *map,
                                                                 AddCurveToShort<float>,
                                                                 residentialMap138Curve_);
        }
        mapUpdateStage_ = 10;
        break;
    case 10:
        if (residentialMap140Curve_ && g_host->Residential()) {
            if (auto* map = g_host->Residential()->Map140())
                ProcessTwoGridsWithCallback<std::int16_t, float>(*desirabilityWorkMap_, *map,
                                                                 AddCurveToShort<float>,
                                                                 residentialMap140Curve_);
        }
        mapUpdateStage_ = 11;
        break;
    case 11:
        if (crimeCurve_ && g_host->Crime()) {
            if (auto* map = g_host->Crime()->CrimeMap())
                ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(
                    *desirabilityWorkMap_, *map, AddCurveToShort<std::uint8_t>, crimeCurve_);
        }
        mapUpdateStage_ = 12;
        break;
    case 12: {
        // Traffic density: the minimum non-zero cell of each tract block (0xff when the block is empty)
        // goes through the congestion curve.
        if (!trafficCurve_ || !traffic_ || !desirabilityWorkMap_) {
            mapUpdateStage_ = 13;
            break;
        }
        IGrowthTrafficDensityApi* density = traffic_->Density();
        if (!density) {
            mapUpdateStage_ = 13;
            break;
        }
        auto& data = desirabilityWorkMap_->GetGridData();
        const int countX = desirabilityWorkMap_->GetTractCountX();
        const int countZ = desirabilityWorkMap_->GetTractCountZ();
        const int tractSize = desirabilityWorkMap_->GetTractSize();
        for (int x = 0; x < countX; ++x) {
            for (int z = 0; z < countZ; ++z) {
                std::uint8_t minimum = 0xff;
                for (int dx = 0; dx < tractSize; ++dx) {
                    for (int dz = 0; dz < tractSize; ++dz) {
                        const std::uint8_t value = density->GetValue(x * tractSize + dx, z * tractSize + dz);
                        const std::uint8_t candidate = value < minimum ? value : minimum;
                        if (value != 0) minimum = candidate;
                    }
                }
                const float curve = trafficCurve_->GetOutputValue(static_cast<float>(minimum));
                std::int16_t& cell = data[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
                cell = static_cast<std::int16_t>(static_cast<std::int32_t>(
                    curve + static_cast<float>(static_cast<std::int32_t>(cell))));
            }
        }
        mapUpdateStage_ = 13;
        break;
    }
    case 13:
        if (trafficMap48Curve_ && traffic_) {
            if (auto* map = traffic_->CongestionMap())
                ProcessTwoGridsWithCallback<std::int16_t, std::uint8_t>(
                    *desirabilityWorkMap_, *map, AddCurveToShort<std::uint8_t>, trafficMap48Curve_);
        }
        mapUpdateStage_ = 14;
        break;
    case 14:
        if (auraCurve78_ && g_host->Aura()) {
            if (auto* map = g_host->Aura()->Int16MapB())
                ProcessTwoGridsWithCallback<std::int16_t, std::int16_t>(
                    *desirabilityWorkMap_, *map, AddCurveToShort<std::int16_t>, auraCurve78_);
        }
        // Collect the special buildings the landmark pass walks.
        specialBuildings_.clear();
        if (buildingDevelopment_) {
            std::list<cRZAutoRefCount<cISC4Occupant>> buildings;
            buildingDevelopment_->GetSpecialBuildings(buildings);    // building development slot 0x160
            for (const auto& building : buildings) specialBuildings_.push_back(building);
        }
        specialCursor_ = specialBuildings_.begin();
        mapUpdateStage_ = 15;
        break;
    case 15: {
        // The landmark pass: one radius effect per call, continuing where the last call stopped.
        if (!desirabilityWorkMap_) {
            mapUpdateStage_ = 16;
            break;
        }
        bool applied = false;
        for (;;) {
            if (specialCursor_ == specialBuildings_.end()) {
                mapUpdateStage_ = 16;
                break;
            }
            cISC4Occupant* occupant = specialCursor_->get();
            applied = ApplyLandmarkEffect(g_host, subType_, *desirabilityWorkMap_, occupant);
            ++specialCursor_;
            if (!applied) continue;
            if (specialCursor_ == specialBuildings_.end()) mapUpdateStage_ = 16;
            break;
        }
        break;
    }
    case 16:
        if (auraCurve68_ && g_host->Aura()) {
            if (auto* map = g_host->Aura()->Int8Map())
                ProcessTwoGridsWithCallback<std::int16_t, std::int8_t>(
                    *desirabilityWorkMap_, *map, AddCurveToShort<std::int8_t>, auraCurve68_);
        }
        mapUpdateStage_ = 17;
        break;
    case 17: {
        // Snapshot the desirability map and collect the tracts that can still grow.
        if (!zoneCellCountMap_ || !desirabilityWorkMap_ || !desirabilityMap_) {
            mapUpdateStage_ = 18;
            break;
        }
        const auto& zoneCells = zoneCellCountMap_->GetGridData();
        auto& desirabilityMap = desirabilityMap_->GetGridData();
        const auto& desirabilityWork = desirabilityWorkMap_->GetGridData();
        const int countX = desirabilityWorkMap_->GetTractCountX();
        const int countZ = desirabilityWorkMap_->GetTractCountZ();
        growthList_.clear();
        for (int x = 0; x < countX; ++x) {
            for (int z = 0; z < countZ; ++z) {
                desirabilityMap[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] =
                    desirabilityWork[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
                if (zoneCells[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] != 0)
                    growthList_.push_back(GrowthItem{x, z, 1});
            }
        }
        mapUpdateStage_ = 18;
        if (cursor_) {
            cursor_->state = 0;
            cursor_->x = 0;
            cursor_->z = 0;
            if (cursor_->startX > 0 || cursor_->startZ > 0 || cursor_->maxX < 0 || cursor_->maxZ < 0)
                cursor_->state = 2;
        }
        break;
    }
    case 18: {
        // The population map driver: capacity times the desirability fraction, one tract per iteration,
        // until the time budget runs out. The port checks the slice every tract (the original tested a
        // counter that steps by 0x400 against a 0x3ff mask, which always fires).
        if (!cursor_ || !desirabilityWorkMap_ || !populationMap_ || !capacityMap_) {
            mapUpdateStage_ = 19;
            break;
        }
        const std::uint64_t slice = g_host->BeginSlice(budgetMilliseconds);
        auto& desirability = desirabilityWorkMap_->GetGridData();
        auto& populationGrid = populationMap_->GetGridData();
        auto& capacity = capacityMap_->GetGridData();
        for (;;) {
            if (g_host->SliceExpired(slice)) return;
            if (cursor_->state == 0) {
                cursor_->state = 1;
            } else {
                if (cursor_->state == 2) {
                    mapUpdateStage_ = 19;
                    return;
                }
                cursor_->x += cursor_->step;
                if (cursor_->maxX < cursor_->x) {
                    cursor_->x = cursor_->startX;
                    cursor_->z += cursor_->step;
                    if (cursor_->maxZ < cursor_->z) {
                        cursor_->state = 2;
                        mapUpdateStage_ = 19;
                        return;
                    }
                }
            }
            const auto x = static_cast<std::size_t>(cursor_->x);
            const auto z = static_cast<std::size_t>(cursor_->z);
            float fraction = static_cast<float>(desirability[x][z]) / 100.0f;
            if (fraction < 0.0f) fraction = 0.0f;
            if (fraction > 1.0f) fraction = 1.0f;
            populationGrid[x][z] = static_cast<std::int16_t>(
                TruncateToUint16(static_cast<float>(capacity[x][z]) * fraction));
            // The tract developer's UpdatePopulation (building development slot 0x118 -> 0x38); the original
            // leaves the profile's stage byte uninitialised.
            if (buildingDevelopment_)
                buildingDevelopment_->UpdateTractPopulation(this, cursor_->x, cursor_->z);
        }
    }
    case 19: {
        // The population of the growth tracts, weighted by how far below the desirability threshold they
        // are, is added to the source demand.
        std::int32_t total = 0;
        if (addPopulationToDemand_ && desirabilityWorkMap_ && populationMap_) {
            const auto& desirability = desirabilityWorkMap_->GetGridData();
            const auto& populationGrid = populationMap_->GetGridData();
            for (const GrowthItem& item : growthList_) {
                const auto x = static_cast<std::size_t>(item.x);
                const auto z = static_cast<std::size_t>(item.z);
                const std::int32_t value = desirability[x][z];
                const std::int32_t remaining = desirabilityThreshold_ - value;
                if (remaining != 0 && value <= desirabilityThreshold_) {
                    float factor = static_cast<float>(remaining) / 100.0f;
                    if (factor > 1.0f) factor = 1.0f;
                    total += static_cast<std::int32_t>(static_cast<float>(populationGrid[x][z]) * factor);
                }
            }
        }
        if (total != populationTotal_) {
            if (sourceDemand_) sourceDemand_->AddToSupply(static_cast<float>(total - populationTotal_));
            populationTotal_ = total;
        }
        mapUpdateStage_ = 20;
        break;
    }
    case 20: {
        // Sort the growth list by desirability, build the decline list from the low end, and hand every
        // growth tract a style index.
        if (!desirabilityWorkMap_) {
            mapUpdateStage_ = 0;
            break;
        }
        const auto& desirability = desirabilityWorkMap_->GetGridData();
        g_desirabilityMap = &desirability;
        std::sort(growthList_.begin(), growthList_.end(), CompareByDesirability);
        declineList_.clear();
        for (auto it = growthList_.end(); it != growthList_.begin();) {
            --it;
            const std::int32_t value =
                desirability[static_cast<std::size_t>(it->x)][static_cast<std::size_t>(it->z)];
            if (declineThreshold_ > value) declineList_.push_back(*it);
        }
        if (static_cast<std::uint32_t>(purpose_ - 5) < 4) {
            g_compareSeedMask = g_host->RandomUint32Raw();
            std::sort(declineList_.begin(), declineList_.end(), CompareByRandom);
        }
        PruneGrowthList();

        // The demand the new tracts may consume.
        float demand = 0.0f;
        IGrowthDemandSimulatorApi* demandSimulator = g_host->DemandSimulator();
        if (type_ == kGrowthTypeIndustrial) {
            if (demandSimulator) {
                if (IGrowthDemandApi* channel = demandSimulator->GetChannel(kDemandTypeIndustrial, kDemandFlagsSource))
                    demand = channel->GetValue();
            }
        } else if (type_ == kGrowthTypeCommercial || type_ == kGrowthTypeResidential) {
            const int connected = g_host->Neighbors() ? g_host->Neighbors()->ConnectedCityCount() : 0;
            const int all = g_host->Neighbors() ? g_host->Neighbors()->AllCityCount() : 0;
            float extra = 0.0f;
            if (all >= 1 && demandSimulator) {
                if (IGrowthDemandApi* channel =
                        demandSimulator->GetChannel(kDemandTypeNeighborShare, kDemandFlagsNeighbor))
                    extra = (static_cast<float>(connected) / static_cast<float>(all)) * channel->GetValue() * 0.5f;
            }
            float base = 0.0f;
            if (demandSimulator) {
                if (IGrowthDemandApi* channel = demandSimulator->GetChannel(kDemandTypeRci, 0))
                    base = channel->GetValue();
            }
            demand = base + extra;
            if (type_ == kGrowthTypeResidential && demandSimulator) {
                if (IGrowthDemandApi* channel = demandSimulator->GetChannel(kDemandTypeResidential, 0)) {
                    const float floor = channel->GetValue();
                    if (floor > demand) demand = floor;
                }
            }
        }

        const float stageMultiplier = buildingDevelopment_ ? buildingDevelopment_->GetStagePopulationMultiplier() : 0.0f;
        const std::size_t styleCount = styleCurves_.size();
        if (styleCount > 0) {
            const float scaled = demand * stageMultiplier;
            float threshold = styleCurves_[styleCount - 1].GetOutputValue(scaled);
            // Kept: the style index is a running value across items, starts at the curve count and is
            // assigned as it stands, so it can be the count itself (out of range) or 1 for a single style.
            std::size_t styleIndex = styleCount;
            std::int32_t itemIndex = 0;
            const float itemCount = static_cast<float>(growthList_.size());
            for (GrowthItem& item : growthList_) {
                if (styleIndex > 1) {
                    const float percent = static_cast<float>(itemIndex * 100) / itemCount;
                    if (percent >= threshold) {
                        std::size_t index = styleIndex;
                        std::size_t curveIndex = styleIndex - 2;
                        for (;;) {
                            threshold += styleCurves_[curveIndex].GetOutputValue(scaled);
                            styleIndex = index - 1;
                            if (index < 3) break;
                            --curveIndex;
                            index = styleIndex;
                            if (percent < threshold) break;
                        }
                    }
                }
                item.style = static_cast<std::int32_t>(styleIndex);
                ++itemIndex;
            }
        }
        mapUpdateStage_ = 0;
        break;
    }
    default:
        break;
    }
}

void cSC4GrowthDeveloper::PruneGrowthList() {
    if (!desirabilityWorkMap_) return;
    const auto& desirability = desirabilityWorkMap_->GetGridData();
    for (auto it = growthList_.begin(); it != growthList_.end();) {
        if (desirability[static_cast<std::size_t>(it->x)][static_cast<std::size_t>(it->z)] <
            desirabilityThreshold_)
            it = growthList_.erase(it);
        else
            ++it;
    }
}

// @ 0x100159800. Per tract: how many of its cells admit a building whose zone fits this developer's
// construction purpose, and the summed maximum capacity of those cells.
void cSC4GrowthDeveloper::UpdateMaxCapacity(const SC4Rect<int>& rect) {
    if (!maxCapacityMap_ || !zoneCellCountMap_ || !zoneManager_ || !buildingDevelopment_ || !g_host) return;
    const int shift = maxCapacityMap_->GetTractShift();              // grid slot 0x58
    const int countX = maxCapacityMap_->GetTractCountX();            // grid slot 0x60
    const int countZ = maxCapacityMap_->GetTractCountZ();            // grid slot 0x68
    std::int32_t x0 = rect.x0 >> shift;
    std::int32_t z0 = rect.z0 >> shift;
    std::int32_t x1 = rect.x1 >> shift;
    std::int32_t z1 = rect.z1 >> shift;
    if (countX <= x1) x1 = countX - 1;
    if (countZ <= z1) z1 = countZ - 1;
    ConstructionProfile profile;
    GetConstructionInfo(profile);                                    // this->vslot 0x48
    // The original only raises the end to the start, so a rectangle that starts outside the grid writes
    // past the map; the port also pulls the start into the grid and does nothing for an empty range.
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x0 >= countX || z0 >= countZ) return;
    if (x1 < x0) x1 = x0;
    if (z1 < z0) z1 = z0;
    auto& zoneCells = zoneCellCountMap_->GetGridData();
    auto& maxCells = maxCapacityMap_->GetGridData();
    for (std::int32_t x = x0; x <= x1; ++x) {
        for (std::int32_t z = z0; z <= z1; ++z) {
            const std::int32_t cellX0 = x << shift;
            const std::int32_t cellZ0 = z << shift;
            std::int32_t cellX1 = ((x + 1) << shift) - 1;
            std::int32_t cellZ1 = ((z + 1) << shift) - 1;
            if (cellX1 < cellX0) cellX1 = cellX0;
            if (cellZ1 < cellZ0) cellZ1 = cellZ0;
            std::int32_t count = 0;
            float total = 0.0f;
            for (std::int32_t cx = cellX0; cx <= cellX1; ++cx) {
                for (std::int32_t cz = cellZ0; cz <= cellZ1; ++cz) {
                    const int zoneType = static_cast<signed char>(zoneManager_->ZoneTypeAt(cx, cz));
                    if (!g_host->BuildingFitsInZone(profile.purpose, static_cast<std::uint32_t>(zoneType)))
                        continue;
                    ++count;
                    total += buildingDevelopment_->GetMaxCapacity(zoneType, purpose_, wealth_);
                }
            }
            zoneCells[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] =
                static_cast<std::uint8_t>(count);
            maxCells[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] =
                TruncateToUint16(total);
        }
    }
}

//==================================================================================================
// queries
//==================================================================================================

// @ 0x10015a6bc: zone types 1..9 map to residential / commercial / industrial, everything else to 0
// (table 0x100a19110 = {1, 1, 1, 2, 2, 2, 3, 3, 3}).
std::int32_t cSC4GrowthDeveloper::ConvertZoneToType(int zoneType) {
    static constexpr std::int32_t kZoneToType[9] = {1, 1, 1, 2, 2, 2, 3, 3, 3};
    const std::uint32_t index = static_cast<std::uint32_t>(zoneType - 1);
    if (index >= 9) return 0;
    return kZoneToType[index];
}

// @ 0x10015a6da. The construction purpose comes from the developer type; the rest of the profile is copied
// straight out of the exemplar fields. For an industrial developer whose purpose is outside 5..8 the purpose
// field is left untouched (the original jumps over the store).
void cSC4GrowthDeveloper::GetConstructionInfo(ConstructionProfile& out) const {
    static constexpr std::uint32_t kIndustrialPurpose[4] = {7, 8, 8, 8};   // table 0x100a190c0
    bool storePurpose = true;
    std::uint32_t purpose = 0;
    if (type_ == kGrowthTypeResidential) {
        purpose = 1;
    } else if (type_ == kGrowthTypeCommercial) {
        purpose = 4;
    } else if (type_ == kGrowthTypeIndustrial) {
        const std::uint32_t index = static_cast<std::uint32_t>(purpose_ - 5);
        if (index >= 4) {
            storePurpose = false;       // the original jumps over the purpose store, nothing else
        } else {
            purpose = kIndustrialPurpose[index];
        }
    }
    if (storePurpose) out.purpose = purpose;
    out.value4 = constructionValue_;
    out.value8 = std::bit_cast<float>(wealth_);          // the original copies the int bit pattern
    out.value12 = static_cast<std::uint32_t>(purpose_);  // ditto
    out.flag10 = constructionFlag10_;
    out.flag11 = constructionFlag11_;
}

// @ 0x10015aace. Sums the response-curve outputs of every simulator map for one cell. The accumulator
// changes to a 16-bit value after the four pollution terms (the original truncates there).
std::int32_t cSC4GrowthDeveloper::EstimateDesirability(int x, int z) const {
    if (desirabilityMap_) (void)desirabilityMap_->GetTractShift();    // the original's dead call
    std::int32_t sum = desirabilityMapValue_;
    auto addByteCurve = [&sum, x, z](const cSC4ResponseCurve* curve, const cSC4SimGrid<std::uint8_t>* map) {
        if (!curve || !map) return;
        sum += static_cast<std::int32_t>(curve->GetOutputValue(static_cast<float>(map->GetCellValue(x, z))));
    };
    auto addInt16Curve = [&sum, x, z](const cSC4ResponseCurve* curve, const cSC4SimGrid<std::int16_t>* map) {
        if (!curve || !map) return;
        sum += static_cast<std::int32_t>(curve->GetOutputValue(static_cast<float>(map->GetCellValue(x, z))));
    };
    auto addFloatCurve = [&sum, x, z](const cSC4ResponseCurve* curve, const cSC4SimGrid<float>* map) {
        if (!curve || !map) return;
        sum += static_cast<std::int32_t>(curve->GetOutputValue(map->GetCellValue(x, z)));
    };
    if (landValueCurve_ && landValue_) {
        addByteCurve(landValueCurve_, useTotalLandValue_ ? landValue_->TotalLandValueMap()
                                                        : landValue_->IntrinsicLandValueMap());
    }
    if (slopeCurve_ && landValue_) addFloatCurve(slopeCurve_, landValue_->SlopeMap());
    if (g_host && g_host->Residential()) {
        if (residentialCurve1_) addByteCurve(residentialCurve1_, g_host->Residential()->WealthMap(1));
        if (residentialCurve2_) addByteCurve(residentialCurve2_, g_host->Residential()->WealthMap(2));
        if (residentialCurve3_) addByteCurve(residentialCurve3_, g_host->Residential()->WealthMap(3));
    }
    if (g_host && g_host->Commercial()) {
        if (commercialCurve3320_)
            addByteCurve(commercialCurve3320_, g_host->Commercial()->DeveloperMap(kDeveloperTypeCommercialOfficeMedium));
        if (commercialCurve3330_)
            addByteCurve(commercialCurve3330_, g_host->Commercial()->DeveloperMap(kDeveloperTypeCommercialOfficeHigh));
    }
    if (auraCurve70_ && g_host && g_host->Aura()) addInt16Curve(auraCurve70_, g_host->Aura()->Int16MapA());
    if (g_host && g_host->Pollution()) {
        for (std::uint32_t i = 0; i < 4; ++i)
            if (pollutionCurves_[i]) addInt16Curve(pollutionCurves_[i], g_host->Pollution()->TypeMap(static_cast<int>(i)));
    }
    // From here on the original accumulates in an int16_t.
    std::int16_t value = static_cast<std::int16_t>(sum);
    auto addShort = [&value](const cSC4ResponseCurve* curve, float input) {
        value = static_cast<std::int16_t>(value + static_cast<std::int16_t>(
                                                      static_cast<std::int32_t>(curve->GetOutputValue(input))));
    };
    if (g_host && g_host->Residential()) {
        if (residentialMap138Curve_) {
            if (auto* map = g_host->Residential()->Map138())
                addShort(residentialMap138Curve_, map->GetCellValue(x, z));
        }
        if (residentialMap140Curve_) {
            if (auto* map = g_host->Residential()->Map140())
                addShort(residentialMap140Curve_, map->GetCellValue(x, z));
        }
    }
    if (crimeCurve_ && g_host && g_host->Crime()) {
        if (auto* map = g_host->Crime()->CrimeMap()) addShort(crimeCurve_, static_cast<float>(map->GetCellValue(x, z)));
    }
    if (trafficMap48Curve_ && g_host && g_host->Traffic()) {
        if (auto* map = g_host->Traffic()->CongestionMap())
            addShort(trafficMap48Curve_, static_cast<float>(map->GetCellValue(x, z)));
    }
    if (auraCurve78_ && g_host && g_host->Aura()) {
        if (auto* map = g_host->Aura()->Int16MapB()) addShort(auraCurve78_, static_cast<float>(map->GetCellValue(x, z)));
    }
    if (auraCurve68_ && g_host && g_host->Aura()) {
        if (auto* map = g_host->Aura()->Int8Map()) addShort(auraCurve68_, static_cast<float>(map->GetCellValue(x, z)));
    }
    return value;
}

// @ 0x10015b21a. The threshold of a density type; the map grows on lookup, so an unknown density gets a 0
// entry (kept).
float cSC4GrowthDeveloper::GetAbandonmentThreshold(std::int32_t density) const {
    return abandonmentThresholds_[density];
}

// @ 0x10015b0cc. A negative index gets the low default (-4) and the high default (0x14); an index past the
// end of either table gets that table's default on its own.
void cSC4GrowthDeveloper::GetSubjectiveThresholds(int index, int& low, int& high) const {
    if (index < 0) {
        low = -4;
        high = 0x14;
        return;
    }
    const auto i = static_cast<std::size_t>(index);
    low = -4;
    if (i < subjectiveThresholdLow_.size()) low = static_cast<int>(subjectiveThresholdLow_[i]);
    high = 0x14;
    if (i < subjectiveThresholdHigh_.size()) high = static_cast<int>(subjectiveThresholdHigh_[i]);
}

// @ 0x10015b25c.
void cSC4GrowthDeveloper::StartMapUpdate() {
    if (mapUpdateStage_ == 0) mapUpdateStage_ = 1;
}

//==================================================================================================
// streams
//==================================================================================================

// @ 0x10015af3a. Version 1: total population, total capacity, then five of the six maps (the working
// desirability map at +0x1f0 is rebuilt every pass and is not saved; the snapshot at +0x1f8 is).
bool cSC4GrowthDeveloper::Write(cIGZOStream& stream) const {
    if (!g_host) return false;
    stream.SetUint16(1);
    stream.SetSint32(populationTotal_);
    stream.SetUint32(static_cast<std::uint32_t>(totalCapacity_));
    g_host->WriteGrid(stream, zoneCellCountMap_);
    g_host->WriteGrid(stream, desirabilityMap_);
    g_host->WriteGrid(stream, populationMap_);
    g_host->WriteGrid(stream, capacityMap_);
    g_host->WriteGrid(stream, maxCapacityMap_);
    return !stream.GetError();
}

// @ 0x10015afe2. Only version 1 is accepted. The original hands the read pointer straight to
// SC4ReadGZSerializable, which overwrites the five map pointers without releasing the Init-time maps (a
// leak); the port shuts the old map down and releases it first.
bool cSC4GrowthDeveloper::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    if (!stream.GetUint16(version)) return false;
    if (version != 1) return false;
    std::int32_t population = 0;
    std::uint32_t capacity = 0;
    if (!stream.GetSint32(population)) return false;
    if (!stream.GetUint32(capacity)) return false;
    populationTotal_ = population;
    totalCapacity_ = static_cast<std::int32_t>(capacity);
    if (!g_host) return false;
    auto replaceUint8 = [](cSC4SimGrid<std::uint8_t>*& map, cIGZSerializable* raw) {
        if (map) {
            map->Shutdown();
            map->Release();
        }
        map = static_cast<cSC4SimGrid<std::uint8_t>*>(raw);
    };
    auto replaceInt16 = [](cSC4SimGrid<std::int16_t>*& map, cIGZSerializable* raw) {
        if (map) {
            map->Shutdown();
            map->Release();
        }
        map = static_cast<cSC4SimGrid<std::int16_t>*>(raw);
    };
    auto replaceUint16 = [](cSC4SimGrid<std::uint16_t>*& map, cIGZSerializable* raw) {
        if (map) {
            map->Shutdown();
            map->Release();
        }
        map = static_cast<cSC4SimGrid<std::uint16_t>*>(raw);
    };
    replaceUint8(zoneCellCountMap_, g_host->ReadGrid(stream));
    replaceInt16(desirabilityMap_, g_host->ReadGrid(stream));
    replaceInt16(populationMap_, g_host->ReadGrid(stream));
    replaceUint16(capacityMap_, g_host->ReadGrid(stream));
    replaceUint16(maxCapacityMap_, g_host->ReadGrid(stream));
    return !stream.GetError();
}

}  // namespace sc4
