// cSC4TrafficSimulator — the ferries (traffic-network.md §2.6, §4.9; traffic-simulator.md §4.12.2). Mac i386 decompile:
// scratch/s85/ghidra/ts.c (DoFerryTerminalActivated 0x0005e53a, RouteAndLinkTerminals 0x000583e0, RouteTerminalToNeighbor
// 0x0005aa68, GetWaterRoute 0x0005281c, FindFerryLink 0x00053914, FindFerryNeighborLink 0x0005393c, GetFerryRouteBetweenTiles
// 0x000612e6, WorkOnFerryRoutes 0x00067c1e, the ferry parts of DoLotDeactivated, Read and Write).
//
// A ferry terminal lot carries three marker props: the departure cell (0x2c520000, where trips board: wormhole bit 4), the
// arrival cell (0x2c530000, where trips land) and the water marker (0x2c540000) beside the water the boats leave from. Every two
// terminals of one type (0 passenger, 1 car) on one body of water are linked by a water route, and every terminal is linked to
// each neighbour city's water connection (type 9) it can reach. The path finder turns the links into edges (pathfinder.md
// §4.8.3); ApplyRoute counts the riders and the fares; the ferry pass of cycle state 19 rotates the counters and re-routes.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "sc4/SCPropertyHelp.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/WaterPathFinder.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kFerryTypeProperty = 0x0bd86fd3;
constexpr std::uint32_t kFerryDeparturePropType = 0x2c520000, kFerryArrivalPropType = 0x2c530000, kFerryWaterPropType = 0x2c540000;
constexpr std::uint16_t kTravelTypesSupportedByFerryType[2] = {1, 3};   // Walk; Walk and Car
constexpr int kMaxFerryTerminalsPerType = 16;
constexpr int kFerryNeighborConnectionType = 9;
constexpr float kFerryCostPerCell = 0.01f;   // cSC4Tank::sfBarrelVelocityScalingFactor, as the routing code reads it
constexpr int kStepX[4] = {-1, 0, 1, 0}, kStepZ[4] = {0, -1, 0, 1};
constexpr int kNetworkOfType[9] = {0, 1, 1, 2, 1, 2, 3, 4, 5};      // net(t), as TrafficCycle.cpp
bool FerryDebug() { static const bool on = std::getenv("SC4_TRAFFICDEBUG") != nullptr; return on; }
bool InRect(const SC4RectInt32& r, int x, int z) { return r.x0 <= x && x <= r.x1 && r.z0 <= z && z <= r.z1; }
}  // namespace

// ---- the water path finder (made in Init, i386 +0x118) -----------------------------------------------------------------------

void cSC4TrafficSimulator::InitWaterPathFinder() {
    if (waterPathFinder_) return;
    IWaterPathFinderHost* host = host_->WaterPathFinderHost();
    if (!host) return;
    waterPathFinder_ = new cSC4WaterPathFinder(host);
    waterPathFinder_->AddRef();
    waterPathFinder_->Init();
}

void cSC4TrafficSimulator::ShutdownWaterPathFinder() {
    if (!waterPathFinder_) return;
    waterPathFinder_->Shutdown();
    waterPathFinder_->Release();
    waterPathFinder_ = nullptr;
}

bool cSC4TrafficSimulator::GetWaterRoute(int x0, int z0, int x1, int z1, std::vector<std::uint8_t>& route) {
    std::int32_t cells = 0;
    return waterPathFinder_ && waterPathFinder_->FindPath(x0, z0, x1, z1, cells, &route, nullptr);
}

// ---- lookups ----------------------------------------------------------------------------------------------------------------

cSC4TrafficSimulator::FerryTerminal* cSC4TrafficSimulator::FerryByDeparture(int x, int z) {
    auto it = ferryDepartures_.find(std::uint16_t(((x & 0xff) << 8) | (z & 0xff)));
    return it == ferryDepartures_.end() ? nullptr : &*it->second;
}

cSC4TrafficSimulator::FerryTerminal* cSC4TrafficSimulator::FerryByArrival(int x, int z) {
    auto it = ferryArrivals_.find(std::uint16_t(((x & 0xff) << 8) | (z & 0xff)));
    return it == ferryArrivals_.end() ? nullptr : &*it->second;
}

const cSC4TrafficSimulator::FerryTerminal* cSC4TrafficSimulator::FerryTerminalByDeparture(int x, int z) const {
    return const_cast<cSC4TrafficSimulator*>(this)->FerryByDeparture(x, z);
}

const cSC4TrafficSimulator::FerryTerminal* cSC4TrafficSimulator::FerryTerminalByArrival(int x, int z) const {
    return const_cast<cSC4TrafficSimulator*>(this)->FerryByArrival(x, z);
}

// ---- §4.9.1 DoFerryTerminalActivated ------------------------------------------------------------------------------------------

void cSC4TrafficSimulator::DoFerryTerminalActivated(ITrafficLot* lot) {
    cISCPropertyHolder* building = host_->LotBuildingProperties(lot);
    if (!building) return;
    std::uint8_t type = 0;
    // type 2 never activates in the original (its count word is a heap pointer, TN18): rejected here
    if (!cSCPropertyHelp::GetPropertyValue(building, kFerryTypeProperty, type) || type >= 2 ||
        ferryTerminalCount_[type] >= kMaxFerryTerminalsPerType)
        return;
    FerryTerminal terminal;
    terminal.type = type;
    terminal.travelTypes = kTravelTypesSupportedByFerryType[type];
    int lx = 0, lz = 0;
    lot->Location(lx, lz);
    terminal.lotX = std::uint8_t(lx);
    terminal.lotZ = std::uint8_t(lz);
    SC4RectInt32 rect;
    lot->OccupiedRect(rect);
    std::vector<ITrafficHost::PropCell> props;
    host_->PropsInRect(rect, props);
    bool departure = false, arrival = false, water = false;
    for (const ITrafficHost::PropCell& prop : props) {   // later props of a kind overwrite earlier ones
        if (prop.type == kFerryArrivalPropType) {
            arrival = true;
            terminal.arrivalX = std::uint8_t(prop.x);
            terminal.arrivalZ = std::uint8_t(prop.z);
        } else if (prop.type == kFerryWaterPropType) {
            for (int d = 0; d < 4; ++d) {   // the first side whose neighbour is outside the lot and under water
                const int nx = prop.x + kStepX[d], nz = prop.z + kStepZ[d];
                if (!InRect(rect, nx, nz) && host_->CellHasWater(nx, nz)) {
                    terminal.waterX = std::uint8_t(nx);
                    terminal.waterZ = std::uint8_t(nz);
                    water = true;
                    break;
                }
            }
        } else if (prop.type == kFerryDeparturePropType) {
            departure = true;
            terminal.departureX = std::uint8_t(prop.x);
            terminal.departureZ = std::uint8_t(prop.z);
        }
    }
    if (FerryDebug())
        std::fprintf(stderr, "ferry: terminal type %u at %d,%d: departure %d arrival %d water %d (props %zu)\n", unsigned(type), lx, lz,
                     int(departure), int(arrival), int(water), props.size());
    if (!departure || !arrival || !water) return;
    ferryTerminals_.push_back(std::move(terminal));
    ++ferryTerminalCount_[type];
    auto it = std::prev(ferryTerminals_.end());
    ferryDepartures_[it->DepartureKey()] = it;
    ferryArrivals_[it->ArrivalKey()] = it;
    for (auto other = ferryTerminals_.begin(); other != ferryTerminals_.end(); ++other)
        if (other != it && other->type == it->type) RouteAndLinkTerminals(*it, *other);
    std::vector<std::pair<int, int>> cells;
    host_->NeighborConnectionCellsOfType(kFerryNeighborConnectionType, cells);
    for (const auto& [cx, cz] : cells) RouteTerminalToNeighbor(*it, cx, cz);
}

// ---- §4.9.4 terminal removal (DoLotDeactivated) -------------------------------------------------------------------------------

void cSC4TrafficSimulator::RemoveFerryTerminal(const SC4RectInt32& lotRect) {
    auto it = std::find_if(ferryTerminals_.begin(), ferryTerminals_.end(),
                           [&](const FerryTerminal& t) { return InRect(lotRect, t.lotX, t.lotZ); });
    if (it == ferryTerminals_.end()) return;
    if (--ferryTerminalCount_[it->type] < 0) ferryTerminalCount_[it->type] = 0;
    const std::uint16_t key = it->DepartureKey();
    for (const FerryTerminalLink& link : it->links)
        if (FerryTerminal* other = FerryByDeparture(link.key >> 8, link.key & 0xff))
            other->links.remove_if([&](const FerryTerminalLink& back) { return back.key == key; });
    if (cycleState_ == 19 && ferryPassValid_) {   // the pass moves on past it
        if (ferryPass_ == it) ++ferryPass_;
        if (ferryPassOther_ == it) ++ferryPassOther_;
    }
    ferryDepartures_.erase(key);
    ferryArrivals_.erase(it->ArrivalKey());
    ferryTerminals_.erase(it);
    // no DoConnectionsChanged (quirk): the departure cell keeps wormhole bit 4 until a later rebuild covers it
}

// ---- §4.9.3 routing ---------------------------------------------------------------------------------------------------------

void cSC4TrafficSimulator::RouteAndLinkTerminals(FerryTerminal& a, FerryTerminal& b) {
    const std::uint16_t keyB = b.DepartureKey(), keyA = a.DepartureKey();
    auto linkAB = std::find_if(a.links.begin(), a.links.end(), [&](const FerryTerminalLink& l) { return l.key == keyB; });
    auto linkBA = std::find_if(b.links.begin(), b.links.end(), [&](const FerryTerminalLink& l) { return l.key == keyA; });
    std::vector<std::uint8_t> routeAB, routeBA;
    std::int32_t cells = 0;
    if (!waterPathFinder_ || !waterPathFinder_->FindPath(a.waterX, a.waterZ, b.waterX, b.waterZ, cells, &routeAB, &routeBA)) {
        if (linkAB != a.links.end()) a.links.erase(linkAB);
        if (linkBA != b.links.end()) b.links.erase(linkBA);
        return;
    }
    const float cost = float(cells) * kFerryCostPerCell;
    if (linkAB == a.links.end()) linkAB = a.links.insert(a.links.end(), FerryTerminalLink{});
    linkAB->key = keyB;
    linkAB->cost = cost;
    linkAB->arrivalX = b.arrivalX;
    linkAB->arrivalZ = b.arrivalZ;
    linkAB->route = routeAB;
    if (linkBA == b.links.end()) linkBA = b.links.insert(b.links.end(), FerryTerminalLink{});
    linkBA->key = keyA;
    linkBA->cost = cost;
    linkBA->arrivalX = a.arrivalX;
    linkBA->arrivalZ = a.arrivalZ;
    linkBA->route = routeBA;
}

void cSC4TrafficSimulator::RouteTerminalToNeighbor(FerryTerminal& terminal, int x, int z) {
    int ix = 0, iz = 0;
    if (GetNeighborConnectionInnerCell(x, z, ix, iz) < 0) return;
    auto link = std::find_if(terminal.neighborLinks.begin(), terminal.neighborLinks.end(),
                             [&](const FerryNeighborLink& l) { return l.x == x && l.z == z; });
    std::vector<std::uint8_t> toEdge, fromEdge;
    std::int32_t cells = 0;
    if (!waterPathFinder_ || !waterPathFinder_->FindPath(terminal.waterX, terminal.waterZ, ix, iz, cells, &toEdge, &fromEdge)) {
        if (link != terminal.neighborLinks.end()) terminal.neighborLinks.erase(link);
        return;
    }
    if (link == terminal.neighborLinks.end()) link = terminal.neighborLinks.insert(terminal.neighborLinks.end(), FerryNeighborLink{});
    link->cost = float(cells) * kFerryCostPerCell;
    link->x = std::int16_t(x);
    link->z = std::int16_t(z);
    link->innerX = std::int16_t(ix);
    link->innerZ = std::int16_t(iz);
    link->toEdge = toEdge;
    link->fromEdge = fromEdge;
}

// ---- §4.9.2 the ferry phase of DoConnectionsChanged ---------------------------------------------------------------------------

void cSC4TrafficSimulator::FerryConnectionsChanged(const SC4RectInt32& r1) {
    auto inCity = [&](int x, int z) { return unsigned(x) < unsigned(cellsX_) && unsigned(z) < unsigned(cellsZ_); };
    auto setConnection = [&](int x, int z, int entry, int exit, int t) {   // SetConnection 0x0070EAB0
        CellConnections(x, z).masks[std::size_t(t)] |= std::uint16_t(1u << (exit + 4 * entry));
        const int nx = x + kStepX[exit], nz = z + kStepZ[exit];
        if (kNetworkOfType[t] == 1 && inCity(nx, nz))
            CellConnections(nx, nz).priorCar[std::size_t((exit + 2) & 3)] |= std::uint16_t(0x1111u << entry);
    };
    for (const FerryTerminal& terminal : ferryTerminals_) {
        const int tx = terminal.departureX, tz = terminal.departureZ;
        const std::uint16_t mask = terminal.travelTypes;
        if (InRect(r1, tx, tz) && inCity(tx, tz)) {
            CellConnections(tx, tz).wormhole |= 4;
            for (int n = 0; n < 2; ++n)
                if (!GetNetworkInfo(n, tx, tz))
                    if (NetworkInfo* info = AddNetworkInfo(n, tx, tz)) {
                        info->connectionCount = 1;
                        info->pieceType = 0;
                        info->inverseCapacity = tunables_.inverse_e58[0];
                        info->potholePenalty = 0.0f;
                    }
            for (int d = 0; d < 4; ++d) {   // the roads around the terminal lead into it
                const int nx = tx + kStepX[d], nz = tz + kStepZ[d];
                if (!inCity(nx, nz)) continue;
                const int ns = (d + 2) & 3;
                for (int t = 0; t < 9; ++t) {
                    if ((mask >> t) & 1) {
                        const std::uint16_t conn = CellConnections(nx, nz).masks[std::size_t(t)];
                        for (int e = 0; e < 4; ++e)
                            if (e != ns && ((conn >> (4 * e)) & 0xf)) setConnection(nx, nz, e, ns, t);
                    } else {
                        CellConnections(nx, nz).masks[std::size_t(t)] &= std::uint16_t(~(0x1111u << ns));
                    }
                }
            }
        }
        const int ax = terminal.arrivalX, az = terminal.arrivalZ;
        if (InRect(r1, ax, az) && inCity(ax, az)) {
            for (int d = 0; d < 4; ++d) {   // the arrival cell leads out onto the roads that leave from beside it
                const int nx = ax + kStepX[d], nz = az + kStepZ[d];
                if (!inCity(nx, nz)) continue;
                const int ns = (d + 2) & 3;
                for (int t = 0; t < 9; ++t) {
                    if ((mask >> t) & 1) {
                        const std::uint16_t conn = CellConnections(nx, nz).masks[std::size_t(t)];
                        bool any = false;
                        for (int x = 0; x < 4; ++x)
                            if (x != ns && ((conn >> (x + 4 * ns)) & 1)) {
                                setConnection(nx, nz, ns, x, t);
                                any = true;
                            }
                        if (any)
                            for (int s = 0; s < 4; ++s) setConnection(ax, az, s, d, t);
                    } else {
                        CellConnections(nx, nz).masks[std::size_t(t)] &= std::uint16_t(~(0xfu << (4 * ns)));
                    }
                }
            }
        }
    }
}

// ---- the ferry pass of cycle state 19 -----------------------------------------------------------------------------------------

void cSC4TrafficSimulator::EnterFerryState() {
    ferryRiderTotal_ = 0;
    for (FerryTerminal& t : ferryTerminals_) {
        t.ridersLast = t.ridersCurrent;
        t.ridersCurrent = 0;
        ferryRiderTotal_ += t.ridersLast;
        t.reroute = true;
        for (FerryTerminalLink& l : t.links) {
            l.lastA = l.currentA; l.currentA = 0;
            l.lastB = l.currentB; l.currentB = 0;
        }
        for (FerryNeighborLink& l : t.neighborLinks) {
            l.lastA = l.currentA; l.currentA = 0;
            l.lastB = l.currentB; l.currentB = 0;
        }
    }
    host_->NeighborConnectionCellsOfType(kFerryNeighborConnectionType, ferryNeighborCells_);
    ferryPass_ = ferryTerminals_.begin();
    ferryPassOther_ = ferryTerminals_.begin();
    ferryNeighborNext_ = 0;
    ferryPassStep_ = 0;
    ferryPassValid_ = true;
}

void cSC4TrafficSimulator::RestartFerryPass() {
    for (FerryTerminal& t : ferryTerminals_) t.reroute = true;
    ferryPass_ = ferryTerminals_.begin();
    ferryPassOther_ = ferryTerminals_.begin();
    ferryPassStep_ = 0;
    ferryPassValid_ = true;
}

bool cSC4TrafficSimulator::WorkOnFerryRoutes(int& units) {
    if (!ferryPassValid_) EnterFerryState();
    for (;;) {
        if (units <= 0) return false;
        if (ferryPassStep_ == 0) {
            if (ferryPass_ == ferryTerminals_.end()) {
                if (FerryDebug()) std::fprintf(stderr, "ferry: the state-19 pass finished (%zu terminals, rider total %u)\n", ferryTerminals_.size(), ferryRiderTotal_);
                host_->PostFerryRoutesDone();
                return true;
            }
            ferryPassOther_ = ferryTerminals_.begin();
            ferryPassStep_ = 1;
        }
        if (ferryPassStep_ == 1) {
            if (ferryPassOther_ == ferryTerminals_.end()) {
                ferryPass_->reroute = false;
                ferryPassStep_ = 2;
                ferryNeighborNext_ = 0;
                continue;
            }
            auto other = ferryPassOther_++;
            if (other != ferryPass_ && other->type == ferryPass_->type && other->reroute) {
                RouteAndLinkTerminals(*ferryPass_, *other);
                --units;
            }
            continue;
        }
        if (ferryNeighborNext_ < ferryNeighborCells_.size()) {
            const auto [cx, cz] = ferryNeighborCells_[ferryNeighborNext_++];
            RouteTerminalToNeighbor(*ferryPass_, cx, cz);
            --units;
        } else {
            ++ferryPass_;
            ferryPassStep_ = 0;
        }
    }
}

// ---- §4.9.5 queries ---------------------------------------------------------------------------------------------------------

const std::vector<std::uint8_t>* cSC4TrafficSimulator::GetFerryRouteBetweenTiles(int x1, int z1, int x2, int z2) const {
    auto inCity = [&](int x, int z) { return 0 <= x && 0 <= z && x <= cellsX_ - 1 && z <= cellsZ_ - 1; };
    if (!inCity(x1, z1)) {   // from a neighbour: the terminal whose arrival cell is (x2, z2), its link from (x1, z1)
        const FerryTerminal* t = FerryTerminalByArrival(x2, z2);
        if (!t) return nullptr;
        for (const FerryNeighborLink& l : t->neighborLinks)
            if (l.x == x1 && l.z == z1) return &l.fromEdge;
        return nullptr;
    }
    const FerryTerminal* t = FerryTerminalByDeparture(x1, z1);
    if (!t) return nullptr;
    if (!inCity(x2, z2)) {
        for (const FerryNeighborLink& l : t->neighborLinks)
            if (l.x == x2 && l.z == z2) return &l.toEdge;
        return nullptr;
    }
    const FerryTerminal* b = FerryTerminalByArrival(x2, z2);
    if (!b) {
        for (const FerryNeighborLink& l : t->neighborLinks)
            if (l.innerX == x2 && l.innerZ == z2) return &l.toEdge;
        return nullptr;
    }
    for (const FerryTerminalLink& l : t->links)
        if (l.key == b->DepartureKey()) return &l.route;
    return nullptr;
}

void cSC4TrafficSimulator::GetAllFerryRoutes(std::list<std::vector<std::uint8_t>>& out) const {
    out.clear();
    for (const FerryTerminal& t : ferryTerminals_) {
        for (const FerryTerminalLink& l : t.links) out.push_back(l.route);
        for (const FerryNeighborLink& l : t.neighborLinks) out.push_back(l.toEdge);
    }
}

void cSC4TrafficSimulator::GetFerryRoutesInUse(std::list<FerryRouteInfo>& out) const {
    out.clear();
    std::vector<std::uint16_t> visited;
    for (const FerryTerminal& a : ferryTerminals_) {
        for (const FerryTerminalLink& l : a.links) {
            if (l.lastA == 0 && l.lastB == 0) continue;
            if (std::find(visited.begin(), visited.end(), l.key) != visited.end()) continue;
            const FerryTerminal* b = FerryTerminalByDeparture(l.key >> 8, l.key & 0xff);
            FerryRouteInfo info;
            info.lotAX = a.lotX; info.lotAZ = a.lotZ;
            if (b) { info.lotBX = b->lotX; info.lotBZ = b->lotZ; }
            info.type = a.type;
            info.abLastA = l.lastA; info.abLastB = l.lastB;
            info.routeAB = l.route;
            if (b)
                for (const FerryTerminalLink& back : b->links)
                    if (back.key == a.DepartureKey()) {
                        info.baLastA = back.lastA; info.baLastB = back.lastB;
                        info.routeBA = back.route;
                        break;
                    }
            out.push_back(std::move(info));
        }
        for (const FerryNeighborLink& l : a.neighborLinks) {
            if (l.lastA == 0 && l.lastB == 0) continue;
            FerryRouteInfo info;
            info.lotAX = a.lotX; info.lotAZ = a.lotZ;
            info.type = a.type;
            info.abLastA = info.baLastA = l.lastA;
            info.abLastB = info.baLastB = l.lastB;
            info.neighbor = true;
            info.neighborX = l.x; info.neighborZ = l.z;
            info.routeAB = l.toEdge;
            info.routeBA = l.fromEdge;
            out.push_back(std::move(info));
        }
        visited.push_back(a.DepartureKey());
    }
}

// ---- the record (row 19's tail): per terminal its cells, type, mask, riders, lot cell and both link lists ---------------------

void cSC4TrafficSimulator::WriteFerryTerminals(cIGZOStream& stream, std::uint32_t version) const {
    if (version > 0x26) stream.SetUint32(ferryRiderTotal_);
    if (version < 0x1e) return;
    stream.SetUint32(std::uint32_t(ferryTerminals_.size()));
    for (const FerryTerminal& t : ferryTerminals_) {
        if (version < 0x20) stream.SetUint16(0);
        for (std::uint8_t b : {t.departureX, t.departureZ, t.arrivalX, t.arrivalZ, t.waterX, t.waterZ}) stream.SetUint8(b);
        stream.SetUint8(t.type);
        stream.SetUint16(t.travelTypes);
        stream.SetUint16(t.ridersLast);
        stream.SetUint16(t.ridersCurrent);
        if (version >= 0x26) {
            stream.SetUint8(t.lotX);
            stream.SetUint8(t.lotZ);
        }
        stream.SetUint32(std::uint32_t(t.links.size()));
        for (const FerryTerminalLink& l : t.links) {
            stream.SetFloat32(l.cost);
            stream.SetSint16(l.arrivalX);
            stream.SetSint16(l.arrivalZ);
            stream.SetUint16(l.key);
            stream.SetUint32(std::uint32_t(l.route.size()));
            if (!l.route.empty()) stream.SetVoid(l.route.data(), std::uint32_t(l.route.size()));
            if (version >= 0x24) {   // the original's order: last A, last B, current A, current B
                stream.SetUint16(l.lastA);
                stream.SetUint16(l.lastB);
                stream.SetUint16(l.currentA);
                stream.SetUint16(l.currentB);
            }
        }
        stream.SetUint32(std::uint32_t(t.neighborLinks.size()));
        for (const FerryNeighborLink& l : t.neighborLinks) {
            stream.SetFloat32(l.cost);
            stream.SetSint16(l.x);
            stream.SetSint16(l.z);
            stream.SetSint16(l.innerX);
            stream.SetSint16(l.innerZ);
            for (const auto* route : {&l.toEdge, &l.fromEdge}) {
                if (route == &l.fromEdge && version < 0x1f) break;
                stream.SetUint32(std::uint32_t(route->size()));
                if (!route->empty()) stream.SetVoid(route->data(), std::uint32_t(route->size()));
            }
            if (version >= 0x24) {
                stream.SetUint16(l.lastA);
                stream.SetUint16(l.lastB);
                stream.SetUint16(l.currentA);
                stream.SetUint16(l.currentB);
            }
        }
    }
}

}  // namespace sc4
