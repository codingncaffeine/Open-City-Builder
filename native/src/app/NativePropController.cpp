// The services of props that carry an automata controller (cSC4PropOccupantAutomataController): the city's automata
// controller manager (SL::spCity vslot 0x178) makes the controller from the template the prop's exemplar names
// (manager vslot 0x28 AddControllerFromTemplateName(occupant, name, null)) and removes it again (vslot 0x40).
#include <cstdio>
#include <cstdlib>

#include "app/NativeCity.h"
#include "rz/cRZString.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/PropOccupantAutomataController.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

cSC4AutomataControllerManager* Controllers() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cSC4AutomataControllerManager*>(city->GetComponent(CityComponent::AutomataControllerManager)) : nullptr;
}

bool Trace() { static const bool on = std::getenv("SC4_PROPCTRLDEBUG") != nullptr; return on; }

struct PropControllerHost final : IPropControllerHost {
    bool HasControllerManager() override { return Controllers() != nullptr; }
    // The manager keeps the controller; the prop takes its own reference (PropOccupantAutomataController.cpp).
    cSC4AutomataController* CreateController(cISC4Occupant* occupant, const cIGZString& templateName) override {
        cSC4AutomataControllerManager* controllers = Controllers();
        cSC4AutomataController* made = controllers ? controllers->AddControllerFromTemplateName(occupant, templateName, nullptr) : nullptr;
        if (Trace()) std::fprintf(stderr, "prop controller: %s -> %s\n", templateName.ToChar(), made ? "made" : "none");
        return made;
    }
    void RemoveController(cSC4AutomataController* controller) override {
        if (cSC4AutomataControllerManager* controllers = Controllers()) controllers->RemoveController(controller);
    }
};

}  // namespace

void InstallNativePropController() { static PropControllerHost host; sc4::SetPropControllerHost(&host); }

}  // namespace native
