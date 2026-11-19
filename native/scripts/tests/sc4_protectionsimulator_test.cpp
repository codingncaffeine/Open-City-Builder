#include <cmath>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "gz/cIGZPersistDBSerialRecord.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZString.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"
#include "sc4/ProtectionSimulator.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

class FakeCity : public ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return 16; }
    std::int32_t CitySizeZInCells() const override { return 16; }
    float CellSizeXMeters() const override { return 16.0f; }
    float CellSizeZMeters() const override { return 16.0f; }
};

struct Station : IProtectionStation {
    float x, z;
    std::uint32_t type = kProtectionBuildingOccupantType;
    std::uint32_t bits = 4, offset = 0, mask = 0;
    int available = 2;
    Station(float px, float pz) : x(px), z(pz) {}
    std::uint32_t OccupantType() override { return type; }
    void Position(float p[3]) override { p[0] = x; p[1] = 0; p[2] = z; }
    cIGZUnknown* object = nullptr;
    cIGZUnknown* Object() override { return object; }
};
// A station as the city record references it (a test class): one marker word.
constexpr std::uint32_t kTestStationClass = 0x7e570002;
struct StationObject final : cIGZSerializable {
    std::uint32_t refs = 0, marker = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZSerializable) { *out = static_cast<cIGZSerializable*>(this); AddRef(); return true; }
        *out = nullptr;
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { if (--refs == 0) { delete this; return 0; } return refs; }
    bool Write(cIGZOStream& s) const override { s.SetUint32(marker); return !s.GetError(); }
    bool Read(cIGZIStream& s) override { s.GetUint32(marker); return !s.GetError(); }
    std::uint32_t GetGZCLSID() const override { return kTestStationClass; }
};
struct RecordFactory final : ISerializableFactory {
    cIGZSerializable* CreateSerializable(std::uint32_t classId) override {
        cIGZSerializable* out = nullptr;
        if (classId == 0x49b9e602) out = static_cast<cIGZSerializable*>(new cSC4SimGrid<std::uint8_t>());
        else if (classId == 0x49b9e605) out = static_cast<cIGZSerializable*>(new cSC4SimGrid<std::int16_t>());
        else if (classId == kTestStationClass) out = new StationObject();
        if (out) out->AddRef();
        return out;
    }
};
cSC4DBSegment* OpenSegment(const std::string& path, bool write) {
    cSC4DBSegment* seg = new cSC4DBSegment();
    seg->AddRef();
    seg->Init();
    cRZString p(path.c_str());
    seg->SetPath(p);
    if (!(write ? seg->Open(false, true) : seg->Open(true, false))) { seg->Release(); return nullptr; }
    return seg;
}
void CloseSegment(cSC4DBSegment* seg) { seg->Close(); seg->Release(); }
std::vector<std::uint8_t> RecordBytes(cSC4DBSegment* seg, const cGZPersistResourceKey& key) {
    std::vector<std::uint8_t> out;
    cISC4DBSegmentIStream* is = nullptr;
    if (!seg->OpenIStream(key, &is)) return out;
    out.resize(is->GetRecord()->GetSize());
    for (std::uint8_t& b : out) is->GetUint8(b);
    seg->CloseIStream(is);
    is->Release();
    return out;
}
std::uint32_t U32At(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | (at + i < b.size() ? b[at + i] : 0);
    return v;
}
// The base record's layout: where the two grid references start (after the department and the iterator) and where the stations do.
std::size_t GridRefsAt(const std::vector<std::uint8_t>& b) { return 2 + 8 + 39 + 2 * std::size_t(U32At(b, 2 + 8 + 3 + 16 + 4)); }
// The record with the serializer's object ids zeroed (it numbers the objects per save).
std::vector<std::uint8_t> WithoutObjectIds(std::vector<std::uint8_t> b) {
    const auto zero = [&](std::size_t at) { for (std::size_t i = at; i < at + 8 && i < b.size(); ++i) b[i] = 0; };
    std::size_t at = GridRefsAt(b);
    zero(at);
    zero(at + 12);
    at += 24;
    const std::uint32_t n = U32At(b, at);
    at += 4;
    for (std::uint32_t i = 0; i < n && at < b.size(); ++i, at += 12) zero(at);
    return b;
}

struct Host : IProtectionHost {
    int passes = 0, cells = 0, taken = 0;
    bool above = true;
    int avail = 3;
    bool GetStationEffect(IProtectionStation* s, ProtectionEffect& e) override {
        auto* st = static_cast<Station*>(s);
        e.x = st->x; e.z = st->z; e.radius = 64.0f; e.peak = 100; e.edge = 20;
        return true;
    }
    void OnAdjustedCell(int, int, int) override { ++cells; }
    void OnPassFinished() override { ++passes; }
    void SetStationDispatchMask(IProtectionStation* s, std::uint32_t m) override { static_cast<Station*>(s)->mask = m; }
    void SetStationDispatchOffset(IProtectionStation* s, std::uint32_t o) override { static_cast<Station*>(s)->offset = o; }
    void GetStationInfo(IProtectionStation* s, int* a, std::uint32_t* o, std::uint32_t* b) override {
        auto* st = static_cast<Station*>(s);
        if (a) *a = st->available;
        if (o) *o = st->offset;
        if (b) *b = st->bits;
    }
    int MinimumDispatchesForService() override { return 2; }
    std::uint32_t DispatchPurpose() override { return 7; }
    int AvailableDispatchCount(std::uint32_t) override { return avail; }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t) override { return {}; }
    bool GetDispatch(std::uint32_t, ProtectionDispatchInfo&) override { return false; }
    bool LotAllowsDispatch(IProtectionStation*) override { return true; }
    bool IsAboveWater(float, float) override { return above; }
    bool IsZonedAt(int, int) override { return true; }
    void RegisterStationProperty(IProtectionStation*, bool) override {}
    void GetDispatchEffect(const ProtectionDispatchInfo&, ProtectionEffect&) override {}
    void TakeStationDispatch(IProtectionStation*) override { ++taken; }
};

}  // namespace

int main() {
    FakeCity city;
    SetSimGridCity(&city);

    // radius effect: linear falloff, saturating
    std::vector<std::vector<std::int16_t>> grid(8, std::vector<std::int16_t>(8, 0));
    AddRadiusEffect(grid, 8, 8, 4, 4, 3.0f, 90.0f, 0.0f);
    CHECK(grid[4][4] == 90);
    CHECK(grid[5][4] == 60);           // 90 - 1 * 30
    CHECK(grid[4][7] == 0 && grid[7][4] == 0);   // distance 3 is not < radius
    CHECK(grid[0][0] == 0);
    for (auto& c : grid) for (auto& v : c) v = 32760;
    AddRadiusEffect(grid, 8, 8, 4, 4, 3.0f, 90.0f, 0.0f);
    CHECK(grid[4][4] == 32767);

    Host host;
    cSC4ProtectionSimulator sim(&host, 1, 4);
    sim.CreateGrids(16, 16);
    sim.zoneColumns.assign(16, std::vector<std::int8_t>(16, 1));
    CHECK(sim.cellsPerDay == 64);      // 16 * 16 / 4
    sim.scale = 0.5f;

    Station a(32.0f, 32.0f), b(160.0f, 160.0f);
    sim.DoOccupantInserted(&a);
    CHECK(sim.Stations().size() == 1 && a.offset == 1 && a.mask == 0xf && sim.dispatchBitsInUse == 5);
    sim.DoOccupantInserted(&b);
    CHECK(b.offset == 5 && sim.dispatchBitsInUse == 9);     // first free run above station a's bits 1..4
    CHECK(sim.GetMaxPossibleDispatchCount() == 8);

    const int cx = static_cast<int>(32.0f * sim.UnadjustedGrid()->GetOneOverTractWidthX());
    CHECK(sim.UnadjustedGrid()->GetTractValue(cx, cx) == 100);
    CHECK(sim.AdjustedGrid()->GetTractValue(cx, cx) == 50);  // rounded unadjusted * scale
    CHECK(sim.AdjustedGrid()->GetTractValue(15, 0) == 0);

    // adjusting an effect scales radius and strengths (truncated)
    ProtectionEffect e;
    e.radius = 10; e.peak = 7; e.edge = 3;
    cSC4ProtectionSimulator::AdjustProtectionEffect(e, 0.5f);
    CHECK(e.radius == 5.0f && e.peak == 3 && e.edge == 1);

    // dispatching
    bool enough = false;
    CHECK(sim.CanDispatch(10, 10, &enough) && enough);
    host.avail = 1;
    CHECK(sim.CanDispatch(10, 10, &enough) && !enough);
    host.above = false;
    CHECK(!sim.CanDispatch(10, 10, &enough));
    std::uint32_t id = 99;
    CHECK(sim.DispatchFromNearestStation(150, 150, &id) == &b && id == 0 && host.taken == 1);
    b.available = 0;
    CHECK(sim.DispatchFromNearestStation(150, 150, &id) == &a);

    // daily sweep: 4 days cover the whole 16x16 city -> one pass finishes on the next tick
    for (int i = 0; i < 4; ++i) sim.DoDailySimulation();
    CHECK(host.cells == 256 && host.passes == 1);   // the very first tick finds the unreset walk empty and closes an empty pass
    sim.DoDailySimulation();
    CHECK(host.passes == 2);
    CHECK(sim.AverageProtection() > 0.0f && sim.CoverageShare() > 0.0f && sim.CoverageShare() <= 1.0f);

    // removal clears the area
    sim.DoOccupantRemoved(&a);
    CHECK(sim.Stations().size() == 1 && sim.UnadjustedGrid()->GetTractValue(cx, cx) == 0);
    a.type = 0;
    sim.DoOccupantInserted(&a);        // wrong occupant type ignored
    CHECK(sim.Stations().size() == 1);

    // The city-file record of a city that did not come from a file (i386 0x0007d392, spec 11): version 3, a null department, the walk,
    // the adjusted (uint8) and unadjusted (int16) grids, the stations the host hands out as objects, the counters; read back whole by
    // the city-file reader and saved again unchanged (but for the serializer's object ids) once it came from a file.
    {
        RecordFactory factory;
        SetSerializableFactory(&factory);
        auto* stationObject = new StationObject();
        stationObject->AddRef();
        stationObject->marker = 0xb0b;
        b.object = stationObject;                                           // a is gone, b has an object; c has none
        Station c(96.0f, 96.0f);
        sim.DoOccupantInserted(&c);
        const cGZPersistResourceKey key{0x8990c0f0, 0x50505050, 1};
        const std::string path = "/tmp/sc4_protection_fresh_" + std::to_string(getpid()) + ".sc4";
        const std::string again = "/tmp/sc4_protection_resave_" + std::to_string(getpid()) + ".sc4";
        std::vector<std::uint8_t> first, second;
        if (cSC4DBSegment* seg = OpenSegment(path, true)) {
            cISC4DBSegmentOStream* os = nullptr;
            CHECK(seg->OpenOStream(key, &os, true));
            CHECK(sim.Write(*os));
            CHECK(seg->CloseOStream(os));
            os->Release();
            CloseSegment(seg);
        }
        Host host2;
        cSC4ProtectionSimulator back(&host2, 1, 4);
        if (cSC4DBSegment* seg = OpenSegment(path, false)) {
            CHECK(seg->LoadClassObjects(0x48));
            first = RecordBytes(seg, key);
            cISC4DBSegmentIStream* is = nullptr;
            CHECK(seg->OpenIStream(key, &is));
            CHECK(back.Read(*is));
            CHECK(!is->GetError() && is->GetRecord()->GetPosition() == is->GetRecord()->GetSize());
            seg->CloseIStream(is);
            is->Release();
            CloseSegment(seg);
        }
        CHECK(first.size() > 60 && first[0] == 3 && first[1] == 0);
        bool nullDepartment = true;
        for (std::size_t i = 2; i < 10 && i < first.size(); ++i) nullDepartment = nullDepartment && first[i] == 0;
        CHECK(nullDepartment);
        const std::size_t grids = GridRefsAt(first);
        CHECK(U32At(first, grids + 8) == 0x49b9e602 && U32At(first, grids + 20) == 0x49b9e605);   // adjusted, then unadjusted
        CHECK(U32At(first, grids + 24) == 1 && U32At(first, grids + 28 + 8) == kTestStationClass);   // station b only
        CHECK(first.size() == grids + 24 + 4 + 12 + 3 * 4 + 2 * 4 + 1 + 4 + 4);
        CHECK(back.AverageProtection() == sim.AverageProtection() && back.CoverageShare() == sim.CoverageShare() &&
              back.dispatchBitsInUse == sim.dispatchBitsInUse && back.FileStationCount() == 1);
        if (cSC4DBSegment* seg = OpenSegment(again, true)) {
            cISC4DBSegmentOStream* os = nullptr;
            CHECK(seg->OpenOStream(key, &os, true));
            CHECK(back.Write(*os));
            CHECK(seg->CloseOStream(os));
            os->Release();
            CloseSegment(seg);
        }
        if (cSC4DBSegment* seg = OpenSegment(again, false)) {
            second = RecordBytes(seg, key);
            CloseSegment(seg);
        }
        CHECK(!first.empty() && WithoutObjectIds(second) == WithoutObjectIds(first));
        unlink(path.c_str());
        unlink(again.c_str());
        SetSerializableFactory(nullptr);
        b.object = nullptr;
        stationObject->Release();
    }

    std::printf(failures ? "FAILED\n" : "sc4_protectionsimulator: all passed\n");
    return failures ? 1 : 0;
}
