#include "sc4/SubwayOccupant.h"

#include <cmath>

namespace sc4 {

namespace {
bool sbSubwaysInited = false;
}  // namespace

// 0x0013756a: the pipe's StaticInit over the subway tables. Each of the diagonal piece's five edge vertices is measured
// (x, z) to its hinge vertex; the piece's share for vertex u (1..4) is d[u] / (d[u] + d[5 - u]) and the cap's table holds
// the same shares in reverse order; both tables are 1 at their open end. The underground statics follow.
bool cSC4SubwayOccupant::StaticInit() {
    if (sbSubwaysInited) return false;
    const tCustomGeometryInfo& info = sCustomGeometryInfo;
    float distance[5];
    for (int i = 0; i < 5; ++i) {
        const UndergroundVertex& edge = info.vertices[info.edge0VertIndices[i]];
        const UndergroundVertex& hinge = info.vertices[info.hingeVertIndices[i]];
        const float dx = edge.x - hinge.x, dz = edge.z - hinge.z;
        distance[i] = std::sqrt(dx * dx + dz * dz);
    }
    sDiagonalPieceEdgeDistancePercentages[0] = 1.0f;
    sDiagonalCapEdgeDistancePercentages[4] = 1.0f;
    for (int u = 1; u < 5; ++u) {
        const float share = distance[u] / (distance[u] + distance[5 - u]);
        sDiagonalPieceEdgeDistancePercentages[u] = share;
        sDiagonalCapEdgeDistancePercentages[4 - u] = share;
    }
    sbSubwaysInited = true;
    return cSC4UndergroundOccupant::StaticInit();
}

bool cSC4SubwayOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZCLSID_cSC4SubwayOccupant) return cSC4UndergroundOccupant::QueryInterface(iid, out);
    *out = static_cast<cSC4SubwayOccupant*>(this);
    AddRef();
    return true;
}

bool cSC4SubwayOccupant::Init() {
    if (!sbSubwaysInited) StaticInit();
    if (!IsInitialized()) {
        cSC4UndergroundOccupant::Init();
        cSC4UndergroundOccupant::SetupPackedRenderState();
    }
    return true;
}

bool cSC4SubwayOccupant::Init(const float (*corners)[3]) {
    if (!sbSubwaysInited) StaticInit();
    if (!IsInitialized()) cSC4UndergroundOccupant::Init(corners, kGZCLSID_cSC4SubwayOccupant, &sCustomGeometryInfo);
    return true;
}

bool cSC4SubwayOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    cSC4UndergroundOccupant::Write(stream);
    return !stream.GetError();
}

bool cSC4SubwayOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (cSC4UndergroundOccupant::Read(stream)) SetModelDirty(true);
    return !stream.GetError();
}

// The flag test is 0x6000000 (the pipe's model offset), as in the original.
void cSC4SubwayOccupant::GetSortValue(std::uint32_t& out) const {
    if (HasAnyNetworkFlag(0x6000000)) cSC4UndergroundOccupant::GetSortValue(out);
    else out = kGZCLSID_cSC4SubwayOccupant;
}

const tCustomGeometryInfo* cSC4SubwayOccupant::GetCustomGeometryInfo() const { return &sCustomGeometryInfo; }

}  // namespace sc4
