// The automata instruments' shared state (SC4_MOVELIFE): the manager names why it is removing an automaton, and the automaton
// records that reason the first time it is marked for removal.
#pragma once

namespace sc4 {
inline const char* gAutomatonRemovalContext = nullptr;
}  // namespace sc4
