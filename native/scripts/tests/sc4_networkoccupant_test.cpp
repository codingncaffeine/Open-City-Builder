#include <cstdio>
#include "rz/PortMemoryStreams.h"
#include "sc4/NetworkOccupant.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace {
class Host final : public INetworkOccupantHost {
public:
    int messages = 0;
    std::uint32_t lastMessage = 0, lastValue = 0;
    void PostMessage(std::uint32_t m, cISC4Occupant*, std::uint32_t v) override { ++messages; lastMessage = m; lastValue = v; }
    void RegisterWithNetworkManager(cISC4Occupant*) override {}
    bool IntersectionPieceId(std::uint32_t id) override { return id == 67122432; }
    void ReadPersistRef(cIGZIStream&, std::uint32_t, cIGZUnknown*& out) override { out = nullptr; }
    void WritePersistRef(cIGZOStream&, cIGZUnknown*) override {}
};
}
int main() {
    Host host;
    SetNetworkOccupantHost(&host);

    // edge connection store
    cSC4EdgeConnectionStore store;
    store.SetEdgeTypeConfig(1, 0x00010203);
    store.SetEdgeTypeConfig(4, 0x05);
    store.SetEdgeTypeConfig(8, 0x0700);
    CHECK(store.GetEdgeTypeConfig(1) == 0x00010203 && store.GetEdgeTypeConfig(4) == 5 && store.GetEdgeTypeConfig(2) == 0);
    CHECK(store.ExtraCount() == 2 && store.GetRaisedEdgeTypeConfig() == 0x0700);
    store.SetEdgeTypeConfig(4, 0);
    CHECK(store.GetEdgeTypeConfig(4) == 0 && store.ExtraCount() == 1);
    PortMemoryOStream os;
    CHECK(store.Write(os));
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    cSC4EdgeConnectionStore back;
    CHECK(back.Read(is) && back.GetEdgeTypeConfig(1) == 0x00010203 && back.GetEdgeTypeConfig(8) == 0x0700);

    // network occupant
    auto* n = new cSC4NetworkOccupant();
    n->AddRef();
    CHECK(n->GetType() == kOccupantTypeNetwork && n->GetNetworkFlag() == 0x06000000);
    const float corners[4][3] = {{16, 1, 32}, {32, 2, 32}, {32, 3, 48}, {16, 4, 48}};
    CHECK(n->Init(corners));
    float p[3], lo[3], hi[3];
    n->GetPosition(p);
    CHECK(p[0] == 24 && p[1] == 4 && p[2] == 40);
    n->GetBoundingBox(lo, hi);
    CHECK(lo[0] == 16 && hi[0] == 32 && lo[1] == 1 && hi[1] == 4);
    std::uint32_t cx, cz;
    n->GetOccupiedCell(cx, cz);
    CHECK(cx == 1 && cz == 2);

    // altitude on the tile: at the first corner it is its height; at the far corner the sum of slopes
    CHECK(n->GetAltitude(16, 32) == 1.0f);
    const float flat[4] = {1, 1, 1, 1};
    n->SetVertexHeights(flat);
    float heights[4];
    n->GetVertexHeights(heights);
    CHECK(heights[0] == 1 && heights[3] == 1);

    // flags and messages
    host.messages = 0;
    n->SetUsable(true);
    CHECK(n->IsUsable());
    n->ClearNetworkFlag(kNetworkFlagUsable);
    CHECK(!n->IsUsable() && host.lastMessage == kMessageNetworkUsableChanged && host.lastValue == 0);
    n->SetImmovable(true);
    CHECK(n->IsImmovable());
    n->SetVariation(3);
    CHECK(n->GetVariation() == 3 && host.lastMessage == kMessageOccupantChanged);
    n->SetQuadDraw(67122432, 0x81);
    CHECK(n->PieceId() == 67122432 && n->GetRotation() == 1 && n->GetFlip() == 1);
    n->SetNetworkFlag(kNetworkFlagPiece);
    CHECK(n->IsIntersection());
    n->SetEdgeType(1, 1, 3);
    CHECK((n->GetEdgeTypesFlag(1) >> 8 & 0xff) == 3);
    n->SetEdgeTypesFlag(1, 0x00000103);
    CHECK(n->GetActiveEdges() == 3 && n->GetActiveEdgesForTypeNoGroup(1) == 3 && n->GetGroupedEdges() == 0);
    n->SetNetworkFlag(1u << 1);
    CHECK(n->IsOfType(1) && n->ConnectsToNeighbor(0, 1) && n->ConnectsToNeighbor(1, 1) && !n->ConnectsToNeighbor(2, 1));

    // property backed state
    n->SetComplexPieceOrigin(5, 7);
    int ox = 0, oz = 0;
    CHECK(n->GetComplexPieceOrigin(ox, oz) && ox == 5 && oz == 7);
    n->SetOneWayDirections(2);
    CHECK(n->GetOneWayDirections() == 2);
    n->SetPassageCeiling(12.5f);
    float ceiling = 0;
    CHECK(n->GetPassageCeiling(ceiling) && ceiling == 12.5f);
    n->SetConnectionNodeEdgeTypes(1, 2);
    std::uint32_t a = 0, b = 0;
    CHECK(n->GetConnectionNodeEdgeTypes(a, b) && a == 1 && b == 2);
    n->SetConnectionPathIDOverride(77);
    CHECK(n->GetConnectionPathIDOverride() == 77);
    n->SetCostOfDemolition(1234567890123LL);

    // add-on part and bounds
    const float positions[3][3] = {{0, 50, 0}, {1, 51, 1}, {2, 52, 2}};
    const float uvs[3][2] = {{0, 0}, {1, 0}, {1, 1}};
    const std::uint32_t indices[3] = {0, 1, 2};
    n->AddPart(positions, uvs, indices, 99);
    n->GetBoundingBox(lo, hi);
    CHECK(lo[0] == 0 && hi[1] == 52);

    // record round trip
    PortMemoryOStream rec;
    CHECK(n->Write(rec));
    PortMemoryIStream ris(rec.bytes().data(), rec.bytes().size());
    auto* r = new cSC4NetworkOccupant();
    r->AddRef();
    CHECK(r->Read(ris));
    CHECK(r->PieceId() == 67122432 && r->GetVariation() == 3 && r->GetRotation() == 1 && r->GetFlip() == 1);
    CHECK(r->GetCostOfDemolition() == 1234567890123LL && r->AddOnParts().size() == 1 && r->AddOnParts()[0].id == 99);
    CHECK(r->GetEdgeTypesFlag(1) == 0x00000103 && r->HasNetworkFlag(kNetworkFlagPiece));
    // (property values only travel through DB segment streams; the record itself keeps the path override id)
    r->Release();
    n->SetPathIDOverride(0x1234);
    CHECK(n->GetPathIDOverride() == 0x1234);
    // translucency lives in the model's draw attributes (i386 0x001331b0): flag 2 and the alpha in shade[3], which the
    // renderer reads to draw the piece after the static view
    CHECK((n->ModelDrawAttributes().flags & 2u) == 0);
    n->SetTranslucency(true, 0x40);
    CHECK((n->ModelDrawAttributes().flags & 2u) != 0);
    CHECK(n->ModelDrawAttributes().shade[3] > 0.25f && n->ModelDrawAttributes().shade[3] < 0.252f);
    CHECK(n->Corner(0).color[3] == 0x40 && n->Corner(3).color[3] == 0x40);
    n->SetTranslucency(false, 0x40);
    CHECK((n->ModelDrawAttributes().flags & 2u) == 0 && n->ModelDrawAttributes().shade[3] == 1.0f);
    CHECK(n->Corner(0).color[3] == 0xff);
    n->Release();
    std::puts(failures ? "sc4_networkoccupant: FAILED" : "sc4_networkoccupant ok");
    return failures != 0;
}
