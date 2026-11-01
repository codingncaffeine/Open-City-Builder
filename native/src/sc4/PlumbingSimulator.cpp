#include "sc4/PlumbingSimulator.h"
#include "sc4/SimStreamOps.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>

#include "gz/Bit1.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/SpiralIterator.h"

namespace sc4 {

namespace {

constexpr std::uint8_t kFlagBuilding = 0x40, kFlagProducer = 0x20, kFlagPipe = 0x10, kKeepOnPipeRemoval = 0xe0;
constexpr std::uint32_t kPipeDistressed = 0x10000, kPipeBurst = 0x20000;

int RoundToInt(float v) { return static_cast<int>(std::nearbyintf(v)); }   // the 1.5*2^23 trick, exact for |v| < 2^22

bool CurveIsEmpty(const cSC4ResponseCurve& curve) {
    float max = 0;
    return !curve.GetMaxInputValue(max);
}

// Loads a curve from the exemplar property `id` when present (the keyed SetResponseCurveData overload).
void LoadCurve(cSC4ResponseCurve& curve, const cISCPropertyHolder* holder, std::uint32_t id) {
    std::vector<float> values;
    if (holder->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(holder, id, values)) curve.SetResponseCurveData(values.data(), values.size());
}

}  // namespace

cSC4PlumbingSimulator::cSC4PlumbingSimulator(IPlumbingHost* host, cRZRandom* random) : host_(host), random_(random) {}

cSC4PlumbingSimulator::~cSC4PlumbingSimulator() { Shutdown(); }

// ---- identity -------------------------------------------------------------------------------------------------

bool cSC4PlumbingSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);         // +0x10
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);  // +0x8
    else if (iid == 1 || iid == kGZIID_cISC4PlumbingSimulator) *out = static_cast<cISC4PlumbingSimulator*>(this);  // +0
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4PlumbingSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4PlumbingSimulator::DoMessage(cIGZMessage2*) { return true; }   // message-server routing unrecovered

// ---- tunables -----------------------------------------------------------------------------------------------------

void cSC4PlumbingSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    const std::vector<SC4Point> flat{{0.0f, 1.0f}, {1.0f, 1.0f}};
    wearByAgeCurve.SetResponseCurveData(flat);
    wearByUseCurve.SetResponseCurveData(flat);
    pipeFundingCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, -0.02f}, {120.0f, 0.01f}});
    if (!exemplar) return;
    using H = cSCPropertyHelp;
    std::uint32_t v = 0;
    if (H::GetPropertyValue(exemplar, 0x68f5436a, v)) tunableF8 = int(v);
    if (H::GetPropertyValue(exemplar, 0x68f5436b, v)) groundSearchRadius = int(v);
    if (H::GetPropertyValue(exemplar, 0x68f5436c, v)) surfaceSearchRadius = int(v);
    if (H::GetPropertyValue(exemplar, 0x68f5436d, v)) farGroundPercent = int(v);
    if (H::GetPropertyValue(exemplar, 0xaa161f81, v)) pollutionThreshold = int(v);
    H::GetPropertyValue(exemplar, 0x8a235c56, burstVariance);
    H::GetPropertyValue(exemplar, 0xea4025a9, departmentFunding);
    H::GetPropertyValue(exemplar, 0x8a84778f, pipeCostFactor);
    const std::vector<SC4Point> identity{{0.0f, 0.0f}, {100.0f, 100.0f}};
    LoadCurve(productionFundingCurve, exemplar, 0x6a410bf2);
    if (CurveIsEmpty(productionFundingCurve)) productionFundingCurve.SetResponseCurveData(identity);
    LoadCurve(wearByAgeCurve, exemplar, 0x911e2e8);
    if (CurveIsEmpty(wearByAgeCurve)) wearByAgeCurve.SetResponseCurveData(identity);
    LoadCurve(wearByUseCurve, exemplar, 0x911e2e9);
    if (CurveIsEmpty(wearByUseCurve)) wearByUseCurve.SetResponseCurveData(identity);
    LoadCurve(pipeFundingCurve, exemplar, 0xca7138c4);
}

void cSC4PlumbingSimulator::InitCellMap() {
    if (gridsInitialized_) return;
    gridsInitialized_ = true;
    auto makeGrid = [](std::uint32_t instanceId) {
        auto* grid = new cSC4SimGrid<std::uint8_t>();
        grid->SetTractSize(1);
        grid->SetInstanceID(instanceId);
        grid->SetTractValues(0);
        return grid;
    };
    if (!auxGrid_) auxGrid_ = makeGrid(0x49d5bb8c);
    if (!supplyGrid_) supplyGrid_ = makeGrid(0x49d5bba1);
    cellsX_ = host_->CityCellsX();
    cellsZ_ = host_->CityCellsZ();
    bitmapSize_ = std::max(cellsX_, cellsZ_);
    cellFlags_.assign(std::size_t(cellsX_) * std::size_t(cellsZ_), 0);
    bitmap_.assign(std::size_t(bitmapSize_) * std::size_t(bitmapSize_) / 32 + 1, 0);
    countdown_ = 0;
    dirty_ = true;
}

bool cSC4PlumbingSimulator::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (auxGrid_) { auxGrid_->Shutdown(); auxGrid_->Release(); auxGrid_ = nullptr; }
    if (supplyGrid_) { supplyGrid_->Shutdown(); supplyGrid_->Release(); supplyGrid_ = nullptr; }
    cellFlags_.clear();
    bitmap_.clear();
    for (WaterProducer& producer : producers_) producer.occupant->Release();
    producers_.clear();
    currentProducer_ = nullptr;
    for (IPlumbingPipe* pipe : pipes_) pipe->Release();
    pipes_.clear();
    burstPipes_ = 0;
    pollutedSources_ = 0;
    return true;
}

// ---- messages -----------------------------------------------------------------------------------------------------

bool cSC4PlumbingSimulator::DoIdle(std::uint32_t) { return true; }   // not recovered (PLUMBING-1)

bool cSC4PlumbingSimulator::DoMessage(IPlumbingMessage& message) {
    const std::uint32_t type = message.Type();
    switch (type) {
        case kMsgSimIdle1:
            return DoIdle(5);
        case kMsgSimIdle2:
            return DoIdle(message.IdleParameter());
        case kMsgFundingChanged:
            dirty_ = dirtySecond_ = true;
            ApplyPipeFunding(SC4Percentage(host_->DepartmentFundingFraction()));
            return true;
        case kMsgPlumbingFlag:
            flag20 = true;
            return true;
        case kMsgOccupantInserted:
        case kMsgOccupantRemoved: {
            IPlumbingOccupant* occupant = message.Occupant();
            if (occupant->PropertyHolder()->HasProperty(0x29244db)) return true;   // marked: not part of the network
            if (type == kMsgOccupantInserted) DoOccupantInserted(occupant);
            else DoOccupantRemoved(occupant);
            return true;
        }
        case kMsgReloadTunables:
            ReadSimulatorTunableValues(nullptr);   // the original re-reads the exemplar through the resource manager
            return true;
        case kMsgCellCleared: {
            const int x = int(message.Data1()), z = message.Data2();
            CellFlag(x, z) &= 0xe0;
            dirty_ = true;
            [[fallthrough]];
        }
        case kMsgNetworkChanged: {
            if (message.Data1() == 0) {
                std::uint32_t changeType = 0;
                if (message.GetNetworkChangeType(changeType) && (changeType == 0x4773ffe3 || changeType == 0x4773ffe2)) {
                    countdown_ = 9;
                    dirty_ = true;
                }
            }
            return true;
        }
        default:
            return true;
    }
}

void cSC4PlumbingSimulator::DoOccupantInserted(IPlumbingOccupant* occupant) {
    const std::uint32_t type = occupant->OccupantType();
    if (type == kPlumbingOccupantBuilding) {
        DoBuildingChanged(occupant, true);
    } else if (type == kPlumbingOccupantNetwork) {
        IPlumbingPipe* pipe = occupant->AsPipe();
        if (pipe && !pipe->TestFlag(0x80000000)) DoPipeChanged(pipe, true);
    }
}

void cSC4PlumbingSimulator::DoOccupantRemoved(IPlumbingOccupant* occupant) {
    const std::uint32_t type = occupant->OccupantType();
    if (type == kPlumbingOccupantBuilding) {
        DoBuildingChanged(occupant, false);
    } else if (type == kPlumbingOccupantNetwork) {
        IPlumbingPipe* pipe = occupant->AsPipe();
        if (pipe && !pipe->TestFlag(0x80000000)) DoPipeChanged(pipe, false);
    }
}

void cSC4PlumbingSimulator::DoPipeChanged(IPlumbingPipe* pipe, bool added) {
    PlumbingRect bounds;
    pipe->GetCellBounds(bounds);
    for (int x = bounds.left; x <= bounds.right; ++x) {
        for (int z = bounds.top; z <= bounds.bottom; ++z) {
            std::uint8_t& flags = CellFlag(x, z);
            if (!added) {
                flags &= kKeepOnPipeRemoval;
            } else {
                flags |= kFlagPipe;
                if (pipe->HasConnection(2, 4)) flags |= 4;
                if (pipe->HasConnection(1, 4)) flags |= 2;
                if (pipe->HasConnection(0, 4)) flags |= 1;
                if (pipe->HasConnection(3, 4)) flags |= 8;
            }
        }
    }
    auto it = std::find(pipes_.begin(), pipes_.end(), pipe);
    if (!added) {
        if (it != pipes_.end()) {
            if (pipe->GetFlags() & kPipeDistressed) --distressedPipes_;
            if (pipe->GetFlags() & kPipeBurst) --burstPipes_;
            pipes_.erase(it);
            pipe->Release();
        }
    } else if (it == pipes_.end()) {
        pipe->AddRef();
        pipes_.push_back(pipe);
    }
    host_->SetPipeInfrastructureCost(RoundToInt(float(pipes_.size()) * pipeCostFactor));
    dirty_ = true;
}

void cSC4PlumbingSimulator::DoBuildingChanged(IPlumbingOccupant* building, bool added) {
    float px = 0, pz = 0;
    building->GetPosition(px, pz);
    int cellX = 0, cellZ = 0;
    auxGrid_->PositionToTract(px, pz, cellX, cellZ);
    cISCPropertyHolder* holder = building->PropertyHolder();
    std::uint8_t& flags = CellFlag(cellX, cellZ);
    std::int32_t consumption = 0;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPropBuildingWaterDemand, consumption)) {
        if (!added) {
            flags &= 0xbf;
            demand_ -= consumption;
        } else {
            flags |= kFlagBuilding;
            demand_ += consumption;
        }
    }
    if (!building->HasBudgetInterface()) return;   // no dirty flag either
    host_->ApplyBuildingBudget(building, added);
    if (!holder->HasProperty(kPropWaterProductionCapacity)) return;   // not a water producer
    if (!added) {
        auto it = std::find_if(producers_.begin(), producers_.end(), [&](const WaterProducer& p) { return p.occupant == building; });
        if (it != producers_.end()) {
            for (int x = it->rect.left; x <= it->rect.right; ++x)
                for (int z = it->rect.top; z <= it->rect.bottom; ++z) CellFlag(x, z) &= 0xdf;
            if (currentProducer_ && currentProducer_->occupant == building) currentProducer_ = nullptr;
            producers_.erase(it);
            building->Release();
        }
    } else {
        building->AddRef();   // quirk kept: the reference is also taken when no lot is found and nothing is stored
        PlumbingRect rect;
        if (!host_->GetLotRect(cellX, cellZ, rect)) return;
        WaterProducer producer;
        producer.x = cellX;
        producer.z = cellZ;
        producer.rect = rect;
        producer.capacity = host_->LotWaterCapacity(building);
        producer.occupant = building;
        producers_.push_back(producer);
        for (int x = rect.left; x <= rect.right; ++x)
            for (int z = rect.top; z <= rect.bottom; ++z) CellFlag(x, z) |= kFlagProducer;
        cSCPropertyHelp::SetPropertyValue(holder, kPropProducerWear, 0.0f);
    }
    dirty_ = true;
}

// ---- passes -------------------------------------------------------------------------------------------------------

void cSC4PlumbingSimulator::ApplyPipeFunding(const SC4Percentage& funding) {
    const float step = pipeFundingCurve.GetOutputValue(funding.value * 100.0f);
    pipeFundingAccum -= step;
    float fraction = pipeFundingAccum;
    if (fraction > 1.0f) {
        fraction = 1.0f;
        pipeFundingAccum = fraction;
    } else if (fraction < 0.0f || pipes_.empty()) {
        fraction = 0.0f;
        pipeFundingAccum = fraction;
    }
    {
        const std::uint32_t distressedTarget = std::uint32_t(float(pipes_.size()) * fraction);
        const int variance = int(float(int(distressedTarget)) * burstVariance + float(int(distressedTarget)) * burstVariance);
        int burstCount = random_->RandomSint32RangeGaussianFast(0, variance);
        if (burstCount == 0 && std::uint32_t(distressedPipes_) == distressedTarget) return;
        std::vector<IPlumbingPipe*> order(pipes_.begin(), pipes_.end());
        // quirk: the original shuffles with libc++'s std::random_shuffle; here the sim's own generator drives a Fisher-Yates
        for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[random_->RandomUint32Uniform(std::uint32_t(i))]);
        if (std::uint32_t(distressedPipes_) < distressedTarget) {
            for (IPlumbingPipe* pipe : order) {
                if (std::uint32_t(distressedPipes_) >= distressedTarget) break;
                if (!(pipe->GetFlags() & kPipeDistressed)) {
                    pipe->SetFlags(kPipeDistressed);
                    ++distressedPipes_;
                }
            }
        } else if (distressedTarget < std::uint32_t(distressedPipes_)) {
            for (IPlumbingPipe* pipe : order) {
                if (pipe->GetFlags() & kPipeDistressed) {
                    pipe->ClearFlags(kPipeDistressed);
                    --distressedPipes_;
                }
                if (distressedTarget >= std::uint32_t(distressedPipes_)) break;
            }
        }
        if (burstCount != 0) {
            for (auto it = order.rbegin(); it != order.rend() && burstCount != 0; ++it) {
                IPlumbingPipe* pipe = *it;
                const std::uint32_t flags = pipe->GetFlags();
                if ((flags & kPipeDistressed) && !(flags & kPipeBurst)) {
                    int x = 0, z = 0;
                    pipe->GetCell(x, z);
                    host_->BurstPipeAt(x, z);
                    pipe->SetFlags(kPipeBurst);
                    ++burstPipes_;
                    --burstCount;
                }
            }
        }
    }
}

void cSC4PlumbingSimulator::UpdateWaterProduction() {
    averagePollution_ = 0;
    const float fundingScale = productionFundingCurve.GetOutputValue(host_->DepartmentFundingFraction() * 100.0f) * 0.01f;
    pollutedSources_ = 0;
    int count = 0;
    std::uint32_t pollutionSum = 0;
    for (WaterProducer& producer : producers_) {
        ++count;
        cISCPropertyHolder* holder = producer.occupant->PropertyHolder();
        const std::uint32_t pollution = host_->PollutionAt(producer.x, producer.z);
        pollutionSum += pollution;
        if (host_->ProducerLotDisabled(producer.occupant)) {
            producer.production = 0;
            producer.percent = 0.0f;
            continue;
        }
        const int capacity = producer.capacity;
        int base = 0;
        auto groundBase = [&]() {
            const int distance = NearestWater(producer.x, producer.z, WaterSourceType::Ground, groundSearchRadius);
            int b = 0;
            if (distance != -1) {
                b = capacity;
                if (distance > 1) b = int(std::uint32_t(farGroundPercent * capacity) / 100u);
            }
            return b;
        };
        bool surface = false;
        if (holder->HasProperty(kPropProducerGroundwater)) {
            bool flag = false;
            cSCPropertyHelp::GetPropertyValue(holder, kPropProducerGroundwater, flag);
            surface = flag;
            if (surface) {
                base = NearestWater(producer.x, producer.z, WaterSourceType::Surface, surfaceSearchRadius) != -1 ? capacity : 0;
            } else {
                base = groundBase();
            }
        } else {
            base = groundBase();
        }
        producer.polluted = false;
        float percent = fundingScale;
        if (capacity > 0 && std::uint32_t(pollutionThreshold) < pollution) {
            ++pollutedSources_;
            producer.polluted = true;
            percent = 0.0f;
        }
        if (holder->HasProperty(kPropProducerConditionCurve)) {
            cSC4ResponseCurve condition;
            std::vector<float> values;
            if (cSCPropertyHelp::GetPropertyValue(holder, kPropProducerConditionCurve, values)) condition.SetResponseCurveData(values.data(), values.size());
            float wear = 0.0f;
            cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear);
            percent *= condition.GetOutputValue(wear);
        }
        producer.percent = percent;
        producer.production = int(float(base) * percent);
    }
    if (count != 0) averagePollution_ = int(pollutionSum / std::uint32_t(count));
}

void cSC4PlumbingSimulator::AgeWaterProducer(WaterProducer& producer) {
    float wear = 0.0f, ageRate = 0.0f, warn = 99.0f, broken = 99.0f;
    std::int32_t currentProduced = 0;
    const int produced = producer.production;
    float funding = host_->DepartmentFundingFraction();
    cISCPropertyHolder* holder = producer.occupant->PropertyHolder();
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerAgeRate, ageRate);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerCurrentProduced, currentProduced);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWearWarn, warn);
    cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWearBroken, broken);
    float use = 0.0f;
    if (produced > 0 && currentProduced > 0) use = float(currentProduced) / float(produced);
    const float rate = wearByAgeCurve.GetOutputValue(funding) * ageRate;
    wear = wearByUseCurve.GetOutputValue(use) * rate + wear;
    cSCPropertyHelp::SetPropertyValue(holder, kPropProducerWear, wear);
    if (broken <= wear || warn <= wear) {
        float px = 0, pz = 0;
        producer.occupant->GetPosition(px, pz);
        int cellX = 0, cellZ = 0;
        host_->PositionToCell(px, pz, cellX, cellZ);
        host_->SetLotCondition(cellX, cellZ, broken <= wear ? 4 : 2);
    }
}

// ---- queries ------------------------------------------------------------------------------------------------------

int cSC4PlumbingSimulator::BuildingConsumption(int cellX, int cellZ) {
    int total = 0;
    for (IPlumbingOccupant* building : host_->BuildingsAtCell(cellX, cellZ)) {
        std::uint32_t demand = 0;
        if (!cSCPropertyHelp::GetPropertyValue(building->PropertyHolder(), kPropBuildingWaterDemand, demand)) demand = 0;
        building->Release();
        total += int(demand);
    }
    return total;
}

void cSC4PlumbingSimulator::FillGridAtOccupant(cSC4SimGrid<std::uint8_t>& grid, IPlumbingOccupant* occupant, std::uint8_t mask) {
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    occupant->GetFootprint(lo, hi);
    int x0 = 0, z0 = 0;
    grid.PositionToTract(lo[0], lo[2], x0, z0);
    int xEnd = 0, zEnd = 0;
    int xBegin = x0, zBegin = z0;
    PlumbingRect lot;
    if (host_->GetLotRect(x0, z0, lot)) {
        xBegin = lot.left;
        zBegin = lot.top;
        xEnd = lot.right + 1;
        zEnd = lot.bottom + 1;
    } else {
        grid.PositionToTract(hi[0], hi[2], xEnd, zEnd);   // exclusive end: the far corner cell itself is not filled
    }
    auto& data = grid.GetGridData();
    for (int x = xBegin; x < xEnd; ++x)
        for (int z = zBegin; z < zEnd; ++z) data[std::size_t(x)][std::size_t(z)] |= mask;
}

bool cSC4PlumbingSimulator::GetOccupantMonthlyUpkeepCost(IPlumbingOccupant* occupant, std::int64_t& cost) {
    const PlumbingBudgetEntry entry = host_->GetBudgetEntry(occupant->PropertyHolder());
    if (entry.tracked) cost = entry.monthlyCost;
    return entry.tracked;
}

void cSC4PlumbingSimulator::FindWaterBuildings(IPlumbingOccupant** leastFunded, SC4Percentage* funding, IPlumbingOccupant** mostWorn,
                                               int* wearPercent) {
    if (producers_.empty()) return;
    float lowestFunding = 2.0f, highestWear = 0.0f;
    IPlumbingOccupant* lowest = nullptr;
    IPlumbingOccupant* worn = nullptr;
    for (const WaterProducer& producer : producers_) {
        cISCPropertyHolder* holder = producer.occupant->PropertyHolder();
        const PlumbingBudgetEntry entry = host_->GetBudgetEntry(holder);
        if (entry.tracked && entry.hasExplicitFunding && entry.funding < lowestFunding) {
            lowestFunding = entry.funding;
            lowest = producer.occupant;
        }
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
        *wearPercent = int(highestWear * 100.0f);
    }
    if (leastFunded && lowest) {
        *leastFunded = lowest;
        lowest->AddRef();
        if (funding) funding->value = lowestFunding;
    }
}

void cSC4PlumbingSimulator::GetWaterProducers(std::list<IPlumbingOccupant*>& out) {
    for (WaterProducer& producer : producers_) {
        producer.occupant->AddRef();
        out.push_back(producer.occupant);
    }
}

bool cSC4PlumbingSimulator::IsBuildingWatered(IPlumbingOccupant* building) {
    float px = 0, pz = 0;
    building->GetPosition(px, pz);
    int cellX = 0, cellZ = 0;
    supplyGrid_->PositionToTract(px, pz, cellX, cellZ);
    return IsCellWatered(cellX, cellZ);
}

int cSC4PlumbingSimulator::GetWaterProduction(IPlumbingOccupant* occupant) {
    for (const WaterProducer& producer : producers_)
        if (producer.occupant == occupant) return producer.production;
    return 0;
}

bool cSC4PlumbingSimulator::GetWaterProducerInfo(IPlumbingOccupant* occupant, std::uint32_t* production, std::uint32_t* capacity,
                                                 SC4Percentage* percent, bool* polluted, std::uint32_t* currentProduced) {
    for (const WaterProducer& producer : producers_) {
        if (producer.occupant != occupant) continue;
        if (production) *production = std::uint32_t(producer.production);
        if (capacity) *capacity = std::uint32_t(producer.capacity);
        if (percent) percent->value = producer.percent;
        if (polluted) *polluted = producer.polluted;
        if (currentProduced) cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPropProducerCurrentProduced, *currentProduced);
        return true;
    }
    return false;
}

bool cSC4PlumbingSimulator::GetWaterProducerConditionPercent(IPlumbingOccupant* occupant, SC4Percentage& out) {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    float wear = 0.0f;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPropProducerWear, wear)) return false;
    if (!holder->HasProperty(kPropProducerConditionCurve)) return false;
    cSC4ResponseCurve curve;
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPropProducerConditionCurve, values)) curve.SetResponseCurveData(values.data(), values.size());
    out.value = curve.GetOutputValue(wear);
    return true;
}

int cSC4PlumbingSimulator::NearestWater(int x, int z, WaterSourceType type, int radius) {
    if (type == WaterSourceType::None) return 0;
    IPlumbingWaterMap* map = host_->WaterMap();
    if (!map) return -1;
    const int width = map->Width(), height = map->Height();
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
        if (map->IsWater(cx, cz)) return spiral.turns;   // the ring the water was found on
    return -1;
}

bool cSC4PlumbingSimulator::IsNeighborConnectedToCurrentNetwork(void* regionalCity, bool markConnected) {
    for (IPlumbingNeighborConnection* connection : host_->NeighborConnections(regionalCity)) {
        int x = 0, z = 0;
        GetConnectionCell(connection, x, z);
        const bool connected = Bit1_PixelTest(bitmap_.data(), bitmapSize_, x, z);
        if (connected && markConnected) {
            connection->SetConnected(true);
            return true;
        }
        if (connected) return true;
    }
    return false;
}

void cSC4PlumbingSimulator::GetConnectionCell(IPlumbingNeighborConnection* connection, int& x, int& z) {
    if (!connection) return;
    const int cellsX = host_->CityCellsX(), cellsZ = host_->CityCellsZ();
    connection->GetCell(x, z);
    if (x < 0) x = 0;
    if (z < 0) z = 0;
    if (x >= cellsX) x = cellsX - 1;
    if (z >= cellsZ) z = cellsZ - 1;
}

// ---- streams -----------------------------------------------------------------------------------------------------

namespace {

// cSC4PlumbingSimulator::WriteWaterProducer (0x10009d00c): x/z, the lot rect, capacity, production and the
// occupant reference. percent (+0x24) and polluted (+0x28) are deliberately not serialized.
void WriteWaterProducer(cIGZOStream& stream, IPlumbingHost* host, const WaterProducer& producer, const cRetainedReference* kept) {
    stream.SetSint32(producer.x);
    stream.SetSint32(producer.z);
    stream.SetSint32(producer.rect.left);
    stream.SetSint32(producer.rect.top);
    stream.SetSint32(producer.rect.right);
    stream.SetSint32(producer.rect.bottom);
    stream.SetUint32(std::uint32_t(producer.capacity));
    stream.SetUint32(std::uint32_t(producer.production));
    if (kept) kept->Write(stream);   // the occupant a city file named (not resolved by the port yet)
    else host->WriteOccupant(stream, producer.occupant);
}

// cSC4PlumbingSimulator::ReadWaterProducer (0x10009d488).
// A city-file stream carries object references the host does not resolve yet; they are consumed so the record stays in step.
bool IsCityFileStream(cIGZIStream& stream) {
    void* db = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &db) || !db) return false;
    static_cast<cIGZUnknown*>(db)->Release();
    return true;
}
void SkipReference(cIGZIStream& stream, std::uint32_t iid) {
    void* raw = nullptr;
    SC4ReadGZSerializable(stream, iid, &raw, false);
    if (raw) static_cast<cIGZUnknown*>(raw)->Release();
}

bool ReadWaterProducer(cIGZIStream& stream, IPlumbingHost* host, WaterProducer& producer, cRetainedReference& kept) {
    stream.GetSint32(producer.x);
    stream.GetSint32(producer.z);
    stream.GetSint32(producer.rect.left);
    stream.GetSint32(producer.rect.top);
    stream.GetSint32(producer.rect.right);
    stream.GetSint32(producer.rect.bottom);
    std::uint32_t capacity = 0, production = 0;
    stream.GetUint32(capacity);
    stream.GetUint32(production);
    producer.capacity = int(capacity);
    producer.production = int(production);
    if (IsCityFileStream(stream)) kept.Read(stream);
    else host->ReadOccupant(stream, &producer.occupant);
    return !stream.GetError();
}

}  // namespace

bool cSC4PlumbingSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(4);
    if (auxGrid_) host_->WriteGrid(stream, auxGrid_); else fileAuxGrid_.Write(stream);              // +0x58
    if (supplyGrid_) host_->WriteGrid(stream, supplyGrid_); else fileSupplyGrid_.Write(stream);     // +0x60
    stream.SetUint32(std::uint32_t(cellsX_));                               // +0xd0
    stream.SetUint32(std::uint32_t(cellsZ_));                               // +0xd4
    for (std::uint8_t flag : cellFlags_) stream.SetUint8(flag);             // +0xc8
    stream.SetSint32(valueE4_);                                             // +0xe4
    stream.SetUint32(std::uint32_t(averagePollution_));                     // +0xe8
    stream.SetSint64(value1b8_);                                            // +0x1b8
    stream.SetUint32(std::uint32_t(producers_.size()));                     // +0x120
    stream.SetUint32(std::uint32_t(pollutedSources_));                      // +0x23c
    std::size_t index = 0;
    for (const WaterProducer& producer : producers_) {
        const cRetainedReference* kept = !producer.occupant && index < fileProducerOccupants_.size() ? &fileProducerOccupants_[index] : nullptr;
        WriteWaterProducer(stream, host_, producer, kept);
        ++index;
    }
    WriteWaterProducer(stream, host_, currentProducer_ ? *currentProducer_ : WaterProducer{}, currentProducer_ ? nullptr : &fileCurrentOccupant_);
    // +0x1b0 ScanningIterator: the live walk of DoIdle's flood fill is not modelled yet (PLUMBING-1), so a fresh one is written.
    stream << ScanningIterator{};
    stream.SetSint64(demand_);                                              // +0x198
    const bool keptPipes = pipes_.empty();
    stream.SetUint32(std::uint32_t(keptPipes ? filePipes_.size() : pipes_.size()));   // +0x218
    stream.SetUint32(std::uint32_t(distressedPipes_));                      // +0x21c
    stream.SetUint32(std::uint32_t(burstPipes_));                           // +0x220
    if (keptPipes) for (const cRetainedReference& pipe : filePipes_) pipe.Write(stream);
    else for (IPlumbingPipe* pipe : pipes_) host_->WritePipe(stream, pipe);  // +0x200
    fileWaterGrid_.Write(stream);   // +0xf0 int16 water grid (host-owned; the port keeps the one the file named, else a null reference)
    stream.SetSint64(capacity_);                                            // +0x178
    stream.SetSint64(consumed_);                                            // +0x180
    stream.SetSint64(imported_);                                            // +0x188
    stream.SetSint64(exported_);                                            // +0x190
    return !stream.GetError();
}

bool cSC4PlumbingSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 4) return false;
    const bool cityFile = IsCityFileStream(stream);
    cSC4SimGrid<std::uint8_t>* grid = nullptr;
    fileAuxGrid_ = cRetainedReference();
    fileSupplyGrid_ = cRetainedReference();
    fileWaterGrid_ = cRetainedReference();
    fileProducerOccupants_.clear();
    filePipes_.clear();
    if (cityFile) fileAuxGrid_.Read(stream); else host_->ReadGrid(stream, &grid);
    auxGrid_ = grid;                                                        // +0x58
    grid = nullptr;
    if (cityFile) fileSupplyGrid_.Read(stream); else host_->ReadGrid(stream, &grid);
    supplyGrid_ = grid;                                                     // +0x60
    std::uint32_t cellsX = 0, cellsZ = 0;
    stream.GetUint32(cellsX); cellsX_ = int(cellsX);                        // +0xd0
    stream.GetUint32(cellsZ); cellsZ_ = int(cellsZ);                        // +0xd4
    bitmapSize_ = std::max(cellsX_, cellsZ_);                               // +0xe0
    cellFlags_.resize(std::size_t(cellsX_) * std::size_t(cellsZ_));
    for (std::uint8_t& flag : cellFlags_) stream.GetUint8(flag);            // +0xc8
    stream.GetSint32(valueE4_);                                             // +0xe4
    std::uint32_t pollution = 0;
    stream.GetUint32(pollution); averagePollution_ = int(pollution);        // +0xe8
    stream.GetSint64(value1b8_);                                            // +0x1b8
    std::uint32_t producerCount = 0;
    stream.GetUint32(producerCount);                                        // producer count
    std::uint32_t polluted = 0;
    stream.GetUint32(polluted); pollutedSources_ = int(polluted);           // +0x23c
    producers_.clear();
    for (std::uint32_t i = 0; i < producerCount; ++i) {
        WaterProducer producer;
        fileProducerOccupants_.emplace_back();
        ReadWaterProducer(stream, host_, producer, fileProducerOccupants_.back());
        producers_.push_back(producer);
    }
    WaterProducer current;
    fileCurrentOccupant_ = cRetainedReference();
    ReadWaterProducer(stream, host_, current, fileCurrentOccupant_);        // +0x1a8 (released if unused)
    currentProducer_ = nullptr;
    if (current.occupant) current.occupant->Release();                      // drop the temporary reference
    ScanningIterator scan;                                                  // +0x1b0 (not kept, see PLUMBING-1)
    stream >> scan;
    stream.GetSint64(demand_);                                              // +0x198
    std::uint32_t pipeCount = 0;
    stream.GetUint32(pipeCount);                                            // +0x218
    std::uint32_t distressed = 0, burst = 0;
    stream.GetUint32(distressed); distressedPipes_ = int(distressed);       // +0x21c
    stream.GetUint32(burst); burstPipes_ = int(burst);                      // +0x220
    pipes_.clear();
    for (std::uint32_t i = 0; i < pipeCount; ++i) {
        IPlumbingPipe* pipe = nullptr;
        if (cityFile) { filePipes_.emplace_back(); filePipes_.back().Read(stream); continue; }
        host_->ReadPipe(stream, &pipe);
        if (pipe) pipes_.push_back(pipe);
    }
    if (cityFile) fileWaterGrid_.Read(stream); else host_->ReadWaterGrid(stream);   // +0xf0 int16 water grid
    gridsInitialized_ = true;
    countdown_ = 0;                                                         // +0x128 cleared on load
    stream.GetSint64(capacity_);                                            // +0x178
    stream.GetSint64(consumed_);                                            // +0x180
    stream.GetSint64(imported_);                                            // +0x188
    stream.GetSint64(exported_);                                            // +0x190
    return !stream.GetError();
}

}  // namespace sc4
