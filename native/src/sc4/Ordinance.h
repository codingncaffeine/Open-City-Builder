// Ordinances: cSC4OrdinanceSimple (55 functions 0x100795fb0..0x100798b3c, class id 0x8bd4fc74, interface
// 0xaa5e2247) is one city ordinance loaded from an exemplar (type 0xe); cSC4OrdinanceSimulator (48 functions
// 0x100798bc4..0x10079a886, class id 0xa990c08e, simulator type 0x41193c3a) owns them, tracks which are
// available / on / enabled and settles their monthly income and expense for the budget simulator.
//
// cSC4OrdinanceSimple layout: +0x10 exemplar property holder (misc properties), +0x48 refcount, +0x4c
// initialised, +0x50 key, +0x5c id, +0x60 name, +0x88 description, +0xb0 first year available, +0xb4 chance of
// availability, +0xb8 enactment income, +0xc0 retraction income, +0xc8 constant monthly income, +0xd0 income
// factor (per residential unit), +0xd4 advisor id, +0xd8 income ordinance, +0xd9 available, +0xda on, +0xdb
// enabled, +0xe0 monthly adjusted income.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZString.h"
#include "sc4/Exemplar.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4OrdinanceSimple = 0x8bd4fc74;
constexpr std::uint32_t kGZIID_cISC4Ordinance = 0xaa5e2247;
constexpr std::uint32_t kGZCLSID_cSC4OrdinanceSimulator = 0xa990c08e;
constexpr std::uint32_t kSimulatorTypeOrdinance = 0x41193c3a;
constexpr std::uint32_t kExemplarTypeOrdinance = 0xe;
constexpr std::uint32_t kOrdinanceMessageMonthlyDone = 0xc8ea5018;
constexpr std::uint32_t kOrdinanceMessageAvailabilityChanged = 0xa2d1c5b9;
constexpr std::uint32_t kOrdinanceMessageOnChanged = 0xc2c09554;

// ordinance exemplar properties
constexpr std::uint32_t kOrdPropId = 0x28ebbc30;
constexpr std::uint32_t kOrdPropNameKey = 0x6a416b21;
constexpr std::uint32_t kOrdPropNameText = 0x28ebbc32;
constexpr std::uint32_t kOrdPropDescriptionKey = 0xca416b2a;
constexpr std::uint32_t kOrdPropDescriptionText = 0x28ebbc34;
constexpr std::uint32_t kOrdPropYearFirstAvailable = 0x28ebbc35;
constexpr std::uint32_t kOrdPropChanceAvailable = 0x28ebbc36;
constexpr std::uint32_t kOrdPropEnactmentIncome = 0x28ebbc37;
constexpr std::uint32_t kOrdPropRetractionIncome = 0x28ebbc38;
constexpr std::uint32_t kOrdPropConstantIncome = 0x28ebbc39;
constexpr std::uint32_t kOrdPropIncomeFactor = 0x28ebbc3a;
constexpr std::uint32_t kOrdPropAdvisor = 0x28ebbc3b;
constexpr std::uint32_t kOrdPropIsIncome = 0x28ebbc3c;

class cSC4OrdinanceSimple;
class cSC4OrdinanceSimulator;

class IOrdinanceHost {
public:
    virtual ~IOrdinanceHost() = default;
    virtual cISCResExemplar* LoadExemplar(const cGZPersistResourceKey& key) = 0;     // resource manager (iid 0xa52160f5)
    virtual int CurrentYear() = 0;                                                    // simulator vslot 0x38
    virtual int ResidentialIncomeBase() = 0;                                          // residential simulator vslot 0x80
    virtual std::string LoadString(const cGZPersistResourceKey& key) = 0;             // cSLString
    virtual void PostMessage(std::uint32_t type, std::uint32_t id, std::uint32_t flag) = 0;
    // Calls back for every exemplar key of the ordinance type (resource manager vslot 0x90 / 0x38 enumeration).
    virtual void ForEachOrdinanceKey(const std::function<void(const cGZPersistResourceKey&)>&) {}
    // GZCOM()->CreateInstance(classId, 0x8bd4fc74): the ordinance class registered under `classId` (the exemplar's instance), one
    // reference, or null when no class has that id (such an exemplar makes no ordinance).
    virtual cSC4OrdinanceSimple* CreateOrdinance(std::uint32_t classId) { (void)classId; return nullptr; }
    // Simulator vslot 0x98 (target, 6 = month, "cSC4OrdinanceSimulator", flags 2 = an established city only) and its removal: the
    // simulator's begin and month messages reach DoMessage through it.
    virtual void AddSimulatorAgent(cSC4OrdinanceSimulator* simulator) { (void)simulator; }
    virtual void RemoveSimulatorAgent(cSC4OrdinanceSimulator* simulator) { (void)simulator; }
};
IOrdinanceHost* OrdinanceHost();
void SetOrdinanceHost(IOrdinanceHost* host);

class cISC4Ordinance : public cIGZUnknown {
public:
    virtual std::uint32_t GetID() const = 0;
    virtual const cRZString& GetName() const = 0;
    virtual const cRZString& GetDescription() const = 0;
    virtual std::int64_t GetEnactmentIncome() const = 0;
    virtual std::int64_t GetRetractionIncome() const = 0;
    virtual bool IsAvailable() const = 0;
    virtual bool IsOn() const = 0;
    virtual bool IsEnabled() const = 0;
    virtual std::int64_t GetMonthlyAdjustedIncome() const = 0;
    virtual bool CheckConditions() = 0;
    virtual bool IsIncomeOrdinance() const = 0;
    virtual cISCPropertyHolder* GetMiscProperties() = 0;
    virtual bool SetAvailable(bool available) = 0;
    virtual bool SetOn(bool on) = 0;
    virtual bool SetEnabled(bool enabled) = 0;
};

class cSC4OrdinanceSimple : public cIGZSerializable, public cISC4Ordinance {
public:
    cSC4OrdinanceSimple() = default;
    virtual ~cSC4OrdinanceSimple();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    virtual bool Init();
    virtual bool Shutdown();
    bool SetKey(const cGZPersistResourceKey& key) { key_ = key; return true; }
    bool FromData(const cGZPersistResourceKey& key);
    virtual std::int64_t GetCurrentMonthlyIncome();
    virtual bool Simulate();

    std::uint32_t GetID() const override { return id_; }
    const cRZString& GetName() const override { return name_; }
    const cRZString& GetDescription() const override { return description_; }
    std::int64_t GetEnactmentIncome() const override { return enactmentIncome_; }
    std::int64_t GetRetractionIncome() const override { return retractionIncome_; }
    std::int64_t GetMonthlyConstantIncome() const { return constantIncome_; }
    std::uint32_t GetYearFirstAvailable() const { return yearFirstAvailable_; }
    float GetChanceAvailability() const { return chanceAvailability_; }
    float GetMonthlyIncomeFactor() const { return incomeFactor_; }
    std::uint32_t GetAdvisorID() const { return advisorId_; }
    bool IsAvailable() const override { return available_; }
    bool IsOn() const override { return available_ && on_; }
    bool IsEnabled() const override { return enabled_; }
    bool IsIncomeOrdinance() const override { return incomeOrdinance_; }
    std::int64_t GetMonthlyAdjustedIncome() const override { return adjustedIncome_; }
    cISCPropertyHolder* GetMiscProperties() override { return &properties_; }
    bool CheckConditions() override;
    bool SetAvailable(bool available) override;       // also clears the adjusted income
    bool SetOn(bool on) override;
    bool SetEnabled(bool enabled) override { enabled_ = enabled; return true; }
    bool ForceID(std::uint32_t id) { id_ = id; return true; }
    bool ForceIncomes(std::int64_t enact, std::int64_t retract, std::int64_t constant, float factor, std::uint32_t year) {
        enactmentIncome_ = enact; retractionIncome_ = retract; constantIncome_ = constant; incomeFactor_ = factor; yearFirstAvailable_ = year; return true; }
    bool ForceName(const char* n) { name_ = cRZString(n); return true; }
    bool ForceIncomeOrdinance(bool v) { incomeOrdinance_ = v; return true; }
    bool ForceAvailable(bool available) { available_ = available; return true; }
    bool ForceOn(bool on) { on_ = on; return true; }
    bool ForceMonthlyAdjustedIncome(std::int64_t value) { adjustedIncome_ = value; return true; }

    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4OrdinanceSimple; }

protected:
    cSCExemplarPropertyHolder properties_;                  // +0x10
    std::uint32_t refCount_ = 0;                            // +0x48
    bool initialised_ = false;                              // +0x4c
    cGZPersistResourceKey key_;                             // +0x50
    std::uint32_t id_ = 0;                                  // +0x5c
    cRZString name_, description_;                          // +0x60, +0x88
    std::uint32_t yearFirstAvailable_ = 0;                  // +0xb0
    float chanceAvailability_ = 0.0f;                       // +0xb4
    std::int64_t enactmentIncome_ = 0, retractionIncome_ = 0, constantIncome_ = 0;   // +0xb8, +0xc0, +0xc8
    float incomeFactor_ = 0.0f;                             // +0xd0
    std::uint32_t advisorId_ = 0;                           // +0xd4
    bool incomeOrdinance_ = false, available_ = false, on_ = false, enabled_ = false;   // +0xd8..+0xdb
    std::int64_t adjustedIncome_ = 0;                       // +0xe0
};

class cSC4OrdinanceSimulator : public cIGZSerializable {
public:
    cSC4OrdinanceSimulator() = default;
    ~cSC4OrdinanceSimulator();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetSimulatorType() const { return kSimulatorTypeOrdinance; }
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4OrdinanceSimulator; }

    bool Init();
    bool Shutdown();
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    void DoMonthlySimulation();
    void SimulationBegin();
    bool DoMessage(std::uint32_t messageType);

    bool AddOrdinance(cISC4Ordinance* ordinance);
    bool RemoveOrdinance(cISC4Ordinance* ordinance);
    void CreateStandardOrdinance(const cGZPersistResourceKey& key);
    void LoadAllStandardOrdinances();                                // @ 0x100798ecc
    void AddOrdinanceData(const cGZPersistResourceKey& key);         // AddOrdinanceDataStatic body
    cISC4Ordinance* GetOrdinanceByID(std::uint32_t id) const;
    cISC4Ordinance* GetOrdinanceByName(const std::string& name) const;
    std::uint32_t GetOrdinanceIDByName(const std::string& name) const;

    bool IsOrdinanceEnabled(std::uint32_t id) const;
    bool SetOrdinanceEnabled(std::uint32_t id, bool enabled);
    bool IsOrdinanceAvailable(std::uint32_t id) const;
    bool IsOrdinanceOn(std::uint32_t id) const;
    bool IsOrdinanceAvailableButOff(std::uint32_t id) const;
    bool SetOrdinanceAvailable(std::uint32_t id, bool available);
    bool SetOrdinanceOn(std::uint32_t id, bool on);
    int GetOrdinanceAvailableCount() const;
    int GetOrdinanceOnCount() const;
    int GetOrdinanceAvailableArray(std::uint32_t* out, std::uint32_t& count) const;
    int GetOrdinanceOnArray(std::uint32_t* out, std::uint32_t& count) const;
    int GetOrdinanceIDArray(std::uint32_t* out, std::uint32_t& count) const;
    std::int64_t GetOrdinanceMonthlyExpense() const { return monthlyExpense_; }
    std::int64_t GetOrdinanceMonthlyIncome() const { return monthlyIncome_; }
    std::int32_t GetCumulativeOffset(std::uint32_t property, std::uint32_t index) const;
    float GetCumulativeFactor(std::uint32_t property, std::uint32_t index) const;

private:
    void ReleaseAll();

    std::uint32_t refCount_ = 0;                                  // +0x1c
    bool initialised_ = false;                                    // +0x18
    bool loaded_ = false;                                         // +0x19
    std::int64_t monthlyExpense_ = 0, monthlyIncome_ = 0;         // +0x20, +0x28
    std::int64_t netAdjustment_ = 0;                              // +0x30 (enactment / retraction money not yet settled)
    std::map<std::uint32_t, cISC4Ordinance*> ordinances_;         // +0x38 (holds a reference each)
};

}  // namespace sc4
