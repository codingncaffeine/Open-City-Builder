// cSC4App city life cycle: new city, close with / without saving, the quit requests and the application messages.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "sc4/App.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Event { std::string how; std::uint32_t type; };
std::vector<Event> g_events;

struct Server final : cIGZMessageServer2 {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2* m) override { g_events.push_back({"send", m->GetType()}); return true; }
    bool MessagePost(cIGZMessage2* m, bool) override { g_events.push_back({"post", m->GetType()}); return true; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2* m, cIGZMessageTarget2* t) override { g_events.push_back({"target", m->GetType()}); target = t; return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
    cIGZMessageTarget2* target = nullptr;
};

struct City final : cISC4City {
    int refs = 1, inits = 0, shutdowns = 0;
    bool saveDisabled = false;
    std::string path;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { --refs; if (refs == 0) { destroyed = true; } return refs; }
    bool Init() override { ++inits; return true; }
    bool Shutdown() override { ++shutdowns; return true; }
    void SetCitySerialNumber(std::uint32_t) override {}
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromCString(path.c_str()); }
    void SetCitySaveFilePath(const cIGZString& p) override { path = p.ToChar(); }
    void SetCityNameChanged(bool) override {}
    bool IsSaveDisabled() const override { return saveDisabled; }
    static inline bool destroyed = false;
};

struct Regional final : cISC4RegionalCity {
    int setups = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetupCity(cISC4City*) override { ++setups; }
    std::uint32_t GetCitySerialNumber() const override { return 7; }
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromCString("/r/c.sc4"); }
};
}  // namespace

int main() {
    Server server;
    SL::spMS2 = &server;
    auto* app = new cSC4App("SimCity 4");
    app->AddRef();
    City* city = nullptr;
    Regional regional;
    cRZAutoRefCount<cISC4RegionalCity> holder(&regional);
    std::vector<std::string> screens;
    int quits = 0;
    bool confirmAnswer = false;
    int modalAnswer = 0;
    sc4::AppCityHooks& hooks = app->CityHooks();
    std::vector<City*> madeCities;   // City::Release never deletes, so the test frees them
    hooks.createCity = [&]() -> cISC4City* { city = new City; madeCities.push_back(city); return city; };
    hooks.destroyCurrentScreen = [&] { screens.push_back("destroy"); };
    hooks.createCityScreen = [&] { screens.push_back("city"); };
    hooks.createRegionScreen = [&] { screens.push_back("region"); };
    hooks.confirm = [&](const std::string&, const std::string&) { return confirmAnswer; };
    hooks.scriptedModal = [&](std::uint32_t, std::uint32_t) { return modalAnswer; };
    hooks.quitFramework = [&] { ++quits; };
    hooks.loadText = [](std::uint32_t, std::uint32_t) { return std::string("text"); };

    // a new city: screens first, then the city comes up with its notification sequence
    CHECK(app->RequestNewCity(&holder));
    CHECK(city && app->GetCity() == city && city->inits == 1 && regional.setups == 1);
    CHECK(screens.size() == 2 && screens[0] == "destroy" && screens[1] == "city");
    CHECK(g_events.size() == 5 && g_events[0].how == "send" && g_events[0].type == 0x26d31ec0 && g_events[1].how == "post" &&
          g_events[1].type == 0x2b96b3ea && g_events[4].type == 0xea8ae29a);

    // a second request while a city is open asks first: "no" closes without saving, then the new city opens
    g_events.clear();
    City* first = city;
    confirmAnswer = false;
    CHECK(app->RequestNewCity(&holder));
    CHECK(first->shutdowns == 1 && city != first && city->inits == 1);
    CHECK(g_events[0].type == 0x26d31ec2 && g_events[1].type == 0x26d31ec3);

    // answering yes tries to save; a city without a save path cannot, so nothing closes
    confirmAnswer = true;
    City* second = city;
    CHECK(!app->RequestCloseCity(true));
    CHECK(app->GetCity() == second && second->shutdowns == 0);
    second->saveDisabled = true;
    CHECK(!app->RequestSaveCity(true, false));

    // quit requests post the quit message to the application, which quits once
    g_events.clear();
    modalAnswer = static_cast<int>(0x8a921f5b);          // "no, do not save" differs from the cancel answer
    modalAnswer = 0;                                      // cancel: nothing happens
    second->saveDisabled = false;
    CHECK(!app->RequestQuit(true, false));
    CHECK(g_events.empty());
    CHECK(app->RequestQuit(false, false));
    CHECK(!g_events.empty() && g_events.back().how == "target" && g_events.back().type == 0x29a89b05);
    cRZMessage2Standard quit;
    quit.SetType(0x29a89b05);
    CHECK(app->DoMessage(&quit));
    CHECK(quits == 1 && app->GetCity() == nullptr);
    CHECK(!app->Quit());

    // application state messages
    cRZMessage2Standard state;
    state.SetType(0xea8ae29a); app->DoMessage(&state); CHECK(app->GetAppState() == 2);
    state.SetType(0xcbb5bb45); app->DoMessage(&state); CHECK(app->GetAppState() == 1);
    state.SetType(0x26d31ec2); app->DoMessage(&state); CHECK(app->GetAppState() == 0);

    // post-framework set-up: the three command systems are created once and initialised
    struct Support final : cIGZUnknown {
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return ++refs; }
        std::uint32_t Release() override { return --refs; }
        int refs = 0;
    };
    Support supports[3];
    int created = 0;
    std::vector<std::string> initialised;
    app->StartupHooks().createCommandSupport = [&](const char*) -> cIGZUnknown* { return &supports[created++]; };
    app->StartupHooks().initializeManager = [&](const char* name, cIGZUnknown*) { initialised.push_back(name); };
    CHECK(app->PostFrameWorkInit() && created == 3 && initialised.size() == 3 && initialised[0] == "cGZWinCommandSystemSupport");
    CHECK(app->PostFrameWorkInit() && created == 3);
    {
        std::ofstream("/tmp/appcity_script.lua") << "x";
        cRZString existing("/tmp/appcity_script.lua"), missing("/tmp/does_not_exist.lua");
        CHECK(app->FindTestScript(existing) && !app->FindTestScript(missing));
    }

    // plug-in libraries and shutdown
    CHECK(!app->AddDynamicLibraryByPath(cRZString("/nonexistent/lib.so"), true));
    CHECK(![&] { cRZString dir("/nonexistent/"); return app->AddDynamicLibraryByName(cRZString("nothing"), &dir, false); }());
    CHECK(app->PreFrameWorkShutdown());
    CHECK(app->Shutdown());

    SL::spMS2 = nullptr;
    app->Release();
    for (City* c : madeCities) delete c;
    if (failures == 0) std::puts("sc4_appcity: all passed");
    return failures ? 1 : 0;
}
