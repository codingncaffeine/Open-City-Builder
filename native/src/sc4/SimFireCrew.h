// cSC4SimFireCrew — the fire crew a fire engine puts down at a fire or a toxic spill (Mac i386 0x0038eb7c..0x0039119e, 0x2d4 bytes;
// a cSC4Pedestrian that never walks). At the scene (the engine's dispatch state 5) the crew is a hose team and its director: every
// animation tick it looks for the nearest burning fire occupant (then the nearest active spill) within the hose radius, in front
// of the engine, and hoses it: the fire's hose count goes up by 2 (3 when the player aims the stream, 1 for the least competent
// crew), which is what makes cSC4DisasterInstanceFire wind the fire down. A water (or foam) stream effect runs from the hose
// registration point of the crew model to the target, with a splash where it lands. When nothing is left the crew tells the
// engine (HandleCrewFinished). An engine blocked on its way (dispatch state 1) puts down a single figure calling for help instead.
//
// Competence levels: 0 and 1 use the trained crew's models and the straight stream, 2 (competence below every threshold of the
// tuning table) the clumsy one whose stream sputters and wanders.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/FireOccupant.h"
#include "sc4/FireTruck.h"
#include "sc4/OccupantManager.h"
#include "sc4/Pedestrian.h"
#include "sc4/ToxicOccupant.h"

namespace sc4 {

constexpr std::uint32_t kMessageFireCrewStatus = 0xa9e3b6e2;      // data1: -1 clumsy crew arrived, 1 finished, 0 leaving
constexpr std::uint32_t kMessageFireCrewHoseOn = 0xaa0f3f75;      // data1 = the crew's sound id
constexpr std::uint32_t kMessageFireCrewHoseOff = 0x8a0f3f7b;

// A running visual effect as the crew drives it (cISC4VisualEffect vslots 0xc, 0x10, 0x14, 0x20, 0x2c).
class ICrewEffect {
public:
    virtual ~ICrewEffect() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void Start(int transition) = 0;
    virtual void Stop(int transition) = 0;
    virtual bool IsStarted() = 0;
    virtual void SetSourceTransform(const cS3DTransform& transform) = 0;
    virtual void SetTarget(const cS3DVector3& target) = 0;           // SetVectorParams(5, &target, 1)
};

class ISimFireCrewHost {
public:
    virtual ~ISimFireCrewHost() = default;
    virtual cIGZMessageServer2* MessageServer() = 0;                                  // SL::spMS2
    virtual ICrewEffect* CreateEffect(const char* name) = 0;                          // effects manager vslot 0x18; one reference
    // occupant manager vslot 0x94 GetOccupantsByBBox(list, min, max, filter, all): a reference each, false when none
    virtual bool OccupantsInBox(const float min[3], const float max[3], cSC4OccupantFilter* filter, OccupantList& out) = 0;
    virtual bool CityCells(int& countX, int& countZ) = 0;                             // city vslots 0x174 / 0x178
    virtual void ViewDirection(float xyz[3]) = 0;                                     // SC4ViewUtilities +0x54: the camera's forward
    virtual std::uint32_t RandomBelow(std::uint32_t range) = 0;                       // cSC4AutomataManager's generator
    virtual std::uint32_t RandomUint32() = 0;
    virtual double RandomRange(double lo, double hi) = 0;
    virtual float CrewDistance() = 0;                                                 // tuning +0x100: the hose team, ahead of the engine
    virtual float DirectorDistance() = 0;                                             // tuning +0x104
    virtual float CompetenceThreshold(int level) = 0;                                 // tuning +0x108 [3]
    virtual float HoseRadius() = 0;                                                   // tuning +0x114
    virtual void MoveModelInRenderer(IAutomatonModel* model) = 0;                     // renderer vslot 0x78
    // The translations of a model's named registration point, one per animation frame; false when the model has none.
    virtual bool RegPointTranslations(IAutomatonModel* model, const char* name, std::vector<cS3DVector3>& out) = 0;
};
ISimFireCrewHost* SimFireCrewHost();
void SetSimFireCrewHost(ISimFireCrewHost* host);

// Spills that still hold waste.
class cSC4ActiveToxicOccupantFilter final : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type == kOccupantTypeToxic; }
};
// Fires that still have their target (the same test as the fire instance's filter; here without the disaster headers).
class cSC4CrewFireOccupantFilter final : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override;
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type == kOccupantTypeFire; }
};

class cSC4SimFireCrew final : public cSC4Pedestrian {
public:
    cSC4SimFireCrew(cSC4FireTruck* truck, IPedestrianHost* host);                    // @ 0x0039119e
    ~cSC4SimFireCrew() override;

    bool Init() override;                                                             // @ 0x0038f616
    bool Shutdown() override;                                                         // @ 0x0038f0aa
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;         // @ 0x003907f4
    bool DoMessage(cIGZMessage2* message) override;                                   // @ 0x0038eef8
    void SetZoomAndRotation(int zoom, int rotation) override;                         // @ 0x0038ebc8
    bool Write(cIGZOStream& stream) const override;                                   // @ 0x0038f448
    bool Read(cIGZIStream& stream) override;                                          // @ 0x0038f32e

    void SetCompetence(float competence) { competence_ = competence; }                // @ 0x0038eb7c
    void SetupFireCrew();                                                             // @ 0x0038f838
    void HideEffects();                                                               // SetVisible(false)'s own part (0x0038ec6e)

    std::uint8_t Level() const { return level_; }
    cSC4FireOccupant* ClosestFire() const { return closestFire_; }
    cSC4ToxicOccupant* ClosestSpill() const { return closestSpill_; }
    int FoundCount() const { return foundCount_; }

private:
    void SetClosestFire(cSC4FireOccupant* fire, bool aimed);                          // @ 0x0038ed02
    void SetClosestToxicSpill(cSC4ToxicOccupant* spill, bool aimed);                  // @ 0x0038ee6a
    void SendFireCrewMessage(std::uint32_t type, cISC4Occupant* occupant, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3,
                             std::uint32_t d4);                                       // @ 0x0038ed90
    void ClampToCityBounds(cS3DVector3& point);                                       // @ 0x0038f016
    void GetWaterHoseTransform(cS3DTransform& out);                                   // @ 0x0038f1fa
    void InitRegTranslationArray();                                                   // @ 0x0038f520
    bool FindTargetsInRadius(const cS3DVector3* point);                               // @ 0x00390024
    int HoseStrength(bool aimed) const { return level_ == 2 ? 1 : (aimed ? 3 : 2); }
    void StopEffect(ICrewEffect* effect, int transition);

    cSC4FireTruck* truck_ = nullptr;                   // +0x280 (a reference)
    float competence_ = 0.0f;                          // +0x284
    std::uint8_t level_ = 1;                           // +0x288
    ICrewEffect* stream_ = nullptr;                    // +0x28c
    ICrewEffect* splash_ = nullptr;                    // +0x290
    std::vector<cS3DVector3> hosePoints_;              // +0x294 the hose registration point, per frame
    cSC4FireOccupant* closestFire_ = nullptr;          // +0x2a0 (a reference)
    cSC4ToxicOccupant* closestSpill_ = nullptr;        // +0x2a4 (a reference)
    cSC4OccupantFilter* fireFilter_ = nullptr;         // +0x2a8
    cSC4OccupantFilter* spillFilter_ = nullptr;        // +0x2ac
    std::int32_t foundCount_ = 0;                      // +0x2b0 ticks with a target
    bool reportedDone_ = false;                        // +0x2b4
    cS3DVector3 target_{};                             // +0x2b8 where the stream lands
    float streamTime_ = 0.0f;                          // +0x2c4 seconds the stream has run
    cS3DVector3 wander_{};                             // +0x2c8 the clumsy crew's aim
};

}  // namespace sc4
