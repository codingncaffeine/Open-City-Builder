// cSC4ProtectionSimulator — shared base of the police and fire simulators (37 functions, 0x1000a9b10..
// 0x1000ab93a). It keeps two tract grids: the "unadjusted" protection grid (int16, 0x49b9e605) that every
// station stamps a radius effect into, and the "adjusted" grid (uint8, 0x49b9e602) = unadjusted * the
// funding/efficiency factor (+0xe4), clamped to 0..255. A daily sweep (cells / cellsPerDayDivisor cells per
// tick, scrambled order) refreshes the adjusted grid, and at the end of a pass publishes the average
// protection of zoned cells and the share of zoned cells with any protection.
//
// Stations (building occupants of type 0x278128a0) are tracked in a list; each owns a run of "dispatch
// bits" (offset assigned first-fit above bit 0, +0xf0 = highest bit in use) that the dispatch manager uses
// to give out vehicles. The subclass hooks the original calls through vslots are the IProtectionHost seam.
//
// Original layout used here: +0x30 cellsPerDay, +0x34 tract size, +0x38 cell divisor, +0x40 RandomIterator,
// +0x58 coverage-less byte list, +0xb8 station list (+0xc8 count), +0xd0 zoned count, +0xd4 protection sum,
// +0xd8 protected count, +0xdc average, +0xe0 coverage share, +0xe4 scale, +0xf0 dispatch bits in use.
// Stream Read/Write, message registration and Init/Shutdown wiring need the full service locator and stay
// unrecovered, as do DispatchFromStation / ReturnDispatchToStation whose decompilation is degenerate (the
// bit-scan loops constant-fold away); the dispatch-manager calls are reached through the host.
#pragma once
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include "gz/cIGZUnknown.h"
#include "sc4/RandomIterator.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/RetainedReference.h"

class cIGZIStream;
class cIGZOStream;

namespace sc4 {

constexpr std::uint32_t kProtectionBuildingOccupantType = 0x278128a0;
constexpr std::uint32_t kProtectionAdjustedGridInstance = 0x49b9e602;
constexpr std::uint32_t kProtectionUnadjustedGridInstance = 0x49b9e605;
constexpr std::uint32_t kGZIID_cISC4ProtectionDispatchSimulator = 0xc9c00435;
constexpr std::uint32_t kInt16GridIid = 0x223e2b67;    // cISC4SimGrid<int16>
constexpr std::uint32_t kUint8GridIid = 0xc9bd939a;    // cISC4SimGrid<uint8>

// A station's radius effect: cell position, radius in cells, centre and edge strength (SC4 ProtectionEffect).
struct ProtectionEffect {
    float x = 0, z = 0;       // metres
    float radius = 0;         // +8, scaled by AdjustProtectionEffect
    int peak = 0;             // +0xc
    int edge = 0;             // +0x10
    int capacity = 0;         // +0x14 (police: copy of peak, not scaled by AdjustProtectionEffect)
};

class IProtectionStation;
struct ProtectionDispatchInfo {   // one active dispatch as reported by the dispatch manager
    bool valid = false;
    bool active = false;      // original tests the flag at +0x10 of the record
    IProtectionStation* origin = nullptr;   // station the dispatch left from
    float targetX = 0, targetZ = 0;
};

class IProtectionStation {
public:
    virtual ~IProtectionStation() = default;
    virtual std::uint32_t OccupantType() = 0;               // vslot 0x38
    virtual void Position(float xyz[3]) = 0;                // vslot 0x40
};

// The 0xc9c00435 "dispatch" view (GetDispatchesAtStation etc.). Answered at +0x10 by the fire/police
// simulators; kept as a distinct (empty) base so cIGZSerializable lands at +0x18, as in the binary.
class cISC4ProtectionDispatchSimulator : public cIGZUnknown {};

class IProtectionHost {
public:
    virtual ~IProtectionHost() = default;
    virtual bool GetStationEffect(IProtectionStation* station, ProtectionEffect& effect) = 0;   // vslot 0xb8
    virtual void OnAdjustedCell(int x, int z, int value) = 0;     // vslot 0xc8
    virtual void OnPassFinished() = 0;                            // vslot 0xd0
    virtual void SetStationDispatchMask(IProtectionStation* station, std::uint32_t mask) = 0;   // vslot 0x98
    virtual void SetStationDispatchOffset(IProtectionStation* station, std::uint32_t offset) = 0;   // vslot 0xc0
    // station info (second interface vslot 0x38): any pointer may be null.
    virtual void GetStationInfo(IProtectionStation* station, int* availableDispatches, std::uint32_t* bitOffset,
                                std::uint32_t* bitCount) = 0;
    virtual int MinimumDispatchesForService() = 0;                // vslot 0x48
    virtual std::uint32_t DispatchPurpose() = 0;                  // vslot 0xb0
    virtual int AvailableDispatchCount(std::uint32_t purpose) = 0;   // dispatch manager 0x58
    virtual std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) = 0;   // dispatch manager 0x60
    virtual bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) = 0;     // dispatch manager 0x50
    virtual bool LotAllowsDispatch(IProtectionStation* station) = 0;   // lot manager 0x70 -> 0xe0 (true when no lot)
    virtual bool IsAboveWater(float x, float z) = 0;              // terrain 0x60 > 0xe8 (water level)
    virtual bool IsZonedAt(int x, int z) = 0;                     // zone manager grid, byte per cell
    virtual void RegisterStationProperty(IProtectionStation* station, bool add) = 0;   // occupant manager 0x198
    virtual void GetDispatchEffect(const ProtectionDispatchInfo& info, ProtectionEffect& effect) = 0;   // vslot 0xa8
    virtual void TakeStationDispatch(IProtectionStation* station) = 0;   // vslot 0x60 (hands out one vehicle)
    // ---- stream seams: the protection base reaches nested serializables through the subclass host ----
    virtual void WriteGridInt16(cIGZOStream&, cSC4SimGrid<std::int16_t>*) {}       // SC4WriteGZSerializable(stream, grid)
    virtual bool ReadGridInt16(cIGZIStream&, cSC4SimGrid<std::int16_t>**) { return false; }
    virtual void WriteGridUint8(cIGZOStream&, cSC4SimGrid<std::uint8_t>*) {}
    virtual bool ReadGridUint8(cIGZIStream&, cSC4SimGrid<std::uint8_t>**) { return false; }
    virtual void WriteStation(cIGZOStream&, IProtectionStation* station) {}   // SC4WriteGZSerializable(stream, occupant)
    virtual bool ReadStation(cIGZIStream&, IProtectionStation** out) { *out = nullptr; return false; }
};

// cSC4MapUtilities::AddRadiusEffect: adds a linear falloff (peak at the centre, `edge` at the radius) to every
// cell of an int16 column-major grid within `radius` of (cx, cz), saturating at the int16 range.
void AddRadiusEffect(std::vector<std::vector<std::int16_t>>& grid, int width, int height, int cx, int cz,
                     float radius, float peak, float edge);

class cSC4ProtectionSimulator {
public:
    explicit cSC4ProtectionSimulator(IProtectionHost* host, int tractSize = 1, int cellDivisor = 1);

    // ProtectionInit core: creates both grids at the tract size and resets the sweep.
    void CreateGrids(int citySizeXInCells, int citySizeZInCells);

    void DoDailySimulation();
    bool IsProtectionGridCellZoned(int tx, int tz) const;     // any zoned cell inside the tract
    static void AdjustProtectionEffect(ProtectionEffect& effect, float factor);
    void UpdateUnadjustedProtectionGrid();
    void CalculateAndWriteAdjustedGridDataByCell(int tx, int tz, int radius);
    void UpdateBothGridsByPosition(float x, float z, float radiusFraction);

    // stations
    void DoOccupantInserted(IProtectionStation* station);
    void DoOccupantRemoved(IProtectionStation* station);
    void DoOccupantChanged(IProtectionStation* station);
    bool GetStations(std::list<IProtectionStation*>& out) const;
    int GetMaxPossibleDispatchCount();
    bool CanDispatch(float x, float z, bool* enough);
    IProtectionStation* DispatchFromNearestStation(float x, float z, std::uint32_t* dispatchId);
    const std::list<IProtectionStation*>& Stations() const { return stations_; }
    // @ 0x1000aadce: replaces `out` with a copy of `source` (the original also takes a reference on each entry; the
    // port's occupants are borrowed) and returns the number copied.
    template <class T>
    static int CopyOccupantList(std::list<T*>& out, const std::list<T*>& source) {
        out.clear();
        for (T* occupant : source) out.push_back(occupant);
        return static_cast<int>(out.size());
    }

    // Streams (the fire/police simulators delegate to these). Version 3.
    bool Write(cIGZOStream& stream) const;
    bool ReadCityRecord(cIGZIStream& stream);   // @ 0x1000aa362
    bool Read(cIGZIStream& stream);

    cSC4SimGrid<std::uint8_t>* AdjustedGrid() const { return adjusted_.get(); }
    cSC4SimGrid<std::int16_t>* UnadjustedGrid() const { return unadjusted_.get(); }
    float AverageProtection() const { return averageProtection_; }
    float CoverageShare() const { return coverageShare_; }
    float scale = 1.0f;                                  // +0xe4
    std::uint32_t dispatchBitsInUse = 1;                 // +0xf0
    int cellsPerDay = 0;                                 // +0x30
    std::vector<std::vector<std::int8_t>> zoneColumns;   // the zone manager's per-cell zone bytes (+0x98 chain)
    bool flagE8_ = false;                                // +0xe8 (streamed flag; the pass that sets it is unrecovered)
    std::uint32_t valueEC_ = 0;                          // +0xec (streamed scalar; unrecovered)

private:
    IProtectionHost* host_;
    int tractSize_, cellDivisor_;
    RandomIterator walk_;
    SimGridPtr<std::uint8_t> adjusted_;
    SimGridPtr<std::int16_t> unadjusted_;
    std::list<IProtectionStation*> stations_;
    // a city file's grid and station references (the coverage model and the stations are not resolved yet): kept as read
    cRetainedReference fileUnadjusted_, fileAdjusted_, fileThird_;
    cRetainedReferences fileStations_;
    bool fromCityFile_ = false;
    bool WriteCityRecord(cIGZOStream& stream) const;
    int zonedCount_ = 0, protectionSum_ = 0, protectedCount_ = 0;
    float averageProtection_ = 0.0f, coverageShare_ = 0.0f;
};

}  // namespace sc4
