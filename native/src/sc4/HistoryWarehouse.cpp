#include <cstdio>
#include <cstdlib>
#include <iterator>
#include "sc4/HistoryWarehouse.h"

#include <chrono>
#include <cmath>
#include <cfloat>
#include <map>

#include "rz/cRZDate.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {

namespace {
IHistoryEnvironment* g_environment = nullptr;
constexpr std::uint32_t kUnsetDay = 0xcdcdcdcd;      // uninitialised-memory fill seen in old saves
}  // namespace

IHistoryEnvironment* HistoryEnvironment() { return g_environment; }
void SetHistoryEnvironment(IHistoryEnvironment* environment) { g_environment = environment; }

// ---- cVecRange ------------------------------------------------------------------------------------------

cVecRange::cVecRange(HistoryList& l, std::uint32_t fromDay, std::uint32_t toDay, bool includeNeighbours)
    : begin(l.end()), end(l.end()), startDay(fromDay), list(&l) {
    const HistoryIter sentinel = l.end();
    begin = end = HistoryIter();             // the original leaves both null until a branch sets them
    if (includeNeighbours && !l.empty()) {
        if (l.back().julianDay < fromDay) {                  // everything lies before the range: the last point only
            end = sentinel;
            begin = std::prev(sentinel);
            return;
        }
        if (toDay < l.front().julianDay) {                   // everything lies after it: the first point only
            begin = l.begin();
            end = std::next(begin);
            return;
        }
    }
    HistoryIter node = l.begin();
    bool foundStart = false, foundEnd = false;
    if (node != sentinel) {
        for (;;) {
            if (!foundStart) {
                if (node->julianDay <= toDay) {
                    if (fromDay <= node->julianDay) {
                        begin = node;
                        foundStart = true;
                    }
                } else if (includeNeighbours) {              // jumped over the whole range: the points around the gap
                    begin = node;
                    end = node;
                    if (l.begin() != node) begin = std::prev(node);
                    end = std::next(node);
                    return;
                } else {
                    break;
                }
            }
            if (!foundEnd && foundStart && toDay < node->julianDay) {
                end = node;
                HistoryIter afterBegin = begin;
                if (sentinel != begin) afterBegin = std::next(begin);
                foundEnd = true;
                if (node == afterBegin && includeNeighbours) {
                    if (l.begin() != begin) begin = std::prev(begin);
                    if (sentinel != node) end = std::next(node);
                }
            }
            if (foundEnd && foundStart) break;
            node = std::next(node);
            if (node == sentinel) break;
        }
    }
    if (!foundStart) begin = sentinel;
    if (!foundEnd) end = sentinel;
}

// One point survives per period (the first one, dated to the period's start); the rest are erased. The last period of
// the range is left open.
bool cVecRange::Coalesce(eCoalesce mode) {
    const HistoryIter sentinel = list->end();
    HistoryIter group = end;                // "no open group" while it equals the range end
    std::int32_t previousKey = -1000;
    std::uint32_t periodStart = 0;
    HistoryIter node = begin;
    if (end == begin) return true;
    const std::uint32_t m = static_cast<std::uint32_t>(mode);
    do {
        std::int32_t key = -1000;
        if (m < 4) {
            cRZDate date = cRZDate::FromJulianDay(node->julianDay);
            switch (mode) {
                case eCoalesce::Monthly: key = static_cast<std::int32_t>(date.Month()); break;
                case eCoalesce::HalfYearly: key = static_cast<std::int32_t>((date.Month() - 1) / 6 + date.Year() * 2); break;
                case eCoalesce::Yearly: key = static_cast<std::int32_t>(date.Year()); break;
                case eCoalesce::FiveYearly: key = static_cast<std::int32_t>(date.Year() / 5); break;
            }
            if (group == end) {                              // open a group at this point
                std::uint32_t month = 1;
                if (mode == eCoalesce::Monthly) month = static_cast<std::uint32_t>(key);
                else if (mode == eCoalesce::HalfYearly) month = (key & 1) ? 7 : 1;
                periodStart = cRZDate(month, 1, date.Year()).JulianDay();
                group = node;
                previousKey = key;
                node = std::next(node);
                continue;
            }
        }
        if (key != previousKey) {                            // the period changed: close the group
            group->julianDay = periodStart;
            HistoryIter first = group;
            if (sentinel != first) first = std::next(first);
            if (node != first) list->erase(first, node);
            group = end;
            if (list->begin() != node) node = std::prev(node);
        }
        node = std::next(node);
    } while (end != node);
    return true;
}

// ---- cStatVecRange --------------------------------------------------------------------------------------

cStatVecRange::cStatVecRange(HistoryList& l, std::uint32_t fromDay, std::uint32_t toDay) : cVecRange(l, fromDay, toDay, true) {
    if (begin != l.end()) {
        ComputeMeans();
        ComputeStandardDeviations();
        ComputePearsonsCorrelation();
    }
}

void cStatVecRange::ComputeMeans() {
    count = 0;
    sumX = sumY = 0;
    meanX = meanY = 0;
    if (end != begin) {
        for (HistoryIter n = begin; n != end; ++n) {
            ++count;
            sumY += static_cast<double>(n->value);
            sumX += static_cast<double>(static_cast<std::int32_t>(n->julianDay - startDay));
        }
        if (count != 0) {
            meanX = sumX / static_cast<double>(count);
            meanY = sumY / static_cast<double>(count);
        }
    }
}

void cStatVecRange::ComputeStandardDeviations() {
    double sx = 0, sy = 0;
    for (HistoryIter n = begin; n != end; ++n) {
        const double dx = static_cast<double>(static_cast<std::int32_t>(n->julianDay - startDay)) - meanX;
        sx += dx * dx;
        const double dy = static_cast<double>(n->value) - meanY;
        sy += dy * dy;
    }
    stdX = stdY = 0;
    if (count != 0) {
        stdX = std::sqrt(sx / static_cast<double>(count));
        stdY = std::sqrt(sy / static_cast<double>(count));
    }
}

void cStatVecRange::ComputePearsonsCorrelation() {
    pearson = 0;
    if ((stdX != 0.0 || std::isnan(stdX)) && (stdY != 0.0 || std::isnan(stdY)) && count != 0) {
        double sum = 0;
        for (HistoryIter n = begin; n != end; ++n)
            sum += ((static_cast<double>(n->value) - meanY) / stdY) *
                   ((static_cast<double>(static_cast<std::int32_t>(n->julianDay - startDay)) - meanX) / stdX);
        pearson = sum / static_cast<double>(count);
    }
}

// ---- the warehouse --------------------------------------------------------------------------------------

bool cSC4HistoryWarehouse::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4HistoryWarehouse) *out = static_cast<cISC4HistoryWarehouse*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4HistoryWarehouse::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4HistoryWarehouse::Init() {
    IHistoryEnvironment* env = HistoryEnvironment();
    if (SL::spMS2) {
        SL::spMS2->AddNotification(this, kMessageHistoryStore);
        SL::spMS2->AddNotification(this, kMessageHistorySetSize);
    }
    if (env) {
        env->AddAgent(this, 4, "cSC4HistoryWarehouse", 3);
        env->AddAgent(this, 7, "cSC4HistoryWarehouse", 3);
    }
    state_ = 0;
    started_ = true;
    return true;
}

bool cSC4HistoryWarehouse::Shutdown() {
    if (SL::spMS2) {
        SL::spMS2->RemoveNotification(this, kMessageHistoryStore);
        SL::spMS2->RemoveNotification(this, kMessageHistorySetSize);
    }
    if (IHistoryEnvironment* env = HistoryEnvironment()) env->RemoveAgent(this);
    started_ = false;
    return true;
}

const cSC4HistoryWarehouse::Series* cSC4HistoryWarehouse::Find(std::uint32_t series) const {
    auto it = series_.find(series);
    return it == series_.end() ? nullptr : &it->second;
}
cSC4HistoryWarehouse::Series* cSC4HistoryWarehouse::Find(std::uint32_t series) {
    auto it = series_.find(series);
    return it == series_.end() ? nullptr : &it->second;
}

bool cSC4HistoryWarehouse::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    switch (message->GetType()) {
        case kMessageHistoryStore:
            Store(static_cast<std::uint32_t>(standard->GetData1()), static_cast<int>(standard->GetData2()));
            break;
        case kMessageHistorySetSize:
            SetSize(static_cast<std::uint32_t>(standard->GetData1()), static_cast<int>(standard->GetData2()));
            break;
        case kMessageDailyTick:
            return DoDailySimulation();
        case kMessageCityStarted:
            if (state_ == 0) state_ = 1;
            break;
        default:
            break;
    }
    return true;
}

bool cSC4HistoryWarehouse::Store(std::uint32_t series, int value) {
    if (std::getenv("SC4_HISTORYDEBUG")) {
        static std::map<std::uint32_t, int> seen;
        const int n = ++seen[series];
        if (n <= 2 || n % 100 == 0) std::fprintf(stderr, "history: store %08x = %d (#%d)\n", series, value, n);
    }
    Series& s = GetKeyVector(series);
    IHistoryEnvironment* env = HistoryEnvironment();
    const std::uint32_t today = env ? env->SimDayNumber() : 0;
    const std::uint32_t last = s.nodes.empty() ? 0 : s.nodes.back().julianDay;
    // An empty series always takes a first point (on day 0, before the environment has a date, `last < today` is false and
    // back() of the empty list would write into the list itself).
    if (s.nodes.empty() || last < today) {
        if (s.maxLength != 0 && s.maxLength <= s.nodes.size()) s.nodes.pop_front();
        s.nodes.push_back(HistoryNode{value, today});
    } else {
        s.nodes.back().value = value;
    }
    return true;
}

bool cSC4HistoryWarehouse::SetSize(std::uint32_t series, int length) {
    Series& s = GetKeyVector(series);
    s.maxLength = static_cast<std::uint32_t>(length);
    if (length > 0 && static_cast<int>(s.nodes.size()) > length) {
        auto last = s.nodes.begin();
        std::advance(last, static_cast<int>(s.nodes.size()) - length);
        s.nodes.erase(s.nodes.begin(), last);
    }
    return true;
}

void cSC4HistoryWarehouse::RecordHistoryData(std::uint32_t series, int value) {
    auto* message = new cRZMessage2Standard;
    message->AddRef();
    message->SetType(kMessageHistoryStore);
    message->SetData1(series);
    message->SetData2(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
    if (SL::spMS2) SL::spMS2->MessageSend(message);
    message->Release();
}

// Time-sliced coalescing: at most about 5 ms per call, at most (series / 365 + 1) series per day.
bool cSC4HistoryWarehouse::DoDailySimulation() {
    bool result = true;
    if (state_ != 2) {
        if (state_ != 1) return true;
        cursor_ = series_.begin();
        coalescedThisDay_ = 0;
        state_ = 2;
    }
    const auto start = std::chrono::steady_clock::now();
    int* reset = &state_;
    if (static_cast<int>(series_.size()) > 0) {
        const int limit = static_cast<int>(series_.size()) / 365;
        reset = &coalescedThisDay_;
        int count = coalescedThisDay_;
        bool done = false;
        do {
            if (limit < coalescedThisDay_) { done = true; break; }
            if (cursor_ == series_.end()) { state_ = 0; done = true; break; }
            if (cursor_->second.maxLength == 0) {
                VariableCoalesce(cursor_->first);
                ++coalescedThisDay_;
            }
            ++cursor_;
            count = coalescedThisDay_;
        } while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 5);
        (void)done;
        if (coalescedThisDay_ <= limit) return false;
    }
    *reset = 0;
    return result;
}

void cSC4HistoryWarehouse::VariableCoalesce(std::uint32_t series) {
    IHistoryEnvironment* env = HistoryEnvironment();
    int year = 0, month = 0;
    if (env) env->SimYearMonth(year, month);
    const std::uint32_t lastMonth = month > 1 ? static_cast<std::uint32_t>(month - 1) : 12;
    const std::uint32_t day0 = cRZDate(lastMonth, 1, static_cast<std::uint32_t>(year - (month < 2 ? 1 : 0))).JulianDay();
    const std::uint32_t day1 = cRZDate(1, 1, static_cast<std::uint32_t>(year - 10)).JulianDay();
    const std::uint32_t day2 = cRZDate(1, 1, static_cast<std::uint32_t>(year - 50)).JulianDay();
    const std::uint32_t day3 = cRZDate(1, 1, static_cast<std::uint32_t>(year - 100)).JulianDay();
    const std::uint32_t day4 = cRZDate(1, 1, static_cast<std::uint32_t>(year - 500)).JulianDay();
    Series* s = Find(series);
    if (!s) return;
    if (!s->nodes.empty()) {
        cVecRange dispose(s->nodes, 0, day4, false);
        if (dispose.begin != s->nodes.end()) {
            HistoryIter last = dispose.begin;
            if (dispose.begin != dispose.end) { last = dispose.end; if (dispose.end != s->nodes.begin()) last = std::prev(dispose.end); }
            if (dispose.begin != last) s->nodes.erase(dispose.begin, last);
        }
    }
    const struct { std::uint32_t from, to; eCoalesce mode; } steps[4] = {
        {day4, day3, eCoalesce::FiveYearly}, {day3, day2, eCoalesce::Yearly}, {day2, day1, eCoalesce::HalfYearly}, {day1, day0, eCoalesce::Monthly}};
    for (const auto& step : steps) {
        cVecRange range(s->nodes, step.from, step.to, false);
        if (range.begin != s->nodes.end()) range.Coalesce(step.mode);
    }
}

// ---- queries --------------------------------------------------------------------------------------------

bool cSC4HistoryWarehouse::GetCountDays(std::uint32_t series, std::uint32_t from, std::uint32_t to, float& count) {
    Series* s = Find(series);
    if (!s) return false;
    cStatVecRange range(s->nodes, from, to);
    count = static_cast<float>(range.count);
    return true;
}

bool cSC4HistoryWarehouse::GetTrendDays(std::uint32_t series, std::uint32_t from, std::uint32_t to, float& slope, float& intercept) {
    Series* s = Find(series);
    if (!s) return false;
    cStatVecRange range(s->nodes, from, to);
    if (range.begin == range.end) return false;
    if (range.stdX != 0.0 || std::isnan(range.stdX)) {
        slope = static_cast<float>((range.pearson * range.stdY) / range.stdX);
        intercept = static_cast<float>((range.sumY - static_cast<double>(slope) * range.sumX) / static_cast<double>(range.count));
    } else {
        slope = 0.0f;
        intercept = static_cast<float>(range.begin->value);
    }
    return true;
}

bool cSC4HistoryWarehouse::GetPointDay(std::uint32_t series, std::uint32_t day, float& value) {
    Series* s = Find(series);
    if (!s) return false;
    cStatVecRange range(s->nodes, day, day);
    if (range.begin == range.end) return false;
    if (range.stdX != 0.0 || std::isnan(range.stdX)) {
        const float slope = static_cast<float>((range.pearson * range.stdY) / range.stdX);
        value = static_cast<float>((range.sumY - static_cast<double>(slope) * range.sumX) / static_cast<double>(range.count));
    } else {
        value = static_cast<float>(range.begin->value);
    }
    return true;
}

bool cSC4HistoryWarehouse::GetMinMaxAverageDays(std::uint32_t series, std::uint32_t from, std::uint32_t to, float& minimum, float& maximum, float& average) {
    Series* s = Find(series);
    if (!s || s->nodes.empty()) return false;
    cVecRange range(s->nodes, from, to, false);
    minimum = FLT_MAX;
    maximum = -FLT_MAX;
    float sum = 0.0f, count = 0.0f;
    bool coversFrom = false, coversTo = false;
    if (range.begin != s->nodes.end() && range.end != range.begin) {
        for (HistoryIter n = range.begin; n != range.end; ++n) {
            if (n->julianDay == from) coversFrom = true;
            if (n->julianDay == to) coversTo = true;
            const float v = static_cast<float>(n->value);
            if (v < minimum) minimum = v;
            if (maximum <= v && v != maximum) maximum = v;
            sum += v;
            count += 1.0f;
        }
    }
    float v = 0;
    if (!coversFrom && GetPointDay(series, from, v)) {
        if (v < minimum) minimum = v;
        if (maximum <= v && v != maximum) maximum = v;
        sum += v;
        count += 1.0f;
    }
    if (!coversTo && GetPointDay(series, to, v)) {
        if (v < minimum) minimum = v;
        if (maximum <= v && v != maximum) maximum = v;
        sum += v;
        count += 1.0f;
    }
    average = sum / count;
    return true;
}

bool cSC4HistoryWarehouse::GetCount(std::uint32_t series, const cIGZDate& from, const cIGZDate& to, float& count) {
    return GetCountDays(series, from.DayNumber(), to.DayNumber(), count);
}
bool cSC4HistoryWarehouse::GetTrend(std::uint32_t series, const cIGZDate& from, const cIGZDate& to, float& slope, float& intercept) {
    return GetTrendDays(series, from.DayNumber(), to.DayNumber(), slope, intercept);
}
bool cSC4HistoryWarehouse::GetMinMaxAverage(std::uint32_t series, const cIGZDate& from, const cIGZDate& to, float& minimum, float& maximum, float& average) {
    return GetMinMaxAverageDays(series, from.DayNumber(), to.DayNumber(), minimum, maximum, average);
}
bool cSC4HistoryWarehouse::GetPoint(std::uint32_t series, const cIGZDate& date, float& value) {
    return GetPointDay(series, date.DayNumber(), value);
}

bool cSC4HistoryWarehouse::Coalesce(std::uint32_t series, const cIGZDate& from, const cIGZDate& to, int mode) {
    Series* s = Find(series);
    if (!s) return false;
    cVecRange range(s->nodes, from.DayNumber(), to.DayNumber(), false);
    if (range.begin == s->nodes.end()) return false;
    range.Coalesce(static_cast<eCoalesce>(mode));
    return true;
}

// Erases everything in the range except its last point.
bool cSC4HistoryWarehouse::Dispose(std::uint32_t series, const cIGZDate& from, const cIGZDate& to) {
    Series* s = Find(series);
    if (!s) return false;
    cVecRange range(s->nodes, from.DayNumber(), to.DayNumber(), false);
    if (range.begin == s->nodes.end()) return false;
    HistoryIter last = range.begin;
    if (range.begin != range.end) {
        last = range.end;
        if (range.end != s->nodes.begin()) last = std::prev(range.end);
    }
    if (range.begin == last) return true;
    s->nodes.erase(range.begin, last);
    return true;
}

bool cSC4HistoryWarehouse::Dispose(std::uint32_t series) {
    Series* s = Find(series);
    if (!s) return false;
    s->nodes.clear();
    return true;
}

// ---- persistence ----------------------------------------------------------------------------------------

bool cSC4HistoryWarehouse::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    stream.SetUint32(static_cast<std::uint32_t>(series_.size()));
    for (const auto& [id, s] : series_) {
        stream.SetUint32(id);
        stream.SetUint32(s.maxLength);
        stream.SetUint32(static_cast<std::uint32_t>(s.nodes.size()));
        std::uint32_t running = 0;
        for (const HistoryNode& node : s.nodes) {
            ++running;
            if (node.julianDay != kUnsetDay) running = node.julianDay;
            stream.SetUint32(running);
            stream.SetSint32(node.value);
        }
    }
    return !stream.GetError();
}

bool cSC4HistoryWarehouse::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 3) return false;
    std::uint32_t seriesCount = 0;
    stream.GetUint32(seriesCount);
    for (std::uint32_t i = 0; i < seriesCount; ++i) {
        Series s;
        std::uint32_t id = 0, length = 0, points = 0;
        stream.GetUint32(id);
        stream.GetUint32(length);
        if (version == 1 && length == 100) length = 0;
        s.maxLength = length;
        stream.GetUint32(points);
        std::uint32_t previous = 0;
        for (std::uint32_t p = 0; p < points; ++p) {
            std::uint32_t day = 0;
            std::int32_t value = 0;
            stream.GetUint32(day);
            stream.GetSint32(value);
            if (day == kUnsetDay) day = previous + 1;
            if (previous < day) {
                s.nodes.push_back(HistoryNode{value, day});
                previous = day;
            } else if (!s.nodes.empty()) {
                s.nodes.back().value = value;
            }
        }
        if (series_.count(id)) return false;
        series_.emplace(id, std::move(s));
    }
    return !stream.GetError();
}

}  // namespace sc4
