// The concrete advisor classes a city file holds, one per "advisors" row, and their immediate base cSC4AdvisorBase.
// They all derive from cSC4AdvisorBase (which derives from the recovered cSC4Advisor) and share one pattern: each
// adds a small data record on top of the base layout, overrides GetGZCLSID / Init / Shutdown / DoMessage and the
// update_variables virtual, subscribes to a few message types, and turns building / simulator messages into Lua
// globals (g_* / l_*) through the advisor system.
//
// Original object layout (the base cSC4Advisor is 0x148 bytes and ends with its bool at +0x140; the derived classes
// repurpose the trailing padding):
//   cSC4AdvisorBase   +0x141 bool update pending   +0x142 bool subscribed to the five base messages
//                     +0x144 std::uint32_t dirty mask (which update_* to run on the deferred 0xca8f3db6 self-update)
//                     size 0x148; adds the virtual update_variables() (primary vtable slot 0x100) and overrides
//                     simulate_day / simulate_month so a pending update runs update_variables first.
//   cSC4AdvisorHQ     +0x148 bool group vectors initialized   size 0x150
//   cSC4AdvisorFinance +0x148 bool (unused flag kept for layout)   size 0x150
//   cSC4AdvisorUtility +0x148 bool subscribed to its three messages   size 0x150
//   cSC4AdvisorSafety  +0x148 bool subscribed to its sixteen messages   size 0x150
//   cSC4AdvisorTraffic +0x148 std::set<void*> car zot occupants   +0x160 bool subscribed to 0x4c04de69   size 0x168
//   cSC4AdvisorCityPlanning +0x148 bool subscribed to its eight messages   +0x14c int building count
//                     +0x150 int park count   +0x154 int recreation count   +0x158 std::map<int,int> landmarks
//                     size 0x170; also adds QueryInterface (answers its own class id), Write/Read (version 3, the
//                     landmarks map is only serialized from version 3 on) and GetLandmarkCount / HasLandmark.
//
// Quirks kept:
//  * post_self_update posts a 0xca8f3db6 message straight back to the advisor (data1 = the update kind, data2 = an
//    advice guid); the dirty mask is set by the building messages and acted on only when that deferred message
//    arrives, so a building change updates the targeted occupant variables, not the whole record.
//  * Finance sets the dirty mask on a building change but its self-update handler only clears it (no targeted update).
//  * cSC4AdvisorCityPlanning::QueryInterface answers its own class id (0x2a5f877d) as the primary interface id.
//  * GetLandmarkCount / HasLandmark walk the landmarks map directly and answer 0 / false for a missing type; the
//    removal path decrements the count instead of erasing it (the value may go negative).
//  * cSC4AdvisorCityPlanning::DoMessage checks the park property (0x1006) twice: once for the park count and once
//    for the recreation count (the original's bug, kept).
//  * Utility's 0xea418d8f handler reads an event id from a static table keyed by the message data word (3..6) and
//    pushes that event with a subject that carries the message's unknown object.
//
// Everything the original reaches through globals (SL::spMS2, cSC4AdvisorSystem::spInstance, the simulators, GZCOM,
// SL::spBuildingDevelopmentSim / SL::spCivicBuildingSim, the property holder behind a message's void1) goes through
// IAdvisorConcreteHost.
#pragma once
#include <cstdint>
#include <map>
#include <set>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/Advisor.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4AdvisorBase = 0xca09f543;          // GetGZCLSID @ 0x100444c90
constexpr std::uint32_t kGZCLSID_cSC4AdvisorCityPlanning = 0x2a5f877d;  // GetGZCLSID @ 0x1004027fe
constexpr std::uint32_t kGZCLSID_cSC4AdvisorTraffic = 0x4a3ad3e1;       // GetGZCLSID @ 0x100449b16
constexpr std::uint32_t kGZCLSID_cSC4AdvisorHQ = 0x6a5f8755;            // GetGZCLSID @ 0x100447784
constexpr std::uint32_t kGZCLSID_cSC4AdvisorFinance = 0xaa5292d7;       // GetGZCLSID @ 0x100447eee
constexpr std::uint32_t kGZCLSID_cSC4AdvisorUtility = 0xca2c2a2f;       // GetGZCLSID @ 0x1004415aa
constexpr std::uint32_t kGZCLSID_cSC4AdvisorSafety = 0xea19e825;        // GetGZCLSID @ 0x10044b9a2

// The base message types cSC4AdvisorBase subscribes to in Init.
constexpr std::uint32_t kAdvisorBaseMessageBuildingChanged = 0x99ef1142;   // +1 direction (data: a property holder)
constexpr std::uint32_t kAdvisorBaseMessageBuildingChanged2 = 0x99ef1143;  // -1 direction
constexpr std::uint32_t kAdvisorBaseMessageSelfUpdate = 0xca8f3db6;        // posted by post_self_update
constexpr std::uint32_t kAdvisorBaseMessageRewardAdviceReeval = 0xea8ae29a;
constexpr std::uint32_t kAdvisorBaseMessagePostUpdate2 = 0xc2c09554;       // handled as post_self_update(2, 0)
// cSC4AdvisorCityPlanning's eight messages (Init @ 0x1004022ea).
constexpr std::uint32_t kAdvisorCityPlanningMessagePlop = 0xea394b74;
constexpr std::uint32_t kAdvisorCityPlanningMessageBulldoze = 0xcb3be815;
constexpr std::uint32_t kAdvisorCityPlanningMessageBulldoze2 = 0xeb3be415;
constexpr std::uint32_t kAdvisorCityPlanningMessageRezone = 0x2b3be44a;
constexpr std::uint32_t kAdvisorCityPlanningMessageRezone2 = 0x6b3be6d8;
constexpr std::uint32_t kAdvisorCityPlanningMessageDevelop = 0x0b3be81a;
constexpr std::uint32_t kAdvisorCityPlanningMessageDevelop2 = 0x8a394dc4;
constexpr std::uint32_t kAdvisorCityPlanningMessageDevelop3 = 0x4a490d33;
// cSC4AdvisorUtility's three messages (Init @ 0x10044046c).
constexpr std::uint32_t kAdvisorUtilityMessageBuilding = 0xea418d8f;
constexpr std::uint32_t kAdvisorUtilityMessagePowerStrikeOn = 0xca358f85;
constexpr std::uint32_t kAdvisorUtilityMessagePowerStrikeOff = 0xca358f9c;
// cSC4AdvisorTraffic's extra message (Init @ 0x100449a6e).
constexpr std::uint32_t kAdvisorTrafficMessageCarZot = 0x4c04de69;
// The property holder class id (a message's void1) the advisors read through.
constexpr std::uint32_t kAdvisorPropertyHolderClassId = 0x278128a0;
constexpr std::uint32_t kAdvisorUtilityPropertyHolderClassId = 0xc772bf98;
// The reward advice class id matched in the 0xea8ae29a re-evaluation walk.
constexpr std::uint32_t kGZCLSID_cSC4RewardAdvice = 0xaa371c32;

class cSC4AdvisorBase;
class cSC4AdvisorCityPlanning;
class cSC4AdvisorTraffic;
class cSC4AdvisorHQ;
class cSC4AdvisorFinance;
class cSC4AdvisorUtility;
class cSC4AdvisorSafety;

// What the concrete advisors need from the running game (one method per original global / engine call). A null host
// service is treated as absent; the original dereferences SL::spMS2 / spInstance / the simulators unconditionally.
class IAdvisorConcreteHost {
public:
    virtual ~IAdvisorConcreteHost() = default;
    virtual cIGZMessageServer2* Messages() = 0;                                    // SL::spMS2
    virtual void SetGlobal(const char* name, double value) = 0;                    // spInstance vslot 0x60
    virtual void SetLocal(const char* name, double value, cISC4AdviceSubject* subject) = 0;  // spInstance vslot 0x70
    virtual bool PushEvent(int eventId, cISC4AdviceSubject* subject) = 0;          // spInstance vslot 0x78
    virtual bool CreateAdviceSubject(cISC4AdviceSubject** out) = 0;                // GZCOM(0x6a26d049, 0x6a26d050)
    // SL::spBudgetSim (the values Finance / HQ / CityPlanning read).
    virtual std::int64_t BudgetInt(std::uint32_t vslot) = 0;                       // 0x30 funds, 0x70 expense, 0x80 income,
                                                                                    // 0x90 tax income, 0x148 borrowed,
                                                                                    // 0x150 borrowing limit, 0x168 bond payments
    virtual float BudgetTaxRate(int index) = 0;                                     // vslot 0x120(index)
    virtual float BudgetLineItemFunding(std::uint32_t lineItemId) = 0;              // GetLineItem(id) -> vslot 0x50 float*
    // SL::spNeighborsSim (Finance).
    virtual std::int64_t NeighborsInt(std::uint32_t vslot) = 0;                     // 0x100 deal income, 0x108 deal expense
    // SL::spResidentialSim (HQ).
    virtual std::int64_t ResidentialInt(std::uint32_t vslot) = 0;                   // 0x80 count, 0x110 life expectancy
    virtual float ResidentialFloat(std::uint32_t vslot) = 0;                        // 0x100 education, 0x108 health
    // SL::spResidentialSim vslot 0x160 / 0x158: the best/worst tract values and their city cells.
    virtual void ResidentialTracts(std::uint32_t vslot, float& bestValue, int& bestX, int& bestZ, float& worstValue,
                                   int& worstX, int& worstZ) = 0;
    // The property holder behind a message's void1 (class id 0x278128a0 / 0xc772bf98).
    virtual bool MessageVoid1IsPropertyHolder(cIGZMessage2* message) = 0;
    virtual bool MessageVoid1HasProperty(cIGZMessage2* message, std::uint32_t propertyId) = 0;   // CheckProperty
    // SL::spBuildingDevelopmentSim: the occupant object behind the holder's 0x87dda3a9 interface -> its type ->
    // vslot 0x68 -> the landmark type / advice guid. Answers false when the object or the simulator is absent.
    virtual bool BuildingDevelopmentLookup(cIGZMessage2* message, std::uint32_t* typeOut) = 0;
    // SL::spCivicBuildingSim vslot 0x30(data) answers a byte whose bit 1 clears the no-links flag (base DoMessage).
    virtual bool CivicBuildingAllowsLinks(std::uint32_t data) = 0;                   // true when null or (byte & 2) == 0
    // The large simulator-statistics updates (the occupant walks of HQ/Traffic/Safety/Utility, the traffic counters,
    // the utility/water reads, the CityPlanning building sweep). Their bodies iterate lists owned by the still
    // unrecovered simulators (spResidentialSim, spOccupantManager, spTrafficSim, spCity, spAirportDeveloper,
    // spSeaportDeveloper, spPowerSim, spWaterSim, ...) and then set the g_*/l_* globals; the port routes the whole
    // update through this seam until those simulators are recovered. advisorClassId selects the concrete advisor.
    virtual void UpdateSimulatorStatistics(std::uint32_t advisorClassId) = 0;
};
IAdvisorConcreteHost* AdvisorConcreteHost();
void SetAdvisorConcreteHost(IAdvisorConcreteHost* host);

class cSC4AdvisorBase : public cSC4Advisor {
public:
    cSC4AdvisorBase();                                   // @ 0x100444c9c
    ~cSC4AdvisorBase() override;                         // @ 0x100444cfa
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4AdvisorBase; }   // @ 0x100444c90

    bool Init(int advisorType) override;                 // @ 0x100444d3c
    bool Shutdown() override;                            // @ 0x100444dee
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x100444e86

    // The deferred update: run by simulate_day when the pending flag is set and by simulate_month unconditionally.
    virtual void update_variables();                     // @ 0x100445270 (empty; primary vtable slot 0x100)
    void simulate_day() override;                        // @ 0x100445276
    void simulate_month() override;                      // @ 0x1004452aa

    void post_self_update(std::uint32_t kind, std::uint32_t value);   // @ 0x1004451aa

    // test / subclass accessors
    bool UpdatePending() const { return updatePending_; }             // +0x141
    bool SubscribedToMessages() const { return subscribed_; }         // +0x142
    std::uint32_t DirtyMask() const { return dirtyMask_; }            // +0x144

protected:
    bool updatePending_ = true;      // +0x141 (the constructor stores 1)
    bool subscribed_ = false;        // +0x142
    std::uint32_t dirtyMask_ = 0;    // +0x144 (which update_* to run on the deferred self-update)
};

class cSC4AdvisorHQ : public cSC4AdvisorBase {
public:
    bool Init(int advisorType) override;                 // @ 0x100447314
    bool Shutdown() override;                            // @ 0x1004476cc
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x100447260
    std::uint32_t GetGZCLSID() const override;           // @ 0x100447784
    void update_variables() override;                    // @ 0x100446dac
    void update_occupant_variables();                    // @ 0x100445ab8

    bool GroupVectorsInitialized() const { return groupsInitialized_; }   // +0x148

private:
    bool groupsInitialized_ = false;    // +0x148
};

class cSC4AdvisorFinance : public cSC4AdvisorBase {
public:
    bool Init(int advisorType) override;                 // @ 0x100447eae
    bool Shutdown() override;                            // @ 0x100447ed6
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x100447e08
    std::uint32_t GetGZCLSID() const override;           // @ 0x100447eee
    void update_variables() override;                    // @ 0x100447a8c

private:
    bool subscribedFlag_ = false;       // +0x148 (kept for the 0x150 layout; nothing recovered uses it)
};

class cSC4AdvisorUtility : public cSC4AdvisorBase {
public:
    bool Init(int advisorType) override;                 // @ 0x10044046c
    bool Shutdown() override;                            // @ 0x100440846
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x100440216
    void simulate_day_events() override;                 // @ 0x1004408fe
    std::uint32_t GetGZCLSID() const override;           // @ 0x1004415aa
    void update_variables() override;                    // @ 0x10043fa78
    void update_occupant_variables();                    // @ 0x10043ed6c
    void update_neighbor_deal_variables();               // @ 0x10043f58c

    bool SubscribedToMessages() const { return subscribed_; }   // +0x148

private:
    bool subscribed_ = false;           // +0x148
};

class cSC4AdvisorSafety : public cSC4AdvisorBase {
public:
    bool Init(int advisorType) override;                 // @ 0x10044b6be
    bool Shutdown() override;                            // @ 0x10044b844
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x10044a922
    std::uint32_t GetGZCLSID() const override;           // @ 0x10044b9a2
    void update_variables() override;                    // @ 0x10044a5ac
    void update_occupant_variables();                    // @ 0x100449d96

    bool SubscribedToMessages() const { return subscribed_; }   // +0x148

private:
    bool subscribed_ = false;           // +0x148 (the constructor zeroes it with a 4-byte store)
};

class cSC4AdvisorTraffic : public cSC4AdvisorBase {
public:
    ~cSC4AdvisorTraffic() override;                      // @ 0x100447fbc
    bool Init(int advisorType) override;                 // @ 0x100449a6e
    bool Shutdown() override;                            // @ 0x100449ac8
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x100449850
    std::uint32_t GetGZCLSID() const override;           // @ 0x100449b16
    void update_variables() override;                    // @ 0x100448690
    void update_occupant_variables();                    // @ 0x100448096

    bool SubscribedToMessages() const { return subscribed_; }   // +0x160
    std::uint32_t CarZotCount() const { return static_cast<std::uint32_t>(carZots_.size()); }   // +0x158 read directly

private:
    std::set<void*> carZots_;           // +0x148
    bool subscribed_ = false;           // +0x160
};

class cSC4AdvisorCityPlanning : public cSC4AdvisorBase {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x10040277a
    bool Init(int advisorType) override;                 // @ 0x1004022ea
    bool Shutdown() override;                            // @ 0x1004023d6
    bool DoMessage(cIGZMessage2* message) override;      // @ 0x100401f7a
    bool Write(cIGZOStream& stream) const override;      // @ 0x1004024ac
    bool Read(cIGZIStream& stream) override;             // @ 0x1004025aa
    std::uint32_t GetGZCLSID() const override;           // @ 0x1004027fe
    void update_variables() override;                    // @ 0x100400c50
    void update_occupant_variables();                    // @ 0x100400b56

    int GetLandmarkCount(int landmarkType);              // @ 0x100402260
    bool HasLandmark(int landmarkType);                  // @ 0x1004022a4

    bool SubscribedToMessages() const { return subscribed_; }   // +0x148
    int BuildingCount() const { return buildingCount_; }        // +0x14c
    int ParkCount() const { return parkCount_; }                // +0x150
    int RecreationCount() const { return recreationCount_; }    // +0x154
    const std::map<int, int>& Landmarks() const { return landmarks_; }   // +0x158

private:
    bool subscribed_ = false;           // +0x148
    int buildingCount_ = 0;             // +0x14c
    int parkCount_ = 0;                 // +0x150
    int recreationCount_ = 0;           // +0x154
    std::map<int, int> landmarks_;      // +0x158 landmark type -> count
};

}  // namespace sc4
