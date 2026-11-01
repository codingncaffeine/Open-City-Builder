#include "sc4/Simulator.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>

#include "gz/GZCOMDirector.h"
#include "gz/StreamOps.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
ISimulatorHost* g_host = nullptr;
constexpr std::uint32_t kFramesPerSpeed[3] = {30, 20, 15};            // DAT_100a1ac68
}  // namespace

ISimulatorHost* SimulatorHost() { return g_host; }
void SetSimulatorHost(ISimulatorHost* host) { g_host = host; }

// ---- cSC4LimitTimerCalibrator ---------------------------------------------------------------------------

cSC4LimitTimerCalibrator::cSC4LimitTimerCalibrator() {
    const double ratio = cRZTimer::GetMillisecondsToCPUCyclesRatio();
    cyclesToMilliseconds = static_cast<float>(ratio);
    const double secondsToCycles = cRZTimer::GetSecondsToTimerCyclesRatio();
    minimumCycles_ = static_cast<std::uint64_t>(1.0 / secondsToCycles);
    millisecondsPerSecond_ = static_cast<float>(1000.0 / (1.0 / secondsToCycles));
}

// Blends the measured ratio of timer cycles to machine time into the coefficient (10% new, 90% old), at most once per
// minimumCycles_ timer cycles.
void cSC4LimitTimerCalibrator::Tick() {
    std::uint64_t cycle = 0;
    cRZTimer::GetTimerCycle(&cycle);
    const std::uint64_t machine = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (minimumCycles_ < cycle - lastTimerCycle_) {
        if (static_cast<std::int64_t>(machine - lastMachTime_) > 0) {
            cyclesToMilliseconds = (static_cast<float>(static_cast<std::int64_t>(cycle - lastTimerCycle_)) /
                                    static_cast<float>(static_cast<std::int64_t>(machine - lastMachTime_))) *
                                       millisecondsPerSecond_ * 0.1f +
                                   cyclesToMilliseconds * 0.9f;
        }
        lastTimerCycle_ = cycle;
        lastMachTime_ = machine;
    }
}

// ---- construction ---------------------------------------------------------------------------------------

cSC4Simulator::cSC4Simulator() : cRZSystemService(kSID_cSC4Simulator, -2000000) {
    for (AgentSet& set : sets_) set.cursor = set.agents.end();
}

cSC4Simulator::~cSC4Simulator() = default;

std::uint32_t cSC4Simulator::Now() const {
    if (clock_) return clock_();
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool cSC4Simulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4Simulator) *out = static_cast<cISC4Simulator*>(this);
    else if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else return cRZSystemService::QueryInterface(iid, out);
    AddRef();
    return true;
}

// ---- start up / shut down -------------------------------------------------------------------------------

bool cSC4Simulator::Init() {
    if (started_) return true;
    started_ = true;
    hiddenPauseCount_ = 1;
    cRZSystemService::Init();
    ReadSimulatorTunableValues();
    ISimulatorHost* host = g_host;
    if (!messages_ && host) {
        messages_ = host->Messages();
        if (messages_) {
            messages_->AddRef();
            messages_->AddNotification(this, 0x26d31ec1);                 // city started
            messages_->AddNotification(this, kCommandGetSimulationDate);
        }
    }
    if (host) host->RegisterCommand(kCommandGetSimulationDate, "GetSimulationDate");
    for (AgentSet& set : sets_) set.cursor = set.agents.begin();
    if (speed_ == 3) SetSimSpeed(0);
    else DoSetSimSpeed(speed_);
    paceStart_ = tickStart_ = Now();
    if (host) {
        host->RegisterService(this);
        host->RegisterCheat(kCheatSetTargetFrameRate, "FPS");
        host->AddCheatNotification(this, kCheatSetTargetFrameRate);
        if (host->DebugFunctionalityEnabled()) {
            host->RegisterCheat(kCheatSimulatorDate, "SimDate");
            host->AddCheatNotification(this, kCheatSimulatorDate);
        }
    }
    return true;
}

bool cSC4Simulator::Shutdown() {
    if (!started_) return true;
    started_ = false;
    ISimulatorHost* host = g_host;
    if (host) {
        host->RemoveCheatNotification(this, kCheatSetTargetFrameRate);
        host->UnregisterCheat(kCheatSetTargetFrameRate);
        host->RemoveCheatNotification(this, kCheatSimulatorDate);
        host->UnregisterCheat(kCheatSimulatorDate);
    }
    date_ = cGZDate(1u, 1u, 2000u);
    speed_ = 3;
    RemoveAllAgents();
    if (messages_) {
        messages_->RemoveNotification(this, 0x26d31ec1);
        messages_->RemoveNotification(this, kCommandGetSimulationDate);
        messages_->Release();
        messages_ = nullptr;
    }
    if (host) {
        host->UnregisterCommand(kCommandGetSimulationDate);
        host->UnregisterService(this);
    }
    cRZSystemService::Shutdown();
    return true;
}

// The pace table (milliseconds per simulated day for the three speeds) and the animation dilation factors come from the
// simulator tuning exemplar; the defaults stay when it is missing.
void cSC4Simulator::ReadSimulatorTunableValues() {
    paceTable_[0] = 5000;
    paceTable_[1] = 1000;
    paceTable_[2] = 100;
    ISimulatorHost* host = g_host;
    if (!host) return;
    cISCPropertyHolder* exemplar = host->TuningExemplar();
    if (!exemplar) return;
    std::vector<std::uint32_t> pace;
    if (exemplar->HasProperty(0x89590e6d) && cSCPropertyHelp::GetPropertyValue(exemplar, 0x89590e6d, pace) && pace.size() == 3)
        for (int i = 0; i < 3; ++i) paceTable_[i] = pace[static_cast<std::size_t>(i)];
    dilation_.assign({0.0f, 1.0f, 1.5f, 2.0f, 1.0f});
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x89590e6e, dilation_);
}

void cSC4Simulator::DoSetSimSpeed(int speed) {
    if (static_cast<unsigned>(speed) >= 3) return;
    targetFrameRate_ = static_cast<int>(kFramesPerSpeed[speed]);
    speed_ = speed;
    msPerInterval_ = paceTable_[speed];
    paceStart_ = Now();
    PostMessageServerMessage(kMessageSimSpeedChanged, static_cast<std::uint32_t>(speed_), 0, 0);
}

bool cSC4Simulator::SetSimSpeed(int speed) {
    if (!emergencyResumeAllowed_) return false;
    if (speed_ != speed) DoSetSimSpeed(speed);
    return true;
}

// ---- pausing ----------------------------------------------------------------------------------------------

void cSC4Simulator::Pause() {
    if (pauseCount_++ == 0) PostMessageServerMessage(kMessagePauseChanged, 1, 0, 0);
}
void cSC4Simulator::HiddenPause() {
    if (hiddenPauseCount_++ == 0) PostMessageServerMessage(kMessageHiddenPauseChanged, 1, 0, 0);
}
void cSC4Simulator::EmergencyPause() {
    Pause();
    if (emergencyResumeAllowed_) emergencyResumeAllowed_ = false;
    if (emergencyPauseCount_++ == 0) PostMessageServerMessage(kMessageEmergencyPauseChanged, 1, 0, 0);
}
void cSC4Simulator::Resume() {
    if (!emergencyResumeAllowed_) return;
    if (--pauseCount_ == 0) PostMessageServerMessage(kMessagePauseChanged, 0, 0, 0);
}
void cSC4Simulator::HiddenResume() {
    if (--hiddenPauseCount_ == 0) PostMessageServerMessage(kMessageHiddenPauseChanged, 0, 0, 0);
}
void cSC4Simulator::EmergencyResume() {
    if (!emergencyResumeAllowed_) emergencyResumeAllowed_ = true;
    Resume();
    if (--emergencyPauseCount_ == 0) PostMessageServerMessage(kMessageEmergencyPauseChanged, 0, 0, 0);
}

float cSC4Simulator::GetAnimationTimeDilation() {
    std::size_t index;
    if (!IsEmergencyPaused() && !(g_host && g_host->IsDrivingMode())) {
        if (!IsPaused()) {
            const int speed = GetSimSpeed();
            index = speed == 2 ? 3 : (speed == 1 ? 2 : 1);
        } else {
            index = 0;
        }
    } else {
        index = 4;
    }
    return dilation_[index];
}

void cSC4Simulator::GetSimDate(int* year, int* month, int* dayOfMonth, int* dayOfYear, int* weekDay) const {
    if (year) *year = static_cast<int>(date_.Year());
    if (month) *month = static_cast<int>(date_.Month());
    if (dayOfMonth) *dayOfMonth = static_cast<int>(date_.DayOfMonth());
    if (dayOfYear) *dayOfYear = static_cast<int>(date_.DayOfYear());
    if (weekDay) *weekDay = static_cast<int>(date_.WeekDay());
}

// ---- messages -------------------------------------------------------------------------------------------

void cSC4Simulator::PostMessageServerMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2, std::uint32_t data3) {
    if (!messages_) return;
    auto* message = new cRZMessage2Standard;
    message->AddRef();
    message->SetType(type);
    message->SetData1(data1);
    message->SetData2(data2);
    message->SetData3(data3);
    messages_->MessagePost(message, false);
    message->Release();
}

bool cSC4Simulator::SendAgentMessage(const AgentInfo& agent, std::uint32_t type, std::uint32_t data1, std::uint32_t data2, std::uint32_t data3) {
    auto* message = new cRZMessage2Standard;
    message->AddRef();
    message->SetType(type);
    message->SetData1(data1);
    message->SetData2(data2);
    message->SetData3(data3);
    const bool result = agent.agent->DoMessage(message);
    message->Release();
    static const bool trace = std::getenv("SC4_AGENTDEBUG") != nullptr;
    if (trace && type != 0x6695681a) {
        static std::map<std::pair<std::string, std::uint32_t>, int> seen;
        const int n = ++seen[{agent.name, type}];
        if (n <= 2 || n % 30 == 0) std::fprintf(stderr, "agent: %s <- %08x #%d (done %d, day %u)\n", agent.name.c_str(), type, n, result ? 1 : 0, data1);
    }
    return result;
}

bool cSC4Simulator::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    if (type == kCommandGetSimulationDate) {
        auto* input = static_cast<cIGZCommandParameterSet*>(standard->GetVoid1());
        auto* output = static_cast<cIGZCommandParameterSet*>(standard->GetVoid2());
        for (std::uint32_t i = 0; i < 6; ++i) {
            std::uint32_t value;
            if (input->CompareParameterString(i, "year", false)) value = date_.Year();
            else if (input->CompareParameterString(i, "month", false)) value = date_.Month();
            else if (input->CompareParameterString(i, "day of month", false)) value = date_.DayOfMonth();
            else if (input->CompareParameterString(i, "day of week", false)) value = date_.WeekDay();
            else if (input->CompareParameterString(i, "day of year", false)) value = date_.DayOfYear();
            else if (input->CompareParameterString(i, "number", false)) value = date_.DayNumber();
            else return true;
            output->SetParameter(i, cRZVariant(value));
        }
    } else if (type == 0x230e27ac) {
        DoCheatCode(static_cast<std::uint32_t>(standard->GetData1()), static_cast<const cIGZString*>(standard->GetVoid2()));
    } else if (type == 0x26d31ec1) {
        if (messages_) messages_->RemoveNotification(this, 0x26d31ec1);
        HiddenResume();                                                   // vslot 0x68: releases the hidden pause Init took
    }
    return true;
}

void cSC4Simulator::DoCheatCode(std::uint32_t id, const cIGZString* text) {
    if (!text) return;
    std::istringstream words(text->Data());
    std::vector<std::string> args;
    for (std::string word; words >> word;) args.push_back(word);
    if (id == kCheatSetTargetFrameRate) {
        if (args.size() > 1) targetFrameRate_ = std::atoi(args[1].c_str());
    } else if (id == kCheatSimulatorDate) {
        if (args.size() > 2) {
            const std::uint32_t month = static_cast<std::uint32_t>(std::atoi(args[1].c_str()));
            const std::uint32_t day = static_cast<std::uint32_t>(std::atoi(args[2].c_str()));
            const std::uint32_t year = args.size() < 4 ? date_.Year() : static_cast<std::uint32_t>(std::atoi(args[3].c_str()));
            date_.Set(month, day, year);
            SimulateTimePassage();
        }
    }
}

// ---- time ---------------------------------------------------------------------------------------------

// Advances the calendar by one step when the pace timer says so and the agents of every boundary being crossed have
// finished their round. With agents waiting in the two per-tick sets the calendar stands still.
void cSC4Simulator::SimulateTimePassage() {
    for (bool& flag : pending_) flag = false;
    if (!sets_[0].agents.empty() || !sets_[1].agents.empty()) return;
    const std::uint32_t elapsed = Now() - paceStart_;
    const std::uint32_t interval = msPerInterval_;
    bool idle[kSimIntervalCount];
    for (int k = 2; k < kSimIntervalCount - 1; ++k) idle[k] = sets_[k].cursor == sets_[k].agents.end();
    cGZDate tomorrow(date_.DayNumber() + 1);
    const bool newWeek = tomorrow.WeekDay() == 1, newMonth = tomorrow.DayOfMonth() == 1, newYear = tomorrow.DayOfYear() == 1;

    std::uint32_t minutes = counters_[2];
    if (!idle[2] || (elapsed < interval && paceInterval_ == 2)) {
        // not due: leave the minute counter alone
    } else {
        if (paceInterval_ == 2) paceStart_ = Now();
        if (sets_[2].agents.empty()) {
            counters_[2] = 60;
        } else {
            pending_[2] = true;
            counters_[2] = ++minutes;
        }
    }
    if (counters_[2] > 59) {
        if (idle[3] && (elapsed >= interval || paceInterval_ != 3)) {
            counters_[2] = 0;
            if (paceInterval_ == 3) paceStart_ = Now();
            if (sets_[3].agents.empty()) {
                counters_[3] = 24;
            } else {
                pending_[3] = true;
                ++counters_[3];
            }
        }
    }
    if (counters_[3] > 23 && (elapsed >= interval || paceInterval_ != 4) && idle[4]) {
        const bool boundaries[3] = {newWeek, newMonth, newYear};
        for (int k = 0; k < 3; ++k)
            if (boundaries[k] && !idle[5 + k]) return;           // that boundary's agents are still working
        counters_[3] = 0;
        if (paceInterval_ == 4) paceStart_ = Now();
        pending_[4] = true;
        date_.Set(date_.DayNumber() + 1);
        counters_[4] = date_.DayOfYear();
        const std::uint32_t day = date_.DayNumber();
        PostMessageServerMessage(0x66956814, day, 0, counters_[4]);
        if (date_.WeekDay() == 1) {
            pending_[5] = true;
            counters_[5] = (date_.DayOfYear() - 1) / 7 + 1;
            PostMessageServerMessage(0x66956815, day, 0, counters_[5]);
        }
        if (date_.DayOfMonth() == 1) {
            pending_[6] = true;
            counters_[6] = date_.Month();
            PostMessageServerMessage(0x66956816, day, 0, counters_[6]);
        }
        if (date_.DayOfYear() == 1) {
            pending_[7] = true;
            counters_[7] = date_.Year();
            PostMessageServerMessage(0x66956817, day, 0, counters_[7]);
        }
    }
}

// ---- agents -----------------------------------------------------------------------------------------------

// A new round of `interval`: every enabled agent of the set is told once, in order, until the budget runs out.
void cSC4Simulator::BeginNewInterval(int interval) {
    AgentSet& set = sets_[interval];
    if (set.agents.empty()) return;
    const std::uint32_t day = date_.DayNumber();
    // agents that were skipped last round get their message now, so nothing is lost across rounds
    for (auto it = set.cursor; it != set.agents.end();) {
        auto current = it++;
        set.cursor = it;
        if (!current->second.enabled) continue;
        while (!SendAgentMessage(current->second, kMessageSimInterval + static_cast<std::uint32_t>(interval), day, 0, counters_[interval])) {
        }
    }
    set.cursor = set.agents.begin();
}

void cSC4Simulator::SimulateAgents() {
    for (int k = 2; k <= 7; ++k)
        if (pending_[k]) BeginNewInterval(k);
    const std::uint32_t day = date_.DayNumber();
    int i = 0;
    for (;;) {
        AgentSet& set = sets_[i];
        bool process = false;
        if (set.cursor == set.agents.end()) {
            if (i < 2) {
                if (!set.agents.empty()) {
                    i = 0;
                    BeginNewInterval(0);
                    process = true;
                }
            } else if (i == 9) {
                return;
            }
            if (!process) {
                ++i;
                if (!sets_[i].agents.empty() && sets_[i].cursor != sets_[i].agents.end()) process = true;
            }
        } else {
            process = true;
        }
        if (process) {
            AgentSet& active = sets_[i];
            if (active.cursor != active.agents.end()) {
                auto node = active.cursor;
                bool advance = true;
                if (node->second.enabled)
                    advance = SendAgentMessage(node->second, kMessageSimInterval + static_cast<std::uint32_t>(i), day, 0, counters_[i]);
                if (advance) ++active.cursor;
                if (i == 0) {                          // the immediate set is a queue: the visited agent is dropped
                    if (active.cursor == node) ++active.cursor;
                    active.agents.erase(node);
                    i = 0;
                }
            }
        }
        if (Now() - budgetStart_ >= budgetMs_) return;
    }
}

// Idle-time agents share whatever is left of the tick budget (at most a third of it each).
void cSC4Simulator::SimulateIdleTime() {
    AgentSet& set = sets_[kIntervalIdle];
    if (hiddenPauseCount_ >= 1 || set.agents.empty()) return;
    if (set.cursor == set.agents.end()) set.cursor = set.agents.begin();
    const std::uint32_t day = date_.DayNumber();
    std::uint32_t elapsed = Now() - budgetStart_;
    while (elapsed < budgetMs_) {
        auto node = set.cursor;
        ++set.cursor;
        if (node->second.enabled) {
            const std::uint32_t slice = std::min(budgetMs_ / 3, budgetMs_ - elapsed);
            SendAgentMessage(node->second, 0x6695681a, day, pauseCount_ > 0 ? 1u : 0u, slice);
        }
        if (set.cursor == set.agents.end()) break;
        elapsed = Now() - budgetStart_;
    }
}

bool cSC4Simulator::OnTick(std::uint32_t) {
    calibrator_.Tick();
    const std::uint32_t sinceLast = Now() - tickStart_;
    tickStart_ = Now();
    int rate = 30;
    if (pauseCount_ < 1 && hiddenPauseCount_ < 1) rate = targetFrameRate_;
    const std::int32_t budget = static_cast<std::int32_t>(1000 / rate) - static_cast<std::int32_t>(sinceLast);
    budgetMs_ = budget > 15 ? static_cast<std::uint32_t>(budget) : 15u;
    if (started_ && !inTick_) {
        inTick_ = true;
        budgetStart_ = Now();
        do {
            if (hiddenPauseCount_ > 0) break;
            if (pauseCount_ < 1) {
                SimulateTimePassage();
                SimulateAgents();
                if (hiddenPauseCount_ >= 1) continue;
            }
            SimulateIdleTime();
        } while (Now() - budgetStart_ < budgetMs_);
        inTick_ = false;
    }
    tickStart_ = Now();
    return true;
}

bool cSC4Simulator::AddAgent(cIGZMessageTarget2* agent, int interval, const cIGZString& name, std::uint32_t flags) {
    if (interval >= kSimIntervalCount || interval < 0) return true;
    if (agent) agent->AddRef();
    AgentInfo info;
    info.agent = agent;
    info.name.assign(name.Data(), name.Strlen());
    info.flags = flags;
    info.enabled = ((2u - (cityEstablished_ ? 0u : 1u)) & flags) != 0;
    AgentSet& set = sets_[interval];
    const bool wasAtEnd = set.cursor == set.agents.end();
    const auto result = set.agents.emplace(agent, info);
    if (result.second && wasAtEnd) set.cursor = set.agents.end();
    return true;
}

bool cSC4Simulator::RemoveAgent(cIGZMessageTarget2* agent, int interval) {
    if (interval >= kSimIntervalCount || interval < 0) return false;
    AgentSet& set = sets_[interval];
    auto it = set.agents.find(agent);
    if (it == set.agents.end()) return false;
    if (set.cursor == it) set.cursor = std::next(it);
    set.agents.erase(it);
    return true;
}

bool cSC4Simulator::RemoveAgent(cIGZMessageTarget2* agent) {
    bool any = false;
    for (int i = 0; i < kSimIntervalCount; ++i) any = RemoveAgent(agent, i) || any;
    return any;
}

bool cSC4Simulator::RemoveAllAgents(int interval) {
    if (interval >= kSimIntervalCount || interval < 0) return true;
    sets_[interval].agents.clear();
    sets_[interval].cursor = sets_[interval].agents.end();
    return true;
}

bool cSC4Simulator::RemoveAllAgents() {
    for (int i = 0; i < kSimIntervalCount; ++i) RemoveAllAgents(i);
    return true;
}

void cSC4Simulator::EnumerateAgentsByName(std::vector<std::string>& names) const {
    names.clear();
    for (const AgentSet& set : sets_)
        for (const auto& entry : set.agents)
            if (std::find(names.begin(), names.end(), entry.second.name) == names.end()) names.push_back(entry.second.name);
}

bool cSC4Simulator::GetAgentEnabled(const cIGZString& name) const {
    for (const AgentSet& set : sets_)
        for (const auto& entry : set.agents)
            if (entry.second.enabled && strcasecmp(entry.second.name.c_str(), name.Data()) == 0) return true;
    return false;
}

void cSC4Simulator::SetAgentEnabled(const cIGZString& name, bool enabled) {
    for (AgentSet& set : sets_)
        for (auto& entry : set.agents)
            if (strcasecmp(entry.second.name.c_str(), name.Data()) == 0) entry.second.enabled = enabled;
}

void cSC4Simulator::SetCityEstablished(bool established) {
    if (cityEstablished_ == established) return;
    cityEstablished_ = established;
    for (AgentSet& set : sets_)
        for (auto& entry : set.agents) entry.second.enabled = (entry.second.flags & (2u - (cityEstablished_ ? 0u : 1u))) != 0;
}

// ---- persistence ------------------------------------------------------------------------------------------

bool cSC4Simulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(4);
    stream.SetUint8(inTick_);
    stream.SetUint8(cityEstablished_);
    stream.SetSint32(pauseCount_);
    stream.SetSint32(speed_);
    stream.SetSint32(simTime_);
    stream.SetUint32(budgetMs_);
    for (int i = 0; i < kSimIntervalCount; ++i) {
        stream.SetUint32(counters_[i]);
        stream << pending_[i];
    }
    stream << cRZDate::FromJulianDay(date_.DayNumber());
    return !stream.GetError();
}

bool cSC4Simulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 5) return false;
    std::uint8_t flag = 0;
    stream.GetUint8(flag);
    inTick_ = flag != 0;
    stream.GetUint8(flag);
    cityEstablished_ = flag != 0;
    stream.GetSint32(pauseCount_);
    stream.GetSint32(speed_);
    stream.GetSint32(simTime_);
    stream.GetUint32(budgetMs_);
    for (int i = 0; i < kSimIntervalCount; ++i) {
        stream.GetUint32(counters_[i]);
        stream >> pending_[i];
    }
    if (version < 4) {                          // old saves carry a timer here
        cRZTimer timer(cRZTimer::kMilliseconds);
        stream >> timer;
    }
    cRZDate date;
    stream >> date;
    date_.Set(date.JulianDay());
    return !stream.GetError();
}

}  // namespace sc4
