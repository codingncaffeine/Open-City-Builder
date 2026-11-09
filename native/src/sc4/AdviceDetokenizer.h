// gAdviceDetokenizerFunction — the advisor system's "unknown token" method: the cSCStringDetokenizer calls it for every
// #token# of an advice text it has no replacement for. Mac i386 0x00303df2..0x00304a1c (decompile scratch/s51/ghidra/
// detok.c, disassembly detok_asm.txt); 64-bit 0x1003fb0a0. The context is the cSC4AdvisorSystem that registered it.
//
// Tokens (trimmed, compared without case):
//   link_id / sit_link_id         "sc4://advice/%08x%08x" of the context advice's advisor id and SESSION id (0, 0 without
//                                 one): the prefix of every anchor of an advice page (cSC4AdvisorSystem::DoLink reads it)
//   mission_target                the city situation manager's current target name
//   num_targets / cur_target_index / num_targets_remaining     "%d" (the index is shown from 1)
//   mission_mayor_rating          "%+.0f" of the situation's aura magnitude; mission_failure_mayor_rating the active
//                                 situation's failure aura magnitude
//   mission_money                 "%+lld" of the situation's money; mission_failure_money the active situation's
//   advisor                       the context advice's advisor's personal name
//   building_initial_cost / building_monthly_cost     the money string of the context advice's subject data1 / data2
//   anything else                 evaluated in the advisors' Lua as "_dasd_=(TOKEN)". An optional two-character prefix
//                                 X@ picks the format of a number: s@ "%i", m@ money, d@ date (a Julian day), t@ time
//                                 (quirk: formatted with MakeDateString as (hour, minute, second)), p@ number + "%", any
//                                 other letter or none = the language's number string. A string answer is copied; a
//                                 user value with the advisor system's data tag 1 is a cIGZString copied as is.
// When nothing answers (no city, no situation, no advice, a Lua error, a nil result) the text becomes
// "%UNKNOWN TOKEN --name--%" and the method answers false.
#pragma once

class cIGZString;

namespace sc4 {

class cISC4AdvisorSystem;
class cSC4CitySituationManager;

// What the method reaches outside the advisor system.
class IAdviceDetokenizerHost {
public:
    virtual ~IAdviceDetokenizerHost() = default;
    // SL::spCity vslot 0xc4: the city situation manager (borrowed; null without a city).
    virtual cSC4CitySituationManager* SituationManager() = 0;
    // The context the method was registered with (the cSC4AdvisorSystem's `this`) as its cISC4AdvisorSystem view.
    virtual cISC4AdvisorSystem* System(void* context) = 0;
};
IAdviceDetokenizerHost* AdviceDetokenizerHost();
void SetAdviceDetokenizerHost(IAdviceDetokenizerHost* host);

// The method itself (a DetokenizeMethod); `context` is the cSC4AdvisorSystem.
bool AdviceDetokenizerFunction(const cIGZString& token, cIGZString& out, void* context);

}  // namespace sc4
