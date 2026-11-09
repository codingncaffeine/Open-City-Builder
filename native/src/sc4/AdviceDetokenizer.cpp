#include "sc4/AdviceDetokenizer.h"

#include <cstdint>
#include <string>

#include "gz/RZServices.h"
#include "lua/ISCLua.h"
#include "rz/LanguageManager.h"
#include "rz/cRZDate.h"
#include "rz/cRZString.h"
#include "sc4/Advice.h"
#include "sc4/AdviceSubject.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/CitySituationAdvice.h"
#include "sc4/CitySituationManager.h"
#include "sc4/SC4Utilities.h"

namespace sc4 {

namespace {

IAdviceDetokenizerHost* g_host = nullptr;

// The token statics (0x00c0e4a0..0x00c0e530, literals 0x005d7910..0x005d79ec).
const cRZString kLinkIdToken("link_id");
const cRZString kSituationLinkIdToken("sit_link_id");
const cRZString kAdvisorNameToken("advisor");
const cRZString kRewardBuildingPlopCost("building_initial_cost");
const cRZString kRewardBuildingBudgetCost("building_monthly_cost");
const cRZString kSituationTargetNameToken("mission_target");
const cRZString kSituationNumTargetsToken("num_targets");
const cRZString kSituationTargetIndexToken("cur_target_index");
const cRZString kSituationTargetsRemainingToken("num_targets_remaining");
const cRZString kSituationMayorRatingToken("mission_mayor_rating");
const cRZString kSituationMayorRatingFailureToken("mission_failure_mayor_rating");
const cRZString kSituationMoneyToken("mission_money");
const cRZString kSituationMoneyFailureToken("mission_failure_money");

constexpr char kLinkFormat[] = "sc4://advice/%08x%08x";   // 0x005d78f8
constexpr char kLuaResultName[] = "_dasd_";                // 0x005d7a14
constexpr int kStringDataTag = 1;                          // GetDataTag(1): a cIGZString user value

// The number formats an "X@" prefix selects.
enum class NumberFormat { kNumber = 0, kInteger = 1, kMoney = 2, kDate = 3, kTime = 4, kPercent = 5 };

bool Is(const cRZString& token, const cRZString& name) { return token.IsEqualToStringRef(name, false); }

cSC4CitySituationManager* Situations() { return g_host ? g_host->SituationManager() : nullptr; }

// The generic path: "_dasd_=(TOKEN)" in the advisors' Lua and its answer formatted. False when nothing answered.
bool EvaluateInLua(cISC4AdvisorSystem* system, const cRZString& token, NumberFormat format, cIGZLanguageUtility* language,
                   cIGZString& out) {
    cISCLua* lua = system->GetScriptingContext();
    if (!lua) return false;   // port guard
    const std::string code = std::string(kLuaResultName) + "=(" + std::string(token.ToChar(), token.Strlen()) + ")";
    if (lua->DoString(code.c_str()) != 0) return false;
    const int top = lua->GetTop();
    lua->GetGlobal(kLuaResultName);
    bool answered = true;
    if (lua->IsNumber(-1)) {
        const auto value = static_cast<std::int64_t>(lua->ToNumber(-1));   // truncated (FISTTP)
        switch (format) {
        case NumberFormat::kNumber:
            if (language) language->MakeNumberString(value, out);
            break;
        case NumberFormat::kInteger:
            out.Sprintf("%i", static_cast<int>(value));
            break;
        case NumberFormat::kMoney:
            SC4MakeMoneyString(value, out);
            break;
        case NumberFormat::kDate: {
            const cRZDate date = cRZDate::FromJulianDay(static_cast<std::uint32_t>(value));
            if (language) language->MakeDateString(date.Month(), date.DayOfMonth(), date.Year(), out, cIGZLanguageUtility::kDateNumeric);
            break;
        }
        case NumberFormat::kTime: {
            // Quirk kept: the original passes hour, minute and second to MakeDateString (i386 call 0x28 = entry +0x30).
            // cRZTime::Hour / Minute read LocalSecs (the local DST offset added); the port uses the seconds as given.
            const auto seconds = static_cast<std::uint32_t>(value);
            if (language) language->MakeDateString((seconds % 86400) / 3600, (seconds % 3600) / 60, seconds % 60, out,
                                                   cIGZLanguageUtility::kDateNumeric);
            break;
        }
        case NumberFormat::kPercent:
            if (language) language->MakeNumberString(value, out);
            out.AppendString(cRZString("%"));
            break;
        }
    } else if (lua->IsUserData(-1)) {
        if (lua->Tag(-1) == system->GetDataTag(kStringDataTag)) {
            auto* text = static_cast<cIGZString*>(lua->ToUserTag(-1));
            if (text) out.Copy(*text);   // port guard on a null value
        } else {
            answered = false;
        }
    } else if (lua->IsString(-1)) {
        const char* text = lua->ToString(-1);
        out.AssignFromIGZString(cRZString(text ? text : ""));
    } else {
        answered = false;
    }
    lua->SetTop(top);
    return answered;
}

}  // namespace

IAdviceDetokenizerHost* AdviceDetokenizerHost() { return g_host; }
void SetAdviceDetokenizerHost(IAdviceDetokenizerHost* host) { g_host = host; }

// @ 0x00303df2 (i386).
bool AdviceDetokenizerFunction(const cIGZString& tokenIn, cIGZString& out, void* context) {
    cIGZLanguageManager* languages = RZ::LM();
    cIGZLanguageUtility* language = languages ? languages->GetLanguageUtility(0) : nullptr;
    cRZString token(tokenIn);
    token.Trim();
    cISC4AdvisorSystem* system = g_host && context ? g_host->System(context) : nullptr;
    bool answered = false;

    if (Is(token, kLinkIdToken) || Is(token, kSituationLinkIdToken)) {
        cISC4Advice* advice = system ? system->GetContextAdvice() : nullptr;
        const int advisorId = advice ? advice->GetAdvisorID() : 0;
        const int sessionId = advice ? advice->GetSessionID() : 0;
        out.Sprintf(kLinkFormat, static_cast<unsigned>(advisorId), static_cast<unsigned>(sessionId));
        return true;
    }
    if (Is(token, kSituationTargetNameToken)) {
        if (cSC4CitySituationManager* situations = Situations()) {
            cRZString name;
            situations->GetCurrentTargetName(name);
            out.AssignFromIGZString(name);
            return true;
        }
    } else if (Is(token, kSituationNumTargetsToken)) {
        if (cSC4CitySituationManager* situations = Situations()) {
            out.Sprintf("%d", static_cast<int>(situations->GetNumTargets()));
            return true;
        }
    } else if (Is(token, kSituationTargetIndexToken)) {
        if (cSC4CitySituationManager* situations = Situations()) {
            out.Sprintf("%d", static_cast<int>(situations->GetCurrentTargetIndex()) + 1);
            return true;
        }
    } else if (Is(token, kSituationTargetsRemainingToken)) {
        if (cSC4CitySituationManager* situations = Situations()) {
            out.Sprintf("%d", static_cast<int>(situations->GetNumTargetsRemaining()));
            return true;
        }
    } else if (Is(token, kSituationMayorRatingToken)) {
        if (cSC4CitySituationManager* situations = Situations()) {
            float radius = 0.0f, magnitude = 0.0f;
            situations->GetSituationAura(radius, magnitude);
            out.Sprintf("%+.0f", static_cast<double>(magnitude));
            return true;
        }
    } else if (Is(token, kSituationMayorRatingFailureToken)) {
        cSC4CitySituationManager* situations = Situations();
        const cISC4CitySituationAdvice* situation = situations ? situations->GetActiveSituation() : nullptr;
        if (situation) {
            float radius = 0.0f, magnitude = 0.0f;
            situation->GetFailureAura(radius, magnitude);
            out.Sprintf("%+.0f", static_cast<double>(magnitude));
            return true;
        }
    } else if (Is(token, kSituationMoneyToken)) {
        if (cSC4CitySituationManager* situations = Situations()) {
            std::int64_t money = 0;
            situations->GetSituationMoney(money);
            out.Sprintf("%+lld", static_cast<long long>(money));
            return true;
        }
    } else if (Is(token, kSituationMoneyFailureToken)) {
        cSC4CitySituationManager* situations = Situations();
        const cISC4CitySituationAdvice* situation = situations ? situations->GetActiveSituation() : nullptr;
        if (situation) {
            long long money = 0;
            situation->GetFailureMoney(money);
            out.Sprintf("%+lld", money);
            return true;
        }
    } else if (Is(token, kAdvisorNameToken)) {
        cISC4Advice* advice = system ? system->GetContextAdvice() : nullptr;
        cISC4Advisor* advisor = advice ? system->GetAdvisor(advice->GetAdvisorID()) : nullptr;
        if (advisor) {
            // PARTIAL: the original first fits the name into 200 pixels of the window font 0x68963c4c (RZ::WCM() vslot
            // 0x14, font vslot 0xc8 with style 0x5377be31); there is no control manager natively, so it is used whole.
            out.Copy(advisor->GetPersonalName());
            return true;
        }
    } else if (Is(token, kRewardBuildingPlopCost) || Is(token, kRewardBuildingBudgetCost)) {
        cISC4Advice* advice = system ? system->GetContextAdvice() : nullptr;
        cISC4AdviceSubject* subject = advice ? advice->GetEventSubject() : nullptr;
        if (subject) {
            const std::uint32_t cost = Is(token, kRewardBuildingPlopCost) ? subject->GetData1() : subject->GetData2();
            SC4MakeMoneyString(static_cast<std::int64_t>(cost), out);
            return true;
        }
    } else if (system) {
        // The generic path, with the optional "X@" format prefix stripped first.
        NumberFormat format = NumberFormat::kNumber;
        if (token.Strlen() > 1 && token.ToChar()[1] == '@') {
            switch (token.ToChar()[0]) {
            case 'D': case 'd': format = NumberFormat::kDate; break;
            case 'M': case 'm': format = NumberFormat::kMoney; break;
            case 'P': case 'p': format = NumberFormat::kPercent; break;
            case 'S': case 's': format = NumberFormat::kInteger; break;
            case 'T': case 't': format = NumberFormat::kTime; break;
            default: break;
            }
            token = token.Right(token.Strlen() - 2);
        }
        answered = EvaluateInLua(system, token, format, language, out);
        if (answered) return true;
    }
    out.Sprintf("%%UNKNOWN TOKEN --%s--%%", token.ToChar());
    return false;
}

}  // namespace sc4
