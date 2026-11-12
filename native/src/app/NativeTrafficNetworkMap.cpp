// The traffic network map's city services (sc4::ITrafficNetworkMapHost) and its views of the city's objects: network pieces
// (ITrafficAutomatonOccupant over cSC4NetworkOccupant, with their path info), lots and buildings. In the original these are the
// objects themselves (the map QueryInterfaces cISC4NetworkOccupant); the port's pieces answer no such interface, so the host
// hands out views. The views are stateless pass-throughs kept per object address for the process: a new object at a recycled
// address simply gets the same pass-through.
#include <map>
#include <memory>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeZoneDeveloperHost.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PathInfo.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/UndergroundOccupant.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

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
    void MakeTunnelPaths(ITrafficNetworkMapOccupant*, ITrafficNetworkMapOccupant*) override {}   // PARTIAL: no tunnels natively
private:
    cISC4PathInfo* Info() const { return occupant_->GetPathInfo(); }   // the piece caches its path info
    cSC4NetworkOccupant* occupant_;
};

// ---- network pieces --------------------------------------------------------------------------------------------------
class PieceView final : public ITrafficAutomatonOccupant {
public:
    explicit PieceView(cSC4NetworkOccupant* occupant) : occupant_(occupant), path_(occupant) {}
    bool QueryInterface(std::uint32_t iid, void** out) override { return occupant_->QueryInterface(iid, out); }
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
};
std::map<cSC4NetworkOccupant*, std::unique_ptr<PieceView>> g_pieces;

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
    bool SimulationActive() override { return HostCity() != nullptr; }   // PARTIAL: the vehicle / pedestrian managers' IsActive
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
    std::vector<ITrafficNetworkMapProp*> PropsAtCell(int, int) override { return {}; }   // PARTIAL: no stoplight props natively
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
    int ActiveEdgesForBuilding(std::uint32_t, std::uint8_t) override { return 0xf; }   // PARTIAL: BDS vslot 0xf0 (every edge)
    void GetViewPosition(float& x, float& y, float& z) override { x = y = z = 0.0f; }   // PARTIAL: view utilities' camera
    void GetViewTarget(float& x, float& y, float& z) override { x = y = z = 0.0f; }
    void SubscribeMessage(std::uint32_t message) override { if (SL::spMS2 && map_) SL::spMS2->AddNotification(map_, message); }
    void UnsubscribeMessage(std::uint32_t message) override { if (SL::spMS2 && map_) SL::spMS2->RemoveNotification(map_, message); }
    void MakeTunnelPaths(ITrafficNetworkMapPathInfo*, ITrafficNetworkMapOccupant*, ITrafficNetworkMapOccupant*) override {}
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
