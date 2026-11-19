#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unistd.h>
#include <unordered_set>

#include "gz/CommandParameterSet.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"
#include "sc4/AutoHelpers.h"
#include "sc4/DBSegment.h"
#include "sc4/FloraSimulator.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/SCPropertyHelp.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
constexpr std::uint32_t kTree = 0x1000, kBarren = 0x2000;   // a tree that scores 0.6 here, one that scores 0
const cGZPersistResourceKey kTuningKey{0x6534284a, 0x7a4a8458, 0x1a2fdb6a};

// The message server: what the simulator subscribes and posts.
class FakeMessageServer final : public cIGZMessageServer2 {
public:
    struct Posted { std::uint32_t type; std::uint64_t data2; cIGZUnknown* unknown; };
    std::vector<std::uint32_t> added, removed;
    std::vector<Posted> posted;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool) override {
        void* raw = nullptr;
        if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return false;
        auto* s = static_cast<cIGZMessage2Standard*>(raw);
        posted.push_back({s->GetType(), s->GetData2(), s->GetIGZUnknown()});
        s->Release();
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t type) override { added.push_back(type); return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override { removed.push_back(type); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return false; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

class Host final : public IFloraSimulatorHost {
public:
    float sea = 100.0f, height = 100.0f;
    std::function<float(float, float)> ground;               // the ground under a point (default: `height`)
    int terrainClass = 7;
    cISC4OccupantManager* occupants = nullptr;
    float sizeX = 1024.0f, sizeZ = 1024.0f;
    std::int32_t cellsX = 64, cellsZ = 64;
    bool established = false, terrainInitialized = true;
    std::uint32_t simDate = 0;
    cISCResExemplar* tuning = nullptr;                       // the flora tuning exemplar (null: absent)
    std::map<std::uint32_t, cISCResExemplar*> flora;         // flora exemplars by instance, enumerated in instance order
    cIGZPersistDBSegment* obliterated = nullptr;
    FakeMessageServer server;
    struct Agent { cIGZMessageTarget2* target; int level; std::string name; std::uint32_t flags; };
    std::vector<Agent> agents;
    std::vector<cIGZMessageTarget2*> agentsRemoved;
    std::vector<std::pair<std::uint32_t, std::string>> commands, cheats;
    std::vector<const char*> commandInfo;
    std::vector<std::uint32_t> commandsRemoved, cheatsRemoved, cheatListeners, cheatListenersRemoved;

    cISCResExemplar* LoadExemplar(const cGZPersistResourceKey& key) override {
        cISCResExemplar* e = nullptr;
        if (key.type == kTuningKey.type && key.group == kTuningKey.group && key.instance == kTuningKey.instance) e = tuning;
        else if (key.type == kFloraDirectoryTypeId && flora.count(key.instance)) e = flora[key.instance];
        if (e) e->AddRef();
        return e;
    }
    void EnumerateFloraResources(void (*callback)(const cGZPersistResourceKey&, void*), void* context) override {
        for (auto& entry : flora) callback(cGZPersistResourceKey{kFloraDirectoryTypeId, 0xe83e0437, entry.first}, context);
    }
    float SeaLevel() override { return sea; }
    float TerrainHeight(float x, float z) override { return ground ? ground(x, z) : height; }
    int TerrainClass(float, float) override { return terrainClass; }
    cISC4OccupantManager* OccupantManager() override { return occupants; }
    float GroundHeight(float x, float z, bool& onLot) override { onLot = false; return TerrainHeight(x, z); }
    float TerrainNormalY(float, float) override { return 1.0f; }
    void CitySizeMeters(float& x, float& z) override { x = sizeX; z = sizeZ; }
    std::uint32_t SimDate() override { return simDate; }
    cIGZMessageServer2* MessageServer() override { return &server; }
    bool RegisterCommand(std::uint32_t id, const char* name, const char* info) override {
        commands.emplace_back(id, name);
        commandInfo.push_back(info);
        return true;
    }
    bool UnregisterCommand(std::uint32_t id) override { commandsRemoved.push_back(id); return true; }
    bool AddSimulatorAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) override {
        agents.push_back({target, level, name, flags});
        return true;
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { agentsRemoved.push_back(target); }
    bool CheatCodeManagerPresent() override { return true; }
    void RegisterCheatCode(std::uint32_t id, const char* text) override { cheats.emplace_back(id, text); }
    void UnregisterCheatCode(std::uint32_t id) override { cheatsRemoved.push_back(id); }
    void AddCheatNotification(cIGZMessageTarget2*, std::uint32_t id) override { cheatListeners.push_back(id); }
    void RemoveCheatNotification(cIGZMessageTarget2*, std::uint32_t id) override { cheatListenersRemoved.push_back(id); }
    bool CityPresent() override { return true; }
    void CityCellCounts(std::int32_t& x, std::int32_t& z) override { x = cellsX; z = cellsZ; }
    bool CityEstablished() override { return established; }
    bool TerrainInitialized() override { return terrainInitialized; }
    cIGZPersistDBSegment* ObliteratedCitySegment() override { return obliterated; }
};

cISCResExemplar* MakeFlora(std::uint32_t type, std::uint32_t familyKey, bool aquatic, std::vector<std::uint32_t> families,
                           float suit, int elevation, int terrainClass) {
    auto* e = new cSCResExemplar();
    e->AddRef();
    cISCPropertyHolder* h = e->AsISCPropertyHolder();
    cSCPropertyHelp::SetPropertyValue(h, 0x10, std::uint32_t{kFloraExemplarTypeValue});
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropTypeId, type);
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropFamilyKey, familyKey);
    if (aquatic) cSCPropertyHelp::SetPropertyValue(h, kFloraPropAquatic, std::uint32_t{1});
    std::vector<float> grid(256, 0.0f);
    grid[elevation * 16 + terrainClass] = suit;
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropSuitability, grid);
    std::vector<float> traits(14, 2.0f);
    traits[10] = 5.0f; traits[13] = 9.0f;
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropTraits, traits);
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropFamilies, families);
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropCost, std::int64_t{25});
    return e;
}

// A flora type for the simulation checks: the code-default ages (maturity 730, seeding age 730, interval 365, lifespan 1460,
// maximum age 1560), minimum score 0.3, seed probability 0.5, 2 seeds within 6 m, tiny boxes (no clearance conflicts), no
// cluster; table value `suit` at elevation 0 and moisture 7 (flat ground at the sea level).
cISCResExemplar* MakeSimFlora(std::uint32_t type, float suit, bool neverAutomatic = false) {
    auto* e = new cSCResExemplar();
    e->AddRef();
    cISCPropertyHolder* h = e->AsISCPropertyHolder();
    cSCPropertyHelp::SetPropertyValue(h, 0x10, std::uint32_t{kFloraExemplarTypeValue});
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropTypeId, type);
    std::vector<float> grid(256, 0.0f);
    grid[0 * 16 + 7] = suit;
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropSuitability, grid);
    const std::vector<float> traits = {730, 730, 365, 2, 6, 1460, 1560, 0.5f, 0.3f, 0.5f, 0.05f, 4, 0.05f, 0.05f};
    cSCPropertyHelp::SetPropertyValue(h, kFloraPropTraits, traits);
    if (neverAutomatic) cSCPropertyHelp::SetPropertyValue(h, kFloraPropFlagExcluded, true);
    return e;
}

cISCResExemplar* MakeTuning(float burn, float step, std::uint32_t period, std::uint32_t wild, float elevation, bool withWild = true) {
    auto* e = new cSCResExemplar();
    e->AddRef();
    cISCPropertyHolder* h = e->AsISCPropertyHolder();
    cSCPropertyHelp::SetPropertyValue(h, 0xc9dbdd00, burn);
    cSCPropertyHelp::SetPropertyValue(h, 0xc9dbdd01, step);
    cSCPropertyHelp::SetPropertyValue(h, 0x8761abd0, period);
    if (withWild) cSCPropertyHelp::SetPropertyValue(h, 0x085489c7, wild);
    cSCPropertyHelp::SetPropertyValue(h, 0xc9dbdd02, elevation);
    return e;
}

struct Accept : cISC4FloraTypeFilter {
    bool Accepts(std::uint32_t t) override { return t == 0x20; }
};

// a building / prop stand-in with a fixed box
class BoxOccupant final : public cSC4Occupant {
public:
    BoxOccupant(std::uint32_t type, std::array<float, 3> min, std::array<float, 3> max) : type(type), min(min), max(max) {}
    std::uint32_t GetType() const override { return type; }
    void GetBoundingBox(float* lo, float* hi) override { for (int i = 0; i < 3; ++i) { lo[i] = min[i]; hi[i] = max[i]; } }
    std::uint32_t type;
    std::array<float, 3> min, max;
};

void Send(cSC4FloraSimulator& sim, std::uint32_t type, std::uint64_t data1 = 0, std::uint64_t data2 = 0, void* void1 = nullptr,
          std::uint64_t data3 = 0) {
    auto* m = new cRZMessage2Standard();
    m->AddRef();
    m->SetType(type);
    m->SetData1(data1);
    if (void1) m->SetVoid1(void1);
    m->SetData2(data2);
    m->SetData3(data3);
    CHECK(sim.DoMessage(m));
    m->Release();
}

void Cheat(cSC4FloraSimulator& sim, const char* line, std::uint32_t id = kFloraCheatCode) {
    cRZString text(line);
    auto* m = new cRZMessage2Standard();
    m->AddRef();
    m->SetType(kFloraMsgCheatCode);
    m->SetData1(id);
    m->SetVoid2(static_cast<cIGZString*>(&text));
    CHECK(sim.DoMessage(m));
    m->Release();
}

cSC4OccupantManager* NewManager(float size) {
    auto* manager = new cSC4OccupantManager();
    manager->AddRef();
    manager->SetCitySize(size, size);
    manager->Init();
    return manager;
}

void FreeManager(cSC4OccupantManager* manager) {
    manager->RemoveOccupants(static_cast<int>(kOccupantTypeFlora), true, 0);
    manager->Shutdown();
    manager->Release();
}

// A flora occupant in the manager (the manager keeps the only reference).
cSC4FloraOccupant* Plant(cISC4OccupantManager* manager, cSC4FloraSimulator& sim, std::uint32_t type, float x, float y, float z,
                         std::uint32_t birth, std::int32_t state, std::uint32_t lastSeed) {
    auto* flora = new cSC4FloraOccupant();
    flora->AddRef();
    flora->SetFloraType(type);
    flora->SetStaticFloraData(sim.GetStaticFloraData(type));
    const float xyz[3] = {x, y, z};
    flora->SetPosition(xyz);
    flora->SetBirthDate(birth);
    flora->SetState(state);
    flora->SetLastSeedingDate(lastSeed);
    CHECK(manager->InsertOccupant(flora->AsOccupant(), 0));
    flora->Release();
    return flora;
}

std::vector<cSC4FloraOccupant*> AllFlora(cISC4OccupantManager* manager) {
    OccupantList found;
    manager->GetOccupantsByStandardCityCells(found, nullptr, nullptr, kOccupantTypeFlora, 0xffffffffu);
    std::vector<cSC4FloraOccupant*> out;
    for (cISC4Occupant* o : found) {
        out.push_back(dynamic_cast<cSC4FloraOccupant*>(o));
        o->Release();
    }
    return out;
}

cSC4FloraOccupant* FloraAt(cISC4OccupantManager* manager, float x, float z) {
    for (cSC4FloraOccupant* f : AllFlora(manager)) {
        float p[3];
        f->GetPosition(p);
        if (p[0] == x && p[2] == z) return f;
    }
    return nullptr;
}

// A packed file under the temp directory.
struct TempSegment {
    std::string path;
    cSC4DBSegment* seg = nullptr;
    explicit TempSegment(const std::string& name) : path(std::string("/tmp/sc4_flora_") + name + "_" + std::to_string(getpid()) + ".sc4") {}
    ~TempSegment() { Close(); unlink(path.c_str()); }
    void Open(bool write) {
        seg = new cSC4DBSegment();
        seg->AddRef();
        seg->Init();
        cRZString p(path.c_str());
        seg->SetPath(p);
        CHECK(write ? seg->Open(false, true) : seg->Open(true, false));
    }
    void Close() {
        if (!seg) return;
        seg->Close();
        seg->Release();
        seg = nullptr;
    }
    cIGZPersistDBSegment* Persist() { return static_cast<cIGZPersistDBSegment*>(seg); }
};

// The bytes of a record as its stream hands them out.
std::vector<std::uint8_t> RecordBytes(cSC4DBSegment* seg, const cGZPersistResourceKey& key) {
    std::vector<std::uint8_t> out;
    cISC4DBSegmentIStream* is = nullptr;
    if (!seg->OpenIStream(key, &is) || !is) return out;
    std::uint8_t b = 0;
    while (is->GetUint8(b)) out.push_back(b);
    seg->CloseIStream(is);
    is->Release();
    return out;
}

// Writes the simulator's record to a fresh packed file and returns its bytes.
std::vector<std::uint8_t> WrittenBytes(cSC4FloraSimulator& sim, const char* name) {
    const cGZPersistResourceKey key{kFloraSimulatorClassId, 0x299b2d1b, 0};
    TempSegment file(name);
    file.Open(true);
    cISC4DBSegmentOStream* os = nullptr;
    CHECK(file.seg->OpenOStream(key, &os, true) && os);
    if (os) {
        CHECK(sim.Write(*os));
        file.seg->CloseOStream(os);
        os->Release();
    }
    file.Close();
    file.Open(false);
    std::vector<std::uint8_t> bytes = RecordBytes(file.seg, key);
    file.Close();
    return bytes;
}

cSC4FloraSimulator* NewSimulator() {
    auto* sim = new cSC4FloraSimulator();
    sim->AddRef();
    return sim;
}
}  // namespace

int main() {
    Host host;
    SetFloraSimulatorHost(&host);
    // defaults
    cSC4StaticFloraData def;
    cSC4FloraSimulator::MakeDefaultStaticFloraData(def);
    CHECK(def.HalfExtent() == 1.0f && def.Height() == 3.0f && def.traits[0] == 730.0f);
    CHECK(def.suitability[5][5] == 0x99 && def.suitability[12][12] == 0x99);
    CHECK(def.suitability[4][5] == 0 && def.suitability[5][13] == 0 && def.suitability[0][0] == 0);

    cSC4FloraSimulator sim;
    CHECK(cSC4FloraSimulator::StaticInit());
    cGZPersistResourceKey key;
    cISCResExemplar* a = MakeFlora(0x10, 0x10, false, {0xA, 0xB}, 0.5f, 3, 7);
    cISCResExemplar* b = MakeFlora(0x20, 0x10, true, {0xA}, 1.0f, 2, 7);
    CHECK(cSC4FloraSimulator::AddStaticFloraData(key, a));
    CHECK(cSC4FloraSimulator::AddStaticFloraData(key, b));
    CHECK(!cSC4FloraSimulator::AddStaticFloraData(key, a));             // duplicate type
    CHECK(sim.IsFloraTypeValid(0x10) && sim.IsFloraTypeValid(0x20) && !sim.IsFloraTypeValid(0x30));
    CHECK(sim.IsFloraFamilyTypeValid(0xA) && sim.IsFloraFamilyTypeValid(0x10) && !sim.IsFloraFamilyTypeValid(0x99));
    cSC4StaticFloraData* d = sim.GetStaticFloraData(0x10);
    CHECK(d && d->HalfExtent() == 5.0f && d->Height() == 9.0f && d->cost == 25 && d->familyKey == 0x10);
    CHECK(d->suitability[3][7] == 127 && !d->aquatic);
    CHECK(sim.GetStaticFloraData(0x20)->aquatic);
    CHECK(d->exemplar == a);

    std::vector<std::uint32_t> types, families;
    sim.GetAllFloraTypes(types);
    CHECK(types.size() == 2 && types[0] == 0x10 && types[1] == 0x20);
    sim.GetAllFloraFamilyTypes(families);
    CHECK(families.size() == 2 && families[0] == 0xA && families[1] == 0xB);
    std::uint32_t buf[4] = {}, n = 1;
    CHECK(sim.GetAllFloraTypes(buf, n) == 1 && buf[0] == 0x10);
    n = 0;
    sim.GetAllFloraTypes(nullptr, n);
    CHECK(n == 2);
    std::unordered_set<std::uint32_t> set;
    CHECK(sim.GetFloraTypesByFamily(set, 0xA) == 2 && set.size() == 2);
    set.clear();
    CHECK(sim.GetFloraTypesByFamily(set, 0xC) == 0);
    Accept accept;
    CHECK(sim.GetFloraTypesByFilter(set, &accept) == 1 && set.count(0x20));

    // elevation + suitability (flora.md 6.1: floor, not rounding). Scale 1: 3 above the sea -> index 3; the aquatic type
    // measures below the sea (none -> 0).
    sim.SetElevationScale(1.0f);
    host.height = 103.0f;
    CHECK(sim.GetElevationIndex(0, 0, false) == 3);
    CHECK(sim.GetElevationIndex(0, 0, true) == 0);
    host.height = 400.0f;
    CHECK(sim.GetElevationIndex(0, 0, false) == 15);
    host.height = 103.0f;
    CHECK(std::fabs(sim.GetPropagationScoreForFlora(0x10, 0, 0) - 127 * 0.003921569f) < 1e-6f);
    CHECK(sim.GetPropagationScoreForFlora(0x77, 0, 0) == 0.0f);
    host.height = 98.0f;                                                // 2 below sea
    float score = -1;
    CHECK(sim.GetBestFloraTypeForLocation(0, 0, score) == 0x20 && std::fabs(score - 1.0f) < 1e-6f);
    host.height = 103.0f;
    CHECK(sim.GetBestFloraTypeForLocation(0, 0, score) == 0x10);
    sim.SetElevationScale(0.5f);
    CHECK(sim.GetElevationIndex(0, 0, false) == 1);                     // 1.5 -> floor 1
    {   // conformance 3 (flora.md 16): shipped scale 16 / 800, sea 250
        host.sea = 250.0f;
        sim.SetElevationScale(16.0f / 800.0f);
        const auto at = [&](float d, bool water) { host.height = 250.0f + d; return sim.GetElevationIndex(0, 0, water); };
        CHECK(at(75, false) == 1);
        CHECK(at(310, false) == 6);
        CHECK(at(1000, false) == 15);
        CHECK(at(-30, false) == 0);
        CHECK(at(50, false) == 1);
        CHECK(at(100, false) == 2);
        CHECK(at(150, false) == 3);
        CHECK(at(-120, true) == 2);
        sim.SetElevationScale(0.08f);
        CHECK(at(70, false) == 5);
        host.sea = 100.0f;
        host.height = 100.0f;
    }

    // SC4ReadGZSerializable round trip: the city-load serializer creates the object from its class id
    // through the factory, then QueryInterface(0xe4fda3d4) hands out the cIGZSerializable view.
    {
        struct Factory : ISerializableFactory {
            cIGZSerializable* CreateSerializable(std::uint32_t classId) override {
                if (classId != kFloraSimulatorClassId) return nullptr;
                auto* f = new cSC4FloraSimulator();
                f->AddRef();
                return f;
            }
        } factory;
        SetSerializableFactory(&factory);
        std::string path = "/tmp/sc4_flora_rt_" + std::to_string(getpid()) + ".sc4";
        const cGZPersistResourceKey rtKey{0x90909090, 0xa0a0a0a0, 1};
        {
            cSC4DBSegment* seg = new cSC4DBSegment();
            seg->AddRef(); seg->Init();
            cRZString p(path.c_str()); seg->SetPath(p);
            CHECK(seg->Open(false, true));
            cISC4DBSegmentOStream* os = nullptr;
            CHECK(seg->OpenOStream(rtKey, &os, true));
            sim.AddRef();   // keep the stack object alive across the serializer's add/release pair
            CHECK(SC4WriteGZSerializable(*os, static_cast<cIGZSerializable*>(&sim)));
            CHECK(seg->CloseOStream(os)); os->Release();
            CHECK(seg->Close()); seg->Release();
        }
        {
            cSC4DBSegment* seg = new cSC4DBSegment();
            seg->AddRef(); seg->Init();
            cRZString p(path.c_str()); seg->SetPath(p);
            CHECK(seg->Open(true, false));
            CHECK(seg->LoadClassObjects(0x48));
            cISC4DBSegmentIStream* is = nullptr;
            CHECK(seg->OpenIStream(rtKey, &is));
            void* raw = nullptr;
            CHECK(SC4ReadGZSerializable(*is, kFloraSimulatorInterfaceId, &raw, false));
            CHECK(raw != nullptr);
            auto* rt = static_cast<cSC4FloraSimulator*>(static_cast<cIGZSerializable*>(raw));
            CHECK(rt && rt->GetGZCLSID() == kFloraSimulatorClassId);
            CHECK(rt && rt->Period() == 6 && rt->Enabled() && rt->WildSeedsPerMonth() == 5);
            if (raw) static_cast<cIGZSerializable*>(raw)->Release();
            CHECK(seg->CloseIStream(is)); is->Release();
            CHECK(seg->Close()); seg->Release();
        }
        unlink(path.c_str());
        SetSerializableFactory(nullptr);
    }

    CHECK(cSC4FloraSimulator::StaticShutdown());
    CHECK(!sim.IsFloraTypeValid(0x10));
    a->Release();
    b->Release();
    {   // DemolishFloraInCellArea (Mac i386 0x0008a9d2): the flora on the inclusive cell rect leaves the occupant manager; a
        // plant outside the rect and a network piece inside it stay
        auto* manager = NewManager(1024);
        host.occupants = manager;
        const auto plant = [manager](float x, float z) {
            auto* flora = new cSC4FloraOccupant();
            flora->AddRef();
            const float xyz[3] = {x, 100.0f, z};
            flora->SetPosition(xyz);
            CHECK(manager->InsertOccupant(flora, 0));
            flora->Release();   // the manager keeps it
        };
        plant(2 * 16 + 4, 3 * 16 + 4);    // cell 2,3
        plant(3 * 16 + 12, 4 * 16 + 8);   // cell 3,4
        plant(6 * 16 + 8, 6 * 16 + 8);    // cell 6,6: outside
        auto* piece = new cSC4NetworkOccupant();
        piece->AddRef();
        const float corners[4][3] = {{32, 100, 48}, {48, 100, 48}, {48, 100, 64}, {32, 100, 64}};   // cell 2,3
        piece->Init(corners);
        CHECK(manager->InsertOccupant(piece, 0));
        const auto count = [manager](std::uint32_t type, int x0, int z0, int x1, int z1) {
            OccupantList found;
            const int a[2] = {x0, z0}, b2[2] = {x1, z1};
            manager->GetOccupantsByStandardCityCells(found, a, b2, type, 0xffffffffu);
            const int n = static_cast<int>(found.size());
            for (cISC4Occupant* occupant : found) occupant->Release();
            return n;
        };
        CHECK(count(kOccupantTypeFlora, 0, 0, 63, 63) == 3);
        CHECK(count(kOccupantTypeNetwork, 2, 3, 2, 3) == 1);
        const std::int32_t rect[4] = {2, 3, 3, 4};
        CHECK(sim.DemolishFloraInCellArea(rect, 0));
        CHECK(count(kOccupantTypeFlora, 2, 3, 3, 4) == 0);
        CHECK(count(kOccupantTypeFlora, 6, 6, 6, 6) == 1);
        CHECK(count(kOccupantTypeNetwork, 2, 3, 2, 3) == 1);
        manager->RemoveOccupant(piece, true, 0);
        piece->Release();
        host.occupants = nullptr;
        FreeManager(manager);
    }

    // ============================================================== Init / Shutdown (flora.md 4)
    host.flora[kTree] = MakeSimFlora(kTree, 0.6f);
    host.flora[kBarren] = MakeSimFlora(kBarren, 0.0f);
    host.flora[kBurntFloraType] = MakeSimFlora(kBurntFloraType, 0.0f, true);
    host.tuning = MakeTuning(1.0f, 1.0f, 24, 0, 800.0f);                // the shipped tuning
    {
        auto* manager = NewManager(1024);
        host.occupants = manager;
        cSC4FloraSimulator* s = NewSimulator();
        const std::size_t postsBefore = host.server.posted.size();     // the earlier StaticInit posted once
        CHECK(!s->IsInitialized());
        CHECK(s->Init());
        CHECK(s->IsInitialized());
        const std::vector<std::uint32_t> subscribed = {0x278e6003, 0x99ef1142, 0x99ef1143, 0xc53d10aa, 0x26d31ec4, 0x26d31ec5, 0xab99a64b};
        CHECK(host.server.added == subscribed);
        CHECK(host.commands.size() == 1 && host.commands[0].first == 0xab99a64b && host.commands[0].second == "PlaceFlora" &&
              host.commandInfo[0] == nullptr);
        CHECK(host.agents.size() == 2 && host.agents[0].level == 4 && host.agents[1].level == 6);
        CHECK(host.agents[0].name == "cSC4FloraSimulator" && host.agents[0].flags == 3 && host.agents[1].flags == 3);
        void* target = nullptr;
        CHECK(s->QueryInterface(0x452294aa, &target) && target == host.agents[0].target && target == host.agents[1].target);
        if (target) static_cast<cIGZMessageTarget2*>(target)->Release();
        CHECK(host.cheats.size() == 1 && host.cheats[0].first == 0x095bdc9c && host.cheats[0].second == "Flora");
        CHECK(host.cheatListeners == std::vector<std::uint32_t>{0x095bdc9c});
        // FloraTypesUpdated after the first load: the simulator, the three types (key 0 comes later, step 8)
        void* primary = nullptr;
        CHECK(s->QueryInterface(0x06c66177, &primary));
        CHECK(host.server.posted.size() == postsBefore + 1 && host.server.posted.back().type == 0x87a6277f &&
              host.server.posted.back().data2 == 3 && host.server.posted.back().unknown == primary);
        void* one = nullptr;
        CHECK(s->QueryInterface(1, &one) && one == primary);
        void* serializable = nullptr;
        CHECK(s->QueryInterface(0xe4fda3d4, &serializable) && serializable == primary);
        void* none = nullptr;
        CHECK(!s->QueryInterface(kFloraSimulatorClassId, &none) && none == nullptr);
        for (void* p : {primary, one, serializable}) static_cast<cIGZSerializable*>(p)->Release();
        // the key-0 record, the state, the tuning (flora.md 3.2, 4.1 steps 6 - 10, 7)
        CHECK(s->IsFloraTypeValid(0) && s->GetStaticFloraData(0) && s->GetStaticFloraData(0)->flags == 0x01);
        std::uint32_t count = 0;
        s->GetAllFloraTypes(nullptr, count);
        CHECK(count == 3);
        CHECK(s->Period() == 24 && s->BlocksPerMonth() == 42 && s->WildSeedsPerMonth() == 0);
        CHECK(s->ElevationScale() == 16.0f / 800.0f && s->BurnRate() == 1.0f && s->BurntRemovalStep() == 1.0f);
        CHECK(s->Propagation() && s->Enabled() && !s->PropagationForced() && !s->Preserve());
        CHECK(!s->BlockIterator().IsModular());
        CHECK(s->Init());                                               // a second Init does nothing
        CHECK(host.server.added.size() == 7 && host.agents.size() == 2 && host.cheats.size() == 1);
        // Shutdown: the reverse registrations (flora.md 4.2)
        CHECK(s->Shutdown());
        const std::vector<std::uint32_t> unsubscribed = {0x26d31ec5, 0x26d31ec4, 0xc53d10aa, 0x278e6003, 0x99ef1142, 0x99ef1143, 0xab99a64b};
        CHECK(host.server.removed == unsubscribed);
        CHECK(host.commandsRemoved == std::vector<std::uint32_t>{0xab99a64b});
        CHECK(host.agentsRemoved.size() == 1 && host.agentsRemoved[0] == host.agents[0].target);
        CHECK(host.cheatsRemoved == std::vector<std::uint32_t>{0x095bdc9c});
        CHECK(host.cheatListenersRemoved == std::vector<std::uint32_t>{0x095bdc9c});
        CHECK(!s->IsInitialized() && s->Shutdown() && host.server.removed.size() == 7);
        // established city (conformance 26): propagation off; CityObliterated on; CityEstablished back to the forced flag
        host.established = true;
        CHECK(s->Init() && !s->Propagation());
        CHECK(host.server.posted.size() == postsBefore + 1);            // the types load once per process
        Send(*s, kFloraMsgCityObliterated);
        CHECK(s->Propagation());
        Send(*s, kFloraMsgCityEstablished);
        CHECK(!s->Propagation());
        {   // the same two messages as plain messages (no payload interface): handled the same way
            struct Plain final : cIGZMessage2 {
                std::uint32_t type = 0;
                bool QueryInterface(std::uint32_t, void**) override { return false; }
                std::uint32_t AddRef() override { return 2; }
                std::uint32_t Release() override { return 1; }
                bool Create(std::uint32_t, void**) const override { return false; }
                std::uint32_t GetType() const override { return type; }
                void SetType(std::uint32_t t) override { type = t; }
                bool IsEqual(const cIGZMessage2&) const override { return false; }
                bool IsLess(const cIGZMessage2&) const override { return false; }
            } plain;
            plain.type = kFloraMsgCityObliterated;
            s->DoMessage(&plain);
            CHECK(s->Propagation());
            plain.type = kFloraMsgCityEstablished;
            s->DoMessage(&plain);
            CHECK(!s->Propagation());
        }
        Cheat(*s, "Flora propogation");
        CHECK(s->Propagation() && s->PropagationForced());
        s->Shutdown();
        CHECK(s->Init() && s->Propagation());                           // forced survives into the next Init
        host.established = false;
        s->Release();                                                   // the final release shuts it down
        CHECK(host.server.removed.size() == 21);
        // period 0 (conformance 35): a divisor of 1, the period kept; wild seeds scale per 2^20 m^2 (conformance 8)
        cISCResExemplar* shipped = host.tuning;
        host.tuning = MakeTuning(1.0f, 1.0f, 0, 5, 800.0f);
        s = NewSimulator();
        s->Init();
        CHECK(s->Period() == 0 && s->BlocksPerMonth() == 1024 && s->WildSeedsPerMonth() == 5);
        s->Release();
        host.sizeX = host.sizeZ = 2048.0f;
        host.cellsX = host.cellsZ = 128;
        s = NewSimulator();
        s->Init();
        CHECK(s->WildSeedsPerMonth() == 20 && s->BlocksPerMonth() == 4096);
        s->Release();
        host.tuning->Release();
        // conformance 7: blocksPerMonth with the shipped period
        host.tuning = shipped;
        s = NewSimulator();
        s->Init();
        CHECK(s->BlocksPerMonth() == 170);
        s->Release();
        host.sizeX = host.sizeZ = 4096.0f;
        host.cellsX = host.cellsZ = 256;
        s = NewSimulator();
        s->Init();
        CHECK(s->BlocksPerMonth() == 682);
        s->Release();
        // no tuning exemplar: the code defaults (period 6: 64^2 -> 170)
        host.sizeX = host.sizeZ = 1024.0f;
        host.cellsX = host.cellsZ = 64;
        host.tuning = nullptr;
        s = NewSimulator();
        s->Init();
        CHECK(s->Period() == 6 && s->BlocksPerMonth() == 170 && s->WildSeedsPerMonth() == 5 && s->ElevationScale() == 0.08f &&
              s->BurntRemovalStep() == 4.0f);
        s->Release();
        host.tuning = shipped;
        host.occupants = nullptr;
        FreeManager(manager);
    }

    // ============================================================== monthly pass (flora.md 6.7)
    {
        // a 64 m city (4 x 4 cells, four 2x2 blocks); period 1 -> 4 blocks a month = every block once
        host.sizeX = host.sizeZ = 64.0f;
        host.cellsX = host.cellsZ = 4;
        cISCResExemplar* shipped = host.tuning;
        host.tuning = MakeTuning(1.0f, 1.0f, 1, 0, 800.0f);
        auto* manager = NewManager(64);
        host.occupants = manager;
        cSC4FloraSimulator* s = NewSimulator();
        s->Init();
        CHECK(s->BlocksPerMonth() == 4 && s->Propagation());
        const std::uint32_t today = 10000;
        host.simDate = today;
        cSC4FloraOccupant* young = Plant(manager, *s, kTree, 8, 100, 8, today - 400, 0, today - 400);       // block (0,0)
        cSC4FloraOccupant* old = Plant(manager, *s, kTree, 40, 100, 8, today - 1500, 1, today - 1500);      // block (1,0)
        Plant(manager, *s, kBarren, 8, 100, 40, today - 10, 0, today - 10);                                  // block (0,1)
        cSC4FloraOccupant* mature = Plant(manager, *s, kTree, 48, 100, 48, today - 800, 2, today - 800);    // block (1,1)
        // The generator draws of the month, from the spec: the mature tree's two seeds, each x then z within 6 m of it
        // (RandomDoubleRangeUniform), then for a seed that lands (flat ground, score 0.6 >= 0.3) one RandomDoubleUniform
        // (accepted for any u: 0.3 u < 0.7 * 0.5), then the new occupant's orientation (RandomSint32RangeUniform(0, 4)).
        s->Random().Seed(12345);
        std::srand(1);
        cRZRandom expect(12345);
        float seedX[2], seedZ[2];
        std::uint32_t orientation[2];
        for (int i = 0; i < 2; ++i) {
            seedX[i] = static_cast<float>(expect.RandomDoubleRangeUniform(42.0, 54.0));
            seedZ[i] = static_cast<float>(expect.RandomDoubleRangeUniform(42.0, 54.0));
            (void)expect.RandomDoubleUniform();
            orientation[i] = static_cast<std::uint32_t>(expect.RandomSint32RangeUniform(0, 4));
        }
        // preconditions of that derivation: the seeds stay clear of the parent and of each other (boxes 0.1 m wide)
        CHECK(std::fabs(seedX[0] - 48) > 0.2f || std::fabs(seedZ[0] - 48) > 0.2f);
        CHECK(std::fabs(seedX[1] - 48) > 0.2f || std::fabs(seedZ[1] - 48) > 0.2f);
        CHECK(std::fabs(seedX[0] - seedX[1]) > 0.2f || std::fabs(seedZ[0] - seedZ[1]) > 0.2f);
        Send(*s, kFloraMsgSimNewMonth, today);
        CHECK(s->Today() == today);
        CHECK(young->GetState() == 1 && young->GetBirthDate() == today - 400);                // 400 >= 365: maturing
        CHECK(old->GetState() == 0 && old->GetBirthDate() == today);                          // past its lifespan: reborn
        CHECK(mature->GetState() == 2 && mature->GetLastSeedingDate() == today);
        const std::vector<cSC4FloraOccupant*> all = AllFlora(manager);
        CHECK(all.size() == 5);                                                               // the barren one is gone, 2 seeds
        CHECK(!FloraAt(manager, 8, 40));
        for (int i = 0; i < 2; ++i) {
            cSC4FloraOccupant* seed = FloraAt(manager, seedX[i], seedZ[i]);
            CHECK(seed != nullptr);
            if (!seed) continue;
            float p[3];
            seed->GetPosition(p);
            CHECK(p[1] == 100.0f && seed->GetFloraType() == kTree && seed->GetState() == 0 && seed->GetBirthDate() == today);
            CHECK(seed->GetOrientation() == orientation[i] && seed->GetLastSeedingDate() == today);
        }
        CHECK(s->Random().RandomUint32Uniform() == expect.RandomUint32Uniform());           // nothing else was drawn
        // the next month: no tree is due (the seeding interval has not passed), nothing is drawn
        host.simDate = today + 30;
        Send(*s, kFloraMsgSimNewMonth, today + 30);
        CHECK(AllFlora(manager).size() == 5);
        CHECK(s->Random().RandomUint32Uniform() == expect.RandomUint32Uniform());
        // wild seeds (6.7.3): tuning 256 per 2^20 m^2 on a 64 m city -> 1 a month; a type draw (the barren and burnt types
        // score 0 or never seed: drawn and planted only when the spot scores), then x and z
        host.tuning->Release();
        host.tuning = MakeTuning(1.0f, 1.0f, 1, 256, 800.0f);
        Send(*s, kFloraMsgExemplarChanged, 0, kFloraTuningInstance);
        CHECK(s->WildSeedsPerMonth() == 1);
        std::vector<std::uint32_t> registered;
        s->GetAllFloraTypes(registered);
        std::uint32_t wildType = 0, wildX = 0, wildZ = 0;
        do {
            wildType = registered[expect.RandomUint32Uniform(static_cast<std::uint32_t>(registered.size()))];
        } while (wildType == kBurntFloraType);                         // flag bit 0: drawn again
        wildX = expect.RandomUint32Uniform(64);
        wildZ = expect.RandomUint32Uniform(64);
        const bool wildLands = wildType == kTree && wildX >= 1 && wildZ >= 1 && !FloraAt(manager, float(wildX), float(wildZ));
        if (wildLands) {
            (void)expect.RandomDoubleUniform();
            (void)expect.RandomSint32RangeUniform(0, 4);
        }
        host.simDate = today + 60;
        Send(*s, kFloraMsgSimNewMonth, today + 60);
        CHECK(AllFlora(manager).size() == (wildLands ? 6u : 5u));
        if (wildLands) CHECK(FloraAt(manager, float(wildX), float(wildZ)) != nullptr);
        CHECK(s->Random().RandomUint32Uniform() == expect.RandomUint32Uniform());

        // ============================================================== daily burnt removal (flora.md 6.8, conformance 9, 10)
        const auto burnFive = [&](cSC4FloraSimulator& sim2) {
            std::vector<cSC4FloraOccupant*> burnt;
            for (int i = 0; i < 5; ++i) {
                cSC4FloraOccupant* f = Plant(manager, sim2, kTree, 4.0f + 6.0f * i, 100, 20, today, 1, today);
                CHECK(sim2.KillFloraOccupant(f->AsOccupant()));
                CHECK(f->GetState() == 3 && f->GetFloraType() == kBurntFloraType);
                burnt.push_back(f);
            }
            CHECK(sim2.KillFloraOccupant(burnt[0]->AsOccupant()));       // already burnt: true, no change
            CHECK(sim2.BurntFlora().size() == 5);
            return burnt;
        };
        const auto standing = [&](const std::vector<cSC4FloraOccupant*>& burnt) {
            const std::vector<cSC4FloraOccupant*> now = AllFlora(manager);
            int n = 0;
            for (cSC4FloraOccupant* f : burnt) n += std::count(now.begin(), now.end(), f) ? 1 : 0;
            return n;
        };
        {
            // shipped (1, 1): one a day, oldest first
            const std::vector<cSC4FloraOccupant*> burnt = burnFive(*s);
            CHECK(s->BurntFlora().front() == burnt[0]->AsOccupant());
            Send(*s, kFloraMsgSimNewDay);
            CHECK(s->BurntFlora().size() == 4 && s->BurntFlora().front() == burnt[1]->AsOccupant());
            CHECK(!FloraAt(manager, 4, 20) && FloraAt(manager, 10, 20));
            Send(*s, kFloraMsgSimNewDay);
            CHECK(s->BurntFlora().size() == 3);
            CHECK(standing(burnt) == 3);
            // a burnt tree removed elsewhere leaves the list (and its reference)
            cISC4Occupant* third = burnt[2]->AsOccupant();
            third->AddRef();
            manager->RemoveOccupant(third, true, 0);
            Send(*s, kFloraMsgRemoveOccupant, 0, 0, third, kOccupantTypeFlora);
            CHECK(s->BurntFlora().size() == 2);
            third->Release();
            Send(*s, kFloraMsgSimNewDay);
            Send(*s, kFloraMsgSimNewDay);
            CHECK(s->BurntFlora().empty());
        }
        s->Release();
        const auto dailyWith = [&](cISCResExemplar* tuning, const std::function<void(cSC4FloraSimulator&)>& check) {
            cISCResExemplar* saved = host.tuning;
            host.tuning = tuning;
            cSC4FloraSimulator* sim2 = NewSimulator();
            sim2->Init();
            check(*sim2);
            sim2->Release();
            if (tuning) tuning->Release();
            host.tuning = saved;
        };
        dailyWith(nullptr, [&](cSC4FloraSimulator& sim2) {           // code defaults (1.0, 4.0): four a day
            burnFive(sim2);
            Send(sim2, kFloraMsgSimNewDay);
            CHECK(sim2.BurntFlora().size() == 1);
            Send(sim2, kFloraMsgSimNewDay);
            CHECK(sim2.BurntFlora().empty() && sim2.BurntRemovalAccumulator() == 0.0f);
        });
        dailyWith(MakeTuning(4.0f, 2.5f, 1, 0, 800.0f), [&](cSC4FloraSimulator& sim2) {   // day 4: 2 (acc 0.5), day 8: 3 (acc 0)
            burnFive(sim2);
            for (int day = 1; day <= 3; ++day) Send(sim2, kFloraMsgSimNewDay);
            CHECK(sim2.BurntFlora().size() == 5);
            Send(sim2, kFloraMsgSimNewDay);
            CHECK(sim2.BurntFlora().size() == 3 && sim2.BurntRemovalAccumulator() == 0.5f);
            for (int day = 5; day <= 7; ++day) Send(sim2, kFloraMsgSimNewDay);
            CHECK(sim2.BurntFlora().size() == 3);
            Send(sim2, kFloraMsgSimNewDay);
            CHECK(sim2.BurntFlora().empty() && sim2.BurntRemovalAccumulator() == 0.0f);
        });
        dailyWith(MakeTuning(0.5f, 1.0f, 1, 0, 800.0f), [&](cSC4FloraSimulator& sim2) {   // rate 2: one step a day, no more
            burnFive(sim2);
            for (int day = 1; day <= 3; ++day) {
                Send(sim2, kFloraMsgSimNewDay);
                CHECK(sim2.BurnAccumulator() == static_cast<float>(day));
                CHECK(sim2.BurntFlora().size() == static_cast<std::size_t>(5 - day));
            }
        });
        host.occupants = nullptr;
        FreeManager(manager);
        host.tuning->Release();
        host.tuning = shipped;
    }

    // ============================================================== cheats, occupants, terrain, PlaceFlora (flora.md 5, 6.10)
    {
        host.sizeX = host.sizeZ = 1024.0f;
        host.cellsX = host.cellsZ = 64;
        auto* manager = NewManager(1024);
        host.occupants = manager;
        cSC4FloraSimulator* s = NewSimulator();
        s->Init();
        const std::uint32_t today = 5000;
        host.simDate = today;
        // "Flora off" with a tree in each corner (conformance 34): every one removed
        for (float x : {1.0f, 1023.0f})
            for (float z : {1.0f, 1023.0f}) Plant(manager, *s, kTree, x, 100, z, today, 0, today);
        CHECK(AllFlora(manager).size() == 4);
        Cheat(*s, "Flora off");
        CHECK(!s->Enabled() && AllFlora(manager).empty());
        cSC4FloraOccupant* idle = Plant(manager, *s, kTree, 300, 100, 300, today - 400, 0, today - 400);
        Send(*s, kFloraMsgSimNewMonth, today);
        CHECK(idle->GetState() == 0);                                   // disabled: no monthly pass
        Cheat(*s, "Flora on");
        CHECK(s->Enabled() && AllFlora(manager).size() == 1);         // nothing restored
        Cheat(*s, "Flora PROPOGATION");
        CHECK(s->Propagation() && s->PropagationForced());
        Cheat(*s, "Flora propogation off");
        CHECK(!s->Propagation() && !s->PropagationForced());
        Cheat(*s, "Flora preserve");
        CHECK(s->Preserve());
        Cheat(*s, "Flora preserve off");
        CHECK(!s->Preserve());
        Cheat(*s, "Flora sideways");                                    // any other word: nothing
        CHECK(s->Enabled() && !s->Preserve() && !s->Propagation());
        // another cheat, or a bare "Flora", takes the exemplar-changed path with data2: a registered type reloads the types
        const std::size_t posts = host.server.posted.size();
        Send(*s, kFloraMsgCheatCode, 0x12345678, kTree);
        CHECK(host.server.posted.size() == posts + 1 && s->IsFloraTypeValid(kTree) && !s->IsFloraTypeValid(0));
        Send(*s, kFloraMsgExemplarChanged, 0, 0x77777777);              // an unknown instance: nothing
        CHECK(host.server.posted.size() == posts + 1);
        // blast 1 2 0: one center (two RandomDoubleUniform), two seeds at it (two RandomDoubleGaussian each, spread 0); the
        // first lands (DropSeed: one RandomDoubleUniform, then the orientation), the second is blocked by the first: no draw
        s->Random().Seed(777);
        cRZRandom expect(777);
        const float cx = 1024.0f * static_cast<float>(expect.RandomDoubleUniform());
        const float cz = 1024.0f * static_cast<float>(expect.RandomDoubleUniform());
        const float bx = static_cast<float>(expect.RandomDoubleGaussian() * 0.0 + static_cast<double>(cx));
        const float bz = static_cast<float>(expect.RandomDoubleGaussian() * 0.0 + static_cast<double>(cz));
        (void)expect.RandomDoubleUniform();
        const std::uint32_t blastOrientation = static_cast<std::uint32_t>(expect.RandomSint32RangeUniform(0, 4));
        (void)expect.RandomDoubleGaussian();
        (void)expect.RandomDoubleGaussian();
        CHECK(bx > 1.0f && bx < 1023.0f && bz > 1.0f && bz < 1023.0f);
        Cheat(*s, "Flora blast 1 2 0");
        CHECK(AllFlora(manager).size() == 2);
        cSC4FloraOccupant* blasted = FloraAt(manager, bx, bz);
        CHECK(blasted && blasted->GetFloraType() == kTree && blasted->GetOrientation() == blastOrientation);
        CHECK(s->Random().RandomUint32Uniform() == expect.RandomUint32Uniform());
        // PlaceFlora (conformance 18): parameter 2 lands in y and z = -1: nothing is planted
        {
            auto* params = new cGZCommandParameterSet();
            params->AddRef();
            cRZVariant v;
            v.SetValUint32(kTree);
            params->SetParameter(0, v);
            v.SetValFloat32(500.0f);
            params->SetParameter(1, v);
            v.SetValFloat32(600.0f);
            params->SetParameter(2, v);
            Send(*s, kFloraCommandPlaceFlora, 0, 0, static_cast<cIGZUnknown*>(params));
            CHECK(AllFlora(manager).size() == 2);
            params->Release();
        }
        // a building (conformance 23): box x 100..120, z 200..220 clears flora in x 98..122, z 198..222 at any height
        cSC4FloraOccupant* near = Plant(manager, *s, kTree, 99, 100, 210, today, 0, today);
        Plant(manager, *s, kTree, 121.5f, 400, 221.5f, today, 0, today);
        cSC4FloraOccupant* far = Plant(manager, *s, kTree, 97, 100, 210, today, 0, today);
        (void)near;
        BoxOccupant building(0x278128a0, {100, 0, 200}, {120, 30, 220});
        building.AddRef();
        BoxOccupant preview(0x278128a0, {100, 0, 200}, {120, 30, 220});
        preview.AddRef();
        cSCPropertyHelp::SetPropertyValue(preview.AsPropertyHolder(), 0x029244db, std::uint32_t{1});
        Send(*s, kFloraMsgInsertOccupant, 0, 0, static_cast<cISC4Occupant*>(&preview), 0x278128a0);   // a preview: ignored
        CHECK(AllFlora(manager).size() == 5);
        Send(*s, kFloraMsgInsertOccupant, 0, 0, static_cast<cISC4Occupant*>(&building), 0x278128a0);
        CHECK(AllFlora(manager).size() == 3 && FloraAt(manager, 97, 210) == far && !FloraAt(manager, 99, 210));
        Send(*s, kFloraMsgInsertOccupant, 0, 0, static_cast<cISC4Occupant*>(&building), 0xa823821e);   // a prop: nothing
        CHECK(AllFlora(manager).size() == 3);
        // terrain change (conformance 24): data1 = (10 << 16) | 20, data2 = 70 -> cells x 9..20, z 0..63
        for (cSC4FloraOccupant* f : AllFlora(manager)) manager->RemoveOccupant(f->AsOccupant(), true, 0);
        host.ground = [](float x, float) {
            if (x < 200) return 250.0f;                                 // unchanged
            if (x < 250) return 252.0f;                                 // moved by 2
            if (x < 260) return 250.0009765625f;                        // moved by 2^-10: within 0.001f
            if (x < 300) return 250.001007080078125f;                   // moved by 2^-10 + 2^-15
            return 252.0f;                                              // outside the cells
        };
        cSC4FloraOccupant* still = Plant(manager, *s, kTree, 150, 250, 100, today, 0, today);
        Plant(manager, *s, kTree, 220, 250, 100, today, 0, today);
        cSC4FloraOccupant* tiny = Plant(manager, *s, kTree, 255, 250, 100, today, 0, today);
        Plant(manager, *s, kTree, 270, 250, 100, today, 0, today);
        cSC4FloraOccupant* outside = Plant(manager, *s, kTree, 400, 250, 100, today, 0, today);
        host.terrainInitialized = false;
        Send(*s, kFloraMsgTerrainHeightsChanged, (10u << 16) | 20u, 70u);   // the terrain is not initialized: ignored
        CHECK(AllFlora(manager).size() == 5);
        host.terrainInitialized = true;
        Send(*s, kFloraMsgTerrainHeightsChanged, (10u << 16) | 20u, 70u);
        CHECK(AllFlora(manager).size() == 3 && FloraAt(manager, 150, 100) == still && FloraAt(manager, 255, 100) == tiny &&
              FloraAt(manager, 400, 100) == outside);
        float p[3];
        tiny->GetPosition(p);
        CHECK(p[1] == 250.0f);                                          // within the tolerance: not moved
        // with "preserve" the moved ground carries the trees
        Plant(manager, *s, kTree, 220, 250, 100, today, 0, today);
        Plant(manager, *s, kTree, 270, 250, 100, today, 0, today);
        Cheat(*s, "Flora preserve");
        Send(*s, kFloraMsgTerrainHeightsChanged, (10u << 16) | 20u, 70u);
        cSC4FloraOccupant* moved = FloraAt(manager, 220, 100);
        CHECK(AllFlora(manager).size() == 5 && moved);
        if (moved) { moved->GetPosition(p); CHECK(p[1] == 252.0f); }
        moved = FloraAt(manager, 270, 100);
        if (moved) { moved->GetPosition(p); CHECK(p[1] == 250.001007080078125f); }
        host.ground = nullptr;
        s->Release();
        host.occupants = nullptr;
        FreeManager(manager);
    }

    // ============================================================== obliterated city (flora.md 10.3, conformance 30)
    {
        auto* manager = NewManager(1024);
        host.occupants = manager;
        cSC4FloraSimulator* s = NewSimulator();
        s->Init();
        const float spots[3][2] = {{100, 100}, {200, 300}, {500, 700}};
        for (int i = 0; i < 3; ++i) Plant(manager, *s, kTree, spots[i][0], 100, spots[i][1], 1000 + i, i, 2000 + i);
        TempSegment file("obliterated");
        file.Open(true);
        CHECK(s->SaveObliterated(file.Persist()));
        for (cSC4FloraOccupant* f : AllFlora(manager)) CHECK(f->AsOccupant()->IsFlagSet(1));   // set again after the write
        file.Close();
        file.Open(false);
        const std::vector<std::uint8_t> count = RecordBytes(file.seg, {kObliteratedFloraRecordType, kObliteratedFloraCountGroup, 1});
        CHECK((count == std::vector<std::uint8_t>{0, 0, 3, 0, 0, 0}));
        for (std::uint32_t i = 0; i < 3; ++i) {
            const std::vector<std::uint8_t> record = RecordBytes(file.seg, {kObliteratedFloraRecordType, kObliteratedFloraOccupantGroup, i});
            CHECK(record.size() > 4 && record[0] == 0 && record[1] == 0 && record[2] == 3 && record[3] == 0);   // u16 0, then version 3
        }
        s->Release();
        host.occupants = nullptr;
        FreeManager(manager);
        // the replacement city's Init restores them (step 12)
        manager = NewManager(1024);
        host.occupants = manager;
        host.obliterated = file.Persist();
        s = NewSimulator();
        s->Init();
        host.obliterated = nullptr;
        CHECK(AllFlora(manager).size() == 3);
        for (int i = 0; i < 3; ++i) {
            cSC4FloraOccupant* f = FloraAt(manager, spots[i][0], spots[i][1]);
            CHECK(f && f->GetFloraType() == kTree && f->GetBirthDate() == 1000u + i && f->GetState() == i &&
                  f->GetLastSeedingDate() == 2000u + i && f->AsOccupant()->IsFlagSet(1));
        }
        // without a count record nothing comes back; without an occupant manager the restore answers false
        TempSegment empty("empty");
        empty.Open(true);
        empty.Close();
        empty.Open(false);
        CHECK(s->LoadObliterated(empty.Persist()) && AllFlora(manager).size() == 3);
        host.occupants = nullptr;
        CHECK(!s->LoadObliterated(file.Persist()));
        host.occupants = manager;
        s->Release();
        host.occupants = nullptr;
        FreeManager(manager);
    }

    // ============================================================== the record (flora.md 10.1, conformance 27)
    {
        cSC4FloraSimulator* s = NewSimulator();
        s->Init();
        s->Random().Seed(99);
        const std::vector<std::uint8_t> written = WrittenBytes(*s, "record");
        CHECK(written.size() == 2158);                                  // a new 64^2 city without burnt flora
        CHECK(written.size() > 2 && written[0] == 2 && written[1] == 0);
        // read back into a fresh simulator: written again byte for byte; the generator continues the same sequence
        TempSegment file("record2");
        file.Open(true);
        const cGZPersistResourceKey key{kFloraSimulatorClassId, 0x299b2d1b, 0};
        cISC4DBSegmentOStream* os = nullptr;
        file.seg->OpenOStream(key, &os, true);
        s->Write(*os);
        file.seg->CloseOStream(os);
        os->Release();
        file.Close();
        file.Open(false);
        cSC4FloraSimulator* r = NewSimulator();
        cISC4DBSegmentIStream* is = nullptr;
        CHECK(file.seg->OpenIStream(key, &is) && is);
        CHECK(r->Read(*is));
        file.seg->CloseIStream(is);
        is->Release();
        CHECK(WrittenBytes(*r, "record3") == written);
        CHECK(r->Random().RandomDoubleUniform() == s->Random().RandomDoubleUniform());
        CHECK(r->Period() == 24 && r->BlocksPerMonth() == 42 && r->Today() == 0 && r->Enabled());
        r->Release();
        s->Release();
        // version 3 is refused after its 2 bytes
        TempSegment bad("badversion");
        bad.Open(true);
        bad.seg->OpenOStream(key, &os, true);
        os->SetUint16(3);
        os->SetUint8(0);
        bad.seg->CloseOStream(os);
        os->Release();
        bad.Close();
        bad.Open(false);
        r = NewSimulator();
        CHECK(bad.seg->OpenIStream(key, &is) && is);
        CHECK(!r->Read(*is));
        std::uint8_t next = 0xff;
        CHECK(is->GetUint8(next) && next == 0);                         // nothing past the version was read
        bad.seg->CloseIStream(is);
        is->Release();
        r->Release();
    }

    // ============================================================== an original save reads and writes back byte-identical
    {
        const std::filesystem::path source = std::filesystem::path(__FILE__).parent_path() /
                                             "../../../scratch/testbed/game/Regions/Berlin/City - Spandau.sc4";
        TempSegment copy("spandau");
        std::error_code error;
        // the test bed is a local, read-only copy of the original's data (never committed): skipped where it is absent
        const bool present = std::filesystem::exists(source);
        const bool copied = present && std::filesystem::copy_file(source, copy.path, std::filesystem::copy_options::overwrite_existing, error);
        CHECK(!present || copied);
        if (copied) {
            copy.Open(false);
            const cGZPersistResourceKey key{kFloraSimulatorClassId, 0x299b2d1b, 0};
            // the class-object record: u32 size, u32 CRC, u32 object id (the segment's object table), then the object's own record
            const std::vector<std::uint8_t> stored = RecordBytes(copy.seg, key);
            CHECK(stored.size() == 12 + 8302);                          // a 128^2 city
            const std::vector<std::uint8_t> original(stored.begin() + std::min<std::size_t>(12, stored.size()), stored.end());
            cSC4FloraSimulator* r = NewSimulator();
            cISC4DBSegmentIStream* is = nullptr;
            CHECK(copy.seg->OpenIStream(key, &is) && is);
            if (is) {
                std::uint32_t header[3] = {};
                for (std::uint32_t& word : header) is->GetUint32(word);
                CHECK(header[0] == 8314);
                CHECK(r->Read(*is));
                copy.seg->CloseIStream(is);
                is->Release();
            }
            CHECK(r->Enabled() && !r->Propagation() && r->Period() == 24 && r->BlocksPerMonth() == 170 && r->Today() == 2452154);
            CHECK(WrittenBytes(*r, "spandau_out") == original);
            r->Release();
            copy.Close();
        }
    }

    for (auto& entry : host.flora) entry.second->Release();
    host.tuning->Release();
    cSC4FloraSimulator::StaticShutdown();
    std::printf(failures ? "FAILED\n" : "sc4_floraSimulator: all passed\n");
    return failures ? 1 : 0;
}
