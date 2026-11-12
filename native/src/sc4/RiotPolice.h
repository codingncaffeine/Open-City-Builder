// cSC4RiotPolice — the figures a patrol car puts down (Mac i386 0x0038de7c..0x0038eace, class id 0x8a6e4fa4, 0x28c bytes; a
// cSC4Pedestrian that never walks). At a call while a disaster runs (the car's dispatch state 5) it is the riot squad: a line of
// officers eight meters ahead of the car towards the call, in the model of the station's competence, with the
// "riot_police_repulsor" controller that pushes a crowd back. A car blocked on its way (dispatch state 1) puts down a sim
// calling for help beside it instead: one of six figures, each with three animations played in random order, facing the camera.
//
// Competence levels: the first tuning threshold the station's competence reaches; the best two share a model. A competence
// under every threshold gives level 3, for which the original reads the word behind its three-entry tables.
#pragma once
#include <cstdint>

#include "sc4/PatrolCar.h"
#include "sc4/Pedestrian.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4RiotPolice = 0x8a6e4fa4;
constexpr std::uint32_t kMessageRiotPoliceStatus = 0xa9e3b6e2;     // data1: -1 the least able squad arrived, 0 leaving
constexpr std::uint32_t kMessageRiotPoliceOn = 0xaa0f3f75;         // data1 = the squad's sound id
constexpr std::uint32_t kMessageRiotPoliceOff = 0x8a0f3f7b;
constexpr float kRiotPoliceCallForHelpDistance = 6.0f;             // 0x00a70874: beside the car
constexpr float kRiotPoliceSquadDistance = 8.0f;                   // 0x00a706a8: ahead of the car
constexpr const char* kRiotPoliceController = "riot_police_repulsor";

class IRiotPoliceHost {
public:
    virtual ~IRiotPoliceHost() = default;
    virtual bool CityCells(int& countX, int& countZ) = 0;                             // city vslots 0x174 / 0x178
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;        // city vslot 0x17c
    virtual void ViewDirection(float xyz[3]) = 0;                                     // SC4ViewUtilities +0x54: the camera's forward
    virtual float CompetenceThreshold(int level) = 0;                                 // tuning +0x108 [3]
    virtual void AddController(cISC4Occupant* occupant, const char* templateName) = 0;   // controller manager vslot 0x14
    // The automata manager's FindExemplarFromModelKey(key) -> +0x28: the exemplar a model belongs to, or null.
    virtual cISCResExemplar* ModelExemplar(const cGZPersistResourceKey& key) = 0;
};
IRiotPoliceHost* RiotPoliceHost();
void SetRiotPoliceHost(IRiotPoliceHost* host);

class cSC4RiotPolice final : public cSC4Pedestrian {
public:
    cSC4RiotPolice(cSC4PatrolCar* car, IPedestrianHost* host);                        // @ 0x0038eace
    ~cSC4RiotPolice() override;

    bool Init() override;                                                             // @ 0x0038e274
    bool Shutdown() override;                                                         // @ 0x0038e17e
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;         // @ 0x0038e4ba
    void SetZoomAndRotation(int zoom, int rotation) override;                         // @ 0x0038e01a
    bool Write(cIGZOStream& stream) const override;                                   // @ 0x0038dec8
    bool Read(cIGZIStream& stream) override;                                          // @ 0x0038df54
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4RiotPolice; }

    void SetCompetence(float competence) { competence_ = competence; }                // @ 0x0038de7c
    void SetupRiotPolice();                                                           // @ 0x0038e618

    std::uint8_t Level() const { return level_; }
    std::uint8_t CallForHelpFigure() const { return figure_; }
    cSC4PatrolCar* Car() const { return car_; }

private:
    void SendRiotPoliceMessage(std::uint32_t type, cISC4Occupant* occupant, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3,
                               std::uint32_t d4);                                     // @ 0x0038e0a4
    void ClampToCityBounds(cS3DVector3& point);                                       // @ 0x0038e1e0

    cSC4PatrolCar* car_ = nullptr;      // +0x280 (a reference)
    float competence_ = 0.0f;           // +0x284
    std::uint8_t level_ = 1;            // +0x288
    bool flag289_ = false;              // +0x289 (saved; nothing reads it)
    std::uint8_t figure_ = 0;           // +0x28a which of the six sims calls for help
};

}  // namespace sc4
