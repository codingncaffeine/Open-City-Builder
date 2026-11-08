#include "sc4/AdvisorSystem.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "gz/CheatCodeManager.h"
#include "gz/cGZDate.h"
#include "lua/SCLuaHelp.h"
#include "rz/RZStringUtils.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/DBSegment.h"
#include "sc4/ScriptLuaAdapter.h"
#include "sc4/TablePropertyReader.h"

namespace sc4 {

namespace {

IAdvisorSystemHost* g_host = nullptr;

constexpr int kScriptLuaTypeFunction = 6;   // cISCLua::Type(-1) for a function value
constexpr int kLuaUserData = 7;             // cISCLua::Type(-1) for a full userdata
constexpr int kLuaGlobalsIndex = -10001;    // Lua 4 pseudo-index used by RawSet
constexpr std::uint32_t kIIDLua = 0xea10c4a2;   // GameObjectRegistry listener / cISCLua interface id
constexpr std::size_t kAdvisorDirectorySuffixLength = 9;   // strlen("advisors/")

// sNotify<T> (the free template the original posts its notifications with): QueryInterface the object for
// cIGZUnknown, build a cRZMessage2Standard with type / unknown and send or post it on SL::spMS2. A failed
// QueryInterface posts nothing; a null object posts a message whose unknown is null.
void Notify(cIGZMessageServer2* messages, bool post, std::uint32_t type, cIGZUnknown* object) {
    cIGZUnknown* unknown = nullptr;
    if (object && !object->QueryInterface(kGZIID_cIGZUnknown, reinterpret_cast<void**>(&unknown))) return;
    cRZMessage2Standard* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetIGZUnknown(unknown);
    if (messages) {
        if (post) {
            messages->MessagePost(message, false);
        } else {
            messages->MessageSend(message);
        }
    }
    message->Release();
    if (unknown) unknown->Release();
}

// The message Post* helpers of the original (type + data1, posted to the message server).
void PostData1(cIGZMessageServer2* messages, std::uint32_t type, std::uint64_t data1) {
    cRZMessage2Standard* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(data1);
    if (messages) messages->MessagePost(message, false);
    message->Release();
}

void PostTick(cIGZMessageServer2* messages, std::uint32_t type) {
    cRZMessage2Standard* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    if (messages) messages->MessagePost(message, false);
    message->Release();
}

// Appends "advisors/" the way the original does (an inlined std::string::append on the cRZString's std::string).
void AppendAdvisorDirectory(cRZString& directory) {
    directory.AppendChars("advisors/", static_cast<std::uint32_t>(kAdvisorDirectorySuffixLength));
}

}  // namespace

IAdvisorSystemHost* AdvisorSystemHost() { return g_host; }
void SetAdvisorSystemHost(IAdvisorSystemHost* host) { g_host = host; }

cSC4AdvisorSystem* cSC4AdvisorSystem::instance_ = nullptr;

// The original constructor sets the five vtable groups, the "loaded" / "initialized" bytes, the random generator
// (seed 0xffffffff), the two maps / two vectors, the timer (milliseconds, started) and the process-wide instance.
cSC4AdvisorSystem::cSC4AdvisorSystem() {
    instance_ = this;
    timer_.Start();
}

cSC4AdvisorSystem::~cSC4AdvisorSystem() {
    instance_ = nullptr;
}

bool cSC4AdvisorSystem::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4AdvisorSystem) {
        *out = static_cast<cISC4AdvisorSystem*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cGZMessageTargetLegacy) {
        *out = static_cast<cIGZMessageTarget*>(this);
    } else if (iid == kGZIID_cIGZSerializable) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return cRZUnknown::QueryInterface(iid, out);
    }
    AddRef();
    return true;
}

std::uint32_t cSC4AdvisorSystem::AddRef() { return cRZUnknown::AddRef(); }

std::uint32_t cSC4AdvisorSystem::Release() { return cRZUnknown::Release(); }

cISCLua* cSC4AdvisorSystem::GetScriptingContext() { return lua_.get(); }

cRZRandom* cSC4AdvisorSystem::GetRandom() { return &random_; }

AdvisorMap* cSC4AdvisorSystem::GetAdvisors() { return &advisors_; }

cISC4Advisor* cSC4AdvisorSystem::GetAdvisor(int id) {
    auto it = advisors_.find(id);
    return it == advisors_.end() ? nullptr : it->second.get();
}

bool cSC4AdvisorSystem::AddAdvisor(cISC4Advisor* advisor) {
    if (advisor) {
        const int id = advisor->GetID();
        cRZAutoRefCount<cISC4Advisor>& slot = advisors_[id];
        if (slot.get() != advisor) slot = advisor;
        Notify(g_host ? g_host->Messages() : nullptr, true, kAdvisorNotifyAdvisorAdded, advisor);
    }
    return true;
}

bool cSC4AdvisorSystem::RemoveAdvisor(int id) {
    auto found = advisors_.find(id);
    if (found == advisors_.end()) return false;
    cISC4Advisor* advisor = found->second.get();
    Notify(g_host ? g_host->Messages() : nullptr, false, kAdvisorNotifyAdvisorRemoved, advisor);

    // The original first collects every (event, set-entry) pair whose advisor set contains this advisor, then erases
    // them and drops the event when its set became empty.
    std::vector<std::pair<AdvisorEventMap::iterator, std::set<cISC4Advisor*>::iterator>> hits;
    for (auto event = eventSubscribers_.begin(); event != eventSubscribers_.end(); ++event) {
        auto hit = event->second.find(advisor);
        if (hit != event->second.end()) hits.emplace_back(event, hit);
    }
    for (auto& hit : hits) {
        hit.first->second.erase(hit.second);
        if (hit.first->second.empty()) eventSubscribers_.erase(hit.first);
    }
    advisors_.erase(found);
    return true;
}

bool cSC4AdvisorSystem::PushEvent(int eventId, cISC4AdviceSubject* subject) {
    auto it = eventSubscribers_.find(eventId);
    if (it != eventSubscribers_.end()) {
        for (cISC4Advisor* advisor : it->second) {
            if (advisor) advisor->PushEvent(eventId, subject);
        }
    }
    return true;
}

bool cSC4AdvisorSystem::SubscribeToEvent(int eventId, cISC4Advisor* advisor) {
    eventSubscribers_[eventId].insert(advisor);
    return true;
}

bool cSC4AdvisorSystem::UnsubscribeFromEvent(int eventId, cISC4Advisor* advisor) {
    auto event = eventSubscribers_.find(eventId);
    if (event == eventSubscribers_.end()) return false;
    auto hit = event->second.find(advisor);
    if (hit == event->second.end()) return false;
    event->second.erase(hit);
    if (event->second.empty()) eventSubscribers_.erase(event);
    return true;
}

void cSC4AdvisorSystem::PushContextAdvice(cISC4Advice* advice) {
    contextAdvices_.emplace_back(advice);
}

cISC4Advice* cSC4AdvisorSystem::PopContextAdvice() {
    if (contextAdvices_.empty()) return nullptr;
    cISC4Advice* advice = contextAdvices_.back().get();
    contextAdvices_.back() = nullptr;   // the original releases the stored reference, then drops the slot
    contextAdvices_.pop_back();
    return advice;
}

cISC4Advice* cSC4AdvisorSystem::GetContextAdvice() {
    return contextAdvices_.empty() ? nullptr : contextAdvices_.back().get();
}

int cSC4AdvisorSystem::GetAdvicePopupPriorityThreshold() const {
    const int level = g_host ? g_host->CityDifficultyLevel() : 0;
    if (level == 1) return thresholds_[1];
    if (level == 2) return thresholds_[2];
    return thresholds_[0];
}

int cSC4AdvisorSystem::GetDataTag(int index) {
    return dataTags_[static_cast<std::size_t>(index)];
}

std::uint32_t cSC4AdvisorSystem::GetGZCLSID() const { return kGZCLSID_cSC4AdvisorSystem; }

bool cSC4AdvisorSystem::SetGlobalValue(const char* name, double value) {
    if (!lua_) return true;
    lua_->GetGlobal("game");
    lua_->PushString(name);
    lua_->PushNumber(value);
    lua_->SetTable(-3);
    lua_->Pop(1);
    return true;
}

bool cSC4AdvisorSystem::SetGlobalValue(const char* name, cIGZString* value) {
    if (!lua_) return true;
    lua_->GetGlobal("game");
    lua_->PushString(name);
    if (value) value->AddRef();
    lua_->PushUserData(value, GetDataTag(1));
    lua_->SetTable(-3);
    lua_->Pop(1);
    return true;
}

bool cSC4AdvisorSystem::SetLocalValue(const char* name, double value, cISC4AdviceSubject* subject) {
    if (!lua_) return true;
    lua_->GetGlobal("game");
    lua_->PushString(name);
    lua_->PushNumber(value);
    lua_->SetTable(-3);
    cRZString key;
    key.Sprintf("%s_subject", name);
    lua_->PushString(key.ToChar());
    if (subject) {
        subject->AddRef();
        lua_->PushUserData(subject, GetDataTag(0));
    } else {
        lua_->PushUserData(nullptr, 0);
    }
    lua_->SetTable(-3);
    lua_->Pop(1);
    return true;
}

// "<type:08x><id:08x><chunk>": an advice link either runs the chunk as Lua with no current advice (types 0 and 0x1d,
// after 8 / 16 hex characters) or hands the text after the 16 hex characters to the advice of that advisor / guid.
bool cSC4AdvisorSystem::DoLink(const cIGZString& link) {
    cRZString text;
    text.AssignFromChars(link.Data(), link.Strlen());
    text.Trim();
    int type = 0;
    int id = 0;
    if (std::sscanf(text.ToChar(), "%08x%08x", &type, &id) != 2) return false;
    if (type != 0 && type != 0x1d) {
        cISC4Advisor* advisor = GetAdvisor(type);
        if (advisor && id != 0) {
            cISC4Advice* advice = advisor->GetAdviceByGUID(id);
            if (advice) {
                text = text.Right(text.Strlen() - 0x10);
                return advice->DoLink(text);
            }
        }
    }
    if (!lua_) return false;
    PushContextAdvice(nullptr);
    const char* chunk = text.ToChar() + (type == 0x1d ? 0x10 : 8);
    lua_->DoBuffer(chunk, static_cast<int>(std::strlen(chunk)), chunk);
    PopContextAdvice();
    return true;
}

// Drops every advisor: sends the "all advisors killed" notification, then removes, shuts down and releases each
// advisor (the explicit reference keeps it alive until Shutdown ran), and finally clears the event subscriber map.
void cSC4AdvisorSystem::kill_advisors() {
    if (!g_host) return;
    Notify(g_host->Messages(), false, kAdvisorNotifyAdvisorsKilled, nullptr);
    while (!advisors_.empty()) {
        cISC4Advisor* advisor = advisors_.begin()->second.get();
        if (!advisor) {
            advisors_.erase(advisors_.begin());
            continue;
        }
        advisor->AddRef();
        const int id = advisor->GetID();
        RemoveAdvisor(id);
        advisor->Shutdown();
        advisor->Release();
    }
    eventSubscribers_.clear();
}

// Reads the "advisors" table rows 1..N, the shared API extensions (register_api) and the advice tables.
bool cSC4AdvisorSystem::load_advisors() {
    if (!lua_) return false;
    bool valid = false;
    {
        cISCLuaAdapter adapter(lua_.get());
        cTablePropertyReader reader(&adapter, kAdvisorTableNameText, 0);
        valid = reader.IsValid();
    }
    if (!valid) return false;
    lua_->GetGlobal(kAdvisorTableNameText);
    const int count = lua_->GetN(-1);
    lua_->Pop(1);
    for (int row = 1; row <= count; ++row) load_advisor(row);
    return true;
}

bool cSC4AdvisorSystem::update_advisors() {
    if (!lua_) return false;
    bool tableValid = false;
    {
        cISCLuaAdapter adapter(lua_.get());
        cTablePropertyReader reader(&adapter, kAdvisorTableNameText, 0);
        tableValid = reader.IsValid();
    }
    if (!tableValid) return false;
    lua_->GetGlobal(kAdvisorTableNameText);
    const int count = lua_->GetN(-1);
    lua_->Pop(1);
    for (int row = 1; row <= count; ++row) {
        int id = 0;
        bool rowValid = false;
        {
            cISCLuaAdapter adapter(lua_.get());
            cTablePropertyReader reader(&adapter, kAdvisorTableNameText, row);
            rowValid = reader.IsValid();
            if (rowValid) id = static_cast<int>(reader.GetNumber("id"));
        }
        if (!rowValid || id == 0) continue;
        cISC4Advisor* advisor = GetAdvisor(id);
        if (!advisor) {
            load_advisor(row);
        } else {
            advisor->Init(row);
        }
    }
    return true;
}

bool cSC4AdvisorSystem::load_advisor(int row) {
    if (!lua_) return false;
    std::uint32_t classId = 0;
    int id = 0;
    {
        cISCLuaAdapter adapter(lua_.get());
        cTablePropertyReader reader(&adapter, kAdvisorTableNameText, row);
        if (!reader.IsValid()) return false;
        classId = reader.GetNumber("class_id");
        id = static_cast<int>(reader.GetNumber("id"));
    }
    if (id == 0) return false;
    cISC4Advisor* advisor = nullptr;
    if (g_host) g_host->CreateAdvisor(classId, kGZIID_cISC4Advisor, &advisor);
    if (advisor) {
        cRZAutoRefCount<cISC4Advisor>& slot = advisors_[id];
        if (slot.get() != advisor) slot = advisor;
        if (!advisor->Init(row)) {
            RemoveAdvisor(id);
        } else {
            Notify(g_host->Messages(), true, kAdvisorNotifyAdvisorAdded, advisor);
        }
        advisor->Release();
    }
    return false;   // the original returns 0 whatever happened
}

// The API extension table: on Init (open) the flagged records go into the globals, on reload the others into the
// "game" table; each record registers its function as "_tf_", pushes its name and stores the function in that table.
bool cSC4AdvisorSystem::register_api(bool open) {
    if (!lua_ || !g_host) return false;
    const int base = lua_->GetTop();
    std::size_t count = 0;
    const IAdvisorSystemHost::LuaExtension* extensions = g_host->LuaExtensions(count);
    for (std::size_t i = 0; extensions && i < count; ++i) {
        const IAdvisorSystemHost::LuaExtension& extension = extensions[i];
        if (extension.global || open) {
            if (extension.global && open) lua_->GetGlobals();
        } else {
            lua_->GetGlobal("game");
        }
        if (lua_->GetTop() != base) {
            lua_->Register(extension.function, "_tf_");
            lua_->PushString(extension.name);
            lua_->GetGlobal("_tf_");
            lua_->SetTable(-3);
            lua_->Pop(1);
        }
    }
    lua_->SetTop(base);
    return true;
}

// Re-reads the advisor scripts from the data / plugin "advisors/" folders, runs the "_on_reload" hook and hands the
// game table extensions to the new script state.
bool cSC4AdvisorSystem::reload_data() {
    if (!g_host) return false;
    dataReloaded_ = false;
    if (lua_) {
        lua_->GetGlobal("_on_reload");
        // Kept quirk: a non-function "_on_reload" stays on the Lua stack (the original only pops it through the call).
        if (lua_->Type(-1) == kScriptLuaTypeFunction) lua_->CallProtected(0, 0);
    }
    cRZString dataDirectory;
    g_host->GetDataDirectory(dataDirectory, -1);
    AppendAdvisorDirectory(dataDirectory);
    g_host->UpdateResourcesFromDirectory(kAdvisorPackageId, dataDirectory.ToChar());
    cRZString pluginDirectory;
    g_host->GetPluginDirectory(pluginDirectory);
    AppendAdvisorDirectory(pluginDirectory);
    g_host->UpdateResourcesFromDirectory(kAdvisorPackageId, pluginDirectory.ToChar());
    g_host->LoadPackageScripts(lua_.get(), kAdvisorPackageId, 0);
    register_api(false);
    g_host->SetTutorialRegistryLua(lua_.get());
    return true;
}

// Three thresholds (easy / medium / hard), read from tuning_constants.ADVICE_POPUP_PRIORITIES_BY_SKILL_LEVEL keyed by
// "low" / "medium" / "high" ("low" is the easy slot). The original saves and restores the Lua stack with a
// cStackPosSaver; the port does the same with GetTop / SetTop on the scripting context.
void cSC4AdvisorSystem::init_advice_popup_priority_thresholds() {
    thresholds_[0] = thresholds_[1] = thresholds_[2] = kAdvisorDefaultPopupPriority;
    if (!lua_) return;
    const int savedTop = lua_->GetTop();
    lua_->GetGlobal("tuning_constants");
    if (lua_->IsTable(-1)) {
        lua_->PushString("ADVICE_POPUP_PRIORITIES_BY_SKILL_LEVEL");
        lua_->GetTable(-2);
        if (lua_->IsTable(-1)) {
            std::unordered_map<std::string, int> values;
            if (SCLuaHelp::ReadTableAsMap(lua_.get(), values, -1)) {
                auto low = values.find("low");
                if (low != values.end()) thresholds_[0] = low->second;
                auto medium = values.find("medium");
                if (medium != values.end()) thresholds_[1] = medium->second;
                auto high = values.find("high");
                if (high != values.end()) thresholds_[2] = high->second;
            }
        }
    }
    lua_->SetTop(savedTop);
}

// The advice evaluation itself lives in cSC4Advice / cSC4Advisor; the system only drives every advisor.
void cSC4AdvisorSystem::simulate_all_advisors(int period) {
    for (auto& entry : advisors_) {
        if (entry.second) entry.second->Simulate(period);
    }
}

void cSC4AdvisorSystem::tick_self() {
    PostTick(g_host ? g_host->Messages() : nullptr, kAdvisorMessageTick);
}

// The message pump: city started (0x26d31ec1 / 0x26d31ec4), the periodic tick (0xca5501f2) and the cheat codes
// (0x230e27ac). Everything else is ignored.
bool cSC4AdvisorSystem::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::int32_t type = static_cast<std::int32_t>(message->GetType());
    if (type > 0x26d31ec0) {
        if (type == static_cast<std::int32_t>(kAdvisorMessageCityStarted)) {
            cIGZUnknown* city = static_cast<cIGZUnknown*>(standard->GetVoid1());
            if (g_host && g_host->CityEstablished(city)) {
                tick_self();
                if (lastDateNumber_ < 1) simulate_all_advisors(1);
            }
            loadedFromSave_ = false;
            return true;
        }
        if (type != static_cast<std::int32_t>(kAdvisorMessageCityStarted2)) return true;
        PushEvent(static_cast<int>(kAdvisorCityStartedEvent), nullptr);
        tick_self();
        if (lastDateNumber_ > 0) return true;
        simulate_all_advisors(1);
        return true;
    }
    if (type == static_cast<std::int32_t>(kAdvisorMessageTick)) {
        if (g_host) {
            cIGZUnknown* city = g_host->City();
            if (city && !g_host->CityEstablished(city)) return true;
            cIGZDate* simDate = g_host->SimDate();
            if (!simDate) return true;
            cRZDate date;
            date.SetJulianDay(simDate->DayNumber());
            if (static_cast<int>(date.JulianDay()) > lastDateNumber_) {
                lastDateNumber_ = static_cast<int>(date.JulianDay());
                if (static_cast<int>(date.DayOfMonth()) == kAdvisorYearlyDayOfMonth) {
                    if (lua_) {
                        (void)lua_->GetGCCount();       // the original reads both only to force a GC step
                        (void)lua_->GetGCThreshold();
                        lua_->SetGCThreshold(0);
                    }
                    simulate_all_advisors(1);
                }
                SetGlobalValue("g_year", static_cast<double>(date.Year()));
                SetGlobalValue("g_month", static_cast<double>(date.Month()));
                SetGlobalValue("g_day", static_cast<double>(date.DayOfMonth()));
                SetGlobalValue("g_date", static_cast<double>(date.JulianDay()));
                cGZDate startDate;
                g_host->SimStartDate(startDate);
                SetGlobalValue("g_year_count",
                               static_cast<double>(static_cast<int>(date.Year()) - static_cast<int>(startDate.Year())));
            } else {
                if (!g_host->SimulatorAnyPaused() || timer_.GetElapsedTimeU32() < kAdvisorTickIntervalMs) {
                    tick_self();
                    return true;
                }
            }
        }
        if (dataReloaded_) {
            reload_data();
            update_advisors();
        }
        simulate_all_advisors(0);
        timer_.Restart();
        tick_self();
        return true;
    }
    if (type != static_cast<std::int32_t>(kAdvisorMessageCheat)) return true;
    const std::uint32_t cheatId = static_cast<std::uint32_t>(standard->GetData1());
    if (cheatId == kAdvisorCheatLlamaHead) {
        if (g_host) g_host->ToggleLlamaCheat();
        return true;
    }
    if (cheatId != kAdvisorCheatCheats) return true;
    cIGZString* text = static_cast<cIGZString*>(standard->GetVoid2());
    if (!text) return true;
    cRZCmdLine commandLine(text->ToChar());
    cRZString value;
    cIGZMessageServer2* messages = g_host ? g_host->Messages() : nullptr;
    if (commandLine.IsSwitchPresent(cRZString("guids"), value, true)) {
        std::uint64_t show = 0;
        if (value.CompareToChars("on", 2, true) == 0) {
            show = 1;
        } else if (value.CompareToChars("off", 3, true) == 0) {
            show = 0;
        } else {
            return true;
        }
        PostData1(messages, kAdvisorMessageTriggered, show);
    } else if (commandLine.IsSwitchPresent(cRZString("trigger"), value, true)) {
        PostData1(messages, kAdvisorMessageAdviceChanged, RZAtoUint32HexA(value.ToChar()));
    } else if (commandLine.IsSwitchPresent(cRZString("print"), value, true)) {
        PostData1(messages, kAdvisorMessageAdviceDump, RZAtoUint32HexA(value.ToChar()));
    } else if (commandLine.IsSwitchPresent(cRZString("refresh"))) {
        PostData1(messages, kAdvisorMessageAdviceReset, 0);
    } else if (commandLine.IsSwitchPresent(cRZString("reload"))) {
        reload_data();
        update_advisors();
    }
    return true;
}

bool cSC4AdvisorSystem::DoMessage(cGZMessage& /*message*/) { return true; }

// ---- serialization ---------------------------------------------------------------------------------------------
bool cSC4AdvisorSystem::Write(cIGZOStream& stream) const {
    stream.SetUint16(kAdvisorStreamVersion);
    stream.SetSint32(static_cast<std::int32_t>(advisors_.size()));
    for (const auto& entry : advisors_) {
        cISC4Advisor* advisor = entry.second.get();
        SC4WriteGZSerializable(stream, advisor);
        stream.SetSint32(advisor ? advisor->GetID() : 0);
    }
    return !stream.GetError();
}

bool cSC4AdvisorSystem::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kAdvisorStreamVersion) return false;
    std::int32_t count = 0;
    stream.GetSint32(count);
    for (std::int32_t i = 0; i < count; ++i) {
        cISC4Advisor* advisor = nullptr;
        SC4ReadGZSerializable(stream, kGZIID_cISC4Advisor, reinterpret_cast<void**>(&advisor), false);
        std::int32_t id = 0;
        stream.GetSint32(id);
        cRZAutoRefCount<cISC4Advisor>& slot = advisors_[id];
        if (slot.get() != advisor) slot = advisor;
        if (advisor) advisor->Release();
    }
    loadedFromSave_ = !stream.GetError();
    return loadedFromSave_;
}

// ---- advisor window --------------------------------------------------------------------------------------------
IAdvisorWinProc* cSC4AdvisorSystem::GetAdvisorWinProc() { return winProc_.get(); }

void cSC4AdvisorSystem::startAdvisorWindow() {
    if (winProc_) return;
    IAdvisorWinProc* proc = g_host ? g_host->CreateAdvisorWinProc() : nullptr;
    if (!proc) return;
    winProc_ = proc;   // the original adds the first reference before storing
    winProc_->Init();
}

bool cSC4AdvisorSystem::IsAdvisorWindowVisible() {
    return winProc_ && winProc_->IsVisible();
}

void cSC4AdvisorSystem::ShowAdvisorWindow() {
    if (winProc_) winProc_->ShowWindow();
}

void cSC4AdvisorSystem::HideAdvisorWindow() {
    if (winProc_) winProc_->HideWindow();
}

void cSC4AdvisorSystem::RemoveWinProcs() {
    if (!winProc_) return;
    winProc_->Shutdown();
    winProc_ = nullptr;
}

// ---- Init / Shutdown -------------------------------------------------------------------------------------------
bool cSC4AdvisorSystem::Init() {
    IAdvisorSystemHost* host = g_host;
    if (!initialized_ && host) {
        cISCLua* lua = host->Lua();
        if (lua_.get() != lua) lua_ = lua;
        if (lua_) {
            lua_->Open(kLuaOpenAllLibraries);
            register_api(true);
            int tag = lua_->NewTag();
            dataTags_.push_back(tag);
            lua_->PushCClosure(&cSC4AdvisorSystem::gc_tag_method, 0);
            lua_->SetTagMethod(tag, "gc");
            tag = lua_->NewTag();
            dataTags_.push_back(tag);
            lua_->PushCClosure(&cSC4AdvisorSystem::gc_tag_method, 0);
            lua_->SetTagMethod(tag, "gc");
        }
        cSCStringDetokenizer* detokenizer = host->Detokenizer();
        if (detokenizer_.get() != detokenizer) detokenizer_ = detokenizer;
        if (detokenizer_) {
            detokenizer_->AddUnknownTokenReplacementMethod(host->AdviceDetokenizerFunction(), this, true);
        }
        reload_data();
        if (!advisors_.empty() && !loadedFromSave_) kill_advisors();
        if (cGZCheatCodeManager* cheats = host->CheatCodeManager()) {
            cheats->AddNotification2(static_cast<cIGZMessageTarget2*>(this), 0);
            cheats->RegisterCheatCode(kAdvisorCheatLlamaHead, cRZString(kAdvisorCheatLlamaHeadText));
            if (host->DebugFunctionalityEnabled()) {
                cheats->RegisterCheatCode(kAdvisorCheatCheats, cRZString(kAdvisorCheatCheatsText));
            }
        }
        if (!loadedFromSave_) load_advisors();
        initialized_ = true;
        update_advisors();
        init_advice_popup_priority_thresholds();
        if (cIGZMessageServer2* messages = host->Messages()) {
            messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), kAdvisorMessageCityStarted);
            messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), kAdvisorMessageCityStarted2);
            messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), kAdvisorMessageTick);
        }
    }
    // The original ends Init by dropping any existing advisor window and creating a fresh one.
    winProc_ = nullptr;
    startAdvisorWindow();
    return true;
}

bool cSC4AdvisorSystem::Shutdown() {
    if (!initialized_) return true;
    IAdvisorSystemHost* host = g_host;
    if (detokenizer_) {
        // Kept quirk: the original unregisters with the Lua context, not with the `this` it registered with.
        detokenizer_->AddUnknownTokenReplacementMethod(host ? host->AdviceDetokenizerFunction() : nullptr,
                                                      lua_.get(), false);
        detokenizer_ = nullptr;
    }
    if (host) {
        if (cIGZMessageServer2* messages = host->Messages()) {
            messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kAdvisorMessageCityStarted);
            messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kAdvisorMessageCityStarted2);
            messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kAdvisorMessageTick);
        }
    }
    if (winProc_) {
        RemoveWinProcs();
        if (winProc_) winProc_ = nullptr;   // dead after RemoveWinProcs, kept for the original's structure
    }
    kill_advisors();
    if (host) {
        if (cGZCheatCodeManager* cheats = host->CheatCodeManager()) {
            cheats->RemoveNotification2(static_cast<cIGZMessageTarget2*>(this), 0);
            cheats->UnregisterCheatCode(kAdvisorCheatLlamaHead);
            if (host->DebugFunctionalityEnabled()) cheats->UnregisterCheatCode(kAdvisorCheatCheats);
        }
    }
    if (lua_) {
        lua_->PushString("game");
        lua_->PushNil();
        lua_->RawSet(kLuaGlobalsIndex);
        lua_->SetGCThreshold(0);
        lua_->Close();
        lua_ = nullptr;
    }
    if (host) host->SetTutorialRegistryLua(nullptr);
    initialized_ = false;
    return true;
}

// The Lua "gc" tag method of the two tags created in Init: release the object stored in the userdata. The thread
// object sits in the Lua state slot immediately before the lua_State (ILuaApi::get_thread_slot); it is queried for
// the cISCLua view (iid 0xea10c4a2) before the tags and the stack are inspected.
cISCLua* IAdvisorSystemHost::LuaFromState(lua_State* state) {
    cISCLua* lua = nullptr;
    cIGZUnknown* threadView =
        *reinterpret_cast<cIGZUnknown**>(reinterpret_cast<char*>(state) - sizeof(void*));
    if (threadView) threadView->QueryInterface(kIIDLua, reinterpret_cast<void**>(&lua));
    return lua;
}

int cSC4AdvisorSystem::gc_tag_method(lua_State* state) {
    cISCLua* lua = g_host ? g_host->LuaFromState(state) : nullptr;
    cSC4AdvisorSystem* self = instance_;
    if (self && lua && self->GetScriptingContext() == lua && lua->Type(-1) == kLuaUserData) {
        const int tag = lua->Tag(-1);
        if (tag == self->GetDataTag(0) || tag == self->GetDataTag(1)) {
            void* object = lua->ToUserTag(-1);
            if (object) static_cast<cIGZUnknown*>(object)->Release();
        }
    }
    if (lua) lua->Release();
    return 0;
}

}  // namespace sc4
