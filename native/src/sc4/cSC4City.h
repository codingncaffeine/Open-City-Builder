// cSC4City — the city object: identity (names, serial number, dates), geometry, difficulty,
// "established" state and the set of city components (terrain, simulators, managers, ...).
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (vtable 0x100a692d0, 140 functions; class id
// 0x8990c372, interfaces 0x66ba4db5 (cISC4City), 0x452294aa (message target), 0xe4fda3d4
// (serializable) and the property holder 0x25216283).
//
// Components are created by class id through the COM class factory (CreateCityComponents) and kept
// as cIGZUnknown references; their typed interfaces are filled in as each class is recovered. The
// table below is the byte-exact (class id, interface id) list of the original, with the offset
// each pointer occupies in the original object.
//
// Serialization (Write version 12; Read accepts 8..12): u16 version, the property holder, serial
// numbers, language/country pairs, birth date, established, difficulty, 6 strings (name, original
// name, mayor, original mayor, description, original description), world position/elevation, the
// save-disable counter, city size/cell metrics, then every component through the class-object
// serializer in a fixed order, the "save enabled" byte and a cheat-code-manager state sub-record
// {0x89d9b9ee, 0x69d9b9f1, 0}. Records of group 0xab85e27d found in the file are retained verbatim
// and written back (so unknown data survives a load/save).
//
// Quirks kept: SetSize only works before Init; the city-name setter truncates by UTF-8 characters
// to 128 bytes while the mayor name/description truncate bytewise (128 / 256); an established
// city whose name was never set keeps the loaded name, an unestablished one gets the localized
// default name; saving writes the version-12 layout only.
#pragma once
#include <array>
#include <cstdint>

#include "sc4/ISC4City.h"
#include <list>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZInitShutdown.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "sc4/SCProperty.h"

namespace sc4 {

constexpr std::uint32_t kCityClassId = 0x8990c372;
constexpr std::uint32_t kGZIID_cISC4City = 0x66ba4db5;
constexpr std::uint16_t kCityVersion = 12;
constexpr std::uint32_t kRetainedRecordGroup = 0xab85e27d;
constexpr std::uint32_t kCityNameChangedMessage = 0x0ab99380;
constexpr std::uint32_t kMayorNameChangedMessage = 0x0ab99381;
constexpr std::uint32_t kCityEstablishedMessage = 0x26d31ec4;    // 0x26d31ec5 when un-established

// Components, in the order of the original object layout.
enum class CityComponent : std::uint8_t {
    DemolitionUtility, DispatchManager, HistoryWarehouse, OccupantManager, PropManager, ZoneManager,
    ZoneDeveloper, LotDeveloper, TractDeveloper, LotManager, LotConfigurationManager, PropDeveloper,
    NetworkLotManager, Unknown250, Simulator, AuraSimulator, BudgetSimulator, BuildingDevelopmentSimulator,
    CommercialSimulator, CrimeSimulator, DemandSimulator, FireProtectionSimulator, FlammabilitySimulator,
    FloraSimulator, IndustrialSimulator, LandValueSimulator, NeighborsSimulator, OrdinanceSimulator,
    PlumbingSimulator, PoliceSimulator, PollutionSimulator, PowerSimulator, ResidentialSimulator,
    TrafficSimulator, WeatherSimulator, MySimAgentSimulator, CivicBuildingSimulator, LandfillDeveloper,
    AirportDeveloper, SeaportDeveloper, ParkManager, DisasterLayer, AdvisorSystem, TutorialSystem,
    VehicleManager, PedestrianManager, AircraftManager, WatercraftManager, AutomataControllerManager,
    AutomataScriptSystem, TrafficNetwork, CitySituationManager, SurfaceWater, Terrain, Clock24Hour,
    EffectsManager, NetworkManager,
    kCount
};

struct CityComponentInfo {
    CityComponent id;
    const char* name;
    std::uint32_t objectOffset;   // offset in the original object
    std::uint32_t classId;        // GZCOM class (0 = obtained from another component)
    std::uint32_t iid;            // interface requested / stored
};
const CityComponentInfo& GetCityComponentInfo(CityComponent component);

// Services the original reaches through SL::sp* globals and the RZ language manager.
class ICityHost {
public:
    virtual ~ICityHost() = default;
    virtual std::uint32_t CreateCitySerialNumber() = 0;                         // SC4CreateCitySerialNumber
    virtual void GetCurrentLanguageAndCountry(int& language, int& country) = 0; // RZ::LM() slots 0x38/0xd8
    virtual cIGZUnknown* CreateComponent(std::uint32_t classId, std::uint32_t iid) = 0;   // GZCOM CreateInstance
    virtual void PostMessage(cIGZMessage2* message) = 0;                        // message server slot 0x20
    virtual void SendMessage(cIGZMessage2* message) = 0;                        // message server slot 0x18
    virtual void OnCityCreated() {}
    virtual void OnCityInitialized() {}
    virtual void OnCityShuttingDown() {}
    virtual void OnCityDestroyed(cISC4City* city) { (void)city; }   // the city is going away (its destructor, after Shutdown)
    virtual void OnComponentCreated(CityComponent /*component*/, cIGZUnknown* /*object*/, float /*sizeX*/, float /*sizeZ*/) {}
    // Init / Shutdown of one component (cSC4City::Init / Shutdown). The original calls slot 0x18 / 0x20 of the stored interface
    // directly: every cISC4 component interface starts QueryInterface, AddRef, Release, Init, Shutdown. The port keeps components
    // as cIGZUnknown, whose interfaces do not carry those slots, so the host (which knows the concrete classes) makes the call; the
    // default asks for cIGZInitShutdown, which only test doubles answer.
    virtual void InitComponent(CityComponent component, cIGZUnknown* object);
    virtual void ShutdownComponent(CityComponent component, cIGZUnknown* object);
    virtual void OnCityNotLoaded() {}                                            // cheat manager reset (slot 0x50)
    virtual bool ShouldEstablishNewCity() { return false; }                      // application flag at +0x208
    virtual void RegisterCityCommands() {}                                       // message/command/cheat ids
    virtual void UnregisterCityCommands() {}
    virtual std::uint32_t GetSimulatorDate() { return 0; }                       // simulator -> clock -> date
    virtual void OnEstablishedChanged(bool /*established*/) {}                   // simulator slot 0x108
    virtual void LoadDefaultCityName(cIGZString& /*name*/) {}                    // SC4LoadString(0x6a231eaa, 0xaa738c4e)
    // SaveObliterated: the terrain (slot SaveObliterated) and then the flora simulator (slot 24) write themselves into the
    // obliteration segment (flora.md 10.3)
    virtual void SaveObliterated(cIGZPersistDBSegment* /*segment*/) {}
    // cISC4App slot 5 RunMessageServer2Pump(100, 500, 5000): the city's Shutdown drains the occupant manager's queued Remove and
    // Destroy messages before the manager is released (occupant-manager.md 2.3, 0x0045B826)
    virtual void PumpMessageServer2() {}
    virtual bool ReadStringRecord(cIGZPersistDBSegment* /*segment*/, std::uint32_t /*instance*/, int /*language*/,
                                  cIGZString& /*out*/) { return false; }
    virtual int GetUILanguage() { return 0; }                                    // RZ::LM() slot 0x38 on load
    // cheat code manager state blob (sub-record 0x89d9b9ee/0x69d9b9f1)
    virtual void SaveCheatCodeState(std::vector<char>& /*out*/) {}
    virtual void LoadCheatCodeState(const void* /*data*/, std::uint32_t /*size*/) {}
    // thumbnails (region view)
    virtual bool WriteRegionViewThumbnail(cIGZPersistDBSegment& /*segment*/) { return false; }
    virtual void WriteDefaultRegionViewThumbnail(cIGZPersistDBSegment& /*segment*/) {}
    virtual cIGZPersistDBSegment* OpenSaveGame(const cIGZString& /*path*/) { return nullptr; }
    virtual void CloseSaveGame(cIGZPersistDBSegment* /*segment*/) {}
    virtual bool FileExists(const cIGZString& /*path*/) { return false; }
};

class cSC4City final : public cISC4City, public cIGZSerializable {
public:
    enum DifficultyLevel : std::uint32_t { kDifficultyEasy = 0, kDifficultyMedium = 1, kDifficultyHard = 2 };
    enum CitySizeType : int { kSizeSmall = 0, kSizeMedium = 1, kSizeLarge = 2 };

    explicit cSC4City(ICityHost* host);
    ~cSC4City();

    // cIGZUnknown / cIGZSerializable
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kCityClassId; }

    bool Init() override;
    bool Shutdown() override;
    void CreateCityComponents();

    // identity
    std::uint32_t GetCitySerialNumber() const { return serialNumber_; }
    void SetCitySerialNumber(std::uint32_t n) override { serialNumber_ = n; }
    std::uint32_t GetNewOccupantSerialNumber() { return ++occupantSerial_; }   // (returns the old+1)
    bool GetOriginalLanguageAndCountry(int& language, int& country) const { language = originalLanguage_; country = originalCountry_; return true; }
    bool GetLastLanguageAndCountry(int& language, int& country) const { language = lastLanguage_; country = lastCountry_; return true; }
    void GetCitySaveFilePath(cIGZString& out) const override { out.Copy(saveFilePath_); }
    void SetCitySaveFilePath(const cIGZString& path) override { saveFilePath_.AssignFromIGZString(path); }
    void GetCityName(cIGZString& out) const { out.AssignFromChars(cityName_.Data(), cityName_.Strlen()); }
    void SetCityName(const cIGZString& name);
    bool GetCityNameChanged() const { return cityNameChanged_; }
    void SetCityNameChanged(bool changed) override { cityNameChanged_ = changed; }
    void GetMayorName(cIGZString& out) const { out.AssignFromChars(mayorName_.Data(), mayorName_.Strlen()); }
    void SetMayorName(const cIGZString& name);
    void GetCityDescription(cIGZString& out) const { out.AssignFromChars(description_.Data(), description_.Strlen()); }
    void SetCityDescription(const cIGZString& description);
    std::uint32_t GetBirthDate() const { return birthDate_; }
    void SetBirthDate(std::uint32_t date) { birthDate_ = date; }
    bool GetEstablished() const { return established_; }
    void SetEstablished(bool established);
    DifficultyLevel GetDifficultyLevel() const { return static_cast<DifficultyLevel>(difficulty_); }
    void SetDifficultyLevel(DifficultyLevel level) { difficulty_ = static_cast<std::uint32_t>(level); }

    // world placement
    void GetWorldPosition(float& longitude, float& latitude) const { longitude = worldLongitude_; latitude = worldLatitude_; }
    void SetWorldPosition(float longitude, float latitude);
    float GetWorldBaseElevation() const { return worldBaseElevation_; }
    void SetWorldBaseElevation(float elevation) { worldBaseElevation_ = elevation; }
    int GetWorldHemisphere() const { return 2 - (worldLatitude_ > 0.0f ? 1 : 0); }   // 1 north, 2 south

    // geometry (cells are 16 m; only SetSize before Init)
    int GetCitySizeType() const { return cellCountX_ == 0x40 ? kSizeSmall : (cellCountX_ == 0x80 ? kSizeMedium : kSizeLarge); }
    bool SetSize(float sizeX, float sizeZ);
    float SizeX() const { return sizeX_; }
    float SizeZ() const { return sizeZ_; }
    float CellWidthX() const { return cellWidthX_; }
    float CellWidthZ() const { return cellWidthZ_; }
    int CellCountX() const { return cellCountX_; }
    int CellCountZ() const { return cellCountZ_; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) const;
    void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) const;
    void CellCenterToPosition(int cellX, int cellZ, float& x, float& z) const;
    bool LocationIsInBounds(float x, float z) const;
    bool CellIsInBounds(int cellX, int cellZ) const;
    bool CellCornerIsInBounds(int cellX, int cellZ) const;

    // save control / UI locks
    void EnableSave() { if (saveDisableCount_ != 0) --saveDisableCount_; }
    void DisableSave() { ++saveDisableCount_; }
    bool IsSaveDisabled() const override { return saveDisableCount_ != 0; }
    void UIIncreaseLockCount() { ++uiLockCount_; }
    void UIDecreaseLockCount() { if (uiLockCount_ != 0) --uiLockCount_; }
    std::uint32_t UIGetLockCount() const { return uiLockCount_; }

    // components
    cIGZUnknown* GetComponent(CityComponent c) const { return components_[static_cast<std::size_t>(c)].get(); }
    void SetComponent(CityComponent c, cIGZUnknown* object) { components_[static_cast<std::size_t>(c)] = object; }

    // retained records and thumbnails
    struct RetainedRecordData {
        cGZPersistResourceKey key;
        std::vector<std::uint8_t> data;
    };
    const std::list<RetainedRecordData>& GetRetainedRecords() const { return retained_; }
    bool ReadRetainedRecords(cIGZPersistDBSegment* segment);
    bool WriteRetainedRecords(cIGZPersistDBSegment* segment) const;
    void WriteRegionViewThumbnail(cIGZPersistDBSegment& target, const cIGZString& sourceFile, bool reuseFromFile);
    bool SaveObliterated(cIGZPersistDBSegment* segment) const;

    cSCPropertyHolder& Properties() { return properties_; }
    const cSCPropertyHolder& Properties() const { return properties_; }

    // message handling (the scripting/cheat commands; see the .cpp for the ids)
    struct CommandReply {
        virtual ~CommandReply() = default;
        virtual void SetString(int index, const cIGZString& value) = 0;
        virtual void SetInt(int index, int value) = 0;
        virtual void SetBool(int index, bool value) = 0;
        virtual void SetStatus(int status) = 0;
    };
    // `arg0` is the first command argument (a string) when the message carries one.
    bool DoCommand(std::uint32_t command, const cIGZString* arg0, bool argIsValid, CommandReply* reply);
    bool DoCheatCode(std::uint32_t code, const char* argument);
    bool IsInCityTimeSimulationMode() const { return inCityTimeMode_; }
    void ToggleSimulationMode() { inCityTimeMode_ = !inCityTimeMode_; }

private:
    ICityHost* host_;
    std::uint32_t refCount_ = 0;                 // +0x40
    bool initialized_ = false;                   // +0x44
    bool established_ = false;                   // +0x45
    std::uint32_t difficulty_ = 0;               // +0x48
    bool cityNameChanged_ = false;               // +0x4d
    bool loaded_ = false;                        // +0x4c (set while reading)
    std::uint32_t serialNumber_ = 0;             // +0x50
    std::uint32_t occupantSerial_ = 0;           // +0x54
    int originalLanguage_ = 0, originalCountry_ = 0, lastLanguage_ = 0, lastCountry_ = 0;   // +0x58..+0x64
    std::uint32_t birthDate_ = 0;                // +0x68
    cRZString saveFilePath_;                     // +0x70
    cRZString cityName_, originalCityName_;      // +0xc0, +0xe8
    cRZString mayorName_, originalMayorName_;    // +0x110, +0x138
    cRZString description_, originalDescription_;   // +0x160, +0x188
    float worldLongitude_ = 122.06f;             // +0x1b0 (0x42f41eb8)
    float worldLatitude_ = 37.91f;               // +0x1b4 (0x4217a3d7)
    float worldBaseElevation_ = 46.0f;           // +0x1b8 (0x42380000)
    std::uint32_t saveDisableCount_ = 0;         // +0x1bc
    std::uint32_t uiLockCount_ = 0;              // +0x1c0
    float sizeX_ = 0, sizeZ_ = 0;                // +0x1c4, +0x1c8
    float cellWidthX_ = 16.0f, cellWidthZ_ = 16.0f;   // +0x1cc, +0x1d0
    float invCellWidthX_ = 0.0625f, invCellWidthZ_ = 0.0625f;   // +0x1d4, +0x1d8
    int cellCountX_ = 0, cellCountZ_ = 0;        // +0x1dc, +0x1e0
    bool saveThumbnailFromFile_ = true;          // +0x3b0 (also written as the "save enabled" byte)
    bool inCityTimeMode_ = true;
    cSCPropertyHolder properties_;               // +0x18 (the property holder base)
    std::array<cRZAutoRefCount<cIGZUnknown>, static_cast<std::size_t>(CityComponent::kCount)> components_;
    std::list<RetainedRecordData> retained_;     // +0x3b8
};

}  // namespace sc4
