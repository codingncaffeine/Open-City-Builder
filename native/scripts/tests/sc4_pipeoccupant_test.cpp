// cSC4PipeOccupant's interfaces: 0x49c05b9f answers the pipe itself (the underground network tool casts it to the occupant
// class), and the port-only kGZIID_PortPipeOccupantView answers the burst view, which shares the pipe's count and forwards
// to the pipe's cell, position and edge connections.
#include <cstdint>
#include <cstdio>

#include "sc4/PipeOccupant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

int main() {
    auto* pipe = new cSC4PipeOccupant();
    pipe->AddRef();

    void* raw = nullptr;
    CHECK(pipe->QueryInterface(kGZCLSID_cSC4PipeOccupant, &raw) && raw == static_cast<void*>(static_cast<cSC4PipeOccupant*>(pipe)));
    if (raw) static_cast<cSC4PipeOccupant*>(raw)->Release();

    raw = nullptr;
    CHECK(pipe->QueryInterface(kGZIID_PortPipeOccupantView, &raw) && raw != nullptr);
    auto* view = static_cast<cISC4PipeOccupantView*>(raw);
    CHECK(raw != static_cast<void*>(static_cast<cSC4PipeOccupant*>(pipe)));
    CHECK(view->AsOccupant() == static_cast<cISC4Occupant*>(pipe));

    // one count: the view's reference is the pipe's
    const std::uint32_t raised = pipe->AddRef();   // the count with one more reference
    const std::uint32_t settled = pipe->Release();
    CHECK(raised == settled + 1);
    CHECK(view->AddRef() == raised);
    CHECK(view->Release() == settled);
    void* back = nullptr;
    CHECK(view->QueryInterface(kGZCLSID_cSC4PipeOccupant, &back) && back == static_cast<void*>(static_cast<cSC4PipeOccupant*>(pipe)));
    if (back) static_cast<cSC4PipeOccupant*>(back)->Release();

    // the forwarded queries answer what the pipe answers
    std::uint32_t vx = 99, vz = 99, px = 98, pz = 98;
    view->GetCell(vx, vz);
    pipe->GetOccupiedCell(px, pz);
    CHECK(vx == px && vz == pz);
    float vp[3] = {-1, -1, -1}, pp[3] = {-2, -2, -2};
    view->GetCenter(vp);
    pipe->GetPosition(pp);
    CHECK(vp[0] == pp[0] && vp[1] == pp[1] && vp[2] == pp[2]);
    for (int edge = 0; edge < 4; ++edge) CHECK(view->IsConnected(edge, 4) == pipe->ConnectsToNeighbor(edge, 4));

    view->Release();
    pipe->Release();
    std::printf(failures ? "FAILED\n" : "sc4_pipeoccupant ok\n");
    return failures ? 1 : 0;
}
