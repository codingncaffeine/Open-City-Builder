#include "sc4/NetworkManager.h"

#include "sc4/NetworkTool.h"   // eNetworkType enumerators, kNetworkTypeCount, kFlagForNetworkType

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace sc4 {

static_assert(kNetworkToolCount == static_cast<std::size_t>(kNetworkTypeCount),
              "the tool array is indexed by cISC4NetworkManager::eNetworkType");

namespace {
INetworkManagerHost* g_host = nullptr;

// kResolverTypeForClass @ 0x100a194fc: the tool each destroy class goes through.
constexpr eNetworkType kNetworkDestroyResolverTypes[3] = {kNetworkRoad, kNetworkPipe, kNetworkSubway};

// The two tiny topology tables every network walk in the class uses
// (nSC4Networks::kNextX @ 0x100a19550 and kNextZ @ 0x100a19560, read with scripts/macho_read.py).
[[maybe_unused]] constexpr int kNextX[4] = {-1, 0, 1, 0};
[[maybe_unused]] constexpr int kNextZ[4] = {0, -1, 0, 1};

// cISC4NetworkOccupant query helper: on success *out is the interface pointer with one reference, as
// QueryInterface hands it back (the original stores it without a cast and releases it explicitly).
NetworkOccupantViewProvider g_viewProvider = nullptr;

bool QueryNetworkOccupant(cISC4Occupant* occupant, INetworkManagerOccupant** out) {
    *out = nullptr;
    if (occupant == nullptr) return false;
    if (g_viewProvider != nullptr) {
        *out = g_viewProvider(occupant);
        return *out != nullptr;
    }
    void* raw = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4NetworkOccupant, &raw) || raw == nullptr) return false;
    *out = static_cast<INetworkManagerOccupant*>(raw);
    return true;
}

// The DoMessage callback the occupant manager's IterateOccupants call runs (0x100170886). It is a nested class
// in the original (cSC4NetworkManager::DoMessage::cRefreshBridgeModels::Go); the port keeps the shape.
struct cRefreshBridgeModels {
    static bool Go(cISC4Occupant* occupant, void* context);
};

}  // namespace

// cSC4NetworkTool::kNetworkTypeNames @ 0x100a7a8a0: the thirteen type names, the same strings the "Shadow
// remap by edges" section matches case-insensitively (the binary stores pointers 0x1009f08d6..0x1009f0929).
const char* const kNetworkTypeNameTable[kNetworkToolCount] = {
    "Road", "Rail", "Highway", "Street", "Pipe", "PowerLine", "Avenue",
    "Subway", "LightRail", "Monorail", "OneWayRoad", "DirtRoad", "GroundHighway",
};

INetworkManagerHost* NetworkManagerHost() { return g_host; }
void SetNetworkManagerHost(INetworkManagerHost* host) { g_host = host; }
void SetNetworkOccupantViewProvider(NetworkOccupantViewProvider provider) { g_viewProvider = provider; }

// ============================================================================================================
// construction / destruction
// ============================================================================================================

// 0x10016dffe. The three vptrs go in at +0/+8/+0x10, the counters and the region rectangle are reset, all
// containers are default-constructed and Reload reads the remap resource. Note +0x1c/+0x1d are cleared
// separately from +0x1e (the inited flag then the path/region flags).
cSC4NetworkManager::cSC4NetworkManager() {
    refCount_ = 0;
    inited_ = false;
    ukPathMapping_ = false;
    extendRegion_ = false;
    regionMinX_ = 0x7fffffff;
    regionMinZ_ = 0x7fffffff;
    regionMaxX_ = -1;
    regionMaxZ_ = -1;
    Reload();
}

// The destructor body (0x10016e486). Shutdown releases the tracked occupants, the retrofit list and the
// tools; the loop below then releases any tool Shutdown did not see (when the object was never Init'ed) in
// the original's descending order +0x168..+0x108. The containers free their own storage.
cSC4NetworkManager::~cSC4NetworkManager() {
    Shutdown();
    for (int type = kNetworkTypeCount - 1; type >= 0; --type) {
        INetworkManagerTool* tool = networkTools_[type];
        if (tool != nullptr) {
            networkTools_[type] = nullptr;
            tool->Release();
        }
    }
    // Quirk kept: the construction crews in +0xf0 keep their references (the original only frees the vector
    // storage in the destructor; only message 0x26d31ec2 releases the crews).
}

// ============================================================================================================
// COM identity
// ============================================================================================================

// 0x100170e1e: iid 1 and 0xa9764c8c answer the primary object, 0xe4fda3d4 the cIGZSerializable sub-object
// (+0x10) and 0x452294aa the cIGZMessageTarget2 one (+8). It re-enters AddRef through vslot 8.
bool cSC4NetworkManager::QueryInterface(std::uint32_t iid, void** out) {
    if (out == nullptr) return false;
    if (iid == 1 || iid == kGZIID_cISC4NetworkManager) {
        *out = static_cast<cISC4NetworkManager*>(this);
    } else if (iid == kGZIID_cIGZSerializable) {
        *out = static_cast<cIGZSerializable*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// 0x10016f38a: the counter starts at 0 in the constructor.
std::uint32_t cSC4NetworkManager::AddRef() { return ++refCount_; }

// 0x100170e7e: only a count above one survives; otherwise the object calls its deleting destructor (vslot
// 0xc8 = 200) and returns 0.
std::uint32_t cSC4NetworkManager::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

std::uint32_t cSC4NetworkManager::GetGZCLSID() const { return kGZCLSID_cSC4NetworkManager; }

// ============================================================================================================
// remap tables (Reload and its four ini callbacks)
// ============================================================================================================

// 0x10016e1ec. Clears the four tables and reads them from the {0x8a5971c5, 0, 0x8a5993b9} ini resource. The
// resource open and the file object are behind the host seam (SC4GetResourceAsFile); the ini is deleted at the
// end, which releases the file (cRZIniFile owns one reference).
void cSC4NetworkManager::Reload() {
    textureRemaps_.clear();
    shadowTextureRemaps_.clear();
    shadowRemapByEdges_.clear();
    pathRemaps_.clear();
    if (g_host == nullptr) return;
    cRZIniFile* ini = g_host->OpenNetworkIni();
    if (ini == nullptr) return;
    ini->EnumEntries(cRZString("Texture remapping"), &cSC4NetworkManager::AddTextureRemapEntryCallback, this);
    ini->EnumEntries(cRZString("Shadow remap"), &cSC4NetworkManager::AddShadowRemapEntryCallback, this);
    ini->EnumEntries(cRZString("Shadow remap by edges"),
                     &cSC4NetworkManager::AddShadowRemapByEdgesEntryCallback, this);
    ini->EnumEntries(cRZString("UK path mapping"), &cSC4NetworkManager::AddPathRemapEntryCallback, this);
    delete ini;
}

// 0x10016ec7e. Straight append (the original's push_back; the vector's slow path is libc++).
void cSC4NetworkManager::AddTextureMapping(std::uint32_t matchValue, std::uint32_t matchMask,
                                           std::uint32_t addValue, std::uint32_t transformMask,
                                           std::uint8_t alphaDelta, bool stop,
                                           std::uint32_t subtractMask) {
    TextureRemapEntry entry;
    entry.matchValue = matchValue;
    entry.matchMask = matchMask;
    entry.addValue = addValue;
    entry.transformMask = transformMask;
    entry.subtractMask = subtractMask;
    entry.alphaDelta = alphaDelta;
    entry.stop = stop;
    entry.padding = 0;   // the original copies two uninitialised stack bytes here (never read)
    textureRemaps_.push_back(entry);
}

// 0x10016edc8: the same record in the "Shadow remap" vector (+0x88). The two functions are separate in the
// binary even though the body is identical.
void cSC4NetworkManager::AddShadowMapping(std::uint32_t matchValue, std::uint32_t matchMask,
                                          std::uint32_t addValue, std::uint32_t transformMask,
                                          std::uint8_t alphaDelta, bool stop, std::uint32_t subtractMask) {
    TextureRemapEntry entry;
    entry.matchValue = matchValue;
    entry.matchMask = matchMask;
    entry.addValue = addValue;
    entry.transformMask = transformMask;
    entry.subtractMask = subtractMask;
    entry.alphaDelta = alphaDelta;
    entry.stop = stop;
    entry.padding = 0;
    shadowTextureRemaps_.push_back(entry);
}

// 0x10016ee80. "value" is "match, mask, add, transform, alphaLow2, alphaBit7, stop, subtract" in hex/decimal.
// The original scans with " %lx , %lx , %lx , %lx , %d , %d , %d , %lx" into two-slot stack words; the port
// scans into unsigned long and narrows (the sscanf %lx mismatch is why the original's words overlap).
void cSC4NetworkManager::AddTextureRemapEntryCallback(const cRZString& key, const cRZString& value,
                                                     void* context) {
    (void)key;
    auto* self = static_cast<cSC4NetworkManager*>(context);
    unsigned long a = 0, b = 0, c = 0, d = 0, last = 0;
    int alphaLow = 0, alphaBit7 = 0, stop = 0;
    const int fields = std::sscanf(value.ToChar(), " %lx , %lx , %lx , %lx , %d , %d , %d , %lx", &a, &b,
                                   &c, &d, &alphaLow, &alphaBit7, &stop, &last);
    if (fields < 4) return;
    const auto threshold = static_cast<std::uint8_t>(((alphaBit7 != 0) << 7) | (alphaLow & 3));
    self->AddTextureMapping(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b),
                            static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(d), threshold,
                            stop != 0, static_cast<std::uint32_t>(last));
}

// 0x10016ef53: the identical parser feeding the shadow vector.
void cSC4NetworkManager::AddShadowRemapEntryCallback(const cRZString& key, const cRZString& value,
                                                    void* context) {
    (void)key;
    auto* self = static_cast<cSC4NetworkManager*>(context);
    unsigned long a = 0, b = 0, c = 0, d = 0, last = 0;
    int alphaLow = 0, alphaBit7 = 0, stop = 0;
    const int fields = std::sscanf(value.ToChar(), " %lx , %lx , %lx , %lx , %d , %d , %d , %lx", &a, &b,
                                   &c, &d, &alphaLow, &alphaBit7, &stop, &last);
    if (fields < 4) return;
    const auto threshold = static_cast<std::uint8_t>(((alphaBit7 != 0) << 7) | (alphaLow & 3));
    self->AddShadowMapping(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b),
                           static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(d), threshold,
                           stop != 0, static_cast<std::uint32_t>(last));
}

// 0x10016f026. "name, piece, add, transform, alphaLow2, alphaBit7": the name selects the network type by
// case-insensitive comparison with kNetworkTypeNames; unknown names are dropped. Duplicates are allowed (the
// original inserts through __insert_multi).
void cSC4NetworkManager::AddShadowRemapByEdgesEntryCallback(const cRZString& key, const cRZString& value,
                                                           void* context) {
    (void)key;
    auto* self = static_cast<cSC4NetworkManager*>(context);
    char name[65] = {};
    unsigned long piece = 0, add = 0, transform = 0;
    int alphaLow = 0, alphaBit7 = 0;
    const int fields = std::sscanf(value.ToChar(), " %64[^, ] , %lx , %lx , %lx , %d , %d", name, &piece,
                                   &add, &transform, &alphaLow, &alphaBit7);
    if (fields < 4) return;
    for (std::uint32_t type = 0; type < kNetworkTypeCount; ++type) {
        if (::strcasecmp(name, kNetworkTypeNameTable[type]) != 0) continue;
        ShadowRemapByEdgesEntry entry;
        entry.networkType = type;
        entry.addValue = static_cast<std::uint32_t>(add);
        entry.transformMask = static_cast<std::uint32_t>(transform);
        entry.alphaDelta = static_cast<std::uint8_t>(((alphaBit7 != 0) << 7) | (alphaLow & 3));
        self->shadowRemapByEdges_.emplace(static_cast<std::uint32_t>(piece), entry);
        return;
    }
}

// 0x10016f138: "match, mask, add, transform".
void cSC4NetworkManager::AddPathRemapEntryCallback(const cRZString& key, const cRZString& value,
                                                   void* context) {
    (void)key;
    auto* self = static_cast<cSC4NetworkManager*>(context);
    unsigned long a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(value.ToChar(), " %lx , %lx , %lx , %lx", &a, &b, &c, &d) < 4) return;
    PathRemapEntry entry;
    entry.matchValue = static_cast<std::uint32_t>(a);
    entry.matchMask = static_cast<std::uint32_t>(b);
    entry.addValue = static_cast<std::uint32_t>(c);
    entry.transformMask = static_cast<std::uint32_t>(d);
    self->pathRemaps_.push_back(entry);
}

// 0x10016ec20. Preview texture remap: the first matching entry transforms the texture; the alpha delta is
// added (bit 7 clear) or subtracted through (delta + 8) (bit 7 set), then masked with 0x83 (quirk: not 0x7f).
// A matching entry with its stop byte set ends the scan; otherwise the scan continues with the new values.
std::uint32_t cSC4NetworkManager::MapPreviewTexture(std::uint32_t texture, std::uint8_t& alpha) {
    for (const TextureRemapEntry& entry : textureRemaps_) {
        if ((entry.matchMask & texture) != entry.matchValue) continue;
        const signed char delta = static_cast<signed char>(entry.alphaDelta);
        std::uint8_t newAlpha;
        if (delta < 0) {
            newAlpha = static_cast<std::uint8_t>((delta + 8) - alpha);
        } else {
            newAlpha = static_cast<std::uint8_t>(delta + alpha);
        }
        texture = ((entry.transformMask & texture) + entry.addValue) - (texture & entry.subtractMask);
        alpha = static_cast<std::uint8_t>(newAlpha & 0x83);
        if (entry.stop) return texture;
    }
    return texture;
}

// 0x10016ece4. Shadow remap. An entry with a non-zero add word is applied directly; a zero one looks the
// fourth argument up in the "by edges" multimap and uses the first entry whose network type flag equals
// (network & 0x304). Either way the *vector* entry's alpha delta then adjusts the alpha, and its stop byte
// returns. Note the map entry replaces the alpha before the vector delta is applied (original behaviour).
std::uint32_t cSC4NetworkManager::MapShadowTexture(std::uint32_t texture, std::uint8_t& alpha,
                                                   std::uint32_t network, std::uint32_t piece) {
    for (const TextureRemapEntry& entry : shadowTextureRemaps_) {
        if ((entry.matchMask & texture) != entry.matchValue) continue;
        if (entry.addValue == 0) {
            const auto range = shadowRemapByEdges_.equal_range(piece);
            bool applied = false;
            for (auto it = range.first; it != range.second; ++it) {
                if (kFlagForNetworkType[it->second.networkType] == (network & 0x304)) {
                    texture = (texture & it->second.transformMask) + it->second.addValue;
                    alpha = it->second.alphaDelta;
                    applied = true;
                    break;
                }
            }
            if (!applied) continue;
        } else {
            texture = ((entry.transformMask & texture) + entry.addValue) - (texture & entry.subtractMask);
        }
        const signed char delta = static_cast<signed char>(entry.alphaDelta);
        const std::uint8_t newAlpha =
            delta < 0 ? static_cast<std::uint8_t>((delta + 8) - alpha) : static_cast<std::uint8_t>(delta + alpha);
        alpha = static_cast<std::uint8_t>(newAlpha & 0x83);
        if (entry.stop) return texture;
    }
    return texture;
}

// 0x10016ee44. UK path mapping is only applied when the language flag Init stores in +0x1d is set.
std::uint32_t cSC4NetworkManager::MapPath(std::uint32_t path) {
    if (!ukPathMapping_) return path;
    for (const PathRemapEntry& entry : pathRemaps_) {
        if ((entry.matchMask & path) == entry.matchValue) {
            return (path & entry.transformMask) + entry.addValue;
        }
    }
    return path;
}

// ============================================================================================================
// the thirteen network tools
// ============================================================================================================

// 0x100170b.. (the repeated block in Init): keep one AddRef'd tool per type.
void cSC4NetworkManager::SetNetworkTool(eNetworkType type, INetworkManagerTool* tool) {
    const auto index = static_cast<std::size_t>(type);
    if (index >= static_cast<std::size_t>(kNetworkTypeCount)) return;
    if (networkTools_[index] == tool) return;
    if (tool != nullptr) tool->AddRef();
    INetworkManagerTool* previous = networkTools_[index];
    networkTools_[index] = tool;
    if (previous != nullptr) previous->Release();
}

void cSC4NetworkManager::AdoptNetworkTool(eNetworkType type, INetworkManagerTool* tool) {
    const auto index = static_cast<std::size_t>(type);
    if (index < static_cast<std::size_t>(kNetworkTypeCount) && networkTools_[index] == nullptr) SetNetworkTool(type, tool);
}

// 0x10016e6c8. create == false hands out the borrowed tool Init made (the caller AddRefs it);
// create == true allocates a fresh one and does *not* store it. Out-of-range types answer null.
INetworkManagerTool* cSC4NetworkManager::GetNetworkTool(eNetworkType type, bool create) {
    if (static_cast<std::uint32_t>(type) >= static_cast<std::uint32_t>(kNetworkTypeCount)) return nullptr;
    if (!create) return networkTools_[static_cast<std::size_t>(type)];
    if (g_host == nullptr) return nullptr;
    // The original switch: 0x638-byte cSC4NetworkTool(new, 0/1/2/3/6/8/9/10/11/12), 0x698 cSC4PipeTool,
    // 0x6e8 cSC4PowerLineTool, 0x698 cSC4SubwayTool. Those classes are behind the host factory.
    return g_host->CreateNetworkTool(type);
}

// ============================================================================================================
// occupants that ask to be tracked / the removal queue
// ============================================================================================================

// 0x10016e8d8. Two modes.
//  * run immediately (queue inactive): for each of the three class masks, gather the occupants whose
//    cISC4NetworkOccupant::GetNetworkFlags (+0x60) intersects the mask and let that group's tool
//    (resolver types 0, 4, 7 = road, pipe, subway) destroy them through vslot 0x190.
//  * queue (queue active): AddRef every occupant and push it into +0xb8 (QueueRemovals drains it later).
void cSC4NetworkManager::DestroyNetworkOccupants(const ilist<cISC4Occupant*>& occupants) {
    if (!removalQueueActive_) {
        for (int group = 0; group < 3; ++group) {
            std::vector<INetworkManagerOccupant*> filtered;
            for (cISC4Occupant* occupant : occupants) {
                INetworkManagerOccupant* networkOccupant = nullptr;
                if (!QueryNetworkOccupant(occupant, &networkOccupant)) continue;
                if ((networkOccupant->GetNetworkFlags() & kNetworkDestroyClassMasks[group]) != 0) {
                    filtered.push_back(networkOccupant);
                    networkOccupant->AddRef();   // the QI reference is handed to the tool; take one more
                }
                networkOccupant->Release();
            }
            if (filtered.empty()) continue;
            INetworkManagerTool* tool = GetNetworkTool(kNetworkDestroyResolverTypes[group], false);
            if (tool == nullptr) continue;   // the original calls through null here (quirk, guarded)
            tool->AddRef();
            tool->Init();
            tool->DestroyOccupantsAndResolve(filtered);
            tool->Shutdown();
            tool->Release();
        }
        return;
    }
    for (cISC4Occupant* occupant : occupants) {
        if (occupant == nullptr) continue;
        occupant->AddRef();
        removalQueue_.push_back(occupant);
    }
}

// 0x10016eaee. Turns the queue on/off; turning it off drains it: sort by pointer, release the duplicates
// (every queued entry owns one reference), let DestroyNetworkOccupants run the tools and hand the list to the
// occupant manager so it releases the entries' references and the occupants leave the city.
void cSC4NetworkManager::QueueRemovals(bool queue) {
    if (removalQueueActive_ == queue) return;
    removalQueueActive_ = queue;
    if (queue) return;
    if (removalQueue_.empty()) return;
    std::sort(removalQueue_.begin(), removalQueue_.end(), [](cISC4Occupant* a, cISC4Occupant* b) {
        return reinterpret_cast<std::uintptr_t>(a) < reinterpret_cast<std::uintptr_t>(b);
    });
    ilist<cISC4Occupant*> unique;
    cISC4Occupant* previous = nullptr;
    for (cISC4Occupant* occupant : removalQueue_) {
        if (occupant == previous) {
            if (occupant != nullptr) occupant->Release();   // duplicate queue entries hold one reference each
        } else {
            unique.push_back(occupant);
            previous = occupant;
        }
    }
    removalQueue_.clear();
    DestroyNetworkOccupants(unique);
    if (g_host != nullptr) {
        if (INetworkManagerOccupantManager* manager = g_host->OccupantManager()) manager->ReleaseOccupantList(unique);
    }
    unique.clear();
}

// 0x100170230 — PARTIAL. The retrofit pass walks the +0xd8 list, asks every entry's CheckForRetrofit
// (occupant vslot 0x30), collects its intersection pieces (this class's vslot 0x60 -> the recursion in
// GetAllIntersectionPieces2, itself unrecovered), reads the connection-node edge pair (occupant vslot 0x238),
// finds the cell occupant through the occupant manager (+0x108) and rewrites the old piece ids through the
// 10-entry kOldIntxPieceToNewIntxPiece table (0x100a19570; the original's loop runs 11 iterations and reads
// the 11th pair past the table) plus the position offsets at 0x100a19574. It then calls AddModel
// (cSC4NetworkOccupantWithPreBuiltModel 0x1001ba886) with the placement offsets. None of the occupant slots
// 0x1a0/0xd0/0x238 nor the table's 11th entry is understood yet, so the body is left out.
void cSC4NetworkManager::Retrofit() {}

// 0x1001700d8. When the view rotation changes, every network occupant in the city is asked to rebuild its
// rotation-dependent state (occupant manager vslot 0x120 = GetOccupantsByBBox with a null rectangle, type
// 0xc772bf98, unlimited; then cISC4NetworkOccupant vslot 0x170 on each). The list entries carry one reference:
// the QI reference is released after the call and the list reference is released by the loop itself; the
// original then clears the list (the nodes have already lost their references — a double clear is avoided
// because GetOccupantsByBBox itself does not take references beyond the list's).
void cSC4NetworkManager::DoViewRotationChange(int oldRotation, int newRotation) {
    if (oldRotation == newRotation) return;
    if (g_host == nullptr) return;
    INetworkManagerOccupantManager* manager = g_host->OccupantManager();
    if (manager == nullptr) return;
    ilist<cISC4Occupant*> occupants;
    manager->GetOccupantsByBBox(occupants, nullptr, nullptr, kNetworkOccupantType, 0xffffffffu);
    for (cISC4Occupant* occupant : occupants) {
        if (occupant == nullptr) continue;
        INetworkManagerOccupant* networkOccupant = nullptr;
        if (QueryNetworkOccupant(occupant, &networkOccupant)) {
            networkOccupant->UpdateViewRotation();
            networkOccupant->Release();
        }
        occupant->Release();
    }
    occupants.clear();
}

// 0x100171c50. Takes one reference and prepends (the original links the node right after the sentinel; list
// order is not observable to any recovered caller).
void cSC4NetworkManager::AddToRetrofitList(cISC4Occupant* occupant) {
    if (occupant == nullptr) return;
    occupant->AddRef();
    retrofitList_.push_front(occupant);
}

// ============================================================================================================
// construction crews
// ============================================================================================================

// 0x10016f1dc
void cSC4NetworkManager::RegisterConstructionCrew(cSC4NetworkConstructionCrew* crew) {
    if (crew == nullptr) return;
    constructionCrews_.push_back(crew);
    crew->AddRef();
}

// 0x10016f226: remove the first match and drop its reference. A crew that is not registered is ignored.
void cSC4NetworkManager::UnregisterConstructionCrew(cSC4NetworkConstructionCrew* crew) {
    const auto it = std::find(constructionCrews_.begin(), constructionCrews_.end(), crew);
    if (it == constructionCrews_.end()) return;
    constructionCrews_.erase(it);
    if (crew != nullptr) crew->Release();
}

// ============================================================================================================
// pipe / subway forwarders and the occupant filters (all unrecovered neighbours behind the host seam)
// ============================================================================================================

// 0x10016f2a8: cSC4SubwayOccupantFilter::IsOccupantSubwayStation @ 0x100192d40.
bool cSC4NetworkManager::IsOccupantSubwayStation(cISC4Occupant* occupant) {
    return g_host != nullptr && g_host->IsOccupantSubwayStation(occupant);
}

// 0x10016f2b6: cSC4PipeTool::FindPipeOccupant @ 0x1001932f0.
void cSC4NetworkManager::FindPipeOccupant(int x, int z, cSC4PipeOccupant*& out) {
    out = nullptr;
    if (g_host != nullptr) g_host->FindPipeOccupant(x, z, out);
}

// 0x10016f2c8: cSC4PipeTool::FindPipeConnectionOccupant @ 0x100193402.
void cSC4NetworkManager::FindPipeConnectionOccupant(int x, int z, cSC4PipeConnectionOccupant*& out) {
    out = nullptr;
    if (g_host != nullptr) g_host->FindPipeConnectionOccupant(x, z, out);
}

// 0x10016f2da / 0x10016f332: new(0x18) cSC4WaterOccupantFilter (vtable 0x100a7b338) / cSC4SubwayOccupantFilter
// (vtable 0x100a7af58), each with one reference.
cISC4OccupantFilter* cSC4NetworkManager::GetWaterOccupantFilter() {
    return g_host != nullptr ? g_host->CreateOccupantFilter(false) : nullptr;
}

cISC4OccupantFilter* cSC4NetworkManager::GetSubwayOccupantFilter() {
    return g_host != nullptr ? g_host->CreateOccupantFilter(true) : nullptr;
}

// 0x100171c3c: the English type name straight out of the shared table (no bounds check in the original).
const char* cSC4NetworkManager::GetEnglishNetworkName(eNetworkType type) {
    return kNetworkTypeNameTable[static_cast<std::uint32_t>(type)];
}

// ============================================================================================================
// persistence
// ============================================================================================================

// 0x100171cb2. u16 version 1, u32 count, then each occupant through SC4ReadGZSerializable(0xa821ef94, false);
// every read occupant is appended to the tracked-occupant vector (+0x30) with the reference the read returned.
// Returns !stream.GetError().
bool cSC4NetworkManager::Read(cIGZIStream& stream) {
    if (stream.GetError()) return false;
    std::uint16_t version = 0;
    if (!stream.GetUint16(version) || version != 1) return false;
    std::uint32_t count = 0;
    if (!stream.GetUint32(count)) return false;
    if (g_host == nullptr) return false;
    for (std::uint32_t i = count; i != 0; --i) {
        void* raw = nullptr;
        if (!g_host->ReadGZSerializable(stream, kGZIID_cISC4NetworkOccupant, &raw)) return false;
        trackedOccupants_.push_back(static_cast<INetworkManagerOccupant*>(raw));
    }
    return !stream.GetError();
}

// 0x100171d8c. u16 version 1, u32 count, then every tracked occupant.
bool cSC4NetworkManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    stream.SetUint32(static_cast<std::uint32_t>(trackedOccupants_.size()));
    if (g_host == nullptr) return false;
    for (INetworkManagerOccupant* occupant : trackedOccupants_) {
        if (!g_host->WriteGZSerializable(stream, static_cast<const cIGZUnknown*>(occupant))) return false;
    }
    return !stream.GetError();
}

// ============================================================================================================
// Init / Shutdown
// ============================================================================================================

// 0x100170928. Quirk: everything after the language query (the eleven message registrations *and* the
// creation of the thirteen tools) only happens when the message server exists.
bool cSC4NetworkManager::Init() {
    if (inited_) return true;
    inited_ = true;
    ukPathMapping_ = !(g_host != nullptr && g_host->LanguageFlag());
    if (g_host == nullptr) return true;
    cIGZMessageServer2* messages = g_host->MessageServer();
    if (messages == nullptr) return true;
    for (std::uint32_t id : kNetworkManagerMessages) {
        messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), id);
    }
    extendRegion_ = false;
    for (std::uint32_t type = 0; type < static_cast<std::uint32_t>(kNetworkTypeCount); ++type) {
        SetNetworkTool(static_cast<eNetworkType>(type), g_host->CreateNetworkTool(static_cast<eNetworkType>(type)));
    }
    return true;
}

// 0x100170ee6. Releases the tracked occupants, the retrofit list and the thirteen tools (ascending from
// +0x108; the destructor uses the descending order), then unregisters the messages (the original unregisters
// 0x26d31ec1 before 0x26d31ec2 here, the reverse of Init; kept).
bool cSC4NetworkManager::Shutdown() {
    if (!inited_) return true;
    inited_ = false;
    for (INetworkManagerOccupant* occupant : trackedOccupants_) {
        if (occupant != nullptr) occupant->Release();
    }
    trackedOccupants_.clear();
    for (cISC4Occupant* occupant : retrofitList_) {
        if (occupant != nullptr) occupant->Release();
    }
    retrofitList_.clear();
    for (int type = 0; type < static_cast<int>(kNetworkTypeCount); ++type) {
        INetworkManagerTool* tool = networkTools_[type];
        if (tool != nullptr) {
            networkTools_[type] = nullptr;
            tool->Release();
        }
    }
    if (g_host != nullptr) {
        if (cIGZMessageServer2* messages = g_host->MessageServer()) {
            static constexpr std::uint32_t kShutdownOrder[11] = {
                0xa6b79602, 0x278e6003, 0xea3b0af1, 0x6a3b0afd, 0x6ab97795, 0x26d31ec1,
                0x26d31ec2, 0x99ef1142, 0x99ef1143, 0xc9da96e8, 0xc9da96e9,
            };
            for (std::uint32_t id : kShutdownOrder) {
                messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), id);
            }
        }
    }
    return true;
}

// ============================================================================================================
// messages
// ============================================================================================================

// 0x100170886: the "refresh bridge models" iteration callback (occupant manager slot 0x2c/IterateOccupants).
// A network occupant carrying flag 0x40000 that is visible (cISC4Occupant flag 4) is told its model changed.
// The original never releases the QueryInterface reference (leak quirk, kept).
bool cRefreshBridgeModels::Go(cISC4Occupant* occupant, void* context) {
    (void)context;
    if (occupant == nullptr) return true;
    INetworkManagerOccupant* networkOccupant = nullptr;
    if (QueryNetworkOccupant(occupant, &networkOccupant) && networkOccupant->HasNetworkFlag(0x40000)) {
        if (occupant->IsFlagSet(kOccupantFlagVisible)) occupant->PostOccupantMessage(0x99ef1145, 0);
    }
    return true;
}

// 0x10016f3b4 — PARTIAL. Implemented branches:
//   0x99ef1142 / 0x99ef1143  track the network occupants that answer IsManagedGlobally (+0x28); the tracked
//                            vector owns the QI reference and drops it on removal (removal swaps the entry
//                            with the last one, exactly like the original).
//   0xa6b79602               two payload points whose second int is the view rotation; a change runs
//                            DoViewRotationChange (the original passes the message as `this`, which the callee
//                            never reads).
//   0xc9da96e8 / 0xc9da96e9  lights off / on: every tracked occupant that supports lighting (+0x1f0) gets
//                            SetLit(false/true, +0x1e8).
//   0x26d31ec1               Retrofit(), then — when the 24-hour clock answers "night" (+0xe0) — SetLit(true).
//   0x26d31ec2               clear the construction crews (+0xf0, each crew's vslot 0x28 = Shutdown, which must
//                            unregister itself) and release the thirteen tools in ascending order.
//   0x6ab97795               occupant manager IterateOccupants(Go, ..., type 0xc772bf98).
//   0xea3b0af1               reset the region rectangle and arm the extend flag (+0x1e).
// Not implemented (the rest of the original body): 0x278e6003 (city-size change) and 0x6a3b0afd, which
// rebuild the pipe/subway occupant terrain heights. They need cSC4NetworkORFilter, the occupant manager's
// +0x148 query, cSC4UndergroundNetworkTool::UpdateUndergroundOccupantTerrainHeights and the pipe/subway tools'
// height passes — all unrecovered — so the city-size fan-out is left out (the region rectangle is not updated).
bool cSC4NetworkManager::DoMessage(cIGZMessage2* message) {
    if (message == nullptr) return true;
    const std::uint32_t type = message->GetType();
    switch (type) {
    case 0x99ef1142:      // occupant inserted
    case 0x99ef1143: {    // occupant removed
        // The original takes payload 1 through cIGZMessage2Standard ([10] +0x50) with a plain downcast (no
        // QueryInterface; a null payload crashes it too).
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        if (occupant == nullptr) break;
        INetworkManagerOccupant* networkOccupant = nullptr;
        if (!QueryNetworkOccupant(occupant, &networkOccupant)) break;
        if (networkOccupant->IsManagedGlobally()) {
            if (type == 0x99ef1142) {
                trackedOccupants_.push_back(networkOccupant);   // owns the QI reference
                break;
            }
            const auto it = std::find(trackedOccupants_.begin(), trackedOccupants_.end(), networkOccupant);
            if (it != trackedOccupants_.end()) {
                (*it)->Release();
                *it = trackedOccupants_.back();
                trackedOccupants_.pop_back();
            }
        }
        networkOccupant->Release();
        break;
    }
    case 0xa6b79602: {   // view rotation changed
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        const auto* first = static_cast<const std::int32_t*>(standard->GetVoid1());
        const auto* second = static_cast<const std::int32_t*>(standard->GetVoid2());
        if (first != nullptr && second != nullptr && first[1] != second[1]) {
            DoViewRotationChange(first[1], second[1]);
        }
        break;
    }
    case 0xc9da96e8:   // lights off
    case 0xc9da96e9: { // lights on
        const bool lit = type == 0xc9da96e9;
        for (INetworkManagerOccupant* occupant : trackedOccupants_) {
            if (occupant != nullptr && occupant->SupportsLighting()) occupant->SetLit(lit);
        }
        break;
    }
    case 0x26d31ec1: {
        Retrofit();
        if (g_host != nullptr && g_host->IsNight()) {
            for (INetworkManagerOccupant* occupant : trackedOccupants_) {
                if (occupant != nullptr && occupant->SupportsLighting()) occupant->SetLit(true);
            }
        }
        break;
    }
    case 0x26d31ec2: {
        // Quirk kept: the crew is only shutdown; it has to unregister itself or this loop never ends.
        while (!constructionCrews_.empty()) {
            cSC4NetworkConstructionCrew* crew = constructionCrews_.front();
            if (crew == nullptr) {
                constructionCrews_.erase(constructionCrews_.begin());
                continue;
            }
            crew->Shutdown();
        }
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(kNetworkTypeCount); ++index) {
            INetworkManagerTool* tool = networkTools_[index];
            if (tool != nullptr) {
                networkTools_[index] = nullptr;
                tool->Release();
            }
        }
        break;
    }
    case 0x6ab97795: {
        if (g_host != nullptr) {
            if (INetworkManagerOccupantManager* manager = g_host->OccupantManager()) {
                manager->IterateOccupants(&cRefreshBridgeModels::Go, nullptr, kNetworkOccupantType);
            }
        }
        break;
    }
    case 0xea3b0af1: {
        extendRegion_ = true;
        regionMinX_ = 0x7fffffff;
        regionMinZ_ = 0x7fffffff;
        regionMaxX_ = -1;
        regionMaxZ_ = -1;
        break;
    }
    default:
        break;
    }
    return true;
}

// ============================================================================================================
// PARTIAL entry points (kept compiling and documented; see the header and the pending ledger)
// ============================================================================================================

// 0x100171064 — PARTIAL. Needs cSC4NetworkTool::GetCellInfo (0x1001b4bc0) plus the network tool's cell-info
// write (vslot 0x250) and the occupant removal path (occupant vslot 0x108/0x60, occupant manager +0xa8). Only
// the first test — the occupant answers cISC4Occupant::CanRemove(1) — is understood.
bool cSC4NetworkManager::RemoveOrReplaceOccupant(INetworkManagerTool* tool, cISC4Occupant* occupant,
                                                 bool& replaced) {
    (void)tool;
    (void)occupant;
    (void)replaced;
    return false;
}

// 0x1001711c2 / 0x1001712aa — PARTIAL. The walk needs cSC4NetworkORFilter (0x1002eee4c) and the network
// occupant's intersection-owner accessor (vslot 0x180) and same-intersection test (vslot 0x210), whose meanings
// are only inferred from the call sites. Its cell accessor (vslot 0x1d8 = GetCell(x, z)) *is* identified, but
// without the other two the visited-set recursion cannot be reproduced honestly.
void cSC4NetworkManager::GetAllIntersectionPieces(INetworkManagerOccupant* occupant, ilist<cISC4Occupant*>& out,
                                                  INetworkManagerOccupantManager* manager,
                                                  INetworkManagerOccupant* root) {
    (void)occupant;
    (void)out;
    (void)manager;
    (void)root;
}

void cSC4NetworkManager::GetAllIntersectionPieces2(INetworkManagerOccupant* occupant, ilist<cISC4Occupant*>& out,
                                                   INetworkManagerOccupantManager* manager,
                                                   INetworkManagerOccupant* root,
                                                   cISC4OccupantFilter* filter) {
    (void)occupant;
    (void)out;
    (void)manager;
    (void)root;
    (void)filter;
}

// 0x100171494 — PARTIAL. Builds a connection-node occupant: the network tool's placement pass (slots 0x18
// Init, 0x40 PlaceNetworkOccupantById, 0x20 Shutdown, 0x260), the occupant manager's cell query (+0x108), the
// lot manager's lot -> buildable-property chain and the new occupant's initialisation slots 0x120/0x80/0xf0/
// 0xb8/0xa0/0x70 are all unrecovered. The pipe/subway tools that would place the node are not recovered either.
bool cSC4NetworkManager::CreateConnectionNode(cISC4Occupant*& out, eNetworkType type, const SC4PointT<int>& cell,
                                               std::uint32_t a, std::uint32_t b, std::uint32_t c,
                                               std::uint8_t d) {
    (void)type;
    (void)cell;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    out = nullptr;
    return false;
}

// 0x100171780 — PARTIAL. Walks the lot's network-adjacent occupants and asks each network type's tool to
// place the connecting line (tool slots 0x38/0x18/0x20, the lot's cell rectangle at vslot 0x70, the occupant
// manager's cell query +0x108 and the occupant's cell/edge-mask slots 0x1d8/0x68/0x140), none of which is
// recovered; cSC4NetworkORFilter and the 0xb0 OR-filter mask are missing as well.
void cSC4NetworkManager::AutoconnectLotNodes(cISC4Lot* lot, const ilist<cISC4Occupant*>& occupants) {
    (void)lot;
    (void)occupants;
}

}  // namespace sc4
