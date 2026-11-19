// Two small automata services on the native application: the occupant group's inclusion roll (sc4/OccupantGroupTemplate.h,
// IOccupantGroupHost) and the attractor's question about an automaton's current state (sc4/AutomataAttractor.h).
#include "rz/cRZRandom.h"
#include "sc4/AutomataAttractor.h"
#include "sc4/Automaton.h"
#include "sc4/OccupantGroupTemplate.h"

namespace native {
namespace {
using namespace sc4;

// cSC4OccupantGroupTemplate::IsOccupantIncluded (i386 0x0015999e) makes a cRZRandom(0xffffffff) on the stack for each roll: the
// seed comes from the clock and the time (cRZRandom::Seed), and one uniform double is drawn.
class OccupantGroupHost final : public IOccupantGroupHost {
public:
    double RandomDouble() override {
        cRZRandom random(0xffffffffu);
        return random.RandomDoubleUniform();
    }
};

// The occupant's automaton interface (0xa9b40f05) and its current state record (vslot 0x1c0, GetState): the record's
// behavior state at +8.
class AttractorHost final : public IAutomataAttractorHost {
public:
    bool GetAutomatonStateId(cISC4Occupant* occupant, std::int32_t& stateId) override {
        void* raw = nullptr;
        if (!occupant || !occupant->QueryInterface(kAutomatonIid, &raw) || !raw) return false;
        auto* automaton = static_cast<cSC4Automaton*>(raw);
        const tAutomataState* state = automaton->GetState();
        if (state) stateId = static_cast<std::int32_t>(state->stateType);
        automaton->Release();
        return state != nullptr;
    }
};

}  // namespace

void InstallAutomataGroupHosts() {
    static OccupantGroupHost groups;
    static AttractorHost attractors;
    sc4::SetOccupantGroupHost(&groups);
    sc4::SetAutomataAttractorHost(&attractors);
}

}  // namespace native
