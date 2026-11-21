// cSC4UndergroundCustomView — see UndergroundCustomView.h.
#include "sc4/UndergroundCustomView.h"

#include "sc4/NetworkOccupant.h"
#include "sc4/Occupant.h"
#include "sc4/PipeOccupant.h"
#include "sc4/SubwayOccupant.h"

namespace sc4 {
namespace {
IUndergroundCustomViewHost* g_host = nullptr;

constexpr std::uint32_t kViewSurfaceNetworkMask = 0x1f4f;
constexpr std::uint32_t kViewFlagConnectionNode = 0x200000;
constexpr std::uint32_t kViewFlagTranslucent = 0x4000;
constexpr std::uint32_t kViewFlagUsable = 0x10000000;
constexpr std::uint8_t kUndergroundViewAlpha = 0x40;

// The surface piece among an occupant's interfaces (the original asks for cISC4NetworkOccupant 0xa821ef94 and keeps
// pieces of a surface network that are not lot connection nodes), with one reference, or null.
cSC4NetworkOccupant* SurfacePiece(cISC4Occupant* occupant) {
    auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
    if (!network) return nullptr;
    const std::uint32_t flags = network->GetNetworkFlag();   // vslot 0x30
    if ((flags & kViewSurfaceNetworkMask) == 0 || (flags & kViewFlagConnectionNode) != 0) return nullptr;
    network->AddRef();
    return network;
}
}  // namespace

void SetUndergroundCustomViewHost(IUndergroundCustomViewHost* host) { g_host = host; }

cSC4UndergroundCustomView::cSC4UndergroundCustomView(std::uint32_t networkGziid) : networkGziid_(networkGziid) {}

cSC4UndergroundCustomView::~cSC4UndergroundCustomView() {
    for (cSC4NetworkOccupant* piece : pieces_) piece->Release();
}

// 0x0013a8fa: 0x452294aa is the message target, 0x8a175d6b and 1 the custom view.
bool cSC4UndergroundCustomView::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cISC4CustomView || iid == 1) {
        *out = static_cast<cISC4CustomView*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4UndergroundCustomView::Release() {
    if (--refs_ != 0) return refs_;
    delete this;
    return 0;
}

// 0x0013ab1c: a piece inserted while the view is up turns translucent as well.
bool cSC4UndergroundCustomView::DoMessage(cIGZMessage2* message) {
    if (message && message->GetType() == kMessageUndergroundOccupantInserted)
        DoOccupantInserted(static_cast<cISC4Occupant*>(static_cast<cIGZMessage2Standard*>(message)->GetVoid1()));
    return true;
}

// 0x0013ae90: every cell of the city, then the insertion messages.
bool cSC4UndergroundCustomView::InitCustomView() {
    if (!g_host) return true;
    const int cellsX = g_host->CityCellCountX(), cellsZ = g_host->CityCellCountZ();
    for (int x = 0; x < cellsX; ++x)
        for (int z = 0; z < cellsZ; ++z) SetRoadsTranslucent(x, z);
    if (cIGZMessageServer2* server = g_host->MessageServer())
        server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageUndergroundOccupantInserted);
    return true;
}

// 0x0013b474: a usable piece becomes opaque again; one still under construction keeps its translucency (its flag is set
// again, as when it was placed).
bool cSC4UndergroundCustomView::ShutdownCustomView() {
    if (g_host) {
        if (cIGZMessageServer2* server = g_host->MessageServer())
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageUndergroundOccupantInserted);
    }
    for (cSC4NetworkOccupant* piece : pieces_) {
        if (piece->GetNetworkFlag() & kViewFlagUsable) piece->ClearNetworkFlag(kViewFlagTranslucent);   // vslot 0x2c
        else piece->SetNetworkFlag(kViewFlagTranslucent);                                                  // vslot 0x28
        piece->Release();
    }
    pieces_.clear();
    return true;
}

// 0x0013ad46: the surface pieces of a cell turn translucent (flag 0x4000, then alpha 0x40) and tell their model.
void cSC4UndergroundCustomView::SetRoadsTranslucent(int x, int z) {
    if (!g_host) return;
    std::list<cISC4Occupant*> occupants;
    g_host->CellOccupants(x, z, occupants);
    for (cISC4Occupant* occupant : occupants) {
        if (cSC4NetworkOccupant* piece = SurfacePiece(occupant)) {
            piece->SetNetworkFlag(kViewFlagTranslucent);                                   // vslot 0x28
            piece->SetTranslucency(true, kUndergroundViewAlpha);                             // vslot 0xe8
            piece->PostOccupantMessage(kMessageOccupantTranslucencyChanged, 0);              // AsOccupant()->0x3c
            pieces_.push_back(piece);
        }
        occupant->Release();
    }
}

// 0x0013aa7a: as SetRoadsTranslucent for one piece, without the alpha (the flag's own translucency).
void cSC4UndergroundCustomView::DoOccupantInserted(cISC4Occupant* occupant) {
    if (!occupant) return;
    if (cSC4NetworkOccupant* piece = SurfacePiece(occupant)) {
        piece->SetNetworkFlag(kViewFlagTranslucent);
        piece->PostOccupantMessage(kMessageOccupantTranslucencyChanged, 0);
        pieces_.push_back(piece);
    }
}

cISC4CustomView* CreateCustomView(std::uint32_t classId) {
    cSC4UndergroundCustomView* view = nullptr;
    if (classId == kGZCLSID_cSC4PipeCustomView) view = new cSC4UndergroundCustomView(kGZCLSID_cSC4PipeOccupant);   // cSC4PipeCustomView 0x00678e44
    else if (classId == kGZCLSID_cSC4SubwayCustomView) view = new cSC4UndergroundCustomView(kGZCLSID_cSC4SubwayOccupant);   // cSC4SubwayCustomView 0x00678e0e
    if (!view) return nullptr;
    view->AddRef();
    return view;
}

}  // namespace sc4
