// cGZFramework: service priority bands, start-up/shutdown order, tick/idle passes, hooks, streams, quirks.
#include <cstdio>
#include <string>
#include <vector>

#include "gz/GZFramework.h"
#include "rz/cRZIStream.h"
#include "rz/cRZOStream.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static std::vector<std::string> gLog;

namespace {
class TestService final : public cRZSystemService {
public:
    TestService(const char* name, std::uint32_t id, std::int32_t priority, bool initOk = true)
        : cRZSystemService(id, priority), name_(name), initOk_(initOk) {}
    bool Init() override { gLog.push_back(name_ + ".init"); return initOk_; }
    bool Shutdown() override { gLog.push_back(name_ + ".shutdown"); return true; }
    bool OnTick(std::uint32_t n) override { gLog.push_back(name_ + ".tick" + std::to_string(n)); if (onTick) onTick(); return true; }
    bool OnIdle(std::uint32_t n) override { gLog.push_back(name_ + ".idle" + std::to_string(n)); return true; }
    std::function<void()> onTick;
private:
    std::string name_;
    bool initOk_;
};

class TestApp final : public cGZApp {
public:
    TestApp(cGZFramework& f, bool initOk = true) : cGZApp("Test", f.AppView()), framework(f), initOk_(initOk) {}
    bool Init() override { gLog.push_back("app.init"); return initOk_; }
    bool Shutdown() override { gLog.push_back("app.shutdown"); return true; }
    bool PreFrameWorkInit() override { gLog.push_back("app.PreFrameWorkInit"); return true; }
    bool PostFrameWorkInit() override { gLog.push_back("app.PostFrameWorkInit"); return true; }
    void AddApplicationServicesHere() override { gLog.push_back("app.AddApplicationServices"); AddApplicationService(this); for (auto* s : extra) framework.AddSystemService(s); }
    void AddCOMDirectorsHere() override { gLog.push_back("app.AddCOMDirectors"); }
    void AddDynamicLibrariesHere() override { gLog.push_back("app.AddDynamicLibraries"); }
    cGZFramework& framework;
    std::vector<cIGZSystemService*> extra;
private:
    bool initOk_;
};

struct TestDirector final : cRZCOMDllDirector { std::uint32_t GetDirectorID() const override { return 1; } };

class TestHooks final : public cIGZFrameWorkHooks {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool PreFrameWorkInit() override { gLog.push_back("hook.PreFrameWorkInit"); return true; }
    bool PreAppInit() override { gLog.push_back("hook.PreAppInit"); return true; }
    bool PostAppInit() override { gLog.push_back("hook.PostAppInit"); return true; }
    bool PreAppShutdown() override { return true; }
    bool PostAppShutdown() override { return true; }
    bool PostSystemServiceShutdown() override { return true; }
    bool AbortiveQuit() override { return true; }
    bool OnInstall() override { return true; }
    std::uint32_t refs = 1;
};

struct Platform final : IFrameworkPlatform {
    int passes = 0, quitAfter = 3;
    cGZFramework* framework = nullptr;
    void BeginRun() override { gLog.push_back("platform.begin"); }
    void PumpEvents(cGZFramework&) override { if (++passes >= quitAfter) framework->Quit(7); }
    bool IsSystemLocked() override { return false; }
    void EndRun() override { gLog.push_back("platform.end"); }
    void ExitProcess(int code) override { exited = code; }
    int exited = -1;
};

std::string Joined() { std::string s; for (auto& e : gLog) { s += e; s += ' '; } return s; }
}  // namespace

int main() {
    auto* processDirector = new TestDirector();
    cGZFramework::SetProcessDirector(processDirector);
    // ---- registration rules
    {
        auto* f = new cGZFramework();
        auto* a = new TestService("a", 10, 5);
        auto* dup = new TestService("dup", 10, 6);
        CHECK(f->AddSystemService(a));
        CHECK(!f->AddSystemService(dup));              // duplicate id
        CHECK(!f->AddSystemService(nullptr));
        void* out = nullptr;
        CHECK(f->GetSystemService(10, kGZIID_cIGZSystemService, &out) && out == static_cast<cIGZSystemService*>(a));
        a->cRZSystemService::Release();
        CHECK(!f->GetSystemService(11, kGZIID_cIGZSystemService, &out));
        CHECK(f->RemoveSystemService(a));
        CHECK(!f->GetSystemService(10, kGZIID_cIGZSystemService, &out));
        CHECK(f->AddSystemService(dup));               // the id is free again
        // refcount floor
        CHECK(f->Release() == 0 && f->AddRef() == 1 && f->AddRef() == 2 && f->Release() == 1 && f->Release() == 0);
        void* iface = nullptr;
        CHECK(f->QueryInterface(0x23ea70a1, &iface) && iface == static_cast<cIGZFrameWorkWindow*>(f));
        CHECK(f->QueryInterface(0x65, &iface) && iface == static_cast<cIGZFrameWork*>(f));
        CHECK(!f->QueryInterface(0x1234, &iface));
        CHECK(f->GetOnIdleInterval() == 4);
        f->SetOnIdleInterval(-3); CHECK(f->GetOnIdleInterval() == 1);
        f->SetOnIdleInterval(6); CHECK(f->GetOnIdleInterval() == 6);
        CHECK(f->GetState() == 0 && !f->IsInstall() && f->GetDebugLevel() == 2);
        delete f;   // Release never destroys the framework; the service table drops dup
    }

    // ---- start-up / run / hooks
    {
        gLog.clear();
        auto* f = new cGZFramework();
        cGZFramework::sSetFrameWork(f);
        auto* app = new TestApp(*f);
        auto* pre = new TestService("pre", 1, -1999999);
        auto* high = new TestService("high", 2, 500);
        auto* mid = new TestService("mid", 3, -2500000);
        auto* post = new TestService("post", 4, -3500000);
        app->extra = {post, mid, pre, high};
        cGZFramework::sSetApplication(static_cast<cIGZApp*>(app));
        auto* hooks = new TestHooks();
        f->AddHook(hooks);
        CHECK(!f->AddHook(hooks));
        auto* ticker = new TestService("tk", 20, 0);
        auto* once = new TestService("once", 21, 0);
        once->onTick = [&] { f->RemoveFromTick(once); };       // removal during a tick pass
        CHECK(f->AddToTick(ticker) && f->AddToTick(ticker) && f->AddToTick(once));   // duplicates are accepted silently
        CHECK(f->AddToOnIdle(ticker));
        Platform platform;
        platform.framework = f;
        cGZFramework::SetPlatform(&platform);
        cRZCmdLine cmd("sc4 -x");
        cGZFramework::Main(cmd, true);
        std::string log = Joined();
        // the application hooks run before PreFrameWorkInit; services start highest priority first within each band
        CHECK(log.find("app.AddApplicationServices app.AddCOMDirectors app.AddDynamicLibraries app.PreFrameWorkInit hook.PreFrameWorkInit") != std::string::npos);
        CHECK(log.find("high.init pre.init hook.PreAppInit app.init mid.init post.init app.PostFrameWorkInit hook.PostAppInit") != std::string::npos);
        CHECK(log.find("platform.begin") != std::string::npos && log.find("platform.end") != std::string::npos);
        CHECK(f->GetState() == cGZFramework::kStateRun);
        // 3 loop passes + 1 final pass: ticks 1..4; the idle service runs on every 4th pass (once, number 1)
        CHECK(log.find("tk.tick1 once.tick1") != std::string::npos && log.find("tk.tick4") != std::string::npos);
        CHECK(log.find("once.tick2") == std::string::npos);
        CHECK(log.find("tk.idle1") != std::string::npos && log.find("tk.idle2") == std::string::npos);
        CHECK(f->IsTickEnabled());
        CHECK(cGZFramework::ReturnCode() == 0);
        CHECK(std::string(f->CommandLine()->argv(1)) == "-x");
        CHECK(pre->IsServiceRunning() && high->IsServiceRunning() && mid->IsServiceRunning() && post->IsServiceRunning());
        CHECK(f->IsInstall() == false);   // quirk: the install argument is stored elsewhere

        // shutdown bands: <= -3000000, then (-3000000,-2000000], then the rest (ascending priority each)
        gLog.clear();
        CHECK(f->PreAppShutdown()); CHECK(f->AppShutdown()); CHECK(f->PostAppShutdown());
        CHECK(Joined() == "post.shutdown mid.shutdown app.shutdown pre.shutdown high.shutdown ");
        CHECK(!post->IsServiceRunning() && !high->IsServiceRunning());
        cGZFramework::SetPlatform(nullptr);
        // The framework's tables hold the services and hooks; the test owns the hooks and the application (framework + sSetApplication refs).
        f->COM().AddRef();   // the member COM service sits in the service table: keep the table from deleting it
        delete f;
        CHECK(hooks->refs == 1);
        delete hooks;
        app->Release();
    }

    // ---- application failing to start aborts with return code -1; other failures are tolerated
    {
        gLog.clear();
        auto* f = new cGZFramework();
        cGZFramework::sSetFrameWork(f);
        auto* app = new TestApp(*f, false);
        auto* bad = new TestService("bad", 5, -2200000, false);
        app->extra = {bad};
        cGZFramework::sSetApplication(static_cast<cIGZApp*>(app));
        cRZCmdLine cmd("sc4");
        CHECK(!cGZFramework::sInit(cmd, false));
        CHECK(cGZFramework::ReturnCode() == -1);
        CHECK(f->GetState() == cGZFramework::kStateAppInit);
        CHECK(!bad->IsServiceRunning());               // the app (-2000000) starts first, fails and aborts the pass
        CHECK(Joined().find("app.init") != std::string::npos);
        f->COM().AddRef();   // the member COM service sits in the service table: keep the table from deleting it
        delete f;
        app->Release();
    }
    delete processDirector;
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}
