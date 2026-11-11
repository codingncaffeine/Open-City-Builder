// The five hand-driven disaster managers and the helpers they need: cSC4DisasterManagerFire (22 functions at
// 0x10044f02e..0x100452a30), ...Volcano (14, 0x100456e28..0x100457244), ...Riot (14, 0x10044cb8a..0x10044d472),
// ...PipeBurst (11, 0x1004550e2..0x100455820), ...Earthquake (10, 0x100452be2..0x100452f88), cSC4WateredPipeFilter (4,
// 0x10045681a..0x1004568ba) and SpiralIterator (GetNext / DoXZEnd, 0x1004f45e0 / 0x1004fe1f0).
//
// Every manager keeps the instances of its disaster in the common list (cSC4DisasterManager) and forwards the
// disaster-specific verbs to the instance, creating one on demand (fire) or only when none runs (volcano, earthquake,
// pipe burst; riots allow a few). The instance classes themselves are not recovered yet: the managers reach them through
// the small instance interfaces below and the factory seam (IDisasterInstanceFactory).
// Quirks kept: fire's AttemptDisasterStart(x, z) never starts anything (it calls the no-argument form); a riot never reports the instance it starts.
#pragma once
#include <cstdint>
#include <set>

#include "rz/cRZRandom.h"
#include "sc4/Disaster.h"
#include "sc4/SpiralIterator.h"

namespace sc4 {

// SpiralIterator lives in sc4/SpiralIterator.h

// ---- instance views --------------------------------------------------------------------------------------------
constexpr std::uint32_t kGZIID_cISC4DisasterManagerFire = 0x49b43f57, kGZIID_cISC4DisasterInstanceFire = 0x49b43f58,
                        kGZIID_cISC4DisasterManagerVolcano = 0xa9b5ab9e, kGZIID_cISC4DisasterInstanceVolcano = 0x29b5ab88,
                        kGZIID_cISC4PipeOccupant = 0x49c05b9f;
constexpr std::uint32_t kOccupantTypeNetworkOccupant = 0xc772bf98;

// The fire instance's own interface (iid 0x49b43f58; Mac i386 slots 0xc..0x40 in this order).
class cISC4DisasterInstanceFire : public cISC4DisasterInstance {
public:
    virtual bool Init() = 0;               // reads the tuning, registers the instance with the fire manager
    virtual void Update() = 0;
    virtual bool Ignite(cISC4Occupant* occupant, bool force, std::uint32_t family, bool jump) = 0;
    virtual bool IgniteCell(std::uint32_t x, std::uint32_t z, bool roll, bool force, std::uint32_t family) = 0;
    virtual std::uint32_t ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) = 0;
    virtual bool QuickBurnCell(std::uint32_t x, std::uint32_t z) = 0;
    virtual bool IgniteCellRadius(std::uint32_t x, std::uint32_t z, std::uint32_t inner, std::uint32_t outer) = 0;
    virtual bool Extinguish(cISC4Occupant* target) = 0;
    virtual bool BurnDown(cISC4Occupant* target) = 0;
    virtual std::uint32_t SetCause(std::uint32_t cause) = 0;
    virtual std::uint32_t GetCause() = 0;
    virtual int GetFireCountInCellRect(const SC4CellRect& rect) = 0;
    virtual int GetFireCount() = 0;
};
class cISC4DisasterInstanceVolcano : public cISC4DisasterInstance {
public:
    virtual float GetLavaHeightByCityCell(int x, int z) = 0;   // view slot 6 (0x30)
    virtual void Erupt() = 0;                      // instance slot 0x98
    virtual void Prepare() = 0;                    // 0x60
    virtual void SetPhase(int phase) = 0;          // 0xa8
};
class cISC4DisasterInstanceRiot : public cISC4DisasterInstance {
public:
    virtual bool Init() = 0;
    virtual void AddRiotLeader(int x, int z, bool flag) = 0;
};
class cISC4DisasterInstancePipeBurst : public cISC4DisasterInstance {
public:
    virtual bool Init(cIGZUnknown* pipeOccupant) = 0;      // registers itself with the manager
};
class cISC4DisasterInstanceEarthquake : public cISC4DisasterInstance {
public:
    virtual void Init(int x, int z) = 0;
    virtual void BeginDeformation() = 0;           // counter = 0, phase = 2, StartDeformation()
};
// A network occupant as the riots see it: two calls on its state record.
class cISC4RiotNetworkOccupant : public cIGZUnknown {
public:
    virtual void ClearState(std::uint32_t bits) = 0;     // state record slot 0x60
    virtual void SetState(std::uint32_t bits) = 0;       // state record slot 0x68
};
// The plumbing grid as the pipe filter asks it.
class IPlumbingGrid : public cIGZUnknown {
public:
    virtual bool IsWatered(int x, int z) = 0;            // slot 0x58
};
// The network occupant interface the pipe filter reads (iid 0x49c05b9f): the cell it sits on.
class cISC4PipeOccupantView : public cIGZUnknown {
public:
    virtual void GetCell(std::uint32_t& x, std::uint32_t& z) = 0;   // slot 0x1d8
    // What the burst effect needs of a pipe (defaults answer "nothing").
    virtual void GetCenter(float* xyz) { xyz[0] = xyz[1] = xyz[2] = 0.0f; }   // slot 0x290
    virtual bool IsConnected(int direction, int kind) { (void)direction; (void)kind; return false; }   // slot 0x178
    virtual cISC4Occupant* AsOccupant() { return nullptr; }                  // the occupant sub-object (+8)
};

// What the managers need that is not in IDisasterHost.
class IDisasterInstanceFactory {
public:
    virtual ~IDisasterInstanceFactory() = default;
    // Every Create* answers a fresh object with no references taken yet.
    virtual cISC4DisasterInstanceFire* CreateFire() = 0;
    virtual cISC4DisasterInstanceVolcano* CreateVolcano() = 0;
    virtual cISC4DisasterInstanceRiot* CreateRiot() = 0;
    virtual cISC4DisasterInstancePipeBurst* CreatePipeBurst() = 0;
    virtual cISC4DisasterInstanceEarthquake* CreateEarthquake() = 0;
    virtual IPlumbingGrid* PlumbingGrid() = 0;                           // city vslot 0x200 (counted reference)
    virtual bool ReadRiotTuning(int& maxInstances, int& maxLeaders) = 0;   // exemplar 0x29b6c670 / 0x4a38206d
};
IDisasterInstanceFactory* DisasterInstanceFactory();
void SetDisasterInstanceFactory(IDisasterInstanceFactory* factory);

// ---- occupant filter: pipes that carry water -------------------------------------------------------------------
class cSC4WateredPipeFilter : public cSC4OccupantFilter {
public:
    explicit cSC4WateredPipeFilter(IPlumbingGrid* grid) : grid_(grid) { if (grid_) grid_->AddRef(); }
    ~cSC4WateredPipeFilter() override;                         // @ 0x10045681a
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;   // @ 0x1004568ba
    void SetGrid(IPlumbingGrid* grid);
    IPlumbingGrid* Grid() const { return grid_; }

private:
    IPlumbingGrid* grid_ = nullptr;   // +0x18
};

// ---- fire --------------------------------------------------------------------------------------------------------
class cISC4DisasterManagerFire : public cIGZUnknown {
public:
    virtual cISC4DisasterInstanceFire* GetFireInstance() = 0;
    virtual std::uint32_t SetCause(std::uint32_t cause) = 0;
    virtual std::uint32_t GetCause() = 0;
    virtual bool IgniteOccupant(cISC4Occupant* occupant) = 0;
    virtual bool IgniteCell(std::uint32_t x, std::uint32_t z, bool a, bool b, std::uint32_t c) = 0;
    virtual std::uint32_t ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) = 0;
    virtual bool QuickBurnCell(std::uint32_t x, std::uint32_t z) = 0;
    virtual bool IgniteCellRadius(std::uint32_t x, std::uint32_t z, std::uint32_t radius, std::uint32_t c) = 0;
};

class cSC4DisasterManagerFire final : public cSC4DisasterManager, public cISC4DisasterManagerFire {
public:
    cSC4DisasterManagerFire() { spInstance_ = this; }
    ~cSC4DisasterManagerFire() override { spInstance_ = nullptr; }   // @ 0x10044f02e
    static cSC4DisasterManagerFire* Instance() { return spInstance_; }

    bool QueryInterface(std::uint32_t iid, void** out) override;    // @ 0x10044f18a
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override { return cSC4DisasterManager::Release(); }
    std::uint32_t GetID() const override { return kDisasterManagerIdFire; }             // @ 0x100452a24
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4DisasterManagerFire; }       // @ 0x100452a30
    cISC4DisasterManager* AsISC4DisasterManager() { return this; }                      // @ 0x10044f236
    bool Init() override;                                                               // @ 0x10044f63e
    bool Shutdown() override { return cSC4DisasterManager::Shutdown(); }                // @ 0x10044f68a
    bool OnStandardMessage(cIGZMessage2Standard*) override { return true; }            // @ 0x10044f636
    bool AttemptDisasterStart() override { return false; }                              // @ 0x10044f5ee
    bool AttemptDisasterStart(int x, int z) override;                                   // @ 0x10044f5fe

    cISC4DisasterInstanceFire* GetFireInstance() override;           // @ 0x10044f24a: a new reference to the running one
    cISC4DisasterInstanceFire* FireInstance();                       // @ 0x10044f2be: the running one, or a fresh one
    std::uint32_t SetCause(std::uint32_t cause) override;            // @ 0x10044f32a
    std::uint32_t GetCause() override;                               // @ 0x10044f374
    bool IgniteOccupant(cISC4Occupant* occupant) override;           // @ 0x10044f3b8
    bool IgniteCell(std::uint32_t x, std::uint32_t z, bool a, bool b, std::uint32_t c) override;   // @ 0x10044f40c
    std::uint32_t ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) override;           // @ 0x10044f486
    bool QuickBurnCell(std::uint32_t x, std::uint32_t z) override;   // @ 0x10044f532
    bool IgniteCellRadius(std::uint32_t x, std::uint32_t z, std::uint32_t radius, std::uint32_t c) override;   // @ 0x10044f586

private:
    static inline cSC4DisasterManagerFire* spInstance_ = nullptr;
};

// ---- volcano -----------------------------------------------------------------------------------------------------
class cISC4DisasterManagerVolcano : public cIGZUnknown {
public:
    virtual float GetLavaHeightByCityCell(int x, int z) = 0;
};

class cSC4DisasterManagerVolcano final : public cSC4DisasterManager, public cISC4DisasterManagerVolcano {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;    // @ 0x100457164
    std::uint32_t AddRef() override { return ++refCount_; }         // @ 0x100456e28
    std::uint32_t Release() override { return cSC4DisasterManager::Release(); }   // @ 0x100457210
    std::uint32_t GetID() const override { return kDisasterManagerIdVolcano; }    // @ 0x10045713a
    cISC4DisasterManager* AsISC4DisasterManager() { return this; }                // @ 0x100456e74
    bool Init() override { return cSC4DisasterManager::Init(); }                   // @ 0x100456e5c
    bool Shutdown() override { return cSC4DisasterManager::Shutdown(); }          // @ 0x100457122
    bool OnStandardMessage(cIGZMessage2Standard* message) override;                // @ 0x100457152
    bool AttemptDisasterStart() override;                                          // @ 0x100456e88
    bool AttemptDisasterStart(int x, int z) override;                              // @ 0x100456f12
    bool CreateVolcanoInstance(cISC4DisasterInstanceVolcano** out);                // @ 0x100456fb8
    float GetLavaHeightByCityCell(int x, int z) override;                          // @ 0x100457244: first instance, else 0
};

// ---- riots -------------------------------------------------------------------------------------------------------
class cSC4DisasterManagerRiot final : public cSC4DisasterManager {
public:
    std::uint32_t GetID() const override { return kDisasterManagerIdRiot; }       // @ 0x10044d14e
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4DisasterManagerRiot; } // @ 0x10044d142
    cISC4DisasterManager* AsISC4DisasterManager() { return this; }                // @ 0x10044cb8a
    bool Init() override;                                                          // @ 0x10044d166
    bool Shutdown() override;                                                      // @ 0x10044d234
    bool OnStandardMessage(cIGZMessage2Standard*) override { return true; }        // @ 0x10044d472
    bool AttemptDisasterStart() override { return false; }                         // @ 0x10044cb94
    bool AttemptDisasterStart(int x, int z) override;                              // @ 0x10044cba4
    bool RemoveInstanceFromList(cISC4DisasterInstance* instance) override;         // @ 0x10044d43a
    void ReadTuningParameters();                                                   // @ 0x10044d186
    void ClearRiotNetworkOccupants();                                              // @ 0x10044d25a
    void AddRiotNetworkOccupant(cISC4RiotNetworkOccupant* occupant);               // @ 0x10044d324

    static inline int snRiotMaxInstances = 3, snRiotMaxLeaders = 5;

private:
    std::set<cISC4RiotNetworkOccupant*> riotOccupants_;   // +0x78
};

// ---- pipe bursts -------------------------------------------------------------------------------------------------
class cSC4DisasterManagerPipeBurst final : public cSC4DisasterManager {
public:
    std::uint32_t GetID() const override { return kDisasterManagerIdPipeBurst; }      // @ 0x1004556e0
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4DisasterManagerPipeBurst; }   // @ 0x1004556d4
    cISC4DisasterManager* AsISC4DisasterManager() { return this; }                     // @ 0x1004550e2
    bool Init() override;                                                              // @ 0x1004556f8
    bool Shutdown() override;                                                          // @ 0x1004557b6
    bool OnStandardMessage(cIGZMessage2Standard*) override { return true; }            // @ 0x100455820
    bool AttemptDisasterStart() override;                                              // @ 0x1004550ec
    bool AttemptDisasterStart(int x, int z) override;                                  // @ 0x100455388
    cISC4DisasterInstance* CreateDisasterInstance(cISC4Occupant* occupant);            // @ 0x10045527e (the instance registers itself)
    cSC4WateredPipeFilter* Filter() const { return filter_; }

private:
    cSC4WateredPipeFilter* filter_ = nullptr;   // +0x78
};

// ---- earthquakes -------------------------------------------------------------------------------------------------
class cSC4DisasterManagerEarthquake final : public cSC4DisasterManager {
public:
    std::uint32_t GetID() const override { return kDisasterManagerIdEarthquake; }      // @ 0x100452f46
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4DisasterManagerEarthquake; }   // @ 0x100452f3a
    cISC4DisasterManager* AsISC4DisasterManager() { return this; }                     // @ 0x100452be2
    bool Init() override { return cSC4DisasterManager::Init(); }                       // @ 0x100452f5e
    bool Shutdown() override { return cSC4DisasterManager::Shutdown(); }               // @ 0x100452f7e
    bool OnStandardMessage(cIGZMessage2Standard*) override { return true; }            // @ 0x100452f76
    bool AttemptDisasterStart() override { return false; }                             // @ 0x100452bec
    bool AttemptDisasterStart(int x, int z) override;                                  // @ 0x100452bfc
};

}  // namespace sc4
