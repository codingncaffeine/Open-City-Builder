// cSC4WealthRequester (9 functions + thunks, 0x1001261fc..0x1001264a0, class id 0x89c49986, interfaces 0x89c498be
// requester / 0xe4fda3d4 serializable) — the prop requester that picks a prop of the wealth class of the
// requested cell. Layout: +0 requester vtable, +8 serializable vtable, +0x10 refcount. Streams are empty.
// SelectProp: the wanted wealth is the lot's (zones 11, 12 and 15 force wealth 2) or, without a lot, the
// city's cell wealth; one of the candidate props whose wealth byte (+0x14 of the prop record) matches is
// picked uniformly; 0 when none match.
#pragma once
#include <cstdint>
#include <vector>

#include "gz/cIGZSerializable.h"
#include "rz/cRZRandom.h"
#include "sc4/PropRequesterInterface.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4WealthRequester = 0x89c49986;
constexpr std::uint32_t kWealthRequesterSatisfactionId = 0x49a1e995;

class IWealthHost {
public:
    virtual ~IWealthHost() = default;
    virtual bool HasLot(int x, int z) = 0;            // lot manager GetLot(x, z, 0)
    virtual int LotWealth(int x, int z) = 0;          // lot vslot 0x148
    virtual int CityCellWealth(int x, int z) = 0;     // city vslot 0x150 -> vslot 0x50
    virtual unsigned ZoneType(int x, int z) = 0;      // zone manager vslot 0x60
    virtual int PropWealth(std::uint32_t propId) = 0; // prop manager record +0x14
};

class cSC4WealthRequester final : public cISC4PropRequester, public cIGZSerializable {
public:
    cSC4WealthRequester(IWealthHost* host, cRZRandom* random) : host_(host), random_(random) {}

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool Write(cIGZOStream&) const override { return true; }
    bool Read(cIGZIStream&) override { return true; }
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4WealthRequester; }
    std::uint32_t SelectProp(const std::vector<std::uint32_t>& candidates, int x, int z) override;
    int GetRequestedShare(const void*) const override { return 10; }
    std::uint32_t GetRequesterSatisfactionId() const override { return kWealthRequesterSatisfactionId; }
    bool UsesCustomPropSelector() const override { return true; }

private:
    IWealthHost* host_;
    cRZRandom* random_;
    std::uint32_t refCount_ = 0;
};

}  // namespace sc4
