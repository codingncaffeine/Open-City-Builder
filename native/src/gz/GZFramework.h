// cGZFramework — the process-wide framework object (76 functions, 0x100002a54..0x10002d6e2). It owns the COM
// registry, the system-service table (a priority-ordered multimap plus an id -> service hash), the tick and idle
// lists, the framework hooks, the std streams and the application. The startup order is driven from
// cGZFramework::Main: sInit (COM init, application registration hooks, PreAppInit / AppInit / PostAppInit by service
// priority) then sRun (the application's own loop, else Run).
//
// Service priority bands (higher first on start, lowest first on shutdown):
//   PreAppInit   priority >= -1999999            Init, then SetServiceRunning(true)
//   AppInit      -2999999 .. -2000000            same; a failing *application* (answers iid 0x66) aborts start-up
//   PostAppInit  <= -3000000
//   PreAppShutdown  <= -3000000, AppShutdown (-3000000, -2000000], PostAppShutdown > -2000000
//
// Quirks kept from the binary:
//  * Release never destroys the object; the count stops at 1.
//  * AddToTick / AddToOnIdle order by cIGZSystemService::GetServiceTickPriority() through the interface, which is
//    the default body (0), so services are appended in call order.
//  * Services removed while a tick / idle pass is running are kept alive in a pending list until the outermost pass ends.
//  * IsInstall reads a flag that sInit never sets (the install argument lands in another field), so it is always false.
//  * AbortiveQuit calls Quit(code) and then _exit with the stored code.
//  * SetCommandLine copies the command line object including its reference count field.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "gz/GZApp.h"
#include "gz/cGZCOM.h"
#include "gz/cIGZFrameWork.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZCOMDllDirector.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZCriticalSection.h"

// What Run needs from the platform (SDL on the Mac build).
class IFrameworkPlatform {
public:
    virtual ~IFrameworkPlatform() = default;
    virtual void BeginRun() = 0;                 // show the hardware cursor when windowed, start text input
    virtual void PumpEvents(class cGZFramework& framework) = 0;   // SDL_PumpEvents + dispatch every pending event
    virtual bool IsSystemLocked() = 0;           // MacGlobals::IsSystemLocked
    virtual void EndRun() = 0;                   // stop text input
    virtual void ExitProcess(int code) = 0;      // _exit
};

namespace cRZAppCmdLine {
// @ 0x100015e26: copies argument 0 into a local string and does nothing else.
void FixCommandLine(cIGZCmdLine& commandLine, const cIGZString* extra);
}

class cGZFramework final : public cIGZFrameWork, public cIGZFrameWorkWindow, private IGZCOMHost {
public:
    enum State { kStateNew = 0, kStatePreInit = 1, kStatePreAppInit = 3, kStateAppInit = 4, kStatePostAppInit = 5,
                 kStateRunning = 6, kStateRun = 7 };

    cGZFramework();                                  // @ 0x10002ab58
    ~cGZFramework();

    // process globals (mpFrameWork / mpApp / mnReturnCode)
    static cGZFramework* FrameWork() { return sFrameWork; }          // @ 0x10002ab3e
    static void sSetFrameWork(cGZFramework* framework);              // @ 0x10002b1dc
    static void sSetApplication(cIGZApp* app);                       // @ 0x10002b202
    static bool Main(const cRZCmdLine& commandLine, bool install);   // @ 0x10002d3ec (returns the original's 0)
    static bool sInit(const cRZCmdLine& commandLine, bool install);  // @ 0x10002d410
    static bool sRun();                                              // @ 0x10002d5da
    static int ReturnCode() { return sReturnCode; }
    static void SetPlatform(IFrameworkPlatform* platform) { sPlatform = platform; }
    // RZGetCOMDllDirector(): the process-wide director the COM registry initialises with.
    static void SetProcessDirector(cIGZCOMDirector* director) { sProcessDirector = director; }

    bool QueryInterface(std::uint32_t iid, void** out) override;     // @ 0x10002b05a
    std::uint32_t AddRef() override;                                 // @ 0x10002b098
    std::uint32_t Release() override;                                // @ 0x10002b134

    bool AddSystemService(cIGZSystemService* service) override;      // @ 0x10002bb2a
    bool RemoveSystemService(cIGZSystemService* service) override;   // @ 0x10002bfa0
    bool GetSystemService(std::uint32_t serviceId, std::uint32_t iid, void** out) override;   // @ 0x10002c1ae
    bool EnumSystemServices(cIGZUnknownEnumerator& enumerator, cIGZUnknown* context, std::uint32_t flags) override;   // @ 0x10002c258
    bool AddHook(cIGZFrameWorkHooks* hooks) override;                // @ 0x10002c972
    bool RemoveHook(cIGZFrameWorkHooks* hooks) override;             // @ 0x10002ca6e
    bool AddToTick(cIGZSystemService* service) override;             // @ 0x10002c312
    bool RemoveFromTick(cIGZSystemService* service) override;        // @ 0x10002c4c2
    bool AddToOnIdle(cIGZSystemService* service) override;           // @ 0x10002c5e6
    bool RemoveFromOnIdle(cIGZSystemService* service) override;      // @ 0x10002c6e2
    int GetOnIdleInterval() const override { return idleInterval_; }                // @ 0x10002babc
    void SetOnIdleInterval(int interval) override { idleInterval_ = interval > 0 ? interval : 1; }   // @ 0x10002baa6
    void OnTick(std::uint32_t tickNumber) override;                  // @ 0x10002b7ce
    void OnIdle() override;                                          // @ 0x10002b90e
    // Port-only: one tick (and, every idle interval, one idle call) for the registered services whose ids are listed.
    // The native frame loop runs the application's own steps by hand, so it ticks groups of services instead of the
    // whole list; a service that took itself out of the tick or idle list is not called.
    void TickServices(const std::uint32_t* serviceIds, std::size_t count);
    bool IsTickEnabled() const override { return tickEnabled_; }     // @ 0x10002d662
    void ToggleTick(bool enabled) override { tickEnabled_ = enabled; }              // @ 0x10002d66e
    void Quit(int exitCode) override { quit_ = true; exitCode_ = exitCode; }        // @ 0x10002d62a
    void AbortiveQuit(int exitCode) override;                        // @ 0x10002d63e
    cIGZCmdLine* CommandLine() override { return &commandLine_; }    // @ 0x10002ccbc
    bool IsInstall() const override { return isInstall_; }           // @ 0x10002b214
    cIGZCOM* GetCOMObject() override { return &com_; }               // @ 0x10002b6be
    int GetState() const override { return state_; }                 // @ 0x10002d67c
    cIGZDebugStream* GetDebugStream() override { return debugStream_; }             // @ 0x10002cec8
    cIGZDebugStream* DefaultDebugStream() override { return GetDebugStreamSlot(); } // @ 0x10002ced6
    cIGZDebugStream* DebugStream() override { return GetDebugStreamSlot(); }        // @ 0x10002cee4
    void SetDebugStream(cIGZDebugStream* stream) override { debugStream_ = stream; }   // @ 0x10002cf60
    void SetDebugLevel(int level) override;                          // @ 0x10002cef2
    int GetDebugLevel() const override { return debugLevel_; }       // @ 0x10002cf54
    cIGZOStream* StdOut() override;                                  // @ 0x10002cf8a
    cIGZOStream* StdErr() override;                                  // @ 0x10002d040
    cIGZIStream* StdIn() override;                                   // @ 0x10002d0f6
    bool GetStream(int which, std::uint32_t iid, void** out) override;   // @ 0x10002d1ac
    bool SetStream(int which, cIGZUnknown* stream) override;         // @ 0x10002d284
    void SetApplication(cIGZApp* app) override;                      // @ 0x10002b6d4
    cIGZApp* Application() const override { return sApp; }           // @ 0x10002ccae
    void ReportException(const char*) override {}                    // @ 0x10002d65c
    cIGZUnknown* ExceptionNotificationObj() const override;          // @ 0x10002cf7c

    cIGZFrameWork* AsIGZFrameWork() override { return this; }        // @ 0x10002d688
    void* GetWindowsInstance() const override { return windowsInstance_; }   // @ 0x10002d692
    void* GetMainHWND() override { return mainHwnd_; }               // @ 0x10002d6a0
    void SetMainHWND(void* hwnd) override { mainHwnd_ = hwnd; }      // @ 0x10002d6ae

    // slots 40+ (not part of the interface)
    bool PreAppInit();       // @ 0x10002b220
    bool AppInit();          // @ 0x10002b550
    bool PostAppInit();      // @ 0x10002b62a
    bool PreAppShutdown();   // @ 0x10002ccca
    bool AppShutdown();      // @ 0x10002cd72
    bool PostAppShutdown();  // @ 0x10002ce20
    void OnInstall() {}      // @ 0x10002b7c8
    void Run();              // @ 0x100002a54

    void SetCommandLine(const cRZCmdLine& commandLine);   // @ 0x10002b716
    bool HookPreFrameWorkInit();   // @ 0x10002cb4a
    bool HookPreAppInit();         // @ 0x10002cbc0
    bool HookPostAppInit();        // @ 0x10002cc36
    // The other hook phases (shutdown, abortive quit) are driven from the application.
    cGZCOM& COM() { return com_; }
    IGZAppFramework* AppView() { return &appView_; }

private:
    // IGZCOMHost
    cIGZUnknown* Framework() override { return static_cast<cIGZFrameWork*>(this); }
    cIGZCOMDirector* ProcessDirector() override;
    void CurrentAppPath(cIGZString&) override {}

    struct AppFrameworkView final : IGZAppFramework {
        explicit AppFrameworkView(cGZFramework& f) : framework(f) {}
        void AddSystemService(cIGZSystemService* service) override { framework.AddSystemService(service); }
        cGZFramework& framework;
    };

    using ServiceList = std::list<cRZAutoRefCount<cIGZSystemService>>;
    using HookList = std::list<cRZAutoRefCount<cIGZFrameWorkHooks>>;

    // @ 0x10002b314: a snapshot of the service table, ascending priority when `ascending`, else descending
    void MakeSystemServiceListCopy(ServiceList& out, bool ascending);
    void MakeHookListCopy(HookList& out);   // @ 0x10002c806
    bool RunHooks(int phase);
    cIGZDebugStream* GetDebugStreamSlot() { return debugStream_; }
    bool AddToServiceList(ServiceList& list, ServiceList& pending, bool& locked, int& depth, cIGZSystemService* service);

    static cGZFramework* sFrameWork;
    static cGZFramework* sSavedFrameWork;
    static cIGZApp* sApp;
    static int sReturnCode;
    static IFrameworkPlatform* sPlatform;
    static cIGZCOMDirector* sProcessDirector;

    std::uint32_t refCount_ = 0;                                         // +0xc0
    HookList hooks_;                                                     // +0xc8
    std::multimap<int, cRZAutoRefCount<cIGZSystemService>> services_;    // +0xe0 (by priority)
    std::unordered_map<std::uint32_t, cRZAutoRefCount<cIGZSystemService>> servicesById_;   // +0xf8
    ServiceList idleList_;                                               // +0x120
    ServiceList idlePending_;                                            // +0x138 removed during an idle pass
    ServiceList tickList_;                                               // +0x150
    ServiceList tickPending_;                                            // +0x168 removed during a tick pass
    bool tickEnabled_ = false;                                           // +0x180
    int idleInterval_ = 4;                                               // +0x184
    std::uint32_t partialTickCounter_ = 0;                               // port-only (TickServices)
    std::uint32_t idleCounter_ = 0;                                      // +0x188
    std::uint32_t idleServiceCounter_ = 0;                               // +0x18c
    bool isInstall_ = false;                                             // +0x190
    int state_ = 0;                                                      // +0x194
    bool servicesLocked_ = false, idMapLocked_ = false, idleLocked_ = false, tickLocked_ = false, hooksLocked_ = false;   // +0x198..0x19c
    int idleDepth_ = 0, tickDepth_ = 0;                                  // +0x1a0, +0x1a4
    cRZCriticalSection lock_;                                            // +0x1a8
    cRZCmdLine commandLine_{"X"};                                        // +0x1b8
    bool installArgument_ = false;                                       // +0x230
    cIGZDebugStream* debugStream_ = nullptr;                             // +0x238
    cRZAutoRefCount<cIGZOStream> stdOut_;                                // +0x240
    cRZAutoRefCount<cIGZOStream> stdErr_;                                // +0x248
    cRZAutoRefCount<cIGZIStream> stdIn_;                                 // +0x250
    int debugLevel_ = 2;                                                 // +0x258
    cRZAutoRefCount<cIGZUnknown> exceptionNotification_;                 // +0x260
    bool quit_ = false;                                                  // +0x268
    int exitCode_ = 0;                                                   // +0x26c
    void* windowsInstance_ = nullptr;                                    // +0x270
    void* mainHwnd_ = nullptr;                                           // +0x278
    cGZCOM com_{this};                                                   // +0x10
    AppFrameworkView appView_{*this};
};
