#include "sc4/PowerSimulator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborDeal.h"
#include "sc4/Occupant.h"
#include "sc4/RandomIterator.h"

namespace sc4 {

namespace {
// Bit1_* take (bits, size, column, row) in the order the power simulator calls them; the declared parameter
// names in gz/Bit1.h are the same slots seen from the helper's own decompile.
inline bool TestBit(const std::uint32_t* bits, int size, int x, int z) { return bits && Bit1_PixelTest(bits, size, x, z); }
inline cRZRect ToRect(const PowerRect& r) { cRZRect out; out.left = r.left; out.top = r.top; out.right = r.right; out.bottom = r.bottom; return out; }
// the original's float -> unsigned conversion (negative and NaN go to 0)
inline std::uint32_t ToUnsigned(float v) { return v > 0.0f ? static_cast<std::uint32_t>(v) : 0u; }

constexpr std::uint32_t kPlantCostPerPower = 0xc910b4ce;     // plant exemplar: generation cost per unit produced
constexpr std::uint32_t kPlantBaseExpense = 0x2a58d350;      // the plant's budget item expense
constexpr std::uint32_t kPlantExpenseByWear = 0x0911e2e7;    // curve: wear -> expense factor
constexpr std::uint32_t kPlantWearRate = 0xea1cf220;
constexpr std::uint32_t kPlantWearDistress = 0xea1cf221, kPlantWearFail = 0xea1cf222;
constexpr std::uint32_t kPlantFailureFlags = 0x6a43150f;
}  // namespace

cSC4PowerSimulator::cSC4PowerSimulator(IPowerHost* host, cRZRandom* random) : host_(host), random_(random) {}

cSC4PowerSimulator::~cSC4PowerSimulator() {
    for (PowerPlantEntry& entry : plants_) if (entry.occupant) entry.occupant->Release();
    for (cISC4Occupant* pole : poles_) if (pole) pole->Release();
    ShutdownMarkAndWireBits();
}

// ---- identity -------------------------------------------------------------------------------------------

bool cSC4PowerSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4PowerSimulator) *out = static_cast<cISC4PowerSimulator*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4PowerSimulator::Release() {
    if (refCount_ < 2) {
        delete this;   // the destructor releases the bit maps
        return 0;
    }
    return --refCount_;
}

// ---- life cycle -------------------------------------------------------------------------------------------

// The agents (6, 4 and 10 under "cSC4PowerSimulator"), the nine per-type history series, the notifications, the consumption map,
// the bit maps and the supply grid. The department budget and the supply grid themselves live in the host (it creates them when the
// city file brought none).
bool cSC4PowerSimulator::Init() {
    if (initialised_) return true;
    initialised_ = true;
    ReadSimulatorTunableValues(host_->SimulatorExemplar());
    cIGZMessageTarget2* self = this;
    host_->AddAgent(self, 6, "cSC4PowerSimulator", 2);
    host_->AddAgent(self, 4, "cSC4PowerSimulator", 2);
    host_->AddAgent(self, 10, "cSC4PowerSimulator", 2);
    for (std::uint32_t id : {0xaa120d5eu, 0xaa120d61u, 0xaa120d63u, 0xaa120d65u, 0xaa120d66u, 0xaa120d68u, 0xaa120d6au, 0xaa120d6bu,
                             0xaa120d6du})
        host_->CreateHistory(id, 2);
    for (std::uint32_t message : {kMsgPowerOccupantInserted, kMsgPowerOccupantRemoved, kMsgPowerNetworkCells, kMsgPowerTunablesChanged,
                                  kMsgPowerTerrain, kMsgPowerCheat, kMsgPowerLotStateChanged, kMsgPowerDealsChanged,
                                  kMsgPowerCityEstablished, kMsgPowerPoleCosts})
        host_->AddNotification(self, message);
    const int cellsX = host_->CityCellsX(), cellsZ = host_->CityCellsZ();
    if (consumptionMap_.size() != std::size_t(cellsX)) consumptionMap_.assign(std::size_t(cellsX), std::vector<std::int16_t>(std::size_t(cellsZ), 0));
    if (!markBits_) {   // a city file brings its maps; a new city starts empty
        flag2dc_ = 0;
        refresh_ = agePlants_ = pending_ = false;
        InitMarkAndWireBits();
        for (auto& slot : monthlyPower_) slot.fill(0);
        annualPower_.fill(0);
        state_ = 4;
    }
    return true;
}

bool cSC4PowerSimulator::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    cIGZMessageTarget2* self = this;
    for (std::uint32_t message : {kMsgPowerOccupantInserted, kMsgPowerOccupantRemoved, kMsgPowerNetworkCells, kMsgPowerTunablesChanged,
                                  kMsgPowerTerrain, kMsgPowerCheat, kMsgPowerLotStateChanged, kMsgPowerDealsChanged,
                                  kMsgPowerCityEstablished, kMsgPowerPoleCosts})
        host_->RemoveNotification(self, message);
    host_->RemoveAgent(self);
    for (PowerPlantEntry& entry : plants_) if (entry.occupant) entry.occupant->Release();
    plants_.clear();
    for (cISC4Occupant* pole : poles_) if (pole) pole->Release();
    poles_.clear();
    ShutdownMarkAndWireBits();
    consumptionMap_.clear();
    enabled_ = false;
    return true;
}

// ---- messages ---------------------------------------------------------------------------------------------
// PARTIAL: the terrain message (0x278e6003, power line geometry follows the ground), the pole cost refresh (0x26d31ec1), the
// network manager's half of 0x07699455 and the meltdown cheat (0xc2c09554) are not ported.
bool cSC4PowerSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    switch (message->GetType()) {
    case kMsgPowerCityEstablished: enabled_ = true; break;
    case kMsgPowerOccupantInserted: DoOccupantInserted(static_cast<cISC4Occupant*>(standard->GetVoid1())); break;
    case kMsgPowerOccupantRemoved: DoOccupantRemoved(static_cast<cISC4Occupant*>(standard->GetVoid1())); break;
    case kMsgPowerMonth: DoMonthlySimulation(); break;
    case kMsgPowerTick:
    case kMsgPowerTickFast: DoIdle(); break;
    case kMsgPowerTunablesChanged: ReadSimulatorTunableValues(host_->SimulatorExemplar()); break;
    case kMsgPowerNetworkCells:
        if (const auto* rect = static_cast<const std::uint32_t*>(standard->GetVoid2())) CellChanged(rect[0], rect[1], rect[2], rect[3]);
        break;
    case kMsgPowerLotStateChanged: {   // a lot reaching state 1 on a powered cell has power at once
        int x = 0, z = 0;
        if (host_->LotCentreIfStateOne(standard->GetIGZUnknown(), x, z) && IsPowered(x, z)) host_->SetLotPoweredAt(x, z);
        break;
    }
    case kMsgPowerDealsChanged:
        if (standard->GetData1() == 0) {   // a power deal changed: the next pass starts at once
            state_ = 4;
            refresh_ = true;
        }
        break;
    default: break;
    }
    return true;
}

// ---- streams ---------------------------------------------------------------------------------------------
// Version 6 (i386 Read 0x00185636 / Write 0x00185208).

bool cSC4PowerSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(6);
    host_->WriteDepartment(stream);                                     // +0x20
    stream.SetSint32(month_);                                           // +0x38
    stream.SetSint32(pass.produced);                                    // +0x3c
    stream.SetSint32(pass.consumed);                                    // +0x40
    stream.SetFloat32(shortage);                                        // +0x64
    stream.SetSint32(consumers);                                        // +0x68
    stream.SetSint32(poweredConsumers);                                 // +0x6c
    stream.SetSint32(unpoweredConsumers);                               // +0x70
    stream.SetSint32(totalConsumption);                                 // +0x74
    stream.SetSint64(poleUpkeep);                                       // +0x78
    stream.SetSint32(last.produced);                                    // +0x50
    stream.SetSint32(last.consumed);                                    // +0x54
    stream.SetSint32(last.imported);                                    // +0x58
    stream.SetSint32(last.exported);                                    // +0x5c
    stream.SetSint32(last.demand);                                      // +0x60
    host_->WriteSupplyGrid(stream);                                     // +0x80
    stream.SetUint32(std::uint32_t(consumptionMap_.size()));            // +0x84 [x][z]
    stream.SetUint32(consumptionMap_.empty() ? 0 : std::uint32_t(consumptionMap_.front().size()));
    for (const auto& column : consumptionMap_)
        for (std::int16_t value : column) stream.SetSint16(value);
    for (const auto& slot : monthlyPower_)                              // +0x90
        for (int value : slot) stream.SetSint32(value);
    for (int value : annualPower_) stream.SetSint32(value);             // +0x270
    stream.SetSint32(plantCount);                                       // +0x2e0
    for (std::int32_t value : plantCountByType) stream.SetSint32(value);   // +0x2e4
    for (const PowerPlantEntry& entry : plants_) {                      // +0x298 (plantCount entries)
        stream.SetUint32(std::uint32_t(entry.state));
        host_->WriteOccupant(stream, entry.occupant);
    }
    stream.SetUint32(std::uint32_t(poles_.size()));                     // +0x2bc
    stream.SetUint32(enabledPoles_);                                    // +0x2c0
    for (cISC4Occupant* pole : poles_) host_->WriteOccupant(stream, pole);   // +0x2a0
    stream.SetUint32(bitWords_);                                        // +0x2d0
    for (const std::uint32_t* map : {markBits_, wireBits_, poweredBits_, workBits_})
        for (std::uint32_t i = 0; i < bitWords_; ++i) stream.SetUint32(map ? map[i] : 0);
    stream.SetUint8(flag2dc_);                                          // +0x2dc
    stream.SetUint8(0);                                                 // +0x32c (always cleared)
    stream.SetSint32(value330_);                                        // +0x330
    stream.SetUint8(refresh_ ? 1 : 0);                                  // +0x334
    stream.SetUint8(agePlants_ ? 1 : 0);                                // +0x335
    stream.SetUint32(std::uint32_t(state_));                            // +0x348
    return !stream.GetError();
}

bool cSC4PowerSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 6) return false;
    host_->ReadDepartment(stream);
    stream.GetSint32(month_);
    stream.GetSint32(pass.produced);
    stream.GetSint32(pass.consumed);
    if (version < 6) {
        std::int32_t percent = 0;
        stream.GetSint32(percent);
        shortage = float(percent) * 0.01f;                              // older saves stored whole percents
    } else {
        stream.GetFloat32(shortage);
    }
    stream.GetSint32(consumers);
    stream.GetSint32(poweredConsumers);
    stream.GetSint32(unpoweredConsumers);
    stream.GetSint32(totalConsumption);
    stream.GetSint64(poleUpkeep);
    stream.GetSint32(last.produced);
    stream.GetSint32(last.consumed);
    stream.GetSint32(last.imported);
    stream.GetSint32(last.exported);
    stream.GetSint32(last.demand);
    host_->ReadSupplyGrid(stream);
    std::uint32_t columns = 0, rows = 0;
    stream.GetUint32(columns);
    stream.GetUint32(rows);
    consumptionMap_.assign(columns, std::vector<std::int16_t>(rows, 0));
    for (auto& column : consumptionMap_)
        for (std::int16_t& value : column) stream.GetSint16(value);
    for (auto& slot : monthlyPower_)
        for (int& value : slot) stream.GetSint32(value);
    for (int& value : annualPower_) stream.GetSint32(value);
    if (std::getenv("SC4_POWERDEBUG")) {
        std::fprintf(stderr, "power %p: loaded consumers %d total %d month %d produced %d consumed %d; last produced %d consumed %d imported %d exported %d demand %d\n",
                     static_cast<const void*>(this), consumers, totalConsumption, month_, pass.produced, pass.consumed, last.produced, last.consumed, last.imported, last.exported, last.demand);
        for (const auto& slot : monthlyPower_) {
            std::fprintf(stderr, "power: loaded monthly");
            for (int value : slot) std::fprintf(stderr, " %d", value);
            std::fprintf(stderr, "\n");
        }
    }
    stream.GetSint32(plantCount);
    for (std::int32_t& value : plantCountByType) stream.GetSint32(value);
    for (PowerPlantEntry& entry : plants_) if (entry.occupant) entry.occupant->Release();
    plants_.clear();
    for (int i = 0; i < plantCount; ++i) {
        PowerPlantEntry entry;
        std::uint32_t state = 0;
        stream.GetUint32(state);
        entry.state = int(state);
        host_->ReadOccupant(stream, kGZIID_cISC4Occupant, &entry.occupant);
        plants_.push_back(entry);   // (the original keeps a plant whose reference did not resolve)
    }
    std::uint32_t poleCount = 0;
    stream.GetUint32(poleCount);
    stream.GetUint32(enabledPoles_);
    for (cISC4Occupant* pole : poles_) if (pole) pole->Release();
    poles_.assign(poleCount, nullptr);
    for (cISC4Occupant*& pole : poles_) host_->ReadOccupant(stream, kPowerPoleIid, &pole);
    stream.GetUint32(bitWords_);
    ShutdownMarkAndWireBits();
    for (std::uint32_t** map : {&markBits_, &wireBits_, &poweredBits_, &workBits_}) {
        *map = new std::uint32_t[bitWords_ ? bitWords_ : 1]();
        for (std::uint32_t i = 0; i < bitWords_; ++i) stream.GetUint32((*map)[i]);
    }
    stream.GetUint8(flag2dc_);
    std::uint8_t byte = 0;
    stream.GetUint8(byte);                                              // +0x32c, cleared after reading
    stream.GetSint32(value330_);
    stream.GetUint8(byte); refresh_ = byte != 0;
    stream.GetUint8(byte); agePlants_ = byte != 0;
    std::uint32_t state = 0;
    stream.GetUint32(state);
    state_ = int(state);
    loadedPowered_.assign(poweredBits_, poweredBits_ + bitWords_);   // SC4_POWERDEBUG scores the first pass against it
    return !stream.GetError();
}

// ---- tunables --------------------------------------------------------------------------------------------

void cSC4PowerSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    neighborhoodRadius_ = 2;
    value2b8 = 1.1f;
    value35c = 1.2f;
    fundingEfficiencyCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {100.0f, 100.0f}, {120.0f, 110.0f}});
    wearByFundingCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1.0f}, {1.0f, 1.0f}});
    wearByUsageCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1.0f}, {1.0f, 1.0f}});
    curve37c.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 20.0f}, {100.0f, 100.0f}});
    poleFundingCurve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, -0.02f}, {120.0f, 0.01f}});
    strikeEfficiency = 0.5f;
    value364 = 10;
    value368 = 0.9f;
    conditionSafe = 0.6f;
    if (exemplar) {
        using H = cSCPropertyHelp;
        const auto loadCurve = [&](cSC4ResponseCurve& curve, std::uint32_t id) {
            std::vector<float> values;
            if (exemplar->HasProperty(id) && H::GetPropertyValue(exemplar, id, values)) curve.SetResponseCurveData(values.data(), values.size());
        };
        std::uint32_t radius = 0;
        if (H::GetPropertyValue(exemplar, 0xa911e2f1, radius)) neighborhoodRadius_ = int(radius);
        float f = 0;
        if (H::GetPropertyValue(exemplar, 0xe911e2fc, f)) value2b8 = f / 100.0f;
        if (H::GetPropertyValue(exemplar, 0x29121e8b, f)) conditionSafe = f * 0.01f;
        loadCurve(fundingEfficiencyCurve, 0x6911e2df);
        if (H::GetPropertyValue(exemplar, 0xe911e2d1, f)) value35c = f * 0.01f;
        if (H::GetPropertyValue(exemplar, 0x09122027, f)) strikeEfficiency = f * 0.01f;
        H::GetPropertyValue(exemplar, 0x2912202e, value364);
        if (H::GetPropertyValue(exemplar, 0x49122036, f)) value368 = f * 0.01f;
        loadCurve(curve37c, 0x0911e2e6);
        loadCurve(wearByFundingCurve, 0x0911e2e8);
        loadCurve(wearByUsageCurve, 0x0911e2e9);
        loadCurve(poleFundingCurve, 0xaa713782);
    }
    unitsPerPower = host_->OrdinancePowerFactor();
}

// ---- coordinates -------------------------------------------------------------------------------------------

void cSC4PowerSimulator::PositionToCell(float x, float z, int& cellX, int& cellZ) const {
    cellX = int((1.0f / host_->CellSizeX()) * x);
    cellZ = int((1.0f / host_->CellSizeZ()) * z);
}

void cSC4PowerSimulator::CellToPosition(int cellX, int cellZ, float& x, float& z) const {
    x = host_->CellSizeX() * float(cellX);
    z = host_->CellSizeZ() * float(cellZ);
}

namespace {
// start of a window: r cells before `v`, but not below 0 (unsigned compare as in the original)
inline int WindowStart(int v, std::uint32_t r) { return (std::uint32_t(v) <= r) ? 0 : int(std::uint32_t(v) - r); }
// end of a window: r cells after `v`, clamped to the last cell
inline int WindowEnd(int v, std::uint32_t r, int cells) { return (std::uint32_t(r) + std::uint32_t(v) < std::uint32_t(cells)) ? v + int(r) : cells - 1; }
}  // namespace

void cSC4PowerSimulator::ExpandRectToIncludeNeighborhood(PowerRect& rect) const {
    const std::uint32_t r = std::uint32_t(neighborhoodRadius_);
    rect.left = WindowStart(rect.left, r);
    rect.top = WindowStart(rect.top, r);
    rect.right = WindowEnd(rect.right, r, host_->CityCellsX());
    rect.bottom = WindowEnd(rect.bottom, r, host_->CityCellsZ());
}

void cSC4PowerSimulator::GetNeighborhoodOfPoint(int x, int z, PowerRect& out) const {
    const std::uint32_t r = std::uint32_t(neighborhoodRadius_);
    out.left = WindowStart(x, r);
    out.top = WindowStart(z, r);
    out.right = WindowEnd(x, r, host_->CityCellsX());
    out.bottom = WindowEnd(z, r, host_->CityCellsZ());
}

// ---- bit maps ------------------------------------------------------------------------------------------------

void cSC4PowerSimulator::InitMarkAndWireBits() {
    ShutdownMarkAndWireBits();
    const int cells = host_->CityCellsX() * host_->CityCellsZ();
    bitWords_ = std::uint32_t((cells + (std::uint32_t(cells >> 31) >> 27)) >> 5);
    for (std::uint32_t** map : {&markBits_, &wireBits_, &poweredBits_, &workBits_}) {
        *map = new std::uint32_t[bitWords_ ? bitWords_ : 1];
        Bit1_Clear(*map, Size());
    }
}

void cSC4PowerSimulator::ShutdownMarkAndWireBits() {
    for (std::uint32_t** map : {&markBits_, &wireBits_, &poweredBits_, &workBits_}) {
        delete[] *map;
        *map = nullptr;
    }
}

bool cSC4PowerSimulator::IsPowered(int x, int z) const { return TestBit(poweredBits_, Size(), x, z); }
bool cSC4PowerSimulator::IsWired(int x, int z) const { return TestBit(wireBits_, Size(), x, z); }
bool cSC4PowerSimulator::IsMarked(int x, int z) const { return TestBit(markBits_, Size(), x, z); }
void cSC4PowerSimulator::SetMarkBit(int x, int z) { if (markBits_) Bit1_PixelSet(markBits_, Size(), x, z); }

bool cSC4PowerSimulator::IsMarked(const PowerRect& cells) const {
    for (int x = cells.left; x <= cells.right; ++x)
        for (int z = cells.top; z <= cells.bottom; ++z)
            if (IsMarked(x, z)) return true;
    return false;
}

bool cSC4PowerSimulator::SetsWireBits(cISC4Occupant* occupant) {
    const std::uint32_t type = occupant->GetType();
    return type == kPowerOccupantBuilding || type == kPowerOccupantPole || type == kPowerOccupantNetwork || type == kPowerOccupantPylon;
}

void cSC4PowerSimulator::SetWireBits(const PowerRect& rect) { if (wireBits_) Bit1_RectSet(wireBits_, Size(), ToRect(rect)); }
void cSC4PowerSimulator::ResetWireBits(const PowerRect& rect) { if (wireBits_) Bit1_RectClear(wireBits_, Size(), ToRect(rect)); }

// @ i386 0x00181436: every occupant around the rectangle wires its cells again (a building its cells, a pole or network piece its
// cell, each spread over the neighbourhood; a flagged pylon just its cell), then the power line cells, then the supply grid.
void cSC4PowerSimulator::UpdateWireBits(const PowerRect& rect, cISC4Occupant* ignore) {
    if (!wireBits_) return;
    const int size = Size();
    PowerRect search{rect.left - neighborhoodRadius_ * 2, rect.top - neighborhoodRadius_ * 2, rect.right + neighborhoodRadius_ * 2,
                     rect.bottom + neighborhoodRadius_ * 2};
    std::vector<cISC4Occupant*> occupants;
    host_->OccupantsInRect(search, occupants);
    for (cISC4Occupant* occupant : occupants) {
        if (occupant == ignore || !SetsWireBits(occupant)) continue;
        const std::uint32_t type = occupant->GetType();
        PowerRect footprint;
        if (type == kPowerOccupantPylon || type == kPowerOccupantNetwork || type == kPowerOccupantPole) {
            int x, z;
            if (!host_->WireCell(occupant, x, z)) continue;
            footprint = {x, z, x, z};
            if (type != kPowerOccupantPylon) ExpandRectToIncludeNeighborhood(footprint);   // a pylon wires only its cell
        } else {
            SC4CellRect cells;
            occupant->GetBoundingCityCells(cells);
            footprint = {cells.x0, cells.z0, cells.x1, cells.z1};
            ExpandRectToIncludeNeighborhood(footprint);
        }
        Bit1_RectSet(wireBits_, size, ToRect(footprint));
    }
    for (int x = rect.left; x <= rect.right; ++x)
        for (int z = rect.top; z <= rect.bottom; ++z) {
            if (!host_->IsZoned(x, z)) continue;
            PowerRect around;
            GetNeighborhoodOfPoint(x, z, around);
            Bit1_RectSet(wireBits_, size, ToRect(around));
        }
    for (int x = rect.left; x <= rect.right; ++x)
        for (int z = rect.top; z <= rect.bottom; ++z) {
            const bool wired = TestBit(wireBits_, size, x, z);
            const bool powered = TestBit(poweredBits_, size, x, z);
            host_->SetSupplyValue(x, z, wired ? std::uint8_t(powered + 1) : std::uint8_t(0));
        }
}

bool cSC4PowerSimulator::CellChanged(unsigned x0, unsigned z0, unsigned x1, unsigned z1) {
    PowerRect rect{int(x0), int(z0), int(x1), int(z1)};
    ExpandRectToIncludeNeighborhood(rect);
    if (host_->IsZoned(int(x0), int(z0))) SetWireBits(rect);
    GrowPendingRect(rect);
    return true;
}

void cSC4PowerSimulator::GrowPendingRect(const PowerRect& rect) {
    if (!pending_) {
        pending_ = true;
        pendingRect_ = rect;
    } else {
        pendingRect_.left = std::min(pendingRect_.left, rect.left);
        pendingRect_.top = std::min(pendingRect_.top, rect.top);
        pendingRect_.right = std::max(pendingRect_.right, rect.right);
        pendingRect_.bottom = std::max(pendingRect_.bottom, rect.bottom);
    }
    refresh_ = true;
}

// ---- the grid ----------------------------------------------------------------------------------------------------------

void cSC4PowerSimulator::GetConnectionCell(cISC4NeighborConnection* connection, int& x, int& z) const {
    if (!connection) return;
    connection->GetConnectionCell(x, z);
    x = std::clamp(x, 0, host_->CityCellsX() - 1);
    z = std::clamp(z, 0, host_->CityCellsZ() - 1);
}

bool cSC4PowerSimulator::IsNeighborConnectedToCurrentGrid(void* city, bool markVisited, bool powerConnectionCell) {
    std::vector<cISC4NeighborConnection*> connections;
    host_->Connections(city, connections);
    for (cISC4NeighborConnection* connection : connections) {
        int x = 0, z = 0;
        GetConnectionCell(connection, x, z);
        if (!IsMarked(x, z)) continue;
        if (markVisited) connection->SetVisitedThisCycle(true);
        if (powerConnectionCell && workBits_) Bit1_PixelSet(workBits_, Size(), x, z);
        return true;
    }
    return false;
}

// @ i386 0x00182616. One grid: the cells wired to (x, z) are marked; the plants on them that have not been counted yet supply this
// grid, with the power bought from connected neighbours; the power sold to connected neighbours comes off first. The grid's 8x8 blocks
// are then served in a random order, each in full or not at all: the first block that does not fit leaves the grid short, and the
// power left over (none when the grid was short) is not consumed. The plants report their share of what the grid used.
void cSC4PowerSimulator::UpdatePowerGrid(int x, int z) {
    if (!markBits_ || !wireBits_ || !workBits_) return;
    const int size = Size();
    Bit1_Clear(markBits_, size);
    SetMarkBit(x, z);
    int zMin = z, zMax = z + 1;
    Bit1_SelectionGrow(markBits_, wireBits_, size, 600, &zMin, &zMax);
    if (zMax < zMin) { zMin = z; zMax = z + 1; }
    if (host_->CityCellsZ() <= zMax) zMax = host_->CityCellsZ() - 1;

    std::uint32_t produced = 0;
    for (PowerPlantEntry& entry : plants_) {
        if (entry.state != 0 || !entry.occupant) continue;
        float lo[3], hi[3];
        entry.occupant->GetBoundingBox(lo, hi);
        PowerRect cells;
        PositionToCell(lo[0], lo[2], cells.left, cells.top);
        PositionToCell(hi[0], hi[2], cells.right, cells.bottom);
        if (!IsMarked(cells)) continue;
        produced += GetCurrentPowerProduced(entry.occupant);
        entry.state = 1;
    }
    pass.produced += std::int32_t(produced);

    std::vector<cISC4NeighborDeal*> imports;
    host_->Deals(kPowerDealImport, imports);
    std::int32_t bought = 0;
    for (cISC4NeighborDeal* deal : imports)
        if (deal->GetAmountUsed() < deal->GetAmount() && IsNeighborConnectedToCurrentGrid(host_->DealCity(deal), true, false))
            bought += deal->GetAmount() - deal->GetAmountUsed();
    const std::uint32_t supply = std::uint32_t(bought) + ToUnsigned(float(produced) * 1.1f);   // FLOAT_00a707cc
    std::uint32_t units = ToUnsigned(float(supply) / unitsPerPower);

    std::vector<cISC4NeighborDeal*> exports;
    host_->Deals(kPowerDealExport, exports);
    for (cISC4NeighborDeal* deal : exports) {
        if (deal->GetAmountUsed() >= deal->GetAmount() || !IsNeighborConnectedToCurrentGrid(host_->DealCity(deal), false, units != 0)) continue;
        const std::int32_t open = deal->GetAmount() - deal->GetAmountUsed();
        const std::int32_t sold = open < std::int32_t(units) ? open : std::int32_t(units);
        if (sold < 1) {
            deal->SetFlags(0x20, true);
        } else {
            pass.exported += sold;
            units -= std::uint32_t(sold);
            deal->SetAmountUsed(sold + deal->GetAmountUsed());
        }
    }

    RandomIterator blocks;
    blocks.SetModular(false);
    const int lastBlockX = std::max(0, (size >> 3) - 1);
    blocks.SetRect(0, zMin >> 3, lastBlockX, std::max(zMin >> 3, zMax >> 3));
    blocks.Reset(true);
    bool short_ = false;
    int bx = 0, bz = 0;
    while (blocks.Next(bx, bz)) {
        const PowerRect block{bx * 8, bz * 8, bx * 8 + 7, bz * 8 + 7};
        if (!Bit1_RectTestAny(markBits_, size, ToRect(block))) continue;
        std::uint32_t demand = 0;
        std::int32_t count = 0;
        for (int cx = block.left; cx <= block.right; ++cx)
            for (int cz = block.top; cz <= block.bottom; ++cz) {
                if (!IsMarked(cx, cz) || std::size_t(cx) >= consumptionMap_.size() || std::size_t(cz) >= consumptionMap_[std::size_t(cx)].size()) continue;
                const std::int16_t value = consumptionMap_[std::size_t(cx)][std::size_t(cz)];
                if (0 < value) {
                    demand += std::uint32_t(value);
                    ++count;
                }
            }
        const bool fits = units != 0 && demand <= units;
        pass.demand += std::int32_t(demand);
        if (fits) {
            Bit1_RectOr(workBits_, markBits_, size, ToRect(block));
            poweredConsumers += count;
            units -= demand;
        } else {
            short_ = true;
        }
    }
    const float left = short_ ? 0.0f : float(units);
    std::uint32_t used = supply - ToUnsigned(left * unitsPerPower);
    pass.consumed += std::int32_t(used);

    for (cISC4NeighborDeal* deal : imports) {
        if (deal->GetAmountUsed() >= deal->GetAmount() || !IsNeighborConnectedToCurrentGrid(host_->DealCity(deal), false, false)) continue;
        const std::int32_t open = deal->GetAmount() - deal->GetAmountUsed();
        const std::int32_t taken = open < std::int32_t(used) ? open : std::int32_t(used);
        pass.imported += taken;
        used -= std::uint32_t(taken);
        deal->SetAmountUsed(taken + deal->GetAmountUsed());
    }
    MarkPowerPlantsAsVisited(used, std::max(used, produced));
}

// @ i386 0x00181012. The cells whose power changed (work ^ powered, called after the swap) take their new supply value (0 not
// wired, 1 wired, 2 powered); each changed 8x8 block is announced (0x8a07ae00 with the block) and the pass notes the change.
void cSC4PowerSimulator::UpdatePoweredLots() {
    if (!workBits_ || !poweredBits_) return;
    const int size = Size();
    Bit1_Xor(workBits_, poweredBits_, size);
    const int lastBlock = (size >> 3) - 1;
    for (int bz = 0; bz <= lastBlock; ++bz)
        for (int bx = 0; bx <= lastBlock; ++bx) {
            const PowerRect block{bx * 8, bz * 8, bx * 8 + 7, bz * 8 + 7};
            if (!Bit1_RectTestAny(workBits_, size, ToRect(block))) continue;
            for (int x = block.left; x <= block.right; ++x)
                for (int z = block.top; z <= block.bottom; ++z) {
                    if (!TestBit(workBits_, size, x, z)) continue;
                    const bool wired = TestBit(wireBits_, size, x, z);
                    const bool powered = TestBit(poweredBits_, size, x, z);
                    host_->SetSupplyValue(x, z, wired ? std::uint8_t(powered ? 2 : 1) : std::uint8_t(0));
                }
            host_->PostMessage(kMsgPowerCellsChanged, 0, &block);
            gridChanged_ = true;
        }
}

// @ i386 0x00182ec8: one state per call while the city is established. 0 opens a pass (the month's slot, the deals and the pass
// counters reset), 1 floods a grid from every plant not reached yet and books the generation costs (and the plants' wear once a
// month), 2 floods from the live neighbour connections the city buys over and publishes the new powered set, 3 records the
// statistics, 4 waits for a change (a refresh request starts the next pass).
//   PARTIAL: the plants' budget items (0x2a58d350 by the 0x0911e2e7 wear curve) go through the host, which keeps them as they are.
void cSC4PowerSimulator::DoIdle() {
    if (!enabled_) return;
    if (pending_) {
        ResetWireBits(pendingRect_);
        UpdateWireBits(pendingRect_, nullptr);
        pending_ = false;
    }
    switch (state_) {
    case 0: {
        month_ = month_ + 1 > 11 ? 0 : month_ + 1;
        for (int type = 0; type < kPowerTypes; ++type) {
            annualPower_[std::size_t(type)] -= monthlyPower_[std::size_t(month_)][std::size_t(type)];
            monthlyPower_[std::size_t(month_)][std::size_t(type)] = 0;
        }
        for (std::uint32_t type : {kPowerDealImport, kPowerDealExport}) {
            std::vector<cISC4NeighborDeal*> deals;
            host_->Deals(type, deals);
            for (cISC4NeighborDeal* deal : deals) deal->SetAmountUsed(0);
        }
        std::vector<cISC4NeighborConnection*> connections;
        host_->Connections(nullptr, connections);
        for (cISC4NeighborConnection* connection : connections) connection->SetVisitedThisCycle(false);
        pass = PassCounters{};
        poweredConsumers = 0;
        unpoweredConsumers = 0;
        for (PowerPlantEntry& entry : plants_) entry.state = 0;
        if (workBits_) Bit1_Clear(workBits_, Size());
        state_ = 1;
        gridChanged_ = false;
        break;
    }
    case 1: {
        std::int64_t cost = poleUpkeep;
        for (PowerPlantEntry& entry : plants_) {
            if (!entry.occupant) continue;
            if (entry.state == 0) {
                float position[3];
                entry.occupant->GetPosition(position);
                int x = 0, z = 0;
                PositionToCell(position[0], position[2], x, z);
                UpdatePowerGrid(x, z);
            }
            cISCPropertyHolder* holder = entry.occupant->AsPropertyHolder();
            float costPerPower = 0.0f, wear = 0.0f;
            std::int64_t expense = 0;
            std::int32_t current = 0;
            cSCPropertyHelp::GetPropertyValue(holder, kPlantCostPerPower, costPerPower);
            cSCPropertyHelp::GetPropertyValue(holder, kPlantBaseExpense, expense);
            cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantCurrentProduced, current);
            cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantWear, wear);
            if (holder->HasProperty(kPlantExpenseByWear)) {
                std::vector<float> values;
                cSC4ResponseCurve curve;
                if (cSCPropertyHelp::GetPropertyValue(holder, kPlantExpenseByWear, values)) curve.SetResponseCurveData(values.data(), values.size());
                host_->SetPlantExpense(entry.occupant, std::int64_t(float(expense) * curve.GetOutputValue(wear)));
            }
            cost += std::int64_t(float(current) * costPerPower);
        }
        host_->SetGenerationCost(cost);
        if (agePlants_) {
            overworkedPlants = agedPlants = distressedPlants = 0;
            agePlants_ = false;
            for (PowerPlantEntry& entry : plants_) if (entry.occupant) AgePowerPlant(entry.occupant);
        }
        state_ = 2;
        break;
    }
    case 2: {
        std::vector<cISC4NeighborDeal*> imports;
        host_->Deals(kPowerDealImport, imports);
        for (cISC4NeighborDeal* deal : imports) {
            void* city = host_->DealCity(deal);
            if (deal->GetAmountUsed() >= deal->GetAmount()) continue;
            while (cISC4NeighborConnection* connection = host_->UnvisitedConnection(city)) {
                connection->SetVisitedThisCycle(true);
                if (host_->OtherSideActive(connection)) {
                    int x = 0, z = 0;
                    GetConnectionCell(connection, x, z);
                    UpdatePowerGrid(x, z);
                }
            }
        }
        std::swap(workBits_, poweredBits_);
        UpdatePoweredLots();
        if (gridChanged_) host_->PostMessage(kMsgPowerGridChanged, 0, nullptr);
        state_ = 3;
        break;
    }
    case 3: {
        if (static const bool trace = std::getenv("SC4_POWERDEBUG") != nullptr; trace) {
            static int passes = 0;
            const int size = Size();
            if (passes == 0 && !loadedPowered_.empty() && poweredBits_) {
                int both = 0, onlySaved = 0, onlyOurs = 0;
                for (int x = 0; x < size; ++x)
                    for (int z = 0; z < host_->CityCellsZ(); ++z) {
                        const bool saved = TestBit(loadedPowered_.data(), size, x, z), ours = TestBit(poweredBits_, size, x, z);
                        both += saved && ours;
                        onlySaved += saved && !ours;
                        onlyOurs += !saved && ours;
                    }
                std::fprintf(stderr, "power: first pass vs the file: %d powered in both, %d only in the file, %d only ours\n", both, onlySaved,
                             onlyOurs);
            }
            if (passes == 0) {
                std::vector<cISC4NeighborDeal*> in, out;
                std::vector<cISC4NeighborConnection*> connections;
                host_->Deals(kPowerDealImport, in);
                host_->Deals(kPowerDealExport, out);
                host_->Connections(nullptr, connections);
                std::fprintf(stderr, "power: %zu import deals, %zu export deals, %zu power connections\n", in.size(), out.size(), connections.size());
                for (cISC4NeighborDeal* deal : in)
                    std::fprintf(stderr, "power: import deal amount %d used %d flags %08x city %p\n", deal->GetAmount(), deal->GetAmountUsed(),
                                 deal->GetFlags(), host_->DealCity(deal));
            }
            if (passes++ < 4)
                std::fprintf(stderr, "power %p: pass %d produced %d consumed %d demand %d imported %d exported %d; consumers %d powered %d; plants %zu\n",
                             static_cast<const void*>(this), passes, pass.produced, pass.consumed, pass.demand, pass.imported, pass.exported, consumers, poweredConsumers, plants_.size());
        }
        last = pass;
        host_->AddHistory(0xaa11f7ce, pass.produced);
        host_->AddHistory(0x0ab2f224, pass.imported);
        host_->AddHistory(0x4ab2f236, pass.exported);
        host_->AddHistory(0xaa11f7d2, std::int64_t(float(pass.consumed - pass.exported) * unitsPerPower));
        host_->AddHistory(0xaa11f7d5, std::int64_t(float(pass.demand) * unitsPerPower));
        host_->AddHistory(0xaa11f7d8, pass.produced - pass.consumed);
        std::vector<cISC4NeighborDeal*> exports;
        host_->Deals(kPowerDealExport, exports);
        for (cISC4NeighborDeal* deal : exports)
            if (deal->GetAmountUsed() < deal->GetAmount()) deal->SetFlags(0x20, true);
        host_->RefreshNeighborDeals();
        if (0 < pass.consumed) host_->AddHistory(0xaa11f7dc, host_->DepartmentExpense() / pass.consumed);
        std::int64_t unpoweredPercent = 0;
        if (0 < consumers) {
            const double percent = (1.0 - double(float(poweredConsumers) / float(consumers))) * 100.0;
            unpoweredPercent = std::int64_t(std::clamp(percent, 0.0, 2147483647.0));
        }
        host_->AddHistory(0xaa11fb59, unpoweredPercent);
        static constexpr std::uint32_t kTypeHistory[9] = {0xaa120d5e, 0xaa120d61, 0xaa120d63, 0xaa120d65, 0xaa120d66,
                                                          0xaa120d68, 0xaa120d6a, 0xaa120d6b, 0xaa120d6d};
        for (int type = 1; type < kPowerTypes; ++type)
            host_->AddHistory(kTypeHistory[type - 1], monthlyPower_[std::size_t(month_)][std::size_t(type)]);
        for (int type = 0; type < kPowerTypes; ++type) annualPower_[std::size_t(type)] += monthlyPower_[std::size_t(month_)][std::size_t(type)];
        shortage = pass.consumed < pass.produced ? float((pass.produced - pass.consumed) / pass.produced) : 0.0f;
        unpoweredConsumers = std::max(0, consumers - poweredConsumers);
        std::vector<cISC4NeighborConnection*> connections;
        host_->Connections(nullptr, connections);
        for (cISC4NeighborConnection* connection : connections) {
            int x = 0, z = 0;
            GetConnectionCell(connection, x, z);
            connection->SetIsActive(IsPowered(x, z));
        }
        state_ = 4;
        host_->PostMessage(kMsgPowerDealsChanged, 8, nullptr);
        break;
    }
    case 4:
        if (refresh_) {
            state_ = 0;
            refresh_ = false;
        }
        break;
    default: break;
    }
}

// @ i386 0x00183b7a. A pole wires its neighbourhood and joins the pole list (with its upkeep), a network piece and a flagged pylon
// wire theirs; a building wires its cells' neighbourhood and is a consumer (its draw goes into the consumption map at its centre and
// its lot is powered at once when that cell is) or a plant (into the plant list and the plant budget). Each asks for a new pass.
void cSC4PowerSimulator::DoOccupantInserted(cISC4Occupant* occupant) {
    if (!initialised_ || !occupant) return;
    const std::uint32_t type = occupant->GetType();
    if (type == kPowerOccupantPole || type == kPowerOccupantNetwork || type == kPowerOccupantPylon) {
        int x = 0, z = 0;
        if (!host_->WireCell(occupant, x, z)) return;
        PowerRect rect{x, z, x, z};
        if (type != kPowerOccupantPylon) ExpandRectToIncludeNeighborhood(rect);
        SetWireBits(rect);
        if (type == kPowerOccupantPole) {
            occupant->AddRef();
            poles_.push_back(occupant);
            std::int64_t upkeep = 0;
            cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPowerPoleUpkeep, upkeep);
            poleUpkeep += upkeep;
        }
        refresh_ = true;
        return;
    }
    if (type != kPowerOccupantBuilding) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    float lo[3], hi[3];
    occupant->GetBoundingBox(lo, hi);
    int cx = 0, cz = 0;
    PositionToCell((lo[0] + hi[0]) * 0.5f, (lo[2] + hi[2]) * 0.5f, cx, cz);
    SC4CellRect cells;
    occupant->GetBoundingCityCells(cells);
    PowerRect rect{cells.x0, cells.z0, cells.x1, cells.z1};
    ExpandRectToIncludeNeighborhood(rect);
    SetWireBits(rect);
    std::uint32_t consumed = 0, maxProduced = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPowerConsumed, consumed);
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantMaxProduced, maxProduced);
    if (consumed != 0 && maxProduced < consumed) {
        if (std::size_t(cx) < consumptionMap_.size() && std::size_t(cz) < consumptionMap_[std::size_t(cx)].size())
            consumptionMap_[std::size_t(cx)][std::size_t(cz)] = std::int16_t(consumptionMap_[std::size_t(cx)][std::size_t(cz)] + std::int16_t(consumed));
        ++consumers;
        totalConsumption += std::int32_t(consumed);
        if (IsPowered(cx, cz)) host_->SetOccupantLotPowered(occupant);
    }
    if (maxProduced == 0 || maxProduced < consumed) return;
    std::int32_t powerType = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantPowerType, powerType);
    occupant->AddRef();
    plants_.push_back(PowerPlantEntry{occupant, 0});
    ++plantCount;
    if (std::uint32_t(powerType) < std::uint32_t(kPowerTypes)) ++plantCountByType[std::size_t(powerType)];
    refresh_ = true;
    host_->AddPlantToBudget(occupant);
}

// @ i386 0x00185bba. The removed occupant's cells (with the neighbourhood) are wired again from what is left at the next pass; a
// consumer leaves the consumption map, a plant the plant list and the plant budget, a pole the pole list.
void cSC4PowerSimulator::DoOccupantRemoved(cISC4Occupant* occupant) {
    if (!initialised_ || !occupant) return;
    const std::uint32_t type = occupant->GetType();
    if (type == kPowerOccupantPole || type == kPowerOccupantNetwork || type == kPowerOccupantPylon) {
        int x = 0, z = 0;
        if (!host_->WireCell(occupant, x, z)) return;
        PowerRect rect{x, z, x, z};
        if (type != kPowerOccupantPylon) ExpandRectToIncludeNeighborhood(rect);
        GrowPendingRect(rect);
        if (type == kPowerOccupantPole) {
            for (auto it = poles_.begin(); it != poles_.end();) {
                if (*it == occupant) {
                    (*it)->Release();
                    it = poles_.erase(it);
                } else {
                    ++it;
                }
            }
            std::int64_t upkeep = 0;
            cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPowerPoleUpkeep, upkeep);
            poleUpkeep -= upkeep;
        }
        return;
    }
    if (type != kPowerOccupantBuilding) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    float lo[3], hi[3];
    occupant->GetBoundingBox(lo, hi);
    int cx = 0, cz = 0;
    PositionToCell((lo[0] + hi[0]) * 0.5f, (lo[2] + hi[2]) * 0.5f, cx, cz);
    std::uint32_t consumed = 0, maxProduced = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPowerConsumed, consumed);
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantMaxProduced, maxProduced);
    if (consumed != 0 && maxProduced < consumed) {
        if (std::size_t(cx) < consumptionMap_.size() && std::size_t(cz) < consumptionMap_[std::size_t(cx)].size())
            consumptionMap_[std::size_t(cx)][std::size_t(cz)] = std::int16_t(consumptionMap_[std::size_t(cx)][std::size_t(cz)] - std::int16_t(consumed));
        consumers = std::max(0, consumers - 1);
        totalConsumption = std::max(0, totalConsumption - std::int32_t(consumed));
    }
    SC4CellRect cells;
    occupant->GetBoundingCityCells(cells);
    PowerRect rect{cells.x0, cells.z0, cells.x1, cells.z1};
    ExpandRectToIncludeNeighborhood(rect);
    GrowPendingRect(rect);
    auto plant = std::find_if(plants_.begin(), plants_.end(), [&](const PowerPlantEntry& entry) { return entry.occupant == occupant; });
    if (plant == plants_.end()) return;
    host_->RemovePlantFromBudget(occupant);
    std::int32_t powerType = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantPowerType, powerType);
    plant->occupant->Release();
    plants_.erase(plant);
    if (std::uint32_t(powerType) < std::uint32_t(kPowerTypes)) --plantCountByType[std::size_t(powerType)];
    --plantCount;
    refresh_ = true;
}

// @ i386 0x001821de. Once a month: the plant wears by its wear rate, scaled by the department's funding and by how much of its
// output it delivers. Past 100% use it counts as overworked, from 0.8 wear as aged; at its first wear limit its lot is distressed.
//   PARTIAL: a plant past its failure limit with failure flags (0x6a43150f) is not demolished (the disaster and its pollution burst).
void cSC4PowerSimulator::AgePowerPlant(cISC4Occupant* plant) {
    cISCPropertyHolder* holder = plant->AsPropertyHolder();
    float wear = 0.0f, rate = 0.0f, distressAt = 99.0f, failAt = 99.0f;
    std::int32_t maxProduced = 0, current = 0;
    std::uint32_t failureFlags = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantWear, wear);
    cSCPropertyHelp::GetPropertyValue(holder, kPlantWearRate, rate);
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantMaxProduced, maxProduced);
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantCurrentProduced, current);
    cSCPropertyHelp::GetPropertyValue(holder, kPlantWearDistress, distressAt);
    cSCPropertyHelp::GetPropertyValue(holder, kPlantWearFail, failAt);
    cSCPropertyHelp::GetPropertyValue(holder, kPlantFailureFlags, failureFlags);
    maxProduced = std::int32_t(float(maxProduced) * GetCurrentPlantConditionPercent(plant));
    const float use = maxProduced < 1 || current < 1 ? 0.0f : float(current) / float(maxProduced);
    wear += wearByUsageCurve.GetOutputValue(use) * (wearByFundingCurve.GetOutputValue(host_->DepartmentFundingFraction()) * rate);
    cSCPropertyHelp::SetPropertyValue(holder, kPowerPlantWear, wear);
    if (1.0f < use) ++overworkedPlants;
    if (0.8 <= double(wear)) ++agedPlants;
    if (failAt <= wear || distressAt <= wear) {
        if (wear < failAt || failureFlags == 0) {
            if (distressAt <= wear) {
                ++distressedPlants;
                host_->DistressPlantLot(plant);
            }
        }
    }
}

// ---- plants ---------------------------------------------------------------------------------------------------------

std::uint32_t cSC4PowerSimulator::GetCurrentPowerProduced(cISC4Occupant* plant) {
    if (!plant) return 0;
    std::uint32_t maxProduced = 0;
    if (!cSCPropertyHelp::GetPropertyValue(plant->AsPropertyHolder(), kPowerPlantMaxProduced, maxProduced)) return maxProduced;
    return ToUnsigned(float(maxProduced) * GetCurrentPlantEfficiencyPercent(plant));
}

float cSC4PowerSimulator::GetCurrentPlantConditionPercent(cISC4Occupant* plant) {
    if (!plant) return 0.0f;
    cISCPropertyHolder* holder = plant->AsPropertyHolder();
    if (!holder->HasProperty(kPowerPlantConditionCurve)) return 0.0f;
    cSC4ResponseCurve curve;
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantConditionCurve, values)) curve.SetResponseCurveData(values.data(), values.size());
    float wear = 0.0f;
    cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantWear, wear);
    return curve.GetOutputValue(wear);
}

float cSC4PowerSimulator::GetCurrentPlantEfficiencyPercent(cISC4Occupant* plant) {
    if (!plant) return 0.0f;
    const PowerPlantBudgetEntry entry = host_->GetPlantBudgetEntry(plant);
    float funded = 0.0f;
    if (entry.tracked) funded = fundingEfficiencyCurve.GetOutputValue(entry.funding * 100.0f) * 0.01f;
    return GetCurrentPlantConditionPercent(plant) * funded;
}

bool cSC4PowerSimulator::GetPowerPlantFundingPercentage(cISC4Occupant* plant, SC4Percentage& out) {
    const PowerPlantBudgetEntry entry = host_->GetPlantBudgetEntry(plant);
    if (!entry.tracked) return false;
    out.value = entry.funding;
    return true;
}

bool cSC4PowerSimulator::GetMonthlyPowerGenerationCost(cISC4Occupant* plant, std::int64_t& cost) {
    const PowerPlantBudgetEntry entry = host_->GetPlantBudgetEntry(plant);
    if (!entry.tracked) return false;
    cost = entry.monthlyCost;
    return true;
}

void cSC4PowerSimulator::FindPowerPlants(cISC4Occupant** leastFunded, SC4Percentage* funding, cISC4Occupant** mostWorn, int* wearPercent) {
    if (plants_.empty()) return;
    float lowestFunding = 2.0f, highestWear = 0.0f;
    cISC4Occupant* lowest = nullptr;
    cISC4Occupant* worn = nullptr;
    for (const PowerPlantEntry& entry : plants_) {
        if (!entry.occupant) continue;
        cISCPropertyHolder* holder = entry.occupant->AsPropertyHolder();
        const PowerPlantBudgetEntry budget = host_->GetPlantBudgetEntry(entry.occupant);
        if (budget.tracked && budget.hasExplicitFunding && budget.funding < lowestFunding) {
            lowestFunding = budget.funding;
            lowest = entry.occupant;
        }
        float wear = 0.0f, limitA = 99.0f, limitB = 99.0f;
        cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantWear, wear);
        cSCPropertyHelp::GetPropertyValue(holder, kPlantWearDistress, limitA);
        cSCPropertyHelp::GetPropertyValue(holder, kPlantWearFail, limitB);
        wear = wear / std::max(limitA, limitB);
        if (highestWear < wear) {
            highestWear = wear;
            worn = entry.occupant;
        }
    }
    if (mostWorn && worn) {
        *mostWorn = worn;
        *wearPercent = int(highestWear * 100.0f);
    }
    if (leastFunded && lowest) {
        *leastFunded = lowest;
        if (funding) funding->value = lowestFunding;
    }
}

void cSC4PowerSimulator::MarkPowerPlantsAsVisited(std::uint32_t used, std::uint32_t capacity) {
    float scale = 0.0f;
    if (0 < pass.produced) scale = float(used) / (capacity == 0 ? 1.0f : float(capacity));
    for (PowerPlantEntry& entry : plants_) {
        if (entry.state != 1 || !entry.occupant) continue;
        cISCPropertyHolder* holder = entry.occupant->AsPropertyHolder();
        const std::uint32_t produced = GetCurrentPowerProduced(entry.occupant);
        std::int32_t type = 0;
        cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantPowerType, type);
        const int current = int(float(produced) * scale);
        cSCPropertyHelp::SetPropertyValue(holder, kPowerPlantCurrentProduced, std::int32_t(current));
        if (std::uint32_t(type) < std::uint32_t(kPowerTypes)) monthlyPower_[std::size_t(month_)][std::size_t(type)] += current;
        entry.state = 2;
    }
}

bool cSC4PowerSimulator::GetOccupantPowerProduced(cISC4Occupant* plant, std::uint32_t& produced, std::uint32_t& currentProduced) {
    cISCPropertyHolder* holder = plant->AsPropertyHolder();
    if (!holder->HasProperty(kPowerPlantMaxProduced)) return false;
    produced = GetCurrentPowerProduced(plant);
    currentProduced = 0;
    if (holder->HasProperty(kPowerPlantCurrentProduced)) {
        std::uint32_t v = 0;
        cSCPropertyHelp::GetPropertyValue(holder, kPowerPlantCurrentProduced, v);
        currentProduced = v;
    }
    return true;
}

bool cSC4PowerSimulator::GetOccupantMaxPowerProduced(cISC4Occupant* plant, std::uint32_t& out) {
    return cSCPropertyHelp::GetPropertyValue(plant->AsPropertyHolder(), kPowerPlantMaxProduced, out);
}

void cSC4PowerSimulator::GetPowerPlants(std::list<cISC4Occupant*>& out) const {
    for (const PowerPlantEntry& entry : plants_) out.push_back(entry.occupant);
}

// ---- poles and funding -----------------------------------------------------------------------------------------------

void cSC4PowerSimulator::ApplyPoleFunding(const SC4Percentage& funding) {
    poleFunding_ -= poleFundingCurve.GetOutputValue(funding.value * 100.0f);
    if (poleFunding_ > 1.0f) poleFunding_ = 1.0f;
    else if (poleFunding_ < 0.0f) poleFunding_ = 0.0f;
    const std::uint32_t target = std::uint32_t(float(poles_.size()) * poleFunding_);
    if (enabledPoles_ == target) return;
    std::vector<cISC4Occupant*> shuffled = poles_;
    for (std::size_t i = shuffled.size(); i > 1; --i) std::swap(shuffled[i - 1], shuffled[random_->RandomUint32Uniform(std::uint32_t(i))]);
    for (cISC4Occupant* pole : shuffled) {
        if (enabledPoles_ == target) break;
        if (!pole) continue;
        if (!host_->PoleEnabled(pole)) {
            if (enabledPoles_ < target) { host_->SetPoleEnabled(pole, true); ++enabledPoles_; }
        } else if (target < enabledPoles_) {
            host_->SetPoleEnabled(pole, false);
            --enabledPoles_;
        }
    }
}

// ---- monthly statistics --------------------------------------------------------------------------------------------------

// @ i386 0x001842b0. The ordinance factor, the month's slot taken out of the annual sums (and added back: the slot was just cleared),
// the shortage, a refresh and the plants' wear at the next pass, then the pole funding.
void cSC4PowerSimulator::DoMonthlySimulation() {
    unitsPerPower = host_->OrdinancePowerFactor();
    month_ = month_ + 1 > 11 ? 0 : month_ + 1;
    auto& slot = monthlyPower_[std::size_t(month_)];
    for (int type = 0; type < kPowerTypes; ++type) {
        annualPower_[std::size_t(type)] -= slot[std::size_t(type)];
        slot[std::size_t(type)] = 0;
    }
    shortage = pass.produced < pass.consumed && 0 < pass.produced ? float((pass.produced - pass.consumed) / pass.produced) : 0.0f;
    for (int type = 0; type < kPowerTypes; ++type) annualPower_[std::size_t(type)] += slot[std::size_t(type)];
    refresh_ = true;
    agePlants_ = true;
    ApplyPoleFunding(SC4Percentage{host_->DepartmentFundingFraction()});
}

int cSC4PowerSimulator::GetAnnualConsumption() const {
    int sum = 0;
    for (int v : annualPower_) sum += v;
    return sum;
}

}  // namespace sc4
