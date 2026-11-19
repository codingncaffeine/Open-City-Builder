// cSC4NeighborDeal — a trade deal (power / water / garbage / ...) between the city and a neighbour across a
// region connection (61 functions, 0x100072e24..0x100076180). Class id 0x2a134f42, interface 0x773fef1.
// Version 5 record: type, the two connection cells, amount / used / cost / flags, increments, the dates and
// the property bag. Layout: +8 serializable view, +0x10 cSCPropertyHolder, +0x38 refcount, +0x3c type,
// +0x40 cell on the other side, +0x48 cell on the city side, +0x50 amount, +0x54/+0x58 increments,
// +0x5c/+0x60 last amount / increment, +0x64 amount used, +0x68 cost (s64), +0x70 start, +0x80 expiration,
// +0x90 current, +0xa0 suspension (cRZDate), +0xb0 flags (initially 0x81).
// Deal types come in buy/sell pairs 0x4773ffe0/e1, e2/e3, e4/e5 (the other side's view swaps them).
// Flags: 1 proposed, 2 active, 4 cancelled, 8 affordable, 0x10 unaffordable, 0x38 = state mask, 0x80 silent,
// 0x100 locked state, 0x300 frozen.
#pragma once
#include <cstdint>

#include "gz/cGZDate.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZDate.h"
#include "sc4/SCProperty.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4NeighborDeal = 0x2a134f42, kGZIID_cISC4NeighborDeal = 0x0773fef1;
constexpr std::uint32_t kDealTypeBase = 0x4773ffe0;
constexpr std::uint32_t kDealFlagProposed = 1, kDealFlagActive = 2, kDealFlagCancelled = 4, kDealFlagAffordable = 8,
                        kDealFlagUnaffordable = 0x10, kDealStateMask = 0x38, kDealFlagSilent = 0x80,
                        kDealFlagLockedState = 0x100, kDealFlagFrozen = 0x300;

class cSC4NeighborDeal;

// What the deal needs from the city / region / neighbour simulators.
class INeighborDealHost {
public:
    virtual ~INeighborDealHost() = default;
    virtual bool CityHasBudget() = 0;                                         // SL::spCity vslot 0x1f0 != null
    virtual double DealCost(double amount, std::uint32_t type, void* otherCity) = 0;   // the current city's neighbors simulator vslot 0x118 (GetDealCost)
    virtual bool CityEntryAtCell(int x, int y, void*& city) = 0;              // regional manager vslot 0x58
    virtual void* CurrentCity() = 0;                                          // SL::spSC4 vslot 0x120
    virtual double CurrentCityFunds() = 0;                                    // city vslot 0x190
    virtual void ReportDealStatus(bool changed, cSC4NeighborDeal* deal) = 0; // sReportDealStatus<cSC4NeighborDeal>
    // SL::spSC4 vslot 0x120 then cISC4RegionalCity vslot 0x190 (GetBudget), used by UpdateState. Defaulted so the
    // existing fakes keep compiling; the real host overrides it.
    virtual double CurrentRegionalCityBudget() { return 0.0; }
    // SL::spNeighborsSim vslot 0xb8 FindConnectionSuitableForDeal(type, city) != null (UpdateState). Default false.
    virtual bool HasSuitableConnection(std::uint32_t, void*) { return false; }
};
INeighborDealHost* NeighborDealHost();
void SetNeighborDealHost(INeighborDealHost* host);

class cISC4NeighborDeal : public cIGZUnknown {
public:
    virtual void SetRegionCellOnCitySideOfConnection(int x, int z) = 0;
    virtual void GetRegionCellOnCitySideOfConnection(int& x, int& z) const = 0;
    virtual void SetRegionCellOnOtherSideOfConnection(int x, int z) = 0;
    virtual void GetRegionCellOnOtherSideOfConnection(int& x, int& z) const = 0;
    virtual void* GetCityOnOtherSideOfConnection() const = 0;
    virtual std::uint32_t GetType() const = 0;
    virtual void SetType(std::uint32_t type) = 0;
    virtual int GetAmount() const = 0;
    virtual void SetAmount(int amount) = 0;
    virtual int GetLastAmount() const = 0;
    virtual void SetLastAmount(int amount) = 0;
    virtual int GetLastIncAmount() const = 0;
    virtual void SetLastIncAmount(int amount) = 0;
    virtual int GetIncAmount() const = 0;
    virtual void SetIncAmount(int amount) = 0;
    virtual int GetIncAmount2() const = 0;
    virtual void SetIncAmount2(int amount) = 0;
    virtual int GetAmountUsed() const = 0;
    virtual void SetAmountUsed(int amount) = 0;
    virtual std::int64_t GetCost() const = 0;
    virtual void UpdateCost() = 0;
    virtual void GetStartDate(cIGZDate& out) const = 0;
    virtual void SetStartDate(const cIGZDate& date) = 0;
    virtual void GetExpirationDate(cIGZDate& out) const = 0;
    virtual void SetExpirationDate(const cIGZDate& date) = 0;
    virtual void GetCurrentDate(cIGZDate& out) const = 0;
    virtual void SetCurrentDate(const cIGZDate& date) = 0;
    virtual void GetSuspensionDate(cIGZDate& out) const = 0;
    virtual void SetSuspensionDate(const cIGZDate& date) = 0;
    virtual void Copy(const cISC4NeighborDeal& other) = 0;
    virtual void UpdateState() = 0;
    virtual void Cancel() = 0;
    virtual bool Activate() = 0;
    virtual std::uint32_t GetFlags() const = 0;
    virtual std::uint32_t SetFlags(std::uint32_t flags, bool on) = 0;
    virtual bool CheckFlagsAnd(std::uint32_t flags) const = 0;
    virtual bool CheckFlagsOr(std::uint32_t flags) const = 0;
    virtual bool Clone(cISC4NeighborDeal** out, bool silent) const = 0;
};

class cSC4NeighborDeal : public cISC4NeighborDeal, public cIGZSerializable, public cSCPropertyHolder {
public:
    cSC4NeighborDeal() = default;
    ~cSC4NeighborDeal() override = default;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    void SetRegionCellOnCitySideOfConnection(int x, int z) override { cityX_ = x; cityZ_ = z; }
    void GetRegionCellOnCitySideOfConnection(int& x, int& z) const override { x = cityX_; z = cityZ_; }
    void SetRegionCellOnOtherSideOfConnection(int x, int z) override { otherX_ = x; otherZ_ = z; }
    void GetRegionCellOnOtherSideOfConnection(int& x, int& z) const override { x = otherX_; z = otherZ_; }
    void* GetCityOnOtherSideOfConnection() const override;
    std::uint32_t GetType() const override { return type_; }
    void SetType(std::uint32_t type) override { type_ = type; }
    int GetAmount() const override { return amount_; }
    void SetAmount(int amount) override;
    int GetLastAmount() const override { return lastAmount_; }
    void SetLastAmount(int amount) override { lastAmount_ = amount; }
    int GetLastIncAmount() const override { return lastIncAmount_; }
    void SetLastIncAmount(int amount) override { lastIncAmount_ = amount; }
    int GetIncAmount() const override { return incAmount_; }
    void SetIncAmount(int amount) override { incAmount_ = amount; }
    int GetIncAmount2() const override { return incAmount2_; }
    void SetIncAmount2(int amount) override { incAmount2_ = amount; }
    int GetAmountUsed() const override { return amountUsed_; }
    void SetAmountUsed(int amount) override { amountUsed_ = amount; }
    std::int64_t GetCost() const override { return cost_; }
    void UpdateCost() override;
    void GetStartDate(cIGZDate& out) const override { out.Set(start_.JulianDay()); }
    void SetStartDate(const cIGZDate& date) override { start_.SetJulianDay(date.DayNumber()); }
    void GetExpirationDate(cIGZDate& out) const override { out.Set(expiration_.JulianDay()); }
    void SetExpirationDate(const cIGZDate& date) override { expiration_.SetJulianDay(date.DayNumber()); }
    void GetCurrentDate(cIGZDate& out) const override { out.Set(current_.JulianDay()); }
    void SetCurrentDate(const cIGZDate& date) override { current_.SetJulianDay(date.DayNumber()); }
    void GetSuspensionDate(cIGZDate& out) const override { out.Set(suspension_.JulianDay()); }
    void SetSuspensionDate(const cIGZDate& date) override { suspension_.SetJulianDay(date.DayNumber()); }
    void Copy(const cISC4NeighborDeal& other) override;
    void UpdateState() override;
    void Cancel() override;
    bool Activate() override;
    std::uint32_t GetFlags() const override { return flags_; }
    std::uint32_t SetFlags(std::uint32_t flags, bool on) override;
    bool CheckFlagsAnd(std::uint32_t flags) const override { return (flags_ & flags) == flags; }
    bool CheckFlagsOr(std::uint32_t flags) const override { return (flags_ & flags) != 0; }
    bool Clone(cISC4NeighborDeal** out, bool silent) const override;

    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NeighborDeal; }

private:
    std::uint32_t refCount_ = 0;     // +0x38
    std::uint32_t type_ = 0;         // +0x3c
    int otherX_ = 0, otherZ_ = 0;    // +0x40
    int cityX_ = 0, cityZ_ = 0;      // +0x48
    int amount_ = 0;                 // +0x50
    int incAmount_ = 0;              // +0x54
    int incAmount2_ = 0;             // +0x58
    int lastAmount_ = 0;             // +0x5c
    int lastIncAmount_ = 0;          // +0x60
    int amountUsed_ = 0;             // +0x64
    std::int64_t cost_ = 0;          // +0x68
    cRZDate start_, expiration_, current_, suspension_;   // +0x70, +0x80, +0x90, +0xa0 (today on construction)
    std::uint32_t flags_ = 0x81;     // +0xb0
};

}  // namespace sc4
