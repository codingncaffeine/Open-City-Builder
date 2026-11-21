// cSC4NetworkConstructionCrew: the hand-off of a segment's pieces, the short-line shortcut, a crew walking a line piece by
// piece (each piece usable once the crew leaves it), the lead vehicle on the pieces' paths, a bulldozed piece under the crew
// and the reset message.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <vector>

#include <SDL.h>   // the test stub: the timers read g_ticks
#include "gz/cIGZMessage2.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/NetworkConstructionCrew.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTool.h"
#include "sc4/PathInfo.h"

static int failures = 0;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);           \
            ++failures;                                                \
        }                                                              \
    } while (0)

class cIGZPersistResourceManager;
namespace SL {
cIGZPersistResourceManager* spRM = nullptr;
}

using namespace sc4;

namespace {

struct FakeVehicle final : INetworkCrewVehicle {
    int refs = 0;
    std::uint32_t model = 0;
    float position[3] = {0, 0, 0};
    std::uint32_t pathKey = 0xffffffffu;
    cSC4NetworkOccupant* pathPiece = nullptr;
    int moves = 0;
    bool removed = false;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetAutomataFlag(std::uint32_t, bool) override {}
    void SetOpacity(float, float) override {}
    void GetPosition(float* xyz) override { for (int i = 0; i < 3; ++i) xyz[i] = position[i]; }
    void GetOrientation(float* xyz) override { xyz[0] = 1; xyz[1] = 0; xyz[2] = 0; }
    void MoveTo(const float* p, const float*) override {
        for (int i = 0; i < 3; ++i) position[i] = p[i];
        ++moves;
    }
    void RemoveCallback(cIGZMessageTarget2*) override {}
    void SetPath(cSC4NetworkOccupant* piece, std::uint32_t key, bool) override {
        pathPiece = piece;
        pathKey = key;
    }
    void SetPathOffset(float) override {}
    bool InterpolateAlongPath(float distance, const float* from, float* out) override {   // straight along +x
        out[0] = from[0] + distance;
        out[1] = from[1];
        out[2] = from[2];
        return true;
    }
};

struct FakeWorker final : INetworkCrewWorker {
    int refs = 0;
    bool removed = false;
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetAutomataFlag(std::uint32_t, bool) override {}
    void SetOrientation(const float*) override {}
    int ModelFrameCount() override { return -1; }
    void SetModelFrame(int) override {}
};

struct FakeEffect final : INetworkCrewEffect {
    int* alive;
    explicit FakeEffect(int* counter) : alive(counter) { ++*alive; }
    void Release() override {
        --*alive;
        delete this;
    }
    void SetSourcePosition(const float*) override {}
    void Start(int) override {}
    void Stop(int) override {}
};

struct FakeHost final : INetworkConstructionCrewHost {
    std::vector<cSC4NetworkConstructionCrew*> registered;
    std::vector<cIGZMessageTarget2*> tickables;
    bool vehicles = false;
    std::vector<FakeVehicle*> made;
    std::vector<FakeWorker*> workers;
    int effects = 0;
    cRZRandomSuper random{7};
    void RegisterCrew(cSC4NetworkConstructionCrew* crew) override {
        registered.push_back(crew);
        crew->AddRef();
    }
    void UnregisterCrew(cSC4NetworkConstructionCrew* crew) override {
        for (auto it = registered.begin(); it != registered.end(); ++it) {
            if (*it != crew) continue;
            registered.erase(it);
            crew->Release();
            return;
        }
    }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    bool AddTickable(cIGZMessageTarget2* target, std::uint32_t) override {
        tickables.push_back(target);
        return true;
    }
    bool RemoveTickable(cIGZMessageTarget2* target, std::uint32_t) override {
        for (auto it = tickables.begin(); it != tickables.end(); ++it)
            if (*it == target) { tickables.erase(it); return true; }
        return false;
    }
    bool SoundSystemPresent() override { return false; }
    void PostConstructionMessage(std::uint32_t, cISC4Occupant*, cIGZMessageTarget2*, std::uint32_t, std::uint32_t) override {}
    int CurrentViewLevel() override { return 1; }
    bool VehicleManagerPresent() override { return vehicles; }
    INetworkCrewVehicle* CreateVehicle(std::uint32_t group, const float* position, std::uint32_t flags, const std::uint32_t key[3],
                                       std::uint8_t) override {
        if (group != 0x4002 || flags != 0x3b000) return nullptr;
        auto* v = new FakeVehicle();
        v->model = key[2];
        for (int i = 0; i < 3; ++i) v->position[i] = position[i];
        made.push_back(v);
        return v;
    }
    void RemoveVehicle(INetworkCrewVehicle* vehicle) override { static_cast<FakeVehicle*>(vehicle)->removed = true; }
    INetworkCrewWorker* CreateWorker(std::uint32_t group, const float*, std::uint32_t) override {
        if (group != 0x4101) return nullptr;
        auto* w = new FakeWorker();
        workers.push_back(w);
        return w;
    }
    void RemoveWorker(INetworkCrewWorker* worker) override { static_cast<FakeWorker*>(worker)->removed = true; }
    INetworkCrewEffect* CreateEffect(const char*) override { return new FakeEffect(&effects); }
    bool CellHasWater(std::uint32_t, std::uint32_t, bool& terrain) override {
        terrain = true;
        return false;
    }
    cRZRandomSuper* Random() override { return &random; }
    ~FakeHost() override {
        for (FakeVehicle* v : made) delete v;
        for (FakeWorker* w : workers) delete w;
    }
};

// One straight car path across a tile (west side 0 to east side 2, and back), in tile coordinates.
struct FakePathHost final : INetworkOccupantPathHost {
    std::uint32_t MapPath(std::uint32_t id) override { return id; }
    cISC4PathInfo* LoadPathInfo(const cGZPersistResourceKey&) override {
        auto* info = new cSC4PathInfo();
        info->AddRef();
        tPath east;
        east.points = {cS3DVector3{-8, 0, 0}, cS3DVector3{8, 0, 0}};
        east.length = 16;
        tPath west;
        west.points = {cS3DVector3{8, 0, 0}, cS3DVector3{-8, 0, 0}};
        west.length = 16;
        (*info->GetPathMap())[MakePathKey(1, 0, 2)] = east;
        (*info->GetPathMap())[MakePathKey(1, 2, 0)] = west;
        return info;
    }
};

cSC4NetworkOccupant* MakePiece(std::uint32_t x, std::uint32_t z) {
    auto* piece = new cSC4NetworkOccupant();
    piece->AddRef();
    const float fx = static_cast<float>(x) * 16.0f, fz = static_cast<float>(z) * 16.0f;
    const float corners[4][3] = {{fx, 0, fz}, {fx, 0, fz + 16}, {fx + 16, 0, fz + 16}, {fx + 16, 0, fz}};
    piece->Init(corners);
    piece->SetNetworkFlag(0x4000);   // inserted translucent, not usable, as InsertOccupantsForSolvedCells leaves a placed piece
    return piece;
}

// A one-cell-per-step line along x from (x0, z) to (x1, z): its pieces (one reference each), steps and cells.
struct Line {
    std::vector<cSC4NetworkOccupant*> pieces;
    std::vector<tDraggedStep> steps;
    std::vector<SC4PointU> cells;
    Line(std::uint32_t x0, std::uint32_t x1, std::uint32_t z) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            pieces.push_back(MakePiece(x, z));
            const std::uint32_t index = static_cast<std::uint32_t>(cells.size());
            cells.push_back(SC4PointU{x, z});
            steps.push_back(tDraggedStep{index, index, 0});
        }
    }
    std::list<cSC4NetworkOccupant*> Placed() const {   // PlaceNetwork's list: one more reference per piece
        std::list<cSC4NetworkOccupant*> out;
        for (cSC4NetworkOccupant* piece : pieces) {
            piece->AddRef();
            out.push_back(piece);
        }
        return out;
    }
    int Usable() const {
        int n = 0;
        for (cSC4NetworkOccupant* piece : pieces) n += piece->IsUsable() ? 1 : 0;
        return n;
    }
    ~Line() {
        for (cSC4NetworkOccupant* piece : pieces) piece->Release();
    }
};

// What PlaceNetwork does with a solved segment: the crew takes the pieces, the rest are usable, the list's references go.
void HandOff(cSC4NetworkConstructionCrew* crew, Line& line) {
    std::list<cSC4NetworkOccupant*> placed = line.Placed();
    crew->AddNetworkSegment(placed, line.steps, line.cells);
    cSC4NetworkConstructionCrew::MarkOccupantsUsable(placed, true);
}

cSC4NetworkConstructionCrew* NewCrew(float seconds) {
    auto* crew = new cSC4NetworkConstructionCrew();
    crew->AddRef();
    crew->Init(1, seconds, 32.0f, 0x107c0000, 0x107d0000, 1);
    return crew;
}

std::uint32_t RefsOf(cSC4NetworkOccupant* piece) {
    piece->AddRef();
    return piece->Release();
}

void Pause(int ms) { g_ticks += static_cast<std::uint32_t>(ms); }   // the stub clock

}  // namespace

// The bridge specialization script loader the network tool links: no resources here.
namespace sc4 {
bool SC4GetResourceAsFile(const cGZPersistResourceKey&, cIGZFile** out, bool) {
    *out = nullptr;
    return false;
}
}  // namespace sc4

int main() {
    g_ticks = 1000;
    // ---- a line of fewer than five steps is usable at once and the crew goes away ---------------------------------------
    {
        FakeHost host;
        SetNetworkConstructionCrewHost(&host);
        Line line(0, 3, 0);
        const std::uint32_t baseline = RefsOf(line.pieces[0]);
        cSC4NetworkConstructionCrew* crew = NewCrew(0.02f);
        CHECK(host.registered.size() == 1);
        HandOff(crew, line);
        CHECK(crew->PendingPieces().size() == 4);
        CHECK(line.Usable() == 0);
        crew->BeginConstruction();
        CHECK(line.Usable() == 4);
        CHECK(host.registered.empty());
        CHECK(host.tickables.empty());
        crew->Release();
        CHECK(RefsOf(line.pieces[0]) == RefsOf(line.pieces[1]) && RefsOf(line.pieces[0]) == baseline);   // every reference returned
        CHECK((line.pieces[0]->GetNetworkFlag() & 0x4000) == 0);   // opaque in the normal view
    }

    // ---- AddNetworkSegment: pieces off the line's cells are usable at once, the others wait -------------------------
    {
        FakeHost host;
        SetNetworkConstructionCrewHost(&host);
        Line line(0, 5, 0);
        cSC4NetworkOccupant* stray = MakePiece(9, 9);
        cSC4NetworkConstructionCrew* crew = NewCrew(0.02f);
        std::list<cSC4NetworkOccupant*> placed = line.Placed();
        stray->AddRef();
        placed.push_back(stray);
        crew->AddNetworkSegment(placed, line.steps, line.cells);
        CHECK(placed.size() == 1 && placed.front() == stray);
        CHECK(stray->IsUsable());
        CHECK(crew->PendingPieces().size() == 6);
        CHECK(crew->StepCount() == 6);
        cSC4NetworkConstructionCrew::MarkOccupantsUsable(placed, true);
        // A second hand-off of the same cells adds no step and replaces the waiting pieces.
        Line again(0, 5, 0);
        HandOff(crew, again);
        CHECK(crew->StepCount() == 6);
        CHECK(crew->PendingPieces().size() == 6);
        CHECK(crew->PendingPieces().front() == again.pieces[0]);
        crew->Shutdown();   // the waiting pieces are handed over as usable
        CHECK(again.Usable() == 6);
        crew->Release();
        stray->Release();
    }

    // ---- a crew walks the line: each piece becomes usable when the crew leaves it, then the crew ends -----------------
    {
        FakeHost host;
        SetNetworkConstructionCrewHost(&host);
        Line line(0, 7, 0);
        const std::uint32_t baseline = RefsOf(line.pieces[0]);
        cSC4NetworkConstructionCrew* crew = NewCrew(0.02f);
        HandOff(crew, line);
        crew->BeginConstruction();
        crew->Release();   // PlaceAllSegments' reference: the registration keeps the crew
        CHECK(host.registered.size() == 1);
        CHECK(host.tickables.size() == 1);
        CHECK(crew->Crews().size() == 1);
        CHECK(crew->Crews()[0].count == 8);
        CHECK(crew->Crews()[0].current == line.pieces[0]);
        CHECK(host.effects == 1);
        CHECK(line.Usable() == 0);
        CHECK(cSC4NetworkConstructionCrew::GetNetworkOccupantCrewCount(line.pieces[0]) == 1);
        crew->DoAnimationTick(16000);   // the piece's time is not up yet
        CHECK(line.Usable() == 0);
        int order = 0;
        bool inOrder = true, partial = false;
        for (int i = 0; i < 40 && !host.registered.empty(); ++i) {
            Pause(25);
            host.registered[0]->DoAnimationTick(16000);
            const int usable = line.Usable();
            for (int k = 0; k < usable; ++k) inOrder = inOrder && line.pieces[k]->IsUsable();
            if (usable > order) order = usable;
            partial = partial || (usable > 0 && usable < 8);   // the pieces turn usable one by one as the crew passes
        }
        CHECK(inOrder);
        CHECK(partial);
        CHECK(line.Usable() == 8);
        CHECK(host.registered.empty());   // the crew unregistered (and was released) when it finished
        CHECK(host.tickables.empty());
        CHECK(host.effects == 0);
        for (cSC4NetworkOccupant* piece : line.pieces) CHECK(cSC4NetworkConstructionCrew::GetNetworkOccupantCrewCount(piece) == 0);
        CHECK(RefsOf(line.pieces[3]) == baseline);
    }

    // ---- the lead vehicle: the network type's model, on the first piece's car path, heading for the next piece ----------
    {
        FakeHost host;
        host.vehicles = true;
        FakePathHost paths;
        SetNetworkConstructionCrewHost(&host);
        SetNetworkOccupantPathHost(&paths);
        Line line(2, 8, 4);
        cSC4NetworkConstructionCrew* crew = NewCrew(0.02f);
        HandOff(crew, line);
        crew->BeginConstruction();
        crew->Release();
        CHECK(host.made.size() == 1);
        FakeVehicle* lead = host.made.empty() ? nullptr : host.made[0];
        if (lead) {
            CHECK(lead->model == 0x107c0000);
            CHECK(lead->pathPiece == line.pieces[0]);
            CHECK((lead->pathKey >> 24) == 1 && ((lead->pathKey >> 8) & 0xff) == 0 && (lead->pathKey & 0xff) == 2);
            CHECK(std::fabs(lead->position[0] - (2 * 16 + 8)) < 0.01f);   // the middle of the first piece's path
            const auto& data = crew->Crews()[0];
            CHECK(std::fabs(data.lead.endPoint[0] - (3 * 16 + 8)) < 0.01f);   // ... going to the middle of the next one
            CHECK(!data.lead.arrived && data.lead.onPath);
        }
        CHECK(!host.workers.empty() && host.workers.size() >= 4 && host.workers.size() <= 7);   // four to seven workers on the piece
        // The vehicle drives at 32 units/s: half a second of ticks takes it the 16 units to the next piece's middle.
        for (int i = 0; i < 40; ++i) crew->DoAnimationTick(16000);
        if (lead) CHECK(crew->Crews()[0].lead.arrived || crew->Crews()[0].lead.vehicle != lead);
        // ---- a bulldozed piece under the crew: the crew moves on and the piece leaves its list -------------------------
        cSC4NetworkOccupant* under = crew->Crews()[0].current;
        const std::size_t before = crew->Crews()[0].pieces.size();
        crew->DoOccupantRemoved(under);
        CHECK(crew->Crews()[0].current != under);
        CHECK(crew->Crews()[0].pieces.size() == before - 1);
        // ---- the reset message (0x26d31ec2) stops every crew and hands the pieces over as usable ---------------------
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(0x26d31ec2);
        host.registered[0]->DoMessage(message);
        message->Release();
        CHECK(host.registered.empty());
        CHECK(line.Usable() == 7);   // all seven: the crew made the bulldozed one usable when it left it
        CHECK(line.pieces[0]->IsUsable());
        for (FakeVehicle* v : host.made) CHECK(v->refs == 0);
        for (FakeWorker* w : host.workers) CHECK(w->refs == 0 && w->removed);
        SetNetworkOccupantPathHost(nullptr);
    }

    SetNetworkConstructionCrewHost(nullptr);
    if (failures == 0) std::printf("sc4_networkconstructioncrew: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
