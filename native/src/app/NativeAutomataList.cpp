// The automata a manager restored from the city file. Kept in its own translation unit: the recovered AutomataManager.h and Automaton.h
// cannot be included together. The manager's registry holds the loaded objects as they came out of the serialization factory (the
// recovered cSC4Automaton does not implement the manager's IAutomataAutomaton view), so they are handed out as plain pointers.
#include "app/NativeAutomata.h"

#include "app/NativeAutomataViews.h"
#include "sc4/AutomataManager.h"

namespace native {

std::vector<void*> LoadedAutomataOf(cIGZUnknown* manager) {
    std::vector<void*> out;
    void* raw = nullptr;
    if (!manager || !manager->QueryInterface(sc4::kGZIID_cISC4AutomataManager, &raw) || !raw) return out;
    auto* base = static_cast<sc4::cSC4AutomataManager*>(static_cast<sc4::cISC4AutomataManager*>(raw));
    for (sc4::IAutomataAutomaton* automaton : base->Automata())
        if (sc4::cSC4Automaton* object = AutomatonOf(automaton)) out.push_back(object);   // the list holds the managers' views
    static_cast<cIGZUnknown*>(raw)->Release();
    return out;
}

}  // namespace native
