#include "sc4/PipeOccupant.h"

#include <cmath>


namespace sc4 {

namespace {
bool sbPipesInited = false;
// sWateredStateRGBs @ 0x100b017f0: dry, watered
const float kWateredStateRGBs[2][3] = {{0.8f, 0.23f, 0.23f}, {1.0f, 1.0f, 1.0f}};
}  // namespace

cSC4PipeOccupant::cSC4PipeOccupant() = default;

// 0x001368d8: the diagonal piece's edge distances. Each of its five edge vertices is measured (x, z) to its hinge
// vertex; the piece's share for vertex u (1..4) is d[u] / (d[u] + d[5 - u]) and the cap's table holds the same shares
// in reverse order; both tables are 1 at their open end. The underground statics follow.
bool cSC4PipeOccupant::StaticInit() {
    if (sbPipesInited) return false;
    tCustomGeometryInfo& info = sCustomGeometryInfo;
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
    sbPipesInited = true;
    return cSC4UndergroundOccupant::StaticInit();
}

bool cSC4PipeOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_PortPipeOccupantView) {   // port-only: see sc4/PipeOccupantView.h
        *out = static_cast<cISC4PipeOccupantView*>(&burstView_);
        AddRef();
        return true;
    }
    if (iid != kGZIID_cISC4PipeOccupantView && iid != kGZCLSID_cSC4PipeOccupant)
        return cSC4UndergroundOccupant::QueryInterface(iid, out);
    *out = static_cast<cSC4PipeOccupant*>(this);
    AddRef();
    return true;
}

bool cSC4PipeOccupant::Init() {
    if (!sbPipesInited) StaticInit();
    if (!IsInitialized()) {
        cSC4UndergroundOccupant::Init();
        cSC4UndergroundOccupant::SetupPackedRenderState();
    }
    return true;
}

bool cSC4PipeOccupant::Init(const float (*corners)[3]) {
    if (!sbPipesInited) StaticInit();
    if (!IsInitialized()) {
        cSC4UndergroundOccupant::Init(corners, kGZCLSID_cSC4PipeOccupant, &sCustomGeometryInfo);
        wateredState_ = 0;
    }
    return true;
}

bool cSC4PipeOccupant::Shutdown() { return cSC4UndergroundOccupant::Shutdown(); }

bool cSC4PipeOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(3);
    if (cSC4UndergroundOccupant::Write(stream)) stream.SetSint32(wateredState_);
    return !stream.GetError();
}

bool cSC4PipeOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 3) return false;
    if (cSC4UndergroundOccupant::Read(stream)) {
        std::int32_t state = 0;
        stream.GetSint32(state);
        wateredState_ = state;
        SetModelDirty(true);
    }
    return !stream.GetError();
}

void cSC4PipeOccupant::GetSortValue(std::uint32_t& out) const {
    if (HasAnyNetworkFlag(kPipeModelInstanceOffset)) cSC4UndergroundOccupant::GetSortValue(out);
    else out = kGZCLSID_cSC4PipeOccupant;
}

void cSC4PipeOccupant::SetWateredState(bool watered) {
    if ((wateredState_ == 1) != watered) {
        wateredState_ = watered ? 1 : 0;
        ModelStateChanged(false);
    }
}

cS3DVector3 cSC4PipeOccupant::GetDrawColor() const {
    const float* rgb = kWateredStateRGBs[wateredState_ & 1];
    return cS3DVector3{rgb[0], rgb[1], rgb[2]};
}

const tCustomGeometryInfo* cSC4PipeOccupant::GetCustomGeometryInfo() const { return &sCustomGeometryInfo; }

}  // namespace sc4
