// cISC4PropRequester — the prop requester interface (iid 0x89c498be). The prop developer asks each requester in its
// list for a share of an 8x8 tract and, when the requester uses its own selector, which prop to place. Implemented
// by cSC4WealthRequester, cSC4PollutionSimulator (+8 in the original) and cSC4CrimeSimulator (+8). The developer's
// list is saved as references to these objects, so a loaded city finds its simulators through this interface.
#pragma once
#include <cstdint>
#include <vector>

#include "gz/cIGZUnknown.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4PropRequester = 0x89c498be;

class cISC4PropRequester : public cIGZUnknown {
public:
    virtual std::uint32_t SelectProp(const std::vector<std::uint32_t>& candidates, int x, int z) = 0;
    // rect: an SC4Rect<int> (x0, z0, x1, z1, inclusive), the tract in cells.
    virtual int GetRequestedShare(const void* rect) const = 0;
    virtual std::uint32_t GetRequesterSatisfactionId() const = 0;
    virtual bool UsesCustomPropSelector() const = 0;
};

}  // namespace sc4
