// The automaton calls behind a construction crew member loaded from a city file (NativeConstruction.cpp's LoadedCrew).
// `automaton` is the cISC4Automaton answer (0xa9b40f05), which is the automaton's primary pointer.
#include "sc4/Automaton.h"

namespace native {

namespace {
sc4::cSC4Automaton* AsAutomaton(cIGZUnknown* automaton) { return static_cast<sc4::cSC4Automaton*>(static_cast<void*>(automaton)); }
}  // namespace

sc4::cISC4Occupant* CrewAsOccupant(cIGZUnknown* automaton) { return AsAutomaton(automaton)->AsOccupant(); }
void CrewSetAutomataFlag(cIGZUnknown* automaton, std::uint32_t flag, bool on) { AsAutomaton(automaton)->SetAutomataFlag(flag, on); }
void CrewSetOrientation(cIGZUnknown* automaton, const float* direction) { AsAutomaton(automaton)->SetOrientation(direction); }

}  // namespace native
