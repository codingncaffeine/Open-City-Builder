// cSC4PropManager — creates props from prop exemplars and tracks the city / lot / street / building prop
// sets (69 functions, 0x100193866..0x100196074). Prop exemplar data (type -> exemplar key, profile, name,
// family membership) lives in process-wide tables filled once from the resource manager (exemplars whose
// exemplar type 0x10 is 0x1e). Props are classified by "usage": 1 city, 2 lot, 3 building, 4 street; the
// occupant manager keeps them and the manager filters by usage (filter field +0x14).
// Layout: +0x08 message target vptr, +0x10 serializable vptr, +0x18 initialised, +0x1c refcount,
// +0x20 occupant manager, +0x28 resource manager, +0x30 usage filter, +0x38 lot manager, +0x40 3D view.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "rz/cRZString.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/PropOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4PropManager = 0x28326f3e;
constexpr std::uint32_t kGZCLSID_cSC4PropManager_Prop = 0xa990bd10;
constexpr std::uint32_t kMessagePropViewFilterChanged = 0x06cdb65b;
constexpr std::uint32_t kMessagePropCityInit = 0x26d31ec1;
constexpr std::uint32_t kMessagePropTextDecal = 0x0beaf1a2;
constexpr std::uint32_t kMessagePropLoadedInfo = 0xa6b79602;
constexpr std::uint32_t kMessagePropExemplarDataUpdated = 0x896fdc70;
constexpr std::uint32_t kPropExemplarType = 0x1e;                      // exemplar type property 0x10 of props
constexpr std::uint32_t kPropClassDefault = 0x2977aa47;
constexpr std::uint32_t kPropClassTerrainDecal = 0x2977aa48;
constexpr std::uint32_t kPropClassVisualEffect = 0x2977aa49;
constexpr std::uint32_t kPropertyBuildingProps = 0xa8322b7f;           // property on a building holding its props

// Property ids of a prop exemplar.
constexpr std::uint32_t kPropPropertyClass = 0xa977a86b;
constexpr std::uint32_t kPropPropertySize = 0x27812810;
constexpr std::uint32_t kPropPropertyFamilies = 0x27812870;
constexpr std::uint32_t kPropPropertyA = 0x49a1e05a;
constexpr std::uint32_t kPropPropertyFlag = 0xe9a316eb;
constexpr std::uint32_t kPropPropertyC = 0x8aa864d5;

// cISC4PropOccupant::PropProfile (0x1c bytes).
struct PropProfile {
    std::uint32_t propClass = kPropClassDefault;   // +0 occupant class to create
    float size[3] = {};                            // +4 prop footprint from property 0x27812810
    std::uint32_t valueA = 0;                      // +0x10 property 0x49a1e05a
    std::uint8_t flag = 0;                         // +0x14 property 0xe9a316eb
    std::uint32_t valueC = 0;                      // +0x18 property 0x8aa864d5
};

// The list a building keeps its props in (stored as property 0xa8322b7f of the building).
class cSC4BuildingPropList final : public cIGZUnknown {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs <= 1) { delete this; return 0; }
        return --refs;
    }
    std::vector<cISC4Occupant*> props;
    std::uint32_t refs = 1;
};

// cSC4ViewFilterInformation as DoPostViewFilterChange sees it: its first field is a counted filter object.
class IPropViewFilter {
public:
    virtual ~IPropViewFilter() = default;
    virtual std::uint32_t AddRef() = 0;       // vslot 0x08
    virtual std::uint32_t Release() = 0;      // vslot 0x10
};
struct PropViewFilterInformation {
    IPropViewFilter* filter = nullptr;
};

class IPropManagerHost {
public:
    virtual ~IPropManagerHost() = default;
    // Resource manager: every prop-capable exemplar key (enumerator 0x6534284a) and its property holder.
    virtual void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) = 0;
    virtual cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key) = 0;   // borrowed
    virtual void ReleaseExemplar(cISCPropertyHolder* holder) = 0;
    // The exemplar becomes the prop's default exemplar (its holder's 0x0ac2b5f7 view, vslot 0xc): the prop answers every exemplar
    // property it does not set itself (occupant groups, demolition effect, ...).
    virtual void SetDefaultExemplar(cSC4PropOccupant* prop, cISCPropertyHolder* exemplar) { (void)prop; (void)exemplar; }
    // A registered occupant class (GZCOM class factory) for a custom prop class id, or null.
    virtual cSC4PropOccupant* CreateRegisteredProp(std::uint32_t classId) = 0;
    // Terrain-decal / visual-effect props are separate classes in the original.
    virtual cSC4PropOccupant* CreateTerrainDecalProp() = 0;
    virtual cSC4PropOccupant* CreateVisualEffectProp() = 0;
    virtual void PostPropExemplarDataUpdated() = 0;                    // message 0x896fdc70
    // city: world position <-> city cell
    virtual void PositionToCityCell(float x, float z, int& cx, int& cz) = 0;     // vslot 0x2f8
    virtual void CityCellToPosition(int cx, int cz, float& x, float& z) = 0;     // vslot 0x300
    virtual cSC4Lot* LotAtCell(int x, int z) = 0;                       // lot manager vslot 0x28
    virtual void CreateTextDecal(const std::vector<std::uint32_t>& fields) = 0;  // message 0xbeaf1a2
};
IPropManagerHost* PropManagerHost();
void SetPropManagerHost(IPropManagerHost* host);

class cSC4PropManager {
public:
    cSC4PropManager() = default;
    ~cSC4PropManager() = default;

    std::uint32_t AddRef() { return ++refCount_; }
    std::uint32_t Release();
    bool QueryInterface(std::uint32_t iid, void** out);
    bool Init();
    bool Shutdown();
    bool DoMessage(std::uint32_t messageType);
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4PropManager_Prop; }
    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);

    // exemplar data
    void GetAllPropTypes(std::vector<std::uint32_t>& out) const;
    void GetAllPropFamilyTypes(std::vector<std::uint32_t>& out) const;
    bool GetPropKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) const;
    bool IsPropTypeValid(std::uint32_t type) const;
    int GetPropTypesByFamily(std::vector<std::uint32_t>& out, std::uint32_t family) const;
    bool IsPropFamilyTypeValid(std::uint32_t type) const;
    bool IsPropInFamily(std::uint32_t type, std::uint32_t family) const;
    cRZString& GetPropName(std::uint32_t type) const;
    PropProfile& GetPropProfile(std::uint32_t type) const;
    const PropProfile* GetPropProfilePtr(std::uint32_t type) const;
    bool AddPropExemplarData(const cGZPersistResourceKey& key, cISCPropertyHolder* exemplar);
    void LoadPropProperties(cISCPropertyHolder* exemplar, std::uint32_t type, bool firstLoad);
    bool LoadPropProperties(cSC4PropOccupant* prop, std::uint32_t type);

    // creation
    bool CreateProp(std::uint32_t type, cSC4PropOccupant*& out);
    bool CreateGenericProp(const cGZPersistResourceKey& key, cSC4PropOccupant*& out);

    // city / lot / street / building props
    bool GetCityProps(OccupantList& out, const int rect[4]);                  // x0,z0,x1,z1 cells
    bool GetCityProps(OccupantList& out, const float rect[4]);
    bool AddCityProp(std::uint32_t type, const float* position, int orientation);
    bool AddCityProp(cISC4Occupant* occupant);
    bool RemoveCityProps(const int rect[4]);
    bool RemoveCityProps(const float rect[4]);
    bool GetStreetProps(OccupantList& out, int x, int z);
    bool GetStreetProps(OccupantList& out, const int rect[4]);
    bool AddStreetProp(std::uint32_t type, const float* position, int orientation);
    void RemoveStreetPropsFromTile(int x, int z);
    bool GetLotProps(OccupantList& out, cSC4Lot* lot);
    bool GetLotProps(OccupantList& out, int x, int z);
    cSC4PropOccupant* AddLotProp(std::uint32_t type, const float* position, int orientation);
    bool AddLotProp(cSC4Lot* lot, cISC4Occupant* occupant);
    bool RemoveLotProps(cSC4Lot* lot);
    bool RemoveLotProps(int x, int z);
    bool RemovePropA(cISC4Occupant* occupant);
    bool GetBuildingProps(OccupantList& out, cISC4Occupant* building);
    bool GetBuildingProps(OccupantList& out, int x, int z);
    bool AddBuildingProp(cISC4Occupant* building, cISC4Occupant* prop);
    bool RemoveBuildingProps(cISC4Occupant* building);
    bool RemoveBuildingProps(int x, int z);
    bool RemoveBuildingProp(cISC4Occupant* building, cISC4Occupant* prop);

    // @ 0x100193c06: feeds every exemplar of the prop type to AddPropExemplarDataStatic, then announces the update.
    void LoadAllStandardPropExemplarData();
    static void AddPropExemplarDataStatic(const cGZPersistResourceKey& key, void* manager);   // @ 0x100194548
    static void PostPropExemplarDataUpdatedMessage();                                         // @ 0x1001945cc
    // @ 0x100194bb2: keeps the new filter object (+0x40), taking a reference on it and dropping the old one.
    void DoPostViewFilterChange(PropViewFilterInformation* oldFilter, PropViewFilterInformation* newFilter);

    static bool StaticInit(cSC4PropManager* manager);
    static bool StaticShutdown();

private:
    cSC4PropOccupant* AddPropWithUsage(std::uint32_t type, const float* position, int orientation,
                                       std::uint32_t usage);
    void SetPropUsage(cISC4Occupant* occupant, std::uint32_t usage);
    void RemoveAndReleaseOccupants(OccupantList& list);
    void RemoveOccupant(cISC4Occupant* occupant);
    bool InitProp(cSC4PropOccupant* prop);

    std::uint32_t refCount_ = 0;
    bool initialized_ = false;
    IPropViewFilter* viewFilter_ = nullptr;     // +0x40
    std::uint32_t usageFilter_ = 0;             // +0x30 object's field +0x14: which usage a query accepts
};

}  // namespace sc4
