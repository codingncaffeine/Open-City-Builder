#include "sc4/AdvisorCitySituations.h"

#include "platform/Unwired.h"
#include "sc4/CitySituationAdvice.h"

namespace sc4 {

namespace {
std::function<void()> g_refresh;
}

void SetAdvisorCitySituationsRefresh(std::function<void()> refresh) { g_refresh = std::move(refresh); }

bool cSC4AdvisorCitySituations::DoMessage(cIGZMessage2* message) { return cSC4AdvisorBase::DoMessage(message); }

bool cSC4AdvisorCitySituations::Init(int advisorType) {
    updatePending_ = true;
    return cSC4AdvisorBase::Init(advisorType);
}

bool cSC4AdvisorCitySituations::Shutdown() { return cSC4AdvisorBase::Shutdown(); }

void cSC4AdvisorCitySituations::UpdateMySimAdvices() {
    if (g_refresh) g_refresh();
    else SC4_UNWIRED("cSC4AdvisorCitySituations::UpdateMySimAdvices: the sim-agent manager refresh (SL::spCity 0x238 -> 0x60 -> 0x220) has no host");
    auto& advices = MutableAdvices();
    auto& current = MutableCurrentAdvices();
    for (int index = 0; index < static_cast<int>(advices.size()); ++index) {
        cISC4Advice* advice = advices[index].advice.get();
        if (!advice) continue;
        cISC4CitySituationAdvice* situation = nullptr;
        if (!advice->QueryInterface(kGZIID_cISC4CitySituationAdvice, reinterpret_cast<void**>(&situation)) || !situation) continue;
        if ((situation->GetSituationFlags() & kSituationAdviceActiveFlag) != 0) {
            if (!advice->GetTrigger()) {
                auto it = current.find(advices[index].advice);
                if (it != current.end()) current.erase(it);
                on_advice_deactivation(advices[index]);
            } else {
                current[advices[index].advice] = index;
                on_advice_activation(advices[index]);
            }
        }
        situation->Release();
    }
}

bool cSC4AdvisorCitySituations::RetireAdvice(cISC4Advice* advice) {
    if (!advice || !advice->CheckFlags(2)) return false;
    auto& current = MutableCurrentAdvices();
    auto& advices = MutableAdvices();
    auto it = current.find(AdviceRef(advice));
    if (it != current.end()) {
        const std::size_t index = static_cast<std::size_t>(it->second);
        if (index < advices.size()) {
            current.erase(it);
            on_advice_deactivation(advices[index]);
        }
    }
    return true;
}

}  // namespace sc4
