// cSC4ZoneDeveloper — see ZoneDeveloper.h for the layout, the four COM views and the list of quirks.
//
// Every call outside the object goes through IZoneDeveloperHost (one method per original call, with
// its slot); cSC4Lot is used directly through the recovered methods of Lot.h. The comments name the
// original vtable slot of every such call.

#include "sc4/ZoneDeveloper.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <list>
#include <unordered_set>

#include "gz/StreamOps.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/LotConfiguration.h"
#include "sc4/SC4Utilities.h"
#include "sc4/cSC4SimGrid.h"

namespace sc4 {

namespace {

IZoneDeveloperHost* g_host = nullptr;
bool g_staticInitialized = false;   // mbStaticInitialized @ 0x100b... (set by the first Init)
std::set<const void*> g_cheatRegisteringObjects;   // developers that registered their cheat codes

constexpr std::uint32_t kGZIID_cISC4ZoneDeveloper = 0xe818ed2b;
constexpr std::uint32_t kGZIID_cISC4LotManager = 0x681905bd;
constexpr std::uint32_t kZoneDeveloperIIDSerializable = 0xe4fda3d4;
constexpr std::uint32_t kGZIID_cISC4NetworkOccupant = 0xa821ef94;

// The messages Init subscribes to and DoMessage handles.
constexpr std::uint32_t kZoneMessageOccupantRemoved = 0x99ef1142;
constexpr std::uint32_t kZoneMessageOccupantRemovedSubType = 0xc7729698;
constexpr std::uint32_t kZoneMessageHighLightScale = 0xa6b79602;
constexpr std::uint32_t kZoneMessageCheatCode = 0x230e27ac;
constexpr std::uint32_t kZoneMessageCityInit = 0x26d31ec1;
constexpr std::uint32_t kZoneMessageDeferOccupantOperations = 0x49d968c4;

// Cheat commands registered by Init (strings at 0x100b000d0/d8).
constexpr std::uint32_t kCheatPlopAllLots = 0x8929e68e;
constexpr std::uint32_t kCheatPlopAllBuildings = 0x4ab569c9;
constexpr const char* kCheatCodePlotAllLots = "PlopAllLots";
constexpr const char* kCheatCodePlotAllBuildings = "PlopAllBuildings";

// Network cell flags used by the zoning queries.
constexpr std::uint32_t kNetworkFlagAnyConnection = 0x1f4f;
constexpr std::uint32_t kNetworkFlagStreet = 0x104;
constexpr std::uint32_t kNetworkFlagRail = 0x808;
constexpr std::uint32_t kNetworkFlagRoad = 0x401;
constexpr std::uint32_t kNetworkFlagPowerLine = 0x40;

// The occupant types ExamineCell treats as a blocked boundary (cSC4BuildingOccupant and its
// neighbour-connection occupant).
constexpr std::uint32_t kZoneOccupantTypeBuilding = 0x278128a0;
constexpr std::uint32_t kZoneOccupantTypeBlocking2 = 0x2890d4de;
constexpr std::uint32_t kZoneOccupantTypeNetwork = 0xc772bf98;

// Direction index -> the network occupant's direction flag (DAT_100a18dbc = {1, 2, 3, 0}).
constexpr std::uint32_t kDirectionFlags[4] = {1, 2, 3, 0};

// GetNetwork / GetNetworkOccupant out-object flag: an occupant that already reports this flag is skipped
// when the occupant-removed message is handled (0x80200090).
constexpr std::uint32_t kNetworkOccupantSkipFlag = 0x80200090;

// The magic round-to-nearest sequence of the original cell conversions (0x1001310b4, 0x100131c10):
//   cell = (int)(world * 1/16 + (-0.5 or -0.499) + 12582912.0f) - 12582912, then use base + 1 when the
//   scaled value is exactly the next integer. The net effect is floor(world / 16) for non-negative
//   coordinates; the exact float sequence is kept so the port matches on the boundaries.
int WorldToCell(float world, float halfOffset) {
    const float scaled = world * 0.0625f;
    const float biased = scaled + halfOffset + 12582912.0f;
    const int base = static_cast<int>(biased) - 12582912;
    const int up = base + 1;
    if (std::isnan(scaled) || std::isnan(static_cast<float>(up))) return base;
    return scaled == static_cast<float>(up) ? up : base;
}
int WorldToCell(float world) { return WorldToCell(world, -0.5f); }

int Clamp(int value, int lo, int hi) { return value < lo ? lo : (value > hi ? hi : value); }

bool IsBoundaryLot(const cSC4Lot* lot) {
    return lot != nullptr && (lot->GetFlags() & 1) != 0;   // cISC4Lot vslot 0x60, kLotFlagLocked
}

// The parked-lot lookups read the lot map without a bounds check in the original; the queries here
// keep that contract but never index outside a valid city (callers pass city cells).
cSC4Lot* LotAt(std::vector<std::vector<cSC4Lot*>>& map, int x, int z) {
    if (x < 0 || z < 0 || static_cast<std::size_t>(x) >= map.size()) return nullptr;
    if (static_cast<std::size_t>(z) >= map[static_cast<std::size_t>(x)].size()) return nullptr;
    return map[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
}

}  // namespace

IZoneDeveloperHost* ZoneDeveloperHost() { return g_host; }
void SetZoneDeveloperHost(IZoneDeveloperHost* host) { g_host = host; }

//---------------------------------------------------------------------------------------------------------------------
// construction / COM plumbing
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100128098. The original zeroes 0x24..0xaf and initialises every container; the members do the
// same here. The five parcellization tunables (2, 2, 1, 3, 6), the option bytes and the boundary maps
// are set by their in-class initialisers.
cSC4ZoneDeveloper::cSC4ZoneDeveloper() = default;

// @ 0x10012826c (base object destructor), 0x10012836a (complete object destructor) and 0x100128374
// (deleting destructor). The original only releases the lot developer here; every other service is
// released by Shutdown (the destructor assumes it ran).
cSC4ZoneDeveloper::~cSC4ZoneDeveloper() {
    if (lotDeveloper_) {
        lotDeveloper_->Release();   // primary vslot 0x10 of SL::spLotDeveloper
        lotDeveloper_ = nullptr;
    }
}

// @ 0x1001283a2. iid 1 and 0xe818ed2b answer the primary view, 0x681905bd the lot-manager view,
// 0x452294aa the message target and 0xe4fda3d4 the serializable view. The original stores the pointer
// before AddRef; the single count at +0x24 is incremented for every successful QI.
bool cSC4ZoneDeveloper::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    void* result = nullptr;
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4ZoneDeveloper) {
        result = static_cast<cISC4ZoneDeveloper*>(this);
    } else if (iid == kGZIID_cISC4LotManager) {
        result = static_cast<cISC4LotManager*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        result = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kZoneDeveloperIIDSerializable) {
        result = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    *out = result;
    AddRef();
    return true;
}

// @ 0x10012841e (the three secondary AddRef thunks adjust back to this same count).
std::uint32_t cSC4ZoneDeveloper::AddRef() { return ++refCount_; }

// @ 0x100128456. Quirk kept: the object is destroyed as soon as the count drops below 2 (it never
// checks for zero) and the call answers 0 in that case. The three thunks route here with their `this`
// adjusted to the primary object.
std::uint32_t cSC4ZoneDeveloper::Release() {
    if (static_cast<std::int32_t>(refCount_) < 2) {
        Shutdown();      // primary vslot 0x20
        delete this;     // primary vslot 0x58 (the deleting destructor)
        return 0;
    }
    return --refCount_;
}

// @ 0x10012853c. Attaches every service the class uses, sizes the lot map, creates the capacity grid
// and the boundary texture rules, subscribes to the three messages and (when the cheat-code manager
// and the application allow it) registers the two plop cheat codes.
bool cSC4ZoneDeveloper::Init() {
    if (initialized_) return true;
    if (!g_staticInitialized) g_staticInitialized = true;
    highlightScale_ = 0;

    if (!lotDeveloper_) {   // SL::spLotDeveloper
        lotDeveloper_ = g_host ? g_host->LotDeveloper() : nullptr;
        if (!lotDeveloper_) return false;
        lotDeveloper_->AddRef();
    }
    if (!renderer_) {       // SL::spRenderer; its vslot 0x50 object carries the scale at +0x14
        renderer_ = g_host ? g_host->Renderer() : nullptr;
        if (!renderer_) return false;
        renderer_->AddRef();
        highlightScale_ = g_host->RendererHighlightScale();
    }
    if (!buildingDevelopment_) {   // SL::spBuildingDevelopmentSim
        buildingDevelopment_ = g_host ? g_host->BuildingDevelopment() : nullptr;
        if (!buildingDevelopment_) return false;
        buildingDevelopment_->AddRef();
    }
    if (!city_) {                  // SL::spCity
        city_ = g_host ? g_host->City() : nullptr;
        if (!city_) return false;
        city_->AddRef();
        cellsX_ = city_->CellCountX();   // city vslot 0x2e8
        cellsZ_ = city_->CellCountZ();   // city vslot 0x2f0
    }
    if (!occupantManager_) {       // SL::spOccupantManager
        occupantManager_ = g_host ? g_host->OccupantManager() : nullptr;
        if (!occupantManager_) return false;
        occupantManager_->AddRef();
    }
    if (!terrain_) {               // SL::spTerrain
        terrain_ = g_host ? g_host->Terrain() : nullptr;
        if (!terrain_) return false;
        terrain_->AddRef();
    }
    if (!trafficSim_) {            // SL::spTrafficSim
        trafficSim_ = g_host ? g_host->TrafficSim() : nullptr;
        if (!trafficSim_) return false;
        trafficSim_->AddRef();
    }
    if (!zoneManager_) {           // SL::spZoneManager
        zoneManager_ = g_host ? g_host->ZoneManager() : nullptr;
        if (!zoneManager_) return false;
        zoneManager_->AddRef();
    }

    if (lotMap_.empty()) {
        lotMap_.resize(static_cast<std::size_t>(cellsX_));
        for (auto& column : lotMap_) column.assign(static_cast<std::size_t>(cellsZ_), nullptr);
    } else if (cellsX_ > 0) {
        // Re-init: the original walks the existing map with GetLot(x, z, true) (vslot 0x40, i386 0x000e100c) and calls
        // cISC4Lot::Init on each lot it returns. The exact-cell query answers only a lot's anchor cell, so every lot is initialised
        // once (a second Init drops the configuration the first restored from the save).
        for (int x = 0; x < cellsX_; ++x)
            for (int z = 0; z < cellsZ_; ++z) {
                cSC4Lot* lot = GetLot(x, z, true);
                if (lot) lot->Init();
            }
    }

    if (!capacityGrid_) {
        // The original builds a cSC4SimGrid<uint16_t> by hand: refcount 1, initialised, class id
        // 0x49b9e604, tract size 4 and instance id 0x69d5c456.
        capacityGrid_ = new cSC4SimGrid<std::uint16_t>();
        capacityGrid_->AddRef();
        capacityGrid_->Init();
        capacityGrid_->SetTractSize(4);
        capacityGrid_->SetInstanceID(0x69d5c456);
    }

    InitBoundaryRules();

    if (!messageServer_) {   // SL::spMS2
        messageServer_ = g_host ? g_host->MessageServer() : nullptr;
        if (!messageServer_) return false;
        messageServer_->AddRef();
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        messageServer_->AddNotification(target, kZoneMessageOccupantRemoved);
        messageServer_->AddNotification(target, kZoneMessageCityInit);
        messageServer_->AddNotification(target, kZoneMessageHighLightScale);
    }

    if (g_host && g_host->CheatCodesEnabled()) {   // SL::spCCM != null && SL::spSC4->vslot 0xa8
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        g_host->RegisterCheatCode(kCheatCodePlotAllLots, kCheatPlopAllLots, target);
        g_host->RegisterCheatCode(kCheatCodePlotAllBuildings, kCheatPlopAllBuildings, target);
        g_cheatRegisteringObjects.insert(this);
    }

    initialized_ = true;
    return true;
}

// @ 0x100128b94. 47 entries keyed by ((s0 * 16 + s1) * 16 + s2) * 16 + s3. Key 0 is the fallback
// rule used by GetBoundaryLotTexture when the exact combination is absent.
void cSC4ZoneDeveloper::InitBoundaryRules() {
    struct Entry {
        int key;
        BoundaryLotTextureRule rule;
    };
    static const Entry kRules[] = {
        {0x0000, {0x3000, 0, false}},   {0x1000, {0x2000, 0, false}},   {0x0100, {0x2000, 1, false}},
        {0x0010, {0x2000, 2, false}},   {0x0001, {0x2000, 3, false}},   {0x2000, {0x2600, 0, false}},
        {0x0200, {0x2600, 1, false}},   {0x0020, {0x2600, 2, false}},   {0x0002, {0x2600, 3, false}},
        {0x1100, {0x2100, 1, false}},   {0x0110, {0x2100, 2, false}},   {0x0011, {0x2100, 3, false}},
        {0x1001, {0x2100, 0, false}},   {0x1200, {0x2400, 1, false}},   {0x0120, {0x2400, 2, false}},
        {0x0012, {0x2400, 3, false}},   {0x2001, {0x2400, 0, false}},   {0x2100, {0x2400, 0x100, false}},
        {0x0210, {0x2400, 0x103, false}}, {0x0021, {0x2400, 0x102, false}}, {0x1002, {0x2400, 0x101, false}},
        {0x1010, {0x2500, 0, false}},   {0x0101, {0x2500, 1, false}},   {0x1020, {0x2900, 2, false}},
        {0x0102, {0x2900, 3, false}},   {0x2010, {0x2900, 0, false}},   {0x0201, {0x2900, 1, false}},
        {0x1110, {0x2200, 1, false}},   {0x0111, {0x2200, 2, false}},   {0x1011, {0x2200, 3, false}},
        {0x1101, {0x2200, 0, false}},   {0x1120, {0x2700, 2, false}},   {0x0112, {0x2700, 3, false}},
        {0x2011, {0x2700, 0, false}},   {0x1201, {0x2700, 1, false}},   {0x1210, {0x2800, 1, false}},
        {0x0121, {0x2800, 2, false}},   {0x1012, {0x2800, 3, false}},   {0x2101, {0x2800, 0, false}},
        {0x2110, {0x2700, 0x100, false}}, {0x0211, {0x2700, 0x103, false}}, {0x1021, {0x2700, 0x102, false}},
        {0x1102, {0x2700, 0x101, false}}, {0x1112, {0x2300, 3, false}}, {0x2111, {0x2300, 0, false}},
        {0x1211, {0x2300, 1, false}},   {0x1121, {0x2300, 2, false}},
    };
    for (const Entry& entry : kRules) boundaryRules_[entry.key] = entry.rule;
}

// @ 0x1001291fe. Unregisters the cheats and the three messages, drops the lot maps and every service.
bool cSC4ZoneDeveloper::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;

    if (g_cheatRegisteringObjects.erase(this) != 0) {
        if (g_host) {
            g_host->UnregisterCheatCode(kCheatPlopAllLots);
            g_host->UnregisterCheatCode(kCheatPlopAllBuildings);
        }
    }

    if (messageServer_) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        messageServer_->RemoveNotification(target, kZoneMessageOccupantRemoved);
        messageServer_->RemoveNotification(target, kZoneMessageCityInit);
        messageServer_->RemoveNotification(target, kZoneMessageHighLightScale);
        messageServer_->Release();
        messageServer_ = nullptr;
    }

    boundaryRules_.clear();
    ClearLotMap();
    lotMap_.clear();

    if (dirtRoadTool_) { dirtRoadTool_->Shutdown(); dirtRoadTool_->Release(); dirtRoadTool_ = nullptr; }
    if (streetTool_) { streetTool_->Shutdown(); streetTool_->Release(); streetTool_ = nullptr; }
    if (roadTool_) { roadTool_->Shutdown(); roadTool_->Release(); roadTool_ = nullptr; }
    if (capacityGrid_) {
        capacityGrid_->Shutdown();
        capacityGrid_->Release();
        capacityGrid_ = nullptr;
    }
    if (zoneManager_) { zoneManager_->Release(); zoneManager_ = nullptr; }
    if (trafficSim_) { trafficSim_->Release(); trafficSim_ = nullptr; }
    if (terrain_) { terrain_->Release(); terrain_ = nullptr; }
    if (terrainView_) { terrainView_->Release(); terrainView_ = nullptr; }
    if (occupantManager_) { occupantManager_->Release(); occupantManager_ = nullptr; }
    if (city_) { city_->Release(); city_ = nullptr; }
    if (buildingDevelopment_) { buildingDevelopment_->Release(); buildingDevelopment_ = nullptr; }
    if (renderer_) { renderer_->Release(); renderer_ = nullptr; }
    if (lotDeveloper_) {
        lotDeveloper_->Release();
        lotDeveloper_ = nullptr;
    }
    return true;
}

// @ 0x1001294de. Deleting a lot clears its whole footprint, so the following cells are already null;
// the loop still reads every cell like the original.
void cSC4ZoneDeveloper::ClearLotMap() {
    for (auto& column : lotMap_)
        for (cSC4Lot*& cell : column) {
            if (cell) {
                cSC4Lot* lot = cell;
                DeleteLot(lot);
            }
        }
}

//---------------------------------------------------------------------------------------------------------------------
// messages
//---------------------------------------------------------------------------------------------------------------------

// @ 0x10012bdac. The message carries the flag in its first data word; the message server posts it to
// the occupant manager (vslot 0x20).
void cSC4ZoneDeveloper::DeferOccupantOperations(bool defer) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kZoneMessageDeferOccupantOperations);
    message->SetData1(defer ? 1u : 0u);
    if (messageServer_) messageServer_->MessagePost(message, false);
    message->Release();
}

// @ 0x10012959a. PARTIAL: the message dispatch, the highlight-scale word and the reload walk are
// recovered. The occupant-removed path stops after DeleteLot: CreateLotRemnants (0x100129956) and the
// road/rail connection helpers (0x10012a040, 0x10012a534) are not recovered, and the two plop cheat
// commands (0x10012abe4, 0x10012ad80) are skipped, so those branches leave the city untouched.
bool cSC4ZoneDeveloper::DoMessage(cIGZMessage2* message) {
    if (!message) return false;
    auto* standard = dynamic_cast<cIGZMessage2Standard*>(message);
    if (!standard) return false;
    // The original compares the message type as a signed 32-bit value (`if (type < 0x230e27ac)`),
    // which is why the two negative ids 0x99ef1142 / 0xa6b79602 are handled by the first branch.
    const std::int32_t type = static_cast<std::int32_t>(standard->GetType());

    if (type < static_cast<std::int32_t>(kZoneMessageCheatCode)) {
        if (type == static_cast<std::int32_t>(kZoneMessageOccupantRemoved)) {
            if (standard->GetData3() != kZoneMessageOccupantRemovedSubType) return true;
            auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
            if (!occupant) return true;
            // QI(0xa821ef94): occupants that already advertise the skip flag are ignored (e.g. network
            // occupants whose own removal owns the lot).
            void* network = nullptr;
            bool skip = false;
            if (occupant->QueryInterface(kGZIID_cISC4NetworkOccupant, &network) && network) {
                auto* view = static_cast<IZoneDeveloperNetworkOccupant*>(network);
                skip = view->HasNetworkFlag(kNetworkOccupantSkipFlag);
                view->Release();
            }
            if (skip) return true;

            float position[3] = {0, 0, 0};
            occupant->GetPosition(position);                       // cISC4Occupant vslot 0x40
            int cellX = 0, cellZ = 0;
            if (!city_) return true;
            city_->PositionToCell(position[0], position[2], cellX, cellZ);   // city vslot 0x2f8
            if (cellX < 0 || cellZ < 0 || cellX >= cellsX_ || cellZ >= cellsZ_) return true;   // original indexes unchecked
            cSC4Lot* lot = lotMap_[static_cast<std::size_t>(cellX)][static_cast<std::size_t>(cellZ)];
            if (lot) {
                std::int32_t rect[4] = {0, 0, 0, 0};
                lot->GetBoundingRect(rect);                         // cISC4Lot vslot 0x70
                const std::uint32_t zoneType = lot->GetZoneType();  // vslot 0xf8
                currentZoneType_ = static_cast<int>(zoneType);
                DeleteLot(lot);                                     // primary vslot 0x118
                if (zoneManager_ && zoneManager_->IsZoneTypeValid(currentZoneType_)) {
                    // CreateLotRemnants(rect.x0, rect.z0, rect.x2 + 1 - rect.x0, rect.z2 + 1 - rect.z0,
                    // lot->GetFacing()) is not recovered (0x100129956); the remnants are left unbuilt.
                }
                currentZoneType_ = 0;
            }
            const std::uint64_t removalKind = standard->GetData4();
            if (removalKind == 1 || removalKind == 2) {
                // ConnectRoadToAnyAdjacentStreets (0x10012a040) is not recovered.
            } else if (removalKind == 3) {
                // ConnectStreetsAcrossRail (0x10012a534) is not recovered.
            }
        } else if (type == static_cast<std::int32_t>(kZoneMessageHighLightScale)) {
            if (void* data = standard->GetVoid2()) highlightScale_ = *static_cast<int*>(data);
        }
    } else if (type == static_cast<std::int32_t>(kZoneMessageCheatCode)) {
        const std::uint64_t command = standard->GetData1();
        if (command == kCheatPlopAllLots) {
            // PlopAllLotTypes (0x10012abe4) is not recovered.
        } else if (command == kCheatPlopAllBuildings) {
            // PlopAllBuildingTypes (0x10012ad80) is not recovered.
        }
    } else if (type == static_cast<std::int32_t>(kZoneMessageCityInit)) {
        if (!terrainView_ && g_host) {
            terrainView_ = g_host->TerrainView();                  // SL::spTerrainView
            if (terrainView_) terrainView_->AddRef();
        }
        if (!streetTool_ && g_host) {
            streetTool_ = g_host->NetworkSubnet(1);               // network manager, type 3
            if (streetTool_) {
                streetTool_->AddRef();
                streetTool_->Init();                              // network vslot 0x18
            }
        }
        if (!dirtRoadTool_ && g_host) {
            dirtRoadTool_ = g_host->NetworkSubnet(2);               // network manager, type 0xb
            if (dirtRoadTool_) {
                dirtRoadTool_->AddRef();
                dirtRoadTool_->Init();
            }
        }
        if (!roadTool_ && g_host) {
            roadTool_ = g_host->NetworkSubnet(0);             // network manager, type 0
            if (roadTool_) {
                roadTool_->AddRef();
                roadTool_->Init();
            }
        }
        if (loaded_ && cellsX_ > 0) {
            for (int x = 0; x < cellsX_; ++x) {
                for (int z = 0; z < cellsZ_; ++z) {
                    cSC4Lot* lot = LotAt(lotMap_, x, z);
                    if (!lot) continue;
                    int lotX = 0, lotZ = 0;
                    lot->GetLocation(lotX, lotZ);                  // cISC4Lot vslot 0xa8
                    if (lotX == x && lotZ == z) lot->PostCityInit();  // vslot 0x40
                }
            }
        }
    }
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
// persistence
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100132f32. Version 1, the lot-map dimensions and every non-null cell (multi-cell lots are written
// once per cell, like the original).
bool cSC4ZoneDeveloper::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    const std::uint32_t rows = static_cast<std::uint32_t>(lotMap_.size());
    const std::uint32_t columns = rows != 0 ? static_cast<std::uint32_t>(lotMap_[0].size()) : 0u;
    stream.SetUint32(rows);
    stream.SetUint32(columns);
    // one reference per cell, a null one for an empty cell (SC4WriteGZSerializable), as Read reads them
    for (const auto& column : lotMap_)
        for (cSC4Lot* lot : column) {
            if (g_host) g_host->WriteLot(stream, lot);
            else if (lot) stream.SetUint32(0);
        }
    return !stream.GetError();
}

// @ 0x100132ffc. Version 1 only; the stream serializer returns a referenced lot for every cell and the
// duplicate pointers get their extra reference released (the original keeps a pointer hash set for
// this).
bool cSC4ZoneDeveloper::Read(cIGZIStream& stream) {
    loaded_ = true;
    std::uint16_t version = 0;
    if (!stream.GetUint16(version) || version != 1) return false;
    std::uint32_t countX = 0, countZ = 0;
    if (!stream.GetUint32(countX)) return false;
    stream.GetUint32(countZ);
    lotMap_.resize(countX);
    std::unordered_set<cSC4Lot*> seen;
    void* dbStream = nullptr;
    const bool cityFile = stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream;
    if (dbStream) static_cast<cIGZUnknown*>(dbStream)->Release();
    for (std::uint32_t x = 0; x < countX; ++x) {
        lotMap_[x].resize(countZ, nullptr);   // keeps pre-existing cells, like the original resize
        for (std::uint32_t z = 0; z < countZ; ++z) {
            // In a city file a lot is an object reference: the object is created (and read later) by the serialization layer, and a plain
            // lot is reached through the adapter that holds it. The adapter reference is kept for the lot's lifetime.
            cSC4Lot* lot = nullptr;
            cIGZUnknown* adapter = nullptr;
            if (cityFile) {
                void* raw = nullptr;
                SC4ReadGZSerializable(stream, kGZIID_cIGZSerializable, &raw, false);
                adapter = static_cast<cIGZUnknown*>(raw);
                void* plain = nullptr;
                if (adapter && adapter->QueryInterface(kGZIID_PlainObject, &plain) && plain) lot = static_cast<cSC4Lot*>(plain);
                else if (adapter) { adapter->Release(); adapter = nullptr; }
            } else {
                lot = g_host ? g_host->ReadLot(stream) : nullptr;
            }
            lotMap_[x][z] = lot;
            if (!lot) continue;
            if (!seen.insert(lot).second) {
                if (adapter) adapter->Release(); else lot->Release();   // the stream handed out a second reference for a duplicate cell
            } else {
                lots_.push_front(lot);
                ++lotCount_;
                if (adapter && g_host) g_host->LotRead(lot, adapter);
            }
        }
    }
    return !stream.GetError();
}

//---------------------------------------------------------------------------------------------------------------------
// lot queries
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100131192. `exactCell` additionally requires cISC4Lot::GetLocation (vslot 0xa8) to name the
// queried cell, which rejects the rest of a multi-cell footprint.
cSC4Lot* cSC4ZoneDeveloper::GetLot(int x, int z, bool exactCell) {
    if (x < 0 || z < 0 || x >= cellsX_ || z >= cellsZ_) return nullptr;
    cSC4Lot* lot = LotAt(lotMap_, x, z);
    if (exactCell) {
        if (!lot) return nullptr;
        int lotX = 0, lotZ = 0;
        lot->GetLocation(lotX, lotZ);
        if (lotX != x || lotZ != z) return nullptr;
    }
    return lot;
}

// @ 0x10013122a. City LocationIsInBounds (vslot 0x310) then PositionToCell (vslot 0x2f8).
cSC4Lot* cSC4ZoneDeveloper::GetLot(const cS3DVector3& location) {
    if (!city_ || !city_->LocationIsInBounds(location.x, location.z)) return nullptr;
    int cellX = 0, cellZ = 0;
    city_->PositionToCell(location.x, location.z, cellX, cellZ);
    return GetLot(cellX, cellZ, false);
}

// @ 0x1001310b4. cISC4Occupant::GetPosition (vslot 0x40) then the world-to-cell conversion.
cSC4Lot* cSC4ZoneDeveloper::GetOccupantLot(cISC4Occupant* occupant) {
    if (!occupant) return nullptr;
    float position[3] = {0, 0, 0};
    occupant->GetPosition(position);
    return GetLot(WorldToCell(position[0]), WorldToCell(position[2]), false);
}

// @ 0x100130354. With lot changes enabled: a lot under the cell. While the changes are deferred: a
// pending deletion also counts as "has a lot".
bool cSC4ZoneDeveloper::DoesCellHaveLot(int x, int z) {
    if (lotChangesEnabled_) return GetLot(x, z, false) != nullptr;
    const SC4Point<int> key{x, z};
    if (pendingDeletes_.find(key) != pendingDeletes_.end()) return true;
    return GetLot(x, z, false) != nullptr;
}

// @ 0x100131040. Pulls up to 0x3e6 random cells; returns null when none holds a lot.
cSC4Lot* cSC4ZoneDeveloper::GetRandomLot() {
    if (!g_host) return nullptr;
    for (int attempt = 0; attempt < 0x3e6; ++attempt) {
        const int x = static_cast<int>(g_host->RandomBelow(static_cast<std::uint32_t>(cellsX_)));
        const int z = static_cast<int>(g_host->RandomBelow(static_cast<std::uint32_t>(cellsZ_)));
        if (cSC4Lot* lot = LotAt(lotMap_, x, z)) return lot;
    }
    return nullptr;
}

// @ 0x100130e66. Looks the packed boundary key up and falls back to key 0 (the constructor's default
// rule {0x3000, 0}). The end-node read of the original when key 0 is absent is replaced by zeroes.
void cSC4ZoneDeveloper::GetBoundaryLotTexture(const BoundaryType& boundary, std::uint32_t& texture,
                                              std::uint8_t& shape, bool& flag) const {
    const int key = (((boundary.sides[0] * 16 + boundary.sides[1]) * 16 + boundary.sides[2]) * 16 +
                     boundary.sides[3]);
    auto it = boundaryRules_.find(key);
    if (it == boundaryRules_.end()) it = boundaryRules_.find(0);
    if (it == boundaryRules_.end()) {
        texture = 0;
        shape = 0;
        flag = false;
        return;
    }
    texture = it->second.texture;
    shape = static_cast<std::uint8_t>(it->second.shape);   // GetBoundaryLotTexture truncates the 16-bit rule
    flag = it->second.flag;
}

// @ 0x1001312a8. Clamps the rectangle, clears the list and pushes every lot that touches the cells. In
// the `unique` mode only the cell that names the lot (GetLocation == cell) is accepted; the normal mode
// dedupes a multi-cell lot against its left and upper neighbours.
int cSC4ZoneDeveloper::GetLotsInCellRect(std::list<cRZAutoRefCount<cSC4Lot>>& out, int x0, int z0,
                                         int x1, int z1, bool unique) {
    out.clear();
    const int cx0 = std::max(x0, 0);
    const int cz0 = std::max(z0, 0);
    const int cx1 = std::min(x1, cellsX_ - 1);
    const int cz1 = std::min(z1, cellsZ_ - 1);
    int count = 0;
    if (cx0 > cx1 || cz0 > cz1) return 0;
    if (unique) {
        for (int x = cx0; x <= cx1; ++x) {
            for (int z = cz0; z <= cz1; ++z) {
                cSC4Lot* lot = LotAt(lotMap_, x, z);
                if (!lot) continue;
                int lotX = 0, lotZ = 0;
                lot->GetLocation(lotX, lotZ);
                if (lotX == x && lotZ == z) {
                    out.emplace_back(lot);
                    ++count;
                }
            }
        }
        return count;
    }
    const std::vector<cSC4Lot*>* previousRow = nullptr;
    for (int x = cx0; x <= cx1; ++x) {
        cSC4Lot* previousCellLot = nullptr;
        const std::vector<cSC4Lot*>& row = lotMap_[static_cast<std::size_t>(x)];
        for (int z = cz0; z <= cz1; ++z) {
            cSC4Lot* lot = row[static_cast<std::size_t>(z)];
            const cSC4Lot* above = previousRow ? (*previousRow)[static_cast<std::size_t>(z)] : nullptr;
            if (lot && lot != previousCellLot && (previousRow == nullptr || lot != above)) {
                out.emplace_back(lot);
                ++count;
            }
            previousCellLot = lot;
        }
        previousRow = &row;
    }
    return count;
}

// @ 0x100131800.
int cSC4ZoneDeveloper::GetLotsSurroundingLot(const cSC4Lot* lot, std::list<cRZAutoRefCount<cSC4Lot>>& out,
                                             int edgeMask) {
    if (!lot) return 0;
    std::int32_t rect[4] = {0, 0, 0, 0};
    lot->GetBoundingRect(rect);
    const SC4Rect<int> bounds{rect[0], rect[1], rect[2], rect[3]};
    return GetLotsSurroundingRect(bounds, out, edgeMask);
}

// @ 0x100131896.
int cSC4ZoneDeveloper::GetLotsSurroundingLot(int x, int z, std::list<cRZAutoRefCount<cSC4Lot>>& out,
                                             int edgeMask) {
    cSC4Lot* lot = GetLot(x, z, false);
    return lot ? GetLotsSurroundingLot(lot, out, edgeMask) : 0;
}

// @ 0x100131514. Walks the one-cell ring around `rect` (top row, left/right cells of the middle rows,
// bottom row), keeps the cells allowed by `edgeMask` (1 north, 2 east, 4 south, 8 west) and pushes the
// distinct overlapping lots.
int cSC4ZoneDeveloper::GetLotsSurroundingRect(const SC4Rect<int>& rect,
                                              std::list<cRZAutoRefCount<cSC4Lot>>& out, int edgeMask) {
    const int left = rect.x0 - 1;
    const int top = rect.z0 - 1;
    const int right = rect.x1 + 1;
    const int bottom = rect.z1 + 1;

    int firstX = left;
    int firstZ = top;
    int lastX = right < cellsX_ ? right : cellsX_ - 1;
    int lastZ = bottom < cellsZ_ ? bottom : cellsZ_ - 1;
    int mask = edgeMask;
    if (rect.x0 < 1) {
        firstX = 0;
        mask &= ~8;
    }
    if (rect.z0 < 1) {
        firstZ = 0;
        mask &= ~1;
    }
    if (right >= cellsX_) mask &= ~2;
    if (bottom >= cellsZ_) mask &= ~4;

    const int ringRight = std::max(right, left);
    const int ringBottom = std::max(bottom, top);
    int count = 0;

    auto consider = [&](int x, int z) {
        if (x > lastX || z > lastZ || x < firstX || z < firstZ) return;
        cSC4Lot* lot = LotAt(lotMap_, x, z);
        if (!lot) return;
        std::int32_t lotRect[4] = {0, 0, 0, 0};
        lot->GetBoundingRect(lotRect);
        const bool overlaps = rect.x0 <= lotRect[2] && lotRect[0] <= rect.x1 &&
                              rect.z0 <= lotRect[3] && lotRect[1] <= rect.z1;
        if (!overlaps) return;
        if (mask != 0xf) {
            if (z == top && (mask & 1) == 0) return;
            if (x == right && (mask & 2) == 0) return;
            if (z == bottom && (mask & 4) == 0) return;
            if (x == left && (mask & 8) == 0) return;
        }
        for (const auto& held : out)
            if (held.get() == lot) return;
        out.emplace_back(lot);
        ++count;
    };

    if (top <= ringBottom && left <= ringRight) {
        for (int x = left; x <= ringRight; ++x) consider(x, top);
        for (int z = top + 1; z < ringBottom; ++z) {
            consider(left, z);
            if (right != left) consider(right, z);
        }
        if (ringBottom != top)
            for (int x = left; x <= ringRight; ++x) consider(x, ringBottom);
    }
    return count;
}

// @ 0x100131946. The four vertices of one side of the lot rectangle (edge 1 north, 2 east, 4 south,
// 8 west); other bits leave the output untouched, like the original.
void cSC4ZoneDeveloper::GetVerticesAtEdgeOfLot(const SC4Rect<int>& rect, int edge, SC4Rect<int>& out) const {
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    if ((edge & 1) == 0) {
        if ((edge & 2) == 0) {
            if ((edge & 4) != 0) {
                x0 = rect.x0;
                x1 = rect.x1 + 1;
                z0 = rect.z1 + 1;
                z1 = rect.z1 + 1;
            } else if ((edge & 8) != 0) {
                x0 = rect.x0;
                z0 = rect.z0;
                x1 = rect.x0;
                z1 = rect.z1 + 1;
            } else {
                return;
            }
        } else {
            x0 = rect.x1 + 1;
            z0 = rect.z0;
            x1 = rect.x1 + 1;
            z1 = rect.z1 + 1;
        }
    } else {
        x0 = rect.x0;
        z0 = rect.z0;
        x1 = rect.x1 + 1;
        z1 = rect.z0;
    }
    out = {x0, z0, x1, z1};
}

// @ 0x1001319a6. The cells on the other side of one lot edge, clamped to the city; the function
// answers false when the edge is outside the city (the output is then {0,0,-1,-1}).
bool cSC4ZoneDeveloper::GetNeighborCellsAtEdgeOfLot(const SC4Rect<int>& rect, int edge,
                                                    SC4Rect<int>& out) const {
    out = {0, 0, -1, -1};
    if ((edge & 1) == 0) {
        if ((edge & 2) == 0) {
            if ((edge & 4) == 0) {
                if ((edge & 8) != 0 && rect.x0 > 0) {
                    out = {rect.x0 - 1, rect.z0, rect.x0 - 1, rect.z1};
                }
            } else if (rect.z1 < cellsZ_ - 1) {
                out = {rect.x0, rect.z1 + 1, rect.x1, rect.z1 + 1};
            }
        } else if (rect.x1 < cellsX_ - 1) {
            out = {rect.x1 + 1, rect.z0, rect.x1 + 1, rect.z1};
        }
    } else if (rect.z0 > 0) {
        out = {rect.x0, rect.z0 - 1, rect.x1, rect.z0 - 1};
    }
    return out.x0 <= out.x1;
}

// @ 0x100131a5e. Looks for a boundary lot in the four cells around the vertex (x, z), in the order
// (x-1,z-1), (x-1,z), (x,z-1), (x,z); falls back to the terrain cell altitude (vslot 0x70).
float cSC4ZoneDeveloper::GetLotSavvyTerrainVertexAltitude(int x, int z, const cSC4Lot* exclude,
                                                          bool* lotFound) const {
    if (lotFound) *lotFound = true;
    const cSC4Lot* found = nullptr;
    if (x > 0 && z > 0) {
        const cSC4Lot* lot = LotAt(const_cast<std::vector<std::vector<cSC4Lot*>>&>(lotMap_), x - 1, z - 1);
        if (IsBoundaryLot(lot) && lot != exclude) found = lot;
    }
    if (!found && z > 0 && x < cellsX_) {
        const cSC4Lot* lot = LotAt(const_cast<std::vector<std::vector<cSC4Lot*>>&>(lotMap_), x, z - 1);
        if (IsBoundaryLot(lot) && lot != exclude) found = lot;
    }
    if (!found && x > 0 && z < cellsZ_) {
        const cSC4Lot* lot = LotAt(const_cast<std::vector<std::vector<cSC4Lot*>>&>(lotMap_), x - 1, z);
        if (IsBoundaryLot(lot) && lot != exclude) found = lot;
    }
    if (!found && x < cellsX_ && z < cellsZ_) {
        const cSC4Lot* lot = LotAt(const_cast<std::vector<std::vector<cSC4Lot*>>&>(lotMap_), x, z);
        if (IsBoundaryLot(lot) && lot != exclude) found = lot;
    }
    if (found) return found->GetAltitude();   // cISC4Lot vslot 0x80
    if (lotFound) *lotFound = false;
    return terrain_ ? terrain_->CellAltitude(x, z) : 0.0f;
}

// @ 0x100131c10. Clamps the world point to the city, then answers the boundary lot's altitude or the
// terrain altitude (vslot 0x60).
float cSC4ZoneDeveloper::GetLotSavvyTerrainAltitude(float x, float z, bool* lotFound) const {
    int cellX = WorldToCell(x, -0.499f);
    int cellZ = WorldToCell(z, -0.499f);
    if (static_cast<unsigned>(cellX) >= static_cast<unsigned>(cellsX_)) cellX = Clamp(cellX, 0, cellsX_ - 1);
    if (static_cast<unsigned>(cellZ) >= static_cast<unsigned>(cellsZ_)) cellZ = Clamp(cellZ, 0, cellsZ_ - 1);
    const cSC4Lot* lot = LotAt(const_cast<std::vector<std::vector<cSC4Lot*>>&>(lotMap_), cellX, cellZ);
    if (IsBoundaryLot(lot)) {
        if (lotFound) *lotFound = true;
        return lot->GetAltitude();
    }
    if (lotFound) *lotFound = false;
    return terrain_ ? terrain_->Altitude(x, z) : 0.0f;
}

// @ 0x100131d20. The boundary lot's altitude, or the terrain altitude at the cell centre (vslot 0x60).
float cSC4ZoneDeveloper::GetLotSavvyCellAltitude(int x, int z, bool* lotFound) const {
    const cSC4Lot* lot = LotAt(const_cast<std::vector<std::vector<cSC4Lot*>>&>(lotMap_), x, z);
    if (IsBoundaryLot(lot)) {
        if (lotFound) *lotFound = true;
        return lot->GetAltitude();
    }
    if (lotFound) *lotFound = false;
    return terrain_ ? terrain_->Altitude((static_cast<float>(x) + 0.5f) * 16.0f,
                                         (static_cast<float>(z) + 0.5f) * 16.0f)
                    : 0.0f;
}

// @ 0x10012c1d0. The distinct lots in the 2x2 cell block [x-1..x] x [z-1..z]; a lot beyond its first
// state (cISC4Lot::GetState, vslot 0x108) forces both bounds to 32.0.
void cSC4ZoneDeveloper::GetLotVertexAltitudeAdjustmentRange(int x, int z, float& minAltitude,
                                                            float& maxAltitude) {
    const int x0 = std::max(x - 1, 0);
    const int z0 = std::max(z - 1, 0);
    const int x1 = std::min(x, cellsX_ - 1);
    const int z1 = std::min(z, cellsZ_ - 1);
    minAltitude = FLT_MAX;
    maxAltitude = FLT_MAX;
    if (x1 < x0) return;
    cSC4Lot* found[4] = {nullptr, nullptr, nullptr, nullptr};
    int foundCount = 0;
    for (int cx = x0; cx <= x1; ++cx) {
        for (int cz = z0; cz <= z1; ++cz) {
            cSC4Lot* lot = LotAt(lotMap_, cx, cz);
            if (!lot) continue;
            bool seen = false;
            for (int i = 0; i < foundCount; ++i)
                if (found[i] == lot) seen = true;
            if (seen) continue;
            if (foundCount < 4) found[foundCount++] = lot;
        }
    }
    for (int i = 0; i < foundCount; ++i) {
        if (found[i]->GetState() > 1) {   // cISC4Lot vslot 0x108
            minAltitude = 32.0f;
            maxAltitude = 32.0f;
        }
    }
}

//---------------------------------------------------------------------------------------------------------------------
// network / water queries
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100130604. Lists the network occupants of the cell (occupant manager vslot 0x140, type
// 0xc772bf98) and returns the one whose network flag set contains `flags`. The returned pointer is
// borrowed, like the original (the QI reference is released before returning).
IZoneDeveloperNetworkOccupant* cSC4ZoneDeveloper::GetNetwork(int x, int z, std::uint32_t flags) {
    if (!occupantManager_) return nullptr;
    std::vector<cISC4Occupant*> occupants;
    occupantManager_->ListOccupants(occupants, x, z, kZoneOccupantTypeNetwork, -1);
    IZoneDeveloperNetworkOccupant* result = nullptr;
    for (cISC4Occupant* occupant : occupants) {
        if (!occupant) continue;
        // QueryInterface(cISC4NetworkOccupant); the original releases the QI reference even for the chosen occupant
        IZoneDeveloperNetworkOccupant* view = occupantManager_->NetworkView(occupant);
        if (!view) continue;
        if (view->HasAnyNetworkFlag(flags)) {
            result = view;
            break;
        }
    }
    for (cISC4Occupant* occupant : occupants)
        if (occupant) occupant->Release();
    return result;
}

// @ 0x100130f02. A lot-manager network occupant first; while the lot changes are deferred the road
// (flag 8) and rail (flag 0x800) sub-network cell flags are consulted instead.
bool cSC4ZoneDeveloper::ExistsNetworkOfType(int x, int z, std::uint32_t flags) {
    if (GetNetwork(x, z, flags)) return true;
    if (lotChangesEnabled_) return false;
    IZoneDeveloperNetworkApi* network = nullptr;
    std::uint32_t mask = 0;
    if ((flags & 8) != 0) {
        network = streetTool_;
        mask = 8;
    } else if ((flags & 0x800) != 0) {
        network = dirtRoadTool_;
        mask = 0x800;
    } else {
        return false;
    }
    if (!network) return false;
    std::uint32_t cellFlags = 0;
    return network->GetCellFlags(x, z, mask, cellFlags);
}

// @ 0x100130274. Direction 0..3 maps to {1, 2, 3, 0}; a network occupant that reports the connection
// answers directly, else (in the deferred mode) the road and rail cell flags are tested with the
// direction byte.
bool cSC4ZoneDeveloper::ExistsConnectionOfType(int x, int z, std::uint32_t flags, int direction,
                                               int networkType) {
    const std::uint32_t directionFlag =
        static_cast<unsigned>(direction) < 4 ? kDirectionFlags[direction] : 0xffffffffu;
    if (IZoneDeveloperNetworkOccupant* network = GetNetwork(x, z, flags)) {
        if (network->ConnectsToNeighbor(directionFlag, networkType)) return true;
    }
    if ((flags & 0x808) == 0 || lotChangesEnabled_) return false;
    std::uint32_t cellFlags = 0;
    bool found = false;
    if (streetTool_) found = streetTool_->GetCellFlags(x, z, 8, cellFlags);
    if (!found && dirtRoadTool_) found = dirtRoadTool_->GetCellFlags(x, z, 0x800, cellFlags);
    if (!found) return false;
    return (cellFlags & (0xffu << ((directionFlag & 3) << 3))) != 0;
}

// @ 0x1001308ea. Tail call to the terrain water query (vslot 0x110).
bool cSC4ZoneDeveloper::IsCellWater(int x, int z) { return terrain_ ? terrain_->IsCellWater(x, z) : false; }

// @ 0x1001303ea. Classifies one cell of the parcellization boundary and adds its cost:
//   - a street (0x1f4f) cell: 5 for diagonal/road (0x104), 30 for rail (0x808), 100 for a road (0x401),
//     400 for a power line (0x40), 5 for a crossing (0x2); no other street cell costs anything;
//   - no lot and no water: 5 when a building occupant (0x278128a0 / 0x2890d4de) sits on the cell;
//   - a lot or water: 5.
// An unclassified (0) boundary costs 1.
void cSC4ZoneDeveloper::ExamineCell(int x, int z, BoundaryCondition& boundary, int& cost) {
    auto hasNetwork = [&](std::uint32_t flags) { return ExistsNetworkOfType(x, z, flags); };
    if (hasNetwork(kNetworkFlagAnyConnection)) {
        if (hasNetwork(kNetworkFlagStreet)) {
            boundary = kBoundaryBlocked;
            cost += 5;
        } else if (hasNetwork(kNetworkFlagRail)) {
            if (static_cast<int>(boundary) < 1) boundary = kBoundarySoft;
            cost += 0x1e;
        } else if (hasNetwork(kNetworkFlagRoad)) {
            if (static_cast<int>(boundary) < 1) boundary = kBoundarySoft;
            cost += 100;
        } else if (hasNetwork(kNetworkFlagPowerLine)) {
            if (static_cast<int>(boundary) < 1) boundary = kBoundarySoft;
            cost += 400;
        } else if (hasNetwork(2)) {
            boundary = kBoundaryBlocked;
            cost += 5;
        }
    } else if (!GetLot(x, z, false) && !IsCellWater(x, z)) {
        if (occupantManager_) {
            std::vector<cISC4Occupant*> occupants;
            occupantManager_->ListOccupants(occupants, x, z, 0, -1);
            for (cISC4Occupant* occupant : occupants) {
                if (!occupant) continue;
                const std::uint32_t type = occupant->GetType();   // cISC4Occupant vslot 0x38
                if (type == kZoneOccupantTypeBuilding || type == kZoneOccupantTypeBlocking2) {
                    boundary = kBoundaryBlocked;
                    cost += 5;
                }
            }
            for (cISC4Occupant* occupant : occupants)
                if (occupant) occupant->Release();
        }
    } else {
        boundary = kBoundaryBlocked;
        cost += 5;
    }
    if (boundary == kBoundaryNone) cost += 1;
}

//---------------------------------------------------------------------------------------------------------------------
// lot creation
//---------------------------------------------------------------------------------------------------------------------

// @ 0x1001307bc. 0 out of the city, 1 water (unless allowed), 2 zone/region mismatch, 3 network on the
// cell, 7/8 when a lot already exists.
int cSC4ZoneDeveloper::CanCreateLotOnCell(int x, int z, bool skipZoneCheck, bool allowWater) {
    if (x < 0 || z < 0 || x >= cellsX_ || z >= cellsZ_) return 0;
    if (currentRegion_) {
        const std::uint32_t dx = static_cast<std::uint32_t>(x - currentRegion_->bounds.x0);
        if (dx >= currentRegion_->rowCount) return 0;
        const std::uint32_t dz = static_cast<std::uint32_t>(z - currentRegion_->bounds.z0);
        if (dz >= currentRegion_->bitCount) return 0;
        if (dx >= currentRegion_->rows.size() || (dz >> 5) >= currentRegion_->rows[dx].size()) return 0;
        if (((currentRegion_->rows[dx][dz >> 5] >> (dz & 0x1f)) & 1u) == 0) return 0;
    }
    if (!allowWater && IsCellWater(x, z)) return 1;
    if (!lotChangesEnabled_ && !skipZoneCheck) {
        const int zoneType = zoneManager_ ? zoneManager_->ZoneTypeAt(x, z) : 0;
        if (currentZoneType_ == 0) {
            if (!zoneManager_ || !zoneManager_->IsZoneTypeValid(zoneType)) return 2;
        } else if (zoneType != currentZoneType_) {
            return 2;
        }
    }
    if (ExistsNetworkOfType(x, z, kNetworkFlagAnyConnection)) return 3;
    return 8 - (DoesCellHaveLot(x, z) ? 1 : 0);
}

// @ 0x100131de6. The minimum cell code over the footprint; 8 means "can place".
int cSC4ZoneDeveloper::CanCreateLot(int x, int z, int sizeX, int sizeZ, bool skipZoneCheck,
                                    bool allowWaterAlways, cSC4LotConfiguration* configuration) {
    bool allowWater = true;
    if (!allowWaterAlways) {
        allowWater = configuration != nullptr && configuration->IsCompatibleWithWater();   // vslot 0x70
    }
    int result = 8;
    for (int cx = x; cx < x + sizeX; ++cx) {
        for (int cz = z; cz < z + sizeZ; ++cz) {
            const int cell = CanCreateLotOnCell(cx, cz, skipZoneCheck, allowWater);
            if (cell < result) result = cell;
        }
    }
    return result;
}

// @ 0x100131ecc. PARTIAL/SKIPPED: the plopped-lot path needs DecodeNetworkConnectionObject
// (0x100132a04) and the network occupant edge flags; only the "cannot place" answer is produced.
int cSC4ZoneDeveloper::CanCreatePloppedLot(SC4Rect<int>, int, cSC4LotConfiguration*) { return 8; }

// @ 0x100132b32. In the deferred mode the placement is recorded in the pending-lot map (+0x178); in
// the normal mode it creates the lot, either through CreateLot or (facing == -1) through
// CreateLotToFaceRoad with an empty facing list.
bool cSC4ZoneDeveloper::CreateLotIfPossible(int x, int z, int sizeX, int sizeZ, int facing,
                                            cSC4Lot*& out, bool param7, bool param8,
                                            cSC4LotConfiguration* configuration) {
    out = nullptr;
    if (CanCreateLot(x, z, sizeX, sizeZ, param7, param8, configuration) != 8) return false;
    if (!lotChangesEnabled_) {
        for (int cx = x; cx < x + sizeX; ++cx) {
            for (int cz = z; cz < z + sizeZ; ++cz) {
                // The pending map is keyed by cell and carries the placement's origin and facing; the
                // original builds the key with the cell (cx, cz) and stores {x, z} + facing.
                PendingLot placement{SC4Point<int>{x, z}, facing};
                pendingLots_[SC4Point<int>{cx, cz}] = placement;
            }
        }
        return true;
    }
    if (facing == -1) {
        // The original passes an empty facing list, so CreateLotToFaceRoad picks a random start.
        std::list<int> facings;
        CreateLotToFaceRoad(x, z, sizeX, sizeZ, facings, out);
    } else {
        CreateLot(x, z, sizeX, sizeZ, facing, out);
    }
    return out != nullptr;
}

// @ 0x100132cde. Allocates the lot, takes the caller's and the lot list's references, calls
// cISC4Lot::Init (vslot 0x28) and SetState(UnderConstruction, 0, 0) (vslot 0x110), fills every cell of
// the footprint and notifies the safe list / building development simulator.
bool cSC4ZoneDeveloper::CreateLot(int x, int z, int sizeX, int sizeZ, int facing, cSC4Lot*& out) {
    auto* lot = new cSC4Lot(x, z, sizeX, sizeZ, facing);
    if (std::getenv("SC4_SIMDEBUG")) std::fprintf(stderr, "zone developer: CreateLot (%d,%d) %dx%d facing %d\n", x, z, sizeX, sizeZ, facing);
    out = lot;
    lot->AddRef();      // the caller's reference
    lot->Init();
    lot->SetState(1, 0, 0);
    int maxX = x - 1 + sizeX;
    int maxZ = z - 1 + sizeZ;
    if (maxX < x) maxX = x;
    if (maxZ < z) maxZ = z;
    lot->AddRef();      // the developer's lot-list reference
    for (int cx = x; cx <= maxX; ++cx)
        for (int cz = z; cz <= maxZ; ++cz)
            if (cx >= 0 && cz >= 0 && cx < cellsX_ && cz < cellsZ_)
                lotMap_[static_cast<std::size_t>(cx)][static_cast<std::size_t>(cz)] = lot;
    if (g_host) {
        if (IZoneDeveloperSafeListApi* iterator = g_host->SafeListIterator()) {
            // The live iterator inserts the lot at its current position; the count is maintained by the
            // iterator, so it is not bumped here (like the original).
            iterator->AddAfter(lot);
        } else {
            lots_.push_front(lot);
            ++lotCount_;
        }
    } else {
        lots_.push_front(lot);
        ++lotCount_;
    }
    // SL::spBuildingDevelopmentSim vslot 0x120 is the lot developer; its vslot 0x158 gives the new lot its empty base texture.
    if (buildingDevelopment_)
        if (IZoneDeveloperLotDeveloperApi* developer = buildingDevelopment_->GetLotDeveloper()) developer->SetEmptyLotBaseTexture(lot);
    return out != nullptr;
}

//---------------------------------------------------------------------------------------------------------------------
// lot deletion
//---------------------------------------------------------------------------------------------------------------------

// @ 0x10012be32. With lot changes enabled: detach from the lot developer, take the lot's base-texture
// occupant out of the occupant manager, clear the footprint, drop the lot-list reference, let the lot
// developer release the lot's unit (when it has one), shut it down and release the caller's reference. While the changes are deferred the
// footprint is only recorded in the pending-deletion map (+0x190, cell -> {origin, zone type}).
void cSC4ZoneDeveloper::DeleteLot(cSC4Lot* lot) {
    if (!lot) return;
    std::int32_t rawRect[4] = {0, 0, 0, 0};
    lot->GetBoundingRect(rawRect);
    int x0 = rawRect[0], z0 = rawRect[1], x1 = rawRect[2], z1 = rawRect[3];
    if (x1 <= x0) x1 = x0;
    if (z1 < z0) z1 = z0;

    if (lotChangesEnabled_) {
        if (lotDeveloper_) lotDeveloper_->RemoveLot(lot);   // +0x90 vslot 0x58
        // the lot's base texture leaves the occupant manager: building development vslot 0x120 (the lot developer) -> 0x148
        IZoneDeveloperLotDeveloperApi* developer = buildingDevelopment_ ? buildingDevelopment_->GetLotDeveloper() : nullptr;
        cISC4Occupant* texture = nullptr;
        if (developer && developer->GetLotBaseTextureOccupant(lot, texture) && texture) {
            if (occupantManager_) occupantManager_->RemoveOccupant(texture, true, 0);   // vslot 0xa8
            texture->Release();
        }
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                if (x >= 0 && z >= 0 && x < cellsX_ && z < cellsZ_)
                    lotMap_[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = nullptr;
        // cSC4SafeList::Remove only unlinks the lot: the developer's reference is the one the final
        // Release below consumes (the original deletes the object there, 0x10012c0a6 does not release).
        auto it = std::find(lots_.begin(), lots_.end(), lot);
        if (it != lots_.end()) {
            lots_.erase(it);
            if (lotCount_ > 0) --lotCount_;   // cSC4SafeList::Remove keeps the +0xe0 count
        }
        if (lot->GetUnit() != nullptr && developer) developer->ReleaseLotUnit(lot);   // 0x120 -> 0x108
        lot->Shutdown();    // cISC4Lot vslot 0x38
        lot->Release();     // vslot 0x10: the caller's reference
        return;
    }

    const std::uint32_t zoneType = lot->GetZoneType();
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z)
            pendingDeletes_[SC4Point<int>{x, z}] = PendingDelete{SC4Point<int>{x0, z0},
                                                                 static_cast<int>(zoneType)};
}

// @ 0x10012c130. Collects the lots touching the rectangle and deletes each; the temporary list holds a
// reference, so the last release is the list's.
void cSC4ZoneDeveloper::DeleteLots(int x0, int z0, int x1, int z1) {
    std::list<cRZAutoRefCount<cSC4Lot>> lots;
    if (GetLotsInCellRect(lots, x0, z0, x1, z1, false) != 0) {
        for (const auto& lot : lots) DeleteLot(lot.get());
    }
}

// @ 0x10012b0aa. Clears both pending maps and the highlight of the two road networks and the terrain
// view (vslot 0x28 of each network, 0x48 of the terrain view).
void cSC4ZoneDeveloper::ClearHighlight() {
    pendingLots_.clear();
    pendingDeletes_.clear();
    if (streetTool_) streetTool_->ClearHighlight();
    if (roadTool_) roadTool_->ClearHighlight();
    if (terrainView_) terrainView_->ClearHighlight();
}

//---------------------------------------------------------------------------------------------------------------------
// options / zoning entry points
//---------------------------------------------------------------------------------------------------------------------

// @ 0x100133440. The original stores the first argument at +0x12f, the second at +0x12e and the third
// at +0x130 (SetOptions(swap orientation, extend as streets, examine boundaries)).
void cSC4ZoneDeveloper::SetOptions(bool swapOrientation, bool extendAsStreets, bool examineBoundaries) {
    swapOrientation_ = swapOrientation ? 1 : 0;
    extendAsStreets_ = extendAsStreets ? 1 : 0;
    examineBoundaries_ = examineBoundaries ? 1 : 0;
}

// @ 0x100130914. PARTIAL: the flag bookkeeping and the zone-validity test are recovered; OnZone and
// UpdateHighlightTextures are part of the unrecovered parcellization pipeline.
void cSC4ZoneDeveloper::HighlightParcels(const SC4CellRegion<int>& region, int zoneType,
                                         const SC4Point<int>* origin, SC4CellRegion<int>** out) {
    lotChangesEnabled_ = 0;
    currentZoneType_ = zoneType;
    highlightOrigin_ = const_cast<SC4Point<int>*>(origin);
    if (zoneManager_ && zoneManager_->IsZoneTypeValid(zoneType)) {
        ClearHighlight();
        OnZone(&region, out);
        UpdateHighlightTextures(&region);
    }
    lotChangesEnabled_ = 1;
    currentZoneType_ = 0;
    highlightOrigin_ = nullptr;
}

// @ 0x10012b046. PARTIAL for the same reason: both branches call the unrecovered parcellization.
void cSC4ZoneDeveloper::DoParcellization(const SC4CellRegion<int>& region, int zoneType, bool arg) {
    currentZoneType_ = zoneType;
    if (!zoneManager_ || !zoneManager_->IsZoneTypeValid(zoneType)) {
        OnDezone(&region, arg);
    } else {
        ClearHighlight();
        OnZone(&region, nullptr);
    }
    currentZoneType_ = 0;
}

//---------------------------------------------------------------------------------------------------------------------
// parcellization pipeline — helpers, region subdivision and lot layout
//---------------------------------------------------------------------------------------------------------------------

namespace {

// cSC4NetworkTool's kFlagForNetworkType @ 0x100a19aa0, one bit per cISC4NetworkManager::eNetworkType.
constexpr std::uint32_t kNetworkTypeFlag[13] = {0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040,
                                                0x0080, 0x0100, 0x0200, 0x0400, 0x0800, 0x1000};
std::uint32_t FlagForNetworkType(std::uint32_t type) {
    return type < 13 ? kNetworkTypeFlag[type] : 0;
}

// The eight bit-flag combinations ExamineBoundaries / DetermineLotSize pass to the zone and network
// queries (0xc49 = every street-like network, 0x808 = the rail network).
constexpr std::uint32_t kNetworkFlagRailOnly = 0x808;

// cSC4BuildingOccupant::ConvertZoneToDensity (0x10019b9dc) is recovered in BuildingOccupant.cpp,
// which this translation unit does not link; the same 1..9 -> 1,2,3,1,2,3,3,3,3 mapping is repeated
// here for DetermineLotSize (see the header of BuildingOccupant.cpp).
std::uint32_t ZoneToDensity(int zoneType) {
    static const std::uint32_t kDensity[9] = {1, 2, 3, 1, 2, 3, 3, 3, 3};
    return static_cast<std::uint32_t>(zoneType - 1) < 9u ? kDensity[zoneType - 1] : 0u;
}

bool RegionHas(const SC4CellRegion<int>* region, int x, int z) {
    return region != nullptr && region->Test(x, z);
}

int AbsInt(int value) { return value < 0 ? -value : value; }

}  // namespace

// @ 0x10012feac. Builds the mask of region cells that still hold no lot (or a lot whose zone no longer
// matches), lets the zone manager clear the zone type of exactly those cells, and refreshes the base
// texture of every lot whose zone changed. The zone manager call is vslot 0x58 PlaceZone(region,
// zoneType 0, true, true, false, true, false, null, null, null).
void cSC4ZoneDeveloper::UpdateZoneType(const SC4CellRegion<int>* region) {
    if (!region || !lotDeveloper_) return;
    SC4CellRegion<int> empty = SC4CellRegion<int>::Make(region->bounds, false);
    const bool forceEmptyTexture = lotDeveloper_->GetForceEmptyLotBaseTextures();   // vslot 0x170
    bool anyEmpty = false;
    for (int x = region->bounds.x0; x <= region->bounds.x1; ++x) {
        for (int z = region->bounds.z0; z <= region->bounds.z1; ++z) {
            if (!region->Test(x, z)) continue;
            cSC4Lot* lot = GetLot(x, z, false);   // primary vslot 0x80
            if (!lot) {
                empty.SetBit(x, z);
                anyEmpty = true;
            } else {
                const int before = static_cast<int>(lot->GetZoneType());   // 0xf8
                lot->UpdateZoneType();                                     // 0x100
                const int after = static_cast<int>(lot->GetZoneType());
                if (after != before && (forceEmptyTexture || lot->GetState() < 2)) {   // 0x108
                    lotDeveloper_->SetEmptyLotBaseTexture(lot);            // vslot 0x158
                }
            }
        }
    }
    if (anyEmpty && zoneManager_) {
        zoneManager_->PlaceZone(empty, 0, true, true, false, true, false);   // vslot 0x58
    }
}

// @ 0x10012d184. The mask of region cells that can take a lot: still zoned cells that hold no
// incompatible lot; an existing lot is deleted when its configuration does not accept the new zone
// (the 1..6 zone/lot range check when the lot has no configuration object).
SC4CellRegion<int>* cSC4ZoneDeveloper::CreateParcellizableRegion(const SC4CellRegion<int>* region) {
    if (!region) return nullptr;
    auto* result = new SC4CellRegion<int>(SC4CellRegion<int>::Make(region->bounds, false));
    for (int x = region->bounds.x0; x <= region->bounds.x1; ++x) {
        for (int z = region->bounds.z0; z <= region->bounds.z1; ++z) {
            if (!region->Test(x, z)) continue;
            if (currentZoneType_ != 0 && ExistsNetworkOfType(x, z, kNetworkFlagAnyConnection)) continue;
            if (IsCellWater(x, z)) continue;
            bool parcellizable = true;
            if (lotChangesEnabled_ || DoesCellHaveLot(x, z)) {
                if (cSC4Lot* lot = GetLot(x, z, false)) {
                    cISC4LotConfigurationRef* configuration = lot->GetLotConfiguration();   // vslot 0x48
                    bool compatible;
                    if (!configuration) {
                        compatible = static_cast<unsigned>(currentZoneType_ - 1) < 6 &&
                                     static_cast<unsigned>(static_cast<int>(lot->GetZoneType()) - 1) < 6;
                    } else {
                        compatible = g_host && g_host->LotConfigurationCompatibleWithZone(
                                                   configuration, currentZoneType_);
                    }
                    if (compatible) {
                        parcellizable = false;   // the cell keeps its lot
                    } else {
                        DeleteLot(lot);          // primary vslot 0x118
                    }
                }
            }
            if (parcellizable) result->SetBit(x, z);
        }
    }
    return result;
}

// @ 0x10012d3ac. Shrinks the region to the bounding box of its set bits; an empty region and a full
// region are returned unchanged. The old region is destroyed and replaced.
void cSC4ZoneDeveloper::CompressRegion(SC4CellRegion<int>*& region) {
    if (!region) return;
    SC4CellRegion<int>* in = region;
    int minRow = INT_MAX, maxRow = INT_MIN, minBit = INT_MAX, maxBit = INT_MIN;
    for (std::uint32_t r = 0; r < in->rowCount; ++r) {
        for (std::uint32_t b = 0; b < in->bitCount; ++b) {
            if (((in->rows[r][b >> 5] >> (b & 0x1f)) & 1u) == 0) continue;
            minRow = std::min(minRow, static_cast<int>(r));
            maxRow = std::max(maxRow, static_cast<int>(r));
            minBit = std::min(minBit, static_cast<int>(b));
            maxBit = std::max(maxBit, static_cast<int>(b));
        }
    }
    if (minRow == INT_MAX) {
        minRow = maxRow = minBit = maxBit = 0;   // empty: the original uses {0,0,0,0}
    }
    if (static_cast<int>(in->bitCount) - 1 <= maxBit && static_cast<int>(in->rowCount) - 1 <= maxRow &&
        minBit < 1 && minRow < 1) {
        return;   // already tight
    }
    SC4Rect<int> bounds;
    bounds.x0 = in->bounds.x0 + minRow;
    bounds.z0 = in->bounds.z0 + minBit;
    bounds.x1 = in->bounds.x0 + maxRow;
    bounds.z1 = in->bounds.z0 + maxBit;
    auto* out = new SC4CellRegion<int>(SC4CellRegion<int>::Make(bounds, true));
    for (int r = 0; r < static_cast<int>(out->rowCount); ++r) {
        for (int b = 0; b < static_cast<int>(out->bitCount); ++b) {
            const int sr = minRow + r, sb = minBit + b;
            const bool set = static_cast<std::uint32_t>(sr) < in->rowCount &&
                             static_cast<std::uint32_t>(sb) < in->bitCount &&
                             ((in->rows[sr][sb >> 5] >> (sb & 0x1f)) & 1u) != 0;
            if (set) out->SetBit(out->bounds.x0 + r, out->bounds.z0 + b);
            else out->ClearBit(out->bounds.x0 + r, out->bounds.z0 + b);
        }
    }
    delete in;
    region = out;
}

// @ 0x10012fa04. Flood-fills the connected component of the seed cell (row, bit) with a four-way
// neighbourhood, removes it from the source mask (the source bit is toggled) and returns it as a new
// region with the same bounds. The seed is set in the result and the rest of the result follows the
// fill value the original passes to the cRZCellMap constructor.
SC4CellRegion<int>* cSC4ZoneDeveloper::ExtractConnectedCells(SC4CellRegion<int>* region, int row, int bit) {
    if (!region) return nullptr;
    const std::uint32_t seedRow = static_cast<std::uint32_t>(row);
    const std::uint32_t seedBit = static_cast<std::uint32_t>(bit);
    if (seedRow >= region->rowCount || seedBit >= region->bitCount) return nullptr;
    if ((seedBit >> 5) >= region->rows[seedRow].size()) return nullptr;
    const bool seed = ((region->rows[seedRow][seedBit >> 5] >> (seedBit & 0x1f)) & 1u) != 0;
    region->ToggleBit(region->bounds.x0 + static_cast<int>(seedRow),
                      region->bounds.z0 + static_cast<int>(seedBit));
    auto* result = new SC4CellRegion<int>(SC4CellRegion<int>::Make(region->bounds, !seed));
    if (seed) result->SetBit(region->bounds.x0 + static_cast<int>(seedRow),
                             region->bounds.z0 + static_cast<int>(seedBit));
    else result->ClearBit(region->bounds.x0 + static_cast<int>(seedRow),
                          region->bounds.z0 + static_cast<int>(seedBit));
    std::list<std::pair<int, int>> queue;
    queue.push_back({static_cast<int>(seedRow), static_cast<int>(seedBit)});
    while (!queue.empty()) {
        const auto [r, b] = queue.front();
        queue.pop_front();
        const int neighbours[4][2] = {{r + 1, b}, {r, b + 1}, {r - 1, b}, {r, b - 1}};
        for (const auto& neighbour : neighbours) {
            const int nr = neighbour[0], nb = neighbour[1];
            if (nr < 0 || nb < 0) continue;
            if (static_cast<std::uint32_t>(nr) >= region->rowCount) continue;
            if (static_cast<std::uint32_t>(nb) >= region->bitCount) continue;
            if (static_cast<std::uint32_t>(nb >> 5) >= region->rows[nr].size()) continue;
            if ((((region->rows[nr][nb >> 5] >> (nb & 0x1f)) & 1u) != 0) != seed) continue;
            // mark visited in the source (to !seed) and copy into the result (to seed)
            region->rows[nr][nb >> 5] ^= 1u << (nb & 0x1f);
            if (seed) result->SetBit(region->bounds.x0 + nr, region->bounds.z0 + nb);
            else result->ClearBit(region->bounds.x0 + nr, region->bounds.z0 + nb);
            queue.push_back({nr, nb});
        }
    }
    return result;
}

// @ 0x10012ecc2. Splits the parcellizable cells of the region into one compressed region per connected
// component and appends each to the caller's list; when nothing is parcellizable the whole (compressed)
// parcellizable region is appended instead.
void cSC4ZoneDeveloper::SubdivideTheZone(const SC4CellRegion<int>* region,
                                         std::list<SC4CellRegion<int>*>& out) {
    SC4CellRegion<int>* parcellizable = CreateParcellizableRegion(region);
    if (!parcellizable) return;
    for (std::uint32_t r = 0; r < parcellizable->rowCount; ++r) {
        for (std::uint32_t b = 0; b < parcellizable->bitCount; ++b) {
            if (((parcellizable->rows[r][b >> 5] >> (b & 0x1f)) & 1u) == 0) continue;
            SC4CellRegion<int>* connected =
                ExtractConnectedCells(parcellizable, static_cast<int>(r), static_cast<int>(b));
            if (!connected) continue;
            CompressRegion(connected);
            out.push_back(connected);
        }
    }
    if (out.empty()) {
        CompressRegion(parcellizable);
        out.push_back(parcellizable);
    } else {
        delete parcellizable;
    }
}

// @ 0x10012d606. Chooses the lot footprint from the best growth developer for the current zone: the
// developer with the highest desirability estimate (ties broken by its active demand), its growth-list
// style, the lot-configuration filter {purpose, wealth, growth stage} and the building capacity of the
// configurations' primary building families. Writes parcellizationSizes_: [0] size x, [1] size z,
// [2] minimum z, [3] maximum z (the lot-depth range Parcellize uses) and [4] the density gap.
void cSC4ZoneDeveloper::DetermineLotSize(const SC4Rect<int>& rect) {
    if (!buildingDevelopment_) return;
    const int density = static_cast<int>(ZoneToDensity(currentZoneType_));
    const int centerX = (rect.x0 + rect.x1) / 2;
    const int centerZ = (rect.z0 + rect.z1) / 2;
    IZoneDeveloperGrowthApi* bestDev = nullptr;
    int bestScore = 0;
    int bestDemand = 0;
    const auto desirability = [&](IZoneDeveloperGrowthApi* developer) -> int {
        return developer ? developer->EstimateDesirability(centerX, centerZ) : 0;
    };
    const auto activeDemand = [](IZoneDeveloperGrowthApi* developer) -> int {
        if (!developer || !developer->SourceDemand()) return 0;
        return static_cast<int>(developer->SourceDemand()->ActiveDemandValue());   // vslot 0x60
    };
    if (static_cast<unsigned>(currentZoneType_ - 1) < 3) {
        // residential: 0x1010, 0x1020, 0x1030 pick the highest estimate, ties by demand
        bestDev = buildingDevelopment_->GetGrowthDeveloper(0x1010);   // vslot 0x108
        bestScore = desirability(bestDev);
        bestDemand = activeDemand(bestDev);
        for (std::uint32_t type : {0x1020u, 0x1030u}) {
            IZoneDeveloperGrowthApi* developer = buildingDevelopment_->GetGrowthDeveloper(type);
            if (!developer) continue;
            const int score = desirability(developer);
            const int demand = activeDemand(developer);
            if (score > bestScore || (score == bestScore && bestDemand < demand)) {
                bestDev = developer;
                bestScore = score;
                bestDemand = demand;
            }
        }
    } else if (currentZoneType_ == 4 || currentZoneType_ == 5 || currentZoneType_ == 6) {
        // commercial: 0x3110, 0x3120, 0x3130, 0x3320, 0x3330
        bestDev = buildingDevelopment_->GetGrowthDeveloper(0x3110);
        bestScore = desirability(bestDev);
        bestDemand = activeDemand(bestDev);
        for (std::uint32_t type : {0x3120u, 0x3130u, 0x3320u, 0x3330u}) {
            IZoneDeveloperGrowthApi* developer = buildingDevelopment_->GetGrowthDeveloper(type);
            if (!developer) continue;
            const int score = desirability(developer);
            const int demand = activeDemand(developer);
            if (score > bestScore || (score == bestScore && bestDemand < demand)) {
                bestDev = developer;
                bestScore = score;
                bestDemand = demand;
            }
        }
    }
    const bool hasDeveloper = bestDev != nullptr;
    int growthStyle = 1;
    if (bestDev && bestDev->GrowthAndDeclineListsAreValid()) {   // vslot 0x50
        IZoneDeveloperGridApi* grid = bestDev->DesirabilityMap();   // vslot 0x68
        for (const ZoneDeveloperGrowthItem& item : bestDev->GrowthList()) {   // vslot 0x58
            if (!grid || grid->TractValue(item.x, item.z) <= bestScore) break;
            growthStyle = item.style;
        }
    }
    // The original releases every configuration it got from the manager (vslot 0x10) after using it;
    // the port's host owns those references instead, because cSC4LotConfiguration::Release is not part
    // of the header-only interface (see IZoneDeveloperHost::LotConfigurationsByFilter).
    std::vector<cSC4LotConfiguration*> configurations;
    if (hasDeveloper && g_host) {
        cZoneDeveloperLotFilter filter(bestDev->Purpose(), bestDev->Wealth(), growthStyle);
        g_host->LotConfigurationsByFilter(configurations, &filter);   // manager vslot 0x48
    }
    // Flag of the original's filter test: for commercial medium/high zones the lot depth is limited to 2.
    const bool commercialDepthLimit = currentZoneType_ == 5 || currentZoneType_ == 6;
    cSC4LotConfiguration* chosen = nullptr;
    int bestDifference = INT_MAX;
    for (cSC4LotConfiguration* configuration : configurations) {
        if (!configuration) continue;
        cSC4LotConfigurationObject* object = configuration->GetPrimaryBuildingConfigurationObject();  // 0xc0
        if (!object) continue;
        std::uint32_t sizeX = 0, sizeZ = 0;
        configuration->GetSize(sizeX, sizeZ);   // 0x38
        if ((commercialDepthLimit && sizeZ > 2) || sizeX >= 4 || (sizeZ & ~1u) != 2) continue;
        if (object->extra.empty()) continue;
        int minCapacity = INT_MAX, maxCapacity = INT_MIN;
        for (std::uint32_t family : object->extra) {
            std::unordered_set<std::uint32_t> types;
            buildingDevelopment_->GetBuildingTypesByFamily(types, family);   // vslot 0x88
            for (std::uint32_t type : types) {
                const BuildingProfile& profile = buildingDevelopment_->GetBuildingProfile(type);   // 0x138
                if (static_cast<int>(profile.purpose) != bestDev->Purpose() ||
                    static_cast<int>(profile.wealth) != bestDev->Wealth()) {
                    continue;
                }
                minCapacity = std::min(minCapacity, static_cast<int>(profile.capacity));
                maxCapacity = std::max(maxCapacity, static_cast<int>(profile.capacity));
            }
        }
        if (minCapacity != INT_MAX && bestDev->DesirabilityMap()) {
            const int tract = bestDev->DesirabilityMap()->TractSize();   // vslot 0x58
            const int average = (minCapacity + maxCapacity) / 2;
            const int square = (1 << tract) << tract;
            const int difference =
                static_cast<int>(static_cast<float>(average) *
                                 (static_cast<float>(square) /
                                  static_cast<float>(sizeX * sizeZ))) - bestDemand;
            const int magnitude = AbsInt(difference);
            if (magnitude < bestDifference) chosen = configuration;
            if (magnitude <= bestDifference) bestDifference = magnitude;
        }
    }
    if (!chosen) {
        parcellizationSizes_[0] = 2;
        parcellizationSizes_[1] = 2;
    } else {
        std::uint32_t sizeX = 0, sizeZ = 0;
        chosen->GetSize(sizeX, sizeZ);
        parcellizationSizes_[0] = static_cast<int>(sizeX);
        parcellizationSizes_[1] = static_cast<int>(sizeZ);
    }
    parcellizationSizes_[2] = parcellizationSizes_[1];
    parcellizationSizes_[3] = parcellizationSizes_[1];
    if (!configurations.empty()) {
        std::uint32_t minZ = static_cast<std::uint32_t>(parcellizationSizes_[2]);
        std::uint32_t maxZ = static_cast<std::uint32_t>(parcellizationSizes_[3]);
        for (cSC4LotConfiguration* configuration : configurations) {
            std::uint32_t sizeX = 0, sizeZ = 0;
            if (configuration) configuration->GetSize(sizeX, sizeZ);
            if (sizeX != 0 && static_cast<std::uint32_t>(parcellizationSizes_[0]) % sizeX == 0) {
                if (sizeZ < minZ) minZ = sizeZ;
                if (sizeZ > maxZ) maxZ = sizeZ;
            }
        }
        parcellizationSizes_[2] = static_cast<int>(minZ);
        parcellizationSizes_[3] = static_cast<int>(maxZ);
    }
    if (static_cast<std::uint32_t>(parcellizationSizes_[3] - parcellizationSizes_[2]) < 2) {
        const int value = parcellizationSizes_[2] > 2 ? parcellizationSizes_[3] - 2 : 1;
        parcellizationSizes_[2] = value;
        parcellizationSizes_[3] = value + 2;
    }
    if (!hasDeveloper || bestDev->Purpose() != 1) {
        if (static_cast<unsigned>(density - 1) < 3) parcellizationSizes_[4] = 8 - density;
    } else {
        int value;
        if (density == 1) value = parcellizationSizes_[1] * 3;
        else if (density == 2) value = static_cast<int>(static_cast<double>(parcellizationSizes_[1]) * 2.5);
        else if (density == 3) value = parcellizationSizes_[1] * 2;
        else return;
        parcellizationSizes_[4] = value;
    }
}

// @ 0x10012dec2. Along every cell of the rectangle that carries the network type, find the single
// connection it has and extend a street from the cell to the rectangle edge that connection points at.
void cSC4ZoneDeveloper::ExtendAsStreets(const SC4Rect<int>& rect, std::uint32_t networkType) {
    const std::uint32_t flag = FlagForNetworkType(networkType);
    for (int x = rect.x0; x <= rect.x1; ++x) {
        for (int z = rect.z0; z <= rect.z1; ++z) {
            if (!ExistsNetworkOfType(x, z, flag)) continue;
            int count = 0, chosen = 0;
            for (int direction = 0; direction < 4; ++direction) {
                const bool connected =
                    ExistsConnectionOfType(x, z, flag, direction, 0) ||      // road
                    ExistsConnectionOfType(x, z, flag, direction, 10) ||     // one-way road
                    ExistsConnectionOfType(x, z, flag, direction, 3) ||      // street
                    ExistsConnectionOfType(x, z, flag, direction, 11) ||     // dirt road
                    (((x + z) & 1) != 0 && ExistsConnectionOfType(x, z, flag, direction, 6));   // avenue
                if (!connected) continue;
                chosen = direction;
                if (++count > 1) break;
            }
            if (count != 1) continue;
            int endX = x, endZ = z;
            switch (chosen) {
                case 0: endZ = rect.z1; break;
                case 1: endX = rect.x0; break;
                case 2: endZ = rect.z0; break;
                case 3: endX = rect.x1; break;
                default: break;
            }
            if (endX != x || endZ != z) DrawStreet(x, z, endX, endZ, false);
        }
    }
}

// @ 0x10012e0e8. The jog streets: for each of the four sides, scan the row/column just outside the
// rectangle; when a cell there has exactly one connection pointing away from the rectangle and it does
// not already connect to another street-like network in the perpendicular direction, draw a street
// along the rectangle's own edge.
void cSC4ZoneDeveloper::LayInJogStreets(const SC4Rect<int>& rect, std::uint32_t networkType) {
    const std::uint32_t flag = FlagForNetworkType(networkType);
    const int cellsX = city_ ? city_->CellCountX() : 0;
    const int cellsZ = city_ ? city_->CellCountZ() : 0;
    // The five networks a jog street must not cross (avenue, road, one-way road, street, dirt road).
    const auto connectsPerpendicular = [&](int x, int z, int d1, int d2) {
        if (ExistsNetworkOfType(x, z, 0x40) &&
            (ExistsConnectionOfType(x, z, 0x40, d1, 6) || ExistsConnectionOfType(x, z, 0x40, d2, 6)))
            return true;
        if (ExistsNetworkOfType(x, z, 1) &&
            (ExistsConnectionOfType(x, z, 1, d1, 0) || ExistsConnectionOfType(x, z, 1, d2, 0)))
            return true;
        if (ExistsNetworkOfType(x, z, 0x400) &&
            (ExistsConnectionOfType(x, z, 0x400, d1, 10) || ExistsConnectionOfType(x, z, 0x400, d2, 10)))
            return true;
        if (ExistsNetworkOfType(x, z, 8) &&
            (ExistsConnectionOfType(x, z, 8, d1, 3) || ExistsConnectionOfType(x, z, 8, d2, 3)))
            return true;
        if (ExistsNetworkOfType(x, z, 0x800) &&
            (ExistsConnectionOfType(x, z, 0x800, d1, 11) || ExistsConnectionOfType(x, z, 0x800, d2, 11)))
            return true;
        return false;
    };
    const auto singleConnection = [&](int x, int z, int expected) {
        int count = 0, chosen = -1;
        for (int direction = 0; direction < 4; ++direction) {
            if (!ExistsConnectionOfType(x, z, flag, direction, static_cast<int>(networkType))) continue;
            chosen = direction;
            if (++count > 1) break;
        }
        return count == 1 && chosen == expected;
    };
    // Top row: the cells at z0-1; a north connection (0) draws along the top edge.
    if (rect.z0 > 0 && rect.x0 <= rect.x1) {
        bool jog = false, blocked = false;
        for (int x = rect.x0; x <= rect.x1 && !blocked; ++x) {
            if (!ExistsNetworkOfType(x, rect.z0 - 1, flag)) continue;
            if (singleConnection(x, rect.z0 - 1, 0)) jog = true;
            if (connectsPerpendicular(x, rect.z0 - 1, 3, 1)) blocked = true;
        }
        if (jog && !blocked) DrawStreet(rect.x0, rect.z0, rect.x1, rect.z0, true);
    }
    // Bottom row: the cells at z1+1; a south connection (2) draws along the bottom edge.
    if (rect.z1 < cellsZ - 1 && rect.x0 <= rect.x1) {
        bool jog = false, blocked = false;
        for (int x = rect.x0; x <= rect.x1 && !blocked; ++x) {
            if (!ExistsNetworkOfType(x, rect.z1 + 1, flag)) continue;
            if (singleConnection(x, rect.z1 + 1, 2)) jog = true;
            if (connectsPerpendicular(x, rect.z1 + 1, 3, 1)) blocked = true;
        }
        if (jog && !blocked) DrawStreet(rect.x0, rect.z1, rect.x1, rect.z1, true);
    }
    // Left column: the cells at x0-1; a west connection (3) draws along the left edge.
    if (rect.x0 > 0 && rect.z0 <= rect.z1) {
        bool jog = false, blocked = false;
        for (int z = rect.z0; z <= rect.z1 && !blocked; ++z) {
            if (!ExistsNetworkOfType(rect.x0 - 1, z, flag)) continue;
            if (singleConnection(rect.x0 - 1, z, 3)) jog = true;
            if (connectsPerpendicular(rect.x0 - 1, z, 0, 2)) blocked = true;
        }
        if (jog && !blocked) DrawStreet(rect.x0, rect.z0, rect.x0, rect.z1, true);
    }
    // Right column: the cells at x1+1; an east connection (1) draws along the right edge.
    if (rect.x1 < cellsX - 1 && rect.z0 <= rect.z1) {
        bool jog = false, blocked = false;
        for (int z = rect.z0; z <= rect.z1 && !blocked; ++z) {
            if (!ExistsNetworkOfType(rect.x1 + 1, z, flag)) continue;
            if (singleConnection(rect.x1 + 1, z, 1)) jog = true;
            if (connectsPerpendicular(rect.x1 + 1, z, 0, 2)) blocked = true;
        }
        if (jog && !blocked) DrawStreet(rect.x1, rect.z0, rect.x1, rect.z1, true);
    }
}

// @ 0x10012c61c. Draws a street between two cells through the street network tool, clipped to the
// current region mask. The original walks the line, finds each contiguous run of region cells and
// places that run plus the three "end" segments the tool needs (the run's ends, the line's end and the
// cell before it); in extend mode it also places every cell of the run individually.
void cSC4ZoneDeveloper::DrawStreet(int x0, int z0, int x1, int z1, bool extend) {
    IZoneDeveloperNetworkApi* tool = streetTool_;   // +0x80
    if (!tool) return;
    const auto place = [&](int a, int b, int c, int d) {
        return tool->PlaceNetworkOccupants(a, b, c, d, 0xc49, false, lotChangesEnabled_ != 0, -1);
    };
    const int dx = x1 - x0;
    if (dx == 0 && z1 == z0) {
        if (RegionHas(currentRegion_, x1, z1)) place(x1, z1, x1, z1);
        return;
    }
    int xStep = dx;
    if (dx <= -2) xStep = -1;
    if (x0 < x1) xStep = 1;
    int zStep = z1 - z0;
    if (zStep <= -2) zStep = -1;
    if (z0 < z1) zStep = 1;
    const int lastX = x1 - xStep, lastZ = z1 - zStep;
    int currentX = x0, currentZ = z0;
    for (;;) {
        int inX, inZ;
        if (RegionHas(currentRegion_, currentX, currentZ)) {
            inX = currentX;
            inZ = currentZ;
        } else if (currentX == x1 && currentZ == z1) {
            inX = x1;
            inZ = z1;
        } else {
            int cx = currentX, cz = currentZ;
            do {
                cx += xStep;
                cz += zStep;
            } while (!RegionHas(currentRegion_, cx, cz) && !(cx == x1 && cz == z1));
            inX = cx;
            inZ = cz;
        }
        const auto placeRun = [&](int a, int b) {
            // (a,b) is both the first and the last cell of the run; the four calls are the run, the
            // segment from the cell after it to the line end, and the two segments to the cell before
            // the end.
            if (place(a, b, a, b)) return;
            if (place(a + xStep, b + zStep, x1, z1)) return;
            if (place(a, b, lastX, lastZ)) return;
            place(a + xStep, b + zStep, lastX, lastZ);
        };
        if (inX == x1 && inZ == z1) {
            if (RegionHas(currentRegion_, x1, z1)) placeRun(x1, z1);
            return;
        }
        int endX = inX, endZ = inZ;
        while (!(endX == x1 && endZ == z1) && RegionHas(currentRegion_, endX + xStep, endZ + zStep)) {
            endX += xStep;
            endZ += zStep;
        }
        if (!(inX == endX && inZ == endZ && extend)) {
            // The run: from (inX,inZ) to (endX,endZ).
            if (place(inX, inZ, endX, endZ)) goto nextRun;
            if (place(inX + xStep, inZ + zStep, x1, z1)) goto nextRun;
            if (place(inX, inZ, lastX, lastZ)) goto nextRun;
            place(inX + xStep, inZ + zStep, lastX, lastZ);
        }
    nextRun:
        if (extend) {
            int px = inX, pz = inZ;
            for (;;) {
                place(px, pz, px, pz);
                if (px == endX - xStep && pz == endZ - zStep) break;
                px += xStep;
                pz += zStep;
            }
            place(endX, endZ, endX, endZ);
        }
        if (endX == x1 && endZ == z1) return;
        currentX = endX + xStep;
        currentZ = endZ + zStep;
    }
}

// @ 0x10012cbcc. Tries the candidate facings in order (the list order; an empty list is filled with a
// random starting facing and the three after it) and creates a 1x1..NxN lot facing the first side that
// has a street-like network next to the footprint. Falls back to the first candidate.
bool cSC4ZoneDeveloper::CreateLotToFaceRoad(int x, int z, int sizeX, int sizeZ,
                                            std::list<int>& facings, cSC4Lot*& out) {
    out = nullptr;
    if (facings.empty()) {
        int facing = static_cast<int>(g_host ? g_host->RandomBelow(4) : 0);
        for (int i = 0; i < 4; ++i) {
            facings.push_back(facing);
            facing = facing > 2 ? 0 : facing + 1;
        }
    }
    int facing = facings.front();
    for (int candidate : facings) {
        bool found = false;
        switch (candidate) {
            case 0:
                if (sizeX > 0 && sizeZ > 0) {
                    for (int i = 0; i < sizeX && !found; ++i)
                        found = ExistsNetworkOfType(x + i, z - 1, 0xc49);
                }
                break;
            case 1:
                if (sizeZ > 0 && x + sizeX < cellsX_) {
                    for (int i = 0; i < sizeZ && !found; ++i)
                        found = ExistsNetworkOfType(x + sizeX, z + i, 0xc49);
                }
                break;
            case 2:
                if (sizeX > 0 && z + sizeZ < cellsZ_) {
                    for (int i = 0; i < sizeX && !found; ++i)
                        found = ExistsNetworkOfType(x + i, z + sizeZ, 0xc49);
                }
                break;
            case 3:
                if (sizeZ > 0 && x > 0) {
                    for (int i = 0; i < sizeZ && !found; ++i)
                        found = ExistsNetworkOfType(x - 1, z + i, 0xc49);
                }
                break;
            default:
                break;
        }
        if (found) {
            facing = candidate;
            break;
        }
    }
    return CreateLotIfPossible(x, z, sizeX, sizeZ, facing, out, false, false, nullptr);
}

// @ 0x10012ce50. Examines the cells just outside each of the four region edges (only where the edge
// cell is in the region mask) and records the boundary condition and cost per side. The side with the
// highest cost becomes the first lot orientation, the perpendicular side with the higher cost the
// alternate; the two swap roles for residential zones 1/2 when the streets are extended and the region
// is at least 2x2, and again when the "swap orientation" option is set.
void cSC4ZoneDeveloper::ExamineBoundaries() {
    const SC4CellRegion<int>* region = currentRegion_;
    if (!region || !city_) return;
    const int cellsX = city_->CellCountX();
    const int cellsZ = city_->CellCountZ();
    for (int i = 0; i < 4; ++i) boundaryCost_[i] = 0;
    const int boundaryValue = extendAsStreets_ ? 0 : 1;   // i386 0x000dd1xx: extendAsStreets ^ 1
    for (int i = 0; i < 4; ++i) boundary_[i] = static_cast<BoundaryCondition>(boundaryValue);
    const int x0 = region->bounds.x0, z0 = region->bounds.z0;
    const int x1 = region->bounds.x1, z1 = region->bounds.z1;
    for (int x = x0; x <= x1; ++x) {
        if (region->Test(x, z0)) {
            ExamineCell(x, z0 + (z0 > 0 ? -1 : 0), boundary_[0], boundaryCost_[0]);
        }
        if (region->Test(x, z1)) {
            ExamineCell(x, z1 + (z1 < cellsZ - 1 ? 1 : 0), boundary_[2], boundaryCost_[2]);
        }
    }
    for (int z = z0; z <= z1; ++z) {
        if (region->Test(x0, z)) {
            ExamineCell(x0 + (x0 > 0 ? -1 : 0), z, boundary_[3], boundaryCost_[3]);
        }
        if (region->Test(x1, z)) {
            ExamineCell(x1 + (x1 < cellsX - 1 ? 1 : 0), z, boundary_[1], boundaryCost_[1]);
        }
    }
    // The strongest side becomes the lot orientation; the perpendicular one is the alternate.
    int maxIndex = -1;
    int maxCost = INT_MIN;
    for (int i = 0; i < 4; ++i) {
        if (maxCost < boundaryCost_[i]) maxIndex = i;
        if (boundaryCost_[i] > maxCost) maxCost = boundaryCost_[i];
    }
    int alternate;
    switch (maxIndex) {
        case 0:
        case 2:
            alternate = (boundaryCost_[1] <= boundaryCost_[3] ? 1 : 0) + 1 +
                        (boundaryCost_[1] <= boundaryCost_[3] ? 1 : 0);
            break;
        case 1:
        case 3:
            alternate = (boundaryCost_[0] <= boundaryCost_[2] ? 1 : 0) * 2;
            break;
        default:
            alternate = -1;
            break;
    }
    int orientation = maxIndex;
    int alt = alternate;
    scratch12c_ = 1;
    lotOrientationAlt_ = maxIndex;
    lotOrientation_ = alternate;
    bool normalized = false;
    const bool twoByTwo = x1 - x0 >= 2 && z1 - z0 >= 2;
    if (!(extendAsStreets_ && static_cast<unsigned>(currentZoneType_ - 1) <= 1 && twoByTwo)) {
        normalized = false;
    } else {
        scratch12c_ = 0;
        lotOrientation_ = maxIndex;
        lotOrientationAlt_ = alternate;
        normalized = true;
    }
    if (swapOrientation_) {
        scratch12c_ = normalized ? 1 : 0;
        lotOrientation_ = alt;
        lotOrientationAlt_ = orientation;
    }
    if (std::getenv("SC4_SIMDEBUG"))
        std::fprintf(stderr, "zone developer: boundaries (%d,%d)-(%d,%d) costs %d %d %d %d -> orientation %d alt %d\n", x0, z0, x1, z1, boundaryCost_[0],
                     boundaryCost_[1], boundaryCost_[2], boundaryCost_[3], lotOrientation_, lotOrientationAlt_);
}

// @ 0x10012c320. Lays the parallel streets of an industrial zone: every `spacing` columns/rows, in the
// order the lot orientation and its alternate select, skipping the two outer rows/columns; then places
// a 1x1 lot on every free cell (CreateLotToFaceRoad with a 1x1 footprint).
void cSC4ZoneDeveloper::SimpleParcellize(int spacing) {
    SC4CellRegion<int>* region = currentRegion_;
    if (!region) return;
    const int x0 = region->bounds.x0, z0 = region->bounds.z0;
    const int x1 = region->bounds.x1, z1 = region->bounds.z1;
    int xStart, zStart, xStep, zStep;
    switch (lotOrientation_) {
        case 0:
            xStart = lotOrientationAlt_ == 3 ? x0 : x1;
            zStart = z0;
            xStep = lotOrientationAlt_ == 3 ? 1 : -1;
            zStep = 1;
            break;
        case 1:
            xStart = x1;
            if (lotOrientationAlt_ == 0) { zStart = z0; zStep = 1; }
            else { zStart = z1; zStep = -1; }
            xStep = -1;
            break;
        case 2:
            xStart = lotOrientationAlt_ == 3 ? x0 : x1;
            zStart = z1;
            xStep = lotOrientationAlt_ == 3 ? 1 : -1;
            zStep = -1;
            break;
        case 3:
            xStart = x0;
            if (lotOrientationAlt_ == 0) { zStart = z0; zStep = 1; }
            else { zStart = z1; zStep = -1; }
            xStep = 1;
            break;
        default:
            return;
    }
    if (extendAsStreets_) {
        int counter = 0;
        for (int x = xStart; x0 <= x && x <= x1; x += xStep) {
            if (counter < spacing || x - x0 < 2 || x1 - x < 2) {
                ++counter;
            } else {
                counter = 0;
                DrawStreet(x, z0, x, z1, false);
            }
        }
        counter = 0;
        for (int z = zStart; z0 <= z && z <= z1; z += zStep) {
            if (counter < spacing || z - z0 < 2 || z1 - z < 2) {
                ++counter;
            } else {
                counter = 0;
                DrawStreet(x0, z, x1, z, false);
            }
        }
    }
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            cSC4Lot* lot = nullptr;
            std::list<int> facings;
            CreateLotToFaceRoad(x, z, 1, 1, facings, lot);
            if (lot) lot->Release();   // vslot 0x10
        }
    }
    delete currentRegion_;
    currentRegion_ = nullptr;
}

// @ 0x10012ee06. The general lot layout. The function is a nested walk over the region: the outer
// coordinate advances by the lot pitch, the inner one by the lot width, and at every position the lot
// depth is adjusted to the free run (halved or divided by three so a strip is filled evenly), the lot
// is created (or the leftover filled with remnants) and a street is drawn between the rows/columns
// when the streets are extended. Lots are created with their vertices locked and unlocked in one batch
// at the end, and the footprint is levelled through the lot developer.
//
// The original keeps the two coordinates, the four orientation setups and the facing in the same stack
// slots and the decompiler prints them through two pointers (`local_80`/`local_78`); the port keeps that
// shape so the arithmetic below reads exactly like the decompiled code.
void cSC4ZoneDeveloper::Parcellize() {
    if (static_cast<std::uint32_t>(lotOrientation_) > 3) return;
    SC4CellRegion<int>* region = currentRegion_;
    if (!region) return;
    const std::uint32_t x0 = static_cast<std::uint32_t>(region->bounds.x0);
    const std::uint32_t z0 = static_cast<std::uint32_t>(region->bounds.z0);
    const std::uint32_t x1 = static_cast<std::uint32_t>(region->bounds.x1);
    const std::uint32_t z1 = static_cast<std::uint32_t>(region->bounds.z1);
    const std::uint32_t sizeX = static_cast<std::uint32_t>(parcellizationSizes_[0]);
    const std::uint32_t sizeZ = static_cast<std::uint32_t>(parcellizationSizes_[1]);

    std::uint32_t uVar4 = 0, uVar5 = 0, uVar7 = 0, uVar8 = 0, uVar10 = 0, uVar11 = 0, uVar15 = 0,
                  uVar17 = 0, uVar19 = 0, uVar20 = 0;
    std::int32_t iVar13 = 0, iVar16 = 0;
    std::uint64_t uVar14 = 0, uVar18 = 0, local_b0 = 0, local_a8 = 0, local_a0 = 0, local_98 = 0,
                  local_60 = 0, local_88 = 0, local_100 = 0;
    std::uint32_t local_bc = 0, local_b8 = 0, local_b4 = 0, local_d4 = 0, local_d0 = 0, local_cc = 0,
                  local_6c = 0, local_68 = 0, local_64 = 0, local_54 = 0, local_50 = 0, local_4c = 0,
                  local_48 = 0, local_44 = 0, local_40 = 0, local_38 = 0, local_8c = 0, local_dc = 0;
    std::int32_t local_f8 = 0, local_f4 = 0, local_d8 = 0, local_c8 = 0, local_c4 = 0, local_70 = 0,
                 local_3c = 0, local_31 = 0;
    std::uint32_t* local_80 = nullptr;
    std::uint32_t* local_78 = nullptr;
    bool bVar12 = false, bVar21 = false;
    std::int32_t cVar3 = 0;
    cSC4Lot* local_f0 = nullptr;
    std::list<cSC4Lot*> deferred;

    uVar20 = static_cast<std::uint32_t>(lotOrientationAlt_);
    switch (static_cast<std::uint32_t>(lotOrientation_)) {
        case 0:
            local_bc = sizeX;
            uVar8 = sizeZ;
            uVar17 = x0;
            local_44 = x1;
            local_a8 = local_44;
            if (uVar20 == 3) {
                iVar13 = 3;
                local_3c = 1;
                local_98 = uVar8;
                uVar5 = uVar8;
                local_48 = uVar17;
                local_38 = uVar17;
            } else {
                uVar5 = 0u - uVar8;
                local_44 = (local_44 - uVar8) + 1;
                if (static_cast<std::int32_t>(local_44) < 0) local_44 = 0;
                iVar13 = 1;
                local_48 = (1 - uVar8) + uVar17;
                local_3c = 3;
                local_98 = 0xffffffffu;
                local_38 = local_44;
            }
            uVar19 = z0;
            uVar11 = z1;
            local_80 = &local_50;
            local_78 = &local_4c;
            local_b0 = uVar19;
            local_a0 = local_b0;
            local_54 = uVar11;
            uVar15 = local_bc;
            uVar4 = local_bc;
            goto common;
        case 1:
            uVar8 = sizeX;
            local_bc = sizeZ;
            uVar17 = z0;
            uVar11 = z1;
            if (uVar20 == 0) {
                iVar13 = 0;
                local_3c = 2;
                local_98 = local_bc;
                uVar5 = local_bc;
                local_48 = uVar17;
                local_44 = uVar11;
                local_38 = uVar17;
            } else {
                local_44 = (uVar11 - local_bc) + 1;
                local_3c = 0;
                if (static_cast<std::int32_t>(local_44) < 0) local_44 = 0;
                iVar13 = 2;
                local_98 = 0xffffffffu;
                uVar5 = 0u - local_bc;
                local_48 = (1 - local_bc) + uVar17;
                local_38 = local_44;
            }
            uVar15 = x0;
            local_54 = x1;
            uVar19 = (1 - uVar8) + uVar15;
            iVar16 = static_cast<std::int32_t>(local_54 - uVar8);
            local_80 = &local_4c;
            local_78 = &local_50;
            uVar4 = uVar8;
            break;
        case 2:
            local_bc = sizeX;
            uVar8 = sizeZ;
            uVar17 = x0;
            uVar11 = x1;
            iVar13 = 3;
            local_98 = uVar8;
            local_3c = 1;
            uVar5 = uVar8;
            local_48 = uVar17;
            local_44 = uVar11;
            local_38 = uVar17;
            if (uVar20 != 3) {
                uVar5 = 0u - uVar8;
                local_44 = (uVar11 - uVar8) + 1;
                if (static_cast<std::int32_t>(local_44) < 0) local_44 = 0;
                iVar13 = 1;
                local_48 = (1 - uVar8) + uVar17;
                local_3c = 3;
                local_98 = 0xffffffffu;
                local_38 = local_44;
            }
            uVar15 = z0;
            local_54 = z1;
            uVar19 = (1 - local_bc) + uVar15;
            iVar16 = static_cast<std::int32_t>(local_54 - local_bc);
            local_80 = &local_50;
            local_78 = &local_4c;
            uVar4 = local_bc;
            break;
        case 3:
            uVar8 = sizeX;
            local_bc = sizeZ;
            uVar17 = z0;
            local_44 = z1;
            local_a8 = local_44;
            if (uVar20 == 0) {
                iVar13 = 0;
                local_3c = 2;
                local_98 = local_bc;
                uVar5 = local_bc;
                local_48 = uVar17;
                local_38 = uVar17;
            } else {
                local_44 = (local_44 - local_bc) + 1;
                local_3c = 0;
                if (static_cast<std::int32_t>(local_44) < 0) local_44 = 0;
                iVar13 = 2;
                local_98 = 0xffffffffu;
                uVar5 = 0u - local_bc;
                local_48 = (1 - local_bc) + uVar17;
                local_38 = local_44;
            }
            uVar19 = x0;
            uVar11 = x1;
            local_80 = &local_4c;
            local_78 = &local_50;
            local_b0 = uVar19;
            local_a0 = local_b0;
            local_54 = uVar11;
            uVar15 = uVar8;
            uVar4 = uVar8;
            goto common;
        default:
            return;
    }
    // The two cases that fall through here are the ones whose outer coordinate is the first bounds
    // value; the original completes the record before the shared body.
    local_a0 = uVar15;
    local_a8 = uVar11;
    uVar11 = static_cast<std::uint32_t>(iVar16) + 1;
    local_b0 = uVar11;
    uVar15 = 0u - uVar4;
    uVar4 = 0xffffffffu;
common:
    local_d8 = iVar13;
    if (static_cast<std::int32_t>(boundary_[uVar20]) != 1) {
        local_d8 = local_3c;
        if (extendAsStreets_ == 0) local_d8 = iVar13;
    }
    local_6c = extendAsStreets_;
    local_f4 = iVar13;
    if (local_6c == 0) local_f4 = local_3c;
    if (static_cast<std::int32_t>(boundary_[(uVar20 + 2) & 3]) == 1) local_f4 = local_3c;
    uVar7 = static_cast<std::uint32_t>(local_b0);
    uVar20 = uVar7;
    if (static_cast<std::int32_t>(uVar7) <= static_cast<std::int32_t>(uVar11) &&
        static_cast<std::int32_t>(uVar19) <= static_cast<std::int32_t>(uVar7)) {
        bVar12 = false;
        uVar14 = local_b0;
        uVar10 = uVar7;
        do {
            uVar10 = uVar15 + uVar10;
            uVar20 = 0;
            if (bVar12) uVar20 = uVar15;
            uVar20 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(uVar20) +
                                                 static_cast<std::uint64_t>(static_cast<std::uint32_t>(uVar14)));
            uVar14 = static_cast<std::uint64_t>(uVar20);
        } while (static_cast<std::int32_t>(uVar19) <= static_cast<std::int32_t>(uVar10) &&
                 (bVar12 = !bVar12, static_cast<std::int32_t>(uVar10) <= static_cast<std::int32_t>(uVar11)));
    }
    uVar10 = uVar15;
    if (uVar20 == uVar7) uVar10 = 0;
    local_100 = 0;
    *local_78 = uVar7;
    local_70 = iVar13;
    local_40 = uVar5;
    if (static_cast<std::int32_t>(uVar7) <= static_cast<std::int32_t>(uVar11) &&
        static_cast<std::int32_t>(uVar19) <= static_cast<std::int32_t>(uVar7)) {
        local_d4 = (local_54 - static_cast<std::uint32_t>(local_a0)) + 1;
        local_8c = static_cast<std::uint32_t>(local_a8) - uVar17 + 1;
        local_31 = (static_cast<std::int32_t>(local_44) < static_cast<std::int32_t>(local_38) ||
                    static_cast<std::int32_t>(local_38) < static_cast<std::int32_t>(local_48)) ? 1 : 0;
        local_f8 = static_cast<std::int32_t>(local_a8) + 1;
        uVar18 = uVar8;
        uVar14 = local_bc;
        local_68 = 0;
        bVar12 = false;
        uVar7 = local_38;
        local_dc = uVar8;
        local_b8 = uVar19;
        local_b4 = uVar11;
        local_64 = uVar17;
        cVar3 = local_31;
        do {
            *local_80 = uVar7;
            if (cVar3 == 0) {
                local_60 = (local_60 & 0xffffffff00000000ull) | static_cast<std::uint32_t>(local_d8);
                do {
                    local_f0 = nullptr;
                    uVar8 = static_cast<std::uint32_t>(parcellizationSizes_[1]);
                    uVar17 = (uVar7 - uVar17) + uVar8;
                    if (static_cast<std::int32_t>(local_40) > 0) {
                        uVar17 = static_cast<std::uint32_t>(local_f8) - uVar7;
                    }
                    if (static_cast<std::int32_t>(uVar17) < 1) {
                        local_88 &= 0xffffffff00000000ull;
                    } else {
                        uVar19 = uVar17;
                        // LAB_10012f40f, shared by both branches of the original.
                        const auto halfDepth = [&]() {
                            if (uVar11 <= uVar17 && (uVar19 = uVar8, uVar17 < (uVar8 * 2 | local_6c))) {
                                uVar19 = uVar17 > static_cast<std::uint32_t>(parcellizationSizes_[3]) ? uVar17 >> 1
                                                                                                      : uVar17;
                            }
                        };
                        if (local_f4 == local_3c) {
                            local_88 &= 0xffffffff00000000ull;
                            if (static_cast<std::int32_t>(local_60) == iVar13) {
                                if (static_cast<std::uint32_t>(parcellizationSizes_[3]) < uVar17 &&
                                    static_cast<std::uint32_t>(parcellizationSizes_[2] * 2) <= uVar17) {
                                    if (uVar17 < static_cast<std::uint32_t>(parcellizationSizes_[3]) * 2 - 1 ||
                                        uVar17 < static_cast<std::uint32_t>(parcellizationSizes_[2] * 3) + local_6c) {
                                        uVar19 = static_cast<std::uint32_t>(
                                            (static_cast<std::int32_t>(uVar17 + 1) -
                                             (static_cast<std::int32_t>(uVar17 + 1) >> 0x1f)) >> 1);
                                    } else {
                                        uVar19 = uVar8;
                                        if (uVar17 < uVar8 * 3 + local_6c) {
                                            uVar19 = static_cast<std::uint32_t>(static_cast<std::int32_t>(uVar17 + 1) / 3);
                                        }
                                    }
                                }
                                goto depthDone;
                            }
                            uVar11 = static_cast<std::uint32_t>(parcellizationSizes_[2] * 2) | local_6c;
                            halfDepth();
                        } else {
                            local_88 &= 0xffffffff00000000ull;
                            uVar11 = static_cast<std::uint32_t>(parcellizationSizes_[2] * 2) | local_6c;
                            if (static_cast<std::int32_t>(local_60) == iVar13) {
                                halfDepth();
                            } else if (uVar17 < uVar11) {
                                uVar19 = uVar17 - 1;
                                if (static_cast<std::int32_t>(uVar17) < 2) uVar19 = uVar8;
                            } else {
                                uVar19 = uVar8;
                                if (uVar17 < (uVar8 * 2 | local_6c)) {
                                    if (uVar17 - 1 <= static_cast<std::uint32_t>(parcellizationSizes_[3])) {
                                        uVar19 = uVar17 - 1;
                                        if (static_cast<std::int32_t>(uVar17) < 2) uVar19 = uVar8;
                                    } else {
                                        uVar19 = uVar17 >> 1;
                                    }
                                }
                            }
                        }
                    depthDone:
                        if (local_8c <= static_cast<std::uint32_t>(parcellizationSizes_[3]) &&
                            local_d4 < (static_cast<std::uint32_t>(parcellizationSizes_[0] * 2) | local_6c)) {
                            uVar19 = local_8c;
                        }
                        if (uVar19 != uVar8) {
                            switch (static_cast<std::uint32_t>(lotOrientation_)) {
                                case 0:
                                case 2:
                                    iVar13 = static_cast<std::int32_t>(uVar19 - static_cast<std::uint32_t>(uVar18));
                                    uVar18 = uVar19;
                                    break;
                                case 1:
                                case 3:
                                    iVar13 = static_cast<std::int32_t>(uVar19 - static_cast<std::uint32_t>(uVar14));
                                    uVar14 = uVar19;
                                    break;
                                default:
                                    iVar13 = 0;
                                    break;
                            }
                            local_88 = (local_88 & 0xffffffff00000000ull) | static_cast<std::uint32_t>(iVar13);
                            if (static_cast<std::int32_t>(local_40) < 0) {
                                *local_80 = uVar7 - static_cast<std::uint32_t>(iVar13);
                            }
                        }
                    }
                    uVar17 = local_4c;   // the depth the lot will get
                    uVar8 = local_50;
                    local_c8 = static_cast<std::int32_t>(uVar18) - 1 + static_cast<std::int32_t>(local_50);
                    local_c4 = static_cast<std::int32_t>(uVar14) - 1 + static_cast<std::int32_t>(local_4c);
                    local_d0 = local_50;
                    local_cc = local_4c;
                    cVar3 = CreateLotIfPossible(static_cast<int>(local_50), static_cast<int>(local_4c),
                                               static_cast<int>(uVar18), static_cast<int>(uVar14),
                                               static_cast<int>(local_60 & 0xffffffffu), local_f0,
                                               false, false, nullptr) ? 1 : 0;
                    if (cVar3 == 0) {
                        CreateLotRemnants(static_cast<int>(uVar8), static_cast<int>(uVar17),
                                          static_cast<int>(uVar18), static_cast<int>(uVar14),
                                          static_cast<int>(local_60 & 0xffffffffu));
                    }
                    if (static_cast<std::int32_t>(local_40) > 0 && static_cast<std::int32_t>(local_88) != 0) {
                        *local_80 += static_cast<std::uint32_t>(static_cast<std::int32_t>(local_88));
                    }
                    if (static_cast<std::int32_t>(local_60) == local_70) {
                        local_60 = (local_60 & 0xffffffff00000000ull) | static_cast<std::uint32_t>(local_3c);
                    } else {
                        if (extendAsStreets_ != 0) {
                            uVar8 = *local_80;
                            uVar17 = *local_78;
                            uVar5 = uVar8 + static_cast<std::uint32_t>(static_cast<std::int32_t>(local_98));
                            *local_80 = uVar5;
                            if (static_cast<std::int32_t>(local_64) <= static_cast<std::int32_t>(uVar5) &&
                                static_cast<std::int32_t>(uVar5) <= static_cast<std::int32_t>(local_a8)) {
                                if (uVar17 == static_cast<std::uint32_t>(local_b0)) {
                                    *local_78 = static_cast<std::uint32_t>(local_a0);
                                    uVar19 = local_4c;
                                    *local_78 = local_54;
                                    bVar12 = true;
                                    DrawStreet(static_cast<int>(local_50), static_cast<int>(uVar19),
                                               static_cast<int>(local_50), static_cast<int>(local_4c),
                                               uVar5 == static_cast<std::uint32_t>(local_a8) ||
                                                   uVar5 == static_cast<std::uint32_t>(local_64));
                                }
                                if (static_cast<std::int32_t>(local_40) < 1) uVar8 -= 1;
                                else uVar8 += 1;
                            }
                            *local_80 = uVar8;
                            *local_78 = uVar17;
                        }
                        local_60 = (local_60 & 0xffffffff00000000ull) | static_cast<std::uint32_t>(local_70);
                    }
                    iVar13 = local_70;
                    iVar16 = local_c8;
                    if (lotChangesEnabled_ != 0) {
                        if (static_cast<std::int32_t>(local_d0) < 0) local_d0 = 0;
                        if (static_cast<std::int32_t>(local_cc) < 0) local_cc = 0;
                        if (city_->CellCountX() <= iVar16) local_c8 = city_->CellCountX() - 1;
                        iVar16 = local_c4;
                        if (city_->CellCountZ() <= iVar16) local_c4 = city_->CellCountZ() - 1;
                        const SC4Rect<int> area{static_cast<int>(local_d0), static_cast<int>(local_cc),
                                                local_c8, local_c4};
                        lotDeveloper_->AttemptAreaLevelling(area, FLT_MAX, true, true, 0.0f, nullptr);
                    }
                    uVar5 = local_40;
                    if (local_f0 != nullptr) {
                        local_f0->SetVerticesAreLocked(true);   // cISC4Lot vslot 0x228
                        deferred.push_front(local_f0);
                        ++local_100;
                        local_f0 = nullptr;
                        iVar13 = local_70;
                    }
                    uVar7 = *local_80 + uVar5;
                    *local_80 = uVar7;
                    uVar17 = local_64;
                    if (static_cast<std::int32_t>(uVar7) < static_cast<std::int32_t>(local_48)) break;
                    uVar18 = local_dc;
                    uVar14 = local_bc;
                } while (static_cast<std::int32_t>(uVar7) <= static_cast<std::int32_t>(local_44));
                uVar18 = local_dc;
                uVar14 = local_bc;
                uVar19 = local_b8;
                uVar11 = local_b4;
                uVar7 = local_38;
                cVar3 = local_31;
            }
            if (extendAsStreets_ != 0) {
                iVar16 = parcellizationSizes_[0];
                bVar21 = false;
                if (bVar12 && scratch12c_ != 0) {
                    if (iVar16 + 1 + parcellizationSizes_[4] < static_cast<std::int32_t>(local_d4)) {
                        bVar21 = false;
                    } else {
                        bVar21 = *local_78 == uVar20 - uVar10;
                    }
                }
                local_68 = local_68 + static_cast<std::uint32_t>(iVar16);
                local_60 = uVar18;
                if (bVar21 || (uVar7 = local_38, uVar5 = local_40,
                               static_cast<std::uint32_t>(parcellizationSizes_[4]) <= local_68)) {
                    uVar8 = *local_78;
                    uVar17 = uVar8 + uVar4;
                    *local_78 = uVar17;
                    local_88 = uVar14;
                    if (bVar21 || (static_cast<std::uint32_t>(iVar16) + static_cast<std::uint32_t>(local_a0) <= uVar17 &&
                                   uVar17 <= local_54 - static_cast<std::uint32_t>(iVar16))) {
                        *local_80 = local_64;
                        uVar19 = local_4c;
                        uVar5 = local_50;
                        *local_80 = static_cast<std::uint32_t>(local_a8);
                        DrawStreet(static_cast<int>(uVar5), static_cast<int>(uVar19),
                                   static_cast<int>(local_50), static_cast<int>(local_4c),
                                   uVar17 == local_54 || uVar17 == static_cast<std::uint32_t>(local_a0));
                        if (static_cast<std::int32_t>(uVar15) < 1) uVar8 -= 1;
                        else uVar8 += 1;
                        local_68 = 0;
                        uVar19 = local_b8;
                        uVar11 = local_b4;
                    }
                    cVar3 = local_31;
                    uVar7 = local_38;
                    uVar5 = local_40;
                    *local_78 = uVar8;
                    uVar14 = local_88;
                    uVar18 = local_60;
                    uVar17 = local_64;
                    iVar13 = local_70;
                }
            }
            uVar8 = *local_78 + uVar15;
            *local_78 = uVar8;
        } while (static_cast<std::int32_t>(uVar19) <= static_cast<std::int32_t>(uVar8) &&
                 static_cast<std::int32_t>(uVar8) <= static_cast<std::int32_t>(uVar11));
        if (local_100 != 0) {
            for (cSC4Lot* lot : deferred) {
                lot->SetVerticesAreLocked(false);   // cISC4Lot vslot 0x228
                lot->Release();
            }
            deferred.clear();
        }
    }
}

// @ 0x10012b160. The zoning entry point. With the "examine boundaries" option it only creates the one
// lot that faces the strongest boundary; otherwise it builds the parcellizable region, extends the
// streets, subdivides the region into lots, runs the lot layout for each piece and finally updates the
// zone grid. The output region (when requested) is the part of the input region the pass did not
// consume.
void cSC4ZoneDeveloper::OnZone(const SC4CellRegion<int>* region, SC4CellRegion<int>** out) {
    if (!region) return;
    if (examineBoundaries_ != 0) {
        currentRegion_ = const_cast<SC4CellRegion<int>*>(region);
        ExamineBoundaries();
        currentRegion_ = nullptr;
        const int x0 = region->bounds.x0, z0 = region->bounds.z0;
        const int width = 1 - x0 + region->bounds.x1;
        const int depth = 1 - z0 + region->bounds.z1;
        std::list<int> facings;
        int alt = lotOrientationAlt_, orientation = lotOrientation_;
        facings.push_back(alt);
        facings.push_back(orientation);
        facings.push_back((alt + 2) & 3);
        facings.push_back((orientation + 2) & 3);
        cSC4Lot* lot = nullptr;
        const bool created = CreateLotToFaceRoad(x0, z0, width, depth, facings, lot);
        if (out && !created) {
            *out = new SC4CellRegion<int>(SC4CellRegion<int>::Make(region->bounds, false));
        }
        return;
    }
    DeferOccupantOperations(true);
    std::list<SC4CellRegion<int>*> regions;
    currentRegion_ = CreateParcellizableRegion(region);
    CompressRegion(currentRegion_);
    if (!currentRegion_) {
        DeferOccupantOperations(false);
        return;
    }
    const SC4Rect<int> rect = currentRegion_->bounds;
    DetermineLotSize(rect);
    if (extendAsStreets_ != 0) {
        ExtendAsStreets(rect, 0xb);
        ExtendAsStreets(rect, 3);
        ExtendAsStreets(rect, 10);
        ExtendAsStreets(rect, 0);
        ExtendAsStreets(rect, 6);
        if (rect.x1 - rect.x0 > 1 && rect.z1 - rect.z0 > 1) {
            LayInJogStreets(rect, 0xb);
            LayInJogStreets(rect, 3);
            LayInJogStreets(rect, 10);
            LayInJogStreets(rect, 0);
            LayInJogStreets(rect, 6);
        }
    }
    SubdivideTheZone(currentRegion_, regions);
    delete currentRegion_;
    currentRegion_ = nullptr;
    if (out) {
        *out = new SC4CellRegion<int>(SC4CellRegion<int>::Make(region->bounds, false));
        for (int x = region->bounds.x0; x <= region->bounds.x1; ++x)
            for (int z = region->bounds.z0; z <= region->bounds.z1; ++z)
                if (region->Test(x, z)) (*out)->SetBit(x, z);
    }
    for (SC4CellRegion<int>* piece : regions) {
        currentRegion_ = piece;
        const SC4Point<int>* origin = highlightOrigin_;
        if (origin) {
            const int ox = origin->x, oz = origin->z;
            bool near = piece->Test(ox, oz);
            if (!near) {
                near = ox > 0 && piece->Test(ox - 1, oz);
                if (!near && ox < city_->CellCountX() - 1) near = piece->Test(ox + 1, oz);
                if (!near && oz > 0) near = piece->Test(ox, oz - 1);
                if (!near && oz < city_->CellCountZ()) near = piece->Test(ox, oz + 1);
            }
            if (!near) {
                if (out) {
                    for (int x = piece->bounds.x0; x <= piece->bounds.x1; ++x)
                        for (int z = piece->bounds.z0; z <= piece->bounds.z1; ++z)
                            if (piece->Test(x, z)) (*out)->ClearBit(x, z);
                }
                delete piece;
                currentRegion_ = nullptr;
                continue;
            }
        }
        ExamineBoundaries();
        if (static_cast<unsigned>(currentZoneType_ - 1) < 6) {
            Parcellize();
        } else {
            // zone type 7 (agriculture) spaces its streets 16 cells apart, 8/9 use 6. SimpleParcellize
            // consumes the region itself (it clears currentRegion_), so the delete below is guarded.
            SimpleParcellize(currentZoneType_ == 7 ? 16 : 6);
        }
        if (currentRegion_) {
            delete currentRegion_;
            currentRegion_ = nullptr;
        }
    }
    if (out && highlightOrigin_) {
        const int ox = highlightOrigin_->x, oz = highlightOrigin_->z;
        if ((*out)->Test(ox, oz)) {
            SC4CellRegion<int>* connected =
                ExtractConnectedCells(*out, ox - (*out)->bounds.x0, oz - (*out)->bounds.z0);
            CompressRegion(connected);
            delete *out;
            *out = connected;
        }
    }
    if (lotChangesEnabled_) UpdateZoneType(region);
    DeferOccupantOperations(false);
}

// @ 0x10012b976. Clears the zone: deletes every lot of the region (remnants included) and, when
// requested, marks the cells that only carry a rail network so the demolition service can remove the
// orphaned rail pieces.
void cSC4ZoneDeveloper::OnDezone(const SC4CellRegion<int>* region, bool removeOrphanRail) {
    if (!region) return;
    DeferOccupantOperations(true);
    SC4CellRegion<int> railOnly = SC4CellRegion<int>::Make(region->bounds, false);
    bool anyRail = false;
    for (int x = region->bounds.x0; x <= region->bounds.x1; ++x) {
        for (int z = region->bounds.z0; z <= region->bounds.z1; ++z) {
            if (!region->Test(x, z)) continue;
            if (x < 0 || z < 0 || x >= cellsX_ || z >= cellsZ_) continue;   // the original indexes unchecked
            if (cSC4Lot* lot = lotMap_[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)]) {
                std::int32_t rect[4] = {0, 0, 0, 0};
                lot->GetBoundingRect(rect);                  // vslot 0x70
                const int facing = lot->GetFacing();         // vslot 0xb8
                const int zoneType = static_cast<int>(lot->GetZoneType());   // vslot 0xf8
                currentZoneType_ = zoneType;
                if ((zoneManager_ && zoneManager_->IsZoneTypeValid(zoneType)) ||
                    static_cast<unsigned>(zoneType - 11) < 2) {
                    DeleteLot(lot);                          // primary vslot 0x118
                    CreateLotRemnants(rect[0], rect[1], 1 - rect[0] + rect[2], 1 - rect[1] + rect[3],
                                      facing);
                }
                currentZoneType_ = 0;
            }
            if (removeOrphanRail && ExistsNetworkOfType(x, z, kNetworkFlagRailOnly) &&
                !ExistsNetworkOfType(x, z, 0x1747)) {
                railOnly.SetBit(x, z);
                anyRail = true;
            }
        }
    }
    if (anyRail && g_host) g_host->DemolishRegion(railOnly, 1, 1);   // SL::spDemolition->vslot 0x30
    DeferOccupantOperations(false);
}

// @ 0x100129956. Fills a cleared footprint with remnant lots. Industrial zones (7..9) get a 1x1 lot on
// every cell. Otherwise the footprint is scanned from the side the removed lot faced, and every maximal
// run of cells that can take a lot becomes a 1-wide remnant (1 x n when the lot faced north/south,
// n x 1 when it faced east/west), facing the same way.
void cSC4ZoneDeveloper::CreateLotRemnants(int x, int z, int width, int depth, int facing) {
    if (static_cast<unsigned>(currentZoneType_ - 7) < 3) {
        for (int row = 0; row < depth; ++row) {
            for (int column = 0; column < width; ++column) {
                cSC4Lot* lot = nullptr;
                std::list<int> facings;
                CreateLotToFaceRoad(x + column, z + row, 1, 1, facings, lot);
                if (lot) lot->Release();
            }
        }
        return;
    }
    std::list<int> facings;
    facings.push_back(facing);
    const auto place = [&](int lotX, int lotZ, int sizeX, int sizeZ) {
        cSC4Lot* lot = nullptr;
        CreateLotToFaceRoad(lotX, lotZ, sizeX, sizeZ, facings, lot);
        if (lot) lot->Release();
    };
    switch (facing) {
        case 0:   // facing north: scan down every column, runs become 1 x n remnants
            for (int column = 0; column < width; ++column) {
                int runStart = z, run = 0;
                for (int row = 0; row < depth; ++row) {
                    if (CanCreateLotOnCell(x + column, z + row, false, false) == 8) {
                        ++run;
                    } else if (run < 1) {
                        ++runStart;
                    } else {
                        place(x + column, runStart, 1, run);
                        runStart = z + 1 + row;
                        run = 0;
                    }
                }
                if (run > 0) place(x + column, runStart, 1, run);
            }
            break;
        case 1:   // facing east: scan every row right-to-left, runs become n x 1 remnants
            for (int row = 0; row < depth; ++row) {
                int runEnd = x - 1 + width, run = 0;   // the rightmost cell of the row
                for (int column = 0; column < width; ++column) {
                    const int cx = x - 1 + width - column;
                    if (CanCreateLotOnCell(cx, z + row, false, false) == 8) {
                        ++run;
                    } else if (run < 1) {
                        --runEnd;
                    } else {
                        place((runEnd + 1) - run, z + row, run, 1);
                        runEnd = cx - 1;
                        run = 0;
                    }
                }
                if (run > 0) place((runEnd + 1) - run, z + row, run, 1);
            }
            break;
        case 2:   // facing south: scan up every column
            for (int column = 0; column < width; ++column) {
                int runEnd = z - 1 + depth, run = 0;   // the bottommost cell of the column
                for (int row = 0; row < depth; ++row) {
                    const int cz = z - 1 + depth - row;
                    if (CanCreateLotOnCell(x + column, cz, false, false) == 8) {
                        ++run;
                    } else if (run < 1) {
                        --runEnd;
                    } else {
                        place(x + column, (runEnd + 1) - run, 1, run);
                        runEnd = cz - 1;
                        run = 0;
                    }
                }
                if (run > 0) place(x + column, (runEnd + 1) - run, 1, run);
            }
            break;
        case 3:   // facing west: scan every row left-to-right
            for (int row = 0; row < depth; ++row) {
                int runStart = x, run = 0;
                for (int column = 0; column < width; ++column) {
                    if (CanCreateLotOnCell(x + column, z + row, false, false) == 8) {
                        ++run;
                    } else if (run < 1) {
                        ++runStart;
                    } else {
                        place(runStart, z + row, run, 1);
                        runStart = x + 1 + column;
                        run = 0;
                    }
                }
                if (run > 0) place(runStart, z + row, run, 1);
            }
            break;
        default:
            break;
    }
}

// @ 0x10012a040. A road or street piece was removed: look at the four cells around it and, where a
// straightened road/street piece (exactly one neighbour connection) sits next to it, place a street
// segment between the two.
void cSC4ZoneDeveloper::ConnectRoadToAnyAdjacentStreets(cISC4Occupant* occupant) {
    if (!occupant) return;
    void* raw = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4NetworkOccupant, &raw) || !raw) return;
    auto* network = static_cast<IZoneDeveloperNetworkOccupant*>(raw);
    IZoneDeveloperNetworkApi* tool = nullptr;
    if (network->IsOfType(0) || network->IsOfType(10)) tool = roadTool_;          // +0x88
    else if (network->IsOfType(3)) tool = streetTool_;                            // +0x80
    else if (network->IsOfType(0xb)) tool = dirtRoadTool_;                        // +0x78
    if (!tool) {
        network->Release();
        return;
    }
    if (network->IsImmovable()) {   // vslot 0x88
        network->Release();
        return;
    }
    SC4CellRect rect{};
    occupant->GetBoundingCityCells(rect);   // occupant vslot 0x58
    const int x = rect.x0, z = rect.z0;
    // Each neighbour: the network piece next to the removed one, its straight-piece test and the
    // direction the new street has to run in.
    struct Neighbour { int x, z, expected; };
    const Neighbour neighbours[4] = {{x - 1, z, 0}, {x, z - 1, 1}, {x + 1, z, 2}, {x, z + 1, 3}};
    for (const Neighbour& neighbour : neighbours) {
        IZoneDeveloperNetworkOccupant* other = GetNetwork(neighbour.x, neighbour.z, 0xc09);
        if (!other) continue;
        const bool connectable = IsConnectable([&](int type) { return other->IsOfType(type); });
        if (!connectable) continue;
        int roads = 0, dirtRoads = 0, roadDirection = -1, dirtDirection = -1;
        for (int direction = 0; direction < 4; ++direction) {
            if (other->ConnectsToNeighbor(direction, 3)) {   // 0x178
                ++roads;
                roadDirection = direction;
            } else if (other->ConnectsToNeighbor(direction, 0xb)) {
                ++dirtRoads;
                dirtDirection = direction;
            }
        }
        if (roads + dirtRoads != 1) continue;
        if (roadDirection != neighbour.expected && dirtDirection != neighbour.expected) continue;
        tool->PlaceNetworkOccupants(x, z, neighbour.x, neighbour.z, 0, true, true, -1);
    }
    network->Release();
}

// @ 0x10012a534. A rail piece was removed: for each of the four neighbours, when a straight rail piece
// sits on it (a two-connection rail with matching edges), look two cells further for a street or a
// dirt road and connect across the gap.
void cSC4ZoneDeveloper::ConnectStreetsAcrossRail(cISC4Occupant* occupant) {
    if (!occupant || !dirtRoadTool_ || !streetTool_) return;
    void* raw = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4NetworkOccupant, &raw) || !raw) return;
    auto* network = static_cast<IZoneDeveloperNetworkOccupant*>(raw);
    if (network->IsImmovable()) {
        network->Release();
        return;
    }
    SC4CellRect rect{};
    occupant->GetBoundingCityCells(rect);   // occupant vslot 0x58
    const int x = rect.x0, z = rect.z0;
    // Each pass: the neighbouring rail cell, the cell two further out and the four rail-edge
    // connection expectations of the straight piece (a piece that connects exactly the two edges
    // across the removed rail, and not the other two).
    struct Pass {
        int nx, nz, farX, farZ;
        bool edge[4];
    };
    const Pass passes[4] = {
        {x - 1, z, x - 2, z, {false, true, false, true}},    // west: rail runs east-west
        {x, z - 1, x, z - 2, {true, false, true, false}},    // north: rail runs north-south
        {x + 1, z, x + 2, z, {false, true, false, true}},    // east
        {x, z + 1, x, z + 2, {true, false, true, false}},    // south
    };
    for (const Pass& pass : passes) {
        IZoneDeveloperNetworkOccupant* rail = GetNetwork(pass.nx, pass.nz, 2);
        if (!rail) continue;
        if ((rail->GetNetworkFlag() & 0x1f4f) != 2) continue;   // a rail-only piece
        bool straight = true;
        for (int edge = 0; edge < 4; ++edge) {
            if (rail->ConnectsToNeighbor(edge, 1) != pass.edge[edge]) {
                straight = false;
                break;
            }
        }
        if (!straight) continue;
        IZoneDeveloperNetworkOccupant* street = GetNetwork(pass.farX, pass.farZ, 8);
        if (street && street->IsOfType(3)) {
            streetTool_->PlaceNetworkOccupants(x, z, pass.farX, pass.farZ, 0, true, true, -1);
            continue;
        }
        IZoneDeveloperNetworkOccupant* dirt = GetNetwork(pass.farX, pass.farZ, 0x800);
        if (dirt && dirt->IsOfType(0xb)) {
            dirtRoadTool_->PlaceNetworkOccupants(x, z, pass.farX, pass.farZ, 0, true, true, -1);
        }
    }
    network->Release();
}

// @ 0x10012abe4. Cheat command: place one lot of every lot configuration, walking the city row by row.
void cSC4ZoneDeveloper::PlopAllLotTypes() {
    if (!g_host || !buildingDevelopment_) return;
    IZoneDeveloperLotDeveloperApi* lotDeveloper = buildingDevelopment_->GetLotDeveloper();   // vslot 0x120
    if (!lotDeveloper) return;
    std::vector<cSC4LotConfiguration*> configurations;
    g_host->AllLotConfigurations(configurations);   // manager vslot 0xa0
    int maxX = cellsX_ - 1, maxZ = cellsZ_ - 1;
    if (maxX < 0) maxX = 0;
    if (maxZ < 0) maxZ = 0;
    int x = 0, z = 0;
    std::uint32_t placed = 0;
    const auto advance = [&]() -> bool {
        if (++x > maxX) {
            x = 0;
            if (++z > maxZ) return false;
        }
        return true;
    };
    for (cSC4LotConfiguration* configuration : configurations) {
        if (!configuration) continue;
        std::uint32_t sizeX = 0, sizeZ = 0;
        configuration->GetSize(sizeX, sizeZ);
        cSC4Lot* lot = nullptr;
        while (!CreateLotIfPossible(x, z, static_cast<int>(sizeX), static_cast<int>(sizeZ), 0, lot,
                                    true, false, configuration)) {
            if (!advance()) return;
        }
        lotDeveloper->ConstructDefaultLot(lot, configuration, 0, false);   // vslot 0x38
        lot->Release();
        if (sizeX != 0xffffffffu) {
            std::uint32_t count = ~sizeX;
            do {
                if (!advance()) return;
                ++count;
            } while (count != 0);
        }
        if ((++placed & 0xf) == 0) g_host->GameDelay(1000, true);   // SC4GameDelay
    }
}

// @ 0x10012ad80. PARTIAL: the cheat command needs cSpiralIterator's position walk (recovered in
// SpiralIterator.h, but linking it would add a source file to the test line) and the building-type
// catalogue. Left out; see notes/ledger_pending/cSC4ZoneDeveloper.txt.
void cSC4ZoneDeveloper::PlopAllBuildingTypes() {
    // SKIPPED: 0x10012ad80
}

// @ 0x100130994. Writes one zone highlight overlay instance per cell of the region: for every pending
// lot placement first, then for every lot that has no pending placement. The boundary side values are
// 0 when the neighbour belongs to the same pending lot, 2 when the cell's own lot faces that side and 1
// otherwise; GetBoundaryLotTexture turns them into the texture/shape pair the terrain view draws.
void cSC4ZoneDeveloper::UpdateHighlightTextures(const SC4CellRegion<int>* region) {
    if (!region || !terrainView_) return;
    const std::uint32_t zoneColor = zoneManager_ ? zoneManager_->ZoneDragColor(currentZoneType_) : 0;
    ZoneHighlightTexture texture;
    texture.instance = zoneColor;
    const auto emit = [&](int x, int z, BoundaryType boundary) {
        std::uint32_t ruleTexture = 0;
        std::uint8_t shape = 0;
        bool flag = false;
        GetBoundaryLotTexture(boundary, ruleTexture, shape, flag);
        texture.texture = highlightScale_ + static_cast<int>(ruleTexture);
        const SC4Point<int> cell{x, z};
        terrainView_->PlaceHighlightTexture(cell, texture, shape, flag, zoneColor, 3);   // vslot 0x40
    };
    const bool industrial = static_cast<unsigned>(currentZoneType_ - 7) < 3;
    const auto findPending = [&](int x, int z) -> const PendingLot* {
        auto it = pendingLots_.find(SC4Point<int>{x, z});
        return it == pendingLots_.end() ? nullptr : &it->second;
    };
    // 1. the pending lot placements
    if (!industrial) {
        for (const auto& entry : pendingLots_) {
            const int x = entry.first.x, z = entry.first.z;
            const PendingLot& own = entry.second;
            BoundaryType boundary;
            const auto side = [&](int nx, int nz, int facingOfSide) -> int {
                const PendingLot* other = findPending(nx, nz);
                if (other && other->first == own.first) return 0;
                if (own.second == facingOfSide) return 2;
                return 1;
            };
            boundary.sides[0] = side(x, z - 1, 0);
            boundary.sides[1] = side(x + 1, z, 1);
            boundary.sides[2] = side(x, z + 1, 2);
            boundary.sides[3] = side(x - 1, z, 3);
            emit(x, z, boundary);
        }
    }
    // 2. the lots of the region that have no pending placement
    for (int x = region->bounds.x0; x <= region->bounds.x1; ++x) {
        for (int z = region->bounds.z0; z <= region->bounds.z1; ++z) {
            if (!region->Test(x, z)) continue;
            if (ExistsNetworkOfType(x, z, kNetworkFlagAnyConnection)) continue;
            cSC4Lot* lot = GetLot(x, z, false);
            if (!lot) continue;
            if (static_cast<int>(lot->GetZoneType()) != currentZoneType_) continue;
            if (findPending(x, z)) continue;
            BoundaryType boundary;
            if (industrial) {
                boundary.sides[0] = boundary.sides[1] = boundary.sides[2] = boundary.sides[3] = 0;
            } else {
                const int facing = lot->GetFacing();   // vslot 0xb8
                const auto side = [&](int nx, int nz, int facingOfSide) -> int {
                    cSC4Lot* other = GetLot(nx, nz, false);
                    if (other && other == lot) return 0;
                    return facing == facingOfSide ? 2 : 1;
                };
                boundary.sides[0] = side(x, z - 1, 0);
                boundary.sides[1] = side(x + 1, z, 1);
                boundary.sides[2] = side(x, z + 1, 2);
                boundary.sides[3] = side(x - 1, z, 3);
            }
            emit(x, z, boundary);
        }
    }
}

// @ 0x100133be4. cZoneDeveloperLotFilter: the configuration must accept the developer's purpose and
// wealth and its growth stage must not exceed the style the growth list selected.
bool cZoneDeveloperLotFilter::IsLotConfigurationIncluded(cSC4LotConfiguration* configuration) {
    if (!configuration) return false;
    if (!configuration->IsCompatibleWithPurposeType(purpose_)) return false;   // vslot 0x88
    if (!configuration->IsCompatibleWithWealthType(wealth_)) return false;     // vslot 0x80
    return static_cast<int>(configuration->GetGrowthStage()) <= growthStage_;  // vslot 0x58
}

//---------------------------------------------------------------------------------------------------------------------
// misc accessors that the original defines out of line
//---------------------------------------------------------------------------------------------------------------------

bool cSC4ZoneDeveloper::CheatCodesWereRegistered() const {
    return g_cheatRegisteringObjects.find(this) != g_cheatRegisteringObjects.end();
}

IZoneDeveloperLotIterator* cSC4ZoneDeveloper::GetSafeLotIterator() {
    // SKIPPED: the original returns cSC4SafeList<cISC4Lot*>::GetIterator (0x100133466), an unrecovered
    // class that also keeps the live-iterator list at +0xc8.
    return nullptr;
}

}  // namespace sc4
