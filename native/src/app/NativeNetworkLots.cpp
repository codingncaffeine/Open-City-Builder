// The network lot manager's city services (sc4::INetworkLotManagerHost) and the network pieces' messages
// (sc4::INetworkOccupantHost). A network lot is the street dressing of one road, rail or highway piece: the props and
// trees its configuration places, the piece's under-texture and variation by the zone and wealth around it, and the
// pothole effect of a damaged road.
//
// In the original the manager calls the objects' own tables (cISC4NetworkOccupant, cISC4Lot, cISC4PropOccupant,
// cISC4PropOccupantVisualEffect); the port's objects do not answer those ids (other seams read them), so this host hands
// out views. Piece views are stateless pass-throughs kept per piece address for the process (as the traffic map's), and
// share the piece's reference count; an effect view lives as long as its effect.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "app/NativeFlora.h"
#include "app/NativeNetworkLots.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"
#include "sc4/Exemplar.h"
#include "sc4/FloraSimulator.h"
#include "sc4/Lot.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/NetworkLotManager.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::ITrafficAutomatonOccupant* NativeTrafficPiece(sc4::cISC4Occupant* occupant);   // NativeTrafficNetworkMap.cpp (one reference)

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_NETLOTDEBUG") != nullptr;
    return on;
}

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
template <class T> T* Plain(CityComponent c) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}
cSC4PropManager* Props() { return Plain<cSC4PropManager>(CityComponent::PropManager); }
cSC4LotConfigurationManager* LotConfigurations() { return Plain<cSC4LotConfigurationManager>(CityComponent::LotConfigurationManager); }
cSC4ZoneManager* Zones() {
    cIGZUnknown* object = Component(CityComponent::ZoneManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    cSC4ZoneManager* zones = serializable->GetGZCLSID() == kZoneManagerClassId ? dynamic_cast<cSC4ZoneManager*>(serializable) : nullptr;
    serializable->Release();
    return zones;
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();   // the city keeps the manager
    return manager;
}
cSTETerrain* Terrain() {
    cIGZUnknown* object = Component(CityComponent::Terrain);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<cSTETerrain*>(serializable);
}
cSC4TrafficNetworkMap* NetworkMap() {
    cIGZUnknown* object = Component(CityComponent::TrafficNetwork);
    return object ? dynamic_cast<cSC4TrafficNetworkMap*>(object) : nullptr;
}

// ---- views -----------------------------------------------------------------------------------------------------------

// cISC4Occupant's two position slots (0x40 GetPosition / 0x48 SetPosition) over a real occupant.
class OccupantView final : public INetworkLotOccupantObject {
public:
    explicit OccupantView(cISC4Occupant* occupant) : occupant_(occupant) {}
    bool QueryInterface(std::uint32_t iid, void** out) override { return occupant_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return occupant_->AddRef(); }
    std::uint32_t Release() override { return occupant_->Release(); }
    void GetPosition(float xyz[3]) override { occupant_->GetPosition(xyz); }
    void SetPosition(const float xyz[3]) override { occupant_->SetPosition(xyz); }
    cISC4Occupant* Occupant() const { return occupant_; }

private:
    cISC4Occupant* occupant_;
};

// cISC4NetworkOccupant as the manager calls it (the slot comments are in NetworkLotManager.h).
class PieceView final : public INetworkLotNetworkOccupant {
public:
    explicit PieceView(cSC4NetworkOccupant* piece) : piece_(piece), occupant_(piece) {}
    bool QueryInterface(std::uint32_t iid, void** out) override { return piece_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return piece_->AddRef(); }
    std::uint32_t Release() override { return piece_->Release(); }
    bool HasNetworkFlag(std::uint32_t flags) override { return piece_->HasNetworkFlag(flags); }
    bool HasAnyNetworkFlag(std::uint32_t flags) override { return piece_->HasAnyNetworkFlag(flags); }
    void SetNetworkFlag(std::uint32_t flags) override { piece_->SetNetworkFlag(flags); }
    std::uint32_t GetNetworkFlag() override { return piece_->GetNetworkFlag(); }
    bool IsUsable() override { return piece_->IsUsable(); }
    std::uint32_t PieceId() override { return piece_->PieceId(); }
    std::uint8_t GetRotation() override { return piece_->GetRotation(); }
    bool GetFlip() override { return piece_->GetFlip() != 0; }
    void SetVariation(std::uint8_t variation) override { piece_->SetVariation(variation); }
    void SetUnderTexture(std::uint32_t texture) override { piece_->SetUnderTexture(texture); }
    std::uint32_t GetEdgeTypesFlag(std::uint32_t networkType) override {
        return piece_->GetEdgeTypesFlag(static_cast<std::uint8_t>(networkType));
    }
    std::uint32_t GetActiveEdges() override { return piece_->GetActiveEdges(); }
    INetworkLotOccupantObject* AsOccupant() override { return &occupant_; }
    bool IsIntersection() override { return piece_->IsIntersection(); }
    float GetAltitude(float x, float z) override { return piece_->GetAltitude(x, z); }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) override { piece_->GetOccupiedCell(x, z); }
    cSC4NetworkOccupant* Piece() const { return piece_; }

private:
    cSC4NetworkOccupant* piece_;
    OccupantView occupant_;
};
std::map<cSC4NetworkOccupant*, std::unique_ptr<PieceView>> g_pieces;

// The piece view with one reference, or null for an object that is not a network piece.
PieceView* PieceOf(cISC4Occupant* occupant) {
    auto* piece = occupant ? dynamic_cast<cSC4NetworkOccupant*>(occupant) : nullptr;
    if (!piece) return nullptr;
    auto& slot = g_pieces[piece];
    if (!slot) slot = std::make_unique<PieceView>(piece);
    slot->AddRef();
    return slot.get();
}
PieceView* PieceOf(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return nullptr;
    auto* occupant = static_cast<cISC4Occupant*>(raw);
    PieceView* view = PieceOf(occupant);
    occupant->Release();
    return view;
}

// cISC4Lot's four slots over a lot. A borrowed view (LotAt) is rebound per call; a message's view owns a lot reference
// and is deleted with its last release.
class LotView final : public INetworkLot {
public:
    LotView(cSC4Lot* lot, bool owned) : lot_(lot), owned_(owned) { if (owned_) lot_->AddRef(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return owned_ ? ++refs_ : 1; }
    std::uint32_t Release() override {
        if (!owned_) return 1;
        if (refs_ > 1) return --refs_;
        lot_->Release();
        delete this;
        return 0;
    }
    void GetBoundingRect(std::int32_t rect[4]) override { lot_->GetBoundingRect(rect); }
    bool IsInLivableState() override { return lot_->IsInLivableState(); }
    std::uint32_t GetOccupantWealth() override { return lot_->GetOccupantWealth(); }
    std::uint32_t GetBuildingType(bool flag) override { return lot_->GetBuildingType(flag); }
    void Bind(cSC4Lot* lot) { lot_ = lot; }

private:
    cSC4Lot* lot_;
    bool owned_;
    std::uint32_t refs_ = 1;
};

// cISC4Occupant's lot tag (0xe0) and flag (0xe8) slots over a street prop or a tree. Borrowed, like the occupant (the
// occupant manager holds it); rebound per call.
class PropView final : public INetworkLotPropOccupant {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override { return occupant_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return occupant_->AddRef(); }
    std::uint32_t Release() override { return occupant_->Release(); }
    void SetLotTag(std::uint32_t tag) override { occupant_->SetLotTag(tag); }
    void SetFlag(std::uint32_t flag) override { occupant_->SetFlag(flag); }
    PropView* Bind(cISC4Occupant* occupant) { occupant_ = occupant; return this; }

private:
    cISC4Occupant* occupant_ = nullptr;
};

// cISC4PropOccupantVisualEffect (the pothole): shares the effect's count and is deleted with it.
class EffectView final : public INetworkLotVisualEffect {
public:
    explicit EffectView(cSC4PropOccupantVisualEffect* effect) : effect_(effect), occupant_(effect->AsOccupant()) {}
    bool QueryInterface(std::uint32_t iid, void** out) override { return effect_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return effect_->AddRef(); }
    std::uint32_t Release() override {
        const std::uint32_t left = effect_->Release();
        if (left == 0) delete this;
        return left;
    }
    INetworkLotOccupantObject* AsOccupant() override { return &occupant_; }
    void SetEffectType(const char* name) override { effect_->SetEffectType(name); }
    void StartEffect(bool instant) override { effect_->StartEffect(instant); }
    void StopEffect(bool instant) override { effect_->StopEffect(instant); }

private:
    cSC4PropOccupantVisualEffect* effect_;
    OccupantView occupant_;
};

// cSC4NetworkORFilter: a piece with any of the flags. Starts at count 0 like the original constructor.
class NetworkOrFilter final : public INetworkLotOccupantFilter {
public:
    explicit NetworkOrFilter(std::uint32_t flags) : flags_(flags) {}
    void AddRef() override { ++refs_; }
    void Release() override { if (refs_ <= 1) delete this; else --refs_; }
    std::uint32_t Flags() const { return flags_; }

private:
    std::uint32_t flags_;
    std::uint32_t refs_ = 0;
};

class TypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit TypeFilter(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_; }

private:
    std::uint32_t type_;
};

// spRM slot 0x90's enumerator: every key of the resource type, then Release.
class KeyEnumerator final : public INetworkLotResourceEnumerator {
public:
    explicit KeyEnumerator(std::uint32_t type) : type_(type) {}
    void Enumerate(void (*callback)(const cGZPersistResourceKey&, void*), void* context) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(type_);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        for (const cGZPersistResourceKey& key : keys) callback(key, context);
    }
    void Release() override { delete this; }

private:
    std::uint32_t type_;
};

class Host final : public INetworkLotManagerHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    int CityCellCountX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CityCellCountZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    void CityPositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
    }
    void CityCellCenterToPosition(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
    }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    int ZoneTypeAt(int x, int z) override { cSC4ZoneManager* zones = Zones(); return zones ? zones->GetZoneType(x, z) : 0; }
    INetworkLot* LotAt(int x, int z, bool exactCell) override {
        cSC4Lot* lot = NativeLotAtCell(x, z, exactCell);
        if (!lot) return nullptr;
        borrowedLot_.Bind(lot);
        return &borrowedLot_;
    }
    INetworkLotNetworkOccupant* FindNetworkOccupant(int x, int z, std::uint32_t flags, bool includeUnusable) override {
        cSC4TrafficNetworkMap* map = NetworkMap();
        ITrafficNetworkMapOccupant* found = map ? map->FindNetworkOccupant(x, z, flags, includeUnusable) : nullptr;
        if (!found) return nullptr;
        PieceView* view = PieceOf(found->AsOccupant());
        if (view) view->Release();   // borrowed, like the map's own answer (the map's tile holds the piece)
        return view;
    }
    void UpdateIntersection(INetworkLotNetworkOccupant* occupant) override {
        cSC4TrafficNetworkMap* map = NetworkMap();
        if (!map || !occupant) return;
        ITrafficAutomatonOccupant* piece = NativeTrafficPiece(static_cast<PieceView*>(occupant)->Piece());
        if (!piece) return;
        map->UpdateIntersection(piece);
        piece->Release();
    }
    INetworkLotNetworkOccupant* GetFirstNetworkOccupant(int x, int z, INetworkLotOccupantFilter* filter) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants || !filter) return nullptr;
        const std::uint32_t flags = static_cast<NetworkOrFilter*>(filter)->Flags();
        OccupantList found;
        const int a[2] = {x, z}, b[2] = {x, z};
        occupants->GetOccupantsByStandardCityCells(found, a, b, kOccupantTypeNetwork, 0xffffffffu);
        PieceView* first = nullptr;
        for (cISC4Occupant* occupant : found) {
            auto* piece = dynamic_cast<cSC4NetworkOccupant*>(occupant);
            if (!first && piece && piece->HasAnyNetworkFlag(flags)) first = PieceOf(occupant);
            occupant->Release();
        }
        return first;
    }
    void InsertOccupant(INetworkLotOccupantObject* occupant) override {
        cISC4OccupantManager* occupants = Occupants();
        if (occupants && occupant) occupants->InsertOccupant(static_cast<OccupantView*>(occupant)->Occupant(), 0);
    }
    void RemoveOccupant(INetworkLotOccupantObject* occupant, bool arg, std::uint32_t reason) override {
        cISC4OccupantManager* occupants = Occupants();
        if (occupants && occupant) occupants->RemoveOccupant(static_cast<OccupantView*>(occupant)->Occupant(), arg, reason);
    }
    INetworkLotOccupantFilter* CreateNetworkOrFilter(std::uint32_t flags) override { return new NetworkOrFilter(flags); }
    INetworkLotPropOccupant* AddStreetProp(std::uint32_t type, const float* position, int orientation) override {
        cSC4PropManager* props = Props();
        cSC4PropOccupant* prop = props ? props->AddStreetProp(type, position, orientation) : nullptr;
        if (Trace()) {
            std::fprintf(stderr, "network lots: street prop %08x at %.1f %.1f %.1f -> %s\n", type, position[0], position[1],
                         position[2], prop ? "placed" : "none");
        }
        if (prop) ++propsPlaced_;
        return prop ? propView_.Bind(prop->AsOccupant()) : nullptr;
    }
    void RemoveStreetPropsFromTile(int x, int z) override { if (cSC4PropManager* props = Props()) props->RemoveStreetPropsFromTile(x, z); }
    INetworkLotPropOccupant* AddFlora(std::uint32_t type, const float* position, bool arg) override {
        cSC4FloraSimulator* flora = CityFloraSimulator();
        if (!flora) return nullptr;
        float xyz[3] = {position[0], position[1], position[2]};
        cISC4Occupant* plant = flora->AddNewFloraOccupant(type, xyz, arg);   // vslot 0x48 (borrowed)
        if (Trace()) std::fprintf(stderr, "network lots: flora %08x at %.1f %.1f -> %s\n", type, xyz[0], xyz[2], plant ? "planted" : "none");
        if (plant) ++floraPlanted_;
        return plant ? floraView_.Bind(plant) : nullptr;
    }
    void DemolishFloraInCellArea(const std::int32_t rect[4], int arg) override {
        if (cSC4FloraSimulator* flora = CityFloraSimulator()) flora->DemolishFloraInCellArea(rect, arg);
    }
    void GetObjectIDArray(const cSC4LotConfigurationObject* object, std::vector<std::uint32_t>& out) override {
        if (cSC4LotConfigurationManager* manager = LotConfigurations()) manager->GetObjectIDArray(object, out);
    }
    float GetAreaSlope(const std::int32_t rect[4], bool includeWater) override { return NativeAreaSlope(rect, includeWater); }
    std::uint32_t GetBuildingWealth(std::uint32_t buildingType) override {
        const BuildingProfile* profile = NativeBuildingProfile(buildingType);
        return profile ? profile->wealth : 0;
    }
    // GZCOM()->CreateInstance(0x2977aa49, 0x09820792): one reference, like every GZCOM creation.
    INetworkLotVisualEffect* CreateVisualEffect() override {
        auto* effect = new cSC4PropOccupantVisualEffect();
        effect->AddRef();
        return new EffectView(effect);
    }
    bool HasResourceManager() override { return SL::spRM != nullptr; }
    INetworkLotResourceEnumerator* GetExemplarEnumerator(std::uint32_t type) override { return new KeyEnumerator(type); }
    sc4::cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key, std::uint32_t exemplarType) override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, exemplarType, &resource, 0, nullptr) || !resource) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(resource);
        sc4::cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        exemplars_[holder] = exemplar;
        return holder;
    }
    void ReleaseExemplar(sc4::cISCPropertyHolder* holder) override {
        auto it = exemplars_.find(holder);
        if (it == exemplars_.end()) return;
        it->second->Release();
        exemplars_.erase(it);
    }
    // A version-1 configuration is written back into the game's data by the original (spRM 0xf8 + 0x68). The port keeps
    // the player's data read-only: the configuration is used as read and not stored back.
    INetworkLotDBSegment* GetConfigDBSegment(const cGZPersistResourceKey&) override { return nullptr; }
    bool StoreExemplar(sc4::cISCPropertyHolder*, const cGZPersistResourceKey&, INetworkLotDBSegment*) override { return false; }
    bool WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) override { return SC4WriteGZSerializable(stream, object); }
    // The lots' pieces (0xa821ef94) and the potholes (0x09820792) come back as this host's views.
    bool ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override {
        *out = nullptr;
        if (iid == kGZIID_cISC4NetworkOccupant) {
            void* raw = nullptr;
            const bool ok = SC4ReadGZSerializable(stream, kGZIID_cISC4Occupant, &raw, false);
            auto* occupant = static_cast<cISC4Occupant*>(raw);
            if (PieceView* view = PieceOf(occupant)) *out = static_cast<cIGZUnknown*>(static_cast<INetworkLotNetworkOccupant*>(view));
            if (occupant) occupant->Release();
            return ok;
        }
        if (iid == kGZIID_cISC4PropOccupantVisualEffect) {
            void* raw = nullptr;
            const bool ok = SC4ReadGZSerializable(stream, kGZIID_cISC4PropOccupantVisualEffect, &raw, false);
            auto* effect = raw ? dynamic_cast<cSC4PropOccupantVisualEffect*>(static_cast<cISC4PropOccupantVisualEffect*>(raw)) : nullptr;
            if (effect) *out = static_cast<INetworkLotVisualEffect*>(new EffectView(effect));   // takes the read's reference
            else if (raw) static_cast<cISC4PropOccupantVisualEffect*>(raw)->Release();
            return ok;
        }
        return SC4ReadGZSerializable(stream, iid, out, false);
    }
    INetworkLotNetworkOccupant* NetworkOccupantOf(cIGZUnknown* object) override { return PieceOf(object); }
    // The lot-state message carries a plain lot (app/NativeZoneDeveloperHost.cpp PostLotMessage).
    INetworkLot* LotOf(cIGZUnknown* object) override {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
        return new LotView(static_cast<cSC4Lot*>(raw), true);
    }

    int propsPlaced_ = 0, floraPlanted_ = 0;

private:
    LotView borrowedLot_{nullptr, false};
    PropView propView_, floraView_;
    std::unordered_map<sc4::cISCPropertyHolder*, cISCResExemplar*> exemplars_;
};
Host g_host;

// cSC4NetworkOccupant's messages, as the original builds them (Mac i386 SetNetworkFlag 0x00133690, SetUnderTexture
// 0x0013392c, SetLit 0x00133298): posted to SL::spMS2. Like cSC4Occupant::PostOccupantMessage they go through the DB
// segment's server, which the city view binds after the load (the loaded pieces are already in their final state).
class NetworkOccupantHost final : public INetworkOccupantHost {
public:
    void PostMessage(std::uint32_t message, cISC4Occupant* occupant, std::uint32_t value) override {
        cIGZMessageServer2* server = DBSegmentMessageServer();
        if (!server || !occupant) return;
        auto* m = new cRZMessage2Standard();
        m->AddRef();
        m->SetType(message);
        m->SetIGZUnknown(occupant);
        if (message == kMessageOccupantChanged) {   // the piece's look changed (variation, under-texture)
            m->SetVoid1(occupant);
            m->SetData2(1);
            m->SetData3(kOccupantTypeNetwork);
        } else if (message == kMessageNetworkUsableChanged) {
            m->SetData1(value);
        }
        server->MessagePost(m, false);
        m->Release();
    }
    void RegisterWithNetworkManager(cISC4Occupant*) override {}
    bool IntersectionPieceId(std::uint32_t pieceId) override { return cSC4NetworkOccupant::IsAvenueIntersectionPieceId(pieceId); }
    void ReadPersistRef(cIGZIStream& stream, std::uint32_t clsid, cIGZUnknown*& out) override {
        void* raw = nullptr;
        SC4ReadGZPersistRef(stream, clsid, &raw);
        out = static_cast<cIGZUnknown*>(raw);
    }
    void WritePersistRef(cIGZOStream& stream, cIGZUnknown* object) override { SC4WriteGZPersistRef(stream, object); }
};
NetworkOccupantHost g_occupantHost;

}  // namespace

void InstallNetworkLotHosts() {
    sc4::SetNetworkLotManagerHost(&g_host);
    sc4::SetNetworkOccupantHost(&g_occupantHost);
}

void NativeNetworkLotReport(int x, int z, int radius) {
    auto* manager = dynamic_cast<cSC4NetworkLotManager*>(Component(CityComponent::NetworkLotManager));
    if (!manager) {
        std::printf("netlots: no manager\n");
        return;
    }
    int lots = 0;
    manager->IterateAllNetworkLots([](std::uint32_t, std::uint32_t, cSC4NetworkLot*, void* count) {
        ++*static_cast<int*>(count);
        return true;
    }, &lots);
    std::set<cSC4NetworkLotConfiguration*> configurations;
    manager->GetNetworkLotConfigurationSet(configurations);
    const std::size_t configurationCount = configurations.size();
    for (cSC4NetworkLotConfiguration* configuration : configurations) configuration->Release();
    std::printf("netlots: %d lots, %zu configurations, %d street props and %d trees placed this run\n", lots, configurationCount,
                g_host.propsPlaced_, g_host.floraPlanted_);
    {   // the traffic map's tiles against the network pieces the occupant manager holds (the map is where pieces are found)
        int mapCells = 0, pieceCells = 0;
        cSC4TrafficNetworkMap* map = NetworkMap();
        cISC4OccupantManager* occupants = Occupants();
        for (int cz = 0; cz < g_host.CityCellCountZ(); ++cz) {
            for (int cx = 0; cx < g_host.CityCellCountX(); ++cx) {
                if (map && map->FindNetworkOccupant(cx, cz)) ++mapCells;
                if (!occupants) continue;
                OccupantList found;
                const int a[2] = {cx, cz};
                occupants->GetOccupantsByStandardCityCells(found, a, a, kOccupantTypeNetwork, 0xffffffffu);
                if (!found.empty()) ++pieceCells;
                for (cISC4Occupant* occupant : found) occupant->Release();
            }
        }
        std::printf("netlots: traffic map %s, %d cells with a tile; %d cells with a network piece\n", map ? "present" : "missing",
                    mapCells, pieceCells);
    }
    cSC4PropManager* props = Props();
    int x0 = x - radius, z0 = z - radius, x1 = x + radius, z1 = z + radius;
    if (radius < 0) {   // the whole city
        x0 = z0 = 0;
        x1 = g_host.CityCellCountX() - 1;
        z1 = g_host.CityCellCountZ() - 1;
    }
    for (int cz = z0; cz <= z1; ++cz) {
        for (int cx = x0; cx <= x1; ++cx) {
            INetworkLotNetworkOccupant* found = g_host.FindNetworkOccupant(cx, cz, kNetworkLotOccupantFlagMask, true);
            if (!found) continue;
            cSC4NetworkOccupant* piece = static_cast<PieceView*>(found)->Piece();
            cSC4NetworkLot* lot = manager->GetNetworkLot(cx, cz);
            const cSC4NetworkLotConfiguration* configuration = lot ? lot->GetConfiguration() : nullptr;
            OccupantList streetProps;
            if (props) props->GetStreetProps(streetProps, cx, cz);
            std::printf("netlots %d,%d: piece %08x flags %08x usable %d under %08x variation %u zone %u wealth %u lot %s %08x props %zu\n",
                        cx, cz, piece->PieceId(), piece->GetNetworkFlag(), piece->IsUsable() ? 1 : 0, piece->UnderTexture(),
                        piece->GetVariation(), manager->GetZoneTypeForNetwork(cx, cz), manager->GetWealthForNetwork(cx, cz),
                        lot ? "yes" : "no", configuration ? configuration->GetID() : 0u, streetProps.size());
            for (cISC4Occupant* occupant : streetProps) occupant->Release();
        }
    }
}

// Kit check against the original's own decisions: a city the original saved holds every piece's under-texture and
// variation as its UpdateRoadStyle left them, and every network lot with the configuration it chose. Each piece is
// restyled here (the manager's UpdateRoadStyle) and compared with what was loaded; each lot's configuration is checked
// against IsLotCompatible with the zone and wealth computed here.
void NativeNetworkLotVerify() {
    auto* manager = dynamic_cast<cSC4NetworkLotManager*>(Component(CityComponent::NetworkLotManager));
    if (!manager) {
        std::printf("netlotsverify: no manager\n");
        return;
    }
    int pieces = 0, styleSame = 0, styleDiff = 0, lots = 0, lotsCompatible = 0, lotsIncompatible = 0, printed = 0;
    const int cellsX = g_host.CityCellCountX(), cellsZ = g_host.CityCellCountZ();
    for (int z = 0; z < cellsZ; ++z) {
        for (int x = 0; x < cellsX; ++x) {
            INetworkLotNetworkOccupant* found = g_host.FindNetworkOccupant(x, z, kNetworkLotOccupantFlagMask, true);
            if (!found || found->HasAnyNetworkFlag(kNetworkLotBusyFlagMask)) continue;
            if (found->HasAnyNetworkFlag(kNetworkLotNoStyleFlagMask) || !found->IsUsable()) continue;
            if (found->HasNetworkFlag(kNetworkLotAvenueFlag)) continue;   // the avenue pass styles whole runs
            cSC4NetworkOccupant* piece = static_cast<PieceView*>(found)->Piece();
            const std::uint32_t under = piece->UnderTexture();
            const std::uint8_t variation = piece->GetVariation();
            manager->UpdateRoadStyle(x, z, found);
            ++pieces;
            if (piece->UnderTexture() == under && piece->GetVariation() == variation) {
                ++styleSame;
            } else {
                ++styleDiff;
                if (printed++ < 12) {
                    std::printf("netlotsverify %d,%d: piece %08x saved under %08x var %u, now under %08x var %u (zone %u wealth %u, cell zone %d)\n",
                                x, z, piece->PieceId(), under, variation, piece->UnderTexture(), piece->GetVariation(),
                                manager->GetZoneTypeForNetwork(x, z), manager->GetWealthForNetwork(x, z), g_host.ZoneTypeAt(x, z));
                }
            }
            if (cSC4NetworkLot* lot = manager->GetNetworkLot(x, z)) {
                cSC4NetworkLotConfiguration* configuration = lot->GetConfiguration();
                if (!configuration || lot->UsesNextPieceConfiguration()) continue;
                ++lots;
                if (manager->IsLotCompatible(configuration, x, z, manager->GetZoneTypeForNetwork(x, z),
                                             manager->GetWealthForNetwork(x, z), found->GetRotation(), found->GetFlip())) {
                    ++lotsCompatible;
                } else {
                    ++lotsIncompatible;
                }
            }
        }
    }
    std::printf("netlotsverify: %d pieces restyled: %d as saved, %d different; %d lots: %d compatible, %d not\n", pieces,
                styleSame, styleDiff, lots, lotsCompatible, lotsIncompatible);
}


// Kit: the pothole message's handler on a cell (damaged 1 = the effect starts, 0 = it stops).
void NativeNetworkLotPothole(int x, int z, bool damaged) {
    auto* manager = dynamic_cast<cSC4NetworkLotManager*>(Component(CityComponent::NetworkLotManager));
    if (!manager) return;
    manager->DoRoadDamageChange(x, z, damaged);
    int effects = 0;
    std::string names;
    if (cISC4OccupantManager* occupants = Occupants()) {   // the effect props now on the cell
        OccupantList found;
        const int a[2] = {x, z};
        occupants->GetOccupantsByStandardCityCells(found, a, a, kOccupantTypeProp, 0xffffffffu);
        for (cISC4Occupant* occupant : found) {
            if (auto* effect = dynamic_cast<cSC4PropOccupantVisualEffect*>(occupant)) {
                ++effects;
                char line[96];
                std::snprintf(line, sizeof line, " %s(active %d, effect %s)", effect->EffectName().ToChar(),
                              effect->EffectActive() ? 1 : 0, effect->Effect() ? "made" : "none");
                names += line;
            }
            occupant->Release();
        }
    }
    std::printf("netlotpothole %d,%d %d: %d effect props on the cell%s\n", x, z, damaged ? 1 : 0, effects, names.c_str());
}

}  // namespace native
