// cSC4NetworkTool — see NetworkTool.h for the object layout and the list of what is / is not recovered here.
//
// Faithfulness notes (quirks kept on purpose, with the address that shows them):
//  * the ctor loads `~kFlagForNetworkType[type] & 0x1f4f` as the tool's network flags (0x1001862bc).
//  * Init pushes the OR filter's bit mask through a `0x1f4f >> type & 1` test; types 4, 5, 7 and 11 get their
//    own single flag instead of 0x1f4f (0x100187752).
//  * the two network-type-from-flag scanners stop at 12 entries, so ground highway (bit 0x1000) is never
//    returned (0x10018f58a / 0x100190386); `GetFirstNetworkTypeFromFlags` masks the input with its lowest set bit.
//  * IsTerrainOK answers *true* when the terrain view says the cell cannot be built on (0x10071e4ca).
//  * Init sets +0x618 in a loop that runs once per bridge type without looking at the entry (0x100187752).
//  * SetTerrainHilight only skips the first step when validSteps is neither 0 nor the last step (0x10018cd24).
//  * GetLastValidPoint indexes the step array with validSteps-1 without a bounds check (0x100190668).
//  * GetMajorBridge* read the last element of the bridge vector through the vector's end pointer (0x100187f42).
#include "sc4/NetworkTool.h"
#include "sc4/NetworkToolUI.h"

#include "sc4/TileNeighborhood.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace sc4 {
namespace {

INetworkToolHost* g_networkToolHost = nullptr;

}  // namespace

void SetNetworkToolHost(INetworkToolHost* host) { g_networkToolHost = host; }
INetworkToolHost* GetNetworkToolHost() { return g_networkToolHost; }

bool cSC4NetworkTool::sbInited_ = false;

// ============================================================================================================
// construction / COM identity
// ============================================================================================================

// 0x1001862bc
cSC4NetworkTool::cSC4NetworkTool(std::uint32_t networkType) : networkType_(networkType) {
    unknown10_.clear();
    refCount_ = 0;
    inited_ = false;
    // +0x30: the original constructs its cSC4NetworkWorldCache inline. That class is unrecovered, so the host
    // creates one in Init instead (the constructor itself never touches the host).
    worldCache_ = nullptr;
    terrain_ = nullptr;
    occupantManager_ = nullptr;
    cityViewModel_ = nullptr;
    unknown98_ = false;
    unknown9a_ = true;
    forceReload_ = false;
    steps_.clear();
    cells_.clear();
    stepIsHorizontal_ = false;
    totalSteps_ = 0;
    straightSteps_ = 0;
    validSteps_ = 0;
    directionA_ = 0;
    directionB_ = 0;
    solvedCells_.clear();
    solvedCellList_.clear();   // +0x110, the member the base port used to be missing
    unknown128_.clear();
    unknown150_.clear();
    intersections_.clear();
    bridges_.clear();
    tunnels_.clear();
    reservedCells_.clear();
    rendererAltitudeScale_ = 0;
    pendingNeighborConnections_.clear();
    neighborConnections_.clear();
    anchors_.clear();
    constraintA_ = nullptr;
    constraintB_ = nullptr;
    networkFlags_ = ~kFlagForNetworkType[networkType] & kNetworkFlagMask;
    preview_ = nullptr;
    possibleBridges_.clear();
    preferredBridge_ = -1;
    bridgeAltitudeOverride_ = -1.0f;
    unknown458_ = 1;
    failureReason_ = 0;
    std::memset(unknown460_, 0, sizeof unknown460_);
    previewUpdateEnabled_ = true;
    cellSize_ = 0.0f;
    slopeRise_ = 0.0f;
    cityWidth_ = 0;
    cityHeight_ = 0;
    widthPlusOne_ = 0;
    heightPlusOne_ = 0;
    std::memset(initScratch_, 0, sizeof initScratch_);
    waterBridgeTypes_.clear();
    landBridgeTypes_.clear();
    hasBridgeTypes_ = false;
    typeInfo_ = nullptr;
    placementFlagMask_ = 0;
    networkOrFilter_ = nullptr;
}

// 0x100186748 (complete object destructor; the deleting one at 0x100186c34 is `delete this`)
cSC4NetworkTool::~cSC4NetworkTool() {
    Shutdown();
    // The original frees every member inline; here the vectors/sets do that themselves and only the counted
    // preview and the OR filter are left (Shutdown normally cleared them already).
    if (preview_ != nullptr) {
        preview_->Release();
        preview_ = nullptr;
    }
    if (networkOrFilter_ != nullptr) {
        networkOrFilter_->Release();
        networkOrFilter_ = nullptr;
    }
}

// 0x100186cc2
bool cSC4NetworkTool::QueryInterface(std::uint32_t iid, void** out) {
    if (out == nullptr) return false;
    if (iid == 1 || iid == kGZIID_cISC4NetworkTool) {
        *out = static_cast<cISC4NetworkTool*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();  // the original calls vtable slot +0x08 on the primary interface
    return true;
}

// 0x100186c62 (the +0x20 non-virtual thunk adjusts `this` by +8 and hits the same counter)
std::uint32_t cSC4NetworkTool::AddRef() { return ++refCount_; }

// 0x100186c7e
std::uint32_t cSC4NetworkTool::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;  // the original tail-calls vtable slot +0x1a0 (the deleting destructor)
    return 0;
}

// ============================================================================================================
// static state
// ============================================================================================================

// 0x100186d04
bool cSC4NetworkTool::StaticInit() {
    if (!sbInited_) {
        sbInited_ = true;
        if (INetworkToolHost* host = GetNetworkToolHost()) {
            host->RegisterShutdownCallback(&cSC4NetworkTool::StaticShutdown, nullptr);  // SL::spSC4 +0x218
        }
        ReloadRules();
    }
    return true;
}

// 0x100186d3e
bool cSC4NetworkTool::StaticShutdown(void* user) {
    (void)user;
    if (sbInited_) sbInited_ = false;
    return true;
}

// 0x10018774c
void cSC4NetworkTool::ReloadCheck() {}

// 0x100186d56 — NOT RECOVERED. It reads four ini resources (road/network type info, the multitype rules, the
// tile-conflict overrides and the highway intersection rules) through cRZIniFile and hands them to
// cSC4NetworkTypeInfo::Load* (0x1001c7308..0x1001c8b00) and SC4NetworkLoadOverrideMatrix (0x1001c6f23), none of
// which are recovered yet. Keeping it a no-op leaves the network type records empty; every function that reads
// them is marked recovered-partial in notes/ledger_pending/cSC4NetworkTool.txt.
void cSC4NetworkTool::ReloadRules() {}

// ============================================================================================================
// Init / Shutdown / Reset
// ============================================================================================================

// 0x100187752
bool cSC4NetworkTool::Init() {
    if (inited_) return true;
    INetworkToolHost* host = GetNetworkToolHost();
    if (host == nullptr) return false;

    if (worldCache_ == nullptr) worldCache_ = host->CreateWorldCache();  // cSC4NetworkWorldCache ctor 0x10079d6fe
    // +0x228 / +0x328: the original constructs its two cSC4VertexHtConstraintSatisfier members inline (ctor
    // 0x1001bca8c). That class is unrecovered, so the port asks the host for them; the underground tool drives
    // the first one on every cell of a drag.
    if (constraintA_ == nullptr) constraintA_ = host->CreateConstraintSatisfier();
    if (constraintB_ == nullptr) constraintB_ = host->CreateConstraintSatisfier();
    typeInfo_ = host->NetworkTypeInfo(networkType_);
    if (!sbInited_) StaticInit();

    ReloadNetworkSpecificInfo(kNetworkPlacementExemplarIDs[networkType_]);  // vtable slot +0x288

    INetworkPreview* preview = host->CreatePreview();  // cSC4NetworkPreview ctor 0x100718d8c
    if (preview_ != preview) {
        preview->AddRef();
        INetworkPreview* old = preview_;
        preview_ = preview;
        if (old != nullptr) old->Release();
    }
    if (preview_ == nullptr) return false;
    preview_->Init();
    host->AddRenderLayer(preview_, 5, 1000);  // SL::spRenderer +0x100 (layer 5, priority 1000)

    placementFlagMask_ = kNetworkFlagMask;
    if (((kNetworkFlagMask >> (networkType_ & 0x3f)) & 1) == 0) {
        placementFlagMask_ = kFlagForNetworkType[networkType_];
    }
    networkOrFilter_ = host->CreateNetworkOrFilter(placementFlagMask_);  // cSC4NetworkORFilter ctor 0x1002eee0a
    if (networkOrFilter_ != nullptr) networkOrFilter_->AddRef();

    // The placement block lives at network type info +0x80. The original copies five 8-byte words of scalars
    // unconditionally and the vectors only when source and destination differ (they never do in practice).
    if (typeInfo_ != nullptr) {
        const cSC4NetworkPlacementParams& src = typeInfo_->PlacementParams();
        placement_.unknown00[0] = src.unknown00[0];
        placement_.unknown00[1] = src.unknown00[1];
        placement_.unknown00[2] = src.unknown00[2];
        placement_.unknown00[3] = src.unknown00[3];
        placement_.unknown10[0] = src.unknown10[0];
        placement_.unknown10[1] = src.unknown10[1];
        placement_.maxSlope = src.maxSlope;
        placement_.unknown1c = src.unknown1c;
        placement_.unknown20 = src.unknown20;
        placement_.unknown24 = src.unknown24;
        placement_.straightOrDiagonalOnly = src.straightOrDiagonalOnly;
        placement_.unknown26[0] = src.unknown26[0];
        placement_.unknown26[1] = src.unknown26[1];
        if (&placement_ != &src) {
            placement_.list28 = src.list28;
            for (int i = 0; i < 8; ++i) placement_.unknown40[i] = src.unknown40[i];
            placement_.list60 = src.list60;
            placement_.list78 = src.list78;
            placement_.list90 = src.list90;
            for (int i = 0; i < 10; ++i) placement_.unknownA8[i] = src.unknownA8[i];
            placement_.listD0 = src.listD0;
        }

        // IdentifyWaterAndLandBridgeTypes(typeInfo +0x40, waterBridgeTypes_ (+0x5e8), landBridgeTypes_ (+0x600))
        IdentifyWaterAndLandBridgeTypes(&typeInfo_->BridgeTypes(), waterBridgeTypes_, landBridgeTypes_);
        hasBridgeTypes_ = false;
        // Quirk: the original walks the vector's element count but never looks at an element.
        for (std::size_t i = 0; i < typeInfo_->BridgeTypes().size(); ++i) hasBridgeTypes_ = true;
    }

    const int width = host->CityWidth();   // SL::spCity +0x2e8
    cityWidth_ = width;
    widthPlusOne_ = width + 1;
    const int height = host->CityHeight();  // SL::spCity +0x2f0
    cityHeight_ = height;
    heightPlusOne_ = height + 1;
    // +0x4bc: sixteen width-derived values (Init writes the same `width + 2 / width + 1 / 1 / 0` pattern four
    // times and never uses the height). Purpose not recovered; stored verbatim so the documented offsets hold.
    {
        const std::int32_t w = width;
        const std::int32_t scratch[16] = {
            w + 2, w + 1, w + 1, 0, 0, 1, 1, w + 2, 1, 1, w + 2, w + 2, w + 1, w + 1, 0, 0,
        };
        std::memcpy(initScratch_, scratch, sizeof scratch);
    }

    terrain_ = host->AcquireTerrain();  // SL::spTerrain, counted
    if (terrain_ != nullptr) {
        terrain_->AddRef();
        cellSize_ = host->TerrainCellSize();  // SL::spTerrain +0x30
    }
    occupantManager_ = host->AcquireOccupantManager();  // SL::spOccupantManager, counted
    if (occupantManager_ != nullptr) occupantManager_->AddRef();
    cityViewModel_ = host->AcquireCityViewModel();  // SL::spCity +0x108, counted
    if (cityViewModel_ != nullptr) cityViewModel_->AddRef();

    slopeRise_ = std::tan(placement_.maxSlope) * cellSize_;

    rendererAltitudeScale_ = 0;
    rendererAltitudeScale_ = host->RendererAltitudeScale();  // SL::spRenderer +0x50 -> +0x14

    // The original also constructs, touches (slots +0x18 / +0x20) and releases a throw-away
    // cSC4NetworkOccupant here (0x10019bc16) — not reproduced, see PARTIAL.

    host->RegisterMessage(static_cast<cIGZMessageTarget2*>(this), kNetworkToolMessageAltitudes);  // spMS2 +0x28
    inited_ = true;
    return true;
}

// 0x100187ce8
bool cSC4NetworkTool::Shutdown() {
    if (!inited_) return true;
    if (INetworkToolHost* host = GetNetworkToolHost()) {
        host->UnregisterMessage(static_cast<cIGZMessageTarget2*>(this), kNetworkToolMessageAltitudes);  // +0x30
    }
    possibleBridges_.clear();  // *(this+0x440) = *(this+0x438)
    // The original calls vtable slot +0x88 (ClearOccupantHighlights 0x1001901a6) and +0x80 (ClearCellPreviews
    // 0x10018803e) here; both are unrecovered, see PARTIAL.
    anchors_.clear();  // *(this+0x218) = *(this+0x210)
    cells_.clear();    // *(this+0xc0) = *(this+0xb8)
    if (cityViewModel_ != nullptr) {
        cityViewModel_->Release();
        cityViewModel_ = nullptr;
    }
    if (occupantManager_ != nullptr) {
        occupantManager_->Release();
        occupantManager_ = nullptr;
    }
    if (terrain_ != nullptr) {
        terrain_->Release();
        terrain_ = nullptr;
    }
    if (networkOrFilter_ != nullptr) {
        networkOrFilter_->Release();
        networkOrFilter_ = nullptr;
    }
    if (preview_ != nullptr) {
        if (INetworkToolHost* host = GetNetworkToolHost()) host->RemoveRenderLayer(preview_);  // spRenderer +0x108
        preview_->Shutdown();
        preview_->Release();
        preview_ = nullptr;
    }
    inited_ = false;
    return true;
}

// 0x10018fdaa
void cSC4NetworkTool::Reset() {
    if (worldCache_ != nullptr) worldCache_->Clear();  // cSC4NetworkWorldCache::Clear 0x10079d868
    // vtable slot +0x88 (ClearOccupantHighlights) and +0x80 (ClearCellPreviews) follow here — unrecovered.
    anchors_.clear();  // *(this+0x218) = *(this+0x210)
    cells_.clear();    // *(this+0xc0) = *(this+0xb8)
    preferredBridge_ = -1;
    bridgeAltitudeOverride_ = -1.0f;  // *(this+0x450) = 0xbf800000ffffffff
    possibleBridges_.clear();         // *(this+0x440) = *(this+0x438)
    ClearFailureReason();             // vtable slot +0x68
    reservedCells_.clear();           // *(this+0x1c8) = *(this+0x1c0)
}

// ============================================================================================================
// message target
// ============================================================================================================

// 0x100188822 (+0x18 thunk in the message-target vtable)
bool cSC4NetworkTool::DoMessage(cIGZMessage2* message) {
    if (message->GetType() == kNetworkToolMessageAltitudes) {
        // The original calls the cIGZMessage2Standard payload-2 accessor ([15] +0x78) without a QueryInterface
        // and stores the first word it points at; a null payload would crash it too.
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        const auto* payload = static_cast<const std::uint32_t*>(standard->GetVoid2());
        rendererAltitudeScale_ = *payload;
    }
    return true;
}

// ============================================================================================================
// drag state accessors
// ============================================================================================================

// 0x100190604
void cSC4NetworkTool::UpdateOnZoneChange(SC4RectU rect, std::uint32_t network) {
    (void)rect;
    (void)network;
}

// 0x100193650
void cSC4NetworkTool::ForceReload() { forceReload_ = true; }

// 0x100190618
void cSC4NetworkTool::SetTraceFinal(bool final) { (void)final; }

// 0x100193668
void cSC4NetworkTool::EnablePreviewUpdate(bool enabled) { previewUpdateEnabled_ = enabled; }

// 0x100190628
void cSC4NetworkTool::SetValidStepCount(int count) {
    validSteps_ = count;
    SetTerrainHilight(0);
}

// 0x100190638
int cSC4NetworkTool::GetValidStepCount() { return validSteps_; }

// 0x1001936b0
int cSC4NetworkTool::GetStraightStepCount() { return straightSteps_; }

// 0x100190650
int cSC4NetworkTool::GetTotalStepCount() { return totalSteps_; }

// 0x1001936c8
bool cSC4NetworkTool::IsRegularStep(int step) { return steps_[static_cast<std::size_t>(step)].direction == 0; }

// 0x100190668: `steps_[validSteps - 1].lastCell` indexes the dragged-cell array. No bounds check in the
// original (validSteps == 0 reads the element before the vector), so the caller must have a drag in progress.
void cSC4NetworkTool::GetLastValidPoint(SC4PointU& out) {
    const tDraggedStep& step = steps_[static_cast<std::size_t>(validSteps_ - 1)];
    out = cells_[step.lastCell];
}

// 0x100193678
void cSC4NetworkTool::GetDraggedCells(std::vector<SC4PointU>& out) {
    if (&cells_ != &out) out = cells_;
}

// 0x1002ed4ac
void cSC4NetworkTool::SetAnchorPoints(const std::vector<SC4PointU>& anchors) {
    if (&anchors_ != &anchors) anchors_ = anchors;
}

// 0x1002ed4e8
void cSC4NetworkTool::GetPlacementParams(cSC4NetworkPlacementParams& out) {
    // memmove because the caller may pass the tool's own block back in (the original's self-assignment guard
    // only skips the vectors, the scalar copies always happen).
    std::memmove(&out.unknown00, &placement_.unknown00, 0x10);
    std::memmove(&out.unknown10, &placement_.unknown10, 8);
    std::memmove(&out.maxSlope, &placement_.maxSlope, 8);  // +0x18..+0x1f
    // The original copies eight bytes starting at +0x1e as well (a compiler-merged copy that overlaps the two
    // previous ones: the high half of +0x1c, +0x20, +0x24 and the straight-or-diagonal flag at +0x25).
    std::memmove(reinterpret_cast<std::uint8_t*>(&out) + 0x1e,
                 reinterpret_cast<const std::uint8_t*>(&placement_) + 0x1e, 8);
    if (&placement_ != &out) {
        out.list28 = placement_.list28;
        std::memcpy(out.unknown40, placement_.unknown40, sizeof placement_.unknown40);
        out.list60 = placement_.list60;
        out.list78 = placement_.list78;
        out.list90 = placement_.list90;
        std::memcpy(out.unknownA8, placement_.unknownA8, sizeof placement_.unknownA8);
        out.listD0 = placement_.listD0;
    }
}

// 0x1002ed4d0 / 0x1002ed6dc / 0x1002ed6e4 / 0x1002ed6ec / 0x1002ed6f4: the base tool's constant answers. The
// underground tools (pipe / subway) override them.
bool cSC4NetworkTool::StopPickAtWater() { return true; }
bool cSC4NetworkTool::AllowBridges() { return true; }
bool cSC4NetworkTool::AllowTunnels() { return true; }
bool cSC4NetworkTool::IsStoppedByWater() { return true; }
bool cSC4NetworkTool::ShouldAutoConnect() { return false; }

// 0x1002ed6fc / 0x1002ed708 / 0x1001906fc: empty in the base tool (the derived tools do the work).
void cSC4NetworkTool::StartPlacing() {}
void cSC4NetworkTool::PropagateSolvedCellsForSmoothing() {}
void cSC4NetworkTool::ReloadNetworkSpecificInfo(std::uint32_t exemplarId) { (void)exemplarId; }

// 0x1002ed4d8
std::uint32_t cSC4NetworkTool::GetToolViewId() { return typeInfo_ != nullptr ? typeInfo_->ToolViewId() : 0; }

// 0x1001906a0 / 0x1001906b0 / 0x1001906c0 / 0x1001906d0: read straight out of the network type info record
// (+0x08 view input control id, +0x14 cursor id, +0x18 directional cursor base, +0x1c / +0x20 pen widths).
std::uint32_t cSC4NetworkTool::ViewInputControlId() {
    return typeInfo_ != nullptr ? typeInfo_->ViewInputControlId() : 0;
}

std::uint32_t cSC4NetworkTool::CursorId() { return typeInfo_ != nullptr ? typeInfo_->CursorId() : 0; }

std::uint32_t cSC4NetworkTool::DirectionalCursorBase() {
    return typeInfo_ != nullptr ? typeInfo_->DirectionalCursorBase() : 0;
}

void cSC4NetworkTool::PenWidth(int& widthX, int& widthZ) {
    widthX = typeInfo_ != nullptr ? typeInfo_->PenWidthX() : 0;  // +0x1c (DAT_100b231ac)
    widthZ = typeInfo_ != nullptr ? typeInfo_->PenWidthZ() : 0;  // +0x20 (DAT_100b231b0)
}

// ============================================================================================================
// failure state
// ============================================================================================================

// 0x1002ed3e0
void cSC4NetworkTool::ClearFailureReason() { failureReason_ = 0; }

// 0x1002ed3f0
bool cSC4NetworkTool::GetFailureState() { return failureReason_ != 0; }

// 0x1002ed400
void cSC4NetworkTool::GetFailureReasonString(cIGZString& out) {
    cRZString text;
    GetFailureReasonString(failureReason_, text);  // vtable slot +0x1b0
    out.AssignFromChars(text.Data(), text.Strlen());
}

// 0x10018fe7a
void cSC4NetworkTool::GetFailureReasonString(std::uint32_t reason, cRZString& out) {
    out.Erase(0, 0xFFFFFFFFu);  // std::string::erase(0, npos)
    if (reason == 0) return;
    if (INetworkToolHost* host = GetNetworkToolHost()) {
        if (host->LoadString(kFailureReasonStringGroup, reason, out)) return;
    }
    // The original builds the fallback in a fresh libc++ std::string (size 0x23, capacity 0x30) and assigns it.
    out.AssignFromCString("## Missing failure reason string ##");
}

// ============================================================================================================
// bridges
// ============================================================================================================

// 0x100187e88
bool cSC4NetworkTool::GetPossibleBridges(std::vector<int>& out) {
    if (possibleBridges_.empty()) return false;
    if (&possibleBridges_ != &out) out = possibleBridges_;
    return true;
}

// 0x100187ec8 / 0x100187ed4
int cSC4NetworkTool::GetPreferredBridge() { return preferredBridge_; }
void cSC4NetworkTool::SetPreferredBridge(int bridge) { preferredBridge_ = bridge; }

// 0x100187ee0 / 0x100187eee
float cSC4NetworkTool::GetBridgeAltitudeOverride() { return bridgeAltitudeOverride_; }
void cSC4NetworkTool::SetBridgeAltitudeOverride(float altitude) { bridgeAltitudeOverride_ = altitude; }

// 0x100187efc: the bridge type's name, a libc++ std::string at +0x1c8 of the 0x200-byte record.
void cSC4NetworkTool::GetBridgeName(cIGZString& out, int bridge) {
    if (typeInfo_ == nullptr) return;
    const std::vector<cSC4NetworkBridgeTypeInfo>& types = typeInfo_->BridgeTypes();  // +0x40, stride 0x200
    out.AssignFromCString(types[static_cast<std::size_t>(bridge)].name.c_str());     // vtable +0x28
}

// 0x100187f42
int cSC4NetworkTool::GetMajorBridge() { return bridges_.empty() ? -1 : bridges_.back().type; }

// 0x100187f84
bool cSC4NetworkTool::GetMajorBridgeAltitude(float& out) {
    if (bridges_.empty()) return false;
    out = bridges_.back().altitude;
    return true;
}

// 0x100187f62
bool cSC4NetworkTool::GetMajorBridgeReferenceAltitude(float& out) {
    if (bridges_.empty()) return false;
    out = bridges_.back().referenceAltitude;
    return true;
}

// 0x100187fa6
bool cSC4NetworkTool::IsMajorBridgeLandBridge() { return !bridges_.empty() && bridges_.back().landBridge; }

// 0x100187fc8
bool cSC4NetworkTool::GetBridgeBitmapKey(int bridge, std::uint32_t& keyA, std::uint32_t& keyB) {
    if (typeInfo_ == nullptr) return false;
    const std::vector<cSC4NetworkBridgeTypeInfo>& types = typeInfo_->BridgeTypes();
    const cSC4NetworkBridgeTypeInfo& type = types[static_cast<std::size_t>(bridge)];
    if (type.bitmapKeyA == 0 || type.bitmapKeyB == 0) return false;
    keyA = type.bitmapKeyA;
    keyB = type.bitmapKeyB;
    return true;
}

// 0x10018800a
bool cSC4NetworkTool::GetPossibleNeighborConnection(NeighborConnectionInfo& out) {
    if (neighborConnections_.empty()) return false;
    out = neighborConnections_.front().info;
    return true;
}

// 0x100188c0e
const cSC4NetworkSupportStructInfo* cSC4NetworkTool::GetSupportStructureInfo(std::uint32_t id) {
    if (typeInfo_ == nullptr) return nullptr;
    return typeInfo_->FindSupportStructure(id);  // the map at network type info +0x58
}

// 0x100188abe: exemplarId is the resource instance, `groupOrZero` the group (0 = the type info's default id).
cIGZUnknown* cSC4NetworkTool::GetOccupantExemplar(std::uint32_t exemplarId, std::uint32_t groupOrZero) {
    std::uint32_t group = groupOrZero;
    if (group == 0) group = typeInfo_ != nullptr ? typeInfo_->DefaultExemplarId() : 0;  // +0x0c
    if (INetworkToolHost* host = GetNetworkToolHost()) {
        return host->GetTuningProperty(kNetworkTuningType, group, exemplarId, kNetworkTuningProperty);
    }
    return nullptr;
}

// ============================================================================================================
// network type helpers
// ============================================================================================================

// 0x10018f58a: the first of the twelve flag entries that intersects `flag`, or 12 when none does.
int cSC4NetworkTool::GetNetworkTypeFromFlag(std::uint32_t flag) {
    for (int i = 0; i < 12; ++i) {
        if ((kFlagForNetworkType[i] & flag) != 0) return i;
    }
    return 12;
}

// 0x100190386: same scan, but the input is first reduced to its lowest set bit (`flag & -flag`).
int cSC4NetworkTool::GetFirstNetworkTypeFromFlags(std::uint32_t flags) {
    const std::uint32_t lowest = flags & (~flags + 1);
    for (int i = 0; i < 12; ++i) {
        if ((kFlagForNetworkType[i] & lowest) != 0) return i;
    }
    return 12;
}

// 0x1001c2c10
void cSC4NetworkTool::IdentifyWaterAndLandBridgeTypes(const std::vector<cSC4NetworkBridgeTypeInfo>* bridgeTypes,
                                                      std::vector<int>& waterBridgeTypes,
                                                      std::vector<int>& landBridgeTypes) {
    waterBridgeTypes.clear();
    landBridgeTypes.clear();
    if (bridgeTypes == nullptr) return;
    for (std::size_t i = 0; i < bridgeTypes->size(); ++i) {
        const cSC4NetworkBridgeTypeInfo& type = (*bridgeTypes)[i];
        if (type.isLand) landBridgeTypes.push_back(static_cast<int>(i));    // record +0x100
        if (type.isWater) waterBridgeTypes.push_back(static_cast<int>(i));  // record +0x101
    }
}

// ============================================================================================================
// terrain / cell queries
// ============================================================================================================

// 0x10018b0a0: a drag over a plain diagonal tile (one or two diagonal edges, no straight edge).
bool cSC4NetworkTool::IsSimpleDiagonalNetwork(const cSC4NetworkCellInfo& cell, std::uint32_t networkType) {
    const cSC4TileNeighborhoodCompact neighborhood(cell.tileNeighborhoods[networkType]);  // +0x20 + type * 4
    int straight[4] = {};
    int diagonal[4] = {};
    int straightCount = 0;
    int diagonalCount = 0;
    neighborhood.GetStraightAndDiagonalEdges(straight, diagonal, straightCount, diagonalCount);
    if (straightCount != 0) return false;
    return static_cast<std::uint32_t>(diagonalCount - 1) < 2;  // 1 or 2 diagonals
}

// 0x100188b2c
void cSC4NetworkTool::GetPaddedRectAroundCell(std::uint32_t x, std::uint32_t z, SC4RectU& out,
                                              std::uint32_t pad) {
    // Quirk: `x - pad` is computed as an int and clamped to zero (same result as a plain max(0, x - pad)).
    std::int32_t left = static_cast<std::int32_t>(x - pad);
    if (x < pad || left == 0) left = 0;
    out.x0 = static_cast<std::uint32_t>(left);
    out.x1 = pad + x;
    if (static_cast<std::uint32_t>(cityWidth_) <= pad + x) out.x1 = static_cast<std::uint32_t>(cityWidth_ - 1);

    std::int32_t top = static_cast<std::int32_t>(z - pad);
    if (z < pad || top == 0) top = 0;
    out.z0 = static_cast<std::uint32_t>(top);
    out.z1 = pad + z;
    if (static_cast<std::uint32_t>(cityHeight_) <= pad + z) out.z1 = static_cast<std::uint32_t>(cityHeight_ - 1);
}

// 0x10018fe2e
bool cSC4NetworkTool::OccupantExists(int x, int z, std::uint32_t typeMask, std::uint32_t& occupantId) {
    INetworkToolHost* host = GetNetworkToolHost();
    if (host == nullptr) return false;
    const std::uint32_t key = static_cast<std::uint32_t>(x) + (static_cast<std::uint32_t>(z) << 16);
    const cSC4NetworkCellInfo* cell = host->GetCellInfo(key);  // GetCellInfo 0x1001b4bc0
    if (cell == nullptr || !cell->hasOccupant) return false;   // +0x59
    if ((cell->networkFlags & typeMask) == 0) return false;    // +0x08
    occupantId = cell->tileNeighborhoods[networkType_];        // +0x20 + type * 4
    return true;
}

// 0x10071e4ca. Quirk: when the terrain view refuses the cell the original answers true.
bool cSC4NetworkTool::IsTerrainOK(std::uint32_t x, std::uint32_t z) {
    INetworkToolHost* host = GetNetworkToolHost();
    if (host == nullptr) return false;
    // i386: SL::spTerrain vslot 0x88 = CellHasWater; a water cell is fine only where a piece that may stand in water is.
    if (!host->TerrainAllowsNetwork(x, z)) return true;
    const cSC4NetworkCellInfo* cell = GetCellInfo(x, z);
    return cell != nullptr && cell->buildable;  // +0x5e (i386 +0x56)
}

// 0x10071e47a
bool cSC4NetworkTool::IdentifyStartingConditions(int& failureReason) {
    if (IsStoppedByWater()) {  // vtable slot +0x208
        if (cells_.empty()) return false;
        if (!IsTerrainOK(cells_[0].x, cells_[0].z)) {
            failureReason = 0;
            return false;
        }
    }
    return true;
}

// ============================================================================================================
// preview highlight of the valid part of the drag
// ============================================================================================================

// 0x10018cd24
void cSC4NetworkTool::SetTerrainHilight(int param) {
    (void)param;
    if (preview_ == nullptr) return;  // the original assumes Init() ran
    int first = validSteps_;
    int total = totalSteps_;
    if (first >= total) return;
    // Quirk: the skip flag is computed once from the entry value and cleared after the first iteration, so only
    // the first step can be skipped (when validSteps is 0 or the last step).
    bool skip = first != total - 1 && first != 0;
    for (long i = first; i < total; ++i) {
        if (!skip) {
            const tDraggedStep& step = steps_[static_cast<std::size_t>(i)];
            for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
                const SC4PointU cell = cells_[c];
                preview_->Erase(cell);
                const float elevation = networkType_ == kNetworkHighway ? 15.5f : 0.0f;
                preview_->AddUntexturedSelection(cell, 0x80ff0000, nullptr, elevation, false);
            }
            total = totalSteps_;
        } else {
            skip = false;
        }
    }
}

// ============================================================================================================
// straight / diagonal splitter
// ============================================================================================================

// 0x10071aa64. The function snaps `end` onto the nearest legal end point and reports how the drag is laid out:
//   totalSteps    (+0xd4): number of steps, long axis length + 1
//   straightSteps (+0xd8): straight part length (+1) for a straight drag, |dx| - |dz| for a diagonal one
//   directionA/ B (+0xe0 / +0xe4): the two 8-way direction codes (0=E, 1=NE, 2=N, 3=NW, 4=W, 5=SW, 6=S, 7=SE)
//   isVertical / isHorizontal: which of the two the drag is (the caller stores isHorizontal at +0xd0)
// `penWidthX` is the network's x pen width (type info +0x1c), `penWidthZ` its z width (+0x20); only penWidthX
// and the straight-or-diagonal flag of the placement block take part.
void cSC4NetworkTool::BreakIntoStraightAndDiagSegments(const SC4PointU& start, SC4PointU& end, int& totalSteps,
                                                       int& straightSteps, int& directionA, int& directionB,
                                                       int& isVertical, int& isHorizontal, bool allowDiagonals,
                                                       int penWidthX, int penWidthZ) {
    const int startX = static_cast<int>(start.x);
    const int startZ = static_cast<int>(start.z);
    const int endX = static_cast<int>(end.x);  // the direction code below uses the *original* end point
    const int endZ = static_cast<int>(end.z);
    int curX = endX;
    int curZ = endZ;

    const std::uint32_t rawDx = static_cast<std::uint32_t>(endX - startX);
    std::uint32_t absDx = -rawDx;
    if (static_cast<std::int32_t>(rawDx) > 0) absDx = rawDx;
    const std::uint32_t rawDz = static_cast<std::uint32_t>(endZ - startZ);
    std::uint32_t absDz = -rawDz;
    if (static_cast<std::int32_t>(rawDz) > 0) absDz = rawDz;

    if (absDx == 1 && absDz > 2) {
        curX = startX;  // one cell off the axis: snap to the vertical
        absDx = 0;
    } else if (absDx < 3 || absDz != 1) {
        if (rawDz == 0 || absDx != absDz + 2) {
            if (rawDx != 0 && absDz == absDx + 2) {
                const int sign = startZ < endZ ? -1 : 1;
                curZ = sign + endZ;
                absDz = absDx + 1;
            }
        } else {
            const int sign = startX < endX ? -1 : 1;
            curX = sign + endX;
            absDx = absDz + 1;
        }
    } else {
        curZ = startZ;  // one cell off the axis: snap to the horizontal
        absDz = 0;
    }

    if (!placement_.straightOrDiagonalOnly) {
        if (allowDiagonals) {
            if (absDz * 2 < absDx) {
                curZ = startZ;
                absDz = 0;
            } else if (absDz <= absDx * 2) {
                if (penWidthX == 1 && absDz < absDx) {
                    absDx = absDz + 1;
                } else {
                    const bool dzLonger = absDx < absDz;
                    if (absDz < absDx) absDx = absDz;
                    absDz = absDx + ((dzLonger && penWidthX == 1) ? 1u : 0u);
                }
                const int signX = endX < startX ? -1 : 1;
                curX = signX * static_cast<int>(absDx) + startX;
                const int signZ = endZ < startZ ? -1 : 1;
                curZ = signZ * static_cast<int>(absDz) + startZ;
            } else {
                curX = startX;
                absDx = 0;
            }
        }
    } else if (absDz < absDx) {
        curZ = startZ;
        absDz = 0;
    } else {
        curX = startX;
        absDx = 0;
    }
    end.x = static_cast<std::uint32_t>(curX);
    end.z = static_cast<std::uint32_t>(curZ);

    // 8-way direction codes from the original (un-snapped) end point.
    const bool northwards = startZ < endZ;
    const int nsCode = northwards ? 7 : 1;
    int ewCode = 3 + 2 * static_cast<int>(northwards);
    if (endX <= startX) ewCode = nsCode;

    const bool zLonger = absDx < absDz;
    const bool equal = absDx == absDz;
    std::uint32_t steps = absDz;
    int mainCode = 2 + 4 * static_cast<int>(northwards);
    if (!zLonger && !equal) {
        steps = absDx;
        mainCode = (startX < endX) ? 4 : 0;
    }
    totalSteps = static_cast<int>(steps) + 1;
    isVertical = zLonger || equal;
    isHorizontal = !zLonger && !equal;
    directionA = mainCode;
    directionB = ewCode;

    if (absDx == 0) {
        straightSteps = static_cast<int>(absDz) + 1;
        return;
    }
    if (absDz == 0) {
        straightSteps = static_cast<int>(absDx) + 1;
        return;
    }
    int diff = static_cast<int>(absDx) - static_cast<int>(absDz);
    if (diff < 0) diff = -diff;
    straightSteps = diff;
    if (penWidthZ != 1) return;
    if (diff != 1) return;
    if (totalSteps < 2) return;
    straightSteps = 0;
}

// ============================================================================================================
// the drag: line, cells, segments (recovered from the Mac i386 build; i386 offsets in the comments)
// ============================================================================================================

// i386 0x00122c58: a new UI for this tool, one reference, already bound to the tool and its network type.
bool cSC4NetworkTool::GetUITool(cISC4NetworkToolUI** out) {
    auto* ui = new cSC4NetworkToolUI();
    ui->AddRef();
    ui->Init(this, networkType_);
    *out = ui;
    return true;
}

// i386 0x0012563e: the preview layer and the preview occupants (+0x25c) go. The preview occupant set is not modelled
// yet (unknown460_), so only the layer is cleared.
void cSC4NetworkTool::ClearCellPreviews() {
    if (preview_) preview_->Clear();
}

// i386 0x0012519a: every highlighted occupant (+0x23c) gets highlight 0 and the set and the cell map (+0x238) go.
// Not modelled yet (unknown460_): nothing is ever highlighted.
void cSC4NetworkTool::ClearOccupantHighlights() {}

// i386 0x0012b18c. Not ported yet: the multi-type intersection query (it only asks when the solve found some).
void cSC4NetworkTool::QueryUserForPotentialIntersections() {}

// i386 0x00124dbe. The per-cell part: each newly solved cell (status) that is not +0x5a costs the most expensive of
// its network types' per-cell costs (type info +0xdc), and every corner the solve moved costs |terrain - solved| times
// the terrain-change factor (params, i386 +0x368). The bridge, tunnel and intersection terms read vectors the
// unported solver steps fill; they stay empty, so they add nothing yet. The rounding is (float)x + 1.5 * 2^23.
int cSC4NetworkTool::GetCostOfSolution() {
    INetworkToolHost* host = GetNetworkToolHost();
    std::int32_t cost = 0;
    float heightChange = 0.0f;
    for (const tSolvedCell& solved : solvedCellList_) {
        cSC4NetworkCellInfo* cell = worldCache_ ? worldCache_->GetCell(solved.cellKey) : nullptr;
        if (!cell) continue;
        if (cell->status != 0 && cell->unknown5a == 0) {
            std::uint32_t most = 0;
            for (std::uint32_t type = 0; type < 13; ++type) {
                const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(type) : nullptr;
                if ((kFlagForNetworkType[type] & cell->networkFlags) != 0 && info && most < info->CostPerCell()) most = info->CostPerCell();
            }
            cost += static_cast<std::int32_t>(most);
        }
        for (int i = 0; i < 4; ++i) {
            const float terrain = host ? host->TerrainVertexHeight(cell->vertexBaseHeights[i]) : 0.0f;
            const float solvedHeight = cell->vertexHeights[i];
            heightChange += terrain <= solvedHeight ? solvedHeight - terrain : terrain - solvedHeight;
        }
    }
    float rounded = heightChange * placement_.floatA8[8] + 12582912.0f;
    std::int32_t bits;
    std::memcpy(&bits, &rounded, sizeof bits);
    return bits - 0x4b400000 + cost + baseCost_;
}

// i386 0x004cdd0a.
bool cSC4NetworkTool::DrawNetworkLine(SC4PointU& start, SC4PointU& end, bool allowDiagonals, std::uint32_t networkType) {
    INetworkToolHost* host = GetNetworkToolHost();
    const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(networkType) : nullptr;
    const int penX = info ? info->PenWidthX() : 1;
    const int penZ = info ? info->PenWidthZ() : 1;
    steps_.clear();
    totalSteps_ = 0;
    cells_.clear();
    int zMajor = 0, xMajor = 0;
    BreakIntoStraightAndDiagSegments(start, end, totalSteps_, straightSteps_, directionA_, directionB_, zMajor, xMajor,
                                     allowDiagonals, penX, penZ);
    stepIsHorizontal_ = xMajor != 0;
    if (placement_.straightOrDiagonalOnly && totalSteps_ != straightSteps_) return false;
    ComputeDraggedCells(start, end, zMajor, xMajor, networkType);
    if (penX > 1 || penZ > 1) {
        // A wide pen must stay inside the city.
        for (int s = 0; s < totalSteps_; ++s) {
            const tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
            for (std::uint32_t c = step.firstCell; c <= step.lastCell; ++c) {
                const SC4PointU& cell = cells_[c];
                if (static_cast<std::uint32_t>(cityWidth_) <= cell.x || static_cast<std::uint32_t>(cityHeight_) <= cell.z) {
                    totalSteps_ = 0;
                    steps_.clear();
                    cells_.clear();
                    return false;
                }
            }
        }
    }
    return true;
}

// i386 0x004cc96a. A Bresenham walk from start to end that groups the cells into steps along the long axis: on a
// z-major drag every z move starts a step and x moves widen it (the step spans an x range), and the other way round.
// On a tie the walk moves z first, then x, alternating. The pen widths widen the steps' ranges; then every step's
// cells are listed (+0x60) and the steps record their half-open cell ranges (+0x54).
// Not ported yet: the wide-pen pass after that (pen x > 1, the highways: it checks the ends against existing network
// occupants through cSC4PlacedNetworkOccupantFilter and extends or shifts the end steps, 0x004ccd48..0x004cdc4e).
void cSC4NetworkTool::ComputeDraggedCells(const SC4PointU& start, const SC4PointU& end, int zMajor, int xMajor,
                                          std::uint32_t networkType) {
    struct StepBound { int min, max; };
    INetworkToolHost* host = GetNetworkToolHost();
    const INetworkTypeInfo* info = host ? host->NetworkTypeInfo(networkType) : nullptr;
    const std::uint32_t penX = info ? static_cast<std::uint32_t>(info->PenWidthX()) : 1;
    const std::uint32_t penZ = info ? static_cast<std::uint32_t>(info->PenWidthZ()) : 1;
    const bool penDirIsX = zMajor != 0;   // tStepBound::sbPenDirIsX
    std::vector<StepBound> bounds(static_cast<std::size_t>(totalSteps_) + 1, StepBound{0, 0});
    bounds[0] = StepBound{100000, -100000};
    int x = static_cast<int>(start.x), z = static_cast<int>(start.z);
    std::size_t k = 0;
    bool zFirst = true;
    for (;;) {
        StepBound& b = bounds[k];
        const int coord = penDirIsX ? x : z;
        if (coord < b.min) b.min = coord;
        if (b.max < coord) b.max = coord;
        if ((static_cast<std::uint32_t>(x) == end.x && static_cast<std::uint32_t>(z) == end.z) || k == static_cast<std::size_t>(totalSteps_)) break;
        const int dx = static_cast<int>(end.x) - x, dz = static_cast<int>(end.z) - z;
        const std::uint32_t adx = static_cast<std::uint32_t>(dx < 0 ? -dx : dx), adz = static_cast<std::uint32_t>(dz < 0 ? -dz : dz);
        if (adz < adx || (adz <= adx && !zFirst)) {
            x += dx < 1 ? -1 : 1;
            if (!zMajor) bounds[++k] = StepBound{100000, -100000};
            zFirst = true;
        } else {
            z += dz < 1 ? -1 : 1;
            if (!xMajor) bounds[++k] = StepBound{100000, -100000};
            zFirst = false;
        }
    }
    // The pen widths: a wide pen grows the range away from the drag's side (direction B).
    const auto penDelta = [&](std::uint32_t pen) {
        const bool negative = directionB_ == 1 || (zMajor ? directionB_ == 7 : directionB_ == 3);
        return negative ? 1 - static_cast<int>(pen) : static_cast<int>(pen) - 1;
    };
    const auto grow = [](StepBound& b, int delta) { if (delta < 0) b.min += delta; else b.max += delta; };
    if (penX > 1) {
        const int delta = penDelta(penX);
        for (int s = 0; s < straightSteps_; ++s) grow(bounds[static_cast<std::size_t>(s)], delta);
    }
    if (penZ > 1) {
        const int delta = penDelta(penZ);
        if (straightSteps_ < totalSteps_) {
            int s = straightSteps_;
            StepBound& first = bounds[static_cast<std::size_t>(s)];
            if (s < 1) {
                if (first.min == first.max) grow(first, delta);
            } else {
                grow(first, delta);
            }
            while (++s < totalSteps_ - 1) grow(bounds[static_cast<std::size_t>(s)], delta);
            StepBound& last = bounds[static_cast<std::size_t>(totalSteps_ - 1)];
            if (last.min == last.max) grow(last, delta);
        }
    }
    steps_.assign(static_cast<std::size_t>(totalSteps_), tDraggedStep{});
    const bool backwards = zMajor ? end.z <= start.z : end.x <= start.x;
    const int sign = backwards ? -1 : 1;
    std::uint32_t next = 0;
    int along = 0;
    for (int s = 0; s < totalSteps_; ++s) {
        const StepBound& b = bounds[static_cast<std::size_t>(s)];
        const int count = b.max - b.min + 1;
        tDraggedStep& step = steps_[static_cast<std::size_t>(s)];
        step.firstCell = next;
        next += static_cast<std::uint32_t>(count);
        step.lastCell = next - 1;
        step.direction = 0;
        for (int i = 0; i < count; ++i) {
            if (!zMajor) cells_.push_back(SC4PointU{static_cast<std::uint32_t>(along + static_cast<int>(start.x)), static_cast<std::uint32_t>(i + b.min)});
            else cells_.push_back(SC4PointU{static_cast<std::uint32_t>(i + b.min), static_cast<std::uint32_t>(along + static_cast<int>(start.z))});
        }
        along += sign;
    }
    if (penX > 1) {
        static bool warned = false;
        if (!warned) { warned = true; std::fprintf(stderr, "network tool: the wide-pen pass of ComputeDraggedCells is not ported (highways)\n"); }
    }
}

// i386 0x004cf1ce: every anchor segment is drawn, its previews cleared and solved (and placed when `place`).
// Not ported yet: the construction crew a real placement of a crewed type (mask 0x1f4f) builds the segments with
// (cSC4NetworkConstructionCrew, new(0x58)); the occupants are placed directly.
void cSC4NetworkTool::PlaceAllSegments(bool place, bool allowDiagonals, bool connectNeighbor, std::uint32_t networkType) {
    const bool savedFlag = unknown9a_;
    unknown9a_ = false;
    ReloadCheck();
    for (std::size_t i = 0; i + 1 < anchors_.size(); ++i) {
        SC4PointU from = anchors_[i], to = anchors_[i + 1];
        DrawNetworkLine(from, to, allowDiagonals, networkType);
        ClearCellPreviews();
        PlaceNetwork(place, allowDiagonals, connectNeighbor, networkType);
    }
    unknown9a_ = savedFlag;
}


}  // namespace sc4
