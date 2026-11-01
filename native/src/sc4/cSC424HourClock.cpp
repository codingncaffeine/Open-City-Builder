#include "sc4/cSC424HourClock.h"
#include "gz/StreamOps.h"
#include "rz/cRZTimer.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace sc4 {

namespace {
// Splits a command line like cRZCmdLine: whitespace-separated tokens, double quotes group.
std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (any) { out.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur.push_back(c);
        any = true;
    }
    if (any) out.push_back(cur);
    return out;
}
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

cSC424HourClock::cSC424HourClock(IClockHost* host) : host_(host) {
    // The constructor leaves visual time equal to the real time of day.
    visualSeconds_ = timeSeconds_;
}

bool cSC424HourClock::Init() {
    if (initialized_) return true;
    if (pauseCount_ == 0 && hiddenPauseCount_ == 0) timer_.Start();
    ReadSimulatorTunableValues();
    initialized_ = true;
    return true;
}

bool cSC424HourClock::Shutdown() {
    if (!initialized_) return true;
    timer_.Reset();
    timeSeconds_ = 0;   // (sic) the original zeroes the time on shutdown
    initialized_ = false;
    return true;
}

void cSC424HourClock::ReadSimulatorTunableValues() {
    if (!host_) return;
    IClockHost::Tunables t{magnification_, nightStart_, nightEnd_};
    if (host_->ReadTunables(t)) {
        SetTimeMagnification(t.timeMagnification);
        nightStart_ = t.nightStartHour;
        nightEnd_ = t.nightEndHour;
    }
}

void cSC424HourClock::SetTimePercentage(float fraction) {
    auto seconds = static_cast<std::int64_t>(fraction * 86400.0f);
    auto value = static_cast<std::int32_t>(seconds);
    // wraps by whole days using a 16-bit day count, exactly like the original
    std::uint32_t days = static_cast<std::uint16_t>((static_cast<std::uint64_t>(seconds) & 0xffffffffu) / 86400u);
    timeSeconds_ = static_cast<std::uint32_t>(value) - days * 86400u;
}

void cSC424HourClock::SetHour(std::uint32_t hour) {
    std::uint32_t current = GetHour();
    std::uint32_t h = hour < 24 ? hour : hour % 24;
    if (h == current) return;
    timeSeconds_ = h * 3600;
    Post(kMsgHourChanged, h);
    Post(kMsgMinuteChanged, h);
}

void cSC424HourClock::SetMinute(std::uint32_t minute) {
    std::uint32_t m = minute < 60 ? minute : minute % 60;
    timeSeconds_ = m * 60 + GetHour() * 3600;
}

void cSC424HourClock::SetSeconds(std::uint32_t seconds) {
    std::uint32_t s = seconds < 60 ? seconds : seconds % 60;
    timeSeconds_ = GetMinute() * 60 + GetHour() * 3600 + s;
}

bool cSC424HourClock::SetTimeMagnification(float value) {
    magnification_ = value;
    if (value < magMin_) { magnification_ = magMin_; value = magMin_; }
    if (magMax_ < value) magnification_ = magMax_;
    return true;
}

void cSC424HourClock::Pause(bool pause) {
    if (pause) {
        ++pauseCount_;
        if (pauseCount_ != 1) return;
        if (hiddenPauseCount_ == 0) timer_.Stop();
        Post(kMsgPauseChanged, 1);
    } else {
        if (pauseCount_ < 1) return;
        --pauseCount_;
        if (pauseCount_ != 0) return;
        if (hiddenPauseCount_ == 0) timer_.Start();
        Post(kMsgPauseChanged, 0);
    }
}

void cSC424HourClock::HiddenPause(bool pause) {
    if (pause) {
        ++hiddenPauseCount_;
        if (hiddenPauseCount_ != 1) return;
        if (pauseCount_ == 0) timer_.Stop();
        Post(kMsgHiddenPauseChanged, 1);
    } else {
        if (hiddenPauseCount_ < 1) return;
        --hiddenPauseCount_;
        if (hiddenPauseCount_ != 0) return;
        if (pauseCount_ == 0) timer_.Start();
        Post(kMsgHiddenPauseChanged, 0);
    }
}

bool cSC424HourClock::IsNight() const {
    std::uint32_t hour = GetVisualHour();
    return (hour + 24 - nightStart_) % 24 < (24 - nightStart_ + nightEnd_) % 24;
}

void cSC424HourClock::EnableDayNightTransitionNotifications(bool enable) {
    if (dayNightNotifications_ == enable) return;
    dayNightNotifications_ = enable;
    if (enable) {
        std::uint32_t hour = GetVisualHour();
        // posts the current state: 1 when the visual hour is already past the night length
        Post(kMsgDayNightChanged,
             (24 - nightStart_ + nightEnd_) % 24 <= (hour + 24 - nightStart_) % 24 ? 1u : 0u);
    }
}

void cSC424HourClock::SetVisualTimeMode(VisualTimeMode mode) {
    if (visualMode_ == mode) return;
    visualMode_ = mode;
    if (mode == kVisualMidnight) ChangeVisualTime(0);
    else if (mode == kVisualNoon) ChangeVisualTime(0xa8c0);
    else if (mode == kVisualReal) ChangeVisualTime(timeSeconds_);
}

void cSC424HourClock::ChangeVisualTime(std::uint32_t seconds) {
    if (visualSeconds_ == seconds) return;
    std::uint32_t oldHour = GetVisualHour();
    std::uint32_t oldMinute = GetVisualMinute();
    visualSeconds_ = seconds;
    std::uint32_t newHour = GetVisualHour();
    if (GetVisualHour() == oldHour) {
        std::uint32_t minute = GetVisualMinute();
        if (oldMinute / 5 != minute / 5) Post(kMsgVisualMinuteChanged, newHour, minute);
        return;
    }
    Post(kMsgVisualHourChanged, newHour);
    Post(kMsgVisualMinuteChanged, newHour);
    if (dayNightNotifications_) {
        std::uint32_t oldH = oldHour;
        std::uint32_t before = (oldH + 24 - nightStart_) % 24;
        std::uint32_t span = (24 - nightStart_ + nightEnd_) % 24;
        std::uint32_t after = (newHour + 24 - nightStart_) % 24;
        if ((before < span) != (after < span)) Post(kMsgDayNightChanged, span <= after ? 1u : 0u, 0);
    }
}

bool cSC424HourClock::OnIdle(std::uint32_t) {
    if (pauseCount_ > 0 || hiddenPauseCount_ > 0) return true;
    std::uint32_t gameSeconds = static_cast<std::uint32_t>(static_cast<float>(Elapsed()) * 1e-6f * magnification_);
    if (gameSeconds < 0x3d) return true;
    std::uint32_t step = gameSeconds < 0x4b0 ? gameSeconds : 0x4b0;
    std::uint32_t oldHour = GetHour();
    std::uint32_t oldMinute = GetMinute();
    std::uint32_t t = step + timeSeconds_;
    timeSeconds_ = t < 0x15180 ? t : t % 0x15180;
    std::uint32_t hour = GetHour();
    std::uint32_t minute;
    if (oldHour == hour) {
        minute = GetMinute();
        if (oldMinute / 5 == minute / 5) goto visual;
    } else {
        Post(kMsgHourChanged, hour);
        minute = 0;
        oldHour = hour;
    }
    Post(kMsgMinuteChanged, oldHour, minute);
visual:
    if (visualMode_ == kVisualReal) ChangeVisualTime(timeSeconds_);
    RestartTimer();
    return true;
}

bool cSC424HourClock::DoCheat(std::uint32_t cheat, const std::string& commandLine) {
    std::vector<std::string> argv = Tokenize(commandLine);
    if (argv.size() < 2) return true;
    if (cheat == kClockCheatSetTime) {
        SetHour(static_cast<std::uint32_t>(std::atoi(argv[1].c_str())));
    } else if (cheat == kClockCheatControl) {
        std::string verb = Lower(argv[1]);
        if (verb == "pause") Pause(true);
        else if (verb == "resume") Pause(false);
        else if (verb == "magnify" && argv.size() > 2)
            SetTimeMagnification(static_cast<float>(std::atoi(argv[2].c_str())));
    }
    return true;
}

bool cSC424HourClock::Write(cIGZOStream& s) const {
    s.SetUint16(2);
    s.SetSint32(pauseCount_);
    s.SetFloat32(magnification_);
    s.SetFloat32(magMax_);
    s.SetUint32(timeSeconds_);
    s.SetUint32(nightStart_);
    s.SetUint32(nightEnd_);
    return !s.GetError();
}

bool cSC424HourClock::Read(cIGZIStream& s) {
    std::uint16_t version = 0;
    s.GetUint16(version);
    s.GetSint32(pauseCount_);
    s.GetFloat32(magnification_);
    s.GetFloat32(magMax_);
    s.GetUint32(timeSeconds_);
    if (version < 2) {   // version 1 (the PC saves) also streamed a timer, dropped on read
        cRZTimer timer(static_cast<cRZTimer::Units>(4));
        s >> timer;
    }
    s.GetUint32(nightStart_);
    s.GetUint32(nightEnd_);
    return !s.GetError();
}

}  // namespace sc4
