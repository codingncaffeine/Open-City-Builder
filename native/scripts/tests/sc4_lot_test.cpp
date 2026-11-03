#include <cstdio>
#include <cmath>
#include "rz/PortMemoryStreams.h"
#include "sc4/Lot.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Config final : public cISC4LotConfigurationRef {
public:
    explicit Config(std::uint32_t id) : id_(id) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return --refs_; }
    std::uint32_t GetID() const override { return id_; }
    std::uint32_t id_, refs_ = 0;
};
class Host final : public ILotHost {
public:
    LotBuildingProfile profile;
    Config config{0x777};
    int day = 100;
    int roadCalls = 0;
    bool road = true;
    int messages = 0;
    std::uint32_t lastMessage = 0;
    std::uint8_t ZoneTypeAt(int x, int z) override { return static_cast<std::uint8_t>(x + z); }
    std::int32_t SimulatorDay() override { return day; }
    float TerrainAltitude(float, float) override { return 270.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t t) override { return t ? &profile : nullptr; }
    float SlopeOf(const cSC4Lot&) override { return 1.5f; }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override { return purpose * 0x100 + wealth; }
    void OnDeveloperLotAbandoned(std::uint16_t) override {}
    std::uint8_t LandValueAt(int a, int b) override { return static_cast<std::uint8_t>(a + b); }
    std::uint32_t RandomPercent() override { return 42; }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t id) override { return id == 0x777 ? &config : nullptr; }
    void PostLotMessage(std::uint32_t m, const cSC4Lot&, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) override { ++messages; lastMessage = m; }
    void SetLotHighlight(int, int, bool, float) override {}
    std::int32_t TrafficJobs(const cSC4Lot&, int) override { return 0; }
    void SetTrafficJobs(const cSC4Lot&, int, std::int32_t) override {}
    float ResidentialTravelFactor(int, int) override { return 1.0f; }
    float TravelVolumeFactor() override { return 1.0f; }
    void TravelDistribution(int, float out[3]) override { out[0] = 0.5f; out[1] = 0.5f; out[2] = 0; }
    void JobsPerOccupant(float out[4], int) override { out[1] = 1; out[2] = 2; out[3] = 3; }
    void ExemplarJobs(const cSC4Lot&, float*) override {}
    bool ExemplarProperty(const cSC4Lot&, std::uint32_t, std::int32_t&) override { return false; }
    bool PowerRequirementExceeded(const cSC4Lot&, const LotBuildingProfile&, bool& available) override { available = false; return false; }
    bool TrafficHasJobAccess(const cSC4Lot&, int) override { return false; }
    void NeighborLots(const cSC4Lot&, int, std::vector<cSC4Lot*>&) override {}
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>&) override {}
    bool RoadAccessCells(const cSC4Lot&, int) override { ++roadCalls; return road; }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot&) override { return nullptr; }
    void WriteSerializable(cIGZOStream& s, cIGZUnknown* o) override { s.SetUint8(o ? 1 : 0); }
    cIGZUnknown* ReadSerializable(cIGZIStream& s, std::uint32_t) override { std::uint8_t b = 0; s.GetUint8(b); return nullptr; }
};
}
int main() {
    Host host;
    host.profile.purpose = 2;
    host.profile.wealth = 2;
    host.profile.capacities = {{{0x201, 10}, {0x202, 20}}, {{0x201, 30}}};
    host.profile.allowPartialCapacity = true;
    SetLotHost(&host);

    // geometry and facing
    auto* lotp = new cSC4Lot(10, 20, 3, 2, 0);
    cSC4Lot& lot = *lotp;
    std::int32_t r[4];
    lot.GetBoundingRect(r);
    CHECK(r[0] == 10 && r[1] == 20 && r[2] == 12 && r[3] == 21);
    int cx, cz;
    lot.GetLocation(cx, cz);
    std::uint8_t sx, sz;
    lot.GetSize(sx, sz);
    CHECK(cx == 12 && cz == 20 && sx == 3 && sz == 2 && lot.GetZoneType() == 32);
    cSC4Lot rot(10, 20, 3, 2, 1);
    rot.GetLocation(cx, cz);
    rot.GetSize(sx, sz);
    CHECK(cx == 12 && cz == 21 && sx == 2 && sz == 3);
    float c[3];
    lot.GetCenter(c);
    CHECK(c[0] == (12 * 16 + 16 + 160) * 0.5f && c[2] == (21 * 16 + 16 + 320) * 0.5f);

    // init: altitude from terrain, slope from the sim, random percent once
    lot.AddRef();
    CHECK(lot.Init() && lot.IsInitialized() && lot.GetAltitude() == 270.0f && lot.GetSlope() == 1.5f);
    lot.SetAltitude(270.0f + 0.0005f);
    CHECK(lot.GetAltitude() == 270.0f);

    // building + state machine
    lot.SetBuildingType(0x1234);
    lot.SetState(kLotStateOccupied, 2, 0);
    CHECK(lot.GetState() == kLotStateOccupied && lot.GetOccupantWealth() == 2 && host.messages == 1 &&
          host.lastMessage == kMessageLotStateChanged && lot.GetPopulations().size() == 2);
    CHECK(lot.GetCapacity(0x201, false) == 10 && lot.GetCapacity(0x202, true) == 20);
    lot.SetState(kLotStateLivable, 0, 0);
    CHECK(lot.IsInLivableState() && lot.HasCapacity() && lot.GetDaysSinceLastStateChange() == 0);
    // becoming livable fills the top developer type to capacity
    CHECK(lot.GetPopulation(0x202) == 20 && lot.GetAvailableCapacity(0x201, false) == 0);
    CHECK(lot.SetPopulation(0x202, 0) && lot.GetPopulation(0x202) == 0);
    CHECK(lot.SetPopulation(0x201, 4) && lot.GetPopulation(0x201) == 4);
    CHECK(lot.GetAvailableCapacity(0x201, false) == 10);
    CHECK(lot.SetPopulation(0x202, 5));
    // 4/10 of the other developer type consumed -> 60% of 20 available
    CHECK(lot.GetAvailableCapacity(0x202, false) == 12);
    CHECK(lot.SetPopulation(0x202, 99) && lot.GetPopulation(0x202) == 12);
    std::map<std::uint32_t, float> by;
    lot.GetOccupancyByType(by);
    CHECK(by.size() == 2);
    host.day = 130;
    CHECK(lot.GetDaysSinceLastStateChange() == 30);
    lot.SetCondition(2);
    CHECK(host.lastMessage == kMessageLotConditionChanged);

    // flags
    lot.SetHistorical(true);
    lot.SetHasPower(true);
    CHECK(lot.IsHistorical() && lot.GetHasPower() && !lot.GetNeedsPower());
    lot.SetFlags(1);
    CHECK(lot.GetFlags() == 1);

    // road access caches
    CHECK(lot.HasRoadAccess() && host.roadCalls == 1);
    CHECK(lot.HasRoadAccess() && host.roadCalls == 1);
    host.road = false;
    lot.UpdateRoadAccess();
    CHECK(!lot.HasRoadAccess());

    // jobs
    float jobs[4];
    lot.GetJobs(jobs);
    CHECK(lot.GetOccupantWealth() == 2 && jobs[1] == 12 && jobs[2] == 24 && jobs[3] == 36);

    // travel
    auto* otherp = new cSC4Lot(1, 1, 1, 1, 0);
    cSC4Lot& other = *otherp;
    other.AddRef();
    other.Init();
    other.AddTravelPopulation(lot);
    CHECK(other.GetTravelPopulation(0x201) == 4);

    // trips
    lot.SetState(kLotStateLivable, 0, 0);
    lot.UpdateTrafficVolume();

    // round trip
    PortMemoryOStream os;
    CHECK(lot.Write(os));
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    auto* backp = new cSC4Lot();
    cSC4Lot& back = *backp;
    back.AddRef();
    CHECK(back.Read(is));
    std::int32_t r2[4];
    back.GetBoundingRect(r2);
    CHECK(r2[0] == 10 && r2[3] == 21 && back.GetState() == kLotStateLivable && back.GetPopulation(0x201) == 4 &&
          back.GetBuildingType(false) == 0x1234 && back.GetFacing() == 0 && back.GetFlags() == 1);
    CHECK(back.GetAltitude() == 270.0f && back.GetCondition() == 2);
    back.Release();
    other.Release();
    lot.Release();
    std::puts(failures ? "sc4_lot: FAILED" : "sc4_lot ok");
    return failures != 0;
}
