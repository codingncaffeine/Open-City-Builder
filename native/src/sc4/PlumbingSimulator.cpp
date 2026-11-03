#include "sc4/PlumbingSimulator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gz/Bit1.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborDeal.h"
#include "sc4/Occupant.h"
#include "sc4/SpiralIterator.h"

namespace sc4 {

namespace {
constexpr std::uint8_t kFlagWest = 1, kFlagNorth = 2, kFlagEast = 4, kFlagSouth = 8, kFlagPipe = 0x10, kFlagProducer = 0x20,
                       kFlagBuilding = 0x40, kKeepOnPipeRemoval = 0xe0;
constexpr std::uint32_t kPipeDistressed = 0x10000, kPipeBurst = 0x20000;
constexpr std::uint32_t kGridWorkingInstance = 0x49d5bb8c, kGridPublishedInstance = 0x49d5bba1;
constexpr std::uint32_t kBuildingOccupantIid = 0x87dda3a9;   // cISC4BuildingOccupant
constexpr std::uint32_t kNeighborDealIid = 0x0773fef1;

// the original's float -> unsigned conversion (negative and NaN go to 0, large values saturate)
inline std::uint32_t ToUnsigned(float v) { return v > 0.0f ? (v >= 4294967296.0f ? 0xffffffffu : std::uint32_t(v)) : 0u; }

void LoadCurve(cSC4ResponseCurve& curve, const cISCPropertyHolder* holder, std::uint32_t id) {
    std::vector<float> values;
    if (holder->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(holder, id, values)) curve.SetResponseCurveData(values.data(), values.size());
}
bool CurveIsEmpty(const cSC4ResponseCurve& curve) {
    float max = 0;
    return !curve.GetMaxInputValue(max);
}
bool IsBuildingOccupant(cISC4Occupant* occupant) {
    void* raw = nullptr;
    if (!occupant->QueryInterface(kBuildingOccupantIid, &raw) || !raw) return false;
    static_cast<cIGZUnknown*>(raw)->Release();
    return true;
}
}  // namespace

cSC4PlumbingSimulator::cSC4PlumbingSimulator(IPlumbingHost* host, cRZRandom* random) : host_(host), random_(random) {}

cSC4PlumbingSimulator::~cSC4PlumbingSimulator() {
    for (WaterProducer& producer : producers_) if (producer.occupant) producer.occupant->Release();
    for (cISC4Occupant* pipe : pipes_) if (pipe) pipe->Release();
    if (working_) working_->Release();
    if (published_) published_->Release();
}

// ---- identity -------------------------------------------------------------------------------------------------

bool cSC4PlumbingSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4PlumbingSimulator) *out = static_cast<cISC4PlumbingSimulator*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4PlumbingSimulator::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

// ---- life cycle -------------------------------------------------------------------------------------------------

cSC4SimGrid<std::uint8_t>* cSC4PlumbingSimulator::MakeGrid(std::uint32_t instance) {
    auto* grid = new cSC4SimGrid<std::uint8_t>();
    grid->AddRef();
    grid->Init();
    grid->SetTractSize(1);
    grid->SetInstanceID(instance);
    grid->SetTractValues(0);
    return grid;
}

// Phase one (once): tunables, the three agents (6, 4 and 10 under "cSC4PlumbingSimulator"), the notifications and the water
// department (the host creates it when the city has none). Phase two (a city file brings its own maps): the two watered grids, the
// cell flag map, the visit bitmap and the scan rectangle, and the first pass is requested.
bool cSC4PlumbingSimulator::Init() {
    if (!initialised_) {
        initialised_ = true;
        ReadSimulatorTunableValues(host_->SimulatorExemplar());
        cIGZMessageTarget2* self = this;
        host_->AddAgent(self, 6, "cSC4PlumbingSimulator", 2);
        host_->AddAgent(self, 4, "cSC4PlumbingSimulator", 2);
        host_->AddAgent(self, 10, "cSC4PlumbingSimulator", 2);
        for (std::uint32_t message : {kMsgPlumbingOccupantInserted, kMsgPlumbingOccupantRemoved, kMsgPlumbingTunablesChanged,
                                      kMsgPlumbingPipeBurst, kMsgPlumbingDealsChanged, kMsgPlumbingCityEstablished})
            host_->AddNotification(self, message);
        host_->EnsureDepartment(maxFunding.value);
    }
    if (!gridsInitialised_) {
        gridsInitialised_ = true;
        if (!working_) working_ = MakeGrid(kGridWorkingInstance);
        if (!published_) published_ = MakeGrid(kGridPublishedInstance);
        cellsX_ = host_->CityCellsX();
        cellsZ_ = host_->CityCellsZ();
        visitedSize_ = std::max(cellsX_, cellsZ_);
        cellFlags_.assign(std::size_t(cellsX_) * std::size_t(cellsZ_), 0);
        visited_.assign(std::size_t(visitedSize_) * std::size_t(visitedSize_) / 32 + 1, 0);
        ResetScan();
        state_ = 0;
        refresh_ = true;
    }
    if (visited_.empty()) visited_.assign(std::size_t(visitedSize_) * std::size_t(visitedSize_) / 32 + 1, 0);
    return true;
}

bool cSC4PlumbingSimulator::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    cIGZMessageTarget2* self = this;
    for (std::uint32_t message : {kMsgPlumbingOccupantInserted, kMsgPlumbingOccupantRemoved, kMsgPlumbingTunablesChanged,
                                  kMsgPlumbingPipeBurst, kMsgPlumbingDealsChanged, kMsgPlumbingCityEstablished})
        host_->RemoveNotification(self, message);
    host_->RemoveAgent(self);
    if (working_) { working_->Release(); working_ = nullptr; }
    if (published_) { published_->Release(); published_ = nullptr; }
    cellFlags_.clear();
    visited_.clear();
    for (WaterProducer& producer : producers_) if (producer.occupant) producer.occupant->Release();
    producers_.clear();
    current_ = nullptr;
    for (cISC4Occupant* pipe : pipes_) if (pipe) pipe->Release();
    pipes_.clear();
    pipeListCount_ = distressedPipes_ = burstPipes_ = pollutedSources_ = 0;
    gridsInitialised_ = false;
    enabled_ = false;
    return true;
}

void cSC4PlumbingSimulator::ResetScan() {
    scan_ = ScanningIterator{};
    scan_.remaining = 1;
    scan_.rect.left = 0;
    scan_.rect.top = 0;
    scan_.rect.right = cellsX_ - 1;
    scan_.rect.bottom = cellsZ_ - 1;
}

// ---- tunables -----------------------------------------------------------------------------------------------------

void cSC4PlumbingSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    const std::vector<SC4Point> flat{{0.0f, 1.0f}, {1.0f, 1.0f}};
    wearByFundingCurve.SetResponseCurveData(flat);
    wearByUseCurve.SetResponseCurveData(flat);
    pipeFundingCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, -0.02f}, {120.0f, 0.01f}});
    if (!exemplar) return;
    using H = cSCPropertyHelp;
    H::GetPropertyValue(exemplar, 0x68f5436a, serviceRadius);
    H::GetPropertyValue(exemplar, 0x68f5436b, groundSearchRadius);
    H::GetPropertyValue(exemplar, 0x68f5436c, surfaceSearchRadius);
    H::GetPropertyValue(exemplar, 0x68f5436d, farGroundPercent);
    H::GetPropertyValue(exemplar, 0xaa161f81, pollutionThreshold);
    H::GetPropertyValue(exemplar, 0x8a235c56, burstVariance);
    H::GetPropertyValue(exemplar, 0xea4025a9, maxFunding);
    H::GetPropertyValue(exemplar, 0x8a84778f, pipeCostFactor);
    const std::vector<SC4Point> identity{{0.0f, 0.0f}, {100.0f, 100.0f}};
    LoadCurve(productionFundingCurve, exemplar, 0x6a410bf2);
    if (CurveIsEmpty(productionFundingCurve)) productionFundingCurve.SetResponseCurveData(identity);
    LoadCurve(wearByFundingCurve, exemplar, 0x0911e2e8);
    if (CurveIsEmpty(wearByFundingCurve)) wearByFundingCurve.SetResponseCurveData(identity);
    LoadCurve(wearByUseCurve, exemplar, 0x0911e2e9);
    if (CurveIsEmpty(wearByUseCurve)) wearByUseCurve.SetResponseCurveData(identity);
    LoadCurve(pipeFundingCurve, exemplar, 0xca7138c4);
}

// ---- messages -----------------------------------------------------------------------------------------------------

bool cSC4PlumbingSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    switch (type) {
    case kMsgPlumbingOccupantInserted:
    case kMsgPlumbingOccupantRemoved: {
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
        if (!occupant || (holder && holder->HasProperty(0x029244db))) break;   // marked: not part of the network
        if (type == kMsgPlumbingOccupantInserted) DoOccupantInserted(occupant);
        else DoOccupantRemoved(occupant);
        break;
    }
    case kMsgPlumbingMonth: DoMonthlySimulation(); break;
    case kMsgPlumbingTick: return DoIdle(5);
    case kMsgPlumbingTickFast: return DoIdle(std::uint32_t(standard->GetData3()));
    case kMsgPlumbingCityEstablished: enabled_ = true; break;
    case kMsgPlumbingTunablesChanged: ReadSimulatorTunableValues(host_->SimulatorExemplar()); break;
    case kMsgPlumbingPipeBurst:
    case kMsgPlumbingDealsChanged: {
        if (type == kMsgPlumbingPipeBurst) {   // the burst cell loses its pipe bits until the pipe is rebuilt
            const int x = int(standard->GetData1()), z = int(standard->GetData2());
            if (x >= 0 && z >= 0 && x < cellsX_ && z < cellsZ_) CellFlag(x, z) &= kKeepOnPipeRemoval;
            refresh_ = true;
        }
        if (standard->GetData1() != 0) break;
        cIGZUnknown* object = standard->GetIGZUnknown();
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kNeighborDealIid, &raw) || !raw) break;
        auto* deal = static_cast<cISC4NeighborDeal*>(raw);
        if (deal->GetType() == kWaterDealExport || deal->GetType() == kWaterDealImport) {   // a water deal changed: a new pass at once
            state_ = 9;
            refresh_ = true;
        }
        static_cast<cIGZUnknown*>(raw)->Release();
        break;
    }
    default: break;
    }
    return true;
}

// The month: a new pass and producer ageing are due, and the department's funding sets the pipes' condition.
void cSC4PlumbingSimulator::DoMonthlySimulation() {
    refresh_ = true;
    ageProducers_ = true;
    ApplyPipeFunding(SC4Percentage(host_->DepartmentFundingFraction()));
}

void cSC4PlumbingSimulator::DoOccupantInserted(cISC4Occupant* occupant) {
    if (!gridsInitialised_ || !occupant) return;
    const std::uint32_t type = occupant->GetType();
    if (type == kPlumbingOccupantBuilding) DoBuildingChanged(occupant, true);
    else if (type == kPlumbingOccupantNetwork && host_->IsPipe(occupant)) DoPipeChanged(occupant, true);
}

void cSC4PlumbingSimulator::DoOccupantRemoved(cISC4Occupant* occupant) {
    if (!gridsInitialised_ || !occupant) return;
    const std::uint32_t type = occupant->GetType();
    if (type == kPlumbingOccupantBuilding) DoBuildingChanged(occupant, false);
    else if (type == kPlumbingOccupantNetwork && host_->IsPipe(occupant)) DoPipeChanged(occupant, false);
}

// A pipe sets (or clears) the pipe bit and its four connection bits over its cells, joins (or leaves) the pipe list and the pipe
// infrastructure line item follows the count.
void cSC4PlumbingSimulator::DoPipeChanged(cISC4Occupant* pipe, bool added) {
    SC4CellRect cells;
    pipe->GetBoundingCityCells(cells);
    for (int x = std::max(0, cells.x0); x <= cells.x1 && x < cellsX_; ++x)
        for (int z = std::max(0, cells.z0); z <= cells.z1 && z < cellsZ_; ++z) {
            std::uint8_t& flags = CellFlag(x, z);
            if (!added) {
                flags &= kKeepOnPipeRemoval;
                continue;
            }
            flags |= kFlagPipe;
            if (host_->PipeConnects(pipe, 2)) flags |= kFlagEast;
            if (host_->PipeConnects(pipe, 1)) flags |= kFlagNorth;
            if (host_->PipeConnects(pipe, 0)) flags |= kFlagWest;
            if (host_->PipeConnects(pipe, 3)) flags |= kFlagSouth;
        }
    auto it = std::find(pipes_.begin(), pipes_.end(), pipe);
    if (added) {
        if (it == pipes_.end()) {
            pipe->AddRef();
            pipes_.push_back(pipe);
            ++pipeListCount_;
        }
    } else if (it != pipes_.end()) {
        const std::uint32_t flags = host_->PipeFlags(pipe);
        if (flags & kPipeDistressed) --distressedPipes_;
        if (flags & kPipeBurst) --burstPipes_;
        pipes_.erase(it);
        pipe->Release();
        --pipeListCount_;
    }
    host_->SetPipeCost(std::int64_t(std::nearbyintf(float(pipeListCount_) * pipeCostFactor)));
    refresh_ = true;
}

// A building with a water demand marks its cell (0x40) and adds to the demand; a building with a budget item books it; a water
// producer (0x88ed3303) joins the producer list at the front with its lot's rectangle (0x20 over it) and starts unworn.
void cSC4PlumbingSimulator::DoBuildingChanged(cISC4Occupant* building, bool added) {
    float position[3] = {};
    building->GetPosition(position);
    int x = 0, z = 0;
    host_->PositionToCell(position[0], position[2], x, z);
    if (x < 0 || z < 0 || x >= cellsX_ || z >= cellsZ_) return;
    cISCPropertyHolder* holder = building->AsPropertyHolder();
    std::uint8_t& flags = CellFlag(x, z);
    std::uint32_t demand = 0;
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropBuildingWaterDemand, demand)) {
        if (added) {
            flags |= kFlagBuilding;
            demand_ += std::int32_t(demand);
        } else {
            flags &= std::uint8_t(~kFlagBuilding);
            demand_ -= std::int32_t(demand);
        }
    }
    if (!holder || !IsBuildingOccupant(building)) return;
    host_->ApplyBuildingBudget(building, added);
    if (!holder->HasProperty(kPropWaterProductionCapacity)) return;
    if (added) {
        PlumbingRect rect;
        if (!host_->LotRectAt(x, z, rect)) return;   // (the original takes the building's reference before this test and keeps it)
        WaterProducer producer;
        producer.x = x;
        producer.z = z;
        producer.rect = rect;
        cSCPropertyHelp::GetPropertyValue(holder, kPropWaterProductionCapacity, producer.capacity);
        producer.occupant = building;
        building->AddRef();
        producers_.push_front(producer);
        for (int cx = rect.left; cx <= rect.right; ++cx)
            for (int cz = rect.top; cz <= rect.bottom; ++cz)
                if (cx >= 0 && cz >= 0 && cx < cellsX_ && cz < cellsZ_) CellFlag(cx, cz) |= kFlagProducer;
        cSCPropertyHelp::SetPropertyValue(holder, kPropProducerWear, 0.0f);
    } else {
        auto it = std::find_if(producers_.begin(), producers_.end(), [&](const WaterProducer& p) { return p.occupant == building; });
        if (it != producers_.end()) {
            for (int cx = it->rect.left; cx <= it->rect.right; ++cx)
                for (int cz = it->rect.top; cz <= it->rect.bottom; ++cz)
                    if (cx >= 0 && cz >= 0 && cx < cellsX_ && cz < cellsZ_) CellFlag(cx, cz) &= 0xdf;
            if (current_ == &*it) current_ = nullptr;
            producers_.erase(it);
            building->Release();
        }
    }
    refresh_ = true;
}

// ---- the pass -------------------------------------------------------------------------------------------------

bool cSC4PlumbingSimulator::Visited(int x, int z) const {
    return !visited_.empty() && Bit1_PixelTest(visited_.data(), visitedSize_, x, z);
}
void cSC4PlumbingSimulator::Visit(int x, int z) {
    if (!visited_.empty()) Bit1_PixelSet(visited_.data(), visitedSize_, x, z);
}

WaterProducer* cSC4PlumbingSimulator::FindWaterProducer(int x, int z) {
    for (WaterProducer& producer : producers_)
        if (producer.x == x && producer.z == z) return &producer;
    return nullptr;
}

void cSC4PlumbingSimulator::Seed(int x, int z, std::int64_t supply) {
    pipeQueue_.clear();
    serviceQueue_.clear();
    if (!visited_.empty()) Bit1_Clear(visited_.data(), visitedSize_);
    const QueuePoint seed{std::uint8_t(x), std::uint8_t(z)};
    pipeQueue_.push_back(seed);
    serviceQueue_.push_back(seed);
    Visit(x, z);
    supply_ = supply;
    consumed_ = 0;
}

// State 3's walk: from the queued cells along their connection bits to unvisited pipe cells (each also queued for service); the
// producers whose cell the walk reaches add their output to the network's supply, once per pass.
void cSC4PlumbingSimulator::FloodPipes() {
    while (!pipeQueue_.empty()) {
        const auto [ux, uz] = pipeQueue_.front();
        pipeQueue_.pop_front();
        const int x = ux, z = uz;
        const std::uint8_t flags = CellFlag(x, z);
        if (flags & kFlagProducer) {
            if (WaterProducer* producer = FindWaterProducer(x, z); producer && !producer->counted) {
                supply_ += producer->production;
                producer->counted = true;
            }
        }
        const auto follow = [&](int nx, int nz) {
            if (!(CellFlag(nx, nz) & kFlagPipe) || Visited(nx, nz)) return;
            Visit(nx, nz);
            const QueuePoint next{std::uint8_t(nx), std::uint8_t(nz)};
            pipeQueue_.push_back(next);
            serviceQueue_.push_back(next);
        };
        if ((flags & kFlagWest) && x != 0) follow(x - 1, z);
        if ((flags & kFlagEast) && x < cellsX_ - 1) follow(x + 1, z);
        if ((flags & kFlagNorth) && z != 0) follow(x, z - 1);
        if ((flags & kFlagSouth) && z < cellsZ_ - 1) follow(x, z + 1);
    }
}

// State 4: one network. Its supply less the seed deal is production; exports to connected neighbours come off first; then the cells
// are watered ring by ring outwards from the pipes (diagonals in the inner half) until the supply runs out or the service radius
// is reached; the import deals cover what the network used beyond its own supply, and each producer of the network gets its share.
void cSC4PlumbingSimulator::ServeNetwork() {
    const std::int64_t base = supply_ - imported_;
    pass_.production += base;
    std::int64_t sold = 0;
    std::vector<cISC4NeighborDeal*> exports;
    host_->Deals(kWaterDealExport, exports);
    for (cISC4NeighborDeal* deal : exports) {
        const std::int64_t open = std::int64_t(deal->GetAmount()) - deal->GetAmountUsed();
        const std::int64_t amount = open < supply_ ? open : supply_;
        if (amount <= 0) continue;
        if (!IsNeighborConnectedToCurrentNetwork(host_->DealCity(deal), false)) {
            deal->SetFlags(8, true);
            continue;
        }
        pass_.exported += amount;
        supply_ -= amount;
        deal->SetAmountUsed(deal->GetAmountUsed() + int(amount));
        sold += amount;
    }
    if (supply_ > 0 || sold > 0) {
        auto& grid = working_->GetGridData();
        while (!serviceQueue_.empty()) {
            if (ringRemaining_ < 1) {
                ringRemaining_ = int(serviceQueue_.size());
                if (serviceRadius <= ++ring_) break;
            }
            --ringRemaining_;
            const auto [ux, uz] = serviceQueue_.front();
            serviceQueue_.pop_front();
            const int x = ux, z = uz;
            if (CellFlag(x, z) & kFlagBuilding) {
                consumed_ += std::int32_t(ToUnsigned(float(BuildingConsumption(x, z)) * waterFactor_));
                if (consumed_ > supply_) {
                    consumed_ = supply_;
                    break;
                }
            }
            grid[std::size_t(x)][std::size_t(z)] |= 1;
            const auto spread = [&](int nx, int nz) {
                if (Visited(nx, nz)) return;
                Visit(nx, nz);
                serviceQueue_.push_back(QueuePoint{std::uint8_t(nx), std::uint8_t(nz)});
            };
            const bool west = x != 0, east = x < cellsX_ - 1, north = z != 0, south = z < cellsZ_ - 1;
            if (west) spread(x - 1, z);
            if (east) spread(x + 1, z);
            if (north) spread(x, z - 1);
            if (south) spread(x, z + 1);
            if (ring_ < serviceRadius / 2) {
                if (west && north) spread(x - 1, z - 1);
                if (east && north) spread(x + 1, z - 1);
                if (west && south) spread(x - 1, z + 1);
                if (east && south) spread(x + 1, z + 1);
            }
        }
    }
    pass_.consumed += consumed_;
    consumed_ += sold;
    if (imported_ > 0) {
        std::vector<cISC4NeighborDeal*> imports;
        host_->Deals(kWaterDealImport, imports);
        for (cISC4NeighborDeal* deal : imports) {
            const std::int64_t open = std::int64_t(deal->GetAmount()) - deal->GetAmountUsed();
            const std::int64_t amount = open <= consumed_ ? open : consumed_;
            if (amount <= 0 || !IsNeighborConnectedToCurrentNetwork(host_->DealCity(deal), false)) continue;
            pass_.imported += amount;
            consumed_ -= amount;
            deal->SetAmountUsed(deal->GetAmountUsed() + int(amount));
        }
    }
    const float share = base > 0 ? float(double(consumed_) / double(base)) : 0.0f;
    for (WaterProducer& producer : producers_) {
        if (!producer.counted || producer.shareWritten || !producer.occupant) continue;
        cSCPropertyHelp::SetPropertyValue(producer.occupant->AsPropertyHolder(), kPropProducerCurrentProduced,
                                          ToUnsigned(std::max(0.0f, float(producer.production) * share)));
        producer.shareWritten = true;
    }
    serviceQueue_.clear();
    current_ = nullptr;
}

// @ i386 0x0007208a: one state per call while the city is established. 0 opens a pass (the deals' use, the connections' visits and
// the pass counters reset), 1 updates every producer's output, 2 clears the working grid to the pipe cells, 3 picks the next network
// seed (an import deal's live connection, else a producer not reached yet) and walks its pipes, 4 serves that network (back to 3),
// 5 tells the pipes and connections whether they carry water, 6 marks the buildings and producers on the working grid and publishes
// it, 7 finds the changed cells, 8 books costs, ages the producers once a month and records the statistics, 9 waits for a refresh.
//   The original slices states 6 and 7 by time (rdtsc); the port runs each whole.
//   PARTIAL: state 7's changed rectangle goes to the terrain view's refresh (city vslot 0x150, vslot 0x110), not wired natively.
bool cSC4PlumbingSimulator::DoIdle(std::uint32_t) {
    if (!enabled_ || !working_ || !published_) return true;
    if (captureLoaded_) {   // SC4_WATERDEBUG scores the first pass against the grid the city file brought
        captureLoaded_ = false;
        loadedWatered_.clear();
        if (published_->TractIsInBounds(cellsX_ - 1, cellsZ_ - 1)) {
            loadedWatered_.resize(std::size_t(cellsX_) * std::size_t(cellsZ_));
            for (int x = 0; x < cellsX_; ++x)
                for (int z = 0; z < cellsZ_; ++z)
                    loadedWatered_[std::size_t(x) * std::size_t(cellsZ_) + std::size_t(z)] = published_->GetTractValue(x, z);
        }
    }
    if (state_ == 9 && refresh_) {
        refresh_ = false;
        state_ = 0;
    }
    switch (state_) {
    case 0: {
        ++month_;
        waterFactor_ = host_->OrdinanceWaterFactor();
        for (std::uint32_t type : {kWaterDealImport, kWaterDealExport}) {
            std::vector<cISC4NeighborDeal*> deals;
            host_->Deals(type, deals);
            for (cISC4NeighborDeal* deal : deals) deal->SetAmountUsed(0);
        }
        std::vector<cISC4NeighborConnection*> connections;
        host_->Connections(nullptr, connections);
        for (cISC4NeighborConnection* connection : connections) connection->SetVisitedThisCycle(false);
        pass_ = Totals{};
        buildings_ = watered_ = 0;
        state_ = 1;
        break;
    }
    case 1:
        UpdateWaterProduction();
        state_ = 2;
        break;
    case 2: {
        auto& grid = working_->GetGridData();
        for (int z = 0; z < cellsZ_; ++z)
            for (int x = 0; x < cellsX_; ++x) grid[std::size_t(x)][std::size_t(z)] = (CellFlag(x, z) & kFlagPipe) ? 8 : 0;
        for (WaterProducer& producer : producers_) producer.counted = producer.shareWritten = false;
        current_ = nullptr;
        state_ = 3;
        break;
    }
    case 3: {
        imported_ = 0;
        if (!current_) {
            bool seeded = false;
            std::vector<cISC4NeighborDeal*> imports;
            host_->Deals(kWaterDealImport, imports);
            for (cISC4NeighborDeal* deal : imports) {
                if (deal->GetAmountUsed() >= deal->GetAmount()) continue;
                cISC4NeighborConnection* connection = host_->UnvisitedConnection(host_->DealCity(deal));
                if (!connection) continue;
                connection->SetVisitedThisCycle(true);
                if (!host_->OtherSideActive(connection)) continue;
                int x = 0, z = 0;
                GetConnectionCell(connection, x, z);
                imported_ = std::int64_t(deal->GetAmount()) - deal->GetAmountUsed();
                Seed(x, z, imported_);
                seeded = true;
                break;
            }
            if (!seeded)
                for (WaterProducer& producer : producers_) {
                    if (producer.counted) continue;
                    producer.counted = true;
                    current_ = &producer;
                    Seed(producer.x, producer.z, producer.production);
                    seeded = true;
                    break;
                }
            if (!seeded) {
                ResetScan();
                state_ = 5;
                break;
            }
        }
        FloodPipes();
        state_ = 4;
        ringRemaining_ = int(serviceQueue_.size());
        ring_ = 0;
        break;
    }
    case 4:
        ServeNetwork();
        state_ = 3;
        break;
    case 5: {
        auto& grid = working_->GetGridData();
        pipeCount_ = 0;
        value13c_ = 0;
        for (cISC4Occupant* pipe : pipes_) {
            int x = -1, z = -1;
            if (pipe) host_->PipeCell(pipe, x, z);
            const bool inside = x >= 0 && z >= 0 && x < cellsX_ && z < cellsZ_;
            host_->SetPipeWatered(pipe, inside && (grid[std::size_t(x)][std::size_t(z)] & 1));
            ++pipeCount_;
        }
        std::vector<cISC4NeighborConnection*> connections;
        host_->Connections(nullptr, connections);
        for (cISC4NeighborConnection* connection : connections) {
            int x = 0, z = 0;
            GetConnectionCell(connection, x, z);
            connection->SetIsActive((grid[std::size_t(x)][std::size_t(z)] & 1) != 0);
        }
        state_ = 6;
        [[fallthrough]];
    }
    case 6: {
        auto& grid = working_->GetGridData();
        std::vector<cISC4Occupant*> buildings;
        for (int z = 0; z < cellsZ_; ++z)
            for (int x = 0; x < cellsX_; ++x) {
                const std::uint8_t flags = CellFlag(x, z);
                if (!(flags & (kFlagProducer | kFlagBuilding))) continue;
                const std::uint8_t watered = grid[std::size_t(x)][std::size_t(z)] & 1;
                std::uint8_t value = watered | 6;
                if (flags & kFlagBuilding) {
                    value = watered | 2;
                    ++buildings_;
                    if (watered) ++watered_;
                }
                buildings.clear();
                host_->OccupantsAtCell(x, z, kPlumbingOccupantBuilding, buildings);
                for (cISC4Occupant* building : buildings) FillGridAtOccupant(*working_, building, value);
            }
        ResetScan();
        std::swap(working_, published_);
        state_ = 7;
        return true;
    }
    case 7: {
        if (static const bool trace = std::getenv("SC4_WATERDEBUG") != nullptr; trace) {
            static int passes = 0;
            if (passes == 0 && !loadedWatered_.empty()) {
                int both = 0, onlyFile = 0, onlyOurs = 0;
                const auto& now = published_->GetGridData();
                for (int x = 0; x < cellsX_; ++x)
                    for (int z = 0; z < cellsZ_; ++z) {
                        const bool saved = loadedWatered_[std::size_t(x) * std::size_t(cellsZ_) + std::size_t(z)] & 1;
                        const bool ours = now[std::size_t(x)][std::size_t(z)] & 1;
                        both += saved && ours;
                        onlyFile += saved && !ours;
                        onlyOurs += !saved && ours;
                    }
                std::fprintf(stderr, "water: first pass vs the file: %d watered in both, %d only in the file, %d only ours\n", both, onlyFile,
                             onlyOurs);
            }
            if (passes++ < 4) {
                std::vector<cISC4NeighborDeal*> in, out;
                host_->Deals(kWaterDealImport, in);
                host_->Deals(kWaterDealExport, out);
                std::fprintf(stderr, "water %p: pass %d production %lld consumed %lld imported %lld exported %lld; buildings %d watered %d; "
                             "producers %zu pipes %d; deals in %zu out %zu\n",
                             static_cast<const void*>(this), passes, static_cast<long long>(pass_.production), static_cast<long long>(pass_.consumed),
                             static_cast<long long>(pass_.imported), static_cast<long long>(pass_.exported), buildings_, watered_, producers_.size(),
                             pipeCount_, in.size(), out.size());
            }
        }
        state_ = 8;
        return true;
    }
    case 8: {
        std::int64_t cost = 0;
        for (WaterProducer& producer : producers_) {
            if (!producer.occupant) continue;
            cISCPropertyHolder* holder = producer.occupant->AsPropertyHolder();
            float costPerUnit = 0.0f, wear = 0.0f;
            std::int32_t current = 0;
            std::int64_t expense = 0;
            cSCPropertyHelp::GetPropertyValue(holder, kPropProducerCostPerUnit, costPerUnit);
            cSCPropertyHelp::GetPropertyValue(holder, kPropProducerCurrentProduced, current);
            cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear);
            cSCPropertyHelp::GetPropertyValue(holder, kPropBuildingBudgetCost, expense);
            if (holder->HasProperty(kPropProducerExpenseByWear)) {
                cSC4ResponseCurve curve;
                LoadCurve(curve, holder, kPropProducerExpenseByWear);
                host_->SetProducerExpense(producer.occupant, std::int64_t(float(expense) * curve.GetOutputValue(wear)));
            }
            cost += std::int64_t(float(current) * costPerUnit);
        }
        lastBuildings_ = buildings_;
        lastWatered_ = watered_;
        waterCost_ = cost;
        if (ageProducers_) {
            ageProducers_ = false;
            for (WaterProducer& producer : producers_) if (producer.occupant) AgeWaterProducer(producer);
        }
        host_->SendUpdated(static_cast<cISC4PlumbingSimulator*>(this));
        last_ = pass_;
        host_->AddHistory(0x0a14ebb4, last_.production);
        host_->AddHistory(0xca14ebd0, last_.consumed);
        host_->AddHistory(0xcab2f27b, last_.imported);
        host_->AddHistory(0x8ab2f280, last_.exported);
        host_->AddHistory(0xca14ebfe, pipeCount_);
        host_->AddHistory(0xca14ec19, value13c_);
        host_->AddHistory(0xca78a955, lastWatered_);
        host_->AddHistory(0x8a78ab06, lastBuildings_ - lastWatered_);
        std::vector<cISC4NeighborDeal*> exports;
        host_->Deals(kWaterDealExport, exports);
        for (cISC4NeighborDeal* deal : exports)
            if (deal->GetAmountUsed() < deal->GetAmount()) deal->SetFlags(0x20, true);
        host_->RefreshNeighborDeals();
        state_ = 9;
        host_->PostMessage(kMsgPlumbingDealsChanged, 8);
        break;
    }
    default: break;
    }
    return true;
}

// ---- per-month work -----------------------------------------------------------------------------------------------

// The funding curve moves the accumulator; that share of the pipes is distressed (picked at random), and a Gaussian number of the
// distressed ones bursts (a pipe-burst disaster at the cell).
void cSC4PlumbingSimulator::ApplyPipeFunding(const SC4Percentage& funding) {
    pipeFunding_ -= pipeFundingCurve.GetOutputValue(funding.value * 100.0f);
    if (pipeFunding_ > 1.0f) pipeFunding_ = 1.0f;
    else if (pipeFunding_ < 0.0f || pipeListCount_ == 0) pipeFunding_ = 0.0f;
    const std::uint32_t target = ToUnsigned(float(std::uint32_t(pipeListCount_)) * pipeFunding_);
    const float spread = float(int(target)) * burstVariance;
    int bursts = random_ ? random_->RandomSint32RangeGaussianFast(0, int(spread + spread)) : 0;
    if (target == std::uint32_t(distressedPipes_) && bursts == 0) return;
    std::vector<cISC4Occupant*> order;
    for (cISC4Occupant* pipe : pipes_) if (pipe) order.push_back(pipe);
    // the original shuffles with std::random_shuffle; here the simulator's own generator drives a Fisher-Yates
    if (random_)
        for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[random_->RandomUint32Uniform(std::uint32_t(i))]);
    if (std::uint32_t(distressedPipes_) < target) {
        for (auto it = order.begin(); it != order.end() && std::uint32_t(distressedPipes_) < target; ++it)
            if (!(host_->PipeFlags(*it) & kPipeDistressed)) {
                host_->SetPipeFlag(*it, kPipeDistressed, true);
                ++distressedPipes_;
            }
    } else if (target < std::uint32_t(distressedPipes_)) {
        for (auto it = order.begin(); it != order.end() && target < std::uint32_t(distressedPipes_); ++it)
            if (host_->PipeFlags(*it) & kPipeDistressed) {
                host_->SetPipeFlag(*it, kPipeDistressed, false);
                --distressedPipes_;
            }
    }
    for (auto it = order.rbegin(); it != order.rend() && bursts != 0; ++it) {
        const std::uint32_t flags = host_->PipeFlags(*it);
        if (!(flags & kPipeDistressed) || (flags & kPipeBurst)) continue;
        int x = 0, z = 0;
        host_->PipeCell(*it, x, z);
        host_->BurstPipeAt(x, z);
        host_->SetPipeFlag(*it, kPipeBurst, true);
        ++burstPipes_;
        --bursts;
    }
}

// Each producer's output: none while its lot needs power and has none; a ground pump needs water within groundSearchRadius (a far
// one gives farGroundPercent of capacity), a surface pump within surfaceSearchRadius, a source of type 2 none; then the department's
// funding, the water pollution at the producer (above the threshold it stops) and its wear curve scale it.
void cSC4PlumbingSimulator::UpdateWaterProduction() {
    averagePollution_ = 0;
    const float funding = productionFundingCurve.GetOutputValue(host_->DepartmentFundingFraction() * 100.0f) * 0.01f;
    pollutedSources_ = 0;
    std::uint32_t count = 0;
    for (WaterProducer& producer : producers_) {
        if (!producer.occupant) continue;   // a reference the city file did not resolve
        const std::uint32_t capacity = producer.capacity;
        cISCPropertyHolder* holder = producer.occupant->AsPropertyHolder();
        const std::uint32_t pollution = std::uint32_t(host_->WaterPollutionAt(producer.x, producer.z));
        averagePollution_ += pollution;
        ++count;
        if (host_->ProducerLotUnpowered(producer.occupant)) {
            producer.production = 0;
            producer.percent = 0.0f;
            continue;
        }
        std::uint8_t source = 0;
        const bool hasSource = holder->HasProperty(kPropProducerWaterSource) &&
                               cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWaterSource, source);
        std::uint32_t base = capacity;
        if (!hasSource || source == 0) {
            const int distance = NearestWater(producer.x, producer.z, WaterSourceType::Ground, int(groundSearchRadius));
            if (distance == -1) base = 0;
            else if (distance > 1) base = capacity * farGroundPercent / 100;
        } else if (source == 1 && NearestWater(producer.x, producer.z, WaterSourceType::Surface, int(surfaceSearchRadius)) == -1) {
            base = 0;
        }
        producer.polluted = false;
        float percent = funding;
        if (pollutionThreshold < pollution && 0 < int(capacity)) {
            ++pollutedSources_;
            producer.polluted = true;
            percent = 0.0f;
        }
        if (holder->HasProperty(kPropProducerConditionCurve)) {
            cSC4ResponseCurve condition;
            LoadCurve(condition, holder, kPropProducerConditionCurve);
            float wear = 0.0f;
            cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear);
            percent *= condition.GetOutputValue(wear);
        }
        producer.production = std::uint32_t(int(float(int(base)) * percent));
        producer.percent = percent;
    }
    if (count != 0) averagePollution_ /= count;
}

// Wear grows by the age rate, scaled by the funding curve and by how much of its output the producer's network used; past the
// warning limit its lot is distressed (2), past the breaking limit broken (4).
void cSC4PlumbingSimulator::AgeWaterProducer(WaterProducer& producer) {
    float wear = 0.0f, ageRate = 0.0f, warn = 99.0f, broken = 99.0f;
    std::int32_t current = 0;
    const int production = int(producer.production);
    const float funding = host_->DepartmentFundingFraction();
    cISCPropertyHolder* holder = producer.occupant->AsPropertyHolder();
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerAgeRate, ageRate);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerCurrentProduced, current);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWearWarn, warn);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWearBroken, broken);
    const float use = (production < 1 || current < 1) ? 0.0f : float(current) / float(production);
    const float rate = wearByFundingCurve.GetOutputValue(funding) * ageRate;
    wear = wearByUseCurve.GetOutputValue(use) * rate + wear;
    cSCPropertyHelp::SetPropertyValue(holder, kPropProducerWear, wear);
    if (broken <= wear || warn <= wear) {
        float position[3] = {};
        producer.occupant->GetPosition(position);
        int x = 0, z = 0;
        host_->PositionToCell(position[0], position[2], x, z);
        host_->SetLotConditionAt(x, z, broken <= wear ? 4 : 2);
    }
}

// ---- queries ------------------------------------------------------------------------------------------------------

int cSC4PlumbingSimulator::BuildingConsumption(int x, int z) {
    std::vector<cISC4Occupant*> buildings;
    host_->OccupantsAtCell(x, z, kPlumbingOccupantBuilding, buildings);
    int total = 0;
    for (cISC4Occupant* building : buildings) {
        std::uint32_t demand = 0;
        if (cSCPropertyHelp::GetPropertyValue(building->AsPropertyHolder(), kPropBuildingWaterDemand, demand)) total += int(demand);
    }
    return total;
}

// The occupant's lot (else its bounding box, far corner exclusive) is OR-ed with the mask.
void cSC4PlumbingSimulator::FillGridAtOccupant(cSC4SimGrid<std::uint8_t>& grid, cISC4Occupant* occupant, std::uint8_t mask) {
    float lo[3] = {}, hi[3] = {};
    occupant->GetBoundingBox(lo, hi);
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    grid.PositionToTract(lo[0], lo[2], x0, z0);
    PlumbingRect lot;
    if (host_->LotRectAt(x0, z0, lot)) {
        x0 = lot.left;
        z0 = lot.top;
        x1 = lot.right + 1;
        z1 = lot.bottom + 1;
    } else {
        grid.PositionToTract(hi[0], hi[2], x1, z1);
    }
    auto& data = grid.GetGridData();
    for (int x = std::max(0, x0); x < x1 && x < cellsX_; ++x)
        for (int z = std::max(0, z0); z < z1 && z < cellsZ_; ++z) data[std::size_t(x)][std::size_t(z)] |= mask;
}

bool cSC4PlumbingSimulator::GetOccupantMonthlyUpkeepCost(cISC4Occupant* occupant, std::int64_t& cost) {
    const PlumbingBudgetEntry entry = host_->GetBudgetEntry(occupant);
    if (entry.tracked) cost = entry.monthlyCost;
    return entry.tracked;
}

void cSC4PlumbingSimulator::FindWaterBuildings(cISC4Occupant** leastFunded, SC4Percentage* funding, cISC4Occupant** mostWorn, int* wearPercent) {
    if (producers_.empty()) return;
    float lowestFunding = 2.0f, highestWear = 0.0f;
    cISC4Occupant* lowest = nullptr;
    cISC4Occupant* worn = nullptr;
    for (const WaterProducer& producer : producers_) {
        if (!producer.occupant) continue;
        const PlumbingBudgetEntry entry = host_->GetBudgetEntry(producer.occupant);
        if (entry.tracked && entry.hasExplicitFunding && entry.funding < lowestFunding) {
            lowestFunding = entry.funding;
            lowest = producer.occupant;
        }
        cISCPropertyHolder* holder = producer.occupant->AsPropertyHolder();
        float wear = 0.0f, limitA = 99.0f, limitB = 99.0f;
        cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear);
        cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWearWarn, limitA);
        cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWearBroken, limitB);
        wear = wear / std::max(limitA, limitB);
        if (highestWear < wear) {
            highestWear = wear;
            worn = producer.occupant;
        }
    }
    if (mostWorn && worn) {
        *mostWorn = worn;
        worn->AddRef();
        if (wearPercent) *wearPercent = int(highestWear * 100.0f);
    }
    if (leastFunded && lowest) {
        *leastFunded = lowest;
        lowest->AddRef();
        if (funding) funding->value = lowestFunding;
    }
}

void cSC4PlumbingSimulator::GetWaterProducers(std::list<cISC4Occupant*>& out) {
    for (WaterProducer& producer : producers_) {
        if (!producer.occupant) continue;
        producer.occupant->AddRef();
        out.push_back(producer.occupant);
    }
}

bool cSC4PlumbingSimulator::IsBuildingWatered(cISC4Occupant* building) {
    float position[3] = {};
    building->GetPosition(position);
    int x = 0, z = 0;
    host_->PositionToCell(position[0], position[2], x, z);
    return IsCellWatered(x, z);
}

std::uint32_t cSC4PlumbingSimulator::GetWaterProduction(cISC4Occupant* occupant) {
    for (const WaterProducer& producer : producers_)
        if (producer.occupant == occupant) return producer.production;
    return 0;
}

bool cSC4PlumbingSimulator::GetWaterProducerInfo(cISC4Occupant* occupant, std::uint32_t* production, std::uint32_t* capacity,
                                                 SC4Percentage* percent, bool* polluted, std::uint32_t* currentProduced) {
    for (const WaterProducer& producer : producers_) {
        if (producer.occupant != occupant) continue;
        if (production) *production = producer.production;
        if (capacity) *capacity = producer.capacity;
        if (percent) percent->value = producer.percent;
        if (polluted) *polluted = producer.polluted;
        if (currentProduced) cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropProducerCurrentProduced, *currentProduced);
        return true;
    }
    return false;
}

bool cSC4PlumbingSimulator::GetWaterProducerConditionPercent(cISC4Occupant* occupant, SC4Percentage& out) {
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    float wear = 0.0f;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear) || !holder->HasProperty(kPropProducerConditionCurve)) return false;
    cSC4ResponseCurve curve;
    LoadCurve(curve, holder, kPropProducerConditionCurve);
    out.value = curve.GetOutputValue(wear);
    return true;
}

int cSC4PlumbingSimulator::NearestWater(int x, int z, WaterSourceType type, int radius) {
    if (type == WaterSourceType::None) return 0;
    const int width = host_->CityCellsX(), height = host_->CityCellsZ();
    if (width <= 0 || height <= 0) return -1;
    SpiralIterator spiral;
    spiral.x = x;
    spiral.z = z;
    spiral.targetX = x + 1;
    spiral.targetZ = z;
    spiral.minX = std::max(0, x - radius);
    spiral.minZ = std::max(0, z - radius);
    spiral.maxX = radius + x < width ? radius + x : width - 1;
    spiral.maxZ = radius + z < height ? radius + z : height - 1;
    spiral.mode = 1;
    spiral.maxX = std::max(spiral.maxX, spiral.minX);
    spiral.maxZ = std::max(spiral.maxZ, spiral.minZ);
    spiral.direction = 1;
    spiral.state = 0;
    spiral.onZ = 1;
    spiral.edgesHit = 0;
    spiral.turns = 0;
    if (x < spiral.minX || z < spiral.minZ || spiral.maxX < x || spiral.maxZ < z) {
        spiral.state = 2;
        spiral.edgesHit = 0xf;
    }
    int cx = 0, cz = 0;
    while (spiral.GetNext(cx, cz))
        if (host_->IsWaterCell(cx, cz)) return spiral.turns;   // the ring the water was found on
    return -1;
}

void cSC4PlumbingSimulator::GetConnectionCell(cISC4NeighborConnection* connection, int& x, int& z) const {
    if (!connection) return;
    connection->GetConnectionCell(x, z);
    x = std::clamp(x, 0, std::max(0, host_->CityCellsX() - 1));
    z = std::clamp(z, 0, std::max(0, host_->CityCellsZ() - 1));
}

bool cSC4PlumbingSimulator::IsNeighborConnectedToCurrentNetwork(void* city, bool markVisited) {
    std::vector<cISC4NeighborConnection*> connections;
    host_->Connections(city, connections);
    for (cISC4NeighborConnection* connection : connections) {
        int x = 0, z = 0;
        GetConnectionCell(connection, x, z);
        if (!Visited(x, z)) continue;
        if (markVisited) connection->SetVisitedThisCycle(true);
        return true;
    }
    return false;
}

// ---- streams ------------------------------------------------------------------------------------------------------
// Version 4 (i386 Write 0x00074c46 / Read 0x00074f7c).

namespace {
void WriteWaterProducer(cIGZOStream& stream, IPlumbingHost* host, const WaterProducer& producer) {
    stream.SetSint32(producer.x);
    stream.SetSint32(producer.z);
    stream.SetSint32(producer.rect.left);
    stream.SetSint32(producer.rect.top);
    stream.SetSint32(producer.rect.right);
    stream.SetSint32(producer.rect.bottom);
    stream.SetUint32(producer.capacity);
    stream.SetUint32(producer.production);
    host->WriteOccupant(stream, producer.occupant);
}
void ReadWaterProducer(cIGZIStream& stream, IPlumbingHost* host, WaterProducer& producer) {
    stream.GetSint32(producer.x);
    stream.GetSint32(producer.z);
    stream.GetSint32(producer.rect.left);
    stream.GetSint32(producer.rect.top);
    stream.GetSint32(producer.rect.right);
    stream.GetSint32(producer.rect.bottom);
    stream.GetUint32(producer.capacity);
    stream.GetUint32(producer.production);
    host->ReadOccupant(stream, kGZIID_cISC4Occupant, &producer.occupant);
}
}  // namespace

bool cSC4PlumbingSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(4);
    host_->WriteGrid(stream, working_);                                     // +0x30
    host_->WriteGrid(stream, published_);                                   // +0x34
    stream.SetUint32(std::uint32_t(cellsX_));                               // +0x8c
    stream.SetUint32(std::uint32_t(cellsZ_));                               // +0x90
    for (std::uint8_t flag : cellFlags_) stream.SetUint8(flag);             // +0x88
    stream.SetSint32(month_);                                               // +0x9c
    stream.SetUint32(averagePollution_);                                    // +0xa0
    stream.SetSint64(waterCost_);                                           // +0x148
    stream.SetUint32(std::uint32_t(producers_.size()));                     // +0xbc
    stream.SetUint32(std::uint32_t(pollutedSources_));                      // +0x19c
    for (const WaterProducer& producer : producers_) WriteWaterProducer(stream, host_, producer);
    WriteWaterProducer(stream, host_, current_ ? *current_ : WaterProducer{});   // +0x140
    stream << scan_;                                                        // +0x144
    stream.SetSint64(demand_);                                              // +0x130
    stream.SetUint32(std::uint32_t(pipeListCount_));                        // +0x178
    stream.SetUint32(std::uint32_t(distressedPipes_));                      // +0x17c
    stream.SetUint32(std::uint32_t(burstPipes_));                           // +0x180
    for (cISC4Occupant* pipe : pipes_) host_->WriteOccupant(stream, pipe);  // +0x170
    host_->WriteDepartment(stream);                                         // +0xa4
    stream.SetSint64(last_.production);                                     // +0x110
    stream.SetSint64(last_.consumed);                                       // +0x118
    stream.SetSint64(last_.imported);                                       // +0x120
    stream.SetSint64(last_.exported);                                       // +0x128
    return !stream.GetError();
}

bool cSC4PlumbingSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 4) return false;
    for (cSC4SimGrid<std::uint8_t>** grid : {&working_, &published_}) {
        if (*grid) (*grid)->Release();
        *grid = nullptr;
        host_->ReadGrid(stream, grid);
    }
    std::uint32_t cellsX = 0, cellsZ = 0;
    stream.GetUint32(cellsX);
    stream.GetUint32(cellsZ);
    cellsX_ = int(cellsX);
    cellsZ_ = int(cellsZ);
    visitedSize_ = std::max(cellsX_, cellsZ_);
    visited_.assign(std::size_t(visitedSize_) * std::size_t(visitedSize_) / 32 + 1, 0);
    cellFlags_.assign(std::size_t(cellsX_) * std::size_t(cellsZ_), 0);
    for (std::uint8_t& flag : cellFlags_) stream.GetUint8(flag);
    stream.GetSint32(month_);
    stream.GetUint32(averagePollution_);
    stream.GetSint64(waterCost_);
    std::uint32_t producerCount = 0, polluted = 0;
    stream.GetUint32(producerCount);
    stream.GetUint32(polluted);
    pollutedSources_ = int(polluted);
    for (WaterProducer& producer : producers_) if (producer.occupant) producer.occupant->Release();
    producers_.clear();
    for (std::uint32_t i = 0; i < producerCount && !stream.GetError(); ++i) {
        WaterProducer producer;
        ReadWaterProducer(stream, host_, producer);
        producers_.push_back(producer);   // (the original keeps a producer whose reference did not resolve)
    }
    WaterProducer current;
    ReadWaterProducer(stream, host_, current);
    current_ = nullptr;
    for (WaterProducer& producer : producers_)
        if (current.occupant && producer.occupant == current.occupant) {
            current_ = &producer;
            break;
        }
    if (current.occupant) current.occupant->Release();
    stream >> scan_;
    stream.GetSint64(demand_);
    std::uint32_t pipes = 0, distressed = 0, burst = 0;
    stream.GetUint32(pipes);
    stream.GetUint32(distressed);
    stream.GetUint32(burst);
    pipeListCount_ = int(pipes);
    distressedPipes_ = int(distressed);
    burstPipes_ = int(burst);
    for (cISC4Occupant* pipe : pipes_) if (pipe) pipe->Release();
    pipes_.clear();
    for (std::uint32_t i = 0; i < pipes && !stream.GetError(); ++i) {
        cISC4Occupant* pipe = nullptr;
        host_->ReadOccupant(stream, kPlumbingPipeIid, &pipe);
        pipes_.push_back(pipe);
    }
    pipeListCount_ = int(pipes_.size());   // (a reference that did not resolve stays in the list, as in the original)
    host_->ReadDepartment(stream);
    gridsInitialised_ = true;
    state_ = 0;
    stream.GetSint64(last_.production);
    stream.GetSint64(last_.consumed);
    stream.GetSint64(last_.imported);
    stream.GetSint64(last_.exported);
    captureLoaded_ = true;   // the grids are records of their own, filled after this one: captured on the first pass
    if (std::getenv("SC4_WATERDEBUG"))
        std::fprintf(stderr, "water %p: loaded %dx%d producers %zu pipes %zu demand %lld; last production %lld consumed %lld imported %lld "
                     "exported %lld\n",
                     static_cast<const void*>(this), cellsX_, cellsZ_, producers_.size(), pipes_.size(), static_cast<long long>(demand_),
                     static_cast<long long>(last_.production), static_cast<long long>(last_.consumed), static_cast<long long>(last_.imported),
                     static_cast<long long>(last_.exported));
    return !stream.GetError();
}

}  // namespace sc4
