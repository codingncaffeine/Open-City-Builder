// cSC4HistoryWarehouse: recording, length limits, range statistics, coalescing and the record format.
#include <cmath>
#include <cstdio>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZDate.h"
#include "sc4/HistoryWarehouse.h"

namespace SL { cIGZMessageServer2* spMS2 = nullptr; }
using namespace sc4;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Env final : IHistoryEnvironment {
    std::uint32_t day = 1000; int year = 2020, month = 6, agents = 0;
    std::uint32_t SimDayNumber() override { return day; }
    void SimYearMonth(int& y, int& m) override { y = year; m = month; }
    void AddAgent(cIGZMessageTarget2*, int, const char*, std::uint32_t) override { ++agents; }
    void RemoveAgent(cIGZMessageTarget2*) override { agents = 0; }
};
}  // namespace

int main() {
    Env env;
    SetHistoryEnvironment(&env);
    auto* w = new cSC4HistoryWarehouse;
    w->AddRef();
    CHECK(w->Init() && env.agents == 2);

    // recording: one point per day, the same day overwrites
    for (int d = 0; d < 10; ++d) { env.day = 1000 + d; w->Store(7, d * 10); }
    env.day = 1009;
    w->Store(7, 95);
    const auto& s = w->AllSeries().at(7);
    CHECK(s.nodes.size() == 10 && s.nodes.back().value == 95);

    float count = 0, slope = 0, intercept = 0, lo = 0, hi = 0, avg = 0, value = 0;
    CHECK(w->GetCountDays(7, 1002, 1005, count) && count == 4);
    CHECK(!w->GetCountDays(8, 1002, 1005, count));
    CHECK(w->GetTrendDays(7, 1000, 1008, slope, intercept) && std::fabs(slope - 10.0f) < 1e-4f && std::fabs(intercept) < 1e-3f);
    CHECK(w->GetPointDay(7, 1004, value) && std::fabs(value - 40.0f) < 1e-3f);
    CHECK(w->GetMinMaxAverageDays(7, 1002, 1004, lo, hi, avg) && lo == 20 && hi == 40 && std::fabs(avg - 30) < 1e-4f);
    // a day between points is interpolated from its neighbours (the point before/after join the fit)
    CHECK(w->GetPointDay(7, 1004, value));

    // a length limit keeps the newest points
    w->SetHistoryDataQueueLength(7, 4);
    CHECK(w->AllSeries().at(7).nodes.size() == 4 && w->AllSeries().at(7).nodes.front().julianDay == 1006);
    env.day = 1010;
    w->Store(7, 1);
    CHECK(w->AllSeries().at(7).nodes.size() == 4 && w->AllSeries().at(7).nodes.front().julianDay == 1007);

    // the record round trip (version 2)
    PortMemoryOStream out;
    CHECK(w->Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    auto* copy = new cSC4HistoryWarehouse;
    copy->AddRef();
    CHECK(copy->Read(in));
    CHECK(copy->AllSeries().size() == 1 && copy->AllSeries().at(7).nodes.size() == 4 && copy->AllSeries().at(7).maxLength == 4);
    PortMemoryIStream again(out.bytes().data(), out.bytes().size());
    CHECK(!copy->Read(again));          // the series already exists

    // monthly coalescing keeps the first point of each month, dated to the month's start
    auto* c = new cSC4HistoryWarehouse;
    c->AddRef();
    const std::uint32_t jan = cRZDate(1, 1, 2000).JulianDay();
    for (int d = 0; d < 70; ++d) { env.day = jan + d; c->Store(3, d); }
    const std::uint32_t endDay = cRZDate(6, 1, 2000).JulianDay();
    cVecRange range(const_cast<HistoryList&>(c->AllSeries().at(3).nodes), jan, endDay, false);
    CHECK(range.Coalesce(eCoalesce::Monthly));
    const auto& nodes = c->AllSeries().at(3).nodes;
    CHECK(nodes.size() < 70 && nodes.front().julianDay == jan && nodes.front().value == 0);

    // disposing a series empties it
    CHECK(c->Dispose(3) && c->AllSeries().at(3).nodes.empty());
    CHECK(w->Shutdown() && env.agents == 0);
    w->Release(); copy->Release(); c->Release();
    if (failures == 0) std::puts("sc4_historywarehouse: all passed");
    return failures ? 1 : 0;
}
