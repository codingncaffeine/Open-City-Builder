// The coverage mission's target (cSC4CitySituationManager::UpdateCoverageTarget, i386 0x004a4d36): a prop occupant automata
// controller that is never placed in the city, seen by the situation manager as an occupant. Its own file: the controller's
// header and the situation manager's cannot be included together.
#include "sc4/CitySituationEntities.h"
#include "sc4/PropOccupantAutomataController.h"

namespace native {
namespace {
using namespace sc4;

// The coverage mission's target: a prop occupant automata controller that is never placed in the city (UpdateCoverageTarget
// makes one and moves it to the centre of the cells left), seen by the situation manager as an occupant.
class CoverageTarget final : public ISituationOccupant {
public:
    explicit CoverageTarget(const float position[3]) : prop_(new sc4::cSC4PropOccupantAutomataController()) {
        prop_->AddRef();
        prop_->SetPosition(position);
    }
    ~CoverageTarget() { prop_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ > 0) return refs_;
        delete this;
        return 0;
    }
    ISituationPropertyHolder* PropertyHolder() override { return nullptr; }
    std::uint32_t OccupantType() override { return prop_->GetType(); }
    void Position(float xyz[3]) override { prop_->GetPosition(xyz); }
    void SetPosition(const float xyz[3]) override { prop_->SetPosition(xyz); }
    bool IsInGroup(std::uint32_t group) override { return prop_->IsOccupantGroup(group); }
    ISituationAutomaton* AsAutomaton() override { return nullptr; }
    bool IsNetworkOccupant() override { return false; }
    bool NetworkFlagsMatch(std::uint32_t) override { return false; }
    void NetworkCell(int& x, int& z) override { x = z = -1; }

private:
    sc4::cSC4PropOccupantAutomataController* prop_;
    std::uint32_t refs_ = 1;
};

}  // namespace

sc4::ISituationOccupant* NativeNewCoverageTarget(const float position[3]) { return new CoverageTarget(position); }

}  // namespace native
