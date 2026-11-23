#include "app/NativeNetworkManager.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "app/NativeCity.h"
#include "app/NativeDemolition.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/RZServices.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZIniFile.h"
#include "sc4/CommonIIDs.h"
#include "sc4/ComplexViewOccupantFilter.h"
#include "sc4/DBSegment.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/NetworkManager.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "sc4/NetworkTool.h"
#include "sc4/OccupantManager.h"
#include "sc4/PathInfoFactory.h"
#include "sc4/PipeConnectionOccupant.h"
#include "sc4/PipeTool.h"
#include "sc4/PowerLineTool.h"
#include "sc4/ResourceAsFile.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SubwayTool.h"
#include "sc4/UndergroundNetworkTool.h"
#include "sc4/cSTETerrain.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {

// The network manager's view of a network piece (INetworkManagerOccupant): the port's pieces do not answer
// cISC4NetworkOccupant, so the manager gets this through SetNetworkOccupantViewProvider. A piece has one view at a time
// (the manager compares the interface pointers it keeps, as the original compares the cISC4NetworkOccupant pointers), and the
// view answers QueryInterface for the piece, so the manager's SC4WriteGZSerializable writes the piece itself.
class ManagerOccupantView;
std::mutex g_viewsMutex;
std::unordered_map<sc4::cSC4NetworkOccupant*, ManagerOccupantView*> g_views;

class ManagerOccupantView final : public sc4::INetworkManagerOccupant {
public:
    explicit ManagerOccupantView(sc4::cSC4NetworkOccupant* occupant)
        : occupant_(occupant), prebuilt_(dynamic_cast<sc4::cSC4NetworkOccupantWithPreBuiltModel*>(occupant)) {
        occupant_->AddRef();
    }
    bool QueryInterface(std::uint32_t iid, void** out) override { return occupant_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override {
        std::lock_guard<std::mutex> lock(g_viewsMutex);
        return ++refs_;
    }
    std::uint32_t Release() override {
        {
            std::lock_guard<std::mutex> lock(g_viewsMutex);
            if (--refs_ != 0) return refs_;
            g_views.erase(occupant_);
        }
        occupant_->Release();
        delete this;
        return 0;
    }
    // The class's own answers: the pre-built model pieces (bridges and the other model pieces) are managed globally and light
    // through their own SetLit (vslots 0x28 / 0x1e8 of the original's cISC4NetworkOccupant).
    bool IsManagedGlobally() const override { return prebuilt_ ? prebuilt_->IsManagedGlobally() : occupant_->IsManagedGlobally(); }
    bool HasNetworkFlag(std::uint32_t flag) override { return occupant_->HasNetworkFlag(flag); }
    std::uint32_t GetNetworkFlags() override { return occupant_->GetNetworkFlag(); }
    void SetLit(bool lit) override {
        if (prebuilt_) prebuilt_->SetLit(lit);
        else occupant_->SetLit(lit);
    }
    bool SupportsLighting() override { return occupant_->CanBeLit(); }
    void UpdateViewRotation() override {}
    sc4::cISC4Occupant* AsOccupant() override { return occupant_; }

    std::uint32_t refs_ = 0;   // guarded by g_viewsMutex

private:
    sc4::cSC4NetworkOccupant* occupant_;
    sc4::cSC4NetworkOccupantWithPreBuiltModel* prebuilt_;
};

sc4::INetworkManagerOccupant* NetworkOccupantView(sc4::cISC4Occupant* occupant) {
    auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
    if (!network) return nullptr;
    std::lock_guard<std::mutex> lock(g_viewsMutex);
    ManagerOccupantView*& view = g_views[network];
    if (!view) view = new ManagerOccupantView(network);
    ++view->refs_;
    return view;
}

// A network tool for the network manager: cSC4NetworkTool for the surface types, the pipe tool for type 4, the power line tool
// for type 5 and the subway tool for type 7. The tool itself is
// made on first use (the manager Inits its tool around each demolition), when the view's network tool host is installed.
class ManagerRoadTool final : public sc4::INetworkManagerTool {
public:
    explicit ManagerRoadTool(std::uint32_t type) : type_(type) {}
    ~ManagerRoadTool() { if (tool_ && type_ != sc4::kPipeNetworkType && type_ != sc4::kSubwayNetworkType) tool_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    bool Init() override {
        if (!tool_ && type_ == sc4::kPipeNetworkType) {
            tool_ = NativePipeTool();
        } else if (!tool_ && type_ == sc4::kSubwayNetworkType) {
            tool_ = NativeSubwayTool();
        } else if (!tool_ && type_ == sc4::kNetworkPowerLine) {
            tool_ = new sc4::cSC4PowerLineTool();   // GetNetworkTool case 5: operator new(0x400) + cSC4PowerLineTool()
            tool_->AddRef();
        } else if (!tool_) {
            tool_ = new sc4::cSC4NetworkTool(type_);
            tool_->AddRef();
        }
        return tool_->Init();
    }
    bool Shutdown() override { return tool_ ? tool_->Shutdown() : true; }
    sc4::cSC4NetworkTool* Tool() const { return tool_; }
    void PlaceNetworkOccupantById(int x, int z, std::uint32_t pieceId, bool usable, sc4::eNetworkType type) override {
        if (tool_) tool_->PlaceNetworkOccupantById(x, z, pieceId, usable, static_cast<std::uint32_t>(type));
    }
    bool PlaceNetworkOccupants(int x0, int z0, int x1, int z1, bool place) override {
        return tool_ && tool_->PlaceNetworkOccupants(x0, z0, x1, z1, 0, nullptr, place, -1);
    }
    bool RemoveOrReplaceOccupant(sc4::cISC4Occupant* occupant, bool& replaced) override {
        auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        return tool_ && network && tool_->RemoveOrReplaceOccupant(network, replaced);
    }
    float GetNetworkHeightForTerrainHeight(float terrainHeight) override {
        return tool_ ? tool_->GetNetworkHeightForTerrainHeight(terrainHeight) : terrainHeight;
    }
    // The pipe and subway tools re-solve their pieces under changed terrain.
    void UpdateUndergroundOccupantTerrainHeights(const std::vector<sc4::SC4PointU>& cells, std::list<sc4::cSC4NetworkOccupant*>& pieces,
                                                 sc4::eNetworkType type) override {
        auto* underground = dynamic_cast<sc4::cSC4UndergroundNetworkTool*>(tool_);
        if (!underground) {
            sc4::INetworkManagerTool::UpdateUndergroundOccupantTerrainHeights(cells, pieces, type);
            return;
        }
        underground->UpdateUndergroundOccupantTerrainHeights(cells, pieces, static_cast<std::uint32_t>(type));
    }
    // The manager hands one reference per view; the tool borrows the pieces.
    void DestroyOccupantsAndResolve(std::vector<sc4::INetworkManagerOccupant*>& occupants) override {
        std::vector<sc4::cSC4NetworkOccupant*> pieces;
        for (sc4::INetworkManagerOccupant* view : occupants)
            if (auto* network = view ? dynamic_cast<sc4::cSC4NetworkOccupant*>(view->AsOccupant()) : nullptr) pieces.push_back(network);
        if (tool_) tool_->DestroyOccupantsAndResolve(pieces);
        for (sc4::INetworkManagerOccupant* view : occupants)
            if (view) view->Release();
    }

private:
    std::uint32_t type_;
    sc4::cSC4NetworkTool* tool_ = nullptr;
    std::uint32_t refs_ = 0;
};

// GetPathInfo's two services: the manager's (UK) path id mapping and the path files (type 0x296678f7) from the resource manager,
// whose factory reads the TEXT path format.
class PathHost final : public sc4::INetworkOccupantPathHost {
public:
    std::uint32_t MapPath(std::uint32_t pathId) override {
        sc4::cSC4NetworkManager* manager = CityNetworkManager();
        return manager ? manager->MapPath(pathId) : pathId;
    }
    sc4::cISC4PathInfo* LoadPathInfo(const cGZPersistResourceKey& key) override {
        void* out = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa9667793u, &out, 0, 0)) return nullptr;
        return static_cast<sc4::cISC4PathInfo*>(out);
    }
} g_pathHost;

void RegisterPathInfoFactory() {
    static bool registered = false;
    if (registered || !SL::spRM) return;
    registered = true;
    static sc4::cSC4PathInfoFactory factory;
    factory.AddRef();
    SL::spRM->RegisterObjectFactory(0x496678feu, sc4::kPathInfoResourceType, &factory);
    sc4::SetNetworkOccupantPathHost(&g_pathHost);
}

// SL::spOccupantManager as the network manager queries it: the city's occupant manager, borrowed (the city keeps it).
sc4::cISC4OccupantManager* CityOccupants() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}

// The three occupant manager slots the network manager calls: 0x120 GetOccupantsByBBox (by type), 0x180 ReleaseOccupantList
// and 0x160 IterateOccupants (by type, the whole city).
class ManagerOccupants final : public sc4::INetworkManagerOccupantManager {
public:
    void GetOccupantsByBBox(sc4::ilist<sc4::cISC4Occupant*>& out, const float* min, const float* max, std::uint32_t type,
                            std::uint32_t maxCount) override {
        if (sc4::cISC4OccupantManager* manager = CityOccupants()) manager->GetOccupantsByBBox(out, min, max, type, maxCount);
    }
    void ReleaseOccupantList(sc4::ilist<sc4::cISC4Occupant*>& list) override {
        if (sc4::cISC4OccupantManager* manager = CityOccupants()) manager->ReleaseOccupantList(list);
        else {
            for (sc4::cISC4Occupant* occupant : list) if (occupant) occupant->Release();
            list.clear();
        }
    }
    void IterateOccupants(bool (*callback)(sc4::cISC4Occupant*, void*), void* context, std::uint32_t type) override {
        if (sc4::cISC4OccupantManager* manager = CityOccupants()) manager->IterateOccupants(callback, context, nullptr, nullptr, type);
    }
    // GetOccupantsByStandardCityCells with a cSC4NetworkORFilter(mask): the network pieces carrying any of the flags.
    void GetNetworkOccupantsByCells(sc4::ilist<sc4::cISC4Occupant*>& out, const std::int32_t* minCell, const std::int32_t* maxCell,
                                    std::uint32_t flagMask) override {
        sc4::cISC4OccupantManager* manager = CityOccupants();
        if (!manager) return;
        sc4::OccupantList list;
        if (!manager->GetOccupantsByStandardCityCells(list, minCell, maxCell, 0xc772bf98u, 0xffffffffu)) return;
        for (sc4::cISC4Occupant* occupant : list) {
            auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
            if (network && (network->GetNetworkFlag() & flagMask) != 0) out.push_back(occupant);
            else occupant->Release();
        }
    }
} g_managerOccupants;

// The network manager's services, as the original's cSC4NetworkManager reaches them directly.
class NetworkManagerHost final : public sc4::INetworkManagerHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    // Init: the language manager's GetLanguageUtility(0) -> GetLanguageRoadDrivingSide (cRZSysServPtr<cIGZLanguageManager>).
    // Without a language manager the default (right-hand traffic) answer stands.
    bool LanguageFlag() override {
        cIGZLanguageManager* languages = RZ::LM();
        cIGZLanguageUtility* utility = languages ? languages->GetLanguageUtility(0) : nullptr;
        return utility == nullptr || static_cast<std::uint8_t>(utility->GetLanguageRoadDrivingSide()) != 0;
    }
    // Reload: SC4GetResourceAsFile({0, 0x8a5971c5, 0x8a5993b9}, &file, false) read as an ini file (the ini keeps a reference).
    cRZIniFile* OpenNetworkIni() override {
        cIGZFile* file = nullptr;
        const cGZPersistResourceKey key{sc4::kNetworkManagerIniType, sc4::kNetworkManagerIniGroup, sc4::kNetworkManagerIniInstance};
        if (!sc4::SC4GetResourceAsFile(key, &file, false) || !file) return nullptr;
        auto* ini = new cRZIniFile(file);
        file->Release();
        return ini;
    }
    // Init / GetNetworkTool(type, true): every type gets its tool (the surface types, the pipe, the power line, the subway).
    sc4::INetworkManagerTool* CreateNetworkTool(sc4::eNetworkType type) override {
        const auto value = static_cast<std::uint32_t>(type);
        if (value >= sc4::kNetworkToolCount) return nullptr;
        return new ManagerRoadTool(value);
    }
    sc4::cISC4OccupantFilter* CreateOccupantFilter(bool subway) override {
        sc4::cSC4OccupantFilter* filter = subway ? static_cast<sc4::cSC4OccupantFilter*>(new sc4::cSC4SubwayOccupantFilter())
                                                 : static_cast<sc4::cSC4OccupantFilter*>(new sc4::cSC4WaterOccupantFilter());
        filter->AddRef();
        return filter;
    }
    bool IsOccupantSubwayStation(sc4::cISC4Occupant* occupant) override {
        return sc4::cSC4SubwayOccupantFilter::IsOccupantSubwayStation(occupant);
    }
    void FindPipeOccupant(int x, int z, sc4::cSC4PipeOccupant*& out) override {
        sc4::cSC4PipeTool::FindPipeOccupant(CityOccupants(), x, z, out);
    }
    void FindPipeConnectionOccupant(int x, int z, sc4::cSC4PipeConnectionOccupant*& out) override {
        sc4::cSC4PipeTool::FindPipeConnectionOccupant(CityOccupants(), x, z, out);
    }
    sc4::INetworkManagerOccupantManager* OccupantManager() override { return CityOccupants() ? &g_managerOccupants : nullptr; }
    // GetFirstOccupantByStandardCityCell with a cSC4NetworkORFilter: the first network piece on the cell with any of the flags.
    sc4::cSC4NetworkOccupant* FirstNetworkOccupantAt(int x, int z, std::uint32_t flagMask) override {
        sc4::cISC4OccupantManager* occupants = CityOccupants();
        if (!occupants || x < 0 || z < 0) return nullptr;
        const int cell[2] = {x, z};
        sc4::OccupantList list;
        sc4::cSC4NetworkOccupant* found = nullptr;
        if (!occupants->GetOccupantsByStandardCityCells(list, cell, cell, 0xc772bf98u, 0xffffffffu)) return nullptr;
        for (sc4::cISC4Occupant* occupant : list) {
            auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
            if (!found && network && (network->GetNetworkFlag() & flagMask) != 0) {
                network->AddRef();
                found = network;
            }
        }
        occupants->ReleaseOccupantList(list);
        return found;
    }
    sc4::cISCResExemplar* PrebuiltModelExemplar(std::uint32_t group, std::uint32_t instance) override {
        const cGZPersistResourceKey key{0x6534284au, group, instance};
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5u, &raw, 0, nullptr) || !raw) return nullptr;
        return static_cast<sc4::cISCResExemplar*>(raw);
    }
    bool LotBuildingAltitude(int x, int z, float& altitude) override {
        sc4::cSC4Lot* lot = NativeLotAtCell(x, z, false);
        cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
        void* raw = nullptr;
        if (!building || !building->QueryInterface(sc4::kGZIID_cISC4Occupant, &raw) || !raw) return false;
        auto* occupant = static_cast<sc4::cISC4Occupant*>(raw);
        float position[3] = {};
        occupant->GetPosition(position);
        occupant->Release();
        altitude = position[1];
        return true;
    }
    void CityCellCounts(std::int32_t& cellsX, std::int32_t& cellsZ) override {
        sc4::cSC4City* city = HostCity();
        cellsX = city ? city->CellCountX() : 0;
        cellsZ = city ? city->CellCountZ() : 0;
    }
    float TerrainAltitude(float x, float z) override {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return 0.0f;
        auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
        const float altitude = terrain->GetAltitude(x, z);
        static_cast<cIGZSerializable*>(raw)->Release();
        return altitude;
    }
    // SL::sp24HourClock->IsNight(): the city's 24-hour clock.
    bool IsNight() override {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Clock24Hour) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(sc4::kGZIID_PlainObject, &raw) || !raw) return false;
        return static_cast<sc4::cSC424HourClock*>(raw)->IsNight();
    }
    // The tracked occupants are views that answer QueryInterface for their pieces, so the piece itself is written.
    bool WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) override {
        return sc4::SC4WriteGZSerializable(stream, object);
    }
    // SC4ReadGZSerializable(stream, 0xa821ef94, &out, false): the piece is read as an occupant and handed out as its view (the
    // port's pieces do not answer cISC4NetworkOccupant). An object that is not a network piece fails, as the original's
    // QueryInterface does; a null object reads as null.
    bool ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override {
        if (iid != sc4::kGZIID_cISC4NetworkOccupant) return sc4::SC4ReadGZSerializable(stream, iid, out, false);
        *out = nullptr;
        void* raw = nullptr;
        if (!sc4::SC4ReadGZSerializable(stream, sc4::kGZIID_cISC4Occupant, &raw, false)) return false;
        if (!raw) return true;
        auto* occupant = static_cast<sc4::cISC4Occupant*>(raw);
        *out = NetworkOccupantView(occupant);
        occupant->Release();
        return *out != nullptr;
    }
} g_host;

}  // namespace

// The subway tool (cSC4SubwayTool, network type 7): one for the program, as the pipe tool (its destructor traps too).
sc4::cSC4NetworkTool* NativeSubwayTool() {
    static sc4::cSC4SubwayTool* tool = [] {
        auto* made = new sc4::cSC4SubwayTool();
        made->AddRef();
        return made;
    }();
    return tool;
}

// The pipe tool (cSC4PipeTool, network type 4). Its destructor traps as the original's does (the game never destroys a
// network tool), so one tool serves every city and keeps a reference for the life of the program.
sc4::cSC4NetworkTool* NativePipeTool() {
    static sc4::cSC4PipeTool* tool = [] {
        auto* made = new sc4::cSC4PipeTool();
        made->AddRef();
        return made;
    }();
    return tool;
}

void InstallNetworkManagerHost() {
    RegisterPathInfoFactory();
    sc4::SetNetworkOccupantViewProvider(&NetworkOccupantView);
    sc4::SetNetworkManagerHost(&g_host);
}

void PrintNetworkManagerHost() {
    sc4::INetworkManagerHost* host = sc4::NetworkManagerHost();
    std::printf("netmanager: host %s, message server %s\n", host == &g_host ? "native" : host ? "other" : "none",
                host && host->MessageServer() ? "yes" : "no");
    if (!host) return;
    cRZIniFile* ini = host->OpenNetworkIni();
    std::printf("netmanager: ini {%08x, %08x, %08x} %s\n", sc4::kNetworkManagerIniType, sc4::kNetworkManagerIniGroup,
                sc4::kNetworkManagerIniInstance, ini ? "opened" : "not opened");
    if (ini) {
        for (const char* section : {"Texture remapping", "Shadow remapping", "Shadow edge remapping", "UK path remapping"}) {
            int entries = 0;
            ini->EnumEntries(cRZString(section), [](const cRZString&, const cRZString&, void* count) { ++*static_cast<int*>(count); }, &entries);
            std::printf("netmanager: ini [%s] %d entries\n", section, entries);
        }
        delete ini;
    }
    std::printf("netmanager: language flag (right-hand traffic) %d, night %d\n", host->LanguageFlag() ? 1 : 0, host->IsNight() ? 1 : 0);
    sc4::cSC4NetworkManager* manager = CityNetworkManager();
    if (!manager) { std::printf("netmanager: no city network manager\n"); return; }
    std::size_t sizes[4] = {};
    bool uk = false;
    std::size_t tracked = 0;
    manager->GetDiagnostics(sizes, uk, tracked);
    std::printf("netmanager: remaps texture %zu shadow %zu by-edges %zu uk-path %zu, uk path mapping %d, tracked %zu\n",
                sizes[0], sizes[1], sizes[2], sizes[3], uk ? 1 : 0, tracked);
    int tools = 0;
    for (std::uint32_t type = 0; type < sc4::kNetworkToolCount; ++type)
        if (manager->GetNetworkTool(static_cast<sc4::eNetworkType>(type), false)) ++tools;
    std::printf("netmanager: tools %d of %zu\n", tools, sc4::kNetworkToolCount);
    sc4::cISC4OccupantFilter* water = manager->GetWaterOccupantFilter();
    sc4::cISC4OccupantFilter* subway = manager->GetSubwayOccupantFilter();
    int pieces = 0, waterIn = 0, subwayIn = 0, stations = 0;
    if (sc4::cISC4OccupantManager* occupants = CityOccupants()) {
        sc4::OccupantList all;
        occupants->GetOccupantsByBBox(all, nullptr, nullptr, 0u, 0xffffffffu);
        for (sc4::cISC4Occupant* occupant : all) {
            if (dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant)) ++pieces;
            if (water && water->IsOccupantIncluded(occupant)) ++waterIn;
            if (subway && subway->IsOccupantIncluded(occupant)) ++subwayIn;
            if (manager->IsOccupantSubwayStation(occupant)) ++stations;
        }
        occupants->ReleaseOccupantList(all);
    }
    std::printf("netmanager: filters water %s (%d in) subway %s (%d in), subway stations %d, network pieces %d\n",
                water ? "yes" : "no", waterIn, subway ? "yes" : "no", subwayIn, stations, pieces);
    if (water) water->Release();
    if (subway) subway->Release();
}

void NetworkIntersectionPieces(sc4::cSC4NetworkOccupant* occupant, std::vector<sc4::cSC4NetworkOccupant*>& out) {
    sc4::cSC4NetworkManager* manager = CityNetworkManager();
    sc4::INetworkManagerOccupant* view = occupant ? NetworkOccupantView(occupant) : nullptr;
    if (!manager || !view) {
        if (view) view->Release();
        return;
    }
    sc4::ilist<sc4::cISC4Occupant*> pieces;
    manager->GetAllIntersectionPieces(view, pieces, nullptr, nullptr);
    view->Release();
    for (sc4::cISC4Occupant* piece : pieces) {
        if (auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(piece)) out.push_back(network);   // keeps the list's reference
        else if (piece) piece->Release();
    }
}


// The network tool behind the city network manager's slot for a type (the borrowed tool Init made), or null.
sc4::cSC4NetworkTool* NativeManagerNetworkTool(std::uint32_t type) {
    sc4::cSC4NetworkManager* manager = CityNetworkManager();
    if (!manager) return nullptr;
    auto* view = dynamic_cast<ManagerRoadTool*>(manager->GetNetworkTool(static_cast<sc4::eNetworkType>(type), false));
    if (!view) return nullptr;
    if (!view->Tool()) view->Init();   // the adapter makes its tool on the first Init
    return view->Tool();
}

}  // namespace native

namespace native {
std::uint32_t CityMapShadowTexture(std::uint32_t instance, std::uint8_t& rotation, std::uint32_t networkFlags, std::uint32_t raisedEdges) {
    sc4::cSC4NetworkManager* manager = CityNetworkManager();
    return manager ? manager->MapShadowTexture(instance, rotation, networkFlags, raisedEdges) : 0;
}
}  // namespace native
