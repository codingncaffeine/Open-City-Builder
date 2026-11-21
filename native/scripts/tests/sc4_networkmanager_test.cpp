// cSC4NetworkManager (39 functions) — the object, its remap tables, the thirteen tools, the removal queue,
// the retrofit/crew lists, the message handlers that are tractable and the partial entry points.
//
// The test supplies everything outside the object through INetworkManagerHost: the message server, the ini
// resource, the tool factory, the occupant manager queries, the pipe/subway statics, the serializable
// helpers and the 24-hour clock. The occupant and tool neighbours are small fakes; the remap tables are read
// from a real temporary ini file through the recovered cRZIniFile, exactly like the game does.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include <unistd.h>

#include "gz/cIGZMessage2.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/NetworkManager.h"
#include "sc4/NetworkTool.h"   // the eNetworkType enumerators

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace sc4;

// ------------------------------------------------------------------------------------------------------------
// the test's window into the private state (the original keeps these members private too)
// ------------------------------------------------------------------------------------------------------------
namespace sc4 {
struct NetworkManagerTestAccess {
    struct Offsets {
        std::size_t refCount, inited, ukPathMapping, extendRegion, regionMin, tracked, shadowByEdges,
            textureRemaps, shadowRemaps, pathRemaps, removalQueue, queueActive, retrofitList, crews, tools;
    };
    static Offsets Get(const cSC4NetworkManager& nm) {
        Offsets o{};
        o.refCount = Offset(nm, &nm.refCount_);
        o.inited = Offset(nm, &nm.inited_);
        o.ukPathMapping = Offset(nm, &nm.ukPathMapping_);
        o.extendRegion = Offset(nm, &nm.extendRegion_);
        o.regionMin = Offset(nm, &nm.regionMinX_);
        o.tracked = Offset(nm, &nm.trackedOccupants_);
        o.shadowByEdges = Offset(nm, &nm.shadowRemapByEdges_);
        o.textureRemaps = Offset(nm, &nm.textureRemaps_);
        o.shadowRemaps = Offset(nm, &nm.shadowTextureRemaps_);
        o.pathRemaps = Offset(nm, &nm.pathRemaps_);
        o.removalQueue = Offset(nm, &nm.removalQueue_);
        o.queueActive = Offset(nm, &nm.removalQueueActive_);
        o.retrofitList = Offset(nm, &nm.retrofitList_);
        o.crews = Offset(nm, &nm.constructionCrews_);
        o.tools = Offset(nm, &nm.networkTools_);
        return o;
    }
    static std::uint32_t RefCount(const cSC4NetworkManager& nm) { return nm.refCount_; }
    static std::size_t Tracked(const cSC4NetworkManager& nm) { return nm.trackedOccupants_.size(); }
    static std::size_t Retrofits(const cSC4NetworkManager& nm) { return nm.retrofitList_.size(); }
    static std::size_t Crews(const cSC4NetworkManager& nm) { return nm.constructionCrews_.size(); }
    static std::size_t Queued(const cSC4NetworkManager& nm) { return nm.removalQueue_.size(); }
    static bool QueueActive(const cSC4NetworkManager& nm) { return nm.removalQueueActive_; }
    static bool Inited(const cSC4NetworkManager& nm) { return nm.inited_; }
    static bool UkPathMapping(const cSC4NetworkManager& nm) { return nm.ukPathMapping_; }
    static bool ExtendRegion(const cSC4NetworkManager& nm) { return nm.extendRegion_; }
    static std::int32_t RegionMinX(const cSC4NetworkManager& nm) { return nm.regionMinX_; }
    static std::int32_t RegionMaxZ(const cSC4NetworkManager& nm) { return nm.regionMaxZ_; }
    static INetworkManagerTool* Tool(const cSC4NetworkManager& nm, int type) {
        return nm.networkTools_[type];
    }
    static std::size_t TextureRemaps(const cSC4NetworkManager& nm) { return nm.textureRemaps_.size(); }
    static std::size_t ShadowRemaps(const cSC4NetworkManager& nm) { return nm.shadowTextureRemaps_.size(); }
    static std::size_t PathRemaps(const cSC4NetworkManager& nm) { return nm.pathRemaps_.size(); }
    static std::size_t ShadowByEdges(const cSC4NetworkManager& nm) { return nm.shadowRemapByEdges_.size(); }

private:
    static std::size_t Offset(const cSC4NetworkManager& nm, const void* member) {
        return static_cast<std::size_t>(static_cast<const unsigned char*>(member) -
                                        reinterpret_cast<const unsigned char*>(&nm));
    }
};
}  // namespace sc4

namespace {

// ------------------------------------------------------------------------------------------------------------
// fake tools (cSC4NetworkTool / cSC4PipeTool / cSC4PowerLineTool / cSC4SubwayTool)
// ------------------------------------------------------------------------------------------------------------
struct ToolRecord {
    int type = 0;
    int refs = 0;                 // the original constructors leave a new tool at 0
    int inits = 0;
    int shutdowns = 0;
    int destroys = 0;
    bool destroyed = false;
    std::vector<INetworkManagerOccupant*> lastDestroyed;
};

class NmFakeTool final : public INetworkManagerTool {
public:
    NmFakeTool(std::shared_ptr<ToolRecord> record, int type) : record_(std::move(record)) { record_->type = type; }
    bool QueryInterface(std::uint32_t iid, void** out)  {
        if (iid != 1) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++record_->refs); }
    std::uint32_t Release() override {
        record_->refs--;
        if (record_->refs <= 0) {
            record_->destroyed = true;
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(record_->refs);
    }
    bool Init() override {
        ++record_->inits;
        return true;
    }
    bool Shutdown() override {
        ++record_->shutdowns;
        return true;
    }
    void DestroyOccupantsAndResolve(std::vector<INetworkManagerOccupant*>& occupants) override {
        ++record_->destroys;
        record_->lastDestroyed = occupants;
        // The real tool removes the occupants from the city and consumes the references it was handed.
        for (INetworkManagerOccupant* occupant : occupants) occupant->Release();
    }

private:
    std::shared_ptr<ToolRecord> record_;
};

// ------------------------------------------------------------------------------------------------------------
// fake occupant: cISC4Occupant plus the cISC4NetworkOccupant side (iid 0xa821ef94)
// ------------------------------------------------------------------------------------------------------------
class NmFakeOccupant final : public cISC4Occupant, public INetworkManagerOccupant {
public:
    int refs = 0;   // starts at 0 like the original occupants; the test keeps one reference
    std::uint32_t type = kNetworkOccupantType;
    std::uint32_t flags = 0;             // cISC4Occupant flags (IsFlagSet)
    std::uint32_t networkFlags = 0;      // +0x60 GetNetworkFlags
    bool managedGlobally = false;        // +0x28
    bool lighting = false;               // +0x1f0
    bool lit = false;                    // SetLit
    int litCalls = 0;
    int rotationUpdates = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> posted;

    // ---- cIGZUnknown (one counter for both sub-objects) ----
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4NetworkOccupant) {
            *out = static_cast<INetworkManagerOccupant*>(this);
        } else if (iid == kGZIID_cISC4Occupant || iid == 1) {
            *out = static_cast<cISC4Occupant*>(this);
        } else {
            return false;
        }
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (refs > 1) return static_cast<std::uint32_t>(--refs);
        refs = 0;
        delete this;
        return 0;
    }

    // ---- cISC4Occupant ----
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float*) override {}
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t message, std::uint32_t data) override {
        posted.emplace_back(message, data);
    }
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t flag) override { flags |= flag; }
    void SetAllFlags(std::uint32_t value) override { flags = value; }
    void ClearFlag(std::uint32_t flag) override { flags &= ~flag; }
    bool IsFlagSet(std::uint32_t flag) override { return (flags & flag) != 0; }
    std::uint32_t GetFlags() override { return flags; }

    // ---- cISC4NetworkOccupant ----
    bool IsManagedGlobally() const override { return managedGlobally; }
    bool HasNetworkFlag(std::uint32_t flag) override { return (networkFlags & flag) != 0; }
    std::uint32_t GetNetworkFlags() override { return networkFlags; }
    void SetLit(bool value) override {
        lit = value;
        ++litCalls;
    }
    bool SupportsLighting() override { return lighting; }
    void UpdateViewRotation() override { ++rotationUpdates; }
};

// ------------------------------------------------------------------------------------------------------------
// fake message server
// ------------------------------------------------------------------------------------------------------------
class NmFakeMessageServer final : public cIGZMessageServer2 {
public:
    std::vector<std::pair<int, std::uint32_t>> registrations;   // +1 add, -1 remove
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        return true;
    }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        registrations.emplace_back(1, messageType);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t messageType) override {
        registrations.emplace_back(-1, messageType);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// ------------------------------------------------------------------------------------------------------------
// fake occupant manager (the three queries the manager uses)
// ------------------------------------------------------------------------------------------------------------
class NmFakeOccupantManager final : public INetworkManagerOccupantManager {
public:
    std::vector<cISC4Occupant*> listed;       // GetOccupantsByBBox result
    std::vector<cISC4Occupant*> iterated;     // IterateOccupants input
    int bboxCalls = 0;
    int releaseListCalls = 0;
    int iterateCalls = 0;
    std::uint32_t lastType = 0;

    void GetOccupantsByBBox(ilist<cISC4Occupant*>& out, const float*, const float*, std::uint32_t type,
                            std::uint32_t) override {
        ++bboxCalls;
        lastType = type;
        for (cISC4Occupant* occupant : listed) {
            occupant->AddRef();
            out.push_back(occupant);
        }
    }
    void ReleaseOccupantList(ilist<cISC4Occupant*>& list) override {
        ++releaseListCalls;
        for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
    void IterateOccupants(bool (*callback)(cISC4Occupant*, void*), void* context, std::uint32_t type) override {
        ++iterateCalls;
        lastType = type;
        for (cISC4Occupant* occupant : iterated) {
            if (!callback(occupant, context)) break;
        }
    }
};

// ------------------------------------------------------------------------------------------------------------
// fake construction crew (cSC4NetworkConstructionCrew)
// ------------------------------------------------------------------------------------------------------------
class NmFakeCrew final : public INetworkManagerCrew {
public:
    int refs = 1;                       // the test keeps one reference
    int shutdowns = 0;
    cSC4NetworkManager* manager = nullptr;   // when set, Shutdown unregisters itself like the original

    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override {
        if (refs > 1) return static_cast<std::uint32_t>(--refs);
        refs = 0;
        delete this;
        return 0;
    }
    bool Shutdown() override {
        ++shutdowns;
        if (manager != nullptr) manager->UnregisterConstructionCrew(this);
        return true;
    }
};

// ------------------------------------------------------------------------------------------------------------
// fake host
// ------------------------------------------------------------------------------------------------------------
class NmTestHost final : public INetworkManagerHost {
public:
    NmFakeMessageServer messages;
    NmFakeOccupantManager occupants;
    bool presentMessages = true;
    bool languageFlag = false;
    bool night = false;
    std::string iniPath;

    std::vector<std::shared_ptr<ToolRecord>> tools;
    int filterWaterRequests = 0;
    int filterSubwayRequests = 0;
    bool subwayStationAnswer = false;
    int pipeSearches = 0;
    int pipeConnectionSearches = 0;
    int serializedWrites = 0;
    int serializedReads = 0;
    std::vector<cISC4Occupant*> readQueue;   // handed out one per ReadGZSerializable
    std::vector<const cIGZUnknown*> written;
    std::uint8_t waterFilterMarker = 0;
    std::uint8_t subwayFilterMarker = 0;
    std::uint8_t pipeMarker = 0;
    std::uint8_t pipeConnectionMarker = 0;

    cIGZMessageServer2* MessageServer() override { return presentMessages ? &messages : nullptr; }
    bool LanguageFlag() override { return languageFlag; }
    cRZIniFile* OpenNetworkIni() override {
        if (iniPath.empty()) return nullptr;
        return new cRZIniFile(cRZString(iniPath.c_str()));
    }
    INetworkManagerTool* CreateNetworkTool(eNetworkType type) override {
        auto record = std::make_shared<ToolRecord>();
        tools.push_back(record);
        return new NmFakeTool(record, static_cast<int>(type));
    }
    cISC4OccupantFilter* CreateOccupantFilter(bool subway) override {
        if (subway) {
            ++filterSubwayRequests;
            return reinterpret_cast<cISC4OccupantFilter*>(&subwayFilterMarker);
        }
        ++filterWaterRequests;
        return reinterpret_cast<cISC4OccupantFilter*>(&waterFilterMarker);
    }
    bool IsOccupantSubwayStation(cISC4Occupant*) override { return subwayStationAnswer; }
    void FindPipeOccupant(int, int, cSC4PipeOccupant*& out) override {
        ++pipeSearches;
        out = reinterpret_cast<cSC4PipeOccupant*>(&pipeMarker);
    }
    void FindPipeConnectionOccupant(int, int, cSC4PipeConnectionOccupant*& out) override {
        ++pipeConnectionSearches;
        out = reinterpret_cast<cSC4PipeConnectionOccupant*>(&pipeConnectionMarker);
    }
    INetworkManagerOccupantManager* OccupantManager() override { return &occupants; }
    bool IsNight() override { return night; }
    bool WriteGZSerializable(cIGZOStream&, const cIGZUnknown* object) override {
        ++serializedWrites;
        written.push_back(object);
        return true;
    }
    bool ReadGZSerializable(cIGZIStream&, std::uint32_t iid, void** out) override {
        ++serializedReads;
        *out = nullptr;
        if (readQueue.empty() || iid != kGZIID_cISC4NetworkOccupant) return false;
        cISC4Occupant* occupant = readQueue.front();
        readQueue.erase(readQueue.begin());
        occupant->AddRef();   // the serializable helper hands back a counted reference
        *out = static_cast<void*>(static_cast<INetworkManagerOccupant*>(static_cast<NmFakeOccupant*>(occupant)));
        return true;
    }
    std::vector<INetworkManagerOccupant*> destroyedBy(int type) const {
        for (const auto& record : tools)
            if (record->type == type) return record->lastDestroyed;
        return {};
    }
    std::shared_ptr<ToolRecord> record(int type) const {
        for (const auto& record : tools)
            if (record->type == type) return record;
        return nullptr;
    }
    int alive() const {
        int count = 0;
        for (const auto& record : tools)
            if (!record->destroyed) ++count;
        return count;
    }
};

// ------------------------------------------------------------------------------------------------------------
// helpers
// ------------------------------------------------------------------------------------------------------------
std::string WriteIni() {
    const std::string path = "/tmp/nm_test_" + std::to_string(static_cast<long>(::getpid())) + ".ini";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return {};
    const char* text =
        "[Texture remapping]\n"
        "first=0x1a5,0xffff,0x10,0xff,1,0,0,0x0\n"
        "second=0xb5,0xffff,0x100,0xff,3,1,1,0xf\n"
        "junk=not,a,number\n"
        "\n"
        "[Shadow remapping]\n"
        "byedges=0x200,0xffff,0x0,0x0,0,0,0,0x0\n"
        "direct=0x10,0xffff,0x20,0xf,2,0,1,0x0\n"
        "\n"
        "[Shadow edge remapping]\n"
        "highway=Highway,0x200,0x10,0xff,1,0\n"
        "road=Road,0x300,0x1,0x1,0,0\n"
        "unknown=Freeway,0x400,0x1,0x1,0,0\n"
        "\n"
        "[UK path remapping]\n"
        "uk=0x10,0xf0,0x1000,0xf\n"
        "bad=0x1\n";
    std::fwrite(text, 1, std::strlen(text), file);
    std::fclose(file);
    return path;
}

// COM objects start at count 0; the test keeps one reference on everything it allocates.
NmFakeOccupant* NewOccupant() {
    auto* occupant = new NmFakeOccupant();
    occupant->AddRef();
    return occupant;
}

cRZMessage2Standard* MakeMessage(std::uint32_t type, void* payload1 = nullptr, void* payload2 = nullptr) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    if (payload1 != nullptr) message->SetVoid1(payload1);
    if (payload2 != nullptr) message->SetVoid2(payload2);
    return message;
}

}  // namespace

int main() {
    const std::string iniPath = WriteIni();
    CHECK(!iniPath.empty());

    // ---- object layout: the original's 0x170 bytes and member offsets --------------------------------
    {
        NmTestHost host;
        host.iniPath = iniPath;
        SetNetworkManagerHost(&host);
        auto* manager = new cSC4NetworkManager();
        CHECK(sizeof(cSC4NetworkManager) == 0x170);
        const auto offsets = NetworkManagerTestAccess::Get(*manager);
        CHECK(offsets.refCount == 0x18);
        CHECK(offsets.inited == 0x1c);
        CHECK(offsets.ukPathMapping == 0x1d);
        CHECK(offsets.extendRegion == 0x1e);
        CHECK(offsets.regionMin == 0x20);
        CHECK(offsets.tracked == 0x30);
        CHECK(offsets.shadowByEdges == 0x48);
        CHECK(offsets.textureRemaps == 0x70);
        CHECK(offsets.shadowRemaps == 0x88);
        CHECK(offsets.pathRemaps == 0xa0);
        CHECK(offsets.removalQueue == 0xb8);
        CHECK(offsets.queueActive == 0xd0);
        CHECK(offsets.retrofitList == 0xd8);
        CHECK(offsets.crews == 0xf0);
        CHECK(offsets.tools == 0x108);

        // ---- constructor / COM identity ------------------------------------------------------------
        CHECK(NetworkManagerTestAccess::RefCount(*manager) == 0);   // the original starts at 0
        void* raw = nullptr;
        CHECK(manager->QueryInterface(1, &raw) && raw == manager);
        CHECK(NetworkManagerTestAccess::RefCount(*manager) == 1);
        CHECK(manager->QueryInterface(kGZIID_cISC4NetworkManager, &raw) && raw == manager);
        CHECK(manager->QueryInterface(kGZIID_cIGZSerializable, &raw) && raw != manager);
        CHECK(manager->QueryInterface(kGZIID_cIGZMessageTarget2, &raw) && raw != manager);
        CHECK(NetworkManagerTestAccess::RefCount(*manager) == 4);
        CHECK(!manager->QueryInterface(0xdeadbeef, &raw));
        CHECK(NetworkManagerTestAccess::RefCount(*manager) == 4);
        CHECK(manager->Release() == 3);
        CHECK(manager->Release() == 2);
        CHECK(manager->Release() == 1);
        CHECK(manager->Release() == 0);   // destroys the object

        // ---- Reload: the ini tables ---------------------------------------------------------------
        auto* reloaded = new cSC4NetworkManager();
        CHECK(NetworkManagerTestAccess::TextureRemaps(*reloaded) == 2);   // "junk" is dropped
        CHECK(NetworkManagerTestAccess::ShadowRemaps(*reloaded) == 2);
        CHECK(NetworkManagerTestAccess::ShadowByEdges(*reloaded) == 2);   // "Freeway" is dropped
        CHECK(NetworkManagerTestAccess::PathRemaps(*reloaded) == 1);      // "bad" has too few fields

        // Reload twice: the tables are cleared first.
        reloaded->Reload();
        CHECK(NetworkManagerTestAccess::TextureRemaps(*reloaded) == 2);
        CHECK(NetworkManagerTestAccess::ShadowByEdges(*reloaded) == 2);

        // ---- MapPreviewTexture -------------------------------------------------------------------
        std::uint8_t alpha = 0x20;
        const std::uint32_t mapped = reloaded->MapPreviewTexture(0x1a5, alpha);
        CHECK(mapped == 0x1b0);
        CHECK(alpha == 0x82);   // the delta is subtracted through (delta+8), then masked with 0x83

        // no entry matches: value and alpha come back untouched
        alpha = 0x20;
        CHECK(reloaded->MapPreviewTexture(0x1a4, alpha) == 0x1a4);
        CHECK(alpha == 0x20);

        // ---- MapShadowTexture: by-edges entry first, direct entry second ---------------------------
        alpha = 0x20;
        CHECK(reloaded->MapShadowTexture(0x200, alpha, 0x004, 0x200) == 0x20);
        CHECK(alpha == 3);
        // Network 0 does not match the Highway flag 0x4, so the by-edges entry is skipped and the
        // direct entry never matches: unchanged.
        alpha = 0x20;
        CHECK(reloaded->MapShadowTexture(0x200, alpha, 0x000, 0x200) == 0x200);
        CHECK(alpha == 0x20);

        // ---- GetEnglishNetworkName ----------------------------------------------------------------
        CHECK(std::strcmp(reloaded->GetEnglishNetworkName(kNetworkRoad), "Road") == 0);
        CHECK(std::strcmp(reloaded->GetEnglishNetworkName(kNetworkPipe), "Pipe") == 0);
        CHECK(std::strcmp(reloaded->GetEnglishNetworkName(kNetworkPowerLine), "PowerLine") == 0);
        CHECK(std::strcmp(reloaded->GetEnglishNetworkName(kNetworkGroundHighway), "GroundHighway") == 0);

        // ---- MapPath before Init: the UK switch (+0x1d) is still off --------------------------------
        CHECK(!NetworkManagerTestAccess::UkPathMapping(*reloaded));
        CHECK(reloaded->MapPath(0x1f) == 0x1f);
        reloaded->Release();
    }

    // ---- Init / Shutdown ---------------------------------------------------------------------------
    {
        NmTestHost host;
        host.iniPath = iniPath;
        host.occupants.listed.clear();
        SetNetworkManagerHost(&host);
        auto* manager = new cSC4NetworkManager();
        CHECK(!NetworkManagerTestAccess::Inited(*manager));
        CHECK(manager->Init());
        CHECK(NetworkManagerTestAccess::Inited(*manager));
        CHECK(NetworkManagerTestAccess::UkPathMapping(*manager));   // the language flag is inverted
        CHECK(host.messages.registrations.size() == 11);
        CHECK(host.tools.size() == 13);
        CHECK(host.alive() == 13);
        for (int type = 0; type < static_cast<int>(kNetworkTypeCount); ++type) {
            CHECK(host.record(type) != nullptr);
            CHECK(host.record(type)->refs == 1);
            CHECK(NetworkManagerTestAccess::Tool(*manager, type) != nullptr);
        }
        // Second Init is a no-op.
        CHECK(manager->Init());
        CHECK(host.tools.size() == 13);

        // GetNetworkTool: the borrowed tool, a fresh one for create == true, null out of range.
        CHECK(manager->GetNetworkTool(kNetworkRoad, false) != nullptr);
        CHECK(manager->GetNetworkTool(kNetworkRoad, false) == manager->GetNetworkTool(kNetworkRoad, false));
        INetworkManagerTool* fresh = manager->GetNetworkTool(kNetworkSubway, true);
        CHECK(fresh != nullptr);
        CHECK(host.tools.size() == 14);
        CHECK(manager->GetNetworkTool(static_cast<eNetworkType>(13), false) == nullptr);
        CHECK(manager->GetNetworkTool(static_cast<eNetworkType>(13), true) == nullptr);
        fresh->AddRef();
        CHECK(fresh->Release() == 0);   // the fresh tool was not stored and is destroyed

        // UK path mapping after Init: the language flag was false -> the table is used.
        CHECK(manager->MapPath(0x1f) == 0x100f);
        CHECK(manager->MapPath(0x00) == 0x00);

        // Shutdown releases the thirteen tools and unregisters the eleven messages.
        CHECK(manager->Shutdown());
        CHECK(!NetworkManagerTestAccess::Inited(*manager));
        CHECK(host.alive() == 0);
        CHECK(host.messages.registrations.size() == 22);
        int removed = 0;
        for (const auto& entry : host.messages.registrations)
            if (entry.first == -1) ++removed;
        CHECK(removed == 11);
        CHECK(NetworkManagerTestAccess::Tool(*manager, 0) == nullptr);
        manager->Release();
    }

    // ---- Init with the language flag set and without a message server ------------------------------
    {
        NmTestHost host;
        host.iniPath = iniPath;
        host.languageFlag = true;
        SetNetworkManagerHost(&host);
        auto* manager = new cSC4NetworkManager();
        CHECK(manager->Init());
        CHECK(!NetworkManagerTestAccess::UkPathMapping(*manager));   // language flag 1 -> no UK mapping
        CHECK(manager->MapPath(0x1f) == 0x1f);
        manager->Release();

        NmTestHost noServer;
        noServer.iniPath = iniPath;
        noServer.presentMessages = false;
        SetNetworkManagerHost(&noServer);
        auto* bare = new cSC4NetworkManager();
        CHECK(bare->Init());
        CHECK(NetworkManagerTestAccess::Inited(*bare));
        CHECK(noServer.tools.empty());   // quirk: no message server -> no tools
        CHECK(bare->GetNetworkTool(kNetworkRoad, false) == nullptr);
        bare->Release();
    }

    // ---- DestroyNetworkOccupants / QueueRemovals ---------------------------------------------------
    {
        NmTestHost host;
        host.iniPath = iniPath;
        SetNetworkManagerHost(&host);
        auto* manager = new cSC4NetworkManager();
        manager->Init();

        auto* road = NewOccupant();
        road->networkFlags = 0x0001;
        auto* pipe = NewOccupant();
        pipe->networkFlags = 0x0010;
        auto* subway = NewOccupant();
        subway->networkFlags = 0x0080;
        auto* other = NewOccupant();
        other->networkFlags = 0x800000;   // not part of any of the three destroy classes

        ilist<cISC4Occupant*> occupants{road, pipe, subway, other};
        manager->DestroyNetworkOccupants(occupants);
        CHECK(host.record(0) != nullptr && host.record(0)->destroys == 1);
        CHECK(host.record(4) != nullptr && host.record(4)->destroys == 1);
        CHECK(host.record(7) != nullptr && host.record(7)->destroys == 1);
        CHECK(host.destroyedBy(0).size() == 1);
        CHECK(host.destroyedBy(0)[0] == static_cast<INetworkManagerOccupant*>(road));
        CHECK(host.destroyedBy(4)[0] == static_cast<INetworkManagerOccupant*>(pipe));
        CHECK(host.destroyedBy(7)[0] == static_cast<INetworkManagerOccupant*>(subway));
        CHECK(road->refs == 1);
        CHECK(pipe->refs == 1);
        CHECK(subway->refs == 1);
        CHECK(other->refs == 1);

        // Queue mode: every occupant is AddRef'd and queued; draining sorts, dedupes and hands the list to
        // the occupant manager, which releases the references.
        manager->QueueRemovals(true);
        CHECK(NetworkManagerTestAccess::QueueActive(*manager));
        manager->DestroyNetworkOccupants(occupants);
        manager->DestroyNetworkOccupants(occupants);   // queued twice on purpose: the duplicates get released
        CHECK(NetworkManagerTestAccess::Queued(*manager) == 8);
        CHECK(road->refs == 3);
        manager->QueueRemovals(true);   // already active: no change
        CHECK(NetworkManagerTestAccess::Queued(*manager) == 8);
        manager->QueueRemovals(false);
        CHECK(!NetworkManagerTestAccess::QueueActive(*manager));
        CHECK(NetworkManagerTestAccess::Queued(*manager) == 0);
        CHECK(host.occupants.releaseListCalls == 1);
        CHECK(road->refs == 1);
        CHECK(pipe->refs == 1);
        CHECK(subway->refs == 1);
        CHECK(other->refs == 1);
        CHECK(host.destroyedBy(0)[0] == static_cast<INetworkManagerOccupant*>(road));
        CHECK(host.destroyedBy(4)[0] == static_cast<INetworkManagerOccupant*>(pipe));
        CHECK(host.destroyedBy(7)[0] == static_cast<INetworkManagerOccupant*>(subway));

        // QueueRemovals(false) on an empty queue does nothing.
        manager->QueueRemovals(false);
        CHECK(host.occupants.releaseListCalls == 1);

        // ---- construction crews -------------------------------------------------------------------
        auto* crew = new NmFakeCrew();
        CHECK(NetworkManagerTestAccess::Crews(*manager) == 0);
        manager->RegisterConstructionCrew(crew);
        CHECK(crew->refs == 2);
        CHECK(NetworkManagerTestAccess::Crews(*manager) == 1);
        manager->RegisterConstructionCrew(crew);
        CHECK(crew->refs == 3);
        CHECK(NetworkManagerTestAccess::Crews(*manager) == 2);
        manager->UnregisterConstructionCrew(crew);
        CHECK(crew->refs == 2);
        CHECK(NetworkManagerTestAccess::Crews(*manager) == 1);
        NmFakeCrew unknown;
        manager->UnregisterConstructionCrew(&unknown);   // not registered: ignored
        CHECK(NetworkManagerTestAccess::Crews(*manager) == 1);

        // ---- DoMessage: crew reset (0x26d31ec2) releases the crews and the tools -------------------
        crew->manager = manager;   // Shutdown unregisters itself, as the original crew does
        auto* message = MakeMessage(0x26d31ec2);
        CHECK(manager->DoMessage(message));
        message->Release();
        CHECK(crew->shutdowns == 1);
        CHECK(NetworkManagerTestAccess::Crews(*manager) == 0);
        CHECK(crew->refs == 1);   // only the test's reference is left
        CHECK(host.alive() == 0);
        crew->Release();

        // ---- retrofit list ------------------------------------------------------------------------
        auto* retrofit = NewOccupant();
        manager->AddToRetrofitList(retrofit);
        CHECK(retrofit->refs == 2);
        CHECK(NetworkManagerTestAccess::Retrofits(*manager) == 1);
        manager->Shutdown();
        CHECK(retrofit->refs == 1);
        CHECK(NetworkManagerTestAccess::Retrofits(*manager) == 0);
        retrofit->Release();

        road->Release();
        pipe->Release();
        subway->Release();
        other->Release();
        manager->Release();
    }

    // ---- DoMessage: occupant tracking, lights, view rotation, bridge models -----------------------
    {
        NmTestHost host;
        host.iniPath = iniPath;
        SetNetworkManagerHost(&host);
        auto* manager = new cSC4NetworkManager();
        manager->Init();

        auto* tracked = NewOccupant();
        tracked->managedGlobally = true;
        tracked->lighting = true;
        auto* untracked = NewOccupant();
        untracked->managedGlobally = false;

        auto* insert = MakeMessage(0x99ef1142, static_cast<cISC4Occupant*>(tracked));
        CHECK(manager->DoMessage(insert));
        insert->Release();
        CHECK(NetworkManagerTestAccess::Tracked(*manager) == 1);
        CHECK(tracked->refs == 2);   // the tracked vector owns the QI reference

        insert = MakeMessage(0x99ef1142, static_cast<cISC4Occupant*>(untracked));
        CHECK(manager->DoMessage(insert));
        insert->Release();
        CHECK(NetworkManagerTestAccess::Tracked(*manager) == 1);
        CHECK(untracked->refs == 1);   // not managed globally: the QI reference was dropped

        // Removing the tracked occupant releases the vector's reference.
        auto* remove = MakeMessage(0x99ef1143, static_cast<cISC4Occupant*>(tracked));
        CHECK(manager->DoMessage(remove));
        remove->Release();
        CHECK(NetworkManagerTestAccess::Tracked(*manager) == 0);
        CHECK(tracked->refs == 1);

        // Re-track it for the light messages.
        insert = MakeMessage(0x99ef1142, static_cast<cISC4Occupant*>(tracked));
        manager->DoMessage(insert);
        insert->Release();
        CHECK(NetworkManagerTestAccess::Tracked(*manager) == 1);

        auto* lightsOff = MakeMessage(0xc9da96e8);
        CHECK(manager->DoMessage(lightsOff));
        lightsOff->Release();
        CHECK(tracked->litCalls == 1 && !tracked->lit);
        auto* lightsOn = MakeMessage(0xc9da96e9);
        CHECK(manager->DoMessage(lightsOn));
        lightsOn->Release();
        CHECK(tracked->litCalls == 2 && tracked->lit);

        // 0x26d31ec1: Retrofit (PARTIAL: a no-op) then, when the clock says night, set the lights.
        host.night = true;
        auto* nightMessage = MakeMessage(0x26d31ec1);
        CHECK(manager->DoMessage(nightMessage));
        nightMessage->Release();
        CHECK(tracked->litCalls == 3 && tracked->lit);

        // 0xea3b0af1 resets the region rectangle and arms the extend flag.
        auto* reset = MakeMessage(0xea3b0af1);
        CHECK(manager->DoMessage(reset));
        reset->Release();
        CHECK(NetworkManagerTestAccess::ExtendRegion(*manager));
        CHECK(NetworkManagerTestAccess::RegionMinX(*manager) == 0x7fffffff);
        CHECK(NetworkManagerTestAccess::RegionMaxZ(*manager) == -1);

        // 0xa6b79602: two rotation payloads; different values run DoViewRotationChange.
        host.occupants.listed = {static_cast<cISC4Occupant*>(untracked)};
        std::int32_t firstRotation[2] = {0, 1};
        std::int32_t secondRotation[2] = {0, 2};
        auto* rotation = MakeMessage(0xa6b79602, firstRotation, secondRotation);
        CHECK(manager->DoMessage(rotation));
        rotation->Release();
        CHECK(host.occupants.bboxCalls == 1);
        CHECK(untracked->rotationUpdates == 1);
        CHECK(untracked->refs == 1);
        // Same rotation: no query.
        secondRotation[1] = 1;
        rotation = MakeMessage(0xa6b79602, firstRotation, secondRotation);
        CHECK(manager->DoMessage(rotation));
        rotation->Release();
        CHECK(host.occupants.bboxCalls == 1);

        // 0x6ab97795: the bridge-model refresh walks the network occupants. Go leaks its QI reference in
        // the original; the test drops it again so the sanitizer stays quiet.
        auto* visibleFlagged = NewOccupant();
        visibleFlagged->networkFlags = 0x40000;
        visibleFlagged->flags = kOccupantFlagVisible;
        auto* invisible = NewOccupant();
        invisible->networkFlags = 0x40000;   // flagged but not visible
        auto* plain = NewOccupant();
        host.occupants.iterated = {visibleFlagged, invisible, plain};
        auto* bridges = MakeMessage(0x6ab97795);
        CHECK(manager->DoMessage(bridges));
        bridges->Release();
        CHECK(host.occupants.iterateCalls == 1);
        CHECK(host.occupants.lastType == kNetworkOccupantType);
        CHECK(visibleFlagged->posted.size() == 1);
        CHECK(visibleFlagged->posted[0].first == 0x99ef1145);
        CHECK(invisible->posted.empty());
        CHECK(plain->posted.empty());
        // Drop the leaked QI references (one per iteration) — the original leaks them.
        visibleFlagged->Release();
        invisible->Release();
        plain->Release();
        visibleFlagged->Release();
        invisible->Release();
        plain->Release();

        // ---- DoViewRotationChange directly -------------------------------------------------------
        const int before = untracked->rotationUpdates;
        manager->DoViewRotationChange(1, 1);   // equal: nothing
        CHECK(untracked->rotationUpdates == before);
        manager->DoViewRotationChange(0, 2);
        CHECK(untracked->rotationUpdates == before + 1);

        // ---- Read / Write -------------------------------------------------------------------------
        // Write: version 1, the tracked count, then every tracked occupant.
        host.written.clear();
        PortMemoryOStream out;
        CHECK(manager->Write(out));
        CHECK(host.serializedWrites == 1);
        CHECK(host.written.size() == 1);
        CHECK(host.written[0] == static_cast<const cIGZUnknown*>(static_cast<INetworkManagerOccupant*>(tracked)));
        CHECK(out.bytes().size() >= 6);
        CHECK(out.bytes()[0] == 1 && out.bytes()[1] == 0);   // u16 version 1, little endian
        CHECK(out.bytes()[2] == 1 && out.bytes()[3] == 0 && out.bytes()[4] == 0 && out.bytes()[5] == 0);

        // Read: u16 version 1, u32 count; the host hands one counted occupant back per entry.
        auto* reader = new cSC4NetworkManager();
        reader->Init();
        auto* readBack = NewOccupant();
        host.readQueue = {static_cast<cISC4Occupant*>(readBack)};
        const unsigned char data[] = {1, 0, 1, 0, 0, 0};
        PortMemoryIStream in(data, sizeof data);
        CHECK(reader->Read(in));
        CHECK(host.serializedReads == 1);
        CHECK(NetworkManagerTestAccess::Tracked(*reader) == 1);
        CHECK(readBack->refs == 2);
        reader->Shutdown();
        CHECK(readBack->refs == 1);
        readBack->Release();

        // Wrong version and a truncated stream both fail.
        auto* wrongVersion = new cSC4NetworkManager();
        const unsigned char version2[] = {2, 0, 0, 0, 0, 0};
        PortMemoryIStream badVersion(version2, sizeof version2);
        CHECK(!wrongVersion->Read(badVersion));
        CHECK(NetworkManagerTestAccess::Tracked(*wrongVersion) == 0);
        const unsigned char truncated[] = {1, 0};
        PortMemoryIStream badStream(truncated, sizeof truncated);
        CHECK(!wrongVersion->Read(badStream));
        wrongVersion->Release();
        reader->Release();

        // ---- pipe / subway forwarders and occupant filters ---------------------------------------
        CHECK(host.subwayStationAnswer == false);
        host.subwayStationAnswer = true;
        CHECK(manager->IsOccupantSubwayStation(tracked));
        cSC4PipeOccupant* pipeOccupant = nullptr;
        manager->FindPipeOccupant(3, 4, pipeOccupant);
        CHECK(pipeOccupant == reinterpret_cast<cSC4PipeOccupant*>(&host.pipeMarker));
        CHECK(host.pipeSearches == 1);
        cSC4PipeConnectionOccupant* pipeConnection = nullptr;
        manager->FindPipeConnectionOccupant(5, 6, pipeConnection);
        CHECK(pipeConnection == reinterpret_cast<cSC4PipeConnectionOccupant*>(&host.pipeConnectionMarker));
        CHECK(host.pipeConnectionSearches == 1);
        CHECK(manager->GetWaterOccupantFilter() ==
              reinterpret_cast<cISC4OccupantFilter*>(&host.waterFilterMarker));
        CHECK(manager->GetSubwayOccupantFilter() ==
              reinterpret_cast<cISC4OccupantFilter*>(&host.subwayFilterMarker));
        CHECK(host.filterWaterRequests == 1 && host.filterSubwayRequests == 1);

        // ---- the partial entry points answer safely ----------------------------------------------
        bool replaced = false;
        CHECK(!manager->RemoveOrReplaceOccupant(nullptr, tracked, replaced));
        ilist<cISC4Occupant*> intersectionList;
        manager->GetAllIntersectionPieces(static_cast<INetworkManagerOccupant*>(tracked), intersectionList,
                                          &host.occupants, static_cast<INetworkManagerOccupant*>(tracked));
        CHECK(intersectionList.empty());
        cISC4Occupant* node = nullptr;
        SC4PointT<int> cell{1, 2};
        CHECK(!manager->CreateConnectionNode(node, kNetworkRoad, cell, 0, 0, 0, 0));
        CHECK(node == nullptr);
        ilist<cISC4Occupant*> autoconnect;
        manager->AutoconnectLotNodes(nullptr, autoconnect);
        manager->Retrofit();   // PARTIAL: no state to check yet

        // The tracked occupant is released by Shutdown.
        manager->Shutdown();
        CHECK(tracked->refs == 1);
        tracked->Release();
        untracked->Release();
        manager->Release();
    }

    // ---- no host at all ---------------------------------------------------------------------------
    {
        SetNetworkManagerHost(nullptr);
        auto* manager = new cSC4NetworkManager();
        CHECK(manager->Init());
        CHECK(manager->GetNetworkTool(kNetworkRoad, true) == nullptr);
        CHECK(manager->GetWaterOccupantFilter() == nullptr);
        CHECK(!manager->IsOccupantSubwayStation(nullptr));
        std::uint8_t alpha = 0;
        CHECK(manager->MapPreviewTexture(7, alpha) == 7);
        CHECK(manager->MapPath(7) == 7);
        manager->Shutdown();
        manager->Release();
    }

    std::remove(iniPath.c_str());
    if (g_fail != 0) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::puts("cSC4NetworkManager ok");
    return 0;
}
