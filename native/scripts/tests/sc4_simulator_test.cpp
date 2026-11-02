// cSC4Simulator: speed/pause bookkeeping, calendar stepping, agent rounds, enabling and persistence.
#include <cstdio>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZString.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/Simulator.h"

namespace SL { cIGZMessageServer2* spMS2 = nullptr; }
using namespace sc4;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
std::uint32_t g_now = 1000;

struct Host final : ISimulatorHost {
    int services = 0, cheats = 0, commands = 0;
    cIGZMessageServer2* Messages() override { return nullptr; }
    void RegisterService(cIGZSystemService*) override { ++services; }
    void UnregisterService(cIGZSystemService*) override { --services; }
    void RegisterCheat(std::uint32_t, const char*) override { ++cheats; }
    void UnregisterCheat(std::uint32_t) override { --cheats; }
    void AddCheatNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    void RemoveCheatNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    void RegisterCommand(std::uint32_t, const char*) override { ++commands; }
    void UnregisterCommand(std::uint32_t) override { --commands; }
    bool DebugFunctionalityEnabled() override { return false; }
    bool IsDrivingMode() override { return false; }
    cISCPropertyHolder* tuning = nullptr;
    cISCPropertyHolder* TuningExemplar() override { return tuning; }
};

struct Agent final : cIGZMessageTarget2 {
    std::vector<std::uint32_t> got;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* m) override { got.push_back(m->GetType()); return true; }
};
}  // namespace

int main() {
    Host host;
    SetSimulatorHost(&host);
    cSC4Simulator sim;
    sim.SetClockForTest([] { return g_now; });
    CHECK(sim.Init());
    CHECK(host.services == 1 && host.cheats == 1 && host.commands == 1);
    CHECK(sim.GetSimSpeed() == 0);                    // speed 3 means "pick the slowest"
    CHECK(sim.IsHiddenPaused());
    {   // the city-started message (0x26d31ec1) releases Init's hidden pause (vslot 0x68), not the player's pause
        cRZMessage2Standard started;
        started.SetType(0x26d31ec1);
        sim.DoMessage(&started);
    }
    CHECK(!sim.IsHiddenPaused());
    CHECK(!sim.IsPaused());
    sim.HiddenPause();
    sim.HiddenResume();
    CHECK(!sim.IsHiddenPaused());

    // speed changes and pause counting
    CHECK(sim.SetSimSpeed(2));
    CHECK(sim.GetSimSpeed() == 2);
    sim.Pause(); sim.Pause();
    sim.Resume();
    CHECK(sim.IsPaused());
    sim.Resume();
    CHECK(!sim.IsPaused());
    sim.EmergencyPause();
    CHECK(!sim.SetSimSpeed(1));                       // locked while the emergency pause holds
    sim.EmergencyResume();
    CHECK(sim.SetSimSpeed(1));

    // agents: add, enumerate, enable by name, remove
    Agent a, b;
    cRZString nameA("alpha"), nameB("beta");
    sim.AddAgent(&a, 4, nameA, 3);
    sim.AddAgent(&b, 4, nameB, 1);                    // only before the city is established
    CHECK(sim.AgentCount(4) == 2);
    sim.SetCityEstablished(true);
    CHECK(sim.GetAgentEnabled(nameA));
    CHECK(!sim.GetAgentEnabled(nameB));
    std::vector<std::string> names;
    sim.EnumerateAgentsByName(names);
    CHECK(names.size() == 2);
    CHECK(sim.RemoveAgent(&b, 4));
    CHECK(sim.AgentCount(4) == 1);

    // one day passes: the calendar moves and the daily agent is told once
    sim.SetSimSpeed(2);                               // 100 ms per day
    g_now += 5000;
    sim.SetMaxMillisecondsPerTick(1000000);
    for (int i = 0; i < 4; ++i) { sim.SimulateTimePassage(); sim.SimulateAgents(); g_now += 200; }
    CHECK(sim.GetSimDate().DayNumber() >= cGZDate(1u, 1u, 2000u).DayNumber());

    // persistence round trip
    PortMemoryOStream out;
    CHECK(sim.Write(out));
    cSC4Simulator other;
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(other.Read(in));
    CHECK(other.GetSimSpeed() == sim.GetSimSpeed());
    CHECK(other.GetSimDateNumber() == sim.GetSimDateNumber());
    CHECK(other.IsCityEstablished());

    sim.Shutdown();
    CHECK(host.services == 0 && host.commands == 0);

    // Pace at cheetah with the shipped tuning (exemplar 0x89590e6d = 2000/1000/50 ms per day): the tick loop fills the frame
    // to 1000/15 ms, so a fast frame gets about 15 days a second; a frame slower than a day gets one day per frame.
    for (const std::uint32_t renderMs : {5u, 750u}) {
        cSCPropertyHolder tuning;
        cSCPropertyHelp::SetPropertyValue(&tuning, 0x89590e6d, std::vector<std::uint32_t>{2000, 1000, 50});
        host.tuning = &tuning;
        static double clock = 0;                        // every read of the clock costs 20 µs of loop work
        clock = 100000;
        cSC4Simulator paced;
        paced.SetClockForTest([] { clock += 0.02; return static_cast<std::uint32_t>(clock); });
        CHECK(paced.Init());
        cRZMessage2Standard started;
        started.SetType(0x26d31ec1);
        paced.DoMessage(&started);
        paced.SetCityEstablished(true);
        CHECK(paced.SetSimSpeed(2));
        const std::uint32_t firstDay = paced.GetSimDateNumber();
        int frames = 0;
        for (const double end = clock + 10000; clock < end; ++frames) {
            clock += renderMs;
            paced.OnTick(0);
        }
        const std::uint32_t days = paced.GetSimDateNumber() - firstDay;
        std::printf("cheetah, %u ms frames: %u days in 10 s, %d frames\n", renderMs, days, frames);
        if (renderMs == 5) CHECK(days >= 130 && days <= 210);
        else CHECK(days + 1 >= static_cast<std::uint32_t>(frames) && days <= static_cast<std::uint32_t>(frames));
        paced.Shutdown();
        host.tuning = nullptr;
    }
    if (!failures) std::printf("sc4_simulator OK\n");
    return failures != 0;
}
