// The data views' occupant filters: see ComplexViewOccupantFilter.h.
#include "sc4/ComplexViewOccupantFilter.h"

#include <algorithm>

#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/PipeConnectionOccupant.h"
#include "sc4/PropOccupant.h"
#include "sc4/PropOccupantInterface.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {
namespace {
IComplexViewFilterHost* g_host = nullptr;

constexpr std::uint32_t kNetworkOccupantType = 0xc772bf98;
constexpr std::uint32_t kSurfaceNetworkMask = 0x1f4f;
// The two prop types a view that keeps plain lots also keeps (prop vslot 0x1c GetPropType).
constexpr std::uint32_t kKeptPropTypeA = 0xab72fbb3, kKeptPropTypeB = 0xcb79919b;
// The water filter's building properties: either one present includes the building, else the bool 0xcaa45672.
constexpr std::uint32_t kPropWaterA = 0x88ed3303, kPropWaterB = 0x49512a09, kPropWaterFlag = 0xcaa45672;
}  // namespace

void SetComplexViewFilterHost(IComplexViewFilterHost* host) { g_host = host; }

cSC4ComplexViewOccupantFilter::~cSC4ComplexViewOccupantFilter() {
    if (waterFilter_) waterFilter_->Release();
    if (subwayFilter_) subwayFilter_->Release();
}

void cSC4ComplexViewOccupantFilter::SetForceIncludePipeOccupants(cISC4OccupantFilter* waterFilter) {
    if (waterFilter) waterFilter->AddRef();
    if (waterFilter_) waterFilter_->Release();
    waterFilter_ = waterFilter;
}

void cSC4ComplexViewOccupantFilter::SetForceIncludeSubwayOccupants(cISC4OccupantFilter* subwayFilter) {
    if (subwayFilter) subwayFilter->AddRef();
    if (subwayFilter_) subwayFilter_->Release();
    subwayFilter_ = subwayFilter;
}

// 0x0027a246. A lot texture whose lot is missing or not yet built (state < 2) answers the plain-lots flag. Otherwise the
// occupant's type decides (listed types flip the default); an included network piece must also pass the network-type
// test (listed network types flip its default). An occupant those tests drop can still come in through the water or
// subway filter, and, with the plain-lots flag, as one of two prop types.
bool cSC4ComplexViewOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    const std::uint32_t type = occupant->GetType();
    if (type == kOccupantTypeLotBaseTexture) {
        SC4CellRect cells;
        occupant->GetBoundingCityCells(cells);
        const int state = g_host ? g_host->LotStateAt(cells.x0, cells.z0) : -1;
        if (state < 2) return includePlainLots_;
    }
    bool included = includeUnlistedTypes_;
    if (std::find(types_.begin(), types_.end(), type) != types_.end()) included = !included;
    if (included) {
        if (type != kNetworkOccupantType) return true;
        // The original asks for cISC4NetworkOccupant (0xa821ef94); the port's pieces are the class itself.
        auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        if (!network) return true;
        bool networkIncluded = includeUnlistedNetworks_;
        for (std::uint32_t networkType : networkTypes_) {
            if (network->IsOfType(networkType)) {   // vslot 0x58
                networkIncluded = !networkIncluded;
                break;
            }
        }
        if (networkIncluded) return true;
    }
    if (waterFilter_ && waterFilter_->IsOccupantIncluded(occupant)) return true;
    if (subwayFilter_ && subwayFilter_->IsOccupantIncluded(occupant)) return true;
    if (includePlainLots_ && type == kOccupantTypeProp) {
        void* raw = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4PropOccupant, &raw) && raw) {
            auto* prop = static_cast<cISC4PropOccupant*>(raw);
            const std::uint32_t propType = prop->GetPropType();
            prop->Release();
            if (propType == kKeptPropTypeA || propType == kKeptPropTypeB) return true;
        }
    }
    return false;
}

// 0x0012ca8e: a surface network piece, a pipe connection, or a building the property test keeps.
bool cSC4WaterOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant); network && (network->GetNetworkFlag() & kSurfaceNetworkMask) != 0)
        return true;
    void* raw = nullptr;
    if (occupant->QueryInterface(kGZCLSID_cSC4PipeConnectionOccupant, &raw) && raw) {
        static_cast<cIGZUnknown*>(raw)->Release();
        return true;
    }
    return IsPropertyHolderIncluded(occupant->AsPropertyHolder());
}

// 0x0012cb26
bool cSC4WaterOccupantFilter::IsPropertyHolderIncluded(cISCPropertyHolder* holder) {
    if (!holder) return false;
    if (holder->GetProperty(kPropWaterA) || holder->GetProperty(kPropWaterB)) return true;
    bool flag = false;
    cSCPropertyHelp::GetPropertyValue(holder, kPropWaterFlag, flag);
    return flag;
}

}  // namespace sc4
