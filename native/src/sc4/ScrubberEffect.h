// cSC4ScrubberEffect (Mac i386 0x003e0dd0..0x003e1d44, 0x5c bytes; made by cSC4ScrubberDescription::Instantiate 0x003e1cb2)
// and its occupant filter (anonymous cScrubberFilter, 0x0076952e..0x00769786).
//
// The scrubber is the part of a visual effect that acts on the city under it: each time the effect enters a new cell it
// burns, contaminates, demolishes and extinguishes around that cell, posts a message and stamps a blob into one of the
// effect maps, as its description says. The disasters do their damage through it: the earthquake's dust effect carries one
// (demolish, radius 1), each volcano creator carries one (demolish and burn), the lightning strike burns with one.
//
// The description (cSC4ScrubberDescription; the port's field names are the 64-bit offsets, the i386 offsets are given):
//   u18 +0x0c flags: 1 leave network pieces, 2 leave flora, 4 the demolition's fifth argument, 8 act once and finish,
//                    0x10 pause the simulator while started, 0x20 hidden-pause it, 0x40 hidden-pause the day clock
//   u24 +0x14 demolish radius in cells (0 = none)       u28 +0x18 the demolition's flags
//   f2c +0x1c smallest occupant spared (0 = none)       f30 +0x20 size from which occupants are spared (0 = none)
//   u34 +0x24 fire radius (0 = none)                    u38 +0x28 toxic spill (0 = none)
//   u3c +0x2c extinguish amount (0 = none)              u40 +0x30 message to post (0 = none)
//   u44 +0x34 the message's first data word until parameter 0xd replaces it
//   u48 +0x38 effect map to stamp (0 = none), f4c +0x3c the value added, v50 +0x40 the blob's half size in cells,
//   f58 +0x48 the blob's spread                         f5c +0x4c seconds until the scrubber stops itself (0 = never)
#pragma once
#include <cstdint>

#include "sc4/EffectComponents.h"

namespace sc4 {

constexpr std::uint32_t kScrubberLeaveNetworks = 1, kScrubberLeaveFlora = 2, kScrubberDemolishOption = 4, kScrubberOnce = 8,
                        kScrubberPause = 0x10, kScrubberHiddenPause = 0x20, kScrubberClockHiddenPause = 0x40;
constexpr std::uint32_t kScrubberOccupantTypeFlora = 0x74758926;
constexpr int kScrubberParamMessageData = 0xd;
constexpr std::uint32_t kScrubberFireCause = 4;

// What the filter asks of an occupant: the three interfaces it answers, its type (vslot 0x1c) and its box (vslot 0x28).
struct ScrubberOccupant {
    bool signpost = false;        // cISC4SignpostOccupant 0x4b44fbe2
    bool textDecal = false;       // cISC4TextDecalOccupant 0x2b3b7d86
    bool network = false;         // cISC4NetworkOccupant 0xa821ef94
    std::uint32_t type = 0;
    float boxMin[3] = {0.0f, 0.0f, 0.0f}, boxMax[3] = {0.0f, 0.0f, 0.0f};
};
// cScrubberFilter::IsOccupantIncluded @ 0x00769786: signposts and text decals never; networks and flora by the flags; an
// occupant whose largest side reaches `maxSize`, or whose smallest side is within `minSize`, is spared.
bool ScrubberFilterIncludes(std::uint32_t flags, float minSize, float maxSize, const ScrubberOccupant& occupant);

class IScrubberHost {
public:
    virtual ~IScrubberHost() = default;
    virtual bool HasSimulator() = 0;                                  // SL::spSimulator
    virtual int CityCellCountX() = 0;                                 // city vslot 0x174
    virtual int CityCellCountZ() = 0;                                 // city vslot 0x178
    // SL::spDemolition vslot 0x18 DemolishRegion(true, the whole rect, privilege 0, flags, option, filter, ...): the filter
    // is ScrubberFilterIncludes with the three values given.
    virtual void DemolishRegion(const std::int32_t rect[4], std::uint32_t flags, bool option, std::uint32_t filterFlags, float minSize,
                                float maxSize) = 0;
    // the fire manager (0x621cda33): IgniteCellRadius(x, z, 0, radius) then SetCause(4)
    virtual void Burn(std::int32_t cellX, std::int32_t cellZ, std::uint32_t radius) = 0;
    virtual void Extinguish(std::int32_t cellX, std::int32_t cellZ, std::int32_t amount) = 0;   // its ExtinguishCell
    virtual void SpillToxic(std::int32_t cellX, std::int32_t cellZ) = 0;   // the toxic manager (0xebfb0404) AttemptDisasterStart(x, z)
    // SL::spMS2: a standard message with data1 = `data`, data2 = cellX, data3 = cellZ, sent at once
    virtual void PostScrubMessage(std::uint32_t type, std::uintptr_t data, std::int32_t cellX, std::int32_t cellZ) = 0;
    // cISC4EffectMaps vslot 0x24 AddRectToMap(map, rect, value, spread)
    virtual void AddRectToEffectMap(std::uint32_t map, const std::int32_t rect[4], float value, std::uint32_t spread) = 0;
    virtual void SimulatorPause(bool pause) = 0;                      // simulator vslots 0x24 / 0x30
    virtual void SimulatorHiddenPause(bool pause) = 0;                // vslots 0x28 / 0x34
    virtual void ClockHiddenPause(bool pause) = 0;                    // SL::sp24HourClock vslot 0x68
};
IScrubberHost* ScrubberHost();
void SetScrubberHost(IScrubberHost* host);

class cSC4ScrubberEffect final : public cISC4VisualEffectComponent {
public:
    explicit cSC4ScrubberEffect(cSC4ScrubberDescription* description);   // @ 0x003e1c00
    ~cSC4ScrubberEffect() override;                                      // @ 0x003e1cf4

    void Start(int transition) override;                                 // @ 0x003e133e
    void Stop(int transition) override;                                  // @ 0x003e1d44
    bool IsStarted() const override { return started_; }                 // @ 0x003e0e28
    void SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) override;   // @ 0x003e109c
    void ApplyEffect(float elapsed) override;                            // @ 0x003e1936
    bool SetFloatParams(int, const float*, int) override { return false; }
    bool SetVoidParams(int type, void** values, int count) override;     // @ 0x003e0e3c

    void DoScrubberEffect();                                             // @ 0x003e17e4
    std::int32_t CellX() const { return cellX_; }
    std::int32_t CellZ() const { return cellZ_; }
    int TimesApplied() const { return applied_; }
    bool BlobInMap() const { return blobInMap_; }

private:
    void DoBurn();                                                       // @ 0x003e1546
    void DoToxic();                                                      // @ 0x003e0e5c
    void DoDemolish();                                                   // @ 0x003e0eaa
    void DoExtinguish();                                                 // @ 0x003e1498
    void AddBlobToMap();                                                 // @ 0x003e16f2
    void RemoveLastBlob();                                               // @ 0x003e1610

    cSC4ScrubberDescription* description_;        // +0x0c (reference held)
    cS3DVector3 position_{0.0f, 0.0f, 0.0f};      // +0x10
    std::int32_t cellX_ = -1, cellZ_ = -1;        // +0x1c, +0x20
    bool cellChanged_ = true;                     // +0x24
    bool started_ = false;                        // +0x25
    float elapsed_ = 0.0f;                        // +0x28
    int applied_ = 0;                             // +0x2c
    std::int32_t blob_[4] = {0, 0, 0, 0};         // +0x30: the cells the blob covers at the current place
    bool blobInMap_ = false;                      // +0x40
    std::int32_t lastBlob_[4] = {0, 0, 0, 0};     // +0x44
    float lastBlobValue_ = 0.0f;                  // +0x54
    std::uintptr_t messageData_ = 0;              // +0x58
};

}  // namespace sc4
