// cSC4ObliterateCity — the view input control that plays the "Obliterate City" scene (PC 0x0049C790..0x0049D820, spec OpenSC4
// region-city-dialogs.md section 5): blocks of the city vanish in a random order with sparkles, a smoke screen forms, then
// everything left is removed at once and the city is dezoned; when the finale effect ends (or after 20 s) the scene posts
// CityObliterationSceneEnded (0xea65224e) and the application rebuilds the tile as a fresh God-mode city (application.md 11.6).
// Control id 0xca58c969, cursor 15 (busy). The animation tick and the finale effect's end message reach the +0x28 message
// target (DoAnimationTick 0x0049D820).
// Quirks kept: Escape ends the scene in any state and leaves the city partly razed (saving allowed again, the simulator
// resumes, nothing rebuilt); the tuning is read once per process; the isolated-removal target advances even when fewer
// blocks remain; an effect end message in state 2 ends the scene before the bulk removal.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "sc4/OccupantManager.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

constexpr std::uint32_t kObliterateCityToolId = 0xca58c969;
constexpr std::uint32_t kObliterateCursorBusy = 15;
constexpr std::uint32_t kMessageObliterateEffectEnded = 0xea623590;
constexpr std::uint32_t kMessageCityObliterationSceneEnded = 0xea65224e;
constexpr std::uint32_t kMessageObliterateRadioFlag = 0xaa650861;   // data1 = 1
constexpr std::uint32_t kMessageObliterateRadioStop = 0x2a611b51;
constexpr std::uint32_t kObliterateTickPriority = 0xc0;
constexpr std::uint32_t kOccupantTypeFlora = 0x74758926, kOccupantTypeFire = 0x69d25c5f;

// A running particle effect the tool keeps (the finale): its stop.
class IObliterateEffect {
public:
    virtual ~IObliterateEffect() = default;
    virtual void Stop(bool hard) = 0;                                  // Stop(0) soft, Stop(1) hard
    virtual void Release() = 0;
};

// The services behind SL::spCity, spSimulator, spDemolition, spOccupantManager, spZoneManager, spEffectsManager / spTerrain,
// the animation tick manager, message server 2 and the "ObliterateCity" tuning exemplar.
class IObliterateCityHost {
public:
    virtual ~IObliterateCityHost() = default;
    struct Tuning {
        std::uint32_t blockSize = 8;                                   // 0x6a5a6700
        std::vector<std::pair<float, float>> curve;                    // 0x6a5a6701: seconds -> blocks removed by then
        float smokeSeconds = 0.25f;                                    // 0x6a5a6702
    };
    virtual Tuning ReadTuning() = 0;                                   // exemplar 0x6534284a / 0x7a4a8458 / 0xca5a576f
    virtual void CityCellCount(int& x, int& z) = 0;
    virtual void DisableSave() = 0;                                    // city slot 0x1a0
    virtual void EnableSave() = 0;                                     // city slot 0x19c
    virtual void SimulatorHiddenPause() = 0;                           // simulator slot 0x28
    virtual void SimulatorHiddenResume() = 0;                          // simulator slot 0x34
    virtual bool SituationIndicatorVisible() = 0;                      // city situation manager GetCSIVisible
    virtual void SetSituationIndicatorVisible(bool visible) = 0;       // slot 29
    virtual cIGZMessageServer2* Messages() = 0;                        // message server 2
    virtual void AddTick(cIGZMessageTarget2* target, std::uint32_t priority) = 0;
    virtual void RemoveTick(cIGZMessageTarget2* target, std::uint32_t priority) = 0;
    // DemolishRegion over the cells (corners inclusive): commit, privilege 0, flags 0x2000, no dezoning, the tool's filter
    virtual void DemolishCells(int x0, int z0, int x1, int z1, cISC4OccupantFilter* filter) = 0;
    // GetOccupantsByStandardCityCells with the filter, at most one: is anything there
    virtual bool CellsOccupied(int x0, int z0, int x1, int z1, cISC4OccupantFilter* filter) = 0;
    virtual void RemoveOccupants(cISC4OccupantFilter* filter) = 0;    // occupant manager RemoveOccupants(filter, shutdown, 0)
    virtual void DezoneCity(int maxX, int maxZ) = 0;                  // zone manager PlaceZone(whole city, type 0, commit ...)
    // CreateParticleEffect(name, (x, terrain height, z), 0, keep) and, when endMessage is not 0, SetEndMessage(endMessage, tool)
    // (effect slot 10): the effect when kept (null otherwise or without effects)
    virtual IObliterateEffect* CreateEffect(const char* name, float x, float z, bool keep, std::uint32_t endMessage) = 0;
};
IObliterateCityHost* ObliterateCityHost();
void SetObliterateCityHost(IObliterateCityHost* host);

class cSC4ObliterateCity : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    enum State : int { kIdle = 0, kIsolatedRemoval = 1, kSmokeForming = 2, kFinale = 3, kEnded = 4 };

    cSC4ObliterateCity() = default;                                   // 0x0049CF90
    ~cSC4ObliterateCity() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;     // 0x00661D70
    std::uint32_t AddRef() override { return cSC4ViewInputControl::AddRef(); }
    std::uint32_t Release() override { return cSC4ViewInputControl::Release(); }

    bool Init() override;                                              // 0x0049D390
    bool Shutdown() override;                                          // 0x0049C8B0
    bool OnKeyDown(std::uint32_t key, std::uint32_t mods) override;    // 0x0049C790
    bool OnMouseDownL(int, int, std::uint32_t) override { return true; }   // 0x0049C7B0
    bool OnMouseUpL(int, int, std::uint32_t) override { return true; }
    bool OnMouseMove(int, int, std::uint32_t) override { return true; }
    bool DoMessage(cIGZMessage2* message) override;                    // DoAnimationTick 0x0049D820

    State GetState() const { return static_cast<State>(state_); }
    int BlocksProcessed() const { return processed_; }
    std::size_t BlocksLeft() const { return blocks_.size() - next_; }

private:
    class Filter;
    void Tick(std::uint64_t elapsedUs);
    void DoIsolatedRemoval();                                          // 0x0049D5A0
    void StartSmokeScreenFormingState();                               // 0x0049CD90
    void StartFinale();                                                // 0x0049D070 / 0x0049CEA0
    void EndScene();                                                   // 0x0049CC30
    void PostMessage(std::uint32_t type, std::uint64_t data1);         // 0x0049CA70

    int state_ = kIdle;
    std::uint64_t sceneUs_ = 0, smokeUs_ = 0, finaleUs_ = 0;
    int processed_ = 0, skipped_ = 0, removedTarget_ = 0;
    int maxX_ = 0, maxZ_ = 0;
    std::vector<std::pair<int, int>> blocks_;                          // the block iterator, shuffled
    std::size_t next_ = 0;
    cISC4OccupantFilter* filter_ = nullptr;
    IObliterateEffect* finale_ = nullptr;
    bool csiWasVisible_ = false;
};

}  // namespace sc4
