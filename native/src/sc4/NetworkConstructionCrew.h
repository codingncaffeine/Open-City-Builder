// cSC4NetworkConstructionCrew — the crew that builds a newly placed network line (Mac i386 0x00163e54..0x0016722a, vtable
// 0x00bbb928). cSC4NetworkTool::PlaceAllSegments creates one for a real placement of a crewed network type (mask 0x1f4f: the
// roads, rails, streets, avenues, light rail, monorail, one-way roads, dirt roads and ground highways), PlaceNetwork hands it
// the pieces of every solved segment (AddNetworkSegment) and BeginConstruction splits them over crews. Each crew works one
// piece at a time: a "road_construction_dust" particle effect and four to seven construction workers stand on the piece, a
// lead vehicle and a follower drive along the line on the pieces' paths, and when the piece's time is up (the network
// type's crew duration, doubled while a vehicle is still moving) the piece is marked usable (opaque, visible, flag
// 0x10000000) and the crew moves to the next one. The crew is registered with the network manager while it runs and ticks
// on the animation tick (priority 0xc0); it removes itself when every crew has finished. Nothing of it is saved: the
// class is not serializable.
//
// Layout (0x58 bytes): +0x04 piece duration (ms), +0x08 vehicle speed (units per microsecond), +0x0c pen width X (1 = one
// crew walks the pieces in order, otherwise CreateCrewsForSegment puts one crew on each side of a wide line), +0x10
// reference count, +0x14 inited, +0x15 ticking, +0x18 the sound system (counted; the construction messages are only sent
// when there is one), +0x20 / +0x24 lead / follower vehicle model instance, +0x28 crew path type, +0x2c the pieces waiting
// for BeginConstruction, +0x34 the dragged steps, +0x40 their cells, +0x4c the crews.
//
// Every engine global the class reaches goes through INetworkConstructionCrewHost (SL::NetworkManager, spMS2, SL::ATM,
// spSS, spView3D, spCity's vehicle / pedestrian managers, spEffectsManager, spTerrain and ::_sRandom).
//
// Quirks kept (each named where it is ported): GetPathMidpoint compares the edge answers with 0xff although GetEdge and
// SearchForEdge answer 0xffffffff for "none"; AddNetworkSegment rebases the caller's steps in place; the straight branch of
// CreateCrewsForSegment hands the pending pieces to the crews without new references. One deviation: BeginConstruction
// releases the pieces it leaves in the pending list and the original keeps the dangling entries; the port empties the list.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "rz/cRZTimer.h"

class cRZRandomSuper;

namespace sc4 {

// The crew as the network manager sees it (cSC4NetworkManager +0xf0): a counted reference, and its vslot 0x28 (Shutdown)
// when a game reset destroys the crews; the crew unregisters itself there.
class INetworkManagerCrew {
public:
    virtual ~INetworkManagerCrew() = default;
    virtual std::uint32_t AddRef() = 0;
    virtual std::uint32_t Release() = 0;
    virtual bool Shutdown() = 0;   // [5] +0x28
};

class cISC4Occupant;
class cSC4NetworkOccupant;
template <typename T> struct SC4PointT;
struct tDraggedStep;

// A construction vehicle (a cSC4Vehicle of group 0x4002) by the calls the crew makes on it (Mac i386 automaton vslots).
class INetworkCrewVehicle {
public:
    virtual ~INetworkCrewVehicle() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void SetAutomataFlag(std::uint32_t flag, bool on) = 0;                                  // 0x78
    virtual void SetOpacity(float opacity, float seconds) = 0;                                       // 0x84
    virtual void GetPosition(float* xyz) = 0;                                                        // 0x1c
    virtual void GetOrientation(float* xyz) = 0;                                                     // 0x3c
    virtual void MoveTo(const float* position, const float* direction) = 0;                          // 0x40
    virtual void RemoveCallback(cIGZMessageTarget2* target) = 0;                                     // 0xa0
    virtual void SetPath(cSC4NetworkOccupant* piece, std::uint32_t pathKey, bool keepDestination) = 0;   // 0x22c (no point list)
    virtual void SetPathOffset(float offset) = 0;                                                    // 0x248
    // 0x230 InterpolateAlongPath(distance, from, out, no index, no path, no remainder): false at the path's end.
    virtual bool InterpolateAlongPath(float distance, const float* from, float* out) = 0;
};

// A construction worker (a pedestrian of group 0x4101) by the crew's calls.
class INetworkCrewWorker {
public:
    virtual ~INetworkCrewWorker() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void SetAutomataFlag(std::uint32_t flag, bool on) = 0;   // 0x78
    virtual void SetOrientation(const float* xyz) = 0;               // 0x38
    // GetModelInstance (0x164) -> frame count (0x7c) / SetFrame (0x74); a negative count = no model instance.
    virtual int ModelFrameCount() = 0;
    virtual void SetModelFrame(int frame) = 0;
};

// The particle effect on the piece being built (spEffectsManager vslot 0x18, the effect's 0x20 / 0x0c / 0x10).
class INetworkCrewEffect {
public:
    virtual ~INetworkCrewEffect() = default;
    virtual void Release() = 0;
    virtual void SetSourcePosition(const float* xyz) = 0;   // an identity transform translated to the position
    virtual void Start(int transition) = 0;
    virtual void Stop(int transition) = 0;
};

class cSC4NetworkConstructionCrew;
class INetworkConstructionCrewHost {
public:
    virtual ~INetworkConstructionCrewHost() = default;
    virtual void RegisterCrew(cSC4NetworkConstructionCrew* crew) = 0;     // SL::NetworkManager() vslot 0x40
    virtual void UnregisterCrew(cSC4NetworkConstructionCrew* crew) = 0;   // vslot 0x44
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;      // spMS2 vslot 0x14
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;   // spMS2 vslot 0x18
    virtual bool AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;      // SL::ATM() vslot 0x14 (false without one)
    virtual bool RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;   // vslot 0x18
    virtual bool SoundSystemPresent() = 0;                                                 // spSS != null
    // A cRZMessage2Standard posted on spMS2: type, the occupant (SetIGZUnknown), data 1 = the crew, data 2 / data 3.
    virtual void PostConstructionMessage(std::uint32_t type, cISC4Occupant* occupant, cIGZMessageTarget2* crew, std::uint32_t data2,
                                         std::uint32_t data3) = 0;
    virtual int CurrentViewLevel() = 0;                                                    // spView3D vslot 0x58
    virtual bool VehicleManagerPresent() = 0;                                              // spCity vslot 0xac != null
    // The vehicle manager's CreateAutomaton(0x4002, position, 0x3b000, key, no listener, 0x32, no velocity) (vslot 0x3c); the
    // answer carries no reference of the caller's. RemoveVehicle = the manager's RemoveAutomaton(vehicle, false) (vslot 0x20).
    virtual INetworkCrewVehicle* CreateVehicle(std::uint32_t group, const float* position, std::uint32_t flags, const std::uint32_t key[3],
                                               std::uint8_t priority) = 0;
    virtual void RemoveVehicle(INetworkCrewVehicle* vehicle) = 0;
    // The pedestrian manager (spCity vslot 0xb0): CreateAutomaton(0x4101, position, 0x3a000, {0, 0, 0}, 0, 0, 0) / RemoveAutomaton.
    virtual INetworkCrewWorker* CreateWorker(std::uint32_t group, const float* position, std::uint32_t flags) = 0;
    virtual void RemoveWorker(INetworkCrewWorker* worker) = 0;
    virtual INetworkCrewEffect* CreateEffect(const char* name) = 0;                        // one reference, not started
    // spTerrain vslot 0x88 CellHasWater; `terrain` false when there is no terrain.
    virtual bool CellHasWater(std::uint32_t x, std::uint32_t z, bool& terrain) = 0;
    virtual cRZRandomSuper* Random() = 0;                                                  // ::_sRandom
};
void SetNetworkConstructionCrewHost(INetworkConstructionCrewHost* host);
INetworkConstructionCrewHost* NetworkConstructionCrewHost();

constexpr std::uint32_t kNetworkCrewNetworkMask = 0x1f4f;               // PlaceAllSegments: the crewed network types
constexpr std::uint32_t kNetworkCrewTickPriority = 0xc0;
constexpr std::uint32_t kNetworkCrewMessageDone = 0x89f10866;          // a crew starts (data 2 = 1) or finishes (0)
constexpr std::uint32_t kNetworkCrewMessageNextPiece = 0xa9f10e45;     // a crew moves to its next piece
constexpr std::uint32_t kNetworkCrewCountProperty = 0xaa24ae57;        // the crews still working on a piece

class cSC4NetworkConstructionCrew : public cIGZMessageTarget2, public INetworkManagerCrew {
public:
    // tVehicleInfo (0x20 bytes)
    struct tVehicleInfo {
        bool follower = false;                          // +0x00 the second vehicle (it stops behind the crew's piece)
        std::uint32_t modelId = 0;                      // +0x04
        INetworkCrewVehicle* vehicle = nullptr;         // +0x08 (one reference)
        float endPoint[3] = {0, 0, 0};                  // +0x0c where the vehicle drives to
        bool arrived = true;                            // +0x18
        bool onPath = false;                            // +0x19 still driving along its piece's path
        cSC4NetworkOccupant* piece = nullptr;           // +0x1c (one reference)
    };
    // tConstructionCrewData (0x7c bytes)
    struct tConstructionCrewData {
        std::uint32_t duration = 0;                     // +0x00 ms per piece (0 = stopped)
        int count = 0;                                  // +0x04 pieces given to the crew (-1 = finished)
        cSC4NetworkOccupant* current = nullptr;         // +0x08 the piece being built (one reference)
        INetworkCrewEffect* effect = nullptr;           // +0x0c
        std::vector<INetworkCrewWorker*> workers;       // +0x10 (one reference each)
        std::list<cSC4NetworkOccupant*> pieces;         // +0x1c (one reference each)
        cRZTimer timer{cRZTimer::kMilliseconds};        // +0x24
        bool reverse = false;                           // +0x38 walk the pieces' paths backwards
        tVehicleInfo lead;                              // +0x3c
        tVehicleInfo follow;                            // +0x5c
    };

    cSC4NetworkConstructionCrew();
    ~cSC4NetworkConstructionCrew() override;

    // cIGZUnknown / cIGZMessageTarget2
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x00164354: only cIGZMessageTarget2 (0x452294aa)
    std::uint32_t AddRef() override;                               // 0x00164342
    std::uint32_t Release() override;                              // 0x001655d8
    bool DoMessage(cIGZMessage2* message) override;                // 0x0016722a

    // vslot 0x10: pen width X, crew duration (s), vehicle speed (units per second), lead / follower model, crew path type
    virtual bool Init(std::uint32_t penWidthX, float durationSeconds, float speed, std::uint32_t leadModel, std::uint32_t followModel,
                      std::uint32_t pathType);                     // 0x001657ca
    bool Shutdown() override;                                      // 0x001656b0 (vslot 0x14)
    static bool StaticInit();                                      // 0x0016430a
    static bool StaticShutdown();                                  // 0x00164330

    // 0x00164b88: take the pieces of one solved segment (those on the segment's cells move into the pending list, the rest are
    // marked usable at once). `steps` are rebased onto the crew's cell list in place.
    void AddNetworkSegment(std::list<cSC4NetworkOccupant*>& pieces, std::vector<tDraggedStep>& steps,
                           const std::vector<SC4PointT<std::uint32_t>>& cells);
    void BeginConstruction();                                      // 0x00166b66

    // The static helpers the network tool shares (0x0016429e / 0x00164690).
    static void MarkOccupantUsable(cSC4NetworkOccupant* piece);
    static void MarkOccupantsUsable(std::list<cSC4NetworkOccupant*>& pieces, bool release);
    static std::uint32_t GetNetworkOccupantCrewCount(cSC4NetworkOccupant* piece);      // 0x0016437e
    static std::uint32_t RemoveCrewFromNetworkOccupant(cSC4NetworkOccupant* piece);    // 0x001643c4
    static std::uint32_t AddCrewToNetworkOccupant(cSC4NetworkOccupant* piece);         // 0x0016445c

    // Read-only views for reports and tests.
    const std::vector<tConstructionCrewData>& Crews() const { return crews_; }
    const std::list<cSC4NetworkOccupant*>& PendingPieces() const { return pending_; }
    std::size_t StepCount() const;
    bool IsInited() const { return inited_; }
    bool IsTicking() const { return ticking_; }
    std::uint32_t Duration() const { return duration_; }
    float Speed() const { return speed_; }
    void DoAnimationTick(std::uint32_t microseconds);              // 0x001670c6
    void DoOccupantRemoved(cISC4Occupant* occupant);               // 0x001662a6

private:
    std::uint32_t GetEdge(cSC4NetworkOccupant* from, cSC4NetworkOccupant* to);                          // 0x00163e54
    std::uint32_t SearchForEdge(cSC4NetworkOccupant* piece, std::uint32_t edge, bool asEntry);          // 0x00163ee4
    bool GetPathMidpoint(cSC4NetworkOccupant* piece, cSC4NetworkOccupant* previous, cSC4NetworkOccupant* next, float* out,
                         std::uint32_t& pathKey, bool reverse);                                         // 0x00164006
    void DestroyVehicle(tVehicleInfo& info);                                                            // 0x001644ec
    void SendConstructionMessage(std::uint32_t type, cISC4Occupant* occupant, std::uint32_t data2, std::uint32_t data3);   // 0x00164576
    static std::list<cSC4NetworkOccupant*>::iterator FindNetworkOccupantForCell(std::list<cSC4NetworkOccupant*>& pieces, std::uint32_t x,
                                                                                std::uint32_t z);       // 0x00164642
    bool GetNextNetworkOccupant(std::list<cSC4NetworkOccupant*>& pieces, cSC4NetworkOccupant* piece, cSC4NetworkOccupant*& next,
                                bool& adjacent);                                                        // 0x001646d6
    bool AdvanceConstructionCrewSearchCell(int& x, int& z, int dx, int dz, bool& stepX, bool& stepZ, std::uint32_t endCell);   // 0x0016479c
    void CreateConstructionVehicle(cSC4NetworkOccupant* piece, tVehicleInfo& info);                     // 0x0016480e
    void StopConstruction(tConstructionCrewData& crew, bool final);                                     // 0x001648f4
    void InitConstructionCrew(tConstructionCrewData& crew);                                             // 0x00164a4c
    INetworkCrewEffect* CreateConstructionParticles(cSC4NetworkOccupant* piece);                        // 0x00164abe
    void CreateConstructionAutomata(tConstructionCrewData& crew);                                       // 0x00164fec
    void PlaceVehicle(cSC4NetworkOccupant* piece, cSC4NetworkOccupant* next, tVehicleInfo& info, bool reverse);   // 0x0016590a
    bool SetVehicleEndPoint(std::list<cSC4NetworkOccupant*>& pieces, cSC4NetworkOccupant* crewPiece, tVehicleInfo& info, bool reverse);   // 0x00165c3e
    bool MoveVehicleToNextCell(std::list<cSC4NetworkOccupant*>& pieces, tVehicleInfo& info, cSC4NetworkOccupant* crewPiece, bool reverse);   // 0x00165dc8
    void PlaceLeadVehicle(std::list<cSC4NetworkOccupant*>& pieces, cSC4NetworkOccupant* piece, tVehicleInfo& info, bool reverse);   // 0x00165f72
    void StartLeadVehicleMoving(tConstructionCrewData& crew);                                           // 0x00166046
    bool MoveConstructionCrewToNextCell(tConstructionCrewData& crew);                                   // 0x001660d0
    void CreateConstructionCrewParticleEffect(tConstructionCrewData& crew);                             // 0x001663c4
    int CreateCrewsForSegment(int firstStep, int lastStep, std::list<cSC4NetworkOccupant*>& pending);    // 0x00166488
    void AnimateVehicle(std::uint32_t microseconds, tVehicleInfo& info, tConstructionCrewData& crew);   // 0x00166e94

    std::uint32_t duration_ = 0;                        // +0x04
    float speed_ = 0.0f;                                // +0x08
    std::uint32_t penWidthX_ = 0;                       // +0x0c
    std::uint32_t refs_ = 0;                            // +0x10
    bool inited_ = false;                               // +0x14
    bool ticking_ = false;                              // +0x15
    bool soundSystem_ = false;                          // +0x18
    std::uint32_t leadModel_ = 0;                       // +0x20
    std::uint32_t followModel_ = 0;                     // +0x24
    std::uint32_t pathType_ = 0;                        // +0x28
    std::list<cSC4NetworkOccupant*> pending_;           // +0x2c (one reference each)
    std::vector<tDraggedStep> steps_;                   // +0x34
    std::vector<SC4PointT<std::uint32_t>> cells_;       // +0x40
    std::vector<tConstructionCrewData> crews_;          // +0x4c
};

}  // namespace sc4
