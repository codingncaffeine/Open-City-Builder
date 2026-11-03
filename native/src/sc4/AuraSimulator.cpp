// cSC4AuraSimulator — see AuraSimulator.h for the class documentation (layout, state machine, quirks).
#include "sc4/AuraSimulator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "rz/cRZMessage2Standard.h"
#include "sc4/GameDataRegistryHelpers.h"
#include "sc4/ProtectionSimulator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SimGridOps.h"

namespace sc4 {

namespace {

IAuraHost* g_host = nullptr;

// sDeveloperTypeToBaseAuraMap @ 0x100b038d0: three entries plus the type-0 terminator.
AuraTypeEntry g_auraTypeTable[kAuraTypeEntryCount + 1] = {
    {0x1010u, 0, 0, 0, 0},
    {0x1020u, 1, 1, 1, 0},
    {0x1030u, 2, 2, 2, 0},
    {0u, 0, 0, 0, 0},
};

// The original's rounding of `scaled` with the sign of `delta` (0x10021a872 negative / 0x10021a9af positive
// path): the compiler turns the float->int conversion into the 12582912 magic, so the net effect is
// round-away-from-zero with an exact-integer fix-up. Valid for |scaled| < 2^22 (aura deltas are a few hundred).
int AuraRoundStep(int delta, float scaled) {
    constexpr float kMagic = 12582912.0f;
    const float biased = delta < 0 ? scaled - 0.5f + kMagic : scaled + 0.5f + kMagic;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof(bits));
    const int rounded = static_cast<int>(static_cast<std::int32_t>(bits) - 0x4b400000);
    const int neighbour = delta < 0 ? rounded + 1 : rounded - 1;
    return scaled == static_cast<float>(neighbour) ? neighbour : rounded;
}

// The original's float->int conversion through the 12582912 magic (round to nearest, ties to even), used by
// AddRadiusEffect for both the radius bound and the falloff value (0x1000d7292 / 0x1000d7494).
int RoundToInt(float value) {
    const float biased = value + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof(bits));
    return static_cast<int>(static_cast<std::int32_t>(bits) - 0x4b400000);
}

template <class T>
T& CellAt(std::vector<std::vector<T>>& data, int x, int z) {
    return data[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
}

// Shutdown releases every grid through its COM view (grid slots 0x20 Shutdown and 0x10 Release).
template <class T>
void ReleaseGrid(cSC4SimGrid<T>*& grid) {
    if (grid) {
        grid->Release();
        grid = nullptr;
    }
}

// Reads one grid back into a member, releasing the previous grid (the original overwrites the pointer, leaking it;
// the port replaces it - see the header).
template <class T>
void ReplaceGrid(cSC4SimGrid<T>*& slot, cIGZSerializable* raw) {
    auto* replacement = dynamic_cast<cSC4SimGrid<T>*>(raw);
    if (raw && !replacement) {
        raw->Release();
        return;
    }
    if (slot) slot->Release();
    slot = replacement;
}

}  // namespace

AuraTypeEntry* AuraTypeTable() { return g_auraTypeTable; }

IAuraHost* AuraHost() { return g_host; }
void SetAuraHost(IAuraHost* host) { g_host = host; }

// @ 0x1000d7292. The original reads the distances from a 16x16 lookup table for radii below 16; the table holds the
// same floats the else branch computes, so one path serves both.
void AddRadiusEffect(std::vector<std::vector<std::int8_t>>& grid, int width, int height, int cx, int cz, float radius,
                     float peak, float edge) {
    if (!(radius > 0.0f)) return;
    const int r = RoundToInt(radius);
    const int x0 = std::max(cx - r, 0);
    const int x1 = std::min(cx + r, width - 1);
    const int z0 = std::max(cz - r, 0);
    const int z1 = std::min(cz + r, height - 1);
    if (x0 > x1 || z0 > z1) return;
    const float slope = (peak - edge) / radius;
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            const float dx = static_cast<float>(x - cx), dz = static_cast<float>(z - cz);
            const float distance = std::sqrt(dz * dz + dx * dx);
            if (!(distance < radius)) continue;
            const float falloff = distance * slope;
            int value = RoundToInt(peak - falloff) +
                        grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
            if (value < -0x80) value = -0x80;
            if (value > 0x7f) value = 0x7f;
            grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = static_cast<std::int8_t>(value);
        }
    }
}

//==================================================================================================
// construction / COM
//==================================================================================================

// @ 0x10021801e. +0x18 and +0x1c are zero, the random generator is seeded with 0xffffffff ("from the clock"), the
// tract size is 4, the hotspot tunables are {196, 8, 16}, the smoothing / decay factors 0.6 / 0.4 and the twelve
// tax-rate slots -1.0.
cSC4AuraSimulator::cSC4AuraSimulator() : random_(0xffffffffu) {
    std::fill(taxRates_, taxRates_ + 12, -1.0f);
    pollutionCurves_.resize(4, nullptr);
}

// @ 0x10021821c. The original frees the hotspot vector, the hotspot list and the landmark map nodes (it does not
// release their grids), and the landmark grid's storage. Curves and services are only freed by Shutdown, so an
// object destroyed without Init/Shutdown leaks them - kept.
cSC4AuraSimulator::~cSC4AuraSimulator() { landmarkMaps_.clear(); }

// @ 0x1002182e0.
bool cSC4AuraSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (static_cast<std::int32_t>(iid) < 1) {
        if (iid == kGZIID_cISC4AuraSimulatorSerializable) {
            *out = static_cast<cIGZSerializable*>(this);
        } else if (iid == kGZIID_cISC4AuraSimulator) {
            *out = static_cast<cISC4AuraSimulator*>(this);
        } else {
            return false;
        }
    } else if (iid == 1) {
        *out = static_cast<cISC4AuraSimulator*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// @ 0x100218340 (the +8 / +0x10 view thunks touch the same count).
std::uint32_t cSC4AuraSimulator::AddRef() { return ++refCount_; }

// @ 0x10021836a: below 2 destroys the object (Shutdown, then the deleting destructor).
std::uint32_t cSC4AuraSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

//==================================================================================================
// grids
//==================================================================================================

cSC4SimGrid<std::int8_t>* cSC4AuraSimulator::MakeInt8Grid(std::uint32_t instanceId, std::int32_t tractSize) {
    auto* grid = new cSC4SimGrid<std::int8_t>();
    grid->Init();                 // grid byte +0x10 = 1
    grid->AddRef();               // grid +0x14 (the original stores 1)
    grid->SetTractSize(tractSize);
    grid->SetInstanceID(instanceId);
    return grid;
}

cSC4SimGrid<std::int16_t>* cSC4AuraSimulator::MakeInt16Grid(std::uint32_t instanceId, std::int32_t tractSize) {
    auto* grid = new cSC4SimGrid<std::int16_t>();
    grid->Init();
    grid->AddRef();
    grid->SetTractSize(tractSize);
    grid->SetInstanceID(instanceId);
    return grid;
}

//==================================================================================================
// Init / Shutdown
//==================================================================================================

// @ 0x100218422. Creates the nine grids (the aura, cell, smoothed, transient and vehicle int8 grids, the park
// effect, park, landmark and landmark-work int16 grids), stores the city and the building development simulator,
// reads the tunables, subscribes to the five messages, adds the four simulator agents and publishes the three
// game-data entries ("aura" node, "add_transient_effect" and "end_strike" commands).
bool cSC4AuraSimulator::Init() {
    if (initialized_) return true;
    // 0x100b246fc: the original sets a file-scope "static initialised" byte that nothing reads (quirk).
    pollutionCurves_.resize(4, nullptr);
    idleState_ = 0;
    lotIterator_ = nullptr;

    if (g_host && !city_) {
        city_ = g_host->City();
        if (city_) city_->AddRef();
    }
    if (g_host && !buildingDevelopment_) {
        buildingDevelopment_ = g_host->BuildingDevelopment();
        if (buildingDevelopment_) buildingDevelopment_->AddRef();
    }

    if (!auraGrid_) {
        auraGrid_ = MakeInt8Grid(kAuraGridInstance, tractSize_);
        auraGrid_->SetTractValues(0);
    }
    if (!smoothedGrid_) {
        smoothedGrid_ = MakeInt8Grid(kAuraSmoothedGridInstance, tractSize_);
        smoothedGrid_->SetTractValues(-0x80);
    }
    if (!transientGrid_) {
        transientGrid_ = MakeInt8Grid(kAuraTransientGridInstance, tractSize_);
        transientGrid_->SetTractValues(0);
    }
    if (!cellGrid_) {
        // The per-cycle work grid is a cell grid: tract size 1 only.
        cellGrid_ = MakeInt8Grid(kAuraCellGridInstance, 1);
    }
    if (!parkGrid_) parkGrid_ = MakeInt16Grid(kAuraParkGridInstance, tractSize_);
    if (!parkEffectGrid_) {
        parkEffectGrid_ = MakeInt16Grid(kAuraParkEffectGridInstance, tractSize_);
        parkEffectGrid_->SetTractValues(0);
    }
    if (!landmarkGrid_) {
        landmarkGrid_ = MakeInt16Grid(kAuraLandmarkGridInstance, tractSize_);
        landmarkGrid_->SetTractValues(0);
    }
    if (!landmarkWorkGrid_) {
        landmarkWorkGrid_ = MakeInt16Grid(kAuraLandmarkGridInstance, tractSize_);
        landmarkWorkGrid_->SetTractValues(0);
    }
    if (!vehicleGrid_) {
        vehicleGrid_ = MakeInt8Grid(kAuraVehicleGridInstance, tractSize_);
        vehicleGrid_->SetTractValues(0);
    }

    idleState_ = 1;
    ReadSimulatorTunableValues();

    if (g_host && !messageServer_) {
        messageServer_ = g_host->MessageServer();
        if (messageServer_) {
            messageServer_->AddRef();
            cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
            messageServer_->AddNotification(target, kMessageOccupantInsertedAura);
            messageServer_->AddNotification(target, kMessageOccupantRemovedAura);
            messageServer_->AddNotification(target, kAuraMessageOrdinanceChanged);
            messageServer_->AddNotification(target, kAuraMessageReloadTunables);
            messageServer_->AddNotification(target, kAuraMessageLotsChanged);
        }
    }
    if (g_host && g_host->SimulatorPresent()) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        g_host->AddSimulatorAgent(target, 0, "cSC4AuraSimulator", 2);
        g_host->AddSimulatorAgent(target, 10, "cSC4AuraSimulator", 2);
        g_host->AddSimulatorAgent(target, 4, "cSC4AuraSimulator", 2);
        g_host->AddSimulatorAgent(target, 6, "cSC4AuraSimulator", 2);
    }
    if (g_host) {
        if (cISC4GameDataRegistry* registry = g_host->GameDataRegistry()) {
            nsISC4GameDataRegistry::AddNode<cSC4AuraSimulator, cIGZUnknown>(this, "aura", nullptr, 0, nullptr,
                                                                          registry);
            // The original registers the command function pointer itself as the entry handle and `this` as the
            // context; the handler resolves the context from the handle it is called with.
            void* transientHandle = reinterpret_cast<void*>(&cSC4AuraSimulator::sAddTransientEffect);
            nsISC4GameDataRegistry::AddValue(transientHandle, "add_transient_effect",
                                             &cSC4AuraSimulator::sAddTransientEffect, this, 0, this, registry);
            void* strikeHandle = reinterpret_cast<void*>(&cSC4AuraSimulator::sEndStrike);
            nsISC4GameDataRegistry::AddValue(strikeHandle, "end_strike", &cSC4AuraSimulator::sEndStrike, this, 0,
                                             this, registry);
        }
    }
    initialized_ = true;
    return true;
}

// @ 0x100219384. Removes the game-data node, drops the message target, releases every grid, the landmark map, the
// lot iterator, the services and every response curve.
bool cSC4AuraSimulator::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (g_host) {
        if (cISC4GameDataRegistry* registry = g_host->GameDataRegistry()) {
            cIGZUnknown* node = nullptr;
            QueryInterface(1, reinterpret_cast<void**>(&node));
            registry->RemoveChildEntry(node);
            if (node) node->Release();
        }
    }
    ReleaseServices();
    return true;
}

void cSC4AuraSimulator::ReleaseServices() {
    DeleteCurve(riotCurve_);                                   // +0x1e8, freed first
    if (city_) city_->RemoveMessageTarget(static_cast<cIGZMessageTarget2*>(this));
    if (messageServer_) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        messageServer_->RemoveNotification(target, kMessageOccupantInsertedAura);
        messageServer_->RemoveNotification(target, kMessageOccupantRemovedAura);
        messageServer_->RemoveNotification(target, kAuraMessageOrdinanceChanged);
        messageServer_->RemoveNotification(target, kAuraMessageReloadTunables);
        messageServer_->RemoveNotification(target, kAuraMessageLotsChanged);
        messageServer_->Release();
        messageServer_ = nullptr;
    }
    ReleaseGrid(auraGrid_);
    ReleaseGrid(smoothedGrid_);
    ReleaseGrid(cellGrid_);
    ReleaseGrid(transientGrid_);
    ReleaseGrid(parkGrid_);
    ReleaseGrid(parkEffectGrid_);
    ReleaseGrid(landmarkGrid_);
    ReleaseGrid(landmarkWorkGrid_);
    for (auto& entry : landmarkMaps_) {
        if (entry.second) entry.second->Release();
    }
    landmarkMaps_.clear();
    ReleaseGrid(vehicleGrid_);
    if (lotIterator_) {
        lotIterator_->Release();
        lotIterator_ = nullptr;
    }
    if (buildingDevelopment_) {
        buildingDevelopment_->Release();
        buildingDevelopment_ = nullptr;
    }
    if (city_) {
        city_->Release();
        city_ = nullptr;
    }
    DeleteCurve(parkCurve_);
    DeleteCurve(residentialCurve138_);
    DeleteCurve(residentialCurve140_);
    DeleteCurve(crimeCurve_);
    DeleteCurve(trafficTripCurve_);
    DeleteCurve(trafficCurve_);
    DeleteCurve(taxChangeCurve_);
    DeleteCurve(landValueCurve_);
    for (auto*& curve : pollutionCurves_) DeleteCurve(curve);
}

//==================================================================================================
// tunables
//==================================================================================================

void cSC4AuraSimulator::DeleteCurve(cSC4ResponseCurve*& curve) {
    delete curve;
    curve = nullptr;
}

// The body of the original's ReadCurveProperty @ 0x100219ed7: a property of the exemplar, read as interleaved x/y
// floats. A missing property deletes the curve (as the growth developer does).
void cSC4AuraSimulator::ReadCurveProperty(cISCPropertyHolder* holder, std::uint32_t propertyId,
                                         cSC4ResponseCurve*& curve) {
    cISCProperty* property = holder ? holder->GetProperty(propertyId) : nullptr;
    if (!property) {
        DeleteCurve(curve);
        return;
    }
    if (!curve) curve = new cSC4ResponseCurve();
    const cIGZVariant* value = property->GetPropertyValue();
    curve->SetResponseCurveData(value->RefFloat32(), value->GetCount());
}

// @ 0x100218c72. Reads the simulator exemplar {0x6534284a, 0xe7e2c2db, 0xaa023079}; without one every default
// (and every previously read value) stays.
void cSC4AuraSimulator::ReadSimulatorTunableValues() {
    cISCPropertyHolder* holder = nullptr;
    if (!g_host || !g_host->GetTuningProperties(&holder) || !holder) return;

    ReadCurveProperty(holder, kAuraPropertyParkCurve, parkCurve_);
    ReadCurveProperty(holder, kAuraPropertyResidentialCurve138, residentialCurve138_);
    ReadCurveProperty(holder, kAuraPropertyResidentialCurve140, residentialCurve140_);
    ReadCurveProperty(holder, kAuraPropertyCrimeCurve, crimeCurve_);
    ReadCurveProperty(holder, kAuraPropertyTrafficTripCurve, trafficTripCurve_);
    ReadCurveProperty(holder, kAuraPropertyTrafficCurve, trafficCurve_);
    ReadCurveProperty(holder, kAuraPropertyTaxChangeCurve, taxChangeCurve_);
    ReadCurveProperty(holder, kAuraPropertyLandValueCurve, landValueCurve_);
    if (pollutionCurves_.size() < 4) pollutionCurves_.resize(4, nullptr);
    for (std::uint32_t i = 0; i < 4; ++i) {
        ReadCurveProperty(holder, kAuraPropertyPollutionCurve0 + i, pollutionCurves_[i]);
    }

    auraSmoothing_ = 0.6f;
    transientDecay_ = 0.4f;
    cSCPropertyHelp::GetPropertyValue(holder, kAuraPropertySmoothing, auraSmoothing_);
    cSCPropertyHelp::GetPropertyValue(holder, kAuraPropertyTransientDecay, transientDecay_);
    cSCPropertyHelp::GetPropertyValue(holder, kAuraPropertyBaseAuraResidential, baseAuraResidential_);
    cSCPropertyHelp::GetPropertyValue(holder, kAuraPropertyBaseAuraCommercial, baseAuraCommercial_);
    cSCPropertyHelp::GetPropertyValue(holder, kAuraPropertyBaseAuraIndustrial, baseAuraIndustrial_);

    // Push the three base auras into the developer-type table (aura type 0/1/2 -> +0x1b0/+0x1b4/+0x1b8).
    for (AuraTypeEntry* entry = g_auraTypeTable; entry->type != 0; ++entry) {
        switch (entry->auraType) {
        case 0: entry->baseAura = baseAuraResidential_; break;
        case 1: entry->baseAura = baseAuraCommercial_; break;
        case 2: entry->baseAura = baseAuraIndustrial_; break;
        default: break;
        }
    }

    for (int i = 0; i < 4; ++i) {
        transientRadius_[i] = 0.0f;
        transientMagnitude_[i] = 0.0f;
        std::vector<float> pair;
        if (cSCPropertyHelp::GetPropertyValue(holder, kAuraPropertyTransientEffects + static_cast<std::uint32_t>(i),
                                              pair) &&
            pair.size() >= 2) {
            transientRadius_[i] = pair[0];
            transientMagnitude_[i] = pair[1];
        }
    }

    hotspotMergeDistance_ = 8;
    hotspotValueThreshold_ = 196;
    hotspotMinCount_ = 16;
    std::vector<std::uint32_t> hotspotSettings;
    if (cSCPropertyHelp::GetPropertyValue(holder, kAuraPropertyHotspotSettings, hotspotSettings) &&
        hotspotSettings.size() == 3) {
        hotspotValueThreshold_ = static_cast<std::int32_t>(hotspotSettings[0]);
        hotspotMergeDistance_ = static_cast<std::int32_t>(hotspotSettings[1]);
        hotspotMinCount_ = static_cast<std::int32_t>(hotspotSettings[2]);
    }

    DeleteCurve(riotCurve_);
    if (holder->HasProperty(kAuraPropertyRiotCurve)) {
        riotCurve_ = new cSC4ResponseCurve();
        if (cISCProperty* property = holder->GetProperty(kAuraPropertyRiotCurve)) {
            const cIGZVariant* value = property->GetPropertyValue();
            riotCurve_->SetResponseCurveData(value->RefFloat32(), value->GetCount());
        }
    }
}

//==================================================================================================
// game-data command handlers
//==================================================================================================

// @ 0x100219220. "add_transient_effect(x, z, radius, magnitude)".
bool cSC4AuraSimulator::sAddTransientEffect(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet*,
                                            cISC4GameDataRegistry* registry) {
    if (!registry || !in) return true;
    const EntryId entry = registry->GetEntryID(handle);
    auto* self = static_cast<cSC4AuraSimulator*>(registry->GetEntryContext(entry));
    if (!self) return true;
    float x = 0, z = 0, radius = 0, magnitude = 0;
    (*in)[0].GetValFloat32(x);
    (*in)[1].GetValFloat32(z);
    (*in)[2].GetValFloat32(radius);
    (*in)[3].GetValFloat32(magnitude);
    self->AddTransientEffect(x, z, radius, magnitude);
    return true;
}

// @ 0x100219316. "end_strike(eStrikeBuster)".
bool cSC4AuraSimulator::sEndStrike(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet*,
                                   cISC4GameDataRegistry* registry) {
    if (!registry || !in) return true;
    const EntryId entry = registry->GetEntryID(handle);
    auto* self = static_cast<cSC4AuraSimulator*>(registry->GetEntryContext(entry));
    if (!self) return true;
    std::uint32_t which = 0;
    (*in)[0].GetValUint32(which);
    self->BustStrike(static_cast<eStrikeBuster>(which));
    return true;
}

//==================================================================================================
// serialization
//==================================================================================================

// @ 0x10021997e: version 9, seven grids, the landmark map keyed by department, the saved ints and the hotspot list.
bool cSC4AuraSimulator::Write(cIGZOStream& stream) const {
    if (!g_host) return false;
    stream.SetUint16(9);
    g_host->WriteGrid(stream, auraGrid_);
    g_host->WriteGrid(stream, parkGrid_);
    g_host->WriteGrid(stream, parkEffectGrid_);
    g_host->WriteGrid(stream, smoothedGrid_);
    g_host->WriteGrid(stream, transientGrid_);
    g_host->WriteGrid(stream, vehicleGrid_);
    g_host->WriteGrid(stream, landmarkGrid_);
    stream.SetUint32(static_cast<std::uint32_t>(landmarkMaps_.size()));
    for (const auto& entry : landmarkMaps_) {
        stream.SetUint32(entry.first);
        g_host->WriteGrid(stream, entry.second);
    }
    stream.SetSint32(cycleValue_);
    stream.SetSint32(mayorRating_);
    stream.SetSint32(maxRatingX_);
    stream.SetSint32(maxRatingZ_);
    stream.SetSint16(maxRating_);
    stream.SetSint32(minRatingX_);
    stream.SetSint32(minRatingZ_);
    stream.SetSint16(minRating_);
    stream.SetUint32(static_cast<std::uint32_t>(hotspotList_.size()));
    for (const tHotspot& hotspot : hotspotList_) {
        stream.SetSint32(hotspot.x);
        stream.SetSint32(hotspot.z);
        stream.SetSint32(hotspot.count);
    }
    return !stream.GetError();
}

// @ 0x100219b70. A version-8 save carries one extra grid that the original reads and releases, setting +0x148 so
// the next 0x26d31ec1 message re-adds every lot's landmark effect.
bool cSC4AuraSimulator::Read(cIGZIStream& stream) {
    if (!g_host) return false;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < 8) return false;
    ReplaceGrid(auraGrid_, g_host->ReadGrid(stream));
    ReplaceGrid(parkGrid_, g_host->ReadGrid(stream));
    ReplaceGrid(parkEffectGrid_, g_host->ReadGrid(stream));
    ReplaceGrid(smoothedGrid_, g_host->ReadGrid(stream));
    ReplaceGrid(transientGrid_, g_host->ReadGrid(stream));
    ReplaceGrid(vehicleGrid_, g_host->ReadGrid(stream));
    ReplaceGrid(landmarkGrid_, g_host->ReadGrid(stream));
    if (version < 9) {
        if (cIGZSerializable* grid = g_host->ReadGrid(stream)) grid->Release();
        needsLandmarkRefresh_ = true;
    } else {
        std::uint32_t count = 0;
        stream.GetUint32(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t key = 0;
            stream.GetUint32(key);
            cIGZSerializable* raw = g_host->ReadGrid(stream);
            auto* grid = dynamic_cast<cSC4SimGrid<std::int16_t>*>(raw);
            if (!grid) {
                if (raw) raw->Release();
                continue;
            }
            auto& slot = landmarkMaps_[key];
            if (slot) slot->Release();
            slot = grid;
        }
    }
    stream.GetSint32(cycleValue_);
    stream.GetSint32(mayorRating_);
    stream.GetSint32(maxRatingX_);
    stream.GetSint32(maxRatingZ_);
    stream.GetSint16(maxRating_);
    stream.GetSint32(minRatingX_);
    stream.GetSint32(minRatingZ_);
    stream.GetSint16(minRating_);
    std::uint32_t count = 0;
    stream.GetUint32(count);
    hotspotList_.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        tHotspot hotspot;
        stream.GetSint32(hotspot.x);
        stream.GetSint32(hotspot.z);
        stream.GetSint32(hotspot.count);
        hotspotList_.push_back(hotspot);
    }
    return !stream.GetError();
}

//==================================================================================================
// queries
//==================================================================================================

// @ 0x10021b364: the published aura at a cell (the grid's own indexing).
std::int8_t cSC4AuraSimulator::GetAuraValue(int cellX, int cellZ) {
    if (!auraGrid_) return -0x80;
    return auraGrid_->GetCellValue(cellX, cellZ);
}

// @ 0x10021b376: the low byte of the population-weighted average aura (quirk).
std::int32_t cSC4AuraSimulator::GetMayorRating() {
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(mayorRating_ & 0xff));
}

// @ 0x10021d2fe / 0x10021d316: the cell and the low byte of the best / worst aura tract.
std::int32_t cSC4AuraSimulator::GetMaxMayorRating(AuraPoint& cell) {
    cell.x = maxRatingX_;
    cell.z = maxRatingZ_;
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(maxRating_ & 0xff));
}

std::int32_t cSC4AuraSimulator::GetMinMayorRating(AuraPoint& cell) {
    cell.x = minRatingX_;
    cell.z = minRatingZ_;
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(minRating_ & 0xff));
}

cSC4SimGrid<std::int8_t>* cSC4AuraSimulator::GetAuraGrid() { return auraGrid_; }
cSC4SimGrid<std::int8_t>* cSC4AuraSimulator::GetTransientAuraGrid() { return transientGrid_; }
cSC4SimGrid<std::int16_t>* cSC4AuraSimulator::GetParkMap() { return parkGrid_; }
cSC4SimGrid<std::int16_t>* cSC4AuraSimulator::GetLandmarkMap() { return landmarkGrid_; }

// @ 0x10021b384: add one of the four preset transient blobs, only when the exemplar gave it a radius and a
// magnitude (the original tests `!= 0 || NaN`).
void cSC4AuraSimulator::AddTransientEffect(float x, float z, TransientAuraEffect effect) {
    const auto index = static_cast<std::size_t>(effect);
    if (index >= 4) return;
    const float magnitude = transientMagnitude_[index];
    if (magnitude == 0.0f && !std::isnan(magnitude)) return;
    const float radius = transientRadius_[index];
    if (radius == 0.0f && !std::isnan(radius)) return;
    AddTransientEffect(x, z, radius, magnitude);
}

// @ 0x10021b3ba: stamp a blob into the transient grid. The radius is in metres and is converted to tracts with the
// grid's tract *width*, then the position is converted with the grid's own PositionToTract.
void cSC4AuraSimulator::AddTransientEffect(float x, float z, float radius, float magnitude) {
    if (!transientGrid_) return;
    const float width = transientGrid_->GetTractWidthX();
    const float tractRadius = width != 0.0f ? radius / width : radius;
    int cellX = 0, cellZ = 0;
    transientGrid_->PositionToTract(x, z, cellX, cellZ);
    AddRadiusEffect(transientGrid_->GetGridData(), transientGrid_->GetTractCountX(),
                    transientGrid_->GetTractCountZ(), cellX, cellZ, tractRadius, magnitude, 0.0f);
}

// @ 0x10021d272: end the strikes the requested buster covers.
void cSC4AuraSimulator::BustStrike(eStrikeBuster which) {
    const auto value = static_cast<std::uint32_t>(which);
    IAuraStrikeApi* police = g_host ? g_host->PoliceStrike() : nullptr;
    IAuraStrikeApi* fire = g_host ? g_host->FireStrike() : nullptr;
    IAuraResidentialApi* residential = g_host ? g_host->Residential() : nullptr;
    if (value < 2 && police) police->EndStrike();
    if ((value | 2u) == 2u && fire) fire->EndStrike();
    if ((value == 3 || value == 0) && residential) residential->EndHealthStrike();
    if ((value | 4u) == 4u && residential) residential->EndSchoolStrike();
}

//==================================================================================================
// tax rate mood and the lot base aura
//==================================================================================================

// @ 0x10021b476: reset every aura type's current aura to its base, then, for each economy type whose tax rate
// changed since the last call, add the curve of the change to the matching aura type.
void cSC4AuraSimulator::ComputeTaxRateChangeEffect() {
    for (AuraTypeEntry* entry = g_auraTypeTable; entry->type != 0; ++entry) entry->currentAura = entry->baseAura;
    if (!taxChangeCurve_ || !g_host) return;
    IAuraBudgetApi* budget = g_host->Budget();
    if (!budget) return;
    for (std::uint32_t type = 0; type < 12; ++type) {
        const float previous = taxRates_[type];
        const float rate = budget->TaxRate(type);
        if (previous != -1.0f || std::isnan(previous)) {
            const float delta = rate - previous;
            if (delta != 0.0f || std::isnan(delta)) {
                const float adjustment = taxChangeCurve_->GetOutputValue(delta);
                for (AuraTypeEntry* entry = g_auraTypeTable; entry->type != 0; ++entry) {
                    if (type == static_cast<std::uint32_t>(entry->economyType)) {
                        entry->currentAura = entry->baseAura + static_cast<int>(adjustment);
                    }
                }
            }
        } else {
            taxRates_[type] = rate;
        }
    }
}

// @ 0x10021b564: the population-weighted mean of the current aura of the building types the lot hosts (only the
// residential economy types 0..2 count as "found"), or -128 when the lot has no building or no residential
// population. With the land-value curve the difference between the cell land value and the lot's occupant wealth
// is added.
std::int8_t cSC4AuraSimulator::GetLotBaseAura(cSC4Lot* lot) {
    if (!lot || !lot->GetBuilding()) return -0x80;
    int weighted = 0, total = 0;
    bool found = false;
    for (const auto& column : lot->GetPopulations()) {
        for (const auto& element : column) {
            const std::uint32_t developerType = element.first;
            const int population = element.second;
            int aura = 0;
            bool residential = found;
            for (const AuraTypeEntry* entry = g_auraTypeTable; entry->type != 0; ++entry) {
                if (developerType == entry->type) {
                    aura = entry->currentAura;
                    residential = found || entry->economyType <= 2;
                    break;
                }
            }
            weighted += aura * population;
            total += population;
            found = residential;
        }
    }
    if (!found || total <= 0) return -0x80;
    int value = weighted / total;
    if (value <= -0x80) value = -0x7f;
    if (value > 0x7f) value = 0x7f;
    if (!landValueCurve_) return static_cast<std::int8_t>(value);

    std::int32_t rect[4] = {0, 0, 0, 0};
    lot->GetBoundingRect(rect);
    const int cellX = (rect[0] + rect[2]) / 2;
    const int cellZ = (rect[1] + rect[3]) / 2;
    const int occupantWealth = lot->GetOccupantWealth();
    int landValue = 0;
    if (g_host) {
        if (IAuraLandValueApi* landValueSim = g_host->LandValue()) {
            landValue = landValueSim->LandValueAtCell(cellX, cellZ);
        }
    }
    const float adjustment = landValueCurve_->GetOutputValue(static_cast<float>(landValue - occupantWealth));
    int result = static_cast<int>(static_cast<std::int8_t>(value)) + static_cast<int>(adjustment);
    if (result <= -0x80) result = -0x7f;
    if (result >= 0x7f) result = 0x7f;
    return static_cast<std::int8_t>(result);
}

//==================================================================================================
// the state machine
//==================================================================================================

// @ 0x10021a1c4. One stage per call; `budgetMilliseconds` is the idle time the simulator granted.
void cSC4AuraSimulator::DoIdle(std::uint32_t budgetMilliseconds) {
    switch (idleState_) {
    case 1:
        if (initialized_ && g_host) {
            idleStartDate_ = g_host->SimDateNumber();
            idleState_ = 2;
        }
        break;
    case 2: {
        float factor = 1.0f;
        if (g_host) {
            if (IAuraBudgetApi* budget = g_host->Budget()) factor = budget->DepartmentFunding(kAuraParkDepartment);
        }
        if (parkGrid_ && parkEffectGrid_) {
            ProcessTwoGridsWithCallback<std::int16_t, std::int16_t>(*parkGrid_, *parkEffectGrid_, AssignWithFactor,
                                                                   &factor);
        }
        if (static int shown = 0; shown < 3 && std::getenv("SC4_RESDEBUG")) {
            ++shown;
            long park = 0, effect = 0, landmark = 0;
            if (parkGrid_) for (const auto& column : parkGrid_->GetGridData()) for (std::int16_t v : column) park += v;
            if (parkEffectGrid_) for (const auto& column : parkEffectGrid_->GetGridData()) for (std::int16_t v : column) effect += v;
            if (landmarkGrid_) for (const auto& column : landmarkGrid_->GetGridData()) for (std::int16_t v : column) landmark += v;
            long aura = 0, smoothed = 0;
            if (auraGrid_) for (const auto& column : auraGrid_->GetGridData()) for (auto v : column) aura += v != 0;
            if (smoothedGrid_) for (const auto& column : smoothedGrid_->GetGridData()) for (auto v : column) smoothed += v != 0;
            std::fprintf(stderr, "aura: park map sum %ld, park effect sum %ld, landmark sum %ld, landmark maps %zu, funding %.2f; non-zero aura %ld smoothed %ld\n", park,
                         effect, landmark, landmarkMaps_.size(), factor, aura, smoothed);
        }
        idleState_ = 3;
        break;
    }
    case 3: {
        if (landmarkMaps_.empty()) {
            idleState_ = 4;
            break;
        }
        const std::uint32_t previousKey = currentLandmarkKey_;
        std::map<std::uint32_t, cSC4SimGrid<std::int16_t>*>::iterator it;
        if (previousKey == 0xffffffffu) {
            it = landmarkMaps_.begin();
        } else {
            it = landmarkMaps_.lower_bound(previousKey);
            if (it == landmarkMaps_.end() || previousKey < it->first) {
                currentLandmarkKey_ = 0xffffffffu;
                idleState_ = 4;
                break;
            }
            ++it;
            if (it == landmarkMaps_.end()) {
                // The walk is done: the accumulated work grid becomes the published landmark map.
                if (landmarkGrid_ && landmarkWorkGrid_) {
                    landmarkGrid_->GetGridData() = landmarkWorkGrid_->GetGridData();
                }
                currentLandmarkKey_ = 0xffffffffu;
                idleState_ = 4;
                break;
            }
        }
        const std::uint32_t key = it->first;
        cSC4SimGrid<std::int16_t>* grid = it->second;
        currentLandmarkKey_ = key;
        float factor = 1.0f;
        if (key != 0 && g_host) {
            if (IAuraBudgetApi* budget = g_host->Budget()) factor = budget->DepartmentFunding(key);
        }
        if (landmarkWorkGrid_ && grid) {
            if (previousKey == 0xffffffffu) {
                ProcessTwoGridsWithCallback<std::int16_t, std::int16_t>(*landmarkWorkGrid_, *grid, AssignWithFactor,
                                                                       &factor);
            } else {
                ProcessTwoGridsWithCallback<std::int16_t, std::int16_t>(*landmarkWorkGrid_, *grid, AddWithFactor,
                                                                       &factor);
            }
        }
        return;   // the state stays 3 for the next map
    }
    case 4: {
        if (cellGrid_) cellGrid_->SetTractValues(-0x80);
        IAuraLotManagerApi* lotManager = g_host ? g_host->LotManager() : nullptr;
        if (!lotManager) {
            idleState_ = 0;
            break;
        }
        if (!lotIterator_) {
            lotIterator_ = lotManager->LotIterator();
        } else {
            lotIterator_->Restart();
        }
        if (lotManager->LotCount() == 0) {
            idleState_ = 0;
        } else {
            ComputeTaxRateChangeEffect();
            idleState_ = 5;
        }
        break;
    }
    case 5: {
        if (!cellGrid_ || !g_host) {
            idleState_ = 6;
            break;
        }
        IAuraLotIterator* iterator = lotIterator_;
        auto& data = cellGrid_->GetGridData();
        const int shift = cellGrid_->GetTractShift();
        const int countX = cellGrid_->GetTractCountX();
        const int countZ = cellGrid_->GetTractCountZ();
        const std::uint64_t token = g_host->BeginSlice(budgetMilliseconds);
        // The original's per-pass baseline (a file-scope static): the counter steps by 0x400 while the time check
        // masks with 0x3ff, so the clock is consulted after every lot (quirk kept).
        constexpr std::int32_t kTimeCheckStep = 0x400;
        std::int32_t counter = 0;
        bool timedOut = false;
        while (true) {
            counter += kTimeCheckStep;
            if ((counter & 0x3ff) == 0 && g_host->SliceExpired(token)) {
                timedOut = true;
                break;
            }
            if (!iterator || iterator->AtEnd()) break;
            cSC4Lot* lot = iterator->Lot();   // the original advances after reading the node
            iterator->Advance();
            if (!lot) continue;
            const std::int8_t base = GetLotBaseAura(lot);
            std::int32_t rect[4] = {0, 0, 0, 0};
            lot->GetBoundingRect(rect);
            for (std::int32_t& corner : rect) corner >>= shift;
            for (std::int32_t x = rect[0]; x <= rect[2]; ++x) {
                if (x < 0 || x >= countX) continue;   // the original would overrun the grid
                for (std::int32_t z = rect[1]; z <= rect[3]; ++z) {
                    if (z < 0 || z >= countZ) continue;
                    std::int8_t& cell = CellAt(data, x, z);
                    if (cell < base) cell = base;
                }
            }
        }
        if (!timedOut) idleState_ = 6;
        break;
    }
    case 6:
        if (parkCurve_ && cellGrid_ && parkEffectGrid_) {
            ProcessTwoGridsWithCallback<std::int8_t, std::int16_t>(*cellGrid_, *parkEffectGrid_,
                                                                  AddCurveToSignedChar<std::int16_t>, parkCurve_);
            idleState_ = 7;
        }
        break;
    case 7: {
        if (!pollutionCurves_.empty() && cellGrid_) {
            IAuraPollutionApi* pollution = g_host ? g_host->Pollution() : nullptr;
            for (std::size_t i = 0; i < pollutionCurves_.size(); ++i) {
                cSC4ResponseCurve* curve = pollutionCurves_[i];
                if (!curve) continue;
                cSC4SimGrid<std::int16_t>* source = pollution ? pollution->TypeMap(static_cast<int>(i)) : nullptr;
                if (!source) continue;
                ProcessTwoGridsWithCallback<std::int8_t, std::int16_t>(*cellGrid_, *source,
                                                                      AddCurveToSignedChar<std::int16_t>, curve);
            }
        }
        idleState_ = 8;
        break;
    }
    case 8:
    case 9:
    case 11:
    case 12: {
        // The four "residential only" passes: the destination cell gets the value only where a lot is zoned
        // residential (zone type 1..3). Stages 8/9 read the two residential float maps, 11/12 the two traffic
        // uint8 maps.
        const bool first = idleState_ == 8 || idleState_ == 11;
        IAuraResidentialApi* residential = g_host ? g_host->Residential() : nullptr;
        IAuraGridApi* trafficDensity = g_host ? g_host->TrafficDensity() : nullptr;
        IAuraGridApi* trafficCongestion = g_host ? g_host->TrafficCongestion() : nullptr;
        cSC4ResponseCurve* curve = idleState_ == 8    ? residentialCurve138_
                                   : idleState_ == 9  ? residentialCurve140_
                                   : idleState_ == 11 ? trafficTripCurve_
                                                      : trafficCurve_;
        auto zoneTypeOfLotAt = [](int x, int z) -> int {
            IAuraLotManagerApi* lotManager = g_host ? g_host->LotManager() : nullptr;
            cSC4Lot* lot = lotManager ? lotManager->LotAtCell(x, z, 0) : nullptr;
            return lot ? static_cast<int>(lot->GetZoneType()) : 0;
        };
        if (curve && cellGrid_) {
            if (idleState_ == 8 || idleState_ == 9) {
                cSC4SimGrid<float>* source =
                    residential ? (first ? residential->EQCandidateMap() : residential->HQCandidateMap()) : nullptr;
                if (source) {
                    ProcessTwoGridsForROnly<float>(*cellGrid_, *source, AddCurveToSignedChar<float>, curve,
                                                   zoneTypeOfLotAt);
                }
            } else {
                IAuraGridApi* api = first ? trafficDensity : trafficCongestion;
                cSC4SimGrid<std::uint8_t>* source = api ? api->Uint8Map() : nullptr;
                if (source) {
                    GridCallback<std::int8_t, std::uint8_t> callback =
                        first ? AddTripLengthCurve : AddCurveToSignedChar<std::uint8_t>;
                    ProcessTwoGridsForROnly<std::uint8_t>(*cellGrid_, *source, callback, curve, zoneTypeOfLotAt);
                }
            }
        }
        idleState_ += 1;
        break;
    }
    case 10:
        if (crimeCurve_ && cellGrid_ && g_host) {
            if (IAuraGridApi* crime = g_host->Crime()) {
                if (cSC4SimGrid<std::uint8_t>* source = crime->Uint8Map()) {
                    ProcessTwoGridsWithCallback<std::int8_t, std::uint8_t>(
                        *cellGrid_, *source, AddCurveToSignedChar<std::uint8_t>, crimeCurve_);
                }
            }
        }
        idleState_ = 11;
        break;
    case 13:
        if (cellGrid_ && vehicleGrid_) {
            ProcessTwoGridsWithCallback<std::int8_t, std::int8_t>(*cellGrid_, *vehicleGrid_,
                                                                  AddCurveToSignedChar<std::int8_t>, nullptr);
        }
        idleState_ = 14;
        break;
    case 14: {
        // Smooth the per-cycle cell grid into the persistent tract grid: every destination tract averages its
        // source block (skipping -128 cells), the ordinance value is added, and the cell moves
        // (1 - smoothing) of the way towards the target with a round-away-from-zero step.
        if (!cellGrid_ || !smoothedGrid_) {
            idleState_ = 15;
            break;
        }
        auto& source = cellGrid_->GetGridData();
        auto& destination = smoothedGrid_->GetGridData();
        const int difference = smoothedGrid_->GetTractShift() - cellGrid_->GetTractShift();
        const int sourceCountX = cellGrid_->GetTractCountX();
        const int sourceCountZ = cellGrid_->GetTractCountZ();
        const int countX = smoothedGrid_->GetTractCountX();
        const int countZ = smoothedGrid_->GetTractCountZ();
        ordinanceValue_ = 0;
        if (g_host) {
            if (IAuraOrdinanceApi* ordinance = g_host->Ordinance()) {
                ordinanceValue_ = ordinance->OrdinanceValue(kAuraOrdinance, 0);
            }
        }
        const float decay = 1.0f - auraSmoothing_;
        for (int x = 0; x < countX; ++x) {
            for (int z = 0; z < countZ; ++z) {
                const std::int32_t startX = x << difference;
                const std::int32_t endX = (x + 1) << difference;
                const std::int32_t startZ = z << difference;
                const std::int32_t endZ = (z + 1) << difference;
                std::int32_t sum = 0, count = 0;
                for (std::int32_t sx = startX; sx < endX; ++sx) {
                    if (sx < 0 || sx >= sourceCountX) continue;
                    for (std::int32_t sz = startZ; sz < endZ; ++sz) {
                        if (sz < 0 || sz >= sourceCountZ) continue;
                        const std::int32_t sample = CellAt(source, sx, sz);
                        if (sample == -0x80) continue;
                        ++count;
                        sum += sample;
                    }
                }
                std::int8_t* cell = &CellAt(destination, x, z);
                if (count == 0) {
                    *cell = -0x80;
                    continue;
                }
                const std::int32_t target = sum / count + ordinanceValue_;
                const std::int32_t previous = *cell;
                std::int32_t value = target;
                if (previous != -0x80) {
                    const std::int32_t delta = target - previous;
                    if (delta != 0) {
                        const float scaled = static_cast<float>(delta) * decay;
                        value = previous + AuraRoundStep(delta, scaled);
                    }
                }
                if (value > 0x7f) value = 0x7f;
                if (value < -0x7f) value = -0x7f;
                *cell = static_cast<std::int8_t>(value);
            }
        }
        idleState_ = 15;
        break;
    }
    case 15:
        UpdateAuraGrid();
        idleState_ = 16;
        break;
    case 16:
        ComputeHotspotList();
        CheckForStartingRiot();
        idleState_ = 17;
        break;
    case 17: {
        if (transientGrid_) {
            auto& data = transientGrid_->GetGridData();
            const int countX = transientGrid_->GetTractCountX();
            const int countZ = transientGrid_->GetTractCountZ();
            for (int x = 0; x < countX; ++x) {
                for (int z = 0; z < countZ; ++z) {
                    std::int8_t& cell = CellAt(data, x, z);
                    if (cell == 0) continue;
                    cell = static_cast<std::int8_t>(static_cast<int>(static_cast<float>(cell) * transientDecay_));
                }
            }
        }
        DoEndOfSimulationCycle();
        idleState_ = 0;
        break;
    }
    default:
        break;
    }
}

// @ 0x10021acd2. Adds (or removes) one occupant's effects: the park property (0x27812850) into the park effect
// grid, the landmark property (0x2781284f) into the per-department landmark maps and the vehicle aura
// (0xca5b9305) into the vehicle grid. Each value is an int32{value, radius in cells} pair; a removal negates the
// value.
void cSC4AuraSimulator::DoOccupantChanged(cISC4Occupant* occupant, bool added) {
    if (!occupant) return;
    if (added && occupant->GetType() == kAuraVehicleOccupantType) {
        float position[3] = {0, 0, 0};
        occupant->GetPosition(position);
        AddTransientEffect(position[0], position[2], kTransientAuraEffect3);
    }
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    if (!holder) return;
    cISCProperty* parkProperty = holder->GetProperty(kAuraPropertyParkEffect);
    cISCProperty* landmarkProperty = holder->GetProperty(kAuraPropertyLandmarkEffect);

    int cellX = 0, cellZ = 0;
    std::int32_t rect[4] = {0, 0, 0, 0};
    if (parkProperty || landmarkProperty) {
        float position[3] = {0, 0, 0};
        occupant->GetPosition(position);
        if (g_host) {
            if (IAuraCityApi* city = g_host->City()) city->PositionToCell(position[0], position[2], cellX, cellZ);
        }
        cSC4Lot* lot = nullptr;
        if (g_host) {
            if (IAuraLotManagerApi* lotManager = g_host->LotManager()) lot = lotManager->LotAtCell(cellX, cellZ, 0);
        }
        if (lot) {
            // The effect is centred on the lot's centre, nudged one cell towards the occupant when the occupant
            // sits in the other half of the lot.
            lot->GetBoundingRect(rect);
            const float cellSizeX = g_host && g_host->City() ? g_host->City()->CellSizeXMeters() : 0.0f;
            const float cellSizeZ = g_host && g_host->City() ? g_host->City()->CellSizeZMeters() : 0.0f;
            const int width = (rect[2] - rect[0]) + 1;
            if (width - ((width - (width >> 31)) & 0xfffffffe) == 1) {
                cellX = width / 2 + rect[0];
            } else {
                const bool west = (position[0] - static_cast<float>(rect[0]) * cellSizeX) <
                                  (static_cast<float>(width) * cellSizeX +
                                   static_cast<float>(rect[0]) * cellSizeX) - position[0];
                cellX = (width / 2 + rect[0]) - (west ? 1 : 0);
            }
            const int height = (rect[3] - rect[1]) + 1;
            if (height - ((height - (height >> 31)) & 0xfffffffe) == 1) {
                cellZ = height / 2 + rect[1];
            } else {
                const bool north = (position[2] - static_cast<float>(rect[1]) * cellSizeZ) <
                                   (static_cast<float>(height) * cellSizeZ +
                                    static_cast<float>(rect[1]) * cellSizeZ) - position[2];
                cellZ = (height / 2 + rect[1]) - (north ? 1 : 0);
            }
        }
    }

    if (parkProperty && parkEffectGrid_) {
        const std::int32_t* value = parkProperty->GetPropertyValue()->RefSint32();
        const std::int32_t peak = added ? value[0] : -value[0];
        const float radius = static_cast<float>(value[1]);
        const int tractSize = parkEffectGrid_->GetTractSize();
        const float tractRadius = tractSize != 0 ? radius / static_cast<float>(tractSize) : radius;
        const int shift = parkEffectGrid_->GetTractShift();
        AddRadiusEffect(parkEffectGrid_->GetGridData(), parkEffectGrid_->GetTractCountX(),
                        parkEffectGrid_->GetTractCountZ(), cellX >> shift, cellZ >> shift, tractRadius,
                        static_cast<float>(peak), 0.0f);
    }

    if (landmarkProperty) {
        const std::int32_t* value = landmarkProperty->GetPropertyValue()->RefSint32();
        const std::int32_t peak = added ? value[0] : -value[0];
        const float radius = static_cast<float>(value[1]);
        // The map key is 0, or the lot's x0 when the building's department is funded (quirk). The original copies
        // the int bits into a float-typed temporary and uses them as the key, which is the same 32-bit value.
        std::uint32_t key = 0;
        if (g_host) {
            if (IAuraBudgetApi* budget = g_host->Budget()) {
                if (budget->IsDepartmentFunded(holder, kAuraDepartmentProperty)) {
                    key = static_cast<std::uint32_t>(rect[0]);
                }
            }
        }
        cSC4SimGrid<std::int16_t>* grid = nullptr;
        const auto it = landmarkMaps_.find(key);
        if (it != landmarkMaps_.end()) {
            grid = it->second;
        } else if (added) {
            grid = MakeInt16Grid(key, tractSize_);
            landmarkMaps_[key] = grid;
        }
        if (grid) {
            const int tractSize = grid->GetTractSize();
            const float tractRadius = tractSize != 0 ? radius / static_cast<float>(tractSize) : radius;
            const int shift = grid->GetTractShift();
            AddRadiusEffect(grid->GetGridData(), grid->GetTractCountX(), grid->GetTractCountZ(), cellX >> shift,
                            cellZ >> shift, tractRadius, static_cast<float>(peak), 0.0f);
        }
    }

    if (cISCProperty* vehicleProperty = holder->GetProperty(kAuraPropertyVehicleAura)) {
        if (vehicleGrid_) {
            const std::int32_t* value = vehicleProperty->GetPropertyValue()->RefSint32();
            const std::int32_t peak = added ? value[0] : -value[0];
            const float radius = static_cast<float>(value[1]);
            const int tractSize = vehicleGrid_->GetTractSize();
            const float tractRadius = tractSize != 0 ? radius / static_cast<float>(tractSize) : radius;
            float position[3] = {0, 0, 0};
            occupant->GetPosition(position);
            int tractX = 0, tractZ = 0;
            vehicleGrid_->PositionToTract(position[0], position[2], tractX, tractZ);
            AddRadiusEffect(vehicleGrid_->GetGridData(), vehicleGrid_->GetTractCountX(),
                            vehicleGrid_->GetTractCountZ(), tractX, tractZ, tractRadius,
                            static_cast<float>(peak), 0.0f);
        }
    }
}

// @ 0x10021b2d0: reset +0xd0 and post the cycle-end message with this object as data1 and as the payload.
void cSC4AuraSimulator::DoEndOfSimulationCycle() {
    cycleValue_ = 0;
    auto* message = new cRZMessage2Standard();
    cIGZMessage2Standard* view = message;
    view->AddRef();
    view->SetType(kAuraMessageCycleEnd);
    auto* primary = static_cast<cISC4AuraSimulator*>(this);
    view->SetVoid1(static_cast<void*>(primary));
    view->SetIGZUnknown(primary);
    if (messageServer_) messageServer_->MessagePost(view, false);
    view->Release();
}

// @ 0x10021cc5c: publish the aura grid (smoothed + transient, 0 where the smoothed value is "no data"), track the
// worst/best tract, collect this cycle's hotspots and publish the population-weighted mayor rating.
void cSC4AuraSimulator::UpdateAuraGrid() {
    if (!auraGrid_ || !smoothedGrid_ || !transientGrid_ || !g_host) return;
    auto& aura = auraGrid_->GetGridData();
    auto& smoothed = smoothedGrid_->GetGridData();
    auto& transient = transientGrid_->GetGridData();

    cSC4SimGrid<std::uint16_t>* wealth1 = nullptr;
    cSC4SimGrid<std::uint16_t>* wealth2 = nullptr;
    cSC4SimGrid<std::uint16_t>* wealth3 = nullptr;
    if (IAuraResidentialApi* residential = g_host->Residential()) {
        residential->PopulationGrids(wealth1, wealth2, wealth3);
    }

    const int tractSize = auraGrid_->GetTractSize();
    const int countX = auraGrid_->GetTractCountX();
    const int countZ = auraGrid_->GetTractCountZ();

    hotspots_.clear();
    maxRating_ = -0x80;
    minRating_ = 0x7f;
    std::int32_t weightedSum = 0, population = 0;
    for (int x = 0; x < countX; ++x) {
        for (int z = 0; z < countZ; ++z) {
            const std::int32_t smoothedValue = CellAt(smoothed, x, z);
            if (smoothedValue == -0x80) {
                CellAt(aura, x, z) = 0;
                continue;
            }
            std::int32_t value = smoothedValue + CellAt(transient, x, z);
            if (value < -0x7f) value = -0x7f;
            if (value > 0x7f) value = 0x7f;
            CellAt(aura, x, z) = static_cast<std::int8_t>(value);
            const std::int32_t cellX = x * tractSize + tractSize / 2;
            const std::int32_t cellZ = z * tractSize + tractSize / 2;
            if (value < minRating_) {
                minRating_ = static_cast<std::int16_t>(value);
                minRatingX_ = cellX;
                minRatingZ_ = cellZ;
            }
            if (value > maxRating_) {
                maxRating_ = static_cast<std::int16_t>(value);
                maxRatingX_ = cellX;
                maxRatingZ_ = cellZ;
            }
            if (value < hotspotValueThreshold_) {
                tHotspot hotspot;
                hotspot.x = x;
                hotspot.z = z;
                hotspot.count = 1;
                hotspots_.push_back(hotspot);
            }
            std::int32_t tractPopulation = 0;
            if (wealth1 && x < wealth1->GetTractCountX() && z < wealth1->GetTractCountZ()) {
                tractPopulation += wealth1->GetTractValue(x, z);
            }
            if (wealth2 && x < wealth2->GetTractCountX() && z < wealth2->GetTractCountZ()) {
                tractPopulation += wealth2->GetTractValue(x, z);
            }
            if (wealth3 && x < wealth3->GetTractCountX() && z < wealth3->GetTractCountZ()) {
                tractPopulation += wealth3->GetTractValue(x, z);
            }
            population += tractPopulation;
            weightedSum += tractPopulation * value;
        }
    }
    if (wealth1) wealth1->Release();
    if (wealth2) wealth2->Release();
    if (wealth3) wealth3->Release();

    mayorRating_ = population > 0 ? weightedSum / population : 0;
    if (IAuraHistoryApi* history = g_host->History()) {
        history->AddHistory(kAuraHistoryMayorRating, mayorRating_);
    }
}

// @ 0x10021cfdc: merge this cycle's raw hotspots into the list (Manhattan distance below the merge distance,
// running average of the positions) and drop the entries seen fewer times than the minimum count.
void cSC4AuraSimulator::ComputeHotspotList() {
    const int distance = hotspotMergeDistance_;
    hotspotList_.clear();
    for (const tHotspot& raw : hotspots_) {
        bool merged = false;
        for (tHotspot& existing : hotspotList_) {
            const int dx = std::abs(existing.x - raw.x);
            const int dz = std::abs(existing.z - raw.z);
            if (dx + dz < distance) {
                const int count = existing.count;
                const int next = count + 1;
                existing.x = (existing.x * count + raw.x) / next;
                existing.z = (existing.z * count + raw.z) / next;
                existing.count = next;
                merged = true;
                break;
            }
        }
        if (merged) continue;
        tHotspot hotspot;
        hotspot.x = raw.x;
        hotspot.z = raw.z;
        hotspot.count = raw.count;
        hotspotList_.push_front(hotspot);
    }
    for (auto it = hotspotList_.begin(); it != hotspotList_.end();) {
        if (it->count < hotspotMinCount_) {
            it = hotspotList_.erase(it);
        } else {
            ++it;
        }
    }
}

// @ 0x10021d174: with probability riotCurve(count of a random hotspot) start a riot at that hotspot's tract centre.
void cSC4AuraSimulator::CheckForStartingRiot() {
    // The original always consumes the uniform double, even when there is nothing to do (quirk).
    const double roll = random_.RandomDoubleRangeUniform(0.0, 100.0);
    if (hotspotList_.empty() || !riotCurve_) return;
    const auto index = random_.RandomUint32Uniform(static_cast<std::uint32_t>(hotspotList_.size()));
    auto it = hotspotList_.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));
    const float chance = riotCurve_->GetOutputValue(static_cast<float>(it->count));
    if (!(static_cast<float>(roll) < chance)) return;
    const int tractSize = auraGrid_ ? auraGrid_->GetTractSize() : tractSize_;
    const int cellX = it->x * tractSize + tractSize / 2;
    const int cellZ = it->z * tractSize + tractSize / 2;
    if (!g_host) return;
    IAuraCityApi* city = g_host->City();
    if (!city) return;
    IAuraDisasterApi* disaster = city->Disaster(kAuraRiotType);
    if (disaster) disaster->StartAtCell(cellX, cellZ);
}

//==================================================================================================
// messages
//==================================================================================================

// @ 0x100219f62. The five subscribed messages plus the day / month / idle-time ticks of the state machine.
bool cSC4AuraSimulator::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const auto type = static_cast<std::uint32_t>(message->GetType());
    if (type == kMessageOccupantInsertedAura || type == kMessageOccupantRemovedAura) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        if (!occupant) return true;
        cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        if (holder && holder->HasProperty(kAuraPropertyNoAuraBuilding)) return true;
        DoOccupantChanged(occupant, type == kMessageOccupantInsertedAura);
    } else if (type == kAuraMessageOrdinanceChanged) {
        ordinanceValue_ = 0;
        if (g_host) {
            if (IAuraOrdinanceApi* ordinance = g_host->Ordinance()) {
                ordinanceValue_ = ordinance->OrdinanceValue(kAuraOrdinance, 0);
            }
        }
    } else if (type == kAuraMessageReloadTunables) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        const auto data1 = static_cast<std::uint32_t>(standard->GetData1());
        const auto data2 = static_cast<std::uint32_t>(standard->GetData2());
        if (data1 == kAuraExemplarGroup && data2 == kAuraExemplarInstance) ReadSimulatorTunableValues();
    } else if (type == kAuraMessageLotsChanged) {
        if (!needsLandmarkRefresh_ || !g_host) return true;
        IAuraLotManagerApi* lotManager = g_host->LotManager();
        if (!lotManager) return true;
        IAuraLotIterator* iterator = lotManager->LotIterator();
        if (!iterator) return true;
        while (!iterator->AtEnd()) {
            cSC4Lot* lot = iterator->Lot();
            iterator->Advance();
            if (!lot) continue;
            cISC4Occupant* occupant = g_host->BuildingOccupant(lot);
            if (!occupant) continue;
            cISCPropertyHolder* holder = occupant->AsPropertyHolder();
            if (holder && holder->HasProperty(kAuraPropertyLandmarkEffect)) {
                DoOccupantChanged(occupant, false);
                DoOccupantChanged(occupant, true);
            }
        }
        iterator->Release();
    } else if (type == kAuraMessageDay) {
        DoIdle(5);
        if (idleState_ != 0 && g_host) {
            const std::int32_t date = g_host->SimDateNumber();
            if (static_cast<std::uint32_t>(idleState_) < static_cast<std::uint32_t>(date - idleStartDate_)) {
                return false;   // not handled yet: the message server retries it
            }
        }
    } else if (type == kAuraMessageMonth) {
        if (idleState_ == 0) idleState_ = 1;
    } else if (type == kAuraMessageIdleTime) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        DoIdle(static_cast<std::uint32_t>(standard->GetData3()));
    }
    return true;
}

}  // namespace sc4
