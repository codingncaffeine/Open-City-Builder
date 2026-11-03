// The automata managers' view of the recovered automata (app/NativeAutomataViews.cpp). The managers (sc4/AutomataManager.*,
// TrafficAutomataManager.*) talk to automata through IAutomataAutomaton / ITrafficAutomataAutomaton; the automata themselves
// (sc4/Automaton.*, TrafficAutomaton.*, Vehicle.*) were ported with their own seams. In the original both are the same object
// (cSC4Automaton's primary vtable); here a view attached to each automaton forwards every slot to the automaton's method at the
// same vtable offset.
#pragma once
#include <cstdint>

class cIGZUnknown;

namespace sc4 {
class IAutomataAutomaton;
class ITrafficAutomataAutomaton;
class IAutomataOccupant;
class IAutomataGroupTemplate;
class cSC4Automaton;
class cSC4OccupantGroupTemplate;
class cISC4Occupant;
}  // namespace sc4

namespace native {

// The view of an automaton object (a cSC4Automaton answering 0xa9b40f05), made on first use and owned by the automaton; null
// for an object that is not an automaton. The view's AddRef / Release are the automaton's.
sc4::IAutomataAutomaton* AutomatonView(cIGZUnknown* object);
sc4::IAutomataAutomaton* AutomatonView(sc4::cSC4Automaton* automaton);
// The automaton behind a view (null for anything else).
sc4::cSC4Automaton* AutomatonOf(sc4::IAutomataAutomaton* view);
// The traffic view (null unless the automaton is a cSC4TrafficAutomaton).
sc4::ITrafficAutomataAutomaton* TrafficAutomatonView(sc4::IAutomataAutomaton* view);
// The manager's view of a script group template (one per template, kept for the process).
sc4::IAutomataGroupTemplate* GroupTemplateView(sc4::cSC4OccupantGroupTemplate* group);

}  // namespace native
