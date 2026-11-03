// cSC4TrafficSimulator, the live half: Init / Shutdown, the network model rebuild over the city's network pieces, the daily work
// cycle (trips -> path searches -> routes -> maps), the monthly pass and the lot activation bookkeeping. Written to the OpenSC4
// behaviour specs traffic-simulator.md, traffic-network.md and pathfinder.md (Windows 1.1.641 addresses in the comments).
//
// Not modelled yet (each PARTIAL): transit switches (stations, toll booths), tunnels, ferries and water routes, neighbour
// connections as traffic sources (their trips), the subnetwork flood fill (states 12 / 13), accidents, and the save record (Read
// keeps the city file's record and Write replays it, so a save carries the traffic data it was loaded with).
#include "sc4/TrafficSimulator.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sc4/Lot.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/PathFinder.h"
#include "sc4/PathInfo.h"
#include "sc4/SCProperty.h"
#include "sc4/cSC4SimGrid.h"

namespace sc4 {

namespace {

constexpr int kNetworkOfType[9] = {0, 1, 1, 2, 1, 2, 3, 4, 5};      // net(t)
constexpr std::uint32_t kPathTypeOfNetwork[6] = {2, 1, 3, 4, 6, 7};  // pt(n)
// Network types that take part in traffic (all but pipes and power lines) and the transit networks each carries.
constexpr bool kTakesPart[13] = {true, true, true, true, false, false, true, true, true, true, true, true, true};
constexpr std::uint8_t kTransitMask[13] = {3, 4, 2, 3, 0, 0, 3, 8, 0x10, 0x20, 3, 3, 2};
constexpr std::uint32_t kNext[20] = {16, 14, 4, 7, 5, 11, 10, 6, 0, 0, 12, 3, 13, 19, 15, 2, 17, 18, 15, 8};
constexpr int kSideX[4] = {-1, 0, 1, 0}, kSideZ[4] = {0, -1, 0, 1};
constexpr std::uint32_t kNetworkFlagNoTraffic = 0x200000, kNetworkFlagBridge = 0x40000;
constexpr std::uint32_t kLotStrategyBucket = 0x691c863c, kLotStrategyDraw = 0x4a611773;
constexpr std::uint32_t kOrdinanceCarpool = 0xa2bf1ddc;

bool Debug() {
    static const bool on = std::getenv("SC4_TRAFFICDEBUG") != nullptr;
    return on;
}

// round(v): the 1.5 * 2^23 idiom, round half to even
inline std::uint32_t RoundHalfEven(float v) { return std::uint32_t(std::lrintf(v)); }

bool InRect(const SC4RectInt32& r, int x, int z) { return r.x0 <= x && x <= r.x1 && r.z0 <= z && z <= r.z1; }

// One visited cell of a compressed route as the route reader (0x006D64B0 / 0x006D6510) yields it (traffic-simulator.md §4.3).
struct RouteRecord {
    int x = 0, z = 0;
    int type = 0, nextType = 0;
    int entry = -1, exit = -1;   // -1 = none
    bool jumped = false;         // reached by a jump whose token had bit 7 (a ferry or off-map crossing)
};

bool ReadRoute(const std::vector<std::uint8_t>& route, std::vector<RouteRecord>& out) {
    out.clear();
    if (route.size() < 3) return false;
    struct Step { int x, z, type; bool unit; int dir; bool flag; };
    std::vector<Step> steps;
    int x = route[1], z = route[2];
    steps.push_back({x, z, route[0] & 0x3f, false, -1, false});
    std::size_t i = 3;
    while (i < route.size()) {
        const std::uint8_t token = route[i];
        if (token & 0x40) {
            if (i + 1 >= route.size()) break;
            const int type = token & 0x3f, count = route[i + 1];
            const std::size_t bytes = std::size_t((count + 3) / 4);
            if (i + 2 + bytes > route.size()) break;
            for (int k = 0; k < count; ++k) {
                const int code = (route[i + 2 + std::size_t(k / 4)] >> (2 * (k % 4))) & 3;
                x += kSideX[code];
                z += kSideZ[code];
                steps.push_back({x, z, type, true, code, false});
            }
            i += 2 + bytes;
        } else {
            if (i + 2 >= route.size()) break;
            x = route[i + 1];
            z = route[i + 2];
            steps.push_back({x, z, token & 0x3f, false, -1, (token & 0x80) != 0});
            i += 3;
        }
    }
    out.resize(steps.size());
    int carriedEntry = -1;
    for (std::size_t k = 0; k < steps.size(); ++k) {
        RouteRecord& r = out[k];
        r.x = steps[k].x;
        r.z = steps[k].z;
        r.type = steps[k].type;
        r.jumped = steps[k].flag;
        r.entry = k == 0 ? -1 : carriedEntry;
        if (k + 1 == steps.size()) {
            r.nextType = r.type;
            r.exit = -1;
            break;
        }
        const Step& next = steps[k + 1];
        r.nextType = next.type;
        if (next.unit) {
            r.exit = next.dir;
            carriedEntry = (r.exit + 2) & 3;
        } else {
            int dx = std::int16_t(next.x - r.x), dz = std::int16_t(next.z - r.z);
            if (dx != 0 && dz != 0) {
                if (std::abs(dx) <= std::abs(dz)) dx = 0;
                else dz = 0;
            }
            if (std::abs(dx) >= 2) r.exit = dx > 0 ? 2 : 0;
            else if (dx != 0) {
                // dx = +-1: no exit, and this cell's entry is blanked; the entry it arrived with carries over the jump
                r.exit = -1;
                carriedEntry = r.entry;
                r.entry = -1;
                continue;
            } else r.exit = dz <= -2 ? 1 : 3;
            carriedEntry = (r.exit + 2) & 3;
        }
    }
    return true;
}

}  // namespace

// ---- the path finder's view of the network model ------------------------------------------------------------------------------

class cSC4TrafficSimulator::PathGraph final : public IPathFinderHost {
public:
    explicit PathGraph(cSC4TrafficSimulator& sim) : s_(sim) {}
    int CityWidthInCells() override { return s_.cellsX_; }
    int CityDepthInCells() override { return s_.cellsZ_; }
    float TripScaleForDisplay() override { return s_.tunables_.tripScaleForDisplay; }
    std::uint32_t TripCapacityLimit() override { return 0; }
    std::uint32_t UsedTripCapacity(cISCPropertyHolder* holder, std::uint32_t kind) override {
        return holder ? GetTripCapacity(holder, kind + 4) : 0;
    }
    bool NeighborConnectionCost(int x, int z, float& cost) override {
        const TrafficNeighborConnection* record = s_.GetTrafficNeighborConnection(x, z);
        cost = record ? record->cost : 0.0f;
        return true;
    }
    bool InCity(int x, int z) const { return unsigned(x) < unsigned(s_.cellsX_) && unsigned(z) < unsigned(s_.cellsZ_); }
    std::uint16_t Connections(int t, int x, int z) override { return InCity(x, z) && t < 9 ? s_.CellConnections(x, z).masks[t] : 0; }
    std::uint16_t Switches(int t, int x, int z) override { return InCity(x, z) && t < 9 ? s_.CellConnections(x, z).switches[t] : 0xffff; }
    std::uint16_t PriorCar(int side, int x, int z) override { return InCity(x, z) ? s_.CellConnections(x, z).priorCar[side & 3] : 0xffff; }
    std::uint8_t Wormhole(int x, int z) override { return InCity(x, z) ? s_.CellConnections(x, z).wormhole : 0; }
    bool CellNetwork(int n, int x, int z, float& inverse, std::uint8_t& type, float& extra) override {
        const NetworkInfo* info = s_.GetNetworkInfo(n, x, z);
        if (!info) return false;
        inverse = info->inverseCapacity;
        type = info->pieceType;
        extra = info->potholePenalty;
        return true;
    }
    float Volume(int n, int x, int z) override {
        cSC4SimGrid<float>* grid = s_.GetVolumeMap(n);
        return grid && InCity(x, z) ? grid->GetCellValue(x, z) : 0.0f;
    }
    std::uint32_t TransitSwitchId(int, int) override { return 0; }   // PARTIAL: no transit switches natively
    float TransitSwitchCost(std::uint32_t id) override { return s_.TransitSwitchEntryCost(id); }
    std::uint8_t DestinationByte(int x, int z) override { return s_.DestinationByte(x, z); }
    void SetDestinationByte(int x, int z, std::uint8_t value) override {
        const std::size_t row = std::size_t(x + 1), column = std::size_t(z + 1);
        if (row < s_.cellTripFlags_.size() && column < s_.cellTripFlags_[row].size()) s_.cellTripFlags_[row][column] = value;
    }
    std::uint32_t FreeDestinations(int k) override { return s_.FreeDestinations(k); }
    void DecrementFreeDestinations(int k) override { if (unsigned(k) < 4) --s_.connectionCounts_[std::size_t(k)]; }
    bool LotAtCell(int x, int z, SC4RectInt32& rect, cISCPropertyHolder*& holder) override {
        holder = nullptr;
        ITrafficLot* lot = InCity(x, z) ? s_.host_->LotAt(x, z, 0) : nullptr;
        if (!lot) return false;
        lot->OccupiedRect(rect);
        holder = lot->Properties();
        return true;
    }
    cISCPropertyHolder* NeighborPool(int t, int x, int z) override { return s_.GetNeighborPoolPropertyHolder(t, x, z); }
    std::uint32_t MaxTripCapacity(cISCPropertyHolder* holder, std::uint32_t k) override { return holder ? GetTripCapacity(holder, k) : 0; }
    float InverseMaxSpeed(int t, int type) override {
        return unsigned(t) < 9 && unsigned(type) < 13 ? s_.tunables_.inverseMaximumSpeed[std::size_t(t)][std::size_t(type)] : FLT_MAX;
    }
    bool AffectedByTraffic(int t) override { return unsigned(t) < 9 && s_.travelTypeAffectedByTraffic_[std::size_t(t)]; }
    bool CanReachDestination(int t) override { return unsigned(t) < 9 && s_.tunables_.travelTypeFlag_e95[std::size_t(t)]; }
    const float* DefaultStartingCosts() override { return s_.tunables_.values_ee0.data(); }
    std::uint32_t MaxDistanceOriginToNetwork() override { return s_.tunables_.value_ea8; }
    float NearestDestinationAttractiveness() override { return s_.tunables_.value_f60; }
    float CongestionSpeedFactor(float ratio) override { return s_.congestionToInverseSpeed_.GetOutputValue(ratio); }
    bool StrikeOn() override { return s_.strikeState_ == kStrikeOn; }
private:
    cSC4TrafficSimulator& s_;
};

std::uint8_t cSC4TrafficSimulator::DestinationByte(int x, int z) const {
    const std::size_t row = std::size_t(x + 1), column = std::size_t(z + 1);
    if (row >= cellTripFlags_.size() || column >= cellTripFlags_[row].size()) return 0;
    return cellTripFlags_[row][column];
}

template <class T> cSC4SimGrid<T>* cSC4TrafficSimulator::MakeGrid(std::uint32_t id, int tract) {
    auto* grid = new cSC4SimGrid<T>();
    grid->AddRef();
    grid->Init();
    grid->SetTractSize(tract);
    grid->SetInstanceID(id);
    return grid;
}

// The grids the city file's record referenced, by instance id: Read loaded them, Init keeps them (§1 "Init after Read"), so the
// maps a save published are there at once and the first searches see the saved volumes.
template <class T> void cSC4TrafficSimulator::AdoptSavedGrid(cSC4SimGrid<T>*& member, std::uint32_t id) {
    if (member) return;
    cityRecord_.ForEachRef([&](cIGZUnknown* object) {
        auto* grid = dynamic_cast<cSC4SimGrid<T>*>(object);
        if (member || !grid || grid->GetInstanceID() != id) return;
        if (grid->GetTractCountX() <= 0 || grid->GetGridData().size() != std::size_t(grid->GetTractCountX())) return;
        grid->AddRef();
        member = grid;
    });
}

// ---- Init / Shutdown (§1) -----------------------------------------------------------------------------------------------------

bool cSC4TrafficSimulator::Init() {
    if (initialized_) return true;
    initialized_ = true;
    if (!graph_) graph_ = new PathGraph(*this);
    travelTypeAffectedByTraffic_.fill(true);
    ReadSimulatorTunableValues(host_->SimulatorExemplar());
    cIGZMessageTarget2* target = this;
    host_->AddAgent(target, 0, "cSC4TrafficSimulator", 2);
    host_->AddAgent(target, 6, "cSC4TrafficSimulator", 2);
    host_->AddAgent(target, 4, "cSC4TrafficSimulator", 2);
    for (std::uint32_t m : {0x99ef1142u, 0x99ef1143u, 0x26d31ec1u, 0xe8bda2e7u, 0xc53d10aau, 0x8a0d15dau, 0xea0d163eu, 0x08dac3e7u, 0xc2c09554u})
        host_->AddNotification(target, m);
    host_->CityCellCount(cellsX_, cellsZ_);
    InitGrid(cellsX_, cellsZ_);
    AdoptSavedGrid(airMap_, 0x89d5c3a2);
    AdoptSavedGrid(peakVolume_, 0x69d5c3ac);
    AdoptSavedGrid(commercialMap_, 0xcabd2925);
    AdoptSavedGrid(commercialWork_, 0xcabd2926);
    AdoptSavedGrid(tripLengthMap_, 0x69d5c3bf);
    for (int n = 0; n < 6; ++n) AdoptSavedGrid(volume_[std::size_t(n)], 0x69d5c3e0u + std::uint32_t(n));
    AdoptSavedGrid(congestionMap_, 0x69d5c402);
    AdoptSavedGrid(peakRatio_, 0x69d5c40e);
    if (Debug()) {
        int adopted = 0;
        for (void* g : {(void*)airMap_, (void*)peakVolume_, (void*)commercialMap_, (void*)commercialWork_, (void*)tripLengthMap_, (void*)congestionMap_,
                        (void*)peakRatio_, (void*)volume_[0], (void*)volume_[1], (void*)volume_[2], (void*)volume_[3], (void*)volume_[4], (void*)volume_[5]})
            adopted += g ? 1 : 0;
        std::fprintf(stderr, "traffic: adopted %d saved grids\n", adopted);
        auto snapshot = [&](cSC4SimGrid<std::uint8_t>* g, std::vector<std::uint8_t>& out) {
            if (!g) return;
            for (auto& column : g->GetGridData()) out.insert(out.end(), column.begin(), column.end());
        };
        snapshot(congestionMap_, savedCongestion_);
        snapshot(tripLengthMap_, savedTripLength_);
    }
    if (!airMap_) airMap_ = MakeGrid<std::uint8_t>(0x89d5c3a2, 1);
    if (!peakVolume_) peakVolume_ = MakeGrid<float>(0x69d5c3ac, 1);
    if (!commercialMap_) commercialMap_ = MakeGrid<std::uint8_t>(0xcabd2925, 4);
    if (!commercialWork_) commercialWork_ = MakeGrid<std::uint8_t>(0xcabd2926, 4);
    if (!tripLengthMap_) tripLengthMap_ = MakeGrid<std::uint8_t>(0x69d5c3bf, 1);
    for (int n = 0; n < 6; ++n)
        if (!volume_[std::size_t(n)]) volume_[std::size_t(n)] = MakeGrid<float>(0x69d5c3e0u + std::uint32_t(n), 1);
    if (!congestionMap_) congestionMap_ = MakeGrid<std::uint8_t>(0x69d5c402, 1);
    if (!peakRatio_) peakRatio_ = MakeGrid<float>(0x69d5c40e, 1);
    if (!pf_) {
        pf_ = new cSC4PathFinder(graph_);
        pf_->AddRef();
        pf_->Init();
    }
    if (!pf2_) {
        pf2_ = new cSC4PathFinder(graph_);
        pf2_->AddRef();
        pf2_->Init();
    }
    if (const char* units = std::getenv("SC4_TRAFFICUNITS")) SetUnitsPerSlice(std::atoi(units));
    cycleState_ = 0;
    if (Debug()) std::fprintf(stderr, "traffic: init %dx%d commute max %.2f units/slice %d\n", cellsX_, cellsZ_, tunables_.tripScale, unitsPerSlice_);
    return true;
}

cSC4TrafficSimulator::~cSC4TrafficSimulator() {
    Shutdown();
    delete graph_;
}

bool cSC4TrafficSimulator::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    sources_.clear();
    for (cSC4PathFinder** pf : {&pf_, &pf2_})
        if (*pf) { (*pf)->Release(); *pf = nullptr; }
    auto drop = [](auto*& grid) { if (grid) { grid->Release(); grid = nullptr; } };
    drop(airMap_); drop(peakVolume_); drop(commercialMap_); drop(commercialWork_); drop(tripLengthMap_);
    drop(congestionMap_); drop(peakRatio_);
    for (auto*& grid : volume_) drop(grid);
    cIGZMessageTarget2* target = this;
    for (std::uint32_t m : {0xc2c09554u, 0x08dac3e7u, 0x99ef1142u, 0x99ef1143u, 0x26d31ec1u, 0xe8bda2e7u, 0xc53d10aau, 0x8a0d15dau, 0xea0d163eu})
        host_->RemoveNotification(target, m);
    host_->RemoveAgent(target);
    host_->EndOfWork();
    return true;
}

// ---- messages (§3.1) ------------------------------------------------------------------------------------------------------

bool cSC4TrafficSimulator::DoMessage(cIGZMessage2* raw) {
    if (!raw || !initialized_) return true;
    auto* message = static_cast<cIGZMessage2Standard*>(raw);
    const std::uint32_t type = message->GetType();
    switch (type) {
        case 0x66956810: networkChanged_ = true; return true;   // SimBegin
        case 0x66956814: return DoDailySimulation();
        case 0x66956816: DoMonthlySimulation(); return true;
        case 0x26d31ec1: DoPostCityInit(); return true;
        case 0xc2c09554: UpdateTravelStrategies(host_->SimulatorExemplar()); return true;
        case 0xc53d10aa:
            if (message->GetData1() == 0xe7e2c2db && message->GetData2() == 0xc9133286) ReadSimulatorTunableValues(host_->SimulatorExemplar());
            return true;
        case 0x8a0d15da: AddNeighborConnection(int(message->GetData1()), int(message->GetData2()), int(message->GetData3())); return true;
        case 0xea0d163e: RemoveNeighborConnection(int(message->GetData1()), int(message->GetData2()), int(message->GetData3())); return true;
        case 0x99ef1142:
        case 0x99ef1143: {
            networkChanged_ = true;
            if (message->GetData3() != 0xc772bf98) return true;
            auto* occupant = dynamic_cast<cSC4NetworkOccupant*>(static_cast<cISC4Occupant*>(message->GetVoid1()));
            if (occupant) DoNetworkOccupantChanged(occupant, type == 0x99ef1142);
            return true;
        }
        case 0xe8bda2e7: {
            ITrafficLot* lot = host_->LotFromMessage(message->GetIGZUnknown());
            if (message->GetData1() == 3) DoLotDeactivated(lot);
            else if (message->GetData2() == 3) DoLotActivated(lot);
            return true;
        }
        default: return true;
    }
}

// ---- the network model (traffic-network.md §4.2 - §4.6) ---------------------------------------------------------------------

void cSC4TrafficSimulator::DoConnectionsChanged(int x1, int z1, int x2, int z2) {
    if (cellsX_ <= 0 || cellsZ_ <= 0) return;
    const SC4RectInt32 r{std::max(x1 - 2, 0), std::max(z1 - 2, 0), std::min(x2 + 2, cellsX_ - 1), std::min(z2 + 2, cellsZ_ - 1)};
    InvalidateIntersectionData(r.x0, r.z0, r.x1, r.z1);
    for (int x = r.x0; x <= r.x1; ++x)
        for (int z = r.z0; z <= r.z1; ++z) {
            grid_[std::size_t(x)][std::size_t(z)].clear();
            CellConnections(x, z) = TrafficCellConnections{};
        }
    std::vector<cSC4NetworkOccupant*> occupants;
    host_->NetworkOccupantsInRect(r.x0, r.z0, r.x1, r.z1, occupants);
    auto setConnection = [&](int x, int z, int entry, int exit, int t) {   // SetConnection 0x0070EAB0
        CellConnections(x, z).masks[std::size_t(t)] |= std::uint16_t(1u << (exit + 4 * entry));
        const int nx = x + kSideX[exit], nz = z + kSideZ[exit];
        if (kNetworkOfType[t] == 1 && unsigned(nx) < unsigned(cellsX_) && unsigned(nz) < unsigned(cellsZ_))
            CellConnections(nx, nz).priorCar[std::size_t((exit + 2) & 3)] |= std::uint16_t(0x1111u << entry);
    };
    // Lane continuity (RemoveBadElevatedPaths 0x00715940, §4.4): a car leaving through `exit` after entering from `entry` may only
    // have come from the previous cell's lanes that end near this piece's lane starts, next to elevated or highway pieces.
    auto laneContinuity = [&](int cx, int cz, cISC4PathInfo* paths, int entry, int exit) {
        if (!paths) return;
        std::vector<std::uint32_t> keys;
        paths->GetPathsFromEntryEdge(1, std::uint8_t(entry), keys);
        std::vector<cS3DVector3> starts;
        for (std::uint32_t key : keys) {
            if ((key & 0xff) != std::uint32_t(exit)) continue;
            if (const tPath* path = paths->GetPath(key, nullptr); path && !path->points.empty()) starts.push_back(path->points.front());
        }
        if (starts.empty()) return;
        const int px = cx + kSideX[entry], pz = cz + kSideZ[entry];
        std::vector<cSC4NetworkOccupant*> previous;
        if (unsigned(px) < unsigned(cellsX_) && unsigned(pz) < unsigned(cellsZ_)) host_->NetworkOccupantsInRect(px, pz, px, pz, previous);
        for (cSC4NetworkOccupant* other : previous) {
            cISC4PathInfo* otherPaths = other->HasAnyNetworkFlag(0x1f4f) ? other->GetPathInfo() : nullptr;
            if (otherPaths) {
                std::uint16_t mask = 0;
                for (const cS3DVector3& v : starts)
                    for (std::uint32_t key : otherPaths->GetPathsNearEndPoint(1, v, 0xff, std::uint8_t((entry + 2) & 3), 4.0f))
                        mask = std::uint16_t(mask | (1u << ((key >> 8) & 0xff)));
                std::uint16_t& word = CellConnections(cx, cz).priorCar[std::size_t(entry)];
                word = std::uint16_t((word & ~(0xfu << (4 * exit))) | (mask << (4 * exit)));
            }
            other->Release();
        }
    };
    for (cSC4NetworkOccupant* occupant : occupants) {
        bool takesPart = false;
        for (int k = 0; k < 13 && !takesPart; ++k) takesPart = kTakesPart[k] && occupant->IsOfType(std::uint32_t(k));
        if (takesPart && !occupant->HasAnyNetworkFlag(kNetworkFlagNoTraffic)) {
            float position[3] = {};
            static_cast<const cSC4NetworkOccupant*>(occupant)->GetPosition(position);
            int cx = 0, cz = 0;
            host_->PositionToCell(position[0], position[2], cx, cz);
            if (unsigned(cx) < unsigned(cellsX_) && unsigned(cz) < unsigned(cellsZ_)) {
                cISC4PathInfo* paths = nullptr;
                bool pathsFetched = false;
                for (int n = 0; n < 6; ++n) {
                    const std::uint32_t p = kPathTypeOfNetwork[n];
                    std::uint32_t count = 0;
                    for (int e = 0; e < 4; ++e) {
                        bool found = false;
                        for (int x = 0; x < 4; ++x) {
                            if (!occupant->PathConnectionExists(p, std::uint8_t(e), std::uint8_t(x))) continue;
                            ++count;
                            found = true;
                            for (int t = 0; t < 9; ++t) {
                                if (kNetworkOfType[t] != n) continue;
                                CellConnections(cx, cz).masks[std::size_t(t)] |= std::uint16_t(1u << (x + 4 * e));
                                if (t == 1) {
                                    if (!pathsFetched) { paths = occupant->GetPathInfo(); pathsFetched = true; }
                                    laneContinuity(cx, cz, paths, e, x);
                                }
                            }
                        }
                        if (!found) {
                            if (!pathsFetched) { paths = occupant->GetPathInfo(); pathsFetched = true; }
                            if (paths && paths->GetPath(p, std::uint8_t(e), 0xff, 0, nullptr)) {   // the piece allows a turn-around
                                ++count;
                                for (int t = 0; t < 9; ++t)
                                    if (kNetworkOfType[t] == n) setConnection(cx, cz, e, e, t);
                            }
                        }
                    }
                    if ((count & 0xffff) == 0) continue;
                    std::vector<NetworkInfo>& cell = grid_[std::size_t(cx)][std::size_t(cz)];
                    NetworkInfo* info = FindNetworkInfo(n, cx, cz);
                    if (!info) {
                        if (!cell.empty()) cell.back().flag7 = 0;   // creation quirk: the old tail's flags are cleared
                        cell.push_back(NetworkInfo{});
                        info = &cell.back();
                        info->networkType = std::uint8_t(n);
                    }
                    info->connectionCount = std::uint8_t(count);
                    info->potholePenalty = n == 1 && IsRoadDamaged(cx, cz) ? tunables_.potholePenalty : 0.0f;
                    if (occupant->HasAnyNetworkFlag(kNetworkFlagBridge)) info->flag7 |= 1;
                    int type = -1;
                    for (int k = 0; k < 13; ++k) {
                        if (!((kTransitMask[k] >> n) & 1) || !occupant->IsOfType(std::uint32_t(k))) continue;
                        if (k == 3 && type != -1) continue;
                        if (k == 0 && type == 2) continue;
                        type = k;
                    }
                    info->pieceType = std::uint8_t(type < 0 ? 0 : type);
                    info->inverseCapacity = type < 0 ? 1.0f : tunables_.inverse_e58[std::size_t(type)];
                    for (int t = 0; t < 9; ++t) {
                        if (kNetworkOfType[t] != n) continue;
                        edgeDensities_.try_emplace(EdgeDensityKey(cx, cz, t, false));
                        edgeDensities_.try_emplace(EdgeDensityKey(cx, cz, t, true));
                    }
                }
            }
        }
        occupant->Release();
    }
    networkChanged_ = subnetworksStale_ = true;
    continueSearch_ = false;
}

// §4.6: car capacity falls near intersections (the tuned factors 0.7 / 0.8 / 0.9 at 0 / 1 / 2 cells).
void cSC4TrafficSimulator::UpdateIntersectionData() {
    if (!IntersectionDirty() || cellsX_ <= 0) return;
    const int n = tunables_.count_ec0;
    int x0, z0, x1, z1;
    GetIntersectionDirtyRect(x0, z0, x1, z1);
    x0 = std::max(x0 - n, 0); z0 = std::max(z0 - n, 0);
    x1 = std::min(x1 + n, cellsX_ - 1); z1 = std::min(z1 + n, cellsZ_ - 1);
    intersectionDirty_ = false;
    struct Frame { int x, z, depth, side; };
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z) {
            NetworkInfo* info = FindNetworkInfo(1, x, z);
            if (!info || info->pieceType == 2) continue;
            int best = n + 1;
            if (info->connectionCount > 2) best = 0;
            else {
                std::vector<Frame> stack{{x, z, 0, 0}};
                while (!stack.empty()) {
                    Frame& f = stack.back();
                    if (f.side == 0) {   // entering the cell
                        const NetworkInfo* here = FindNetworkInfo(1, f.x, f.z);
                        if (f.depth > 0 && here && here->connectionCount > 2) best = std::min(best, f.depth);
                        if (!here || f.depth >= best || (CellConnections(f.x, f.z).wormhole & 2)) { stack.pop_back(); continue; }
                    }
                    if (f.side >= 4) { stack.pop_back(); continue; }
                    const int side = f.side++;
                    const std::uint16_t conn = CellConnections(f.x, f.z).masks[1];
                    bool exits = false;
                    for (int e = 0; e < 4; ++e) exits = exits || ((conn >> (side + 4 * e)) & 1);
                    const int nx = f.x + kSideX[side], nz = f.z + kSideZ[side];
                    if (exits && unsigned(nx) < unsigned(cellsX_) && unsigned(nz) < unsigned(cellsX_))
                        stack.push_back({nx, nz, f.depth + 1, 0});
                }
            }
            info->inverseCapacity = tunables_.inverse_e58[info->pieceType];
            if (best < n && std::size_t(best) < tunables_.inverse_ec8.size())
                info->inverseCapacity = info->inverseCapacity * tunables_.inverse_ec8[std::size_t(best)];
        }
}

void cSC4TrafficSimulator::BumpCellCount(int type, bool inserted) {
    std::uint32_t& c = cellCounts_[std::size_t(type)];
    if (inserted) ++c;
    else if (c > 0) --c;
}

void cSC4TrafficSimulator::SyncBudgetCounters() {
    counters_.c908 = cellCounts_[0]; counters_.c90c = cellCounts_[1]; counters_.c910 = cellCounts_[2]; counters_.c914 = cellCounts_[3];
    counters_.c920 = cellCounts_[6]; counters_.c924 = cellCounts_[7]; counters_.c928 = cellCounts_[8]; counters_.c92c = cellCounts_[9];
    counters_.c930 = cellCounts_[10]; counters_.c938 = cellCounts_[12];
}

// §3.6: the per-type cell counts (and the department costs from them) and the intact / damaged road and rail cell sets.
void cSC4TrafficSimulator::NetworkBookkeeping(cSC4NetworkOccupant* occupant, bool inserted, bool budgets) {
    float position[3] = {};
    static_cast<const cSC4NetworkOccupant*>(occupant)->GetPosition(position);
    int x = 0, z = 0;
    host_->PositionToCell(position[0], position[2], x, z);
    const std::uint16_t key = std::uint16_t(((x & 0xff) << 8) | (z & 0xff));
    auto roadChanged = [&](int t) {   // DoRoadChanged 0x007155D0
        BumpCellCount(t, inserted);
        SyncBudgetCounters();
        if (budgets) UpdateRoadsBudget();
        if (t == 2 || t == 12) return;
        if (inserted) healthyRoads_.Insert(key), ++roadPotholeTotal_;
        else {
            if (!healthyRoads_.Erase(key) && damagedRoads_.Erase(key)) {
                --roadPotholeCount_;
                --statRoadPotholes_;
                host_->PostPotholeMessage3(x, z, 0);
            }
            --roadPotholeTotal_;
        }
    };
    if (occupant->IsOfType(6)) roadChanged(6);
    if (occupant->IsOfType(0)) roadChanged(0);
    for (int t : {10, 3, 2, 12})
        if (occupant->IsOfType(std::uint32_t(t))) { roadChanged(t); break; }
    if (occupant->IsOfType(1)) {   // DoRailChanged 0x00715790
        BumpCellCount(1, inserted);
        if (!occupant->IsOfType(0) && !occupant->IsOfType(3)) {
            std::uint32_t ox = 0, oz = 0;
            occupant->GetOccupiedCell(ox, oz);
            const std::uint16_t railKey = std::uint16_t(((ox & 0xff) << 8) | (oz & 0xff));
            if (inserted) healthyRail_.Insert(railKey), ++railPotholeTotal_;
            else {
                if (!healthyRail_.Erase(railKey) && damagedRail_.Erase(railKey)) {
                    --railPotholeCount_;
                    --statRoadPotholes_;   // quirk: the road pothole count
                    host_->PostPotholeMessage3(int(ox), int(oz), 0);
                }
                --railPotholeTotal_;
            }
        }
        SyncBudgetCounters();
        if (budgets) UpdateMassTransitBudget();
    }
    if (occupant->IsOfType(7) && !(occupant->GetNetworkFlag() & 0x80000000u)) {
        BumpCellCount(7, inserted);
        SyncBudgetCounters();
        if (budgets) UpdateMassTransitBudget();
    }
}

// §3.6 bookkeeping, then the network part (traffic-network.md §3).
void cSC4TrafficSimulator::DoNetworkOccupantChanged(cSC4NetworkOccupant* occupant, bool inserted) {
    NetworkBookkeeping(occupant, inserted, true);
    float position[3] = {};
    static_cast<const cSC4NetworkOccupant*>(occupant)->GetPosition(position);
    int x = 0, z = 0;
    host_->PositionToCell(position[0], position[2], x, z);
    bool takesPart = false;
    for (int k = 0; k < 13 && !takesPart; ++k) takesPart = kTakesPart[k] && occupant->IsOfType(std::uint32_t(k));
    if (!takesPart || unsigned(x) >= unsigned(cellsX_) || unsigned(z) >= unsigned(cellsZ_)) return;
    if (!inserted)
        for (int t = 0; t < 9; ++t) {
            edgeDensities_.erase(EdgeDensityKey(x, z, t, false));
            edgeDensities_.erase(EdgeDensityKey(x, z, t, true));
        }
    DoConnectionsChanged(x, z, x, z);
}

// ---- lots (§4.1.3) ----------------------------------------------------------------------------------------------------------

void cSC4TrafficSimulator::DoLotActivated(ITrafficLot* lot) {
    if (!lot) return;
    cISCPropertyHolder* holder = lot->Properties();
    int lx = 0, lz = 0;
    lot->Location(lx, lz);
    if (holder) {
        const std::uint32_t buckets = tunables_.value_ea4 ? tunables_.value_ea4 : 1;
        const std::uint32_t b = strategyBucketCounter_;
        strategyBucketCounter_ = (strategyBucketCounter_ + 1) % buckets;
        cSCPropertyHelp::SetPropertyValue(holder, kLotStrategyBucket, b);
        strategyBuckets_.emplace(b, std::make_pair(lx, lz));
        cSCPropertyHelp::SetPropertyValue(holder, kLotStrategyDraw, std::uint8_t(random_->RandomUint32Uniform(100)));
    }
    if (cycleState_ == 10 || !holder) return;
    std::uint8_t bits = 0;
    for (int k = 0; k < 4; ++k)
        if (GetMaxTripCapacity(holder, std::uint32_t(k)) > 0) {
            bits = std::uint8_t(bits | (1u << k));
            ++connectionCounts_[std::size_t(k)];
        }
    SC4RectInt32 rect;
    lot->OccupiedRect(rect);
    for (int x = rect.x0; x <= rect.x1; ++x)
        for (int z = rect.z0; z <= rect.z1; ++z) graph_->SetDestinationByte(x, z, bits);
}

void cSC4TrafficSimulator::DoLotDeactivated(ITrafficLot* lot) {
    if (!lot) return;
    cISCPropertyHolder* holder = lot->Properties();
    int lx = 0, lz = 0;
    lot->Location(lx, lz);
    std::uint32_t b = 0;
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, kLotStrategyBucket, b)) {
        for (auto it = strategyBuckets_.lower_bound(b); it != strategyBuckets_.end() && it->first == b; ++it)
            if (it->second == std::make_pair(lx, lz)) { strategyBuckets_.erase(it); break; }
    }
    SC4RectInt32 rect;
    lot->OccupiedRect(rect);
    if (cycleState_ != 10) {
        const std::uint8_t bits = DestinationByte(rect.x0, rect.z0);
        for (int k = 0; k < 4; ++k)
            if ((bits >> k) & 1) --connectionCounts_[std::size_t(k)];
    }
    for (int x = rect.x0; x <= rect.x1; ++x)
        for (int z = rect.z0; z <= rect.z1; ++z) graph_->SetDestinationByte(x, z, 0);
}

// ---- DoPostCityInit (§3.5) ------------------------------------------------------------------------------------------------

void cSC4TrafficSimulator::DoPostCityInit() {
    if (!initialized_ || cellsX_ <= 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    if (std::getenv("SC4_TRAFFICDUMP")) DumpTrips("trips_saved.txt");
    // The saved cell counts and pothole sets are not read (the record is replayed as loaded): the pieces give them again, with the
    // occupant-message rules of §3.6.
    cellCounts_.fill(0);
    healthyRoads_.Clear(); damagedRoads_.Clear(); healthyRail_.Clear(); damagedRail_.Clear();
    roadPotholeTotal_ = railPotholeTotal_ = 0;
    roadPotholeCount_ = railPotholeCount_ = 0;
    // the saved edge densities; DoConnectionsChanged below adds the missing network cells and drops cells that lost their network
    if (loaded_.valid) {
        for (const auto& [key, record] : loaded_.edgeDensities) edgeDensities_[key] = record;
        for (const auto& [id, holder] : loaded_.pools) neighborPools_[id] = holder;   // a repeated id: the last one wins
        if (loaded_.destinations.size() == cellTripFlags_.size())
            for (std::size_t row = 0; row < cellTripFlags_.size(); ++row)
                if (loaded_.destinations[row].size() == cellTripFlags_[row].size()) cellTripFlags_[row] = loaded_.destinations[row];
    }
    {
        std::vector<cSC4NetworkOccupant*> all;
        host_->NetworkOccupantsInRect(0, 0, cellsX_ - 1, cellsZ_ - 1, all);
        for (cSC4NetworkOccupant* occupant : all) {
            NetworkBookkeeping(occupant, true, false);
            occupant->Release();
        }
    }
    DoConnectionsChanged(0, 0, cellsX_ - 1, cellsZ_ - 1);
    // register the neighbour connections (an Add for each, in list order), then one pool sweep (traffic-network.md §2.7.2)
    std::vector<void*> connections;
    host_->NeighborGetAllConnections(connections);
    for (void* connection : connections) {
        int x = 0, z = 0;
        host_->NeighborConnectionCell(connection, x, z);
        const int type = host_->NeighborConnectionType(connection);
        if (type >= 0) AddNeighborConnection(type, x, z);
        if (Debug()) {
            const TrafficNeighborConnection* record = GetTrafficNeighborConnection(x, z);
            std::fprintf(stderr, "traffic: neighbour connection type %d at (%d,%d): other side %s, city %s, pool %u cost %.2f\n", type, x, z,
                         host_->NeighborFindConnectionOnOtherSide(connection) ? "yes" : "no", host_->NeighborConnectionCity(connection) ? "yes" : "no",
                         record ? record->pool : 0u, record ? record->cost : 0.0f);
        }
    }
    RemoveUnusedPools();
    for (int x = -1; x <= cellsX_; ++x)
        for (int z = -1; z <= cellsZ_; ++z) {
            const bool ring = x == -1 || z == -1 || x == cellsX_ || z == cellsZ_;
            if (ring && !HasMapExit(x, z)) graph_->SetDestinationByte(x, z, 0);
        }
    UpdateSubnetworks();       // §1 step 7: the labels, then the info at once
    UpdateSubnetworkInfo();
    // The destination map and its free counts come from the record; a city without one builds them from the lots (as states 5
    // and 10 do). Then the interrupted phase resumes (§3.5 steps 8 and 11).
    if (loaded_.valid) {
        for (int k = 0; k < 4; ++k) connectionCounts_[std::size_t(k)] = int(loaded_.freeDestinations[std::size_t(k)]);
    } else {
        std::fill(connectionCounts_.begin(), connectionCounts_.end(), 0);
        cycleState_ = 0;
        WorkOnClearTripDestinations();
        WorkOnRefreshCommuteMap();
    }
    if (Debug()) {
        double road = 0;
        if (volume_[1]) for (auto& column : volume_[1]->GetGridData()) for (float v : column) road += v;
        std::fprintf(stderr, "traffic: volume at load: road %.0f\n", road);
    }
    ResumeLoadedCycle();
    // strategy buckets from the lots, in list order
    strategyBuckets_.clear();
    std::vector<std::unique_ptr<ITrafficLot>> lots;
    host_->AllLots(lots);
    for (auto& lot : lots) {
        std::uint32_t b = 0;
        int lx = 0, lz = 0;
        lot->Location(lx, lz);
        if (lot->Properties() && cSCPropertyHelp::GetPropertyValue(lot->Properties(), kLotStrategyBucket, b)) strategyBuckets_.emplace(b, std::make_pair(lx, lz));
    }
    days_ = std::uint32_t(lots.size()) / 18;   // step 13: total, dayIndex and processed keep their loaded values
    UpdateMassTransitBudget();
    UpdateRoadsBudget();
    UpdateIntersectionData();
    if (Debug()) {
        std::size_t cells = 0, records = 0;
        for (int x = 0; x < cellsX_; ++x)
            for (int z = 0; z < cellsZ_; ++z) {
                records += grid_[std::size_t(x)][std::size_t(z)].size();
                cells += grid_[std::size_t(x)][std::size_t(z)].empty() ? 0 : 1;
            }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "traffic: post city init: %zu network cells, %zu records, %zu lots, free destinations %d/%d/%d/%d (%.1f ms)\n", cells,
                     records, lots.size(), connectionCounts_[0], connectionCounts_[1], connectionCounts_[2], connectionCounts_[3], ms);
    }
    host_->EndOfWork();
}

// §3.5 steps 8 and 11: the cycle goes on from the saved state. 16-18 hold the source at the saved phase cell again (ConfirmTrips
// takes a source whole, so it goes on after it; the trip searches go on inside it), 15 goes on after the lot at that cell, 12/13
// become 8, 1/14 restart at 0. A city without a record (new) starts at 0.
void cSC4TrafficSimulator::ResumeLoadedCycle() {
    const LoadedCycle& L = loaded_;
    if (!L.valid) return;
    tripCapacityTotals_ = {int(L.jobCapacity[0]), int(L.jobCapacity[1]), int(L.jobCapacity[2]), tripCapacityTotals_[3]};
    attempted_ = L.attempted;
    routed_ = L.routed;
    carLenSum_ = L.carLenSum; carLenCount_ = L.carLenCount;
    freightLenSum_ = L.freightLenSum; freightCount_ = L.freightCount;
    typeCountsPublished_ = L.typeCountsPublished;
    typeCountsBuilding_ = L.typeCountsBuilding;
    processed_ = L.processed; dayIndex_ = L.dayIndex; total_ = L.total;
    passTotal_ = L.passTotal; toReRoute_ = L.toReRoute; toSearch_ = L.toSearch;
    passCounters_ = L.passCounters;
    rerouteBucket_ = std::uint8_t(L.passCounters[2] & 3);
    strategyBucketCounter_ = L.strategyBucketCounter;
    strategyBucket_ = L.strategyBucket;
    std::uint32_t state = L.state;
    if (state == 12 || state == 13) state = 8;
    if (state == 1 || state == 14 || state > 19) state = 0;
    cycleState_ = state;
    std::size_t held = 0;
    bool found = false;
    if (state >= 15 && state <= 18) {
        ResetSources();
        for (std::size_t i = 0; i < sources_.size() && !found; ++i) {
            int x = 0, z = 0;
            sources_[i]->Location(x, z);
            if (std::int16_t(x) == L.phaseX && std::int16_t(z) == L.phaseZ) { held = i; found = true; }
        }
        sourceIndex_ = !found ? 0 : (state == 15 || state == 16) ? held + 1 : held;
    }
    if (Debug())
        std::fprintf(stderr, "traffic: resume state %u at (%d,%d): source %zu of %zu%s, processed %u of %u, day %u, %zu edge densities\n", state, L.phaseX,
                     L.phaseZ, sourceIndex_, sources_.size(), found ? "" : " (cell not found)", processed_, total_, dayIndex_, L.edgeDensities.size());
}

// ---- the work cycle (§3.2 - §3.4) -------------------------------------------------------------------------------------------

void cSC4TrafficSimulator::ResetSources() {
    sources_.clear();
    host_->AllLots(sources_);
    sourceIndex_ = 0;
}

void cSC4TrafficSimulator::EnterState(std::uint32_t state) {
    cycleState_ = state;
    switch (state) {
        case 16: case 17: case 18:
            ResetSources();
            if (state == 18) typeCountsBuilding_.fill(0);
            break;
        case 15: case 1: ResetSources(); break;
        case 2:
            for (auto& row : statsWork_) row = TrafficStatsRow{};
            statsCompare_.fill(-1.0f);
            commercialWork_->SetTractValues(0);
            break;
        case 4: commercialMap_->GetGridData() = commercialWork_->GetGridData(); break;
        case 10: std::fill(connectionCounts_.begin(), connectionCounts_.end(), 0); break;
        default: break;
    }
}

bool cSC4TrafficSimulator::WorkOnSimCycle() {
    UpdateIntersectionData();
    int units = unitsPerSlice_;
    bool finished = true;
    switch (cycleState_) {
        case 0: StartRecalc(); break;
        case 16: finished = ConfirmTripsPhase(units); break;
        case 17: finished = SearchPhase(true, units); break;
        case 18: finished = SearchPhase(false, units); break;
        case 15: FinishTripMap(); break;
        case 2: CreateMaps(); break;
        case 4: CreateFinalMap(); break;
        case 5:
            WorkOnClearTripDestinations();
            if (Debug())
                std::fprintf(stderr, "traffic: job capacity $%d $$%d $$$%d freight %d; commuters $%u $$%u $$$%u\n", tripCapacityTotals_[0],
                             tripCapacityTotals_[1], tripCapacityTotals_[2], tripCapacityTotals_[3], attempted_[0], attempted_[1], attempted_[2]);
            break;
        case 11: WorkOnUpdateNeighborConnections(); break;
        case 3: ReduceTransitSwitches(); break;
        case 7: DumpEdgeDensities(); break;
        case 6: ReduceEdgeDensities(); break;
        case 10: WorkOnRefreshCommuteMap(); break;
        case 8: FinalCalculations(); break;
        case 12: UpdateSubnetworks(); break;
        case 13: UpdateSubnetworkInfo(); break;
        default: break;   // 19 (ferries): PARTIAL, nothing to do natively yet
    }
    if (!finished) return false;
    std::uint32_t next = kNext[cycleState_ % 20];
    if (next == 12 && !subnetworksStale_) next = 13;
    EnterState(next);
    return true;
}

// §3.3: one call per simulator delivery; false = not done for today (the simulator calls again within the day).
bool cSC4TrafficSimulator::DoDailySimulation() {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(4);
    const int day = host_->DayOfMonth();
    if (day != lastDayOfMonth_) {
        lastDayOfMonth_ = day;
        if (day == 1) {
            host_->SetBudgetIncome(true, 2, std::int64_t(transitIncome_));
            host_->SetBudgetIncome(false, 5, std::int64_t(tollIncome_));
            SetLastMonthlyIncome(int(tollIncome_ + transitIncome_));
        }
        const std::uint32_t buckets = tunables_.value_ea4 ? tunables_.value_ea4 : 1;
        const std::uint32_t period = std::max<std::uint32_t>(1, tunables_.value_ea0 / buckets);
        if (host_->SimDateNumber() % period == 0) {
            for (auto it = strategyBuckets_.lower_bound(strategyBucket_); it != strategyBuckets_.end() && it->first == strategyBucket_; ++it)
                if (ITrafficLot* lot = host_->LotAt(it->second.first, it->second.second, 0))
                    if (lot->Properties())
                        cSCPropertyHelp::SetPropertyValue(lot->Properties(), kLotStrategyDraw, std::uint8_t(random_->RandomUint32Uniform(100)));
            strategyBucket_ = (strategyBucket_ + 1) % buckets;
        }
    }
    bool result = false;
    for (;;) {
        if (Clock::now() >= deadline) break;
        const bool finished = WorkOnSimCycle();
        if (finished) {
            const std::uint32_t s = cycleState_;
            if (s == 16) {
                total_ = passTotal_;
                days_ = std::max<std::uint32_t>(1, total_ / 20);
                dayIndex_ = processed_ = 0;
                toReRoute_ = toSearch_ = 0;
                const std::uint32_t keep1 = passCounters_[1], keep2 = passCounters_[2];
                passCounters_.fill(0);
                passCounters_[1] = keep1;
                passCounters_[2] = keep2;
                continue;
            }
            if (s == 17) {
                total_ = toReRoute_;
                days_ = std::max<std::uint32_t>(1, total_ / 18);
                dayIndex_ = processed_ = 0;
                passCounters_[1] = 0;
                result = true;
                break;
            }
            if (s == 18) {
                total_ = toSearch_;
                days_ = std::max<std::uint32_t>(1, total_ / 18);
                dayIndex_ = processed_ = 0;
                passTotal_ = 0;
                result = true;
                break;
            }
            if (s == 2 || s == 11) { result = true; break; }
            continue;
        }
        if (cycleState_ >= 16 && cycleState_ <= 18 && dayIndex_ < days_ &&
            std::uint64_t(processed_) >= std::uint64_t(total_) * (dayIndex_ + 1) / days_) {
            ++dayIndex_;
            result = true;
            break;
        }
    }
    host_->EndOfWork();
    return result;
}

// ---- states 0, 16, 17, 18 (§4.3, §4.4) ------------------------------------------------------------------------------------

void cSC4TrafficSimulator::StartRecalc() {
    incomeAcc_ = feeAcc_ = 0.0;
    tripCapacityTotals_.fill(0);
    attempted_.fill(0);
    routed_.fill(0);
    rerouteBucket_ = std::uint8_t((rerouteBucket_ + 1) & 3);
    passCounters_[2] = rerouteBucket_;
    carLenSum_ = freightLenSum_ = 0;
    carLenCount_ = freightCount_ = 0;
    RecalculateNeighborConnectionCapacities();
}

bool cSC4TrafficSimulator::ConfirmTripsPhase(int& units) {
    while (sourceIndex_ < sources_.size()) {
        if (units-- <= 0) return false;
        ITrafficLot& source = *sources_[sourceIndex_++];
        source.RefreshTrafficSource(source.TrafficSource());   // UpdateTrafficVolume
        ConfirmSourceTrips(source);
    }
    return true;
}

void cSC4TrafficSimulator::ConfirmSourceTrips(ITrafficLot& source) {
    std::list<cSC4TripData>* trips = source.TripList(source.TrafficSource());
    if (!trips) return;
    const bool reroute = ((++passCounters_[3]) & 3) == rerouteBucket_;
    for (const cSC4TripData& trip : *trips) passCounters_[7] += trip.kind == 0 ? 1 : 0;
    if (reroute) {
        for (auto a = trips->begin(); a != trips->end(); ++a) {
            if (a->kind != 3) continue;
            bool absorbed = false;
            for (auto b = std::next(a); b != trips->end();) {
                if (b->type == a->type && b->index == a->index) {
                    a->count += b->count;
                    a->kind = 0;
                    absorbed = true;
                    b = trips->erase(b);
                } else {
                    ++b;
                }
            }
            if (absorbed) ++passCounters_[5];
        }
    }
    SC4RectInt32 rect;
    source.OccupiedRect(rect);
    for (cSC4TripData& trip : *trips) {
        ++processed_;
        if (trip.kind == 3) {
            if (CapacityStillAvailable(trip)) {
                trip.kind = trip.type == 3 ? 0 : 1;
                if (!reroute && ConfirmRoute(trip.route)) {
                    const bool probe = (trip.flags & 2) != 0;
                    if (trip.type == 3) {
                        if (!probe) ApplyRoute(trip.route, trip.count, rect, trip.sourceX, trip.sourceZ, 0, trip.type);
                        trip.kind = 3;
                    } else if (trip.flags & 1) {
                        if (!probe) ApplyRoute(trip.route, trip.count, rect, trip.sourceX, trip.sourceZ, 2, trip.type);
                        trip.kind = 3;
                    } else if (ConfirmRoute(trip.routeReversed)) {
                        if (!probe) {
                            ApplyRoute(trip.route, trip.count, rect, trip.sourceX, trip.sourceZ, 0, trip.type);
                            ApplyRoute(trip.routeReversed, trip.count, rect, trip.sourceX, trip.sourceZ, 1, trip.type);
                        }
                        trip.kind = 3;
                    } else {
                        ++passCounters_[6];
                    }
                    if (trip.kind == 3) ++passCounters_[0];
                }
            } else {
                trip.kind = 0;
                ++passCounters_[4];
            }
        } else if (trip.kind == 5) {
            trip.kind = 0;
            ++passCounters_[8];
        }
        if (trip.kind == 1) ++toReRoute_;
        else if (trip.kind == 0) ++toSearch_;
    }
}

// SetupTrip 0x0070EC40 for pf (the outbound leg).
void cSC4TrafficSimulator::SetupTripFor(ITrafficLot& source, cSC4TripData& trip) {
    cSC4PathFinder* pf = pf_;
    pf->SetTrafficDensity(trip.count);
    if (trip.kind == 1) {
        pf->SetTripDestinationFilter(3);
        SC4RectInt32 rect{trip.sourceX, trip.sourceZ, trip.sourceX, trip.sourceZ};
        if (unsigned(trip.sourceX) < unsigned(cellsX_) && unsigned(trip.sourceZ) < unsigned(cellsZ_))
            if (ITrafficLot* lot = host_->LotAt(trip.sourceX, trip.sourceZ, 0)) lot->OccupiedRect(rect);
        pf->SetEndRect(rect);
    } else {
        pf->SetTripDestinationFilter(0);
        pf->ClearAllDestinationFlags();
        pf->SetDestinationFlag((trip.flags & 2) && trip.type == 0 ? 7 : std::uint8_t(1u << (trip.type & 7)));
    }
    pf->SetMaxWorkTime(0);
    pf2_->SetMaxWorkTime(0);
    SC4RectInt32 start;
    source.OccupiedRect(start);
    pf->SetStartRect(start);
    pf->ClearAllFlags();
    pf->ClearAllTravelTypeFlags();
    pf->SetStartingCostArray(nullptr);
    pf->SetMaxTripLength(tunables_.tripScale);
    if (trip.type <= 2) {
        if (continueSearch_) pf->SetFlag(4);
        pf->SetTravelTypeFlag(0x1cf, false);
        if (trip.flags & 4) {
            pf->SetTravelTypeFlag(std::uint16_t(1u << (trip.index & 0xf)), true);
        } else {
            pf->SetTravelTypeFlag(0x3, true);
            float* costs = trip.index == 0 ? tunables_.values_f04.data() : trip.index == 1 ? tunables_.values_f28.data() : tunables_.values_ee0.data();
            pf->SetStartingCostArray(costs);
        }
    } else {
        pf->SetFlag(1);
        pf->SetTravelTypeFlag(0x30, true);
    }
}

// PerformPathSearch 0x0071C6C0 (searches never suspend here: no work-time limit).
int cSC4TrafficSimulator::PerformPathSearch(ITrafficLot& source, cSC4TripData& trip) {
    if (pf_->FindPath() != 1) return 0;
    int ex = 0, ez = 0;
    pf_->GetEndCell(ex, ez);
    trip.sourceX = std::int16_t(ex);
    trip.sourceZ = std::int16_t(ez);
    SC4RectInt32 sourceRect;
    source.OccupiedRect(sourceRect);
    const bool probe = (trip.flags & 2) != 0;
    if (trip.type == 3 || pf_->WasTripReversible()) {
        if (trip.type != 3) trip.flags |= 1;
        pf_->GetCompressedRoute(trip.route, trip.extra);
        if (!probe) ApplyRoute(trip.route, pf_->GetTrafficArrived(), sourceRect, ex, ez, trip.type == 3 ? 0 : 2, trip.type);
        return 1;
    }
    trip.flags &= std::uint8_t(~1u);
    // the return leg: pf2 from the destination back to the source (traffic-simulator.md §4.4, TS17)
    if (unsigned(ex) < unsigned(cellsX_) && unsigned(ez) < unsigned(cellsZ_)) {
        if (ITrafficLot* lot = host_->LotAt(ex, ez, 0)) {
            SC4RectInt32 r;
            lot->OccupiedRect(r);
            pf2_->SetStartRect(r);
        }
    } else {
        int hx = ex, hz = ez;
        const std::uint8_t last = pf_->GetLastTravelType();
        const TrafficNeighborConnection* record = GetTrafficNeighborConnection(ex, ez);
        if ((last == 1 || last == 2) && record && (record->types & 0x1108)) FindHighwayReturnTile(hx, hz, false);
        pf2_->SetStartRect(SC4RectInt32{hx, hz, hx, hz});
    }
    SC4RectInt32 end = pf_->StartRect();
    if (!(unsigned(end.x0) < unsigned(cellsX_) && unsigned(end.z0) < unsigned(cellsZ_))) {
        const TrafficNeighborConnection* record = GetTrafficNeighborConnection(end.x0, end.z0);
        if (record && (record->types & 0x1108)) {
            int hx = end.x0, hz = end.z0;
            FindHighwayReturnTile(hx, hz, true);
            end = {hx, hz, hx, hz};
        }
    }
    pf2_->SetEndRect(end);
    pf2_->ClearAllFlags();
    pf2_->SetFlag(1);
    pf2_->SetTrafficDensity(pf_->GetTrafficArrived());
    pf2_->SetTripDestinationFilter(3);
    pf2_->SetTravelTypeMasks(pf_->AllowedTravelTypes(), std::uint16_t(1u << (pf_->GetLastTravelType() & 0xf)));
    pf2_->SetStartingCostArray(const_cast<float*>(pf_->StartingCosts()));
    int px = 0, pz = 0;
    std::uint8_t side = 0;
    if (pf_->ParkAndRide(px, pz, side)) {
        pf2_->SetParkAndRide(px, pz, side);
        pf2_->SetFlag(8);
    }
    if (pf2_->FindPath() != 1) return 0;
    std::uint32_t backTypes = 0;
    pf_->GetCompressedRoute(trip.route, trip.extra);
    pf2_->GetCompressedRoute(trip.routeReversed, backTypes);
    trip.extra |= backTypes;
    if (!probe) {
        ApplyRoute(trip.route, pf_->GetTrafficArrived(), sourceRect, ex, ez, 0, trip.type);
        ApplyRoute(trip.routeReversed, pf_->GetTrafficArrived(), sourceRect, ex, ez, 1, trip.type);
    }
    return 1;
}

// ReRouteTrips (17) and FullTrips (18): the source's status-1 (re-route) or status-0 (full search) trips.
bool cSC4TrafficSimulator::SearchPhase(bool reRoute, int& units) {
    while (sourceIndex_ < sources_.size()) {
        ITrafficLot& source = *sources_[sourceIndex_];
        std::list<cSC4TripData>* trips = source.TripList(source.TrafficSource());
        if (trips) {
            for (auto it = trips->begin(); it != trips->end(); ++it) {
                cSC4TripData& trip = *it;
                if (trip.kind != (reRoute ? 1 : 0)) continue;
                if (units-- <= 0) return false;
                if (reRoute && !CapacityStillAvailable(trip)) {
                    trip.kind = 0;
                    ++toSearch_;
                    ++passCounters_[4];
                    continue;
                }
                SetupTripFor(source, trip);
                const int found = PerformPathSearch(source, trip);
                ++processed_;
                if (found) {
                    trip.kind = 3;
                    trip.distance = pf_->GetTripLength();
                    if (reRoute) {
                        ++passCounters_[1];
                    } else {
                        const std::uint32_t arrived = pf_->GetTrafficArrived();
                        if (arrived < trip.count) {
                            cSC4TripData rest = trip;
                            rest.count = trip.count - arrived;
                            rest.kind = 0;
                            trip.count = arrived;
                            trips->insert(std::next(it), rest);
                            continueSearch_ = true;
                            ++toSearch_;
                            ++total_;
                            ++passCounters_[9];
                        } else {
                            continueSearch_ = false;
                        }
                    }
                } else {
                    trip.kind = 5;
                    continueSearch_ = false;
                }
            }
            if (reRoute) {   // merge into each status-0 trip every later trip of its type and strategy
                for (auto a = trips->begin(); a != trips->end(); ++a) {
                    if (a->kind != 0) continue;
                    for (auto b = std::next(a); b != trips->end();) {
                        if (b->type == a->type && b->index == a->index) {
                            a->count += b->count;
                            b = trips->erase(b);
                        } else {
                            ++b;
                        }
                    }
                }
            } else {
                CompleteTripSourceCalculations(source);
            }
        }
        ++sourceIndex_;
        continueSearch_ = false;
    }
    if (!reRoute) std::swap(typeCountsBuilding_, typeCountsPublished_);
    return true;
}

// §4.6: the source's average trip length into the trip-length map (0 when any trip failed).
void cSC4TrafficSimulator::CompleteTripSourceCalculations(ITrafficLot& source) {
    std::list<cSC4TripData>* trips = source.TripList(source.TrafficSource());
    if (!trips) return;
    float acc = 0;
    std::uint32_t n = 0;
    bool failed = false;
    for (cSC4TripData& trip : *trips) {
        ++passTotal_;
        const bool probe = (trip.flags & 2) != 0;
        if (trip.kind == 3) {
            trip.flags |= 8;
            const float t = float(trip.count) * trip.distance;
            acc += t;
            n += trip.count;
            if (!probe) {
                if (trip.type < 3) {
                    attempted_[trip.type] += trip.count;
                    routed_[trip.type] += trip.count;
                    carLenSum_ += float(trip.count) * trip.distance;
                    carLenCount_ += trip.count;
                } else {
                    freightLenSum_ += t;
                    freightCount_ += trip.count;
                }
                for (int i = 0; i < 9; ++i)
                    if ((trip.extra >> i) & 1) typeCountsBuilding_[std::size_t(i)] += trip.count;
            }
        } else if (trip.kind == 5) {
            trip.flags &= std::uint8_t(~8u);
            failed = true;
            const float t = float(trip.count) * tunables_.tripScale;
            acc += t;
            n += trip.count;
            if (trip.type < 3) {
                if (!probe) attempted_[trip.type] += trip.count;
            } else {
                freightLenSum_ += t;
                freightCount_ += trip.count;
            }
        } else {
            trip.flags &= std::uint8_t(~8u);
        }
    }
    if (n == 0) return;
    const float average = acc / float(std::int32_t(n));
    SC4RectInt32 rect;
    source.OccupiedRect(rect);
    if (!(unsigned(rect.x0) < unsigned(cellsX_) && unsigned(rect.z0) < unsigned(cellsZ_))) return;
    std::uint8_t v = 0;
    if (!failed) {
        const std::uint32_t r = RoundHalfEven(average / tunables_.tripScale * 255.0f);
        v = r > 255 ? 255 : (r == 0 ? 1 : std::uint8_t(r));
    }
    for (int x = rect.x0; x <= rect.x1 && x < cellsX_; ++x)
        for (int z = rect.z0; z <= rect.z1 && z < cellsZ_; ++z) tripLengthMap_->SetTractValue(x, z, v);
}

// ---- states 15, 2, 4, 3, 7, 6, 8 ----------------------------------------------------------------------------------------------

// §4.7: residential lots that are not traffic sources of their own (zones 8, 9 without road access) borrow a neighbour's value.
void cSC4TrafficSimulator::FinishTripMap() {
    for (auto& lotPtr : sources_) {
        ITrafficLot& lot = *lotPtr;
        lot.ClearTripFlag();
        const int zone = lot.ZoneType();
        if (!(zone == 8 || zone == 9) || lot.HasRoadAccess()) continue;
        int lx = 0, lz = 0;
        lot.Location(lx, lz);
        std::uint8_t value = 0;
        if (ITrafficLot* linked = lot.LinkedLot()) {
            int kx = 0, kz = 0;
            linked->Location(kx, kz);
            value = tripLengthMap_->GetCellValue(kx, kz);
        } else {
            bool open[4] = {true, true, true, true};
            bool done = false;
            for (int distance = 1; !done && (open[0] || open[1] || open[2] || open[3]); ++distance) {
                for (int d = 0; d < 4 && !done; ++d) {
                    if (!open[d]) continue;
                    const int x = lx + distance * kSideX[d], z = lz + distance * kSideZ[d];
                    ITrafficLot* other = unsigned(x) < unsigned(cellsX_) && unsigned(z) < unsigned(cellsZ_) ? host_->LotAt(x, z, 0) : nullptr;
                    if (!other || other->ZoneType() != zone) { open[d] = false; continue; }
                    if (other->IsSameLot(&lot) || !other->HasRoadAccess()) continue;
                    value = tripLengthMap_->GetCellValue(x, z);
                    done = true;
                }
            }
        }
        SC4RectInt32 rect;
        lot.OccupiedRect(rect);
        for (int x = rect.x0; x <= rect.x1 && x < cellsX_; ++x)
            for (int z = rect.z0; z <= rect.z1 && z < cellsZ_; ++z)
                if (x >= 0 && z >= 0) tripLengthMap_->SetTractValue(x, z, value);
    }
}

// §4.9
void cSC4TrafficSimulator::CreateMaps() {
    if (Debug()) {
        double sums[6] = {};
        for (int n = 0; n < 6; ++n)
            if (volume_[std::size_t(n)])
                for (auto& column : volume_[std::size_t(n)]->GetGridData()) for (float v : column) sums[n] += v;
        std::fprintf(stderr, "traffic: volume before maps: walk %.0f road %.0f rail %.0f subway %.0f el %.0f mono %.0f\n", sums[0], sums[1], sums[2], sums[3], sums[4], sums[5]);
    }
    auto usage = [&](int x, int z, int t, bool evening) -> std::uint16_t {
        const auto it = edgeDensities_.find(EdgeDensityKey(x & 0xff, z & 0xff, t, evening));
        return it == edgeDensities_.end() ? 0 : it->second.density;
    };
    auto cellValue = [&](int x, int z, bool evening) -> std::uint16_t {
        const std::uint16_t road = std::uint16_t(usage(x, z, 1, evening) + usage(x, z, 2, evening) + usage(x, z, 4, evening));
        return std::max({road, usage(x, z, 7, evening), usage(x, z, 8, evening)});
    };
    const float commercialScale = 0.128f;
    std::array<float, 14> sum{};
    std::array<std::uint32_t, 14> count{}, congested{};
    std::array<std::uint16_t, 14> records{};
    for (int z = 0; z < cellsZ_; ++z)
        for (int x = 0; x < cellsX_; ++x) {
            float b = 0, d = 0;
            if ((x & 3) == 0 && (z & 3) == 0) {
                const int x0 = x, z0 = z;
                std::uint16_t maxM = 0, maxE = 0;
                for (int wz = std::max(0, z0 - 2); wz < std::min(cellsZ_, z0 + 6); ++wz) {
                    const bool blockRow = z0 <= wz && wz < z0 + 4;
                    const int fromX = blockRow ? std::max(0, x0 - 2) : x0, toX = blockRow ? x0 + 5 : x0 + 3;
                    for (int wx = fromX; wx <= toX; ++wx) {
                        maxM = std::max(maxM, cellValue(wx, wz, false));
                        maxE = std::max(maxE, cellValue(wx, wz, true));
                    }
                }
                const std::uint16_t v = std::uint16_t(maxM + maxE);
                commercialWork_->SetTractValue(x >> 2, z >> 2, std::uint8_t(std::min<std::uint32_t>(255, RoundHalfEven(float(v) * commercialScale))));
            }
            bool any = false;
            for (const NetworkInfo& info : grid_[std::size_t(x)][std::size_t(z)]) {
                const int n = info.networkType;
                if (n > 5) continue;
                any = true;
                float& vol = volume_[std::size_t(n)]->GetGridData()[std::size_t(x)][std::size_t(z)];
                if (n != 2 && n != 3) b = std::max(b, vol);
                const float r = vol * info.inverseCapacity;
                d = std::max(d, r);
                const std::size_t slot = (info.flag7 & 1) ? 13 : std::size_t(std::min<int>(info.pieceType, 12));
                TrafficStatsRow& row = statsWork_[slot];
                sum[slot] += r;
                ++count[slot];
                ++records[slot];
                if (r > statsCompare_[slot]) {
                    statsCompare_[slot] = r;
                    row.max = r;
                    row.maxX = x;
                    row.maxZ = z;
                }
                if (r > 1.0f) ++congested[slot];
                vol = vol * tunables_.value_bd4;
                if (n == 1) vol += GetBackgroundTraffic(x, z);
            }
            if (any) b = std::max(b, 1.0f);
            peakVolume_->SetTractValue(x, z, b);
            peakRatio_->SetTractValue(x, z, d);
        }
    // publish the statistics and replace the cell counts
    for (std::size_t s = 0; s < statsWork_.size(); ++s) {
        TrafficStatsRow& row = statsWork_[s];
        row.average = count[s] != 0 ? sum[s] / float(count[s]) : sum[s];
        row.congested = congested[s];
        row.records = records[s];
        statsPublished_[s] = row;
        if (s < 13) cellCounts_[s] = records[s];
    }
    counters_.c908 = cellCounts_[0]; counters_.c90c = cellCounts_[1]; counters_.c910 = cellCounts_[2]; counters_.c914 = cellCounts_[3];
    counters_.c920 = cellCounts_[6]; counters_.c924 = cellCounts_[7]; counters_.c928 = cellCounts_[8]; counters_.c92c = cellCounts_[9];
    counters_.c930 = cellCounts_[10]; counters_.c938 = cellCounts_[12];
    UpdateRoadsBudget();
    UpdateMassTransitBudget();
}

// §4.10
void cSC4TrafficSimulator::CreateFinalMap() {
    const float scale = tunables_.value_eb0;
    std::uint32_t congestedCells = 0;
    for (int x = 0; x < cellsX_; ++x)
        for (int z = 0; z < cellsZ_; ++z) {
            const float b = peakVolume_->GetTractValue(x, z);
            if (b == 0.0f) {
                airMap_->SetTractValue(x, z, 0);
                congestionMap_->SetTractValue(x, z, 0);
                continue;
            }
            airMap_->SetTractValue(x, z, std::uint8_t(std::min<std::uint32_t>(255, RoundHalfEven(b * 0.1f))));
            const float c = congestionToInverseSpeed_.GetOutputValue(peakRatio_->GetTractValue(x, z));
            std::uint8_t value = 1;
            if (c > 1.0f) {
                const std::uint32_t g = RoundHalfEven((c - 1.0f) * scale);
                value = g > 255 ? 255 : (g == 0 ? 1 : std::uint8_t(g));
                ++congestedCells;
            }
            congestionMap_->SetTractValue(x, z, value);
        }
    host_->PostTrafficMapChanged();
    if (Debug())
        std::fprintf(stderr, "traffic: maps published: road avg %.3f max %.3f at (%d,%d), %u congested cells; routed $%u/$$%u/$$$%u of %u/%u/%u, "
                     "car commute %.2f\n", statsPublished_[0].average, statsPublished_[0].max, statsPublished_[0].maxX, statsPublished_[0].maxZ,
                     congestedCells, routed_[0], routed_[1], routed_[2], attempted_[0], attempted_[1], attempted_[2],
                     carLenCount_ ? carLenSum_ / float(carLenCount_) : 0.0f);
    if (Debug()) {
        ScoreAgainstSaved();
        std::fprintf(stderr, "traffic: failed searches %u of %u\n", passCounters_[8], toSearch_);
    }
}

// SC4_TRAFFICDEBUG: our published congestion and trip-length maps against the ones the city file carried (the original's last cycle).
void cSC4TrafficSimulator::ScoreAgainstSaved() {
    auto score = [&](const char* name, cSC4SimGrid<std::uint8_t>* grid, const std::vector<std::uint8_t>& saved, std::uint8_t busy) {
        if (!grid || saved.empty()) return;
        std::vector<std::uint8_t> ours;
        for (auto& column : grid->GetGridData()) ours.insert(ours.end(), column.begin(), column.end());
        if (ours.size() != saved.size()) return;
        std::size_t savedSet = 0, oursSet = 0, both = 0, savedBusy = 0, oursBusy = 0, bothBusy = 0;
        double savedSum = 0, oursSum = 0, absDiff = 0;
        for (std::size_t i = 0; i < ours.size(); ++i) {
            const int a = saved[i], b = ours[i];
            savedSet += a != 0; oursSet += b != 0;
            savedBusy += a > busy; oursBusy += b > busy; bothBusy += a > busy && b > busy;
            if (a != 0 && b != 0) { ++both; savedSum += a; oursSum += b; absDiff += std::abs(a - b); }
        }
        std::fprintf(stderr, "traffic: score %s: cells original %zu ours %zu both %zu; mean original %.1f ours %.1f |diff| %.1f; above %d: original %zu ours %zu "
                     "both %zu\n", name, savedSet, oursSet, both, both ? savedSum / double(both) : 0.0, both ? oursSum / double(both) : 0.0,
                     both ? absDiff / double(both) : 0.0, busy, savedBusy, oursBusy, bothBusy);
    };
    score("congestion", congestionMap_, savedCongestion_, 1);
    score("trip length", tripLengthMap_, savedTripLength_, 0);
    if (std::getenv("SC4_TRAFFICDUMP")) {
        static int cycle = 0;
        char path[64];
        std::snprintf(path, sizeof path, "trips_ours%d.txt", ++cycle);
        DumpTrips(path);
        std::snprintf(path, sizeof path, "maps%d.txt", cycle);
        if (std::FILE* f = std::fopen(path, "w")) {   // x z saved-trip ours-trip saved-congestion ours-congestion
            std::vector<std::uint8_t> trip, cong;
            for (auto& column : tripLengthMap_->GetGridData()) trip.insert(trip.end(), column.begin(), column.end());
            for (auto& column : congestionMap_->GetGridData()) cong.insert(cong.end(), column.begin(), column.end());
            for (std::size_t i = 0; i < trip.size() && i < savedTripLength_.size() && i < cong.size() && i < savedCongestion_.size(); ++i)
                if (trip[i] || savedTripLength_[i] || cong[i] || savedCongestion_[i])
                    std::fprintf(f, "%zu %zu %d %d %d %d\n", i / std::size_t(cellsZ_), i % std::size_t(cellsZ_), savedTripLength_[i], trip[i], savedCongestion_[i], cong[i]);
            std::fclose(f);
        }
    }
}

void cSC4TrafficSimulator::DumpTrips(const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::vector<std::unique_ptr<ITrafficLot>> lots;
    host_->AllLots(lots);
    for (auto& lot : lots) {
        std::list<cSC4TripData>* trips = lot->TripList(lot->TrafficSource());
        if (!trips) continue;
        SC4RectInt32 r;
        lot->OccupiedRect(r);
        for (const cSC4TripData& t : *trips)
            std::fprintf(f, "%d %d %d %d %d type %u strategy %u flags %u status %u count %u dest %d %d len %.3f types %x route %zu back %zu\n", r.x0, r.z0, r.x1, r.z1,
                         lot->ZoneType(), t.type, t.index, t.flags, t.kind, t.count, t.sourceX, t.sourceZ, t.distance, t.extra, t.route.size(), t.routeReversed.size());
    }
    std::fclose(f);
}

void cSC4TrafficSimulator::ReduceTransitSwitches() {
    for (auto& [id, sw] : transitSwitches_) sw.traffic = tunables_.value_bd4 * sw.traffic;
}

void cSC4TrafficSimulator::DumpEdgeDensities() {
    for (auto& [key, record] : edgeDensities_) {
        record.last = record.current;
        std::uint32_t sum = 0;
        for (std::uint16_t v : record.current) sum += v;
        record.density = std::uint16_t(std::min<std::uint32_t>(0xffff, sum));
    }
}

void cSC4TrafficSimulator::ReduceEdgeDensities() {
    for (int x = 0; x < cellsX_; ++x)
        for (int z = 0; z < cellsZ_; ++z) {
            if (grid_[std::size_t(x)][std::size_t(z)].empty()) continue;
            for (int t = 0; t < 9; ++t)
                for (bool evening : {false, true}) {
                    const auto it = edgeDensities_.find(EdgeDensityKey(x, z, t, evening));
                    if (it != edgeDensities_.end()) it->second.current.fill(0);
                }
        }
}

// §4.12
void cSC4TrafficSimulator::FinalCalculations() {
    transitIncome_ = float(incomeAcc_);
    tollIncome_ = float(feeAcc_);
    avgCarCommute_ = carLenCount_ ? carLenSum_ / float(carLenCount_) : 0.0f;   // the original divides by zero (NaN)
    freightByte_ = freightCount_ ? std::uint8_t(std::int64_t(freightLenSum_ / float(freightCount_) / tunables_.tripScale * 255.0f)) : 0;
}

// §4.12: funding -> potholes, strikes, history.
void cSC4TrafficSimulator::DoMonthlySimulation() {
    if (!initialized_) return;
    SC4Percentage roads, transit;
    roads.value = host_->RoadFundingFraction();
    transit.value = host_->MassTransitFundingFraction();
    ApplyRoadFunding(roads);
    ApplyMassTransitFunding(transit);
    UpdateStrikes();
    host_->RecordHistory(0xea5ba01d, std::uint32_t(std::int64_t(statsPublished_[0].average * 100.0f)));
    host_->RecordHistory(0x8a5d4116, std::uint32_t(std::int64_t(avgCarCommute_ * 25.0f)));
    for (std::uint32_t i = 0; i < 9; ++i) host_->RecordHistory(0xabb8faac + i, typeCountsPublished_[i]);
    host_->RecordHistory(0xec0bdbf8, 0);
}

// ---- routes (§4.3, §4.5) --------------------------------------------------------------------------------------------------

bool cSC4TrafficSimulator::ConfirmRoute(const std::vector<std::uint8_t>& route) const {
    std::vector<RouteRecord> records;
    if (!ReadRoute(route, records) || records.empty()) return false;
    const bool strike = strikeState_ == kStrikeOn;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const RouteRecord& r = records[i];
        if (!(unsigned(r.x) < unsigned(cellsX_) && unsigned(r.z) < unsigned(cellsZ_)) || r.exit < 0) continue;
        if (r.type > 8 || r.nextType > 8) return false;
        const TrafficCellConnections& cell = CellConnections(r.x, r.z);
        if (i == 0) {
            if (r.type == r.nextType) {
                bool any = false;
                for (int e = 0; e < 4; ++e) any = any || ((cell.masks[std::size_t(r.type)] >> (r.exit + 4 * e)) & 1);
                if (!any) return false;
            } else if (((cell.switches[std::size_t(r.type)] >> (4 * r.exit)) & 0xf) != std::uint16_t(r.nextType)) {
                return false;
            }
            continue;
        }
        if (r.type == r.nextType) {
            if (r.entry >= 0 && ((cell.masks[std::size_t(r.type)] >> (r.exit + 4 * r.entry)) & 1)) continue;
            if (cell.wormhole & 2) continue;
            if ((cell.wormhole & 4) && !strike) continue;
            return false;
        }
        if (strike) return false;
        if (((cell.switches[std::size_t(r.type)] >> (4 * r.exit)) & 0xf) != std::uint16_t(r.nextType)) return false;
    }
    return true;
}

void cSC4TrafficSimulator::ApplyRoute(const std::vector<std::uint8_t>& route, std::uint32_t count, const SC4RectInt32& sourceRect, int destX,
                                      int destZ, int mode, std::uint8_t type) {
    std::vector<RouteRecord> records;
    if (!ReadRoute(route, records) || records.empty()) return;
    const float n = float(count);
    const float nCar = host_->OrdinanceOn(kOrdinanceCarpool) ? float(std::ceil(double(n * 0.8f))) : n;
    SC4RectInt32 r = sourceRect;
    if (mode == 1) {
        r = {destX, destZ, destX, destZ};
        if (unsigned(destX) < unsigned(cellsX_) && unsigned(destZ) < unsigned(cellsZ_))
            if (ITrafficLot* lot = host_->LotAt(destX, destZ, 0)) lot->OccupiedRect(r);
    }
    float income = 0;
    const float fees = 0;   // only transit switches (toll booths) charge: none natively
    int lastType = records.back().type;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const RouteRecord& rec = records[i];
        const int t = rec.type;
        if (t > 8) continue;
        const float q = t == 1 ? nCar : n;
        const std::uint32_t qi = RoundHalfEven(q);
        if (unsigned(rec.x) < unsigned(cellsX_) && unsigned(rec.z) < unsigned(cellsZ_)) {
            // 1. volume
            if (GetNetworkInfo(kNetworkOfType[t], rec.x, rec.z) && tunables_.travelTypeEnabled[std::size_t(t)]) {
                float& vol = volume_[std::size_t(kNetworkOfType[t])]->GetGridData()[std::size_t(rec.x)][std::size_t(rec.z)];
                vol += q;
                if (mode == 2) vol += q;
            }
            // 3. edge density
            int e = rec.entry, o = rec.exit;
            if (i == 0) e = rec.x < r.x0 ? 2 : rec.x > r.x1 ? 0 : rec.z < r.z0 ? 3 : 1;
            const bool last = i + 1 == records.size();
            if (last) {
                if (mode == 1) {
                    const SC4RectInt32& s = sourceRect;
                    o = rec.x < s.x0 ? 2 : rec.x > s.x1 ? 0 : rec.z < s.z0 ? 3 : 1;
                } else {
                    const int dx = std::int16_t(destX - rec.x), dz = std::int16_t(destZ - rec.z);
                    o = -1;
                    if (std::abs(dx) + std::abs(dz) == 1) o = dx == -1 ? 0 : dz == -1 ? 1 : dx == 1 ? 2 : 3;
                }
            }
            if (e >= 0 && o >= 0) {
                auto add = [&](bool evening, int index) {
                    std::uint16_t& v = EdgeDensityEntry(rec.x, rec.z, t, evening).current[std::size_t(index)];
                    v = std::uint16_t(std::min<std::uint32_t>(0xffff, v + qi));
                };
                add(mode == 1, 4 * e + o);
                if (mode == 2) add(true, 4 * o + e);
            } else if (!last) {
                return;   // quirk: an undefined side mid-route ends ApplyRoute (income lost)
            }
        }
        // 5. income
        income += q * tunables_.values_dfc[std::size_t(t)];
    }
    if (mode != 1) ApplyTrafficToDestination(count, destX, destZ, std::uint8_t(lastType), type);
    incomeAcc_ = double(float(incomeAcc_ + double(income)));
    feeAcc_ = double(float(feeAcc_ + double(fees)));
}

}  // namespace sc4
