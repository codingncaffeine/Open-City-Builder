// Disasters: the disaster layer (cSC4DisasterLayer, 35 functions at 0x100461a62..0x1004639fc, iid 0x29b02a2c) is
// the city component that owns one disaster manager per kind, keeps the registry of disaster locations the camera
// can jump to, assesses the damage (occupants removed while a disaster is running) and times the fire /
// police dispatches. cSC4DisasterManager (28 functions at 0x10046349e..0x100463976, iid 0x21e9cbf) is the common
// base of the per-kind managers (Effect 0x44c174.., Toxic 0x6fc2f4..); every manager owns a list of live
// instances (cISC4DisasterInstance, iid 0x22223ba4) and creates them on demand.
//
// All three object families are cRZUnknown-style multiple-inheritance objects: +0x00 the concrete class,
// +0x10 the cISC4Disaster* interface, +0x18 the message target (iid 0x452294aa).
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gz/CommandParameterSet.h"
#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"
#include "sc4/OccupantManager.h"
#include "sc4/ToxicOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4DisasterManager = 0x021e9cbf;
constexpr std::uint32_t kGZIID_cISC4DisasterInstance = 0x22223ba4;
constexpr std::uint32_t kGZIID_cISC4DisasterLayer = 0x29b02a2c;
constexpr std::uint32_t kGZIID_cISC4DisasterManagerEffect = 0xca7b3f3c;
constexpr std::uint32_t kGZIID_cISC4DisasterManagerToxic = 0x4c1e8345;
constexpr std::uint32_t kGZIID_cSC4DisasterInstanceToxic = 0xebfb0403;

// manager ids (also the key in the layer's manager map) and their GZCOM class ids
constexpr std::uint32_t kDisasterManagerIdVolcano = 0xc4c90997;
constexpr std::uint32_t kDisasterManagerIdFire = 0x621cda33;
constexpr std::uint32_t kDisasterManagerIdEarthquake = 0x42963812;
constexpr std::uint32_t kDisasterManagerIdPipeBurst = 0x4a1332e8;
constexpr std::uint32_t kDisasterManagerIdRiot = 0xca380b06;
constexpr std::uint32_t kDisasterManagerIdEffect = 0x6a7b4397;
constexpr std::uint32_t kDisasterManagerIdToxic = 0xebfb0404;
constexpr std::uint32_t kGZCLSID_cSC4DisasterManagerVolcano = 0xa9b5ab9f;
constexpr std::uint32_t kGZCLSID_cSC4DisasterManagerFire = 0x69c1718e;
constexpr std::uint32_t kGZCLSID_cSC4DisasterManagerEarthquake = 0x0a086bd3;
constexpr std::uint32_t kGZCLSID_cSC4DisasterManagerPipeBurst = 0x4a132fdf;
constexpr std::uint32_t kGZCLSID_cSC4DisasterManagerRiot = 0x4a38206c;
constexpr std::uint32_t kGZCLSID_cSC4DisasterManagerEffect = 0x6a7b3e9b;
constexpr std::uint32_t kGZCLSID_cSC4DisasterManagerToxic = 0xebfb0402;

// messages
constexpr std::uint32_t kMessageDisasterBegan = 0xca1b1e64;
constexpr std::uint32_t kMessageDisasterEnded = 0xaa1b2002;
constexpr std::uint32_t kMessageDispatchBegin = 0xe94671ec;     // starts a dispatch timer
constexpr std::uint32_t kMessageDispatchStop1 = 0xe94671eb;     // stop it
constexpr std::uint32_t kMessageDispatchStop2 = 0xe94671ed;
constexpr std::uint32_t kMessageDispatchStop3 = 0x894671f6;
constexpr std::uint32_t kMessageEffectFinished = 0x2a7b582d;     // visual effect completed (data1 = instance)
constexpr std::uint32_t kMessageEffectMoved = 0x2a7b582e;        // data1 = instance, data2/data3 = cell
constexpr std::uint32_t kMessageToxicSpill = 0x0bfb41ba;         // the "ToxicSpill" command
constexpr std::uint32_t kMessageToxicTick = 0x86ad10ee;          // game tick that updates the spill (-0x7952ef12)

constexpr std::uint32_t kPropertyDamageCost = 0x8a1c3e72;        // int64 exemplar property
constexpr std::int64_t kDamageCostPerNetworkTile = 10;           // cSC4DisasterLayer::kDamageCostPerNetworkTile
constexpr std::uint32_t kDisasterOccupantTypeNetwork = 0xc772bf98;

class cISC4DisasterInstance;
class cISC4DisasterManager;

// cISC4VisualEffect: only the calls the disasters make (slot offsets of the original in comments).
class cISC4VisualEffect : public cIGZUnknown {
public:
    virtual void Start(int flags) = 0;                                          // +0x18
    virtual void Stop(int flags) = 0;                                           // +0x20
    virtual bool IsPlaying() = 0;                                               // +0x28
    virtual void SetCompletionMessage(std::uint32_t message, std::uint32_t userValue) = 0;   // +0x50
    virtual void SetIntParameter(int parameter, const void* data, int count) = 0;            // +0x60
    virtual void SetPointerParameter(int parameter, const void* data, int count) = 0;        // +0x68
    // +0x40: places the effect (3x3 rotation, translation, scale: 13 floats).
    virtual void SetTransform(const float* matrix) { (void)matrix; }
};

class cISC4DisasterInstance : public cIGZUnknown {
public:
    virtual bool Shutdown() = 0;                                  // group-1 slot 0x18
    virtual cISC4DisasterManager* GetManager() const = 0;         // 0x20
    virtual std::uint32_t GetID() const = 0;                      // 0x28
    virtual bool GetLocation(int& x, int& z) const = 0;           // 0x30
    virtual int GetPhase() const = 0;                             // 0x38
};

class cISC4DisasterManager : public cIGZUnknown {
public:
    virtual bool Init() = 0;
    virtual bool Shutdown() = 0;
    virtual bool AddInstanceToList(cISC4DisasterInstance* instance) = 0;
    virtual bool RemoveInstanceFromList(cISC4DisasterInstance* instance) = 0;
    virtual int GetActiveInstanceCount() const = 0;
    virtual const std::list<cISC4DisasterInstance*>& GetInstances() const = 0;
    virtual std::uint32_t GetID() const = 0;
    virtual bool AttemptDisasterStart() = 0;
    virtual bool AttemptDisasterStart(int cellX, int cellZ) = 0;
};

// Aggregate of what a disaster has cost so far, by building category (cSC4DisasterLayer +0x78..+0xc0).
struct DisasterCost {
    // counts / costs of removed buildings by occupant category bit (BuildingProfile::categoryFlags bits 0, 1, 2, 5)
    std::int32_t buildingsDestroyed = 0;      // +0x78
    std::int64_t totalCost = 0;               // +0x80  (every removed occupant, buildings and network tiles)
    std::int32_t category0Count = 0;          // +0x88
    std::int64_t category0Cost = 0;           // +0x90
    std::int32_t category1Count = 0;          // +0x98
    std::int64_t category1Cost = 0;           // +0xa0
    std::int32_t category2Count = 0;          // +0xa8
    std::int64_t category2Cost = 0;           // +0xb0
    std::int32_t category5Count = 0;          // +0xb8
    std::int64_t category5Cost = 0;           // +0xc0
};
struct SpecialBuildingDamage {                // cISC4DisasterLayer::tSpecialBldgInfo, 0x30 bytes
    cRZString name;
    std::int64_t cost = 0;
};

class cISC4DisasterLayer : public cIGZUnknown {
public:
    virtual bool Init() = 0;
    virtual bool Shutdown() = 0;
    virtual void EnableDisasters(bool enable) = 0;
    virtual bool DisastersEnabled() const = 0;
    virtual cISC4DisasterManager* GetDisasterManager(std::uint32_t id) const = 0;
    virtual void DisasterHasBegun(cISC4DisasterInstance* instance) = 0;
    virtual void DisasterHasEnded() = 0;
    virtual bool IsADisasterOngoing() const = 0;
    virtual bool GoToNextDisaster() = 0;
    virtual bool GetMostRecentDisasterLocation(cISC4DisasterInstance* instance, int* x, int* z) = 0;
    virtual bool RegisterDisasterLocation(cISC4DisasterInstance* instance, int x, int z, cISC4Occupant* occupant,
                                          int zoom, bool jump) = 0;
    virtual bool UnregisterDisasterLocations(cISC4DisasterInstance* instance) = 0;
    virtual bool UnregisterDisasterLocationByOccupant(cISC4DisasterInstance* instance, cISC4Occupant* occupant) = 0;
    virtual int GetDisasterLocationCount() const = 0;
    virtual bool GetDisasterLocationByIndex(int index, cISC4DisasterInstance** instance, int* x, int* z) const = 0;
    virtual DisasterCost* GetDisasterCost() = 0;
};

// Services the disaster classes reach through the SL::sp* globals and the city.
class IDisasterHost {
public:
    virtual ~IDisasterHost() = default;
    virtual cIGZMessageServer2* MessageServer() { return nullptr; }              // SL::spMS2 (null when absent)
    virtual cISC4DisasterLayer* DisasterLayer() = 0;                             // city vslot 0x240
    virtual cISC4OccupantManager* Occupants() = 0;                               // SL::spOccupantManager
    // occupant manager vslot FindOccupant: is the occupant still in the city (a disaster location follows it while it is).
    // A host whose automata live outside the occupant manager answers for them too.
    virtual bool IsOccupantInCity(cISC4Occupant* occupant);
    virtual bool CityEstablished() { return true; }                              // city vslot 0xb0
    virtual void EmergencyPause() {}                                             // simulator vslot 0x58
    virtual void EmergencyResume() {}                                            // simulator vslot 0x70
    virtual void DisableSave() {}                                                // city vslot 0x340
    virtual void EnableSave() {}                                                 // city vslot 0x338
    virtual std::uint32_t FireDispatchCount() { return 0; }                      // fire protection simulator vslot 200
    virtual std::uint32_t PoliceDispatchCount() { return 0; }                    // police simulator vslot 0xd0
    virtual bool PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;   // city vslot 0x2f8
    virtual bool CellCenterToPosition(int cellX, int cellZ, float& x, float& z) = 0;   // city vslot 0x308
    virtual float TerrainHeight(float x, float z) = 0;                           // terrain vslot 0x60
    virtual int CityCellCountX() = 0;                                            // city vslot 0x2e8
    virtual int CityCellCountZ() = 0;                                            // city vslot 0x2f0
    virtual void MoveCamera(float /*x*/, float /*y*/, float /*z*/, int /*zoomLevel*/) {}         // camera control, if zoomLevel > 0
    virtual bool AutoJumpToDisasters() { return false; }                         // preferences +0xf08
    virtual cISC4DisasterManager* CreateManager(std::uint32_t classId) = 0;      // GZCOM CreateInstance
    virtual cIGZMessageServer2* NotificationServer() { return MessageServer(); } // SL::ATM()
    virtual void RegisterCommand(std::uint32_t /*id*/, const char* /*name*/) {}          // command server vslot 0x30
    virtual void UnregisterCommand(std::uint32_t /*id*/) {}                          // command server vslot 0x48
};
IDisasterHost* DisasterHost();
void SetDisasterHost(IDisasterHost* host);

// ------------------------------------------------------------------------------------------ the layer

class cSC4DisasterLayer final : public cISC4DisasterLayer, public cIGZMessageTarget2 {
public:
    cSC4DisasterLayer();
    ~cSC4DisasterLayer();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Init() override;
    bool Shutdown() override;
    void EnableDisasters(bool enable) override;
    bool DisastersEnabled() const override { return disableCount_ == 0; }
    cISC4DisasterManager* GetDisasterManager(std::uint32_t id) const override;
    void DisasterHasBegun(cISC4DisasterInstance* instance) override;
    void DisasterHasEnded() override;
    bool IsADisasterOngoing() const override { return ongoing_ != 0; }
    bool GoToNextDisaster() override;
    bool GetMostRecentDisasterLocation(cISC4DisasterInstance* instance, int* x, int* z) override;
    bool RegisterDisasterLocation(cISC4DisasterInstance* instance, int x, int z, cISC4Occupant* occupant, int zoom,
                                  bool jump) override;
    bool UnregisterDisasterLocations(cISC4DisasterInstance* instance) override;
    bool UnregisterDisasterLocationByOccupant(cISC4DisasterInstance* instance, cISC4Occupant* occupant) override;
    int GetDisasterLocationCount() const override { return static_cast<int>(locations_.size()); }
    bool GetDisasterLocationByIndex(int index, cISC4DisasterInstance** instance, int* x, int* z) const override;
    DisasterCost* GetDisasterCost() override { return &cost_; }
    bool DoMessage(cIGZMessage2* message) override;

    const std::vector<SpecialBuildingDamage>& SpecialBuildings() const { return specialBuildings_; }
    float FireDispatchRatio() const { return fireDispatchRatio_; }       // +0xe0
    float PoliceDispatchRatio() const { return policeDispatchRatio_; }   // +0xe4

    struct Location {                         // tDisasterLocation, 0x20 bytes
        cISC4DisasterInstance* instance = nullptr;
        std::int32_t x = 0, z = 0;
        std::int32_t zoom = 0;
        cISC4Occupant* occupant = nullptr;    // reference held
    };

private:
    void StartDamageAssessment();
    void EndDamageAssessment();
    void PostMessageToServer(std::uint32_t type, std::uint64_t a, std::uint64_t b, std::uint64_t c);
    void ResetDisasterCost();
    void DoOnOccupantRemoval(cIGZMessage2Standard* message);
    void DoDispatchMessage(cIGZMessage2Standard* message);
    void ClearDispatchTimers();
    void StartDispatchTimers();
    void EndDispatchTimers();
    bool GetNextDisasterLocation(int* x, int* z, int* zoom);
    void ResolveDisasterLocation(Location& location);

    std::uint32_t refCount_ = 0;
    std::map<std::uint32_t, cRZTimer*> fireTimers_;            // +0x20
    std::map<std::uint32_t, cRZTimer*> policeTimers_;          // +0x38
    cRZTimer totalTimer_{cRZTimer::Units(4)};                  // +0x50
    std::int32_t disableCount_ = 0;                            // +0x68
    bool initialised_ = false;                                 // +0x6c
    std::int32_t ongoing_ = 0;                                 // +0x70
    DisasterCost cost_;
    std::vector<SpecialBuildingDamage> specialBuildings_;      // +0xc8
    float fireDispatchRatio_ = 0.0f, policeDispatchRatio_ = 0.0f;     // +0xe0, +0xe4
    std::map<std::uint32_t, cISC4DisasterManager*> managers_;  // +0xe8
    std::int32_t currentLocation_ = 0;                         // +0x104
    std::vector<Location> locations_;                          // +0x108
};

// ------------------------------------------------------------------------------------- manager base

class cSC4DisasterManager : public cISC4DisasterManager, public cIGZMessageTarget2 {
public:
    cSC4DisasterManager() = default;
    virtual ~cSC4DisasterManager() = default;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Init() override;
    bool Shutdown() override;
    bool AddInstanceToList(cISC4DisasterInstance* instance) override;
    bool RemoveInstanceFromList(cISC4DisasterInstance* instance) override;
    int GetActiveInstanceCount() const override;
    const std::list<cISC4DisasterInstance*>& GetInstances() const override { return instances_; }
    bool AttemptDisasterStart() override { return false; }
    bool AttemptDisasterStart(int, int) override { return false; }
    bool DoMessage(cIGZMessage2* message) override;
    virtual bool OnStandardMessage(cIGZMessage2Standard* /*message*/) { return true; }   // slot 0x70

protected:
    void ProcessTempAddRemoveLists();

    std::uint32_t refCount_ = 0;
    bool initialised_ = false;                                 // +0x20
    std::list<cISC4DisasterInstance*> instances_;              // +0x28 (a reference each)
    bool iterating_ = false;                                   // +0x40 (set while shutting the list down)
    std::list<cISC4DisasterInstance*> pendingAdd_;             // +0x48
    std::list<cISC4DisasterInstance*> pendingRemove_;          // +0x60
};

// ------------------------------------------------------------------------------- effect disasters

class cISC4DisasterManagerEffect : public cIGZUnknown {
public:
    virtual cISC4DisasterInstance* StartDisasterEffect(cISC4VisualEffect* effect, const cS3DVector3& position,
                                                       std::uint32_t id) = 0;
    virtual void AttractDisasterEffect(const float* position) = 0;
    virtual void CancelAttract() = 0;
};

class cSC4DisasterInstanceEffect final : public cISC4DisasterInstance, public cIGZMessageTarget2 {
public:
    cSC4DisasterInstanceEffect(cISC4VisualEffect* effect, std::uint32_t id);
    ~cSC4DisasterInstanceEffect();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Init();
    bool Shutdown() override;
    cISC4DisasterManager* GetManager() const override { return manager_; }
    std::uint32_t GetID() const override { return id_; }
    bool GetLocation(int& x, int& z) const override;
    int GetPhase() const override { return 1; }
    bool DoMessage(cIGZMessage2* message) override;

    void SetLocation(const float* position);       // vslot 0x70
    void CancelAttract();                          // vslot 0x78
    void Stop() { Shutdown(); }                    // vslot 0x80

private:
    std::uint32_t refCount_ = 0;
    std::int32_t cellX_ = 0, cellZ_ = 0;           // +0x20, +0x24
    bool initialised_ = false;                     // +0x28
    std::uint32_t id_ = 0;                         // +0x2c
    cISC4DisasterLayer* layer_ = nullptr;          // +0x30
    cISC4DisasterManager* manager_ = nullptr;      // +0x38
    cISC4VisualEffect* effect_ = nullptr;          // +0x40
};

class cSC4DisasterManagerEffect final : public cSC4DisasterManager, public cISC4DisasterManagerEffect {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override { return cSC4DisasterManager::Release(); }
    std::uint32_t GetID() const override { return kDisasterManagerIdEffect; }
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4DisasterManagerEffect; }
    bool Init() override { return cSC4DisasterManager::Init(); }
    bool Shutdown() override { return cSC4DisasterManager::Shutdown(); }
    bool OnStandardMessage(cIGZMessage2Standard*) override { return true; }

    cISC4DisasterInstance* StartDisasterEffect(cISC4VisualEffect* effect, const cS3DVector3& position,
                                               std::uint32_t id) override;
    void AttractDisasterEffect(const float* position) override;
    void CancelAttract() override;
};

// -------------------------------------------------------------------------------- toxic disasters

class cSC4DisasterInstanceToxic final : public cISC4DisasterInstance, public cIGZMessageTarget2 {
public:
    cSC4DisasterInstanceToxic() = default;
    ~cSC4DisasterInstanceToxic();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Init();
    bool Shutdown() override;
    cISC4DisasterManager* GetManager() const override { return manager_; }
    std::uint32_t GetID() const override { return kDisasterManagerIdToxic; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return 1; }
    bool DoMessage(cIGZMessage2* message) override;

    bool SpillToxicWaste(int cellX, int cellZ, int amount);
    int GetToxicCountInCellRect(const SC4CellRect& rect) const;
    int GetToxicCount() const { return static_cast<int>(waste_.size()); }
    void Update();

private:
    std::uint32_t refCount_ = 0;
    bool initialised_ = false;                       // +0x20
    cISC4DisasterLayer* layer_ = nullptr;            // +0x28
    cISC4DisasterManager* manager_ = nullptr;        // +0x30
    cRZTimer timer_{cRZTimer::Units(4)};             // +0x38
    std::uint32_t nextUpdate_ = 0;                   // +0x50
    std::list<cSC4ToxicOccupant*> waste_;            // +0x58 (a reference each)
};

class cISC4DisasterManagerToxic : public cIGZUnknown {
public:
    virtual int GetToxicCountInCellRect(const SC4CellRect& rect) = 0;
    virtual int GetToxicCount() = 0;
};

class cSC4DisasterManagerToxic final : public cSC4DisasterManager, public cISC4DisasterManagerToxic {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override { return cSC4DisasterManager::Release(); }
    std::uint32_t GetID() const override { return kDisasterManagerIdToxic; }
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4DisasterManagerToxic; }
    bool Init() override;
    bool Shutdown() override;
    bool OnStandardMessage(cIGZMessage2Standard* message) override;
    bool AttemptDisasterStart() override { return false; }
    bool AttemptDisasterStart(int cellX, int cellZ) override;
    int GetToxicCountInCellRect(const SC4CellRect& rect) override;
    int GetToxicCount() override;
    cSC4DisasterInstanceToxic* GetInstance();      // returns one new reference

private:
};

}  // namespace sc4
