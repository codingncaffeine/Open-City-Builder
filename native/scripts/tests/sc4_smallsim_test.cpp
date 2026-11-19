#include <cstdio>
#include <map>
#include <vector>

#include "fake_winmgr.h"
#include "gz/Win.h"
#include "sc4/UIFilters.h"
#include "sc4/WealthRequester.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

struct Host final : IWealthHost {
    bool lot = true;
    int lotWealth = 1, cityWealth = 3;
    unsigned zone = 5;
    std::map<std::uint32_t, int> props;
    bool HasLot(int, int) override { return lot; }
    int LotWealth(int, int) override { return lotWealth; }
    int CityCellWealth(int, int) override { return cityWealth; }
    unsigned ZoneType(int, int) override { return zone; }
    int PropWealth(std::uint32_t id) override { return props[id]; }
};

struct Probe final : cGZWin {
    explicit Probe(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    int seen = 0;
    bool DoMessage(cGZMessage&) override { ++seen; return true; }
};

int main() {
    Host host;
    host.props = {{10, 1}, {11, 2}, {12, 2}, {13, 3}};
    cRZRandom random(7);
    auto* requester = new cSC4WealthRequester(&host, &random);
    requester->AddRef();
    CHECK(requester->GetGZCLSID() == 0x89c49986 && requester->GetRequestedShare(nullptr) == 10);
    CHECK(requester->GetRequesterSatisfactionId() == 0x49a1e995 && requester->UsesCustomPropSelector());
    const std::vector<std::uint32_t> candidates{10, 11, 12, 13};
    CHECK(requester->SelectProp(candidates, 0, 0) == 10);                   // lot wealth 1
    host.zone = 11;                                                         // zones 11/12/15 force wealth 2
    for (int i = 0; i < 20; ++i) { auto p = requester->SelectProp(candidates, 0, 0); CHECK(p == 11 || p == 12); }
    host.lot = false;                                                       // city wealth 3
    CHECK(requester->SelectProp(candidates, 0, 0) == 13);
    host.cityWealth = 9;
    CHECK(requester->SelectProp(candidates, 0, 0) == 0);
    void* out = nullptr;
    CHECK(requester->QueryInterface(0x89c498be, &out) && requester->QueryInterface(0xe4fda3d4, &out) && !requester->QueryInterface(2, &out));
    requester->Release(); requester->Release();   // the two successful queries
    requester->Release();   // refcount 1 -> frees

    auto* filter = new cSC4LotConfigurationFilter();
    filter->AddRef();
    CHECK(filter->IsLotConfigurationIncluded(nullptr));
    CHECK(filter->QueryInterface(0x68f0113a, &out) && !filter->QueryInterface(3, &out));
    filter->Release();
    filter->Release();

    FakeMgr mgr;
    auto* window = new Probe(&mgr);
    window->AddRef();
    auto* ctab = new cSC4CtabFilter(window);
    ctab->AddRef();
    cGZMessage message;
    message.type = kGZMsgCharacter;
    message.param1 = '\t';
    CHECK(ctab->DoMessage(nullptr, message) && window->seen == 1);
    message.param1 = '\r';
    CHECK(ctab->DoMessage(nullptr, message) && window->seen == 2);
    message.param1 = 'a';
    CHECK(!ctab->DoMessage(nullptr, message) && window->seen == 2);
    message.type = kGZMsgKeyDown;
    message.param1 = '\t';
    CHECK(!ctab->DoMessage(nullptr, message) && window->seen == 2);
    CHECK(ctab->QueryInterface(0x22e85d8e, &out) && !ctab->QueryInterface(3, &out));
    ctab->Release();
    ctab->Release();
    window->Release();
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}
