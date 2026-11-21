// cSC4PowerPoleOccupant: the connections between poles (which end draws the wire, its four curves and the buoys over
// water, the wire occupants it keeps), hiding / removing a connection, the wire cells, the highlight state and the
// foundation geometry SetPosition builds. The engine is a fake IPowerPoleOccupantHost.
#include "rz/PortMemoryStreams.h"
#include <cmath>
#include <cstdio>
#include <vector>

#include "sc4/PowerLineOccupant.h"
#include "sc4/PowerPoleOccupant.h"

using namespace sc4;

namespace {

int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);        \
            ++failures;                                                \
        }                                                              \
    } while (0)

struct FakeHost : IPowerPoleOccupantHost {
    float CityCellWidthX() override { return 16.0f; }
    float CityCellWidthZ() override { return 16.0f; }
    float TerrainCellSize() override { return 16.0f; }
    void TerrainCornerHeights(int, int, float out[4]) override {
        for (int i = 0; i < 4; ++i) out[i] = corners[i];
    }
    bool CellHasWater(int x, int z) override { return x == waterX && z == waterZ; }
    // cSC4NetworkRoutines::Get0To3Direction
    int Get0To3Direction(int x1, int z1, int x2, int z2) override {
        if (x1 == x2) return z1 != z2 ? 2 : 0;
        if (z1 == z2) return 0;
        return ((x1 < x2) != (z1 < z2)) ? 3 : 1;
    }
    int ViewZoom() override { return 4; }
    int ViewRotation() override { return 0; }
    float CameraYaw() override { return 0.0f; }
    std::uint32_t ModelInstanceID(cISC4Occupant*, int, int, int) override { return 0; }
    s3d::cIS3DModelInstance* CreateModelInstance(const cGZPersistResourceKey&) override { return nullptr; }
    int TextureId(std::uint32_t) override { return 0; }
    cS3DVector3 LightColor(const cS3DVector3&, const cS3DVector3&) override { return cS3DVector3{1.0f, 1.0f, 1.0f}; }
    void InsertOccupant(cISC4Occupant* occupant) override { inserted.push_back(occupant); }
    void RemoveOccupant(cISC4Occupant* occupant) override { removed.push_back(occupant); }
    const cISCPropertyHolder* TuningPropertyHolder() override { return nullptr; }

    float corners[4] = {10.0f, 12.0f, 9.0f, 11.0f};
    int waterX = -1, waterZ = -1;
    std::vector<cISC4Occupant*> inserted, removed;
};

cSC4PowerPoleOccupant* MakePole(int x, int z, float height) {
    auto* pole = new cSC4PowerPoleOccupant();
    pole->AddRef();
    const float position[3] = {static_cast<float>(x) * 16.0f + 8.0f, height, static_cast<float>(z) * 16.0f + 8.0f};
    pole->SetPosition(position);
    return pole;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

}  // namespace

int main() {
    FakeHost host;
    SetPowerPoleOccupantHost(&host);
    cSC4PowerPoleOccupant::StaticInit();

    // ---- SetPosition: the cell, the foundation floor at the lowest corner, the top and the two visible sides --------
    {
        cSC4PowerPoleOccupant* pole = MakePole(3, 5, 20.0f);
        std::uint32_t x = 0, z = 0;
        pole->GetCell(x, z);
        CHECK(x == 3 && z == 5);
        float minXyz[3], maxXyz[3];
        pole->GetBoundingBox(minXyz, maxXyz);
        CHECK(Near(minXyz[1], 9.0f));            // the lowest of the cell's corners
        CHECK(Near(maxXyz[1], 20.0f + 60.0f));   // 60 m above the pole
        CHECK(Near(minXyz[0], 48.0f) && Near(maxXyz[0], 64.0f));
        pole->Release();
    }

    // ---- AddConnection: the lower cell draws the wire (four curves from its connection points) -----------------------
    cSC4PowerPoleOccupant* a = MakePole(2, 2, 20.0f);
    cSC4PowerPoleOccupant* b = MakePole(2, 6, 20.0f);
    auto* wire = new cSC4PowerLineOccupant();
    wire->AddRef();
    std::vector<tLineInfo> lines(1);
    lines[0].x = 2;
    lines[0].z = 3;
    lines[0].line = wire;
    a->AddConnection(b, &lines);
    b->AddConnection(a, nullptr);
    CHECK(a->GetNumberOfConnections() == 1 && b->GetNumberOfConnections() == 1);
    CHECK(a->GetConnectionOccupant(0) == static_cast<IPowerPoleOccupant*>(b));
    const auto& ca = a->Connections()[0];
    const auto& cb = b->Connections()[0];
    CHECK(ca.drawsWire && !cb.drawsWire);
    CHECK(ca.otherX == 2 && ca.otherZ == 6);
    CHECK(ca.curvesA.size() == 4 && cb.curvesA.empty());
    CHECK(ca.curvesB.empty());   // no water at either end
    CHECK(ca.lines.size() == 1 && ca.lines.front() == wire);
    if (ca.curvesA.size() == 4) {
        // direction 2 (along z): the first point hangs 4.8 m to -x and 31.7 m above the pole
        const cS3DVector3& start = ca.curvesA[0].front();
        CHECK(Near(start.x, 40.0f - 4.8f) && Near(start.y, 20.0f + 31.7f) && Near(start.z, 40.0f));
        const cS3DVector3& end = ca.curvesA[0].back();
        CHECK(Near(end.x, 40.0f - 4.8f) && Near(end.z, 6.0f * 16.0f + 8.0f));   // ends at the other pole's point
        CHECK(ca.curvesA[0].size() >= 5);
    }
    // a second connection to the same cell, or to the pole's own cell, is refused
    a->AddConnection(b, nullptr);
    a->AddConnection(a, nullptr);
    CHECK(a->GetNumberOfConnections() == 1);

    // ---- the wire cells: the pole that does not draw asks the other end ------------------------------------------------
    std::vector<SC4PointU> cells;
    CHECK(b->GetWireCells(a, cells));
    CHECK(cells.size() >= 4 && cells.front().x == 2 && cells.front().z == 2 && cells.back().z >= 5);

    // ---- the wire occupants go into the city once ------------------------------------------------------------------
    a->InsertAllPowerLineOccupants();
    a->InsertAllPowerLineOccupants();
    CHECK(host.inserted.size() == 1 && host.inserted[0] == static_cast<cISC4Occupant*>(wire));

    // ---- DrawConnection: hidden while a preview splits the wire, back by cell order -------------------------------
    a->DrawConnection(b, false);
    CHECK(!a->Connections()[0].drawsWire);
    a->DrawConnection(b, true);
    CHECK(a->Connections()[0].drawsWire);
    b->DrawConnection(a, true);
    CHECK(!b->Connections()[0].drawsWire);

    // ---- the highlight state (SetHighlight): colour index in the attributes' second byte, bit 0 on -----------------
    a->SetModelState(3, false);
    CHECK((a->GetDrawAttributes().flags & 0xff01u) == 0x0301u);
    a->SetModelState(0, false);
    CHECK((a->GetDrawAttributes().flags & 0xff01u) == 0u);

    // ---- RemoveConnection: the wire occupants leave the city, the connection goes --------------------------------
    a->RemoveConnection(b, false);
    CHECK(a->GetNumberOfConnections() == 0);
    CHECK(host.removed.size() == 1 && host.removed[0] == static_cast<cISC4Occupant*>(wire));
    b->RemoveConnection(a, false);
    CHECK(b->GetNumberOfConnections() == 0);

    // ---- over water: each wire gets three buoys -----------------------------------------------------------------------
    host.waterX = 2;
    host.waterZ = 6;
    a->AddConnection(b, nullptr);
    CHECK(a->GetNumberOfConnections() == 1 && a->Connections()[0].curvesB.size() == 4);
    if (a->GetNumberOfConnections() == 1 && !a->Connections()[0].curvesB.empty())
        CHECK(a->Connections()[0].curvesB[0].size() == 3);
    // BreakAllConnectionsAtDestination: the other end forgets this pole
    b->AddConnection(a, nullptr);
    a->BreakAllConnectionsAtDestination(true);
    CHECK(b->GetNumberOfConnections() == 0);

    // ---- the record's last byte is the distressed flag (+0x10e), not the preview flag (+0x114) ---------------------------
    a->SetDistressed(true);   // the fake host makes no effect
    CHECK(a->GetDistressed() && !a->IsPreview());
    {
        PortMemoryOStream out;
        CHECK(a->Write(out));
        auto* c = new cSC4PowerPoleOccupant();
        c->AddRef();
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(c->Read(in));
        CHECK(c->GetDistressed() && !c->IsPreview());
        c->Release();
    }
    a->SetDistressed(false);
    CHECK(!a->GetDistressed());

    wire->Release();
    a->Release();
    b->Release();
    SetPowerPoleOccupantHost(nullptr);
    if (failures == 0) std::printf("sc4_powerpole_test: ok\n");
    return failures == 0 ? 0 : 1;
}
