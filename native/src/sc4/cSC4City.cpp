#include "sc4/CommonIIDs.h"
#include "sc4/cSC4City.h"

#include <cstdio>
#include <cstdlib>
#include <bit>
#include <cmath>
#include <cstring>

#include "gz/cIGZInitShutdown.h"
#include "rz/CharLength.h"
#include "gz/cGZPersistResourceKeyList.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutoRecord.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {

#define C(Name, Off, Class, Iid) {CityComponent::Name, #Name, Off, Class, Iid}
const CityComponentInfo kComponentTable[] = {
    C(DemolitionUtility, 0x1e8, 0x0990c38a, 0x26dde2ea),
    C(DispatchManager, 0x1f0, 0x4990bd93, 0x694028ec),
    C(HistoryWarehouse, 0x1f8, 0x89efa536, 0x69f4a803),
    C(OccupantManager, 0x200, 0x098f964d, 0xc6bb8334),
    C(PropManager, 0x208, 0xa990bd10, 0x28326f3e),
    C(ZoneManager, 0x210, 0x298f9b2d, 0x67652433),
    C(ZoneDeveloper, 0x218, 0x498f9b01, 0xe818ed2b),
    C(LotDeveloper, 0x220, 0xa990bfe0, 0x4818e5d2),
    C(TractDeveloper, 0x228, 0x2990c142, 0xa818ed1b),
    C(LotManager, 0x230, 0, 0x681905bd),                    // queried from the zone developer
    C(LotConfigurationManager, 0x238, 0x299062c0, 0xc8edbec0),
    C(PropDeveloper, 0x240, 0x89c48f47, 0xe9a1f4b6),
    C(NetworkLotManager, 0x248, 0xe9a6cd3b, 0x89a5ba3f),
    C(Unknown250, 0x250, 0xead4ba36, 0x6ad4b9f1),
    C(Simulator, 0x258, 0x2990c1e5, 0x8695664e),
    C(AuraSimulator, 0x260, 0x0990bdd8, 0xc773e880),
    C(BudgetSimulator, 0x268, 0xe990be01, 0x6772bd23),
    C(BuildingDevelopmentSimulator, 0x270, 0x8990be7a, 0x07813eae),
    C(CommercialSimulator, 0x278, 0x8990c080, 0x077abeb2),
    C(CrimeSimulator, 0x280, 0x8990c09a, 0x67659555),
    C(DemandSimulator, 0x288, 0x2990bff2, 0x47967e16),
    C(FireProtectionSimulator, 0x290, 0x2990c05a, 0x88f54c7b),
    C(FlammabilitySimulator, 0x298, 0xe990c045, 0x48f41617),
    C(FloraSimulator, 0x2a0, 0xc990c034, 0x06c66177),
    C(IndustrialSimulator, 0x2a8, 0xa990c022, 0x477a9e7d),
    C(LandValueSimulator, 0x2b0, 0xe990c18e, 0xe7781d8c),
    C(NeighborsSimulator, 0x2b8, 0xc990c0a4, 0x0773fef0),
    C(OrdinanceSimulator, 0x2c0, 0xa990c08e, 0x41193c3a),
    C(PlumbingSimulator, 0x2c8, 0x0990c075, 0x47782ccc),
    C(PoliceSimulator, 0x2d0, 0x6990c04f, 0x67659895),
    C(PollutionSimulator, 0x2d8, 0x8990c065, 0x877181ec),
    C(PowerSimulator, 0x2e0, 0x0990c005, 0xe9e7bff5),
    C(ResidentialSimulator, 0x2e8, 0x4990c013, 0x077ac1ee),
    C(TrafficSimulator, 0x2f0, 0x6990c1aa, 0x68027f37),
    C(WeatherSimulator, 0x2f8, 0x2990c1bc, 0xc6c66245),
    C(MySimAgentSimulator, 0x300, 0x29e37dec, 0xa9e37ebb),
    C(CivicBuildingSimulator, 0x308, 0x0a413481, 0x8a41322f),
    C(LandfillDeveloper, 0x310, 0xea09d4ae, 0x8a09d502),
    C(AirportDeveloper, 0x318, 0xaa2049a1, 0xea20548d),
    C(SeaportDeveloper, 0x320, 0xea230a28, 0xca22fe3e),
    C(ParkManager, 0x328, 0xaa3fe6bc, 0xaa3fe6bc),
    C(DisasterLayer, 0x330, 0x61f6abf5, 0x29b02a2c),
    C(AdvisorSystem, 0x338, 0x4a0dd616, 0x4a0dd617),
    C(TutorialSystem, 0x340, 0xaa3836fb, 0xaa3836f6),
    C(VehicleManager, 0x348, 0xa98f9d3b, 0x098f9bf4),
    C(PedestrianManager, 0x350, 0xc98f9d17, 0x098f9bf4),
    C(AircraftManager, 0x358, 0xc98f9d4a, 0x098f9bf4),
    C(WatercraftManager, 0x360, 0xa98f9d28, 0x098f9bf4),
    C(AutomataControllerManager, 0x368, 0xca14ca03, 0xca14c9fc),
    C(AutomataScriptSystem, 0x370, 0x4a1b664e, 0x8a1b6647),
    C(TrafficNetwork, 0x378, 0x6a0f82b2, 0xca0f81b9),
    C(CitySituationManager, 0x380, 0x0bb14381, 0x6bb1437a),
    C(SurfaceWater, 0x388, 0x0b8da7db, 0xab8da759),
    C(Terrain, 0x390, 0xe98f9525, 0xc70bd37e),
    C(Clock24Hour, 0x398, 0xa7e7f929, 0x681bd53a),
    C(EffectsManager, 0x3a0, 0x49822f75, 0xc97cf5cd),
    C(NetworkManager, 0x3a8, 0xc990bd46, 0xa9764c8c),
};
#undef C
static_assert(sizeof(kComponentTable) / sizeof(kComponentTable[0]) == static_cast<std::size_t>(CityComponent::kCount));

using CC = CityComponent;

// Order the original creates the components in CreateCityComponents (the lot manager comes
// right after the zone developer it is queried from).
constexpr CC kCreateOrder[] = {
    CC::Simulator, CC::Terrain, CC::SurfaceWater, CC::EffectsManager, CC::OccupantManager, CC::ZoneDeveloper,
    CC::LotDeveloper, CC::TractDeveloper, CC::LandfillDeveloper, CC::AirportDeveloper, CC::SeaportDeveloper,
    CC::ParkManager, CC::LotManager, CC::ZoneManager, CC::LotConfigurationManager, CC::PropDeveloper,
    CC::NetworkLotManager, CC::Unknown250, CC::Clock24Hour, CC::PropManager, CC::NetworkManager,
    CC::DispatchManager, CC::AuraSimulator, CC::BudgetSimulator, CC::BuildingDevelopmentSimulator,
    CC::CommercialSimulator, CC::CrimeSimulator, CC::DemandSimulator, CC::FireProtectionSimulator,
    CC::FlammabilitySimulator, CC::FloraSimulator, CC::IndustrialSimulator, CC::LandValueSimulator,
    CC::NeighborsSimulator, CC::OrdinanceSimulator, CC::PlumbingSimulator, CC::PoliceSimulator,
    CC::PollutionSimulator, CC::PowerSimulator, CC::ResidentialSimulator, CC::TrafficSimulator,
    CC::WeatherSimulator, CC::MySimAgentSimulator, CC::CivicBuildingSimulator, CC::DisasterLayer,
    CC::DemolitionUtility, CC::HistoryWarehouse, CC::TrafficNetwork, CC::VehicleManager, CC::PedestrianManager,
    CC::AircraftManager, CC::WatercraftManager, CC::AutomataScriptSystem, CC::AutomataControllerManager,
    CC::CitySituationManager, CC::AdvisorSystem, CC::TutorialSystem,
};

// Order Init() is called on the components (Simulator and HistoryWarehouse first, then the rest).
constexpr CC kInitOrder[] = {
    CC::Simulator, CC::HistoryWarehouse, CC::Terrain, CC::SurfaceWater, CC::EffectsManager, CC::AutomataScriptSystem,
    CC::AutomataControllerManager, CC::OccupantManager, CC::ZoneManager, CC::LotConfigurationManager,
    CC::ZoneDeveloper, CC::LandfillDeveloper, CC::AirportDeveloper, CC::SeaportDeveloper, CC::ParkManager,
    CC::LotManager, CC::Clock24Hour, CC::PropManager, CC::NetworkManager, CC::DispatchManager, CC::PropDeveloper,
    CC::NetworkLotManager, CC::BudgetSimulator, CC::OrdinanceSimulator, CC::PowerSimulator, CC::PlumbingSimulator,
    CC::NeighborsSimulator, CC::BuildingDevelopmentSimulator, CC::TractDeveloper, CC::LotDeveloper,
    CC::DemandSimulator, CC::AuraSimulator, CC::CommercialSimulator, CC::CrimeSimulator,
    CC::FireProtectionSimulator, CC::FlammabilitySimulator, CC::FloraSimulator, CC::IndustrialSimulator,
    CC::LandValueSimulator, CC::PoliceSimulator, CC::TrafficSimulator, CC::PollutionSimulator,
    CC::ResidentialSimulator, CC::WeatherSimulator, CC::CivicBuildingSimulator, CC::DisasterLayer,
    CC::DemolitionUtility, CC::TrafficNetwork, CC::VehicleManager, CC::PedestrianManager, CC::AircraftManager,
    CC::WatercraftManager, CC::AdvisorSystem, CC::CitySituationManager, CC::TutorialSystem,
    CC::MySimAgentSimulator, CC::Unknown250,
};

// Shutdown() then Release() in this order.
constexpr CC kShutdownOrder[] = {
    CC::TutorialSystem, CC::CitySituationManager, CC::AdvisorSystem, CC::WatercraftManager, CC::AircraftManager,
    CC::PedestrianManager, CC::VehicleManager, CC::TrafficNetwork, CC::AutomataControllerManager,
    CC::AutomataScriptSystem, CC::DemolitionUtility, CC::DisasterLayer, CC::CivicBuildingSimulator,
    CC::MySimAgentSimulator, CC::WeatherSimulator, CC::TrafficSimulator, CC::ResidentialSimulator,
    CC::PowerSimulator, CC::PollutionSimulator, CC::PoliceSimulator, CC::PlumbingSimulator,
    CC::OrdinanceSimulator, CC::NeighborsSimulator, CC::LandValueSimulator, CC::IndustrialSimulator,
    CC::FloraSimulator, CC::FlammabilitySimulator, CC::FireProtectionSimulator, CC::CrimeSimulator,
    CC::CommercialSimulator, CC::BudgetSimulator, CC::AuraSimulator, CC::DemandSimulator, CC::AirportDeveloper,
    CC::SeaportDeveloper, CC::ParkManager, CC::LandfillDeveloper, CC::ZoneDeveloper, CC::LotDeveloper,
    CC::TractDeveloper, CC::BuildingDevelopmentSimulator, CC::DispatchManager, CC::NetworkManager,
    CC::Clock24Hour, CC::PropDeveloper, CC::NetworkLotManager, CC::Unknown250, CC::LotConfigurationManager,
    CC::LotManager, CC::ZoneManager, CC::OccupantManager, CC::PropManager, CC::EffectsManager, CC::SurfaceWater,
    CC::Terrain, CC::HistoryWarehouse, CC::Simulator,
};

// Order of the components in the city stream (Write; Read follows the same order). The last three
// only exist from file version 9 / 11 / 12.
constexpr CC kStreamOrder[] = {
    CC::HistoryWarehouse, CC::DemolitionUtility, CC::DispatchManager, CC::OccupantManager, CC::PropManager,
    CC::ZoneManager, CC::ZoneDeveloper, CC::LotManager, CC::LotConfigurationManager, CC::PropDeveloper,
    CC::NetworkLotManager, CC::LandfillDeveloper, CC::AirportDeveloper, CC::SeaportDeveloper, CC::ParkManager,
    CC::LotDeveloper, CC::TractDeveloper, CC::Simulator, CC::AuraSimulator, CC::BudgetSimulator,
    CC::BuildingDevelopmentSimulator, CC::CommercialSimulator, CC::CrimeSimulator, CC::DemandSimulator,
    CC::FireProtectionSimulator, CC::FlammabilitySimulator, CC::FloraSimulator, CC::IndustrialSimulator,
    CC::LandValueSimulator, CC::NeighborsSimulator, CC::OrdinanceSimulator, CC::PlumbingSimulator,
    CC::PoliceSimulator, CC::PollutionSimulator, CC::PowerSimulator, CC::ResidentialSimulator,
    CC::TrafficSimulator, CC::WeatherSimulator, CC::CivicBuildingSimulator, CC::AutomataScriptSystem,
    CC::AutomataControllerManager, CC::TrafficNetwork, CC::VehicleManager, CC::PedestrianManager,
    CC::AircraftManager, CC::WatercraftManager, CC::AdvisorSystem, CC::TutorialSystem, CC::Terrain,
    CC::Clock24Hour, CC::Unknown250, CC::CitySituationManager, CC::NetworkManager,
};
constexpr std::size_t kStreamCoreCount = sizeof(kStreamOrder) / sizeof(kStreamOrder[0]) - 3;   // before the version-gated tail

constexpr std::uint32_t kCityAutoRecordType = 0x89d9b9ee;
constexpr std::uint32_t kCityAutoRecordGroup = 0x69d9b9f1;
constexpr std::uint32_t kNameStringRecordId = 0x6aaee931;
constexpr std::uint32_t kMayorStringRecordId = 0x6aaee932;
constexpr float kInverseCellWidth = 0.0625f;

// floor(value) the way the original computes it (round-half-even of value - 0.5, then fix up).
int FloorLikeOriginal(float value) {
    const float biased = value + -0.5f + 12582912.0f;
    const int bits = static_cast<int>(std::bit_cast<std::uint32_t>(biased));
    int result = bits - 0x4b400000;
    const int next = bits - 0x4b3fffff;
    const float nextAsFloat = static_cast<float>(next);
    if (!std::isnan(value) && !std::isnan(nextAsFloat) && value == nextAsFloat) result = next;
    return result;
}

}  // namespace

const CityComponentInfo& GetCityComponentInfo(CityComponent component) {
    return kComponentTable[static_cast<std::size_t>(component)];
}

// =============================================================================================
cSC4City::cSC4City(ICityHost* host) : host_(host) {
    serialNumber_ = host_->CreateCitySerialNumber();
    host_->GetCurrentLanguageAndCountry(originalLanguage_, originalCountry_);
    lastLanguage_ = originalLanguage_;
    lastCountry_ = originalCountry_;
    host_->OnCityCreated();
}

cSC4City::~cSC4City() {
    Shutdown();
    host_->OnCityDestroyed();
}

bool cSC4City::QueryInterface(std::uint32_t iid, void** out) {
    // The city is two sub-objects: cISC4City (primary) and cIGZSerializable. Callers cast the answer back to the interface
    // they asked for (cSC4App::Read takes the cISC4City one), so each id must answer its own view.
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4City) {
        *out = static_cast<cISC4City*>(this);
        AddRef();
        return true;
    }
    if (iid == kCityClassId || iid == kGZIID_cIGZSerializable) {
        *out = static_cast<cIGZSerializable*>(this);
        AddRef();
        return true;
    }
    return properties_.QueryInterface(iid, out);
}

std::uint32_t cSC4City::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// ---- strings --------------------------------------------------------------------------------
void cSC4City::SetCityName(const cIGZString& name) {
    if (originalCityName_.Strlen() == 0) originalCityName_.AssignFromChars(cityName_.Data(), cityName_.Strlen());
    cityName_.AssignFromIGZString(name);
    SC4LimitStringLength(cityName_, 0x80);
    // heap-allocated like the original (operator new + AddRef, released after the call): the queue keeps posted messages
    auto* message = new cRZMessage2Standard;
    message->AddRef();
    message->SetType(kCityNameChangedMessage);
    message->SetVoid1(this);
    message->SetIGZUnknown(static_cast<cIGZSerializable*>(this));
    host_->PostMessage(message);
    message->Release();
}

void cSC4City::SetMayorName(const cIGZString& name) {
    if (originalMayorName_.Strlen() == 0) originalMayorName_.AssignFromChars(mayorName_.Data(), mayorName_.Strlen());
    mayorName_.AssignFromIGZString(name);
    if (mayorName_.Strlen() > 0x80) mayorName_.Resize(0x80);
    // heap-allocated like the original (operator new + AddRef, released after the call): the queue keeps posted messages
    auto* message = new cRZMessage2Standard;
    message->AddRef();
    message->SetType(kMayorNameChangedMessage);
    message->SetVoid1(this);
    message->SetIGZUnknown(static_cast<cIGZSerializable*>(this));
    host_->PostMessage(message);
    message->Release();
}

void cSC4City::SetCityDescription(const cIGZString& description) {
    if (originalDescription_.Strlen() == 0)
        originalDescription_.AssignFromChars(description_.Data(), description_.Strlen());
    description_.AssignFromIGZString(description);
    if (description_.Strlen() > 0x100) description_.Resize(0x100);
}

void cSC4City::SetEstablished(bool established) {
    if (established_ == established) return;
    established_ = established;
    // heap-allocated like the original (operator new + AddRef, released after the call): the queue keeps posted messages
    auto* message = new cRZMessage2Standard;
    message->AddRef();
    message->SetType(0x26d31ec5u - (established ? 1u : 0u));
    message->SetVoid1(this);
    message->SetIGZUnknown(static_cast<cIGZSerializable*>(this));
    host_->SendMessage(message);
    message->Release();
    host_->OnEstablishedChanged(established);
}

void cSC4City::SetWorldPosition(float longitude, float latitude) {
    worldLongitude_ = longitude > 180.0f ? 180.0f : (longitude < -180.0f ? -180.0f : longitude);
    worldLatitude_ = latitude > 90.0f ? 90.0f : (latitude < -90.0f ? -90.0f : latitude);
}

// ---- geometry -------------------------------------------------------------------------------
bool cSC4City::SetSize(float sizeX, float sizeZ) {
    if (initialized_) return false;
    sizeX_ = sizeX;
    sizeZ_ = sizeZ;
    cellCountX_ = static_cast<int>(sizeX * kInverseCellWidth);
    cellCountZ_ = static_cast<int>(sizeZ * kInverseCellWidth);
    return true;
}

void cSC4City::PositionToCell(float x, float z, int& cellX, int& cellZ) const {
    cellX = FloorLikeOriginal(x * kInverseCellWidth);
    cellZ = FloorLikeOriginal(z * kInverseCellWidth);
}

void cSC4City::CellCornerToPosition(int cellX, int cellZ, float& x, float& z) const {
    x = static_cast<float>(cellX) * cellWidthX_;
    z = static_cast<float>(cellZ) * cellWidthZ_;
}

void cSC4City::CellCenterToPosition(int cellX, int cellZ, float& x, float& z) const {
    x = (static_cast<float>(cellX) + 0.5f) * cellWidthX_;
    z = (static_cast<float>(cellZ) + 0.5f) * cellWidthZ_;
}

bool cSC4City::LocationIsInBounds(float x, float z) const { return x >= 0.0f && z >= 0.0f && x < sizeX_ && z < sizeZ_; }
bool cSC4City::CellIsInBounds(int cellX, int cellZ) const {
    return cellX >= 0 && cellZ >= 0 && cellX < cellCountX_ && cellZ < cellCountZ_;
}
bool cSC4City::CellCornerIsInBounds(int cellX, int cellZ) const {
    return cellX >= 0 && cellZ >= 0 && cellX <= cellCountX_ && cellZ <= cellCountZ_;
}

// ---- components -----------------------------------------------------------------------------
void cSC4City::CreateCityComponents() {
    for (CityComponent id : kCreateOrder) {
        auto& slot = components_[static_cast<std::size_t>(id)];
        if (slot) continue;
        const CityComponentInfo& info = GetCityComponentInfo(id);
        cIGZUnknown* object = nullptr;
        if (id == CityComponent::LotManager) {
            // the lot manager is another view of the zone developer
            cIGZUnknown* developer = GetComponent(CityComponent::ZoneDeveloper);
            void* raw = nullptr;
            if (developer && developer->QueryInterface(info.iid, &raw)) object = static_cast<cIGZUnknown*>(raw);
        } else {
            object = host_->CreateComponent(info.classId, info.iid);
        }
        if (object) {
            slot = object;          // takes its own reference
            object->Release();      // drop the creation reference
            host_->OnComponentCreated(id, object, sizeX_, sizeZ_);
        }
    }
}

void ICityHost::InitComponent(CityComponent, cIGZUnknown* object) {
    void* raw = nullptr;
    if (object && object->QueryInterface(kGZIID_cIGZInitShutdown, &raw)) {
        auto* initShutdown = static_cast<cIGZInitShutdown*>(raw);
        initShutdown->Init();
        initShutdown->Release();
    }
}

void ICityHost::ShutdownComponent(CityComponent, cIGZUnknown* object) {
    void* raw = nullptr;
    if (object && object->QueryInterface(kGZIID_cIGZInitShutdown, &raw)) {
        auto* initShutdown = static_cast<cIGZInitShutdown*>(raw);
        initShutdown->Shutdown();
        initShutdown->Release();
    }
}

bool cSC4City::Init() {
    if (initialized_) return true;
    host_->RegisterCityCommands();
    CreateCityComponents();
    host_->OnCityInitialized();
    bool birthDateSet = false;
    for (CityComponent id : kInitOrder) {
        if (cIGZUnknown* component = GetComponent(id)) host_->InitComponent(id, component);   // slot 0x18 of the component
        if (!birthDateSet && id == CityComponent::HistoryWarehouse) {
            birthDate_ = host_->GetSimulatorDate();      // read once the simulator and history are up
            birthDateSet = true;
        }
    }
    if (!loaded_) host_->OnCityNotLoaded();
    if (host_->ShouldEstablishNewCity()) established_ = true;
    initialized_ = true;
    return true;
}

bool cSC4City::Shutdown() {
    if (!initialized_) return true;
    host_->OnCityShuttingDown();
    for (CityComponent id : kShutdownOrder) {
        auto& slot = components_[static_cast<std::size_t>(id)];
        if (!slot) continue;
        host_->ShutdownComponent(id, slot.get());   // slot 0x20 of the component
        slot = nullptr;
    }
    host_->UnregisterCityCommands();
    initialized_ = false;
    return true;
}

// ---- serialization --------------------------------------------------------------------------
bool cSC4City::Write(cIGZOStream& stream) const {
    if (stream.GetError()) return false;
    stream.SetUint16(kCityVersion);
    properties_.Write(stream);
    stream.SetUint32(serialNumber_);
    stream.SetUint32(occupantSerial_);
    stream.SetSint32(originalLanguage_);
    stream.SetSint32(originalCountry_);
    stream.SetSint32(lastLanguage_);
    stream.SetSint32(lastCountry_);
    stream.SetUint32(birthDate_);
    stream.SetUint8(established_ ? 1 : 0);
    stream.SetUint8(static_cast<std::uint8_t>(difficulty_));
    stream.SetGZStr(cityName_);
    stream.SetGZStr(originalCityName_);
    stream.SetGZStr(mayorName_);
    stream.SetGZStr(originalMayorName_);
    stream.SetGZStr(description_);
    stream.SetGZStr(originalDescription_);
    stream.SetFloat32(worldLongitude_);
    stream.SetFloat32(worldLatitude_);
    stream.SetFloat32(worldBaseElevation_);
    stream.SetUint32(saveDisableCount_);
    stream.SetFloat32(sizeX_);
    stream.SetFloat32(sizeZ_);
    stream.SetFloat32(cellWidthX_);
    stream.SetFloat32(cellWidthZ_);
    stream.SetFloat32(invCellWidthX_);
    stream.SetFloat32(invCellWidthZ_);
    stream.SetSint32(cellCountX_);
    stream.SetSint32(cellCountZ_);
    for (CityComponent id : kStreamOrder) SC4WriteGZSerializable(stream, GetComponent(id));
    stream.SetUint8(saveThumbnailFromFile_ ? 1 : 0);

    // cheat code manager state
    {
        cSC4AutoSaveRecord record(stream, kCityAutoRecordType, kCityAutoRecordGroup, 0, 1);
        if (record.IsOpen()) {
            std::vector<char> state;
            host_->SaveCheatCodeState(state);
            record.GetRecord()->SetFieldUint32(static_cast<std::uint32_t>(state.size()));
            record.GetRecord()->SetFieldVoid(state.data(), static_cast<std::uint32_t>(state.size()));
        }
    }
    cIGZPersistDBSegment* segment = nullptr;
    void* raw = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &raw)) {
        auto* db = static_cast<cISC4DBSegmentOStream*>(raw);
        if (cISC4DBSegment* owner = db->GetSegment()) segment = owner->AsIGZPersistDBSegment();
        db->Release();
    }
    WriteRetainedRecords(segment);
    return !stream.GetError();
}

bool cSC4City::Read(cIGZIStream& stream) {
    const_cast<cSC4City*>(this)->loaded_ = true;
    std::uint16_t version = 0;
    if (stream.GetError()) return false;
    stream.GetUint16(version);
    if (static_cast<std::uint16_t>(version - 8) > 4) return false;
    properties_.Read(stream);
    stream.GetUint32(serialNumber_);
    stream.GetUint32(occupantSerial_);
    stream.GetSint32(originalLanguage_);
    stream.GetSint32(originalCountry_);
    stream.GetSint32(lastLanguage_);
    stream.GetSint32(lastCountry_);
    stream.GetUint32(birthDate_);
    std::uint8_t established = 0;
    stream.GetUint8(established);
    established_ = established != 0;
    if (version < 10) {
        difficulty_ = established_ ? 2u : 0u;
    } else {
        std::uint8_t level = 0;
        stream.GetUint8(level);
        difficulty_ = level;
    }
    stream.GetGZStr(cityName_);
    stream.GetGZStr(originalCityName_);
    stream.GetGZStr(mayorName_);
    stream.GetGZStr(originalMayorName_);
    stream.GetGZStr(description_);
    stream.GetGZStr(originalDescription_);
    stream.GetFloat32(worldLongitude_);
    stream.GetFloat32(worldLatitude_);
    stream.GetFloat32(worldBaseElevation_);
    stream.GetUint32(saveDisableCount_);
    stream.GetFloat32(sizeX_);
    stream.GetFloat32(sizeZ_);
    stream.GetFloat32(cellWidthX_);
    stream.GetFloat32(cellWidthZ_);
    stream.GetFloat32(invCellWidthX_);
    stream.GetFloat32(invCellWidthZ_);
    stream.GetSint32(cellCountX_);
    stream.GetSint32(cellCountZ_);
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "city: read version %u established %d difficulty %u save-disable count %u cells %d x %d\n", version, int(established_), difficulty_, saveDisableCount_, cellCountX_, cellCountZ_);
    if (!established_) host_->LoadDefaultCityName(cityName_);

    auto readComponent = [&](CityComponent id) {
        const CityComponentInfo& info = GetCityComponentInfo(id);
        void* raw = nullptr;
        if (!SC4ReadGZSerializable(stream, kGZIID_cIGZUnknown, &raw, false) || !raw) return;
        auto* unknown = static_cast<cIGZUnknown*>(raw);
        void* typed = nullptr;
        if (unknown->QueryInterface(info.iid, &typed) && typed) {
            auto* object = static_cast<cIGZUnknown*>(typed);
            components_[static_cast<std::size_t>(id)] = object;
            object->Release();
        } else if (void* plain = nullptr; unknown->QueryInterface(kGZIID_PlainObject, &plain) && plain) {
            // a class the port keeps as plain C++ comes wrapped in an adapter that answers no interface of its own: the
            // city keeps the adapter, as it does for one it creates (CreateCityComponents), and the file's object is used
            components_[static_cast<std::size_t>(id)] = unknown;
        }
        unknown->Release();
    };
    for (std::size_t i = 0; i < kStreamCoreCount; ++i) readComponent(kStreamOrder[i]);
    // version-gated tail: Unknown250 (>8), CitySituationManager (>10), NetworkManager (>11)
    if (version > 8) {
        readComponent(CityComponent::Unknown250);
        if (version > 10) {
            readComponent(CityComponent::CitySituationManager);
            if (version > 11) readComponent(CityComponent::NetworkManager);
        }
    }
    std::uint8_t saveEnabled = 0;
    stream.GetUint8(saveEnabled);
    saveThumbnailFromFile_ = saveEnabled != 0;

    {
        cSC4AutoLoadRecord record(stream, kCityAutoRecordType, kCityAutoRecordGroup, 0);
        if (record.IsOpen() && record.GetVersion() == 1) {
            std::uint32_t size = 0;
            record.GetRecord()->GetFieldUint32(size);
            std::vector<char> state(size);
            record.GetRecord()->GetFieldVoid(state.data(), size);
            host_->LoadCheatCodeState(state.data(), size);
        }
    }
    const int language = host_->GetUILanguage();
    cIGZPersistDBSegment* segment = nullptr;
    void* raw = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &raw)) {
        auto* db = static_cast<cISC4DBSegmentIStream*>(raw);
        if (cISC4DBSegment* owner = db->GetSegment()) segment = owner->AsIGZPersistDBSegment();
        db->Release();
    }
    cRZString text;
    if (host_->ReadStringRecord(segment, kNameStringRecordId, language, text)) SetCityName(text);
    if (host_->ReadStringRecord(segment, kMayorStringRecordId, language, text)) SetMayorName(text);
    ReadRetainedRecords(segment);
    return true;
}

bool cSC4City::ReadRetainedRecords(cIGZPersistDBSegment* segment) {
    retained_.clear();
    if (!segment) return true;    // (the original dereferences it)
    auto* filter = new cGZPersistResourceKeyFilterByGroup(kRetainedRecordGroup);
    filter->AddRef();
    auto* keys = new cGZPersistResourceKeyList();
    keys->AddRef();
    segment->GetResourceKeyList(keys, filter);
    for (std::uint32_t i = 0, n = keys->Size(); i < n; ++i) {
        RetainedRecordData record;
        record.key = *keys->GetKey(i);
        if (SC4ReadBinaryRecord(segment, record.key, record.data, 10000000)) retained_.push_back(std::move(record));
    }
    keys->Release();
    filter->Release();
    return true;
}

bool cSC4City::WriteRetainedRecords(cIGZPersistDBSegment* segment) const {
    if (!segment) return false;
    for (const RetainedRecordData& record : retained_) {
        void* data = const_cast<std::uint8_t*>(record.data.data());
        segment->WriteRecord(record.key, data, static_cast<std::uint32_t>(record.data.size()));
    }
    return false;   // (the original returns false regardless)
}

bool cSC4City::SaveObliterated(cIGZPersistDBSegment* /*segment*/) const { return true; }

void cSC4City::WriteRegionViewThumbnail(cIGZPersistDBSegment& target, const cIGZString& sourceFile, bool reuse) {
    bool copied = false;
    const bool tryFile = reuse ? (saveThumbnailFromFile_ = false, true) : saveThumbnailFromFile_;
    if (tryFile && host_->FileExists(sourceFile)) {
        if (cIGZPersistDBSegment* old = host_->OpenSaveGame(sourceFile)) {
            // four thumbnail records (type 0x8a2482b9 / 0x2026960b, group 0x4a2482bb, instances 0..3)
            static const cGZPersistResourceKey keys[4] = {
                {0x8a2482b9, 0x4a2482bb, 0}, {0x2026960b, 0x4a2482bb, 1}, {0x8a2482b9, 0x4a2482bb, 2},
                {0x2026960b, 0x4a2482bb, 3}};
            bool first = true;
            for (const cGZPersistResourceKey& key : keys) {
                std::uint32_t size = old->GetRecordSize(key);
                if (size == 0xffffffffu) {
                    if (first) break;     // the first one decides whether the file has thumbnails
                    continue;
                }
                std::vector<std::uint8_t> buffer(size);
                std::uint32_t got = size;
                if (old->ReadRecord(key, buffer.data(), got) == 0 && !first) continue;
                const bool wrote = target.WriteRecord(key, buffer.data(), got);
                if (first) {
                    copied = wrote;
                    if (!wrote) break;
                    first = false;
                }
            }
            host_->CloseSaveGame(old);
        }
    }
    if (!copied) {
        if (!host_->WriteRegionViewThumbnail(target)) host_->WriteDefaultRegionViewThumbnail(target);
    }
    saveThumbnailFromFile_ = true;
}

// ---- commands ---------------------------------------------------------------------------------
bool cSC4City::DoCommand(std::uint32_t command, const cIGZString* arg0, bool argIsValid, CommandReply* reply) {
    switch (command) {
        case 0x8bb5eae9:   // GetCityEstablished
            if (reply) reply->SetBool(0, GetEstablished());
            break;
        case 0xabac4a22:   // GetCitySize
            if (reply) {
                reply->SetInt(0, cellCountX_);
                reply->SetInt(1, cellCountZ_);
            }
            break;
        case 0xabb5eaeb:   // SetCityEstablished
            if (arg0 && !argIsValid) {
                if (reply) reply->SetStatus(2);
                return true;
            }
            SetEstablished(true);
            break;
        case 0xcbac4b3f:   // GetCityName
            if (reply) reply->SetString(0, cityName_);
            break;
        case 0xebac4b41:   // GetMayorName
            if (reply) reply->SetString(0, mayorName_);
            break;
        case 0xebac4b42:   // SetMayorName
            if (arg0 && arg0->Strlen() != 0) SetMayorName(*arg0);
            break;
        case 0x6bac4b40:   // SetCityName
            if (arg0 && arg0->Strlen() != 0) SetCityName(*arg0);
            break;
        default:
            break;
    }
    return true;
}

bool cSC4City::DoCheatCode(std::uint32_t code, const char* argument) {
    if (!argument) return true;
    cRZString text(argument);
    if (code == 0xea73fcc2) SetCityName(text);        // "change city name" cheat
    else if (code == 0xea73fcd8) SetMayorName(text);  // "change mayor name" cheat
    return true;
}

}  // namespace sc4
