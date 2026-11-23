// cSC4CitySituationAdvice — a "city situation" (a tracked mission: chase, escape, reach a target, cover an area...) as
// an advisor entry. Recovered from SimCity 4 Deluxe Edition 1.2.1 (code 0x1006a81a0..0x1006aa004, class id 0x8be3753b,
// interface cISC4CitySituationAdvice 0x6beb1e8a, object size 0x290). Each situation is one row of the script table
// "advices[<advice id>]"; Init() resolves that row to a Lua registry reference (+0x28c, -2 = none) and later reads
// (text, images, moods, success conditions) go back to the table through it.
//
// Original layout: +0x00 cISC4CitySituationAdvice vptr, +0x08 cSC4Advice base (advice id at +0xe0), +0x10 refcount
// view, +0x20 serializable view; own fields from +0x148:
//   +0x148 successCondition (1 reach target, 2 escape the city, 3 evade, 5 cell coverage; default 1)
//   +0x14c timeLimit  +0x150 successDistance (default 16)  +0x154 minTargetDistance  +0x158 maxTargetDistance
//   +0x15c successTimeout  +0x160 delayAfterDisabled  +0x164 flags (see kSituationFlag*)
//   +0x168 automata groups  +0x180 target groups  +0x198 target sequence  +0x1b0 evil twin guid
//   +0x1b8 reward guid  +0x1bc active radius  +0x1c0 evade list  +0x1d8 evade distance  +0x1dc evade timeout
//   +0x1e0 coverage type (1 zone, 2 lot, 3 network, 4 rect, 5/6 zone variants, 7 explicit cells)  +0x1e4 coverage
//   zone  +0x1e8 coverage occupant group  +0x1ec coverage network type  +0x1f0 coverage rect (4 x u16)  +0x1f8 cell
//   list  +0x210/+0x214 cells min/max  +0x218 success count  +0x21c success fraction (pct * 0.01)
//   +0x220 success aura radius/magnitude  +0x228 success effect  +0x250 success money
//   +0x254 failure aura radius/magnitude  +0x260 failure effect  +0x288 failure money  +0x28c script table ref
//
// Quirks kept: the situation fields are read with whatever Lua reads give (missing fields keep the constructor
// defaults); "always_create_target" sets bit 0 of the byte at +0x165 (= flag 0x100); the evade/target list parse
// removes zero entries from the target sequence only; GetSuccessImageId/GetFailureImageId fall back to the
// situation image (vslot 0xc8), GetRewardUnlockedImageId to the success image (0xd0), the moods to the base advice's
// own mood.
// Services are injected through ICitySituationAdviceHost (the original uses SL::spLua / spCity / spAdvisorSystem).
#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZString.h"
#include "gz/cIGZUnknown.h"
#include "rz/cRZString.h"
#include "sc4/CitySituationEntities.h"
#include "sc4/SLString.h"

namespace sc4 {
class cISC4GameDataRegistry;
}
namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4CitySituationAdvice = 0x8be3753b;
constexpr std::uint32_t kGZIID_cISC4CitySituationAdvice = 0x6beb1e8a;

// cISC4CitySituationAdvice::eSituationFlags (the tests of vslot 0x78)
constexpr std::uint32_t kSituationFlagCreateTarget = 0x1;     // "create_target"
constexpr std::uint32_t kSituationFlagEvade = 0x2;            // an "evade_list" exists (and not a pursuit mission)
constexpr std::uint32_t kSituationFlagTargetSequence = 0x4;   // "target_sequence"
constexpr std::uint32_t kSituationFlagServiceMission = 0x8;   // "service_mission"
constexpr std::uint32_t kSituationFlagUseLotBoundary = 0x10;  // "use_lot_boundary"
constexpr std::uint32_t kSituationFlagMySimMission = 0x20;    // "mysim_mission"
constexpr std::uint32_t kSituationFlagTraining = 0x40;        // "training_mission"
constexpr std::uint32_t kSituationFlagPursuit = 0x80;         // "pursuit_mission"
constexpr std::uint32_t kSituationFlagAlwaysCreateTarget = 0x100;   // "always_create_target"

// Success conditions (vslot 0x20)
constexpr std::uint32_t kSuccessReachTarget = 1, kSuccessEscapeCity = 2, kSuccessEvade = 3, kSuccessCellCoverage = 5;

// Coverage types (vslot 0x120)
enum CoverageType : std::uint32_t {
    kCoverageNone = 0, kCoverageZone = 1, kCoverageLot = 2, kCoverageNetwork = 3, kCoverageRect = 4,
    kCoverageZoneAlt = 5, kCoverageWater = 6, kCoverageCells = 7
};

struct SituationCell { std::uint16_t x = 0, z = 0; };   // SC4Point<unsigned short>

// The part of cISCLua + SCLuaHelp the situation code uses. Stack indices follow Lua: positive from the bottom,
// negative from the top (-1 = top). Vslot offsets of the original's cISCLua are in the comments ([inferred] where the
// meaning was derived from the callers, not from a symbol).
class ISituationLua {
public:
    virtual ~ISituationLua() = default;
    virtual int GetTop() = 0;                          // 0x30
    virtual void SetTop(int top) = 0;                  // 0x38
    virtual void PushValue(int index) = 0;             // 0x40 [inferred]
    virtual void Pop(int count) = 0;                   // 0x58
    virtual bool IsNumber(int index) = 0;              // 0x70
    virtual double ToNumber(int index) = 0;            // 0xb8
    virtual void PushNumber(double value) = 0;         // 0xf8
    virtual void GetGlobal(const char* name) = 0;      // 0x128 [inferred] pushes the global
    virtual void RawGetTable(int index) = 0;           // 0x130 [inferred] pops the key, pushes table[key]
    virtual void RawSetTable(int index) = 0;           // 0x170 [inferred] table[key]=value, pops both
    virtual void GetRef(int ref) = 0;                  // 0x158 pushes registry[ref]
    virtual void NewTable() = 0;                       // 0x160
    virtual int Ref(bool lockValue) = 0;               // 0x190 pops the top into the registry, returns its ref
    virtual void CallProtected(int argCount, int resultCount) = 0;   // 0x198
    virtual void Unref(int ref) = 0;                   // 0x1f0
    virtual bool IsFunction(int index) = 0;            // 0x238
    virtual bool IsTable(int index) = 0;               // 0x240
    // 0x260 ToDataRegistryNode: the game data registry node of the table at `index` (borrowed; null for anything else)
    virtual cIGZUnknown* ToDataRegistryNode(cISC4GameDataRegistry* registry, int index) { (void)registry; (void)index; return nullptr; }
    // SCLuaHelp
    virtual bool GetGlobalTable(const char* name) = 0;                       // SCLuaHelp::GetGlobal: pushes it, false when nil
    virtual bool GetField(const char* name, int tableIndex) = 0;             // SCLuaHelp::GetField: pushes table[name], false when nil
    virtual void ReadFieldU32(const char* name, std::uint32_t& value, int tableIndex) = 0;   // leaves `value` when missing
    virtual void ReadFieldInt(const char* name, std::int32_t& value, int tableIndex) = 0;
    virtual void ReadFieldFloat(const char* name, float& value, int tableIndex) = 0;
    virtual void ReadFieldBool(const char* name, bool& value, int tableIndex) = 0;
    virtual void ReadFieldString(const char* name, cRZString& value, int tableIndex) = 0;
    virtual void ReadTableAsArrayU32(std::vector<std::uint32_t>& out, int tableIndex) = 0;   // replaces `out` with the array part
    virtual void ReadTableAsArrayString(std::vector<cRZString>& out, int tableIndex) = 0;
    // SCLuaTableRef iteration over the table on top of the stack: calls `visit` once per entry with the value pushed
    // on the stack; the visitor must leave the stack as it found it.
    virtual void ForEachEntry(void (*visit)(void* context), void* context) = 0;
};

// What cSC4Advice (the base class at +0x8) provides to the situation.
class ISituationAdviceBase {
public:
    virtual ~ISituationAdviceBase() = default;
    virtual bool Init(int adviceId) = 0;               // cSC4Advice::Init
    virtual void Shutdown() = 0;                       // cSC4Advice::Shutdown
    virtual int AdviceId() = 0;                        // field at this+0xe0
    virtual int AdvisorId() = 0;                       // vslot 0x58
    virtual void SetFlags(int flags, bool on) = 0;     // vslot 0x70
    virtual int Mood() = 0;                            // vslot 0x30 (GetMood)
    virtual int Priority() { return 0; }               // vslot 0x38 (GetPriority; the situation track sort key)
    virtual std::uint32_t Trigger() = 0;               // cSC4Advice::GetTrigger
    virtual bool QueryBaseInterface(std::uint32_t iid, void** out) = 0;
    // The object both views belong to (the original is one object: the situation derives from cSC4Advice, so the two views
    // share one reference count). Null when the situation stands alone; then it counts its own references.
    virtual cIGZUnknown* Outer() { return nullptr; }
};

class ICitySituationAdviceHost {
public:
    virtual ~ICitySituationAdviceHost() = default;
    virtual ISituationLua* Lua() = 0;                  // SL::spLua (may be null)
    virtual int CityCellsX() = 0;                      // spCity vslot 0x2e8
    virtual float CityCellSizeXMeters() = 0;           // spCity vslot 0x2d8
    // Advisor system: the (advisorId, guid) advice exists and answers vslot 0x40 true (the evil twin shown with a message).
    virtual bool IsLinkedAdviceAvailable(int advisorId, std::uint32_t guid) = 0;
    virtual void AdvisorSystemReleaseHook() = 0;       // cSC4AdvisorSystem::spInstance vslot 0x98 (after a failed Init)
    virtual void AdvisorSystemRegister(void* base) = 0;// cSC4AdvisorSystem::spInstance vslot 0x90 (before the Lua lookup)
};
ICitySituationAdviceHost* CitySituationAdviceHost();
void SetCitySituationAdviceHost(ICitySituationAdviceHost* host);

// cISC4CitySituationAdvice (vtable at 0x100ae0590). The slot offsets are the original vslots.
class cISC4CitySituationAdvice : public cIGZUnknown {
public:
    virtual void* AsAdvice() = 0;                                              // 0x18
    virtual std::uint32_t GetSuccessCondition() const = 0;                     // 0x20
    virtual bool GetSuccessConditionText(cIGZString& out) const = 0;           // 0x28
    virtual float GetTimeLimit() const = 0;                                    // 0x30
    virtual float GetSuccessDistance() const = 0;                              // 0x38
    virtual float GetMinTargetDistance() const = 0;                            // 0x40
    virtual float GetMaxTargetDistance() const = 0;                            // 0x48
    virtual float GetSuccessTimeout() const = 0;                               // 0x50
    virtual float GetDelayAfterDisabled() const = 0;                           // 0x58
    virtual const std::vector<std::uint32_t>& GetAutomataGroups() const = 0;   // 0x60
    virtual const std::vector<std::uint32_t>& GetTargetGroups() const = 0;     // 0x68
    virtual std::uint32_t GetSituationFlags() const = 0;                       // 0x70
    virtual bool CheckSituationFlag(std::uint32_t flag) const = 0;             // 0x78
    virtual const std::vector<std::uint32_t>& GetTargetSequence() const = 0;   // 0x80
    virtual bool HasTargetSequence() const = 0;                                // 0x88
    virtual const std::vector<std::uint32_t>& GetEvadeList() const = 0;        // 0x90
    virtual bool HasEvadeList() const = 0;                                     // 0x98
    virtual float GetEvadeDistance() const = 0;                                // 0xa0
    virtual float GetEvadeTimeout() const = 0;                                 // 0xa8
    virtual const cRZString& GetSuccessText() const = 0;                       // 0xb0
    virtual const cRZString& GetFailureText() const = 0;                       // 0xb8
    virtual const cRZString& GetProgressText(std::uint16_t index) const = 0;   // 0xc0
    virtual std::uint32_t GetSituationImageId() const = 0;                     // 0xc8
    virtual std::uint32_t GetSuccessImageId() const = 0;                       // 0xd0
    virtual std::uint32_t GetSuccessMood() const = 0;                          // 0xd8
    virtual std::uint32_t GetFailureImageId() const = 0;                       // 0xe0
    virtual std::uint32_t GetFailureMood() const = 0;                          // 0xe8
    virtual int GetScriptTableRef() const = 0;                                 // 0xf0
    virtual std::uint32_t GetEvilTwinGuid() const = 0;                         // 0xf8
    virtual std::uint32_t GetRewardGuid() const = 0;                           // 0x100
    virtual const cRZString& GetRewardProgressText() const = 0;                // 0x108
    virtual const cRZString& GetRewardUnlockedText() const = 0;                // 0x110
    virtual std::uint32_t GetRewardUnlockedImageId() const = 0;                // 0x118
    virtual std::uint32_t GetCoverageType() const = 0;                         // 0x120
    virtual std::uint32_t GetCoverageZone() const = 0;                         // 0x128
    virtual std::uint32_t GetCoverageOccupantGroup() const = 0;                // 0x130
    virtual std::uint32_t GetCoverageNetworkType() const = 0;                  // 0x138
    virtual const std::uint16_t* GetCoverageRect() const = 0;                  // 0x140 {x0, z0, x1, z1}
    virtual const std::list<SituationCell>& GetCellCoverageList() const = 0;   // 0x148
    virtual std::uint32_t GetCoverageCellsMin() const = 0;                     // 0x150
    virtual std::uint32_t GetCoverageCellsMax() const = 0;                     // 0x158
    virtual std::uint32_t GetCoverageSuccessCount() const = 0;                 // 0x160
    virtual float GetCoverageSuccessPct() const = 0;                           // 0x168
    virtual bool GetSuccessAura(float& radius, float& magnitude) const = 0;    // 0x170
    virtual bool GetSuccessEffectName(cRZString& out) const = 0;               // 0x178
    virtual bool GetSuccessMoney(long long& money) const = 0;                  // 0x180
    virtual bool GetFailureAura(float& radius, float& magnitude) const = 0;    // 0x188
    virtual bool GetFailureEffectName(cRZString& out) const = 0;               // 0x190
    virtual bool GetFailureMoney(long long& money) const = 0;                  // 0x198
    virtual float GetActiveRadius() const = 0;                                 // 0x1a0
    virtual bool IsAutomatonRelevant(ISituationAutomaton* automaton) const = 0;                 // 0x1a8
    virtual bool IsGroupRelevant(const std::vector<std::uint32_t>& groups) const = 0;           // 0x1b0
    virtual void UpdateSituationFields() = 0;                                  // 0x1b8
    // The base advice's priority (cISC4Advice vslot 0x38); the situation track list sorts on it.
    virtual int Priority() const { return 0; }
    // The base advice's title, message and mood (AsAdvice() vslots 0x48 GetTextTitle, 0x50 GetTextMessage, 0x18 GetMood);
    // the situation advice answers the first two from its script table. The situation dialog reads them.
    virtual const cRZString& GetAdviceTitle() const { static const cRZString empty; return empty; }
    virtual const cRZString& GetAdviceMessage() const { static const cRZString empty; return empty; }
    virtual int GetAdviceMood() const { return 0; }
    // GetAdvisorId / GetMood of the base advice, and the advice id used as the situation key (cSC4Advice +0xe0).
    virtual int GetAdviceId() const = 0;
};

class cSC4CitySituationAdvice final : public cISC4CitySituationAdvice {
public:
    explicit cSC4CitySituationAdvice(ISituationAdviceBase* base);       // @ 0x1006a81f2
    ~cSC4CitySituationAdvice();                                         // @ 0x1006a835c (+ 0x1006a8442/44c/45a/468/496/4a4)

    bool Init(int adviceId);                                            // @ 0x1006a84b2 (thunk 0x1006a88b8)
    bool Shutdown();                                                    // @ 0x1006a8fb4 (thunk 0x1006a8ffc); always answers true
    std::uint32_t GetTrigger() const;                                   // @ 0x1006a900e (thunk 0x1006a901c)
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4CitySituationAdvice; }   // @ 0x1006a9fe0 (thunks 0x1006a9fec, 0x1006a9ff8)

    bool QueryInterface(std::uint32_t iid, void** out) override;        // @ 0x1006a9c56 (thunks 0x1006a9c78/c9e/cc4)
    std::uint32_t AddRef() override;                                    // @ 0x1006a9cea (thunks 0x1006a9cf8/d02/d10)
    std::uint32_t Release() override;                                   // @ 0x1006a9d1e (thunks 0x1006a9d2c/d36/d44)

    void* AsAdvice() override { return base_; }                         // @ 0x1006a9d52
    std::uint32_t GetSuccessCondition() const override { return successCondition_; }   // @ 0x1006a9d5c
    bool GetSuccessConditionText(cIGZString& out) const override;       // @ 0x1006a9254
    float GetTimeLimit() const override { return timeLimit_; }          // @ 0x1006a9d68
    float GetSuccessDistance() const override { return successDistance_; }   // @ 0x1006a9d76
    float GetMinTargetDistance() const override { return minTargetDistance_; }   // @ 0x1006a9d84
    float GetMaxTargetDistance() const override;                        // @ 0x1006a9b62
    float GetSuccessTimeout() const override { return successTimeout_; }   // @ 0x1006a9d92
    float GetDelayAfterDisabled() const override { return delayAfterDisabled_; }   // @ 0x1006a9da0
    const std::vector<std::uint32_t>& GetAutomataGroups() const override { return automataGroups_; }   // @ 0x1006a9dae
    const std::vector<std::uint32_t>& GetTargetGroups() const override { return targetGroups_; }       // @ 0x1006a9dbc
    std::uint32_t GetSituationFlags() const override { return flags_; }   // @ 0x1006a9dca
    bool CheckSituationFlag(std::uint32_t flag) const override { return (flags_ & flag) != 0; }   // @ 0x1006a9dd6
    const std::vector<std::uint32_t>& GetTargetSequence() const override { return targetSequence_; }   // @ 0x1006a9de6
    bool HasTargetSequence() const override { return (flags_ & kSituationFlagTargetSequence) != 0; }   // @ 0x1006a9df4
    const std::vector<std::uint32_t>& GetEvadeList() const override { return evadeList_; }   // @ 0x1006a9e06
    bool HasEvadeList() const override { return !evadeList_.empty(); }   // @ 0x1006a9e14
    float GetEvadeDistance() const override { return evadeDistance_; }   // @ 0x1006a9e2c
    float GetEvadeTimeout() const override { return evadeTimeout_; }   // @ 0x1006a9e3a
    const cRZString& GetSuccessText() const override;                   // @ 0x1006a9700
    const cRZString& GetFailureText() const override;                   // @ 0x1006a971a
    const cRZString& GetProgressText(std::uint16_t index) const override;   // @ 0x1006a990c
    std::uint32_t GetSituationImageId() const override;                 // @ 0x1006a9734
    std::uint32_t GetSuccessImageId() const override;                   // @ 0x1006a97da
    std::uint32_t GetSuccessMood() const override;                      // @ 0x1006a983e
    std::uint32_t GetFailureImageId() const override;                   // @ 0x1006a9872
    std::uint32_t GetFailureMood() const override;                      // @ 0x1006a98a4
    int GetScriptTableRef() const override { return scriptTableRef_; }  // @ 0x1006a9e48
    std::uint32_t GetEvilTwinGuid() const override { return evilTwinGuid_; }   // @ 0x1006a9e54
    std::uint32_t GetRewardGuid() const override { return rewardGuid_; }       // @ 0x1006a9e60
    const cRZString& GetRewardProgressText() const override;            // @ 0x1006a98d8
    const cRZString& GetRewardUnlockedText() const override;            // @ 0x1006a98f2
    std::uint32_t GetRewardUnlockedImageId() const override;            // @ 0x1006a980c
    std::uint32_t GetCoverageType() const override { return coverageType_; }   // @ 0x1006a9e6c
    std::uint32_t GetCoverageZone() const override { return coverageZone_; }   // @ 0x1006a9e78
    std::uint32_t GetCoverageOccupantGroup() const override { return coverageOccupantGroup_; }   // @ 0x1006a9e84
    std::uint32_t GetCoverageNetworkType() const override { return coverageNetworkType_; }       // @ 0x1006a9e90
    const std::uint16_t* GetCoverageRect() const override { return coverageRect_; }              // @ 0x1006a9e9c
    const std::list<SituationCell>& GetCellCoverageList() const override { return cellList_; }   // @ 0x1006a9eaa
    std::uint32_t GetCoverageCellsMin() const override { return cellsMin_; }       // @ 0x1006a9eb8
    std::uint32_t GetCoverageCellsMax() const override { return cellsMax_; }       // @ 0x1006a9ec4
    std::uint32_t GetCoverageSuccessCount() const override { return successCount_; }   // @ 0x1006a9ed0
    float GetCoverageSuccessPct() const override { return successPct_; }           // @ 0x1006a9edc
    bool GetSuccessAura(float& radius, float& magnitude) const override;   // @ 0x1006a9eea
    bool GetSuccessEffectName(cRZString& out) const override;           // @ 0x1006a9f0e
    bool GetSuccessMoney(long long& money) const override;              // @ 0x1006a9f48
    bool GetFailureAura(float& radius, float& magnitude) const override;   // @ 0x1006a9f5e
    bool GetFailureEffectName(cRZString& out) const override;           // @ 0x1006a9f82
    bool GetFailureMoney(long long& money) const override;              // @ 0x1006a9fbc
    float GetActiveRadius() const override { return activeRadius_; }    // @ 0x1006a9fd2
    bool IsAutomatonRelevant(ISituationAutomaton* automaton) const override;   // @ 0x1006a9bb0
    bool IsGroupRelevant(const std::vector<std::uint32_t>& groups) const override;   // @ 0x1006a9c08
    void UpdateSituationFields() override;                              // @ 0x1006a88c6
    int GetAdviceId() const override { return base_ ? base_->AdviceId() : 0; }
    int Priority() const override { return base_ ? base_->Priority() : 0; }
    const cRZString& GetTextTitle() const;                              // @ 0x1006a9026 (thunk 0x1006a9236)
    const cRZString& GetAdviceTitle() const override { return GetTextTitle(); }
    const cRZString& GetAdviceMessage() const override { return GetTextMessage(); }
    int GetAdviceMood() const override { return base_ ? base_->Mood() : 0; }
    const cRZString& GetTextMessage() const;                            // @ 0x1006a92e0 (thunk 0x1006a96ea)

private:
    // @ 0x1006a9040 (ReadStringFromTable) / 0x1006a9746 (ReadUint32FromTable)
    const cRZString& ReadStringFromTable(const char* key) const;
    std::uint32_t ReadUint32FromTable(const char* key) const;

    ISituationAdviceBase* base_;
    std::uint32_t refCount_ = 0;
    std::uint32_t successCondition_ = 1;      // +0x148
    float timeLimit_ = 0.0f;                  // +0x14c
    float successDistance_ = 16.0f;           // +0x150
    float minTargetDistance_ = 0.0f;          // +0x154
    float maxTargetDistance_ = 0.0f;          // +0x158
    float successTimeout_ = 0.0f;             // +0x15c
    float delayAfterDisabled_ = 0.0f;         // +0x160
    std::uint32_t flags_ = 0;                 // +0x164
    std::vector<std::uint32_t> automataGroups_;   // +0x168
    std::vector<std::uint32_t> targetGroups_;     // +0x180
    std::vector<std::uint32_t> targetSequence_;   // +0x198
    std::uint32_t evilTwinGuid_ = 0;          // +0x1b0
    std::uint32_t rewardGuid_ = 0;            // +0x1b8
    float activeRadius_ = 0.0f;               // +0x1bc
    std::vector<std::uint32_t> evadeList_;    // +0x1c0
    float evadeDistance_ = 0.0f;              // +0x1d8
    float evadeTimeout_ = 0.0f;               // +0x1dc
    std::uint32_t coverageType_ = 0;          // +0x1e0
    std::uint32_t coverageZone_ = 0;          // +0x1e4
    std::uint32_t coverageOccupantGroup_ = 0; // +0x1e8
    std::uint32_t coverageNetworkType_ = 0;   // +0x1ec
    std::uint16_t coverageRect_[4] = {0, 0, 0, 0};   // +0x1f0
    std::list<SituationCell> cellList_;       // +0x1f8
    std::uint32_t cellsMin_ = 0, cellsMax_ = 0;   // +0x210, +0x214
    std::uint32_t successCount_ = 0;          // +0x218
    float successPct_ = 0.0f;                 // +0x21c
    float successAuraRadius_ = 0.0f, successAuraMagnitude_ = 0.0f;   // +0x220, +0x224
    cRZString successEffect_;                 // +0x228
    std::int32_t successMoney_ = 0;           // +0x250
    float failureAuraRadius_ = 0.0f, failureAuraMagnitude_ = 0.0f;   // +0x254, +0x258
    cRZString failureEffect_;                 // +0x260
    std::int32_t failureMoney_ = 0;           // +0x288
    int scriptTableRef_ = -2;                 // +0x28c
};

// GZCOM_CreatecSC4CitySituationAdvice (0x1006a81a0) / GZCOM_cSC4CitySituationAdviceCLSID (0x1006a81e6)
cSC4CitySituationAdvice* CreateCitySituationAdvice(ISituationAdviceBase* base);
std::uint32_t CitySituationAdviceClassId();

}  // namespace sc4
