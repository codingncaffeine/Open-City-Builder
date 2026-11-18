// cSC4DisasterInstancePipeBurst — a burst water main (31 functions at 0x1004553f4..0x100456802, instance iid
// 0x22223ba4, disaster id 0x4a1332e8). Starting it makes the chosen pipe leak (a particle effect plays on it); after a
// random delay of 5..10 seconds the leaking pipe explodes: its neighbouring pipes start leaking three animation ticks
// later, the pipe is demolished, a sink hole is dug where it stood and the cell is announced with message 0xea1f0dcd.
// The disaster ends when no pipe is leaking or waiting any more. Building or removing a pipe in a leaking cell stops
// that leak. The view level (1 above ground, 3 underground) decides which effect plays.
// Quirks kept: GetLocation always fails and the damaged count is always 0; a burst pipe's effect is stopped but not
// released; the burst pipe's neighbours are the four edge neighbours inside the city.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "rz/cRZRandom.h"
#include "sc4/DisasterManagers.h"

namespace sc4 {

constexpr std::uint32_t kMessagePipeBurstCell = 0xea1f0dcd;       // x, z of the exploded pipe
constexpr std::uint32_t kPipeBurstMessageAnimationTick = 0x86ad10ee;       // data 2 = elapsed milliseconds
constexpr std::uint32_t kMessagePipeOccupantRemoved = 0x99ef1143;   // (kMessageOccupantInserted = 0x99ef1142 is in Occupant.h)
constexpr std::uint32_t kPipeBurstMessageViewLevelChanged = 0xc6cdbf1a;
constexpr std::uint32_t kNotificationCityTick = 0xc0;
constexpr std::uint32_t kPropertyLeaveAloneOccupant = 0x029244db;   // occupants with this property are ignored when inserted

// What the burst needs of the running game besides IDisasterHost.
struct SinkHoleDesc {
    float x = 0, z = 0;          // metres
    bool flag = true;
    float scaleX = 1.0f, scaleZ = 1.0f;
    int mode = 3;
    float strength = 100.0f;
};
class IPipeBurstHost {
public:
    virtual ~IPipeBurstHost() = default;
    virtual bool CreateEffect(const char* name, cISC4VisualEffect** out) = 0;    // effects manager vslot 0x30
    virtual float TerrainCellSize() = 0;                                         // terrain vslot 0x30
    virtual void AddSinkHole(int decalType, const SinkHoleDesc& desc) = 0;       // terrain vslot 0x1c8
    virtual bool HasDemolition() = 0;                                            // SL::spDemolition present
    virtual void DemolishPipe(cISC4Occupant* occupant) = 0;                      // demolition vslot 0x40(1, occupant, 0, 0x302, ...)
    virtual int ViewLevel() { return 1; }                                        // SL::spView3D vslot 0xb0
    virtual bool ReadTuning(float& minSeconds, float& maxSeconds, int& goToZoomLevel) = 0;   // exemplar 0x29b6c670 / 3
    virtual void RegisterShutdownCallback(bool (*callback)(void*)) { (void)callback; }   // SL::spSC4 vslot 0x218
};
IPipeBurstHost* PipeBurstHost();
void SetPipeBurstHost(IPipeBurstHost* host);

class cSC4DisasterInstancePipeBurst final : public cISC4DisasterInstancePipeBurst, public cIGZMessageTarget2 {
public:
    cSC4DisasterInstancePipeBurst() = default;
    ~cSC4DisasterInstancePipeBurst();                                         // @ 0x100455e80

    bool QueryInterface(std::uint32_t iid, void** out) override;                        // @ 0x10045647a
    std::uint32_t AddRef() override { return ++refCount_; }                             // @ 0x1004558d2
    std::uint32_t Release() override;                                                   // @ 0x1004565cc
    bool Init(cIGZUnknown* pipeOccupant) override;                                      // @ 0x1004553f4
    bool Shutdown() override;                                                           // @ 0x1004565f2
    cISC4DisasterManager* GetManager() const override { return manager_; }              // @ 0x10045630c
    std::uint32_t GetID() const override { return kDisasterManagerIdPipeBurst; }        // @ 0x100456802
    bool GetLocation(int&, int&) const override { return false; }                       // @ 0x1004562fc
    int GetPhase() const override { return 1; }                                         // @ 0x100456338
    int GetDamagedOccupantsCount() const { return 0; }                                  // @ 0x1004562f4
    cISC4DisasterInstance* AsISC4DisasterInstance() { return this; }                    // @ 0x1004558f8
    bool DoMessage(cIGZMessage2* message) override;                                     // @ 0x10045607a

    void DoAnimationTick(std::uint32_t elapsedMs);                                      // @ 0x100455f46
    void TickLeaksInWait();                                                             // @ 0x100456022
    void DoViewLevelChange(int level);                                                  // @ 0x100456132
    void DoOccupantInserted(cISC4Occupant* occupant);                                   // @ 0x10045617e
    void DoOccupantRemoved(cISC4Occupant* occupant);                                    // @ 0x1004561f4
    void StopLeakAtCell(int x, int z);                                                  // @ 0x10045627c
    bool StartLeakAtPipe(cISC4PipeOccupantView* pipe);                                  // @ 0x10045638a
    void StartLeakAtCells(const std::vector<std::pair<int, int>>& cells);               // @ 0x10045675a
    void BurstPipe(int x, int z);                                                       // @ 0x100455902
    cISC4PipeOccupantView* GetPipeAtLocation(int x, int z);                             // @ 0x100455ad2 (counted reference)
    void CreateParticleEffect(const char* name, const float* position, bool keep, cISC4VisualEffect** out);   // @ 0x100455b32
    void PostMessageA(std::uint32_t type, std::uint64_t d1, std::uint64_t d2, std::uint64_t d3);   // @ 0x100455c5c
    void CreateSinkHole(int x, int z);                                                  // @ 0x100455d2a

    static bool StaticInit();                                                           // @ 0x100456350
    static bool StaticShutdown(void*);                                                  // @ 0x1004567ac
    static void ReadTuningParameters();                                                 // @ 0x1004564fe
    static void SeedRandom(std::uint32_t seed);

    // The leaking pipes and the leaks still waiting to start (public for tests).
    struct tLeakyPipe {                                // list node payload at +0x10
        float position[3] = {0, 0, 0};
        int cellX = 0, cellZ = 0;
        std::uint32_t elapsed = 0, burstTime = 0;
        cISC4VisualEffect* effect = nullptr;
    };
    struct tLeaksInWait {
        int delay = 0;
        std::vector<std::pair<int, int>> cells;
    };
    void CreateLeakEffectForPipe(tLeakyPipe& pipe);                                     // @ 0x100455dcc
    const std::list<tLeakyPipe>& Leaks() const { return leaks_; }
    const std::list<tLeaksInWait>& Waiting() const { return waiting_; }
    int ViewLevelValue() const { return viewLevel_; }

    static inline int kMinLeakTimeBeforeBurst = 5000000, kMaxLeakTimeBeforeBurst = 10000000, kGoToZoomLevel = 0;

private:
    std::uint32_t refCount_ = 0;
    bool initialised_ = false;                          // +0x20
    cISC4DisasterManager* manager_ = nullptr;           // +0x28
    cSC4WateredPipeFilter* filter_ = nullptr;           // +0x40
    int viewLevel_ = 1;                                 // +0x48
    std::list<tLeakyPipe> leaks_;                       // +0x50
    std::list<tLeaksInWait> waiting_;                   // +0x70
    int cellsX_ = 0, cellsZ_ = 0;                       // +0x68, +0x6c
    static inline bool sbInited = false;
};

}  // namespace sc4
