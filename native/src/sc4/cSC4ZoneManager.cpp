#include "sc4/cSC4ZoneManager.h"

#include <algorithm>

#include <cstdio>
#include <cstdlib>

#include "gz/cIGZMessage2.h"
#include "sc4/DBSegment.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ZoneDeveloper.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kMessageInsertOccupant = 0x99ef1142;   // kSC4MessageInsertOccupant
constexpr std::uint32_t kInsertedNetworkType = 0xc772bf98;    // the network occupant type in data3
constexpr std::uint32_t kNetworkFlagsKeepZone = 0x80200090;      // a network occupant with any of them leaves the zones alone
const cGZPersistResourceKey kZoneTunablesKey{0x6534284a, 0xe7e2c2db, 0xe9482490};
}

cSC4ZoneManager::cSC4ZoneManager(IZoneManagerHost* host) : host_(host) {
    defaultDisplayAlpha_ = 0xff;
    displayAlpha_ = 0xff;
}

// 0x10019829c — the class answers its own interface (0x67652433) and cIGZUnknown at +0, the message
// target 2 (0x452294aa) at +8 and the serializable (0xe4fda3d4) at +0x10. The class id 0x298f9b2d is
// not answered (as in the original).
bool cSC4ZoneManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZSerializable) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4ZoneManager) *out = static_cast<cISC4ZoneManager*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x10019832c
std::uint32_t cSC4ZoneManager::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

// i386 0x004DCA70 (zones.md 5.1): only kSC4MessageInsertOccupant (0x99ef1142) whose data3 is the network occupant type
// (0xc772bf98). A network occupant with none of the flags 0x80200090 dezones its bounding cells, free of charge
// (PlaceZone(region, 0, commit, skip funds, no slope check, skip size, lots inside)); the zone developer then re-parcels
// what is left. The rectangle is clipped to the city (the original also writes outside the grid, which is undefined).
bool cSC4ZoneManager::DoMessage(cIGZMessage2* message) {
    auto* standard = dynamic_cast<cIGZMessage2Standard*>(message);
    if (!standard || standard->GetType() != kMessageInsertOccupant || standard->GetData3() != kInsertedNetworkType) return true;
    auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
    if (!occupant) return true;
    if (host_->NetworkOccupantHasAnyFlag(occupant, kNetworkFlagsKeepZone) != 0) return true;   // not a network, or it keeps the zone
    SC4CellRect cells;
    occupant->GetBoundingCityCells(cells);   // occupant slot 11
    if (!zoneGrid_) return true;
    const int maxX = zoneGrid_->GetTractCountX() - 1, maxZ = zoneGrid_->GetTractCountZ() - 1;   // the zone grid is one tract per cell
    SC4Rect<int> rect{std::max(cells.x0, 0), std::max(cells.z0, 0), std::min(cells.x1, maxX), std::min(cells.z1, maxZ)};
    if (rect.x0 > rect.x1 || rect.z0 > rect.z1) return true;
    SC4CellRegion<int> region = SC4CellRegion<int>::Make(rect, true);
    const bool placed = PlaceZone(region, 0, true, true, false, true, true, nullptr, nullptr, nullptr);
    if (std::getenv("SC4_ZONEDEBUG")) std::fprintf(stderr, "zone manager: network inserted, dezone %d,%d..%d,%d -> %d\n", rect.x0, rect.z0, rect.x1, rect.z1, placed ? 1 : 0);
    return true;
}

std::uint32_t cSC4ZoneManager::GetDevelopmentFailureCount(int zoneType, int result) const {
    if (zoneType >= 16 || result >= 19) return 0;
    auto it = failures_.find({static_cast<std::int8_t>(zoneType), static_cast<std::int8_t>(result)});
    return it == failures_.end() ? 0 : it->second;
}

bool cSC4ZoneManager::GetTextureForZone(int zoneType, cGZPersistResourceKey& key) const {
    if (static_cast<unsigned>(zoneType) >= 16u) return false;
    key.type = 0x7ab50e44;
    key.group = 0x1abe787d;
    key.instance = textureInstance_[zoneType];
    return true;
}

void cSC4ZoneManager::SetZoneDisplayAlpha(std::uint8_t alpha) {
    if (displayAlpha_ == alpha) return;
    displayAlpha_ = alpha;
    host_->OnZoneDisplayAlphaChanged();
}

void cSC4ZoneManager::ReadSimulatorTunableValues() {
    std::fill(std::begin(zoneParameterA_), std::end(zoneParameterA_), 0);
    std::fill(std::begin(zoneParameterB_), std::end(zoneParameterB_), 0);
    cISCPropertyHolder* tunables = host_->GetTunables(kZoneTunablesKey);
    if (!tunables) return;

    auto copy32 = [&](std::uint32_t id, auto* dest) {
        cISCProperty* property = tunables->GetProperty(id);
        if (!property) return;
        const cIGZVariant* value = property->GetPropertyValue();
        using Element = std::remove_pointer_t<decltype(dest)>;
        if constexpr (std::is_same_v<Element, std::int32_t>) {
            const std::int32_t* src = value->RefSint32();
            for (int i = 0; i < kZoneTypeCount; ++i) dest[i] = src[i];
        } else if constexpr (std::is_same_v<Element, std::uint32_t>) {
            const std::uint32_t* src = value->RefUint32();
            for (int i = 0; i < kZoneTypeCount; ++i) dest[i] = src[i];
        } else if constexpr (std::is_same_v<Element, float>) {
            const float* src = value->RefFloat32();
            for (int i = 0; i < kZoneTypeCount; ++i) dest[i] = src[i];
        } else {
            const std::int64_t* src = value->RefSint64();
            for (int i = 0; i < kZoneTypeCount; ++i) dest[i] = src[i];
        }
    };
    copy32(0xe94825b2, minSize_);
    copy32(0xe94825b3, maxSize_);
    copy32(0xe94825b9, developmentCost_);
    copy32(0xe94825b4, zoneParameterA_);
    copy32(0xe94825b5, zoneParameterB_);
    copy32(0xe94825b6, textureInstance_);
    cSCPropertyHelp::GetPropertyValue(tunables, 0xe94825b7, displayAlpha_);
    defaultDisplayAlpha_ = displayAlpha_;
    copy32(0xe94825b8, dragColor_);
}

namespace {
struct ChangedCell { int type, x, z; };
}

bool cSC4ZoneManager::PlaceZone(SC4CellRegion<int>& region, int zoneType, bool commit, bool freeOfCharge, bool checkTerrain,
                                bool hasMask, bool clipToRegion, std::int64_t* costOut, int* reasonOut,
                                std::set<cSC4Lot*>* lotsOut) {
    if (!placement_ || !zoneGrid_) {
        if (costOut) *costOut = 0;
        if (reasonOut) *reasonOut = 5;
        return false;
    }
    IZonePlacementHost& host = *placement_;
    const std::int64_t funds = host.Funds();
    const int type = static_cast<unsigned>(zoneType) > 0xf ? 0 : zoneType;
    int reason = 0;
    bool ok = true;
    std::int64_t cost = 0;
    std::vector<ChangedCell> changed;
    std::vector<cSC4Lot*> demolish;
    const SC4Rect<int> r = region.bounds;

    auto clearCell = [&](int x, int z) { region.ClearBit(x, z); };
    auto fail = [&](int why) { reason = why; ok = false; };

    bool aborted = false;
    if (!hasMask) {
        if (r.x1 - r.x0 + 1 < minSize_[type] || r.z1 - r.z0 + 1 < minSize_[type]) { fail(3); aborted = true; }
        else if (maxSize_[type] <= r.x1 - r.x0 || maxSize_[type] <= r.z1 - r.z0) { fail(4); aborted = true; }
    }
    if (aborted) cost = 0;
    for (int x = r.x0; !aborted && x <= r.x1; ++x) {
        for (int z = r.z0; z <= r.z1; ++z) {
            if (!region.Test(x, z)) continue;
            int old = zoneGrid_->GetTractValue(x, z);
            if (static_cast<unsigned>(old) > 0xf) old = 0;
            if (host.NetworkAt(x, z) && type != 14) {
                if (type != 0) { clearCell(x, z); continue; }   // a road cell cannot be zoned; it is left out
            }
            if (host.IsWater(x, z) && type != 0 && type != 12) {
                if (type == 14) { fail(2); aborted = true; break; }
                continue;                                         // water cells are skipped
            }
            if (checkTerrain && type != 0 && type != 15) {
                const float limit = developmentCost_[type];
                const float value = host.CellDevelopmentCost(x, z);
                if (limit <= value && value != limit) { clearCell(x, z); continue; }
            }
            cSC4Lot* lot = host.LotAt(x, z);
            if (!lot) {
                if (host.PropBlocksCell(x, z)) { fail(5); aborted = true; break; }
            } else {
                if (clipToRegion) {
                    int lx0, lz0, lx1, lz1;
                    host.LotCells(lot, lx0, lz0, lx1, lz1);
                    if (lx0 < r.x0 || r.x1 < lx1 || lz0 < r.z0 || r.z1 < lz1) {
                        if (type == 14) { fail(5); aborted = true; break; }
                        clearCell(x, z);
                        continue;
                    }
                }
                const int kind = host.LotKind(lot);
                if (kind > 15 || ((0x9800u >> kind) & 1u) == 0) {
                    if (type == 0 || !host.LotCompatibleWithZone(lot, type)) {
                        if (host.LotIsProtected(lot) && checkTerrain) { clearCell(x, z); continue; }
                        if (std::find(demolish.begin(), demolish.end(), lot) == demolish.end()) demolish.push_back(lot);
                    }
                } else {
                    if (type == 14) { fail(5); aborted = true; break; }
                    continue;                                     // plopped / airport / seaport lots keep their cells
                }
            }
            changed.push_back({old, x, z});
            if (type != old) cost += zoneParameterA_[type] + zoneParameterB_[old];
        }
    }
    if (!demolish.empty()) {
        cost += host.DemolishCost(demolish);
        if (lotsOut) lotsOut->insert(demolish.begin(), demolish.end());
    }
    if (type == 14) cost += host.LandfillCost(region);
    if (freeOfCharge) cost = 0;
    else if (funds < cost) { reason = 1; ok = false; }
    if (ok && commit) {
        if (!host.SpendFunds(cost)) {
            reason = 1;
            ok = false;
        } else if (!changed.empty()) {
            SC4CellRegion<int> done = SC4CellRegion<int>::Make(r, false);
            for (const ChangedCell& c : changed) {
                done.SetBit(c.x, c.z);
                zoneGrid_->SetTractValue(c.x, c.z, static_cast<std::int8_t>(type));
                --zoneCount_[c.type];
                ++zoneCount_[type];
            }
            host.DemolishLots(demolish);
            host.ApplyZone(done, type, checkTerrain);
            host.PostZoneChanged(done, type, checkTerrain);
        }
    }
    if (costOut) *costOut = cost;
    if (reasonOut) *reasonOut = reason;
    return ok;
}

bool cSC4ZoneManager::Init() {
    if (initialized_) return true;
    ReadSimulatorTunableValues();
    host_->RegisterNotifications();
    if (cIGZMessageServer2* server = host_->MessageServer()) server->AddNotification(this, kMessageInsertOccupant);
    if (!countsLoaded_) {
        const std::int32_t cells = host_->CityCellsX() * host_->CityCellsZ();
        std::fill(std::begin(zoneCount_), std::end(zoneCount_), 0);
        std::fill(std::begin(undevelopedByType_), std::end(undevelopedByType_), cells);
        std::fill(std::begin(secondCounts_), std::end(secondCounts_), cells);
        zoneCount_[0] = cells;
        undevelopedByType_[0] = cells;
        undevelopedTotal_ = cells;
    }
    if (!zoneGrid_) {
        zoneGrid_ = new cSC4SimGrid<std::int8_t>();
        zoneGrid_->Init();
        zoneGrid_->SetTractSize(1);
        zoneGrid_->SetTractValues(0);
    }
    initialized_ = true;
    return true;
}

bool cSC4ZoneManager::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    host_->UnregisterNotifications();
    if (cIGZMessageServer2* server = host_->MessageServer()) server->RemoveNotification(this, kMessageInsertOccupant);
    if (zoneGrid_) {
        zoneGrid_->Shutdown();
        zoneGrid_ = nullptr;
    }
    return true;
}

bool cSC4ZoneManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    SC4WriteGZSerializable(stream, static_cast<const cIGZSerializable*>(zoneGrid_.get()));
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetSint32(zoneCount_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetSint32(undevelopedByType_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetSint32(secondCounts_[i]);
    stream.SetSint32(undevelopedTotal_);
    stream.SetSint32(counter13c_);
    stream.SetSint32(abandonedTotal_);
    stream.SetSint32(counter144_);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetSint32(minSize_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetSint32(maxSize_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetSint64(zoneParameterA_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetSint64(zoneParameterB_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetUint32(textureInstance_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.SetUint32(dragColor_[i]);
    stream.SetUint8(displayAlpha_);
    stream.SetUint32(static_cast<std::uint32_t>(failures_.size()));
    for (const auto& [key, count] : failures_) {
        stream.SetSint8(key.zoneType);
        stream.SetSint8(key.result);
        stream.SetUint32(count);
    }
    stream.SetUint32(static_cast<std::uint32_t>(successes_.size()));
    for (const auto& [key, count] : successes_) {
        stream.SetSint8(key.zoneType);
        stream.SetSint8(key.result);
        stream.SetUint32(count);
    }
    for (int i = 0; i < 5; ++i) SC4WriteGZSerializable(stream, host_->GetServiceForStream(i));
    return !stream.GetError();
}

bool cSC4ZoneManager::Read(cIGZIStream& stream) {
    countsLoaded_ = true;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 1) return false;
    void* grid = nullptr;
    if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &grid, false)) {
        zoneGrid_ = static_cast<cSC4SimGrid<std::int8_t>*>(static_cast<cISC4SimGridBase*>(grid));
        static_cast<cISC4SimGridBase*>(grid)->Release();
    }
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetSint32(zoneCount_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetSint32(undevelopedByType_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetSint32(secondCounts_[i]);
    stream.GetSint32(undevelopedTotal_);
    stream.GetSint32(counter13c_);
    stream.GetSint32(abandonedTotal_);
    stream.GetSint32(counter144_);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetSint32(minSize_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetSint32(maxSize_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetSint64(zoneParameterA_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetSint64(zoneParameterB_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetUint32(textureInstance_[i]);
    for (int i = 0; i < kZoneTypeCount; ++i) stream.GetUint32(dragColor_[i]);
    stream.GetUint8(displayAlpha_);
    auto readMap = [&](std::map<ZoneDevelopmentResult, std::uint32_t>& map) {
        std::uint32_t count = 0;
        stream.GetUint32(count);
        map.clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            ZoneDevelopmentResult key;
            std::uint32_t value = 0;
            stream.GetSint8(key.zoneType);
            stream.GetSint8(key.result);
            stream.GetUint32(value);
            map.emplace(key, value);   // a duplicate key keeps the first value
        }
    };
    readMap(failures_);
    readMap(successes_);
    for (int i = 0; i < 5; ++i) {
        void* service = nullptr;
        if (SC4ReadGZSerializable(stream, kStreamServiceIid[i], &service, false) && service) {
            auto* object = static_cast<cIGZUnknown*>(service);
            host_->OnServiceLoaded(i, object);
            object->Release();
        }
    }
    return !stream.GetError();
}

}  // namespace sc4
