#include "sc4/CitySituationAdvice.h"

#include <algorithm>

namespace sc4 {

namespace {

ICitySituationAdviceHost* g_host = nullptr;

// Instance ids of the success condition descriptions in the advisor text group, indexed by condition - 1
// (table at 0x100a28660; only conditions 1, 2, 3 and 5 have a text, mask 0x17).
constexpr std::uint32_t kConditionTextInstances[5] = {0x0c2bd5fb, 0x2c2bd613, 0x0c2bd605, 0x0c2bd5fb, 0x8c2bd60a};
constexpr std::uint32_t kEvilTwinFormatInstance = 0x2c104d8c;

// The "static cADVString sString" objects the getters hand out (shared between all situations, like the original).
cADVString& SharedTableString() {   // ReadStringFromTable(char const*)::sString
    static cADVString s("");
    return s;
}
cADVString& SharedFullText() {      // GetTextMessage()::sFullText
    static cADVString s("");
    return s;
}
cADVString& SharedProgressText() {  // GetProgressText(unsigned short)::retString
    static cADVString s("");
    return s;
}

void ClearText(cRZString& s) { s.AssignFromChars("", 0); }

// RAII for the "remember the stack top, restore it at the end" pattern the table readers use.
struct LuaStackGuard {
    ISituationLua& lua;
    int top;
    explicit LuaStackGuard(ISituationLua& l) : lua(l), top(l.GetTop()) {}
    ~LuaStackGuard() { lua.SetTop(top); }
};

}  // namespace

ICitySituationAdviceHost* CitySituationAdviceHost() { return g_host; }
void SetCitySituationAdviceHost(ICitySituationAdviceHost* host) { g_host = host; }

// ---------------------------------------------------------------------------------------------- lifetime

// GZCOM_CreatecSC4CitySituationAdvice @ 0x1006a81a0 allocates 0x290 bytes and hands out the +8 (cSC4Advice) view;
// GZCOM_cSC4CitySituationAdviceCLSID @ 0x1006a81e6 answers kGZCLSID_cSC4CitySituationAdvice.
cSC4CitySituationAdvice* CreateCitySituationAdvice(ISituationAdviceBase* base) { return new cSC4CitySituationAdvice(base); }
std::uint32_t CitySituationAdviceClassId() { return kGZCLSID_cSC4CitySituationAdvice; }

cSC4CitySituationAdvice::cSC4CitySituationAdvice(ISituationAdviceBase* base) : base_(base) {}
cSC4CitySituationAdvice::~cSC4CitySituationAdvice() = default;

std::uint32_t cSC4CitySituationAdvice::AddRef() {
    if (cIGZUnknown* outer = base_ ? base_->Outer() : nullptr) return outer->AddRef();
    return ++refCount_;
}

std::uint32_t cSC4CitySituationAdvice::Release() {
    if (cIGZUnknown* outer = base_ ? base_->Outer() : nullptr) return outer->Release();
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4CitySituationAdvice::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4CitySituationAdvice) {
        *out = static_cast<cISC4CitySituationAdvice*>(this);
        AddRef();
        return true;
    }
    return base_ ? base_->QueryBaseInterface(iid, out) : false;
}

std::uint32_t cSC4CitySituationAdvice::GetTrigger() const { return base_ ? base_->Trigger() : 0; }

bool cSC4CitySituationAdvice::Shutdown() {
    if (base_) base_->Shutdown();
    ICitySituationAdviceHost* host = g_host;
    ISituationLua* lua = host ? host->Lua() : nullptr;
    if (lua && scriptTableRef_ != -2) {
        lua->Unref(scriptTableRef_);
        scriptTableRef_ = -2;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- Init

// Registers the advice with the advisor system, resolves advices[<id>] to a registry reference and reads the group
// lists and creation flags. Answers the base advice's Init result; false when there is no script host or the table row is
// missing (then the advisor system is told through hook 0x98).
bool cSC4CitySituationAdvice::Init(int adviceId) {
    const bool baseOk = base_ ? base_->Init(adviceId) : false;
    if (base_) base_->SetFlags(0x2000, true);
    ICitySituationAdviceHost* host = g_host;
    ISituationLua* lua = host ? host->Lua() : nullptr;
    if (!lua) return false;
    host->AdvisorSystemRegister(base_);
    const int savedTop = lua->GetTop();
    lua->GetGlobal("advices");
    lua->PushNumber(static_cast<double>(base_ ? base_->AdviceId() : adviceId));
    lua->RawGetTable(-2);
    if (!lua->IsTable(-1)) {
        lua->SetTop(savedTop);
        host->AdvisorSystemReleaseHook();
        return false;
    }
    const int rowIndex = lua->GetTop();
    lua->PushValue(rowIndex);
    scriptTableRef_ = lua->Ref(true);

    // A group list may be a single number or a table of numbers. Quirk: the single number of "target_sequence" is
    // appended to the TARGET GROUP list (+0x180), not to the sequence (+0x198).
    auto readList = [&](const char* field, std::vector<std::uint32_t>& list, std::vector<std::uint32_t>& singleNumberTarget) {
        if (!lua->GetField(field, rowIndex)) return false;
        if (!lua->IsTable(-1)) {
            if (lua->IsNumber(-1)) singleNumberTarget.push_back(static_cast<std::uint32_t>(static_cast<long long>(lua->ToNumber(-1))));
        } else {
            lua->ReadTableAsArrayU32(list, -1);
        }
        return true;
    };
    if (readList("automata_list", automataGroups_, automataGroups_)) lua->Pop(1);
    if (readList("target_list", targetGroups_, targetGroups_)) lua->Pop(1);
    if (lua->GetField("target_sequence", rowIndex)) {
        flags_ |= kSituationFlagTargetSequence;
        if (!lua->IsTable(-1)) {
            if (lua->IsNumber(-1)) targetGroups_.push_back(static_cast<std::uint32_t>(static_cast<long long>(lua->ToNumber(-1))));
        } else {
            lua->ReadTableAsArrayU32(targetSequence_, -1);
            // zero entries are dropped from the sequence
            targetSequence_.erase(std::remove(targetSequence_.begin(), targetSequence_.end(), 0u), targetSequence_.end());
        }
        // (no Pop: the final SetTop discards it)
    }
    bool flag = false;
    lua->ReadFieldBool("create_target", flag, rowIndex);
    if (flag) flags_ |= kSituationFlagCreateTarget;
    flag = false;
    lua->ReadFieldBool("always_create_target", flag, rowIndex);
    if (flag) flags_ |= kSituationFlagAlwaysCreateTarget;
    flag = false;
    lua->ReadFieldBool("training_mission", flag, rowIndex);
    if (flag) flags_ |= kSituationFlagTraining;
    UpdateSituationFields();
    lua->SetTop(savedTop);
    return baseOk;
}

// ---------------------------------------------------------------------------------------------- UpdateSituationFields

// Rereads every tunable of the situation from its script row. Quirk fixed: "condition", "coverage_type" and
// "coverage_zone" are read into uninitialised locals in the original (garbage when the field is missing); here they
// default to the current value.
void cSC4CitySituationAdvice::UpdateSituationFields() {
    ICitySituationAdviceHost* host = g_host;
    ISituationLua* lua = host ? host->Lua() : nullptr;
    if (!lua || scriptTableRef_ == -2) return;
    const int savedTop = lua->GetTop();
    lua->GetRef(scriptTableRef_);
    const int t = lua->GetTop();

    std::uint32_t condition = successCondition_;
    lua->ReadFieldU32("condition", condition, t);
    successCondition_ = condition;
    lua->ReadFieldFloat("time_limit", timeLimit_, t);
    lua->ReadFieldFloat("success_distance", successDistance_, t);
    lua->ReadFieldFloat("min_target_distance", minTargetDistance_, t);
    lua->ReadFieldFloat("max_target_distance", maxTargetDistance_, t);
    lua->ReadFieldFloat("success_timeout", successTimeout_, t);
    lua->ReadFieldFloat("delay_after_disabled", delayAfterDisabled_, t);
    lua->ReadFieldFloat("active_radius", activeRadius_, t);
    bool flag = false;
    lua->ReadFieldBool("service_mission", flag, t);
    if (flag) flags_ |= kSituationFlagServiceMission;
    flag = false;
    lua->ReadFieldBool("use_lot_boundary", flag, t);
    if (flag) flags_ |= kSituationFlagUseLotBoundary;
    flag = false;
    lua->ReadFieldBool("mysim_mission", flag, t);
    if (flag) flags_ |= kSituationFlagMySimMission;
    bool pursuit = false;
    lua->ReadFieldBool("pursuit_mission", pursuit, t);
    if (pursuit) flags_ |= kSituationFlagPursuit;
    if (lua->GetField("evade_list", t)) {
        if (!pursuit) flags_ |= kSituationFlagEvade;
        lua->ReadTableAsArrayU32(evadeList_, -1);
        lua->ReadFieldFloat("evade_distance", evadeDistance_, t);
        lua->ReadFieldFloat("evade_timeout", evadeTimeout_, t);
    }

    std::uint32_t coverageType = coverageType_;
    lua->ReadFieldU32("coverage_type", coverageType, t);
    coverageType_ = coverageType;
    switch (coverageType) {
    case kCoverageZone: {
        std::uint32_t zone = coverageZone_;
        lua->ReadFieldU32("coverage_zone", zone, t);
        coverageZone_ = zone;
        break;
    }
    case kCoverageLot:
        lua->ReadFieldU32("coverage_lot", coverageOccupantGroup_, t);
        break;
    case kCoverageNetwork:
        lua->ReadFieldU32("coverage_network", coverageNetworkType_, t);
        break;
    case kCoverageRect: {
        std::fill(std::begin(coverageRect_), std::end(coverageRect_), static_cast<std::uint16_t>(0));
        if (!lua->GetField("coverage_rect", t)) break;
        std::vector<std::uint32_t> values;
        lua->ReadTableAsArrayU32(values, -1);
        if (values.size() < 4) values.resize(4);
        for (int i = 0; i < 4; ++i) coverageRect_[i] = static_cast<std::uint16_t>(values[i]);
        break;
    }
    case kCoverageCells: {
        cellList_.clear();
        if (!lua->GetField("coverage_cells", t)) break;
        struct Context { ISituationLua* lua; std::list<SituationCell>* cells; } context{lua, &cellList_};
        lua->ForEachEntry(
            [](void* raw) {
                auto* c = static_cast<Context*>(raw);
                if (!c->lua->IsTable(-1)) return;
                std::vector<std::uint32_t> pair;
                c->lua->ReadTableAsArrayU32(pair, -1);
                if (pair.size() > 1) c->cells->push_back({static_cast<std::uint16_t>(pair[0]), static_cast<std::uint16_t>(pair[1])});
            },
            &context);
        lua->Pop(1);
        break;
    }
    default:
        break;
    }
    lua->ReadFieldU32("coverage_cells_min", cellsMin_, t);
    lua->ReadFieldU32("coverage_cells_max", cellsMax_, t);
    lua->ReadFieldU32("coverage_success_count", successCount_, t);
    float pct = 100.0f;
    lua->ReadFieldFloat("coverage_success_pct", pct, t);
    successPct_ = pct * 0.01f;   // mulss with the float constant 0.01f (0x100a15928)
    lua->ReadFieldU32("evil_twin", evilTwinGuid_, t);
    lua->ReadFieldU32("reward_guid", rewardGuid_, t);
    lua->ReadFieldFloat("success_aura_radius", successAuraRadius_, t);
    lua->ReadFieldFloat("success_aura_magnitude", successAuraMagnitude_, t);
    lua->ReadFieldString("success_effect", successEffect_, t);
    lua->ReadFieldInt("success_money", successMoney_, t);
    lua->ReadFieldFloat("failure_aura_radius", failureAuraRadius_, t);
    lua->ReadFieldFloat("failure_aura_magnitude", failureAuraMagnitude_, t);
    lua->ReadFieldString("failure_effect", failureEffect_, t);
    lua->ReadFieldInt("failure_money", failureMoney_, t);
    lua->SetTop(savedTop);
}

// ---------------------------------------------------------------------------------------------- script table reads

const cRZString& cSC4CitySituationAdvice::ReadStringFromTable(const char* key) const {
    cADVString& result = SharedTableString();
    ClearText(result);
    ICitySituationAdviceHost* host = g_host;
    ISituationLua* lua = host ? host->Lua() : nullptr;
    if (scriptTableRef_ != -2 && lua) {
        LuaStackGuard guard(*lua);
        lua->GetRef(scriptTableRef_);
        const int t = lua->GetTop();
        if (t != guard.top) {
            cRZString raw;
            lua->ReadFieldString(key, raw, t);
            result.FromScriptText(raw.ToChar(), raw.Strlen());
            if (!result.Load(true, true)) {
                result.AssignFromChars(raw.ToChar(), raw.Strlen());
                result.ConvertTokens(nullptr, true);
            }
        }
    }
    return result;
}

std::uint32_t cSC4CitySituationAdvice::ReadUint32FromTable(const char* key) const {
    ICitySituationAdviceHost* host = g_host;
    ISituationLua* lua = host ? host->Lua() : nullptr;
    if (scriptTableRef_ == -2 || !lua) return 0;
    LuaStackGuard guard(*lua);
    std::uint32_t value = 0;
    lua->GetRef(scriptTableRef_);
    const int t = lua->GetTop();
    if (t != guard.top) lua->ReadFieldU32(key, value, t);
    return value;
}

const cRZString& cSC4CitySituationAdvice::GetTextTitle() const { return ReadStringFromTable("title"); }
const cRZString& cSC4CitySituationAdvice::GetSuccessText() const { return ReadStringFromTable("success_text"); }
const cRZString& cSC4CitySituationAdvice::GetFailureText() const { return ReadStringFromTable("failure_text"); }
const cRZString& cSC4CitySituationAdvice::GetRewardProgressText() const { return ReadStringFromTable("reward_progress_text"); }
const cRZString& cSC4CitySituationAdvice::GetRewardUnlockedText() const { return ReadStringFromTable("reward_unlocked_text"); }

std::uint32_t cSC4CitySituationAdvice::GetSituationImageId() const { return ReadUint32FromTable("image"); }
std::uint32_t cSC4CitySituationAdvice::GetSuccessImageId() const {
    const std::uint32_t id = ReadUint32FromTable("success_image");
    return id != 0 ? id : GetSituationImageId();
}
std::uint32_t cSC4CitySituationAdvice::GetRewardUnlockedImageId() const {
    const std::uint32_t id = ReadUint32FromTable("reward_unlocked_image");
    return id != 0 ? id : GetSuccessImageId();
}
std::uint32_t cSC4CitySituationAdvice::GetSuccessMood() const {
    const std::uint32_t mood = ReadUint32FromTable("success_mood");
    return mood != 0 ? mood : static_cast<std::uint32_t>(base_ ? base_->Mood() : 0);
}
std::uint32_t cSC4CitySituationAdvice::GetFailureImageId() const {
    const std::uint32_t id = ReadUint32FromTable("failure_image");
    return id != 0 ? id : GetSituationImageId();
}
std::uint32_t cSC4CitySituationAdvice::GetFailureMood() const {
    const std::uint32_t mood = ReadUint32FromTable("failure_mood");
    return mood != 0 ? mood : static_cast<std::uint32_t>(base_ ? base_->Mood() : 0);
}

// "progress_text" is an array of strings; `index` is 1-based, an out-of-range index falls back to the first entry,
// index 0 (or a missing table) gives the empty string.
const cRZString& cSC4CitySituationAdvice::GetProgressText(std::uint16_t index) const {
    cADVString& result = SharedProgressText();
    ClearText(result);
    ICitySituationAdviceHost* host = g_host;
    ISituationLua* lua = host ? host->Lua() : nullptr;
    if (index != 0 && scriptTableRef_ != -2 && lua) {
        LuaStackGuard guard(*lua);
        lua->GetRef(scriptTableRef_);
        const int t = lua->GetTop();
        if (t != guard.top && lua->GetField("progress_text", t)) {
            std::vector<cRZString> entries;
            lua->ReadTableAsArrayString(entries, -1);
            const std::size_t count = entries.size();
            const std::size_t wanted = static_cast<std::uint16_t>(index - 1);
            const std::size_t chosen = wanted < count ? wanted : 0;
            if (chosen < count) {
                result.FromScriptText(entries[chosen].ToChar(), entries[chosen].Strlen());
                if (!result.Load(true, true)) {
                    result.AssignFromChars(entries[chosen].ToChar(), entries[chosen].Strlen());
                    result.ConvertTokens(nullptr, true);
                }
            }
        }
    }
    return result;
}

// Text of the situation's message; when an "evil twin" advice exists and is available its description is appended
// after "&nbsp;<BR>&nbsp;" (separator order reconstructed from the two std::string concatenations).
const cRZString& cSC4CitySituationAdvice::GetTextMessage() const {
    cADVString& fullText = SharedFullText();
    const std::string message = ReadStringFromTable("message").ToChar();
    ICitySituationAdviceHost* host = g_host;
    const std::uint32_t guid = evilTwinGuid_;
    if (guid != 0 && host && base_ && host->IsLinkedAdviceAvailable(base_->AdvisorId(), guid)) {
        cSLString format(kAdvisorTextGroup, kEvilTwinFormatInstance, false);
        cSLString twin;
        twin.Sprintf(format.ToChar(), guid);
        twin.ConvertTokens(nullptr, true);
        const std::string text = message + "&nbsp;<BR>&nbsp;" + twin.ToChar();
        fullText.AssignFromChars(text.data(), static_cast<std::uint32_t>(text.size()));
    } else {
        fullText.AssignFromChars(message.data(), static_cast<std::uint32_t>(message.size()));
    }
    return fullText;
}

bool cSC4CitySituationAdvice::GetSuccessConditionText(cIGZString& out) const {
    const std::uint32_t index = GetSuccessCondition() - 1;
    if (index < 5 && ((0x17u >> index) & 1u)) {
        cSLString text(kAdvisorTextGroup, kConditionTextInstances[index], true);
        out.AssignFromIGZString(text);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------- simple queries

float cSC4CitySituationAdvice::GetMaxTargetDistance() const {
    if (maxTargetDistance_ > 0.0f) return maxTargetDistance_;
    ICitySituationAdviceHost* host = g_host;
    if (!host) return 0.0f;
    const float cells = static_cast<float>(host->CityCellsX());
    return host->CityCellSizeXMeters() * cells;
}

bool cSC4CitySituationAdvice::GetSuccessAura(float& radius, float& magnitude) const {
    radius = successAuraRadius_;
    magnitude = successAuraMagnitude_;
    return 0.0f < radius;
}
bool cSC4CitySituationAdvice::GetFailureAura(float& radius, float& magnitude) const {
    radius = failureAuraRadius_;
    magnitude = failureAuraMagnitude_;
    return 0.0f < radius;
}
bool cSC4CitySituationAdvice::GetSuccessEffectName(cRZString& out) const {
    out = successEffect_;
    return out.Strlen() != 0;
}
bool cSC4CitySituationAdvice::GetFailureEffectName(cRZString& out) const {
    out = failureEffect_;
    return out.Strlen() != 0;
}
bool cSC4CitySituationAdvice::GetSuccessMoney(long long& money) const {
    money = successMoney_;
    return money != 0;
}
bool cSC4CitySituationAdvice::GetFailureMoney(long long& money) const {
    money = failureMoney_;
    return money != 0;
}

bool cSC4CitySituationAdvice::IsAutomatonRelevant(ISituationAutomaton* automaton) const {
    if (!automaton) return false;
    for (std::uint32_t group : automataGroups_) {
        ISituationOccupant* occupant = automaton->Occupant();
        if (occupant && occupant->IsInGroup(group)) return true;
    }
    return false;
}

bool cSC4CitySituationAdvice::IsGroupRelevant(const std::vector<std::uint32_t>& groups) const {
    for (std::uint32_t mine : automataGroups_)
        for (std::uint32_t g : groups)
            if (g == mine) return true;
    return false;
}

}  // namespace sc4
