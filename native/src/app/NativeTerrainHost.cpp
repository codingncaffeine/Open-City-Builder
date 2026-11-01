// The terrain model's host on the native application (hand-written from the generated stub; scripts/gen_stub_hosts.py skips it).
// The city and its components come from the application; the resource-backed readers use the resource manager.
#include <string>
#include "sc4/DBSegment.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "app/NativeUi.h"
#include "app/NativeLighting.h"
#include "sc4/LightingManager.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZIniFile.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/ISC4City.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "app/NativeCity.h"
#include "sc4/Exemplar.h"
#include "sc4/cSTETerrain.h"
#include "sc4/SurfaceWater.h"
#include "sc4/MySimAgentSimulator.h"
#include "sc4/AdvisorSystem.h"
#include "gz/GZCOM.h"
#include "platform/Unwired.h"

namespace native {
bool NeighborsHasConnection(cIGZUnknown* component, std::uint32_t type, int x, int z);      // NativeSurfaceWaterNeighbors.cpp
void NeighborsDeleteConnection(cIGZUnknown* component, std::uint32_t type, int x, int z);
void NeighborsCreateConnection(cIGZUnknown* component, std::uint32_t type, int x, int z);
namespace {
using namespace sc4;
cISC4City* g_city = nullptr;

cSC4City* City4() { return dynamic_cast<cSC4City*>(g_city); }
cIGZUnknown* Component(CityComponent c) { cSC4City* city = City4(); return city ? city->GetComponent(c) : nullptr; }

class IniAdapter final : public ITerrainIniFile {
public:
    explicit IniAdapter(const std::string& path) : ini_(cRZString(path.c_str())) {}
    bool ReadEntry(const cRZString& section, const cRZString& key, cRZString& value) override { return ini_.ReadEntry(section, key, value); }
    bool EnumEntries(const cRZString& section, EnumCallback callback, void* context) override { return ini_.EnumEntries(section, callback, context); }
private:
    cRZIniFile ini_;
};

class KeyFilter final : public cIGZPersistResourceKeyFilter {
public:
    KeyFilter(std::uint32_t type, std::uint32_t group) : type_(type), group_(group) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_ && key.group == group_; }
private:
    std::uint32_t type_, group_;
};

// The surface water's services on the native application. What the city can answer today is answered; the rest logs SC4_UNWIRED.
cSTESurfaceWater* WaterOf(cIGZUnknown* object) {
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISTESurfaceWater, &serializable) || !serializable) return nullptr;
    auto* water = static_cast<cSTESurfaceWater*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return water;
}

struct NativeSurfaceWaterHost final : ISurfaceWaterHost {
    cSTETerrain* Terrain() override {
        cIGZUnknown* object = Component(CityComponent::Terrain);
        void* serializable = nullptr;
        if (!object || !object->QueryInterface(0xab953253, &serializable) || !serializable) return nullptr;
        auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
        static_cast<cIGZSerializable*>(serializable)->Release();
        return terrain;
    }
    bool CityInitialized() override { cSC4City* city = City4(); return city && city->CellCountX() > 0; }   // PARTIAL: spCity vslot 0xb0 is the city's "initialised" flag
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = static_cast<int>(x * 0.0625f); cz = static_cast<int>(z * 0.0625f); }
    int CitySizeX() override { cSC4City* city = City4(); return city ? city->CellCountX() : 0; }
    int CitySizeZ() override { cSC4City* city = City4(); return city ? city->CellCountZ() : 0; }
    bool CellInCity(int x, int z) override { return x >= 0 && z >= 0 && x < CitySizeX() && z < CitySizeZ(); }   // PARTIAL: spCity vslot 0x318 not verified
    void AddNotifications(cSTESurfaceWater& water) override {
        if (!SL::spMS2) return;
        for (std::uint32_t id : {kWaterMsgCityCell1, kWaterMsgCityCell4, kWaterMsgCityCell5, kWaterMsgNetworkBuilt, kWaterMsgOccupantAdded, kWaterMsgOccupantMoved})
            SL::spMS2->AddNotification(static_cast<cIGZMessageTarget2*>(&water), id);
    }
    void RemoveNotifications(cSTESurfaceWater& water) override {
        if (!SL::spMS2) return;
        for (std::uint32_t id : {kWaterMsgCityCell1, kWaterMsgCityCell4, kWaterMsgCityCell5, kWaterMsgNetworkBuilt, kWaterMsgOccupantAdded, kWaterMsgOccupantMoved})
            SL::spMS2->RemoveNotification(static_cast<cIGZMessageTarget2*>(&water), id);
    }
    void RegisterTicks(cSTESurfaceWater&) override { SC4_UNWIRED("cSTESurfaceWater tick registration (spSimulator vslot 0x98, ATM vslot 0x28): the water never animates"); }
    void UnregisterTicks(cSTESurfaceWater&) override {}
    // The neighbours simulator is a city component (NativeSurfaceWaterNeighbors.cpp: its header cannot share this translation unit).
    bool NeighborHasConnection(std::uint32_t type, int x, int z) override { return NeighborsHasConnection(Component(CityComponent::NeighborsSimulator), type, x, z); }
    void NeighborDeleteConnection(std::uint32_t type, int x, int z) override { NeighborsDeleteConnection(Component(CityComponent::NeighborsSimulator), type, x, z); }
    void NeighborCreateConnection(std::uint32_t type, int x, int z) override { NeighborsCreateConnection(Component(CityComponent::NeighborsSimulator), type, x, z); }
    IWaterNetworkOccupant* NetworkOccupantAt(int, int, std::uint32_t, bool) override { SC4_UNWIRED("ISurfaceWaterHost::NetworkOccupantAt (traffic network map)"); return nullptr; }
    IWaterNetworkOccupant* AsNetworkOccupant(cIGZUnknown*) override { SC4_UNWIRED("ISurfaceWaterHost::AsNetworkOccupant"); return nullptr; }
    IWaterCellOccupant* AsCellOccupant(cIGZUnknown*) override { SC4_UNWIRED("ISurfaceWaterHost::AsCellOccupant"); return nullptr; }
    const void* LotAtCell(int, int) override { SC4_UNWIRED("ISurfaceWaterHost::LotAtCell (lot manager)"); return nullptr; }
    void OccupantsInCell(int, int, std::uint32_t, std::vector<IWaterCellOccupant*>&) override { SC4_UNWIRED("ISurfaceWaterHost::OccupantsInCell (occupant manager)"); }
};

// The MySim agent simulator's services on the native application.
struct NativeMySimAgentHost final : IMySimAgentHost {
    void Advisors(std::vector<MySimAdvisorEntry>& out) override {
        out.clear();
        cIGZUnknown* object = Component(CityComponent::AdvisorSystem);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4AdvisorSystem, &raw) || !raw) return;
        auto* system = static_cast<cISC4AdvisorSystem*>(raw);
        if (AdvisorMap* advisors = system->GetAdvisors())
            for (auto& entry : *advisors)
                if (entry.second) out.push_back({entry.second.get(), entry.second->GetID()});
        system->Release();
    }
    cISC4MySim* AsMySim(cIGZUnknown* object) override {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4MySimLink, &raw)) return nullptr;
        return static_cast<cISC4MySim*>(raw);
    }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void RegisterGameData(cSC4MySimAgentSimulator&) override { SC4_UNWIRED("cSC4MySimAgentSimulator game data registry (get_sim / get_count for Lua): cISC4GameDataRegistry is not recovered"); }
    void UnregisterGameData(cSC4MySimAgentSimulator&) override {}
    bool RiotInProgress() override { SC4_UNWIRED("IMySimAgentHost::RiotInProgress (spCity vslot 0x240 -> 0x50)"); return false; }
    void CellCenter(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = City4()) city->CellCenterToPosition(cellX, cellZ, x, z);
        else { x = 0; z = 0; }
    }
};
struct NativeTerrainHost final : ITerrainHost {
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    cIGZMessage2Standard* NewMessage() override { auto* message = new cRZMessage2Standard(); message->AddRef(); return message; }
    bool WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) override { return SC4WriteGZSerializable(stream, object); }
    bool ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override { return SC4ReadGZSerializable(stream, iid, out, false); }
    double RandomDoubleRangeUniform(double lo, double hi) override { return std::uniform_real_distribution<double>(lo, hi)(rng_); }
    double RandomDoubleRangeGaussian(double mean, double sigma) override { return std::normal_distribution<double>(mean, sigma)(rng_); }
    void ApplyTerrainLightingRow(std::int32_t z, std::int32_t x, std::int32_t endX, float* normals, std::int32_t normalStride, void* colours, std::int32_t colourStride) override {
        // PARTIAL: one fixed sun and an ambient term; the original asks the lighting manager (SL::spLightingManager vslot 0xe0,
        // GetVertexColors) for the time-of-day colours of the vertex.
        if (std::getenv("SC4_DEBUG")) { static int rows = 0; if (rows++ < 3 || rows % 64 == 0) std::fprintf(stderr, "terrain lighting row %d x %d..%d (%d rows so far)\n", z, x, endX, rows); }
        if (sc4::cSC4LightingManager* lighting = LightingManager()) {
            // the third argument of cSTETerrain::DoLighting is the vertex count of the row
            lighting->GetTerrainVertexColors(z, x, endX, reinterpret_cast<const cS3DVector3*>(normals), normalStride, static_cast<std::uint8_t*>(colours), colourStride);
            return;
        }
        endX += x;
        auto* normal = reinterpret_cast<std::uint8_t*>(normals);
        auto* colour = static_cast<std::uint8_t*>(colours);
        for (std::int32_t i = x; i < endX; ++i, normal += normalStride, colour += colourStride) {
            const float* n = reinterpret_cast<const float*>(normal);
            const float dot = n[0] * -0.45f + n[1] * 0.78f + n[2] * 0.43f;
            float lit = 0.80f + 0.45f * (dot > 0.0f ? dot : 0.0f);
            if (lit > 1.0f) lit = 1.0f;
            const std::uint8_t byte = static_cast<std::uint8_t>(lit * 255.0f);
            for (int k = 0; k < 3; ++k) colour[k] = byte;   // colour B: the lit colour the textured passes modulate
            for (int k = 3; k < 6; ++k) colour[k] = 0;      // colour A: the base the textured layers add onto
        }
    }
    cIGZUnknown* City() override { return g_city; }
    std::uint32_t CityCellCountX() override { cSC4City* city = City4(); if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "terrain host: city %p cells %d\n", static_cast<void*>(g_city), city ? city->CellCountX() : -1); return city ? static_cast<std::uint32_t>(city->CellCountX()) : 0; }
    std::uint32_t CityCellCountZ() override { cSC4City* city = City4(); return city ? static_cast<std::uint32_t>(city->CellCountZ()) : 0; }
    std::int32_t CityTerrainType() override { return {}; }
    cIGZUnknown* CityLandSimulator() override { return {}; }
    cIGZUnknown* CityFloraSimulator() override { return Component(CityComponent::FloraSimulator); }
    cIGZUnknown* CityEffectsManager() override { return Component(CityComponent::EffectsManager); }
    cIGZUnknown* Simulator() override { return Component(CityComponent::Simulator); }
    cIGZUnknown* SurfaceWater() override { return Component(CityComponent::SurfaceWater); }
    bool SurfaceWaterInit() override { cSTESurfaceWater* water = WaterOf(Component(CityComponent::SurfaceWater)); return water && water->Init(); }
    void RegisterWithSimulator(cSTETerrain* terrain) override {}
    void UnregisterFromSimulator(cSTETerrain* terrain) override {}
    void AddToAutomataManager(cSTETerrain* terrain) override {}
    void RemoveFromAutomataManager(cSTETerrain* terrain) override {}
    cIGZCommandServer* CommandServer() override { return {}; }
    bool AddTerrainNotification(cSTETerrain* terrain, std::uint32_t type) override { return {}; }
    bool RemoveTerrainNotification(cSTETerrain* terrain, std::uint32_t type) override { return {}; }
    void UpdateLandSimulator(cIGZUnknown* landSimulator) override {}
    void ResetLandSimulator(cIGZUnknown* landSimulator) override {}
    bool WorldManagerBusy() override { return {}; }
    bool PreferencesFlag4ec() override { return {}; }
    std::uint32_t ViewZoomLevel() override { return {}; }
    void VisibleEdges(std::int32_t& horizontal, std::int32_t& vertical) override {}
    ITerrainView* CreateTerrainView() override { return {}; }
    void RegisterStaticShutdown() override {}
    void UnregisterStaticShutdown() override {}
    cIGZPersistResourceManager* AcquireResourceManager() override { return SL::spRM; }
    void ReleaseResourceManager() override {}
    void FloodFillDistance(std::vector<std::vector<std::uint8_t>>& grid, std::int32_t source, std::int32_t threshold) override {}
    cISCPropertyHolder* GetPropertyHolder(const cGZPersistResourceKey& key) override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &resource, 0, nullptr) || !resource) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(resource);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        if (holder) holder->AddRef();
        exemplar->Release();
        return holder;
    }
    void ListTerrainTextureResources(std::vector<cGZPersistResourceKey>& out) override {
        ListResources(out, 0x6534284a, 0x89c2a517);
    }
    static void ListResources(std::vector<cGZPersistResourceKey>& out, std::uint32_t type, std::uint32_t group) {
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "terrain host: ListResources spRM %p\n", static_cast<void*>(SL::spRM));
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        KeyFilter filter(type, group);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "terrain host: resource list type %08x group %08x -> %u keys\n", type, group, list ? list->Size() : 0);
        if (!list) return;
        for (std::uint32_t i = 0; i < list->Size(); ++i) out.push_back(*list->GetKey(i));
        list->Release();
    }
    ITerrainIniFile* OpenTerrainIni() override {
        // SC4GetResourceAsFile({0x8a5971c5, 0, 0xaa597172}): the terrain ini, read through the recovered cRZIniFile from a temporary copy.
        std::vector<std::uint8_t> bytes;
        if (!LoadResourceBytes(0, 0x8a5971c5, 0xaa597172, bytes)) return nullptr;
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "terrain host: terrain.ini %zu bytes\n", bytes.size());
        const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp/") + "sc4_terrain.ini";
        if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f); } else return nullptr;
        auto* ini = new IniAdapter(path);
        if (std::getenv("SC4_DEBUG")) { cRZString v; const bool ok = ini->ReadEntry(cRZString("TropicalTextureMapTable"), cRZString("HighCliff"), v); std::fprintf(stderr, "terrain host: HighCliff %d '%s' path %s\n", int(ok), v.ToChar(), path.c_str()); }
        return ini;
    }
    void ChangeTerrainType(std::int32_t terrainType) override {}
    void SetUpTexturesForCurrentTerrainTypeAndZoom(std::int32_t zoom) override {}
    std::uint32_t TerrainTextureSubTextureCount(std::int32_t zoom, std::uint32_t textureId) override { return {}; }
    std::int32_t MoistureIndexAtX(float worldX) override { return {}; }
    std::uint32_t RandomUint32UniformBelow(std::uint32_t bound) override { return {}; }
    void* LoadBrushComponentResource(std::uint32_t instance) override { return {}; }
    void* BrushComponentTextureInfo(void* resource) override { return {}; }
    bool BrushTextureInfoSize(void* textureInfo, std::uint32_t& width, std::uint32_t& height) override { return {}; }
    const std::uint8_t* BrushTexturePixels(void* textureInfo) override { return {}; }
    bool SurfaceWaterRain(std::int32_t vertexIndex, TerrainBrushContext& context, TerrainRect& rect, float amount) override {
        cSTESurfaceWater* water = WaterOf(Component(CityComponent::SurfaceWater));
        return water && water->Rain(vertexIndex, context, rect, amount);
    }
    void SurfaceWaterAddRain(std::int32_t vertexIndex, float amount) override { if (cSTESurfaceWater* water = WaterOf(Component(CityComponent::SurfaceWater))) water->AddRain(vertexIndex, amount); }
    void SurfaceWaterCreateSpring(std::int32_t vertexIndex) override { if (cSTESurfaceWater* water = WaterOf(Component(CityComponent::SurfaceWater))) water->CreateSpring(vertexIndex); }
    void SurfaceWaterEndBrushScrub(std::uint32_t brushType, std::uint32_t arg) override { if (cSTESurfaceWater* water = WaterOf(Component(CityComponent::SurfaceWater))) water->EndBrushScrub(brushType, arg); }
    void PostBrushSound(std::uint32_t type, std::uint32_t data1, std::uint32_t data2) override {}
    std::uint32_t FloraBrushCount() override { return {}; }
    void FloraBrushList(std::vector<std::uint32_t>& ids) override {}
    bool FloraOccupantIsBrushable(std::uint32_t id) override { return {}; }
    void AddFloraAt(float x, float y, float z) override {}
    bool CityEstablished() override { sc4::cSC4City* city = native::HostCity(); return city && city->GetEstablished(); }
    void AddFloraAura(float x, float z) override {}
    bool AddOverlayDecal(cIGZUnknown* overlay, const cGZPersistResourceKey& key, const TerrainPoint& position, float strength, float randomAmount) override { return {}; }
    void ListBrushResources(std::vector<cGZPersistResourceKey>& out) override { ListResources(out, 0x6534284a, 0x6a01fc2a); }
    ITerrainEffect* CreateTerrainEffect(std::uint32_t id0, std::uint32_t id1) override { return {}; }
    cRZString NewCityName() override { return {}; }
    ITerrainHeightImage* NewCityHeightImage() override { return {}; }
    cIGZPersistDBSegment* NewCityObliteratedTerrainSegment() override { return {}; }
    bool NewCityBaseAltitude(float& out) override { return {}; }
    ITerrainHeightImage* LoadHeightImage(const cRZString& path) override { return {}; }
    std::mt19937 rng_{12345};
    cISC4RegionalCity* RegionalCity() override { return {}; }
    bool CityLoadInProgress() override { return {}; }
    cIGZMessageTarget2* TerrainMessageTarget() override { return {}; }
    cISC4Region* Region() override { return {}; }
    void RegionalCityPosition(const cISC4RegionalCity& city, std::int32_t& x, std::int32_t& z) override {}
    void RegionalCitySize(const cISC4RegionalCity& city, std::int32_t& width, std::int32_t& height) override {}
    cIGZPersistDBSegment* OpenSaveGame(const cIGZString& path, bool readOnly, bool forReading) override { return {}; }
    void CloseSaveGame(cIGZPersistDBSegment* segment) override {}
    bool PopupDialogsEnabled() override { return {}; }
    bool ShowEdgeReconcileDialog(std::int32_t kind) override { return {}; }
    ITerrainView* AdoptLoadedTerrainView(void* loadedObject) override { return {}; }
};
}  // namespace

void InstallTerrainStub() { static NativeTerrainHost s; static NativeSurfaceWaterHost water; static NativeMySimAgentHost agent; sc4::SetTerrainHost(&s); gzcom::SetSurfaceWaterHost(&water); gzcom::SetMySimAgentHost(&agent); }
void SetTerrainHostCity(cISC4City* city) { g_city = city; }

}  // namespace native
