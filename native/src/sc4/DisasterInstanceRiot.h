// cSC4DisasterInstanceRiot — one riot (Mac i386 0x0033a1dc..0x0033ba88, class size 0x74): a list of riot leaders, each a
// pedestrian automaton of group 0x4107 in the riot-leader state with its followers behind it. Once a second every leader's
// strength moves by 1 minus the police power curve at its cell; the strength over the seconds-per-level tuning is the riot
// level, shown as the "Riot<N>" effect on the leader. A level reached for the first time brings the leader its
// "riot_leader_<level>" controller and, by the tuning's table, more leaders around it; a leader whose level falls below zero
// or passes the last level, or who has left the city, goes back to the plain state and leaves the riot. The road tiles in the
// seven by seven cells around a leader are handed to the riot manager, which keeps them from being removed while riots run.
//
// Phases: 0 no leader yet, 1 rioting (the disaster layer counts a running disaster), 3 over.
// Riots start from the mayor rating simulator's hotspots (cSC4AuraSimulator::CheckForStartingRiot) through the riot manager.
//
// The city, the police, the pedestrians and the controllers are the IRiotInstanceHost seam (SL::sp* in the original).
// NOT ported: Write / Read (0x0033a9bc / 0x0033b0f6): the managers do not save their instances natively.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"
#include "sc4/DisasterManagers.h"
#include "sc4/cSC4ResponseCurve.h"

namespace sc4 {

constexpr std::uint32_t kRiotLeaderGroup = 0x4107;             // the automata group of a riot leader
constexpr std::uint32_t kRiotLeaderState = 8, kRiotOverState = 1;
constexpr std::uint32_t kRiotTickPriority = 0xc0;
constexpr std::uint32_t kHistoryRiotsStarted = 0xaa8c87c7;
constexpr std::uint32_t kMessageRiotTick = 0x86ad10ee;         // the animation tick
constexpr std::uint32_t kRiotTuningGroup = 0x29b6c670, kRiotTuningInstance = 0x4a38206d;
constexpr std::uint32_t kPropertyRiotLevels = 0xea400620, kPropertyRiotSecondsPerLevel = 0xea400621, kPropertyRiotValue50 = 0xea400622,
                        kPropertyRiotLeadersPerLevel = 0xea400623, kPropertyRiotPoliceCurve = 0xea400624,
                        kPropertyRiotLocationZoom = 0xca821b8c;
constexpr int kRiotMarkRadius = 3;                             // cells around a leader whose road tiles are marked
constexpr int kRiotRoadSearchTurns = 7;                        // FindNearestRoadTile gives up after this many spiral turns

// A riot leader as the riot drives it (the cISC4Automaton calls of the original, by Mac i386 slot).
class IRiotLeader {
public:
    virtual ~IRiotLeader() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual cISC4Occupant* AsOccupant() = 0;                                              // 0x18
    virtual void GetPosition(float* xyz) = 0;                                             // 0x1c
    virtual void SetVisible(bool visible) = 0;                                            // 0x24
    virtual void AddRegEffect(const cRZString& effect, const cRZString& point) = 0;       // 0xa4
    virtual void RemoveRegEffect(const cRZString& effect) = 0;                            // 0xac
    virtual void SetState(std::uint32_t state, float timer, bool flag) = 0;               // 0xcc (no controller, animation, sound or behavior)
};

class IRiotInstanceHost {
public:
    virtual ~IRiotInstanceHost() = default;
    virtual cISC4DisasterLayer* DisasterLayer() = 0;                                      // city vslot 0x120
    virtual int CellCountX() = 0;                                                         // city vslot 0x174
    virtual int CellCountZ() = 0;                                                         // city vslot 0x178
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;            // city vslot 0x17c
    virtual void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) = 0;      // city vslot 0x180
    virtual float TerrainAltitude(float x, float z) = 0;                                  // terrain vslot 0x30
    virtual std::uint8_t PolicePower(int cellX, int cellZ) = 0;                           // police simulator vslot 0x14, grid vslot 0x1c
    // traffic network map vslot 0x38 (SafeGetTileInfoAtCell), the tile's occupant; null without a tile
    virtual cISC4RiotNetworkOccupant* NetworkOccupantAtCell(int cellX, int cellZ) = 0;
    // network occupant vslot 0x58 (IsOfType) for the types 0, 3 and 6: the tiles a crowd can stand on
    virtual bool IsRiotRoad(cISC4RiotNetworkOccupant* occupant) = 0;
    // pedestrian manager (city vslot 0xb0) vslot 0x3c: CreateAutomaton(group, position, flags 2, no key, no listener, priority
    // 0xff, no velocity); a borrowed pointer, null when the manager refuses
    virtual IRiotLeader* CreateLeader(const float position[3]) = 0;
    virtual void AddController(cISC4Occupant* occupant, const cRZString& templateName) = 0;   // controller manager vslot 0x14
    virtual void RemoveControllers(cISC4Occupant* occupant) = 0;                          // controller manager vslot 0x38
    virtual void RecordHistory(std::uint32_t id, std::int32_t value) = 0;                 // history warehouse vslot 0x30
    virtual void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;     // SL::ATM() vslot 0x14
    virtual void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;  // vslot 0x18
    virtual double RandomUnit() = 0;                                                      // the class's own cRZRandom
    // The tuning exemplar {0x6534284a, 0x29b6c670, 0x4a38206d}; null when the data has none.
    virtual const cISCPropertyHolder* TuningExemplar() = 0;
    virtual bool LoadTuningCurve(cSC4ResponseCurve& curve, std::uint32_t property) = 0;
};
IRiotInstanceHost* RiotInstanceHost();
void SetRiotInstanceHost(IRiotInstanceHost* host);

class cSC4DisasterInstanceRiot final : public cISC4DisasterInstanceRiot, public cIGZMessageTarget2 {
public:
    struct tRiotLeader {
        IRiotLeader* automaton = nullptr;                    // +0 (a reference)
        std::int32_t strength = 0;                           // +4 seconds of unchecked rioting
        std::int32_t level = -1;                             // +8 the level shown now
        std::int32_t maxLevel = -1;                          // +0xc the highest level reached
        cISC4RiotNetworkOccupant* networkOccupant = nullptr; // +0x10 the road tile under the leader (a reference)
    };

    cSC4DisasterInstanceRiot();
    ~cSC4DisasterInstanceRiot();

    bool QueryInterface(std::uint32_t iid, void** out) override;        // @ 0x0033a388
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    // cISC4DisasterInstance
    bool Shutdown() override;                                           // @ 0x0033a256
    cISC4DisasterManager* GetManager() const override { return manager_; }   // @ 0x0033a1f8
    std::uint32_t GetID() const override { return kDisasterManagerIdRiot; }
    bool GetLocation(int&, int&) const override { return false; }
    int GetPhase() const override { return phase_; }                    // @ 0x0033a20a

    // cISC4DisasterInstanceRiot
    bool Init() override;                                               // @ 0x0033ad02
    void AddRiotLeader(int x, int z, bool flag) override { AddLeader(x, z, flag); }

    bool AddLeader(int x, int z, bool fromRiot);                        // @ 0x0033b26c
    void Update();                                                      // @ 0x0033b5bc
    bool DoMessage(cIGZMessage2* message) override;                     // @ 0x0033ba88

    const std::list<tRiotLeader>& Leaders() const { return leaders_; }
    std::uint32_t LevelCount() const { return levels_; }
    std::uint32_t SecondsPerLevel() const { return secondsPerLevel_; }
    const std::vector<std::uint32_t>& LeadersPerLevel() const { return leadersPerLevel_; }
    // Tests: run the once-a-second step now.
    void UpdateLeaders();

private:
    void ReadTuningParameters();                                        // @ 0x0033aa9c
    cISC4RiotNetworkOccupant* FindNearestRoadTile(int& x, int& z);      // @ 0x0033a742
    void MarkRiotNetworkOccupants(int x, int z);                        // @ 0x0033a410
    void UpdateRiotEffects(IRiotLeader* leader, int level);             // @ 0x0033a2e0
    void UpdatePhase();                                                 // @ 0x0033a496

    std::uint32_t refCount_ = 0;
    int cellCountX_ = 0, cellCountZ_ = 0;              // +0x14, +0x18
    std::list<tRiotLeader> leaders_;                   // +0x1c
    bool initialised_ = false;                         // +0x24
    cSC4DisasterManagerRiot* manager_ = nullptr;       // +0x28 (a reference)
    int phase_ = 0;                                    // +0x2c
    cRZTimer timer_{cRZTimer::Units(4)};               // +0x30
    std::uint32_t nextUpdate_ = 0;                     // +0x44
    std::uint32_t levels_ = 0;                         // +0x48
    std::uint32_t secondsPerLevel_ = 0;                // +0x4c
    std::uint32_t value50_ = 0;                        // +0x50 (read from the tuning, used nowhere)
    std::vector<std::uint32_t> leadersPerLevel_;       // +0x54 new leaders a leader brings on reaching each level
    cSC4ResponseCurve policeCurve_;                    // +0x60 police power -> how much a second of rioting is held back
    std::uint8_t locationZoom_ = 0;                    // +0x70
};

}  // namespace sc4
