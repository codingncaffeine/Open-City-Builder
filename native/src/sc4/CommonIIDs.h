// Interface ids several sc4 headers need. They used to be defined again in each header that used them, which made those
// headers impossible to include together in one translation unit (a redefinition error) - and the engine wiring that
// installs the classes' hosts has to include many of them at once. One definition, here.
#pragma once
#include <cstdint>

namespace sc4 {

constexpr std::uint32_t kGZIID_cIGZMessageTarget2 = 0x452294aa;
constexpr std::uint32_t kGZIID_cIGZSerializable = 0xe4fda3d4;
constexpr std::uint32_t kGZIID_cISC4NetworkOccupant = 0xa821ef94;

// Port-only: QueryInterface id on the adapters that wrap a plain (non-COM) recovered object; hands out the wrapped object
// (borrowed, valid while the adapter lives).
constexpr std::uint32_t kGZIID_PlainObject = 0x506c6e31;

// Occupant property that excludes an occupant from the automata system (the automata managers' occupant handlers).
constexpr std::uint32_t kPropertyNoAutomata = 0x029244db;

}  // namespace sc4
