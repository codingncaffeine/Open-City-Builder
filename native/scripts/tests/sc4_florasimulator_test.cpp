#include <cstdio>
#include <string>
#include <unistd.h>
#include <unordered_set>

#include "sc4/DBSegment.h"
#include "sc4/FloraSimulator.h"
#include "sc4/SCPropertyHelp.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Host final : public IFloraSimulatorHost {
public:
    float sea = 100.0f, height = 100.0f;
    int terrainClass = 7;
    int posted = 0;
    cISCResExemplar* LoadExemplar(const cGZPersistResourceKey&) override { return nullptr; }
    void EnumerateFloraResources(void (*)(const cGZPersistResourceKey&, void*), void*) override {}
    void PostFloraTypesUpdated() override { ++posted; }
    float SeaLevel() override { return sea; }
    float TerrainHeight(float, float) override { return height; }
    int TerrainClass(float, float) override { return terrainClass; }
    // planting a new flora occupant is not exercised here: no occupant manager, flat open ground, a 1 km city
    cISC4OccupantManager* OccupantManager() override { return nullptr; }
    float GroundHeight(float, float, bool& onLot) override { onLot = false; return height; }
    float TerrainNormalY(float, float) override { return 1.0f; }
    void CitySizeMeters(float& x, float& z) override { x = z = 1024.0f; }
    std::uint32_t SimDate() override { return 0; }
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
struct Accept : cISC4FloraTypeFilter {
    bool Accepts(std::uint32_t t) override { return t == 0x20; }
};
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

    // elevation + suitability (above sea level 3 -> type 0x10 at [3][7]; below sea level 2 for the aquatic type)
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
    CHECK(sim.GetElevationIndex(0, 0, false) == 2);                     // 1.5 rounds to even

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
            CHECK(SC4WriteGZSerializable(*os, static_cast<cIGZUnknown*>(&sim)));
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
            static_cast<cIGZSerializable*>(raw)->Release();
            CHECK(seg->CloseIStream(is)); is->Release();
            CHECK(seg->Close()); seg->Release();
        }
        unlink(path.c_str());
        SetSerializableFactory(nullptr);
    }

    CHECK(cSC4FloraSimulator::StaticShutdown());
    CHECK(!sim.IsFloraTypeValid(0x10));
    a->Release();
    {
        const int before = host.posted;
        cSC4FloraSimulator::PostFloraTypesUpdatedMessage();
        CHECK(host.posted == before + 1);
    }
    b->Release();
    std::printf(failures ? "FAILED\n" : "sc4_floraSimulator: all passed\n");
    return failures ? 1 : 0;
}
