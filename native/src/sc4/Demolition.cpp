#include "sc4/Demolition.h"

#include <algorithm>
#include <cmath>

namespace sc4 {

bool cSC4Demolition::Init() {
    if (initialized_) return true;
    initialized_ = true;
    host_->AddNotification(kMessageTuningReloaded);
    host_->AddNotification(kMessageCityInit);
    host_->AddNotification(kMessageDemolishRequest);
    host_->CityCells(cellsX_, cellsZ_);
    ReadProperties();
    return true;
}

bool cSC4Demolition::Shutdown() {
    if (!initialized_) return true;
    host_->RemoveNotification(kMessageTuningReloaded);
    host_->RemoveNotification(kMessageCityInit);
    host_->RemoveNotification(kMessageDemolishRequest);
    initialized_ = false;
    return true;
}

// Two exemplars: the model / group size thresholds (group 0x690f693f, instance 0x2a8a09ca) and the lot tile cost
// (group 0xe7e2c2db, instance 0xe8da7677). Values stay as they were when a property is missing.
void cSC4Demolition::ReadProperties() {
    if (cISCPropertyHolder* thresholds = host_->TuningExemplar(0x690f693f, 0x2a8a09ca)) {
        cSCPropertyHelp::GetPropertyValue(thresholds, kPropertyModelSmallMedium, smallMediumModel_);
        cSCPropertyHelp::GetPropertyValue(thresholds, kPropertyModelMediumLarge, mediumLargeModel_);
        cSCPropertyHelp::GetPropertyValue(thresholds, kPropertyGroupSmallMedium, smallMediumGroup_);
        cSCPropertyHelp::GetPropertyValue(thresholds, kPropertyGroupMediumLarge, mediumLargeGroup_);
    }
    if (cISCPropertyHolder* costs = host_->TuningExemplar(0xe7e2c2db, 0xe8da7677))
        cSCPropertyHelp::GetPropertyValue(costs, kPropertyDemolishLotTileCost, lotTileCost_);
}

bool cSC4Demolition::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    return !stream.GetError();
}

bool cSC4Demolition::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return version == 1 && !stream.GetError();
}

bool cSC4Demolition::DoMessage(std::uint32_t type) {
    if (type != kMessageTuningReloaded) return false;
    ReadProperties();
    return true;
}

void cSC4Demolition::PostMessage(std::uint32_t message, IDemolishable* occupant, bool sendNow) {
    host_->PostOccupantMessage(message, occupant, sendNow);
}

// ---- occupant queries -------------------------------------------------------------------------------------

std::vector<IDemolishable*> cSC4Demolition::GetDemolishableLotOccupants(IDemolishableLot* lot) {
    std::vector<IDemolishable*> occupants = lot->Occupants();
    std::vector<IDemolishable*> dropped;
    occupants.erase(std::remove_if(occupants.begin(), occupants.end(),
                                   [&](IDemolishable* o) {
                                       bool drop = !host_->OccupantAllowedByFilter(o, filter_);
                                       if (drop) dropped.push_back(o);
                                       return drop;
                                   }),
                    occupants.end());
    host_->ReleaseOccupantList(dropped);
    return occupants;
}

// The rectangle is converted to metre-space bounds (second corner is one cell past the end) and the occupant
// manager walks it with the default filter in rect-scan mode.
std::vector<IDemolishable*> cSC4Demolition::GetDemolishableRectOccupants(const DemolitionCellRect& rect) {
    filter_.rectScan = true;
    std::vector<IDemolishable*> found = host_->OccupantsInRect(rect, filter_);
    filter_.rectScan = false;
    return found;
}

// ---- follow-up pass ---------------------------------------------------------------------------------------

// Network pieces (type 0xc772bf98) are collected, told to the neighbour simulator and handed to the network
// manager in one batch; poles (0x2890d4de) only get the demolished message and are removed from the manager.
void cSC4Demolition::UpdateNetworks(const std::set<IDemolishable*>& occupants) {
    std::vector<IDemolishable*> networks;
    for (IDemolishable* occupant : occupants) {
        if (!occupant->HasRemovalFlag(1)) continue;
        std::uint32_t type = occupant->Type();
        if (type == kDemolishTypeNetwork) {
            networks.push_back(occupant);
            host_->NotifyNeighborsNetworkChange(occupant);
        } else if (type == kDemolishTypePole) {
            PostMessage(kMessageOccupantDemolished, occupant, false);
            host_->RemoveOccupant(occupant, true, false);
        }
    }
    if (!networks.empty()) {
        host_->UpdateNetworkManager(networks);
        host_->ReleaseOccupantList(networks);
    }
}

// ---- lots ---------------------------------------------------------------------------------------------------

// Builds the union of the lots' rectangles and a cell mask that marks the cells of each plopped lot, then sweeps that
// region with the lots-only flag.
bool cSC4Demolition::DemolishLots(bool commit, const std::list<IDemolishableLot*>& lots, int privilege, std::uint32_t flags,
                                  bool keepZones, IDemolishFilter* filter, std::int64_t* cost,
                                  std::set<IDemolishable*>* demolished) {
    if (lots.empty()) return true;
    DemolitionRegion region;
    region.bounds = lots.front()->Bounds();
    for (IDemolishableLot* lot : lots) {
        DemolitionCellRect r = lot->Bounds();
        region.bounds.x0 = std::min(region.bounds.x0, r.x0);
        region.bounds.z0 = std::min(region.bounds.z0, r.z0);
        region.bounds.x1 = std::max(region.bounds.x1, r.x1);
        region.bounds.z1 = std::max(region.bounds.z1, r.z1);
    }
    region.mask.assign(region.bounds.x1 - region.bounds.x0 + 1, std::vector<bool>(region.bounds.z1 - region.bounds.z0 + 1, false));
    for (IDemolishableLot* lot : lots) {
        if (!lot->IsPlopped()) continue;
        DemolitionCellRect r = lot->Bounds();
        for (int x = r.x0; x <= r.x1; ++x)
            for (int z = r.z0; z <= r.z1; ++z) region.mask[x - region.bounds.x0][z - region.bounds.z0] = true;
    }
    return DemolishRegion(commit, region, privilege, flags | kDemolishLotsOnly, keepZones, filter, cost, demolished, nullptr, 0, 0);
}

// ---- region sweep -------------------------------------------------------------------------------------------------

// Pass 1 walks the cells: every lot found is vetted once (a building or prop the filter refuses vetoes the whole
// lot, and an unaffordable privilege clears the overall result), accepted lots contribute their occupants, and every
// cell contributes the free occupants on it. Pass 2 demolishes the collected occupants (dry run first when effects are
// wanted), pass 3 settles the lots (cost of destroyed lots, building development hooks, re-zoning), pass 4 folds in the
// occupants released by pole removal and refreshes the networks.
bool cSC4Demolition::DemolishRegion(bool commit, const DemolitionRegion& region, int privilege, std::uint32_t flags,
                                    bool keepZones, IDemolishFilter* filter, std::int64_t* cost,
                                    std::set<IDemolishable*>* demolished, IDemolishable* primary, int viewX, int viewY) {
    region_ = &region;
    host_->ViewBeginSweep();
    IDemolishFilter* viewFilter = host_->ViewOccupantFilter();
    const bool lotsOnly = (flags & kDemolishLotsOnly) != 0;
    const bool viewDisabled = lotsOnly || privilege == 0;
    const bool effects = (flags & kDemolishFlagEffectsMask) != 0 && commit;
    if (!viewDisabled) filter_.view = viewFilter;
    filter_.user = filter;
    filter_.active = true;
    filter_.mode = privilege == 0 ? 1 : 2;

    std::set<IDemolishable*> occupantSet;
    std::set<IDemolishableLot*> lotSet;
    std::list<IDemolishableLot*> lotList;
    int newOccupants = 0;
    bool success = true;
    auto addOccupant = [&](IDemolishable* o) { if (occupantSet.insert(o).second) ++newOccupants; };

    const DemolitionCellRect& b = region.bounds;
    for (int x = b.x0; x <= b.x1; ++x) {
        for (int z = b.z0; z <= b.z1; ++z) {
            if (!region.Contains(x, z)) continue;
            IDemolishableLot* lot = host_->LotAtCell(x, z);
            if (lot && lotSet.insert(lot).second) {
                IDemolishable* building = lot->BuildingOccupant();
                bool vetted = false;
                if (!building) {
                    int type = lot->TypeCode();
                    if (type == 6 || type == 1) vetted = !viewFilter || viewFilter->IsOccupantTypeIncluded(0x278128a0);
                } else {
                    if (lot->IsPlopped()) vetted = host_->OccupantAllowedByFilter(building, filter_);
                    if (!vetted && privilege == 1 && !lot->BuildingRemovable(2)) success = false;
                }
                if (vetted) {
                    std::vector<IDemolishable*> all = lot->Occupants();
                    filter_.view = nullptr;
                    bool allowed = true;
                    for (IDemolishable* o : all) {
                        std::uint32_t t = o->Type();
                        if ((t == kDemolishTypeBuilding || t == kDemolishTypeProp) && !host_->OccupantAllowedByFilter(o, filter_)) {
                            allowed = false;
                            break;
                        }
                    }
                    if (!viewDisabled) filter_.view = viewFilter;
                    if (allowed) {
                        lotList.push_front(lot);
                        std::vector<IDemolishable*> mine = GetDemolishableLotOccupants(lot);
                        for (IDemolishable* o : mine)
                            if (!lotsOnly || std::find(all.begin(), all.end(), o) != all.end()) addOccupant(o);
                        host_->ReleaseOccupantList(mine);
                    } else {
                        success = false;
                    }
                    host_->ReleaseOccupantList(all);
                }
            }
            std::vector<IDemolishable*> cellOccupants = GetDemolishableRectOccupants({x, z, x, z});
            for (IDemolishable* o : cellOccupants) {
                bool included = host_->LocalFilterIncludes(o);
                bool lotHere = lot != nullptr;
                if ((lotHere && included) != (!lotsOnly || included)) addOccupant(o);
            }
            host_->ReleaseOccupantList(cellOccupants);
        }
    }

    std::int64_t total = 0;
    std::vector<IDemolishable*> released;
    for (IDemolishable* o : occupantSet) {
        std::int64_t price = 0;
        if (!DemolishOccupant(commit && !effects, o, flags, &price, true, &released)) continue;
        if (effects) {
            if (!viewFilter || viewFilter->IsOccupantIncluded(o)) DoDemolishOccupantEffect(o, flags, host_->LotOfOccupant(o), o == primary, viewX, viewY);
            DemolishOccupant(true, o, flags, &price, true, &released);
        }
        total += price;
        if (demolished) demolished->insert(o);
    }
    if (effects && newOccupants > 0) DoDemolishGroupEffect(flags, 0, newOccupants, b);
    if (commit) host_->NetworkBatch(true);

    for (IDemolishableLot* lot : lotList) {
        if (lot->TypeCode() == 6) {
            std::uint8_t width = 0, depth = 0;
            lot->Size(width, depth);
            total += std::int64_t(width) * depth * lotTileCost_;
        }
        if (!commit) continue;
        const bool lotPolicy = (flags & kDemolishFlagAnyLotPolicy) != 0;
        if ((!lot->HasBuildingObject() && lotPolicy) || (lot->HasBuildingObject() && lotPolicy && !lot->BuildingPlain())) {
            host_->LotSkipRemoval(lot, (flags & kDemolishFlagNoBuildingRestore) == 0);
            continue;
        }
        int zone = lot->ZoneType();
        host_->LotPrepareRemoval(lot);
        host_->LotFinishRemoval(lot);
        if (!keepZones && !((0x9800u >> (zone & 31)) & 1)) {
            if (!(flags & kDemolishFlagKeepLotZone) && static_cast<unsigned>(zone - 7) < 3) {
                DemolitionCellRect r = lot->Bounds();
                std::uint8_t orientation = lot->Orientation();
                host_->RemoveLot(lot);
                for (int x = r.x0; x <= r.x1; ++x)
                    for (int z = r.z0; z <= r.z1; ++z)
                        if (host_->ZoneTypeAt(x, z) == zone) host_->CreateLotAt(x, z, orientation);
            }
        } else {
            host_->ClearZones(lot->Bounds());
        }
    }

    // occupants released by pole removal join the set (and the caller's) so the network pass sees them
    while (!released.empty()) {
        IDemolishable* o = released.back();
        released.pop_back();
        occupantSet.insert(o);
        if (demolished) demolished->insert(o);
    }
    if (commit) {
        UpdateNetworks(occupantSet);
        host_->NetworkBatch(false);
    }
    if (cost) *cost = total;
    filter_ = DemolitionFilterState();
    region_ = nullptr;
    return success;
}

// ---- terrain ---------------------------------------------------------------------------------------------------------

// Counts the cells whose height differs from `height`. Lowering (raise == false) refuses when any occupant stands on
// the area; raising checks every cell: lots that block terrain changes and occupants whose footprint sits wholly
// inside the cell veto it. The count is returned even when a lot blocks the change (nothing is applied then).
int cSC4Demolition::ModifyTerrainHeight(bool apply, bool raise, int x0, int z0, int x1, int z1, float height,
                                        IDemolishableLot* ignoredLot, std::uint32_t reason) {
    DemolitionCellRect search{std::max(x0 - 1, 0), std::max(z0 - 1, 0), std::min(x1, cellsX_ - 1), std::min(z1, cellsZ_ - 1)};
    std::vector<IDemolishable*> occupants = host_->OccupantsForTerrain(search, raise ? -1 : 1);
    int changed = 0;
    auto lotsAllow = [&](const DemolitionCellRect& r) {
        for (IDemolishableLot* lot : host_->LotsInRect(r))
            if (lot != ignoredLot && lot->BlocksTerrainChange()) return false;
        return true;
    };
    if (!raise) {
        if (x1 >= x0 && occupants.empty()) {
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z)
                    if (std::fabs(host_->TerrainHeight(x, z) - height) > 0.05f) ++changed;
            if (changed != 0 && lotsAllow({std::max(x0 - 1, 0), std::max(z0 - 1, 0), x1, z1}) && apply) {
                DemolitionCellRect r{std::max(x0, 0), std::max(z0, 0), std::min(x1, cellsX_), std::min(z1, cellsZ_)};
                host_->TerrainLevelRect(r, reason);
                host_->TerrainUpdateRect(r, reason);
            }
        }
    } else if (x1 >= x0) {
        for (int x = x0; x <= x1; ++x) {
            const float xLo = float(x * 16 - 16) + 1.5f, xHi = float(x * 16 + 16) - 1.5f;
            for (int z = z0; z <= z1; ++z) {
                if (host_->TerrainCellSkipped(x, z) || std::fabs(host_->TerrainHeight(x, z) - height) < 0.05f) continue;
                if (!lotsAllow({x - 1, z - 1, x, z})) continue;
                const float zLo = float(z * 16 - 16) + 1.5f, zHi = float(z * 16 + 16) - 1.5f;
                bool contained = false;
                for (IDemolishable* o : occupants) {
                    float box[6];
                    o->BoundingBox(box);
                    if (zLo <= box[2] && box[5] <= zHi && box[3] <= xHi && xLo <= box[0]) { contained = true; break; }
                }
                if (contained) continue;
                ++changed;
                if (apply) host_->TerrainLevelCell(x, z);
            }
        }
        if (changed != 0) host_->TerrainUpdateRect({search.x0, search.z0, search.x1 + 1, search.z1 + 1}, reason);
    }
    host_->ReleaseOccupantList(occupants);
    return changed;
}

// ---- single occupant ------------------------------------------------------------------------------------------

// Quirks kept: network pieces and buildings are only costed here (their removal is the caller's), foundations are
// removed only when they carry flag 1, and a missing service for flora / buildings fails the call with cost 0.
bool cSC4Demolition::DemolishOccupant(bool commit, IDemolishable* occupant, std::uint32_t flags, std::int64_t* cost,
                                      bool skipNetworks, std::vector<IDemolishable*>* released) {
    if ((flags & kDemolishSkipProtected) && occupant->HasRemovalFlag(0x40)) return false;
    std::int64_t price = 0;
    bool ok = false;
    switch (occupant->Type()) {
    case kDemolishTypeProp:
        ok = host_->PropManagerAvailable();
        if (ok && commit) {
            if (host_->PropManagerRemove(occupant)) occupant->Remove();
            else ok = false;
        }
        break;
    case kDemolishTypeNetwork:
        if (host_->NetworkCost(occupant, price)) {
            if (commit && !skipNetworks && (occupant->Flags() & 1)) UpdateNetworks({occupant});
            ok = true;
        }
        break;
    case kDemolishTypeSpecial:
        ok = commit ? host_->RemoveOccupant(occupant, true, false) : true;
        break;
    case kDemolishTypeBuilding:
        ok = host_->BuildingCost(occupant, price);
        break;
    case kDemolishTypePole:
        if (host_->PoleCost(occupant, price)) {
            ok = true;
            if (commit) {
                std::vector<IDemolishable*> connected = host_->PoleConnections(occupant);
                PostMessage(kMessageOccupantDemolished, occupant, false);
                ok = host_->RemoveOccupant(occupant, false, false);
                if (ok) {
                    host_->PoleDetach(occupant);
                    occupant->Remove();
                    if (released)
                        for (IDemolishable* c : connected)
                            if (host_->IsPlainOccupant(c)) released->push_back(c);
                }
            }
        }
        break;
    case kDemolishTypeFoundation:
        ok = (!commit || !occupant->HasRemovalFlag(1)) ? true : host_->RemoveOccupant(occupant, true, false);
        break;
    case kDemolishTypeFlora:
        if (host_->FloraCost(occupant, price)) {
            ok = true;
            if (commit) host_->RemoveOccupant(occupant, true, false);
        }
        break;
    default:
        break;
    }
    if (cost) *cost = price;
    return ok;
}

// ---- effects ----------------------------------------------------------------------------------------------------------

// Effect index: bits 8+ = caller's effect byte, 0x10 buildings and poles, 0x20 flora / still props, 0x30 animated props,
// 0x40 plain networks, 0x50 networks with flag 1, low bits 1 / 2 = medium / large. When the size's effect is absent the
// smaller indices down to the next multiple of 16 are tried, and the first one found is the only effect played.
void cSC4Demolition::DoDemolishOccupantEffect(IDemolishable* occupant, std::uint32_t flags, IDemolishableLot* lot, bool primary,
                                              int viewX, int viewY) {
    float box[6] = {1, 1, 1, 0, 0, 0};
    occupant->BoundingBox(box);
    float size = box[4] - box[1];
    if (size <= box[3] - box[0]) size = box[3] - box[0];
    float extent = box[5] - box[2];
    if (extent <= size) extent = size;
    if (host_->RenderMinimumSize() > host_->ViewScale() * extent) return;
    std::uint32_t index = (flags & 0xff) << 8;
    switch (occupant->Type()) {
    case kDemolishTypeProp: {
        bool wanted = false;
        bool have = cSCPropertyHelp::GetPropertyValue(occupant->Properties(), kPropertyPropDemolishEffect, wanted);
        if (!wanted || !have) return;
        index |= occupant->TestFlag(0x5004) ? 0x20 : 0x30;
        break;
    }
    case kDemolishTypeNetwork: {
        bool isNetwork = false;
        std::uint32_t kind = 0x40;
        if (occupant->NetworkHasFlag(2, &isNetwork) == false && isNetwork && occupant->NetworkHasFlag(1, nullptr)) kind = 0x50;
        index |= kind;
        break;
    }
    case kDemolishTypeBuilding:
    case kDemolishTypePole: index |= 0x10; break;
    case kDemolishTypeFlora: index |= 0x20; break;
    default: break;
    }
    std::uint32_t sound = 0;
    cISCPropertyHolder* props = occupant->Properties();
    if (props && cSCPropertyHelp::GetPropertyValue(props, kPropertyDemolishSound, sound)) host_->PlaySound(sound);
    if (extent <= mediumLargeModel_) {
        if (smallMediumModel_ < extent) index |= 1;
    } else {
        index |= 2;
    }
    // One effect: the size's own, or (only when that one is absent) the next smaller index (i386 0x000ae090 loop).
    IDemolitionEffect* effect = nullptr;
    while (!(effect = host_->CreateEffect(kEffectSetDemolish, index))) {
        const std::uint32_t low = index & 0xf;
        --index;
        if (low == 0) return;
    }
    if (effect->Attach(occupant)) {
        if (primary) effect->SetViewpoint(viewX, viewY);
        if (lot && lot->ZoneClass() != 0 && lot->ZoneClass() != 1) effect->SetGrayTint();
        effect->Play();
    }
    host_->ReleaseEffect(effect);
}

// The group size compared against the thresholds is `groupSize` (DemolishRegion passes 0 here, see the header).
void cSC4Demolition::DoDemolishGroupEffect(std::uint32_t flags, int groupSize, int, const DemolitionCellRect& bounds) {
    int cx = (bounds.x1 + bounds.x0) / 2, cz = (bounds.z1 + bounds.z0) / 2;
    float a = 0, b = 0;
    host_->OccupantManagerCentre(cx, cz, a, b);
    float height = host_->TerrainHeightAtMetres(float(cx), float(cz));
    std::uint32_t size = 2;
    if (static_cast<std::uint32_t>(groupSize) <= mediumLargeGroup_) size = smallMediumGroup_ < static_cast<std::uint32_t>(groupSize) ? 1u : 0u;
    host_->PlayGroupEffect(size | ((flags & 0xff) << 8), a, height, b);
}

}  // namespace sc4
