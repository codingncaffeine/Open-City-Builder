#include "sc4/MySimFilters.h"

#include "sc4/BuildingOccupant.h"
#include "sc4/PylonOccupant.h"

namespace sc4 {

namespace {
IMySimFilterHost* g_host = nullptr;
}
IMySimFilterHost* MySimFilterHost() { return g_host; }
void SetMySimFilterHost(IMySimFilterHost* host) { g_host = host; }

bool cMySimBuildingFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (occupant->GetType() != kOccupantTypeBuilding) return false;
    cISC4BuildingOccupant* building = nullptr;
    if (!occupant->QueryInterface(kIidMySimBuilding, reinterpret_cast<void**>(&building)) || !building) return false;
    bool included = true;
    if (g_host && g_host->BuildingCondition(building) > 3) included = false;
    else if (building->GetCompletionPercent() < 1.0f) included = false;   // kSC4PercentageOneHundred
    else if (onlyKindOne_ && building->GetBuildingProfile().purpose != 1) included = false;
    building->Release();
    return included;
}

bool cMySimPylonFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (occupant->GetType() != kOccupantTypePylon) return false;
    cISC4PylonOccupant* pylon = nullptr;
    if (!occupant->QueryInterface(kIidMySimPylon, reinterpret_cast<void**>(&pylon)) || !pylon) return false;
    bool included = false;
    if (pylon->GetUIState() != 0 && !pylon->GetDragState()) {
        if (cIGZUnknown* owner = pylon->GetPylonOwner()) {
            void* probe = nullptr;
            included = owner->QueryInterface(kIidPylonLink, &probe);
            if (probe) static_cast<cIGZUnknown*>(probe)->Release();
        }
    }
    pylon->Release();
    return included;
}

}  // namespace sc4
