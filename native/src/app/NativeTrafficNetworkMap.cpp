// The traffic network map's city services (sc4::ITrafficNetworkMapHost) and its views of the city's objects: network pieces
// (ITrafficAutomatonOccupant over cSC4NetworkOccupant, with their path info), lots and buildings. In the original these are the
// objects themselves (the map QueryInterfaces cISC4NetworkOccupant); the port's pieces answer no such interface, so the host
// hands out views. The views are stateless pass-throughs kept per object address for the process: a new object at a recycled
// address simply gets the same pass-through.
#include <map>
#include "platform/Unwired.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "app/NativeCityView.h"
#include <memory>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "sc4/AnimationTickManager.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "sc4/Render3D.h"
#include "sc4/ViewUtilities.h"
#include "app/NativeZoneDeveloperHost.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTunnelOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PathInfo.h"
#include "sc4/OccupantFilters.h"
#include "sc4/PropOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/UndergroundOccupant.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

// A stoplight prop as the traffic map drives it (cISC4PropOccupant: GetState 0x60 / SetState 0x58 / GetOrientation 0x48, the
// occupant's group, position and flags, the holder's properties). Holds one reference on the prop.
class StoplightPropView final : public ITrafficNetworkMapProp {
public:
    explicit StoplightPropView(cSC4PropOccupant* prop) : prop_(prop) { prop_->AddRef(); }
    ~StoplightPropView() { prop_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    bool IsOccupantGroup(std::uint32_t group) override { return prop_->IsOccupantGroup(group); }
    bool HasProperty(std::uint32_t id) override { return prop_->AsPropertyHolder() && prop_->AsPropertyHolder()->HasProperty(id); }
    int GetState() override { return static_cast<int>(prop_->GetState()); }
    void SetState(int state) override {
        static const bool trace = std::getenv("SC4_TRAFFICMAPDEBUG") != nullptr;
        static int traced = 0;
        if (trace && traced < 40 && prop_->GetState() != std::uint32_t(state)) {
            ++traced;
            float p[3] = {};
            prop_->GetPosition(p);
            std::fprintf(stderr, "traffic map: stoplight at %.0f,%.0f state %u -> %d\n", p[0], p[2], prop_->GetState(), state);
        }
        prop_->SetState(static_cast<std::uint32_t>(state));
    }
    void GetPosition(float* xyz) override { prop_->GetPosition(xyz); }
    int GetOrientation() override { return prop_->GetOrientation(); }
    bool IsFlagSet(std::uint32_t flag) override { return prop_->IsFlagSet(flag); }

private:
    cSC4PropOccupant* prop_;
    std::uint32_t refs_ = 1;
};

cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

// ---- path info -------------------------------------------------------------------------------------------------------
class PathInfoView final : public ITrafficNetworkMapPathInfo {
public:
    explicit PathInfoView(cSC4NetworkOccupant* occupant) : occupant_(occupant) {}
    bool IsIntersection(std::uint32_t pathType) override { cISC4PathInfo* p = Info(); return p && p->IsIntersection(pathType); }
    int SnapPointToAnyPath(cS3DVector3& point, std::uint32_t pathType, std::uint32_t* key) override {
        cISC4PathInfo* p = Info();
        return p ? p->SnapPointToAnyPath(point, pathType, key) : -1;
    }
    int GetStopPointCount(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit, std::uint8_t lane) override {
        cISC4PathInfo* p = Info();
        return p ? p->GetStopPointCount(pathType, entry, exit, lane) : 0;
    }
    bool GetJunctionKeys(std::uint32_t pathType, std::uint8_t entry, std::vector<std::uint32_t>& keys) override {
        cISC4PathInfo* p = Info();
        return p && p->GetJunctionKeys(pathType, entry, keys);
    }
    // cISC4PathInfo::MakeTunnelPaths (vslot 0x100) with the two portals as the path info sees them.
    void MakeTunnelPaths(ITrafficNetworkMapOccupant* from, ITrafficNetworkMapOccupant* to) override {
        cISC4PathInfo* p = Info();
        auto* a = from ? dynamic_cast<IPathNetworkOccupant*>(from->AsOccupant()) : nullptr;
        auto* b = to ? dynamic_cast<IPathNetworkOccupant*>(to->AsOccupant()) : nullptr;
        if (p && a && b) p->MakeTunnelPaths(a, b);
    }
private:
    cISC4PathInfo* Info() const { return occupant_->GetPathInfo(); }   // the piece caches its path info
    cSC4NetworkOccupant* occupant_;
};

// ---- network pieces --------------------------------------------------------------------------------------------------
// cISC4NetworkTunnelOccupant as the map reads it: the portal at the other end (borrowed, as GetOtherEndOccupant returns it).
class TunnelView final : public ITrafficNetworkMapTunnel {
public:
    explicit TunnelView(cSC4NetworkTunnelOccupant* tunnel) : tunnel_(tunnel) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return tunnel_->AddRef(); }
    std::uint32_t Release() override { return tunnel_->Release(); }
    ITrafficNetworkMapOccupant* GetOtherEndOccupant() override;
private:
    cSC4NetworkTunnelOccupant* tunnel_;
};
class PieceView final : public ITrafficAutomatonOccupant {
public:
    explicit PieceView(cSC4NetworkOccupant* occupant) : occupant_(occupant), path_(occupant) {}
    // A tunnel portal answers cISC4NetworkTunnelOccupant (0x8a4bd52b) with the map's view of it (its other end).
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZCLSID_cSC4NetworkTunnelOccupant) {
            auto* tunnel = dynamic_cast<cSC4NetworkTunnelOccupant*>(occupant_);
            if (!tunnel) return false;
            if (!tunnel_) tunnel_ = std::make_unique<TunnelView>(tunnel);
            tunnel_->AddRef();
            *out = static_cast<ITrafficNetworkMapTunnel*>(tunnel_.get());
            return true;
        }
        return occupant_->QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return occupant_->AddRef(); }
    std::uint32_t Release() override { return occupant_->Release(); }
    bool HasNetworkFlag(std::uint32_t flags) const override { return occupant_->HasNetworkFlag(flags); }
    bool HasAnyNetworkFlag(std::uint32_t flags) const override { return occupant_->HasAnyNetworkFlag(flags); }
    void SetNetworkFlag(std::uint32_t flags) override { occupant_->SetNetworkFlag(flags); }
    void ClearNetworkFlag(std::uint32_t flags) override { occupant_->ClearNetworkFlag(flags); }
    bool ConnectsToNeighbor(int edge, std::uint32_t type) const override { return occupant_->ConnectsToNeighbor(edge, std::uint8_t(type)); }
    cISC4Occupant* AsOccupant() override { return occupant_; }
    ITrafficNetworkMapPathInfo* GetPathInfo() override { return occupant_->GetPathInfo() ? &path_ : nullptr; }
    void ReleasePathInfo() override {}   // the piece keeps its path info (the map's release only frees memory)
    bool PathConnectionExists(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit) const override {
        return occupant_->PathConnectionExists(pathType, entry, exit) != 0;
    }
    bool IsIntersection() const override { return occupant_->IsIntersection(); }
    float GetAltitude(float x, float z) const override { return occupant_->GetAltitude(x, z); }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { occupant_->GetOccupiedCell(x, z); }
    bool IsOfType(std::uint32_t type) const override { return occupant_->IsOfType(type); }
    bool IsUsable() const override { return occupant_->IsUsable(); }
    cISC4PathInfo* GetPathInfoObject() override { return occupant_->GetPathInfo(); }
private:
    cSC4NetworkOccupant* occupant_;
    mutable PathInfoView path_;
    std::unique_ptr<TunnelView> tunnel_;
};
std::map<cSC4NetworkOccupant*, std::unique_ptr<PieceView>> g_pieces;
ITrafficNetworkMapOccupant* TunnelView::GetOtherEndOccupant() {
    cSC4NetworkOccupant* other = tunnel_->GetOtherEndOccupant();
    if (!other) return nullptr;
    auto& slot = g_pieces[other];
    if (!slot) slot = std::make_unique<PieceView>(other);
    return slot.get();
}

// ---- lots and buildings ----------------------------------------------------------------------------------------------
class LotView final : public ITrafficNetworkMapLot {
public:
    explicit LotView(cSC4Lot* lot) : lot_(lot) {}
    void GetBoundingRect(std::int32_t rect[4]) override { lot_->GetBoundingRect(rect); }
    bool IsLivable() override { return lot_->IsInLivableState(); }
    std::uint8_t OccupantWealth() override { return lot_->GetOccupantWealth(); }
    std::uint16_t TravelPopulation(std::uint32_t developerType) override { return lot_->GetTravelPopulation(developerType); }
    cSC4Lot* Lot() const { return lot_; }
private:
    cSC4Lot* lot_;
};
std::map<cSC4Lot*, std::unique_ptr<LotView>> g_lots;
LotView* LotViewOf(cSC4Lot* lot) {
    if (!lot) return nullptr;
    auto& slot = g_lots[lot];
    if (!slot) slot = std::make_unique<LotView>(lot);
    return slot.get();
}

class BuildingView final : public ITrafficNetworkMapBuilding {
public:
    explicit BuildingView(cISC4BuildingOccupant* building) : building_(building) {}
    std::uint32_t Purpose() override { return building_->GetBuildingProfile().purpose; }
    std::uint32_t Wealth() override { return building_->GetBuildingProfile().wealth; }
private:
    cISC4BuildingOccupant* building_;
};
std::map<cISC4BuildingOccupant*, std::unique_ptr<BuildingView>> g_buildings;

// ---- the host --------------------------------------------------------------------------------------------------------
class MapHost final : public ITrafficNetworkMapHost {
public:
    int CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = int(x / 16.0f);
        cellZ = int(z / 16.0f);
    }
    // i386 0x001e7bb2 / DoAnimationTick: the vehicle manager (city vslot 0xac) or the pedestrian manager (0xb0) is active (vslot 0x18).
    bool SimulationActive() override { return CityAutomataLayersActive(); }
    ITrafficNetworkMapLot* LotAtCell(int x, int z) override { return LotViewOf(NativeLotAtCell(x, z, false)); }
    ITrafficNetworkMapLot* LotOf(cISC4Occupant* occupant) override {
        if (!occupant) return nullptr;
        float p[3] = {};
        occupant->GetPosition(p);
        return LotViewOf(NativeLotAtCell(int(p[0] / 16.0f), int(p[2] / 16.0f), false));
    }
    std::vector<cISC4Occupant*> NetworkOccupantsInBox(const float boxMin[3], const float boxMax[3]) override {
        std::vector<cISC4Occupant*> out;
        cISC4OccupantManager* occupants = Occupants();
        OccupantList found;
        if (occupants && occupants->GetOccupantsByBBox(found, boxMin, boxMax, 0xc772bf98u, 0xffffffffu))
            for (cISC4Occupant* occupant : found) out.push_back(occupant);   // the list's references move to the caller
        return out;
    }
    void ReleaseOccupants(const std::vector<cISC4Occupant*>& occupants) override { for (cISC4Occupant* o : occupants) if (o) o->Release(); }
    // InitIntersectionProps (i386 0x001ea734): the occupant manager's GetOccupantsByStandardCityCells(cell, cell, the stoplight
    // filter) and each one that answers cISC4PropOccupant (0xe9793a65), one reference each.
    std::vector<ITrafficNetworkMapProp*> PropsAtCell(int x, int z) override {
        std::vector<ITrafficNetworkMapProp*> out;
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return out;
        static cStoplightPropFilter* filter = [] { auto* f = new cStoplightPropFilter(); f->AddRef(); return f; }();
        const int cell[2] = {x, z};
        OccupantList found;
        if (occupants->GetOccupantsByStandardCityCells(found, cell, cell, filter, 0xffffffffu)) {
            for (cISC4Occupant* occupant : found) {
                if (auto* prop = dynamic_cast<cSC4PropOccupant*>(occupant)) out.push_back(new StoplightPropView(prop));
                occupant->Release();
            }
        }
        static const bool trace = std::getenv("SC4_TRAFFICMAPDEBUG") != nullptr;
        static int traced = 0;
        if (trace && !out.empty() && traced++ < 20) std::fprintf(stderr, "traffic map: intersection at %d,%d: %zu stoplight props\n", x, z, out.size());
        return out;
    }
    bool HasTerrain() override { return Terrain() != nullptr; }
    float TerrainHeightAt(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    ITrafficNetworkMapBuilding* LotBuilding(ITrafficNetworkMapLot* lot) override {
        auto* view = static_cast<LotView*>(lot);
        cIGZUnknown* object = view ? view->Lot()->GetBuilding() : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return nullptr;
        auto* building = static_cast<cISC4BuildingOccupant*>(raw);
        building->Release();   // the lot keeps it
        auto& slot = g_buildings[building];
        if (!slot) slot = std::make_unique<BuildingView>(building);
        return slot.get();
    }
    // CheckCellOccupancy (i386): SL::spBuildingDevelopmentSim vslot 0x78 GetDeveloperType(purpose, wealth), the index the lot's
    // travel population is read with.
    int ActiveEdgesForBuilding(std::uint32_t purpose, std::uint8_t wealth) override {
        return static_cast<int>(sc4::cSC4BuildingDevelopmentSimulator::GetDeveloperType_Static(purpose, wealth));
    }
    // The view utilities' visible terrain rectangle (i386 +0x3ec..+0x3f8, kept by cSC43DRender::UpdateVisibleTerrainMetrics):
    // its min corner and its max corner (y unused).
    void GetViewPosition(float& x, float& y, float& z) override {
        const SC4ViewUtilities* v = native::CityRenderer() ? native::CityRenderer()->GetViewUtilities() : nullptr;
        x = v ? v->VisibleTerrainRect()[0] : 0.0f;
        z = v ? v->VisibleTerrainRect()[1] : 0.0f;
        y = 0.0f;
    }
    void GetViewTarget(float& x, float& y, float& z) override {
        const SC4ViewUtilities* v = native::CityRenderer() ? native::CityRenderer()->GetViewUtilities() : nullptr;
        x = v ? v->VisibleTerrainRect()[2] : 0.0f;
        z = v ? v->VisibleTerrainRect()[3] : 0.0f;
        y = 0.0f;
    }
    void SubscribeMessage(std::uint32_t message) override { if (SL::spMS2 && map_) SL::spMS2->AddNotification(map_, message); }
    void UnsubscribeMessage(std::uint32_t message) override { if (SL::spMS2 && map_) SL::spMS2->RemoveNotification(map_, message); }
    bool TrafficEdgeDensity(int x, int z, std::uint16_t out[16]) override {
        cSC4City* city = HostCity();
        auto* traffic = city ? dynamic_cast<cSC4TrafficSimulator*>(city->GetComponent(CityComponent::TrafficSimulator)) : nullptr;
        if (!traffic) return false;
        const struct TrafficEdgeDensity& density = traffic->GetTrafficEdgeDensity(x & 0xff, z & 0xff, 1, false);
        for (int i = 0; i < 16; ++i) out[i] = density.last[std::size_t(i)];
        return true;
    }
    void AddTickable(std::uint32_t priority) override {
        if (sc4::cSC4AnimationTickManager* ticks = AnimationTickManager(); ticks && map_) ticks->AddTickableObject(map_, priority);
    }
    void RemoveTickable(std::uint32_t priority) override {
        if (sc4::cSC4AnimationTickManager* ticks = AnimationTickManager(); ticks && map_) ticks->RemoveTickableObject(map_, priority);
    }
    void MakeTunnelPaths(ITrafficNetworkMapPathInfo* pathInfo, ITrafficNetworkMapOccupant* from, ITrafficNetworkMapOccupant* to) override {
        if (pathInfo) pathInfo->MakeTunnelPaths(from, to);
    }
    ITrafficNetworkMapOccupant* AsMapOccupant(cISC4Occupant* occupant) override {
        auto* piece = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        if (!piece) return nullptr;
        auto& slot = g_pieces[piece];
        if (!slot) slot = std::make_unique<PieceView>(piece);
        slot->AddRef();
        return slot.get();
    }
    void SetMap(cIGZMessageTarget2* map) { map_ = map; }
private:
    cIGZMessageTarget2* map_ = nullptr;
};
MapHost g_host;

}  // namespace

void InstallTrafficNetworkMapHost() { sc4::SetDefaultTrafficNetworkMapHost(&g_host); }

// The city's map, once it exists: messages are delivered to it through the host's subscriptions.
void BindTrafficNetworkMap(sc4::cSC4TrafficNetworkMap* map) { g_host.SetMap(map ? static_cast<cIGZMessageTarget2*>(map) : nullptr); }

// The lot at a city cell as the traffic classes see it (the lot manager's slot 0x28, flag 0), or null.
sc4::ITrafficNetworkMapLot* NativeTrafficLotAt(int cellX, int cellZ) { return LotViewOf(NativeLotAtCell(cellX, cellZ, false)); }

// The occupant manager's box query by type (slot 0x120), at most `count`; the references go to the caller.
void NativeOccupantsInBox(const float* lo, const float* hi, std::uint32_t type, int count, std::vector<sc4::cISC4Occupant*>& out) {
    cISC4OccupantManager* occupants = Occupants();
    OccupantList found;
    if (occupants && occupants->GetOccupantsByBBox(found, lo, hi, type, std::uint32_t(count)))
        for (cISC4Occupant* occupant : found) out.push_back(occupant);
}

// The train car's question about the tile it enters (cSC4TrainCar::SetPath): an underground piece's zipper type (slot
// 0x1d8 of the interface 0x4a10afc2), -1 for a piece that is not underground.
int NativeZipperPieceType(sc4::ITrafficAutomatonOccupant* occupant) {
    cISC4Occupant* piece = occupant ? occupant->AsOccupant() : nullptr;
    auto* under = piece ? dynamic_cast<cSC4UndergroundOccupant*>(piece) : nullptr;
    if (!under) return -1;
    bool direction = false;
    return under->GetZipperPieceType(direction);
}

// The HandleExitNetwork overrides' question: the lot's building (lot slot 0xc0), as an occupant, is in `group`.
bool NativeLotBuildingInGroup(void* lot, std::uint32_t group) {
    cIGZUnknown* object = lot ? static_cast<cSC4Lot*>(lot)->GetBuilding() : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return false;
    auto* building = static_cast<cISC4Occupant*>(raw);
    const bool in = building->IsOccupantGroup(group);
    building->Release();
    return in;
}

// A message with two data words, posted to the city's subscribers.
void NativePostCityMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2) {
    if (!SL::spMS2) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(data1);
    message->SetData2(data2);
    SL::spMS2->MessagePost(message, false);
    message->Release();
}

sc4::ITrafficAutomatonOccupant* NativeTrafficPiece(sc4::cISC4Occupant* occupant) {
    return static_cast<sc4::ITrafficAutomatonOccupant*>(g_host.AsMapOccupant(occupant));
}

}  // namespace native
