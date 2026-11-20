#include "gz/GZFramework.h"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "gz/GZCOMDirector.h"
#include "gz/cGZAllocator.h"
#include "rz/cRZSysServPtr.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZIStream.h"
#include "rz/cRZOStream.h"

cGZFramework* cGZFramework::sFrameWork = nullptr;
cGZFramework* cGZFramework::sSavedFrameWork = nullptr;
cIGZApp* cGZFramework::sApp = nullptr;
int cGZFramework::sReturnCode = 0;
IFrameworkPlatform* cGZFramework::sPlatform = nullptr;
cIGZCOMDirector* cGZFramework::sProcessDirector = nullptr;

namespace {
// RAII for the framework's recursive lock
struct Locked {
    explicit Locked(cRZCriticalSection& s) : section(s) { section.Lock(); }
    ~Locked() { section.Unlock(); }
    cRZCriticalSection& section;
};

constexpr std::uint32_t kStdInIid = 0x051b11bd;
constexpr std::uint32_t kStdOutIid = 0xc51b11cb;
}  // namespace

namespace cRZAppCmdLine {
void FixCommandLine(cIGZCmdLine&, const cIGZString*) {}
}

cGZFramework::cGZFramework() {
    // The binary builds a cRZExceptionNotification (refcount 1 after the AddRef below).
    exceptionNotification_ = static_cast<cIGZUnknown*>(new gz::cRZExceptionNotification());
}

cGZFramework::~cGZFramework() = default;

cIGZCOMDirector* cGZFramework::ProcessDirector() { return sProcessDirector ? sProcessDirector : RZGetCOMDllDirector(); }

namespace {
// What the directors and RZ:: service pointers reach the framework through.
class DirectorBridge final : public IDirectorFramework {
public:
    explicit DirectorBridge(cGZFramework& framework) : framework_(framework) {}
    void AddSystemService(cIGZUnknown* service) override {
        cIGZSystemService* asService = nullptr;
        if (service && service->QueryInterface(kGZIID_cIGZSystemService, reinterpret_cast<void**>(&asService))) {
            framework_.AddSystemService(asService);
            asService->Release();
        }
    }
    void AddHooks(cIGZFrameWorkHooks* hooks) override { framework_.AddHook(hooks); }
    // vslots 0x48 / 0x50 of the framework: AddToTick / RemoveFromTick
    void RegisterSystemService(cIGZUnknown* service) override {
        cIGZSystemService* asService = nullptr;
        if (service && service->QueryInterface(kGZIID_cIGZSystemService, reinterpret_cast<void**>(&asService))) {
            framework_.AddToTick(asService);
            asService->Release();
        }
    }
    void UnregisterSystemService(cIGZUnknown* service) override {
        cIGZSystemService* asService = nullptr;
        if (service && service->QueryInterface(kGZIID_cIGZSystemService, reinterpret_cast<void**>(&asService))) {
            framework_.RemoveFromTick(asService);
            asService->Release();
        }
    }
private:
    cGZFramework& framework_;
};
DirectorBridge* g_bridge = nullptr;

bool ResolveFrameworkService(std::uint32_t serviceId, std::uint32_t interfaceId, void** out) {
    return cGZFramework::FrameWork() && cGZFramework::FrameWork()->GetSystemService(serviceId, interfaceId, out);
}
}  // namespace

void cGZFramework::sSetFrameWork(cGZFramework* framework) {
    sFrameWork = framework;
    sSavedFrameWork = framework;
    SetRZFramework(static_cast<cIGZFrameWork*>(framework));
    SetRZServiceResolver(&ResolveFrameworkService);
    delete g_bridge;
    g_bridge = new DirectorBridge(*framework);
    SetDirectorFramework(g_bridge);
    framework->AddRef();
    framework->AddRef();
}

void cGZFramework::sSetApplication(cIGZApp* app) {
    sApp = app;
    app->AddRef();
}

bool cGZFramework::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kGZIID_cIGZFrameWork) {
        *out = static_cast<cIGZFrameWork*>(this);
    } else if (iid == kGZIID_cIGZFrameWorkWindow) {
        *out = static_cast<cIGZFrameWorkWindow*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cGZFramework::AddRef() {
    Locked guard(lock_);
    return ++refCount_;
}

// The count never drops below 1 and the object is never destroyed.
std::uint32_t cGZFramework::Release() {
    Locked guard(lock_);
    std::uint32_t result = 0;
    if (refCount_ > 1) result = --refCount_;
    return result;
}

void cGZFramework::MakeSystemServiceListCopy(ServiceList& out, bool ascending) {
    Locked guard(lock_);
    servicesLocked_ = true;
    if (ascending) {
        for (auto it = services_.begin(); it != services_.end(); ++it) out.push_back(it->second);
    } else {
        for (auto it = services_.rbegin(); it != services_.rend(); ++it) out.push_back(it->second);
    }
    servicesLocked_ = false;
}

bool cGZFramework::PreAppInit() {
    // The COM registry joins the service table here.
    cIGZSystemService* comService = nullptr;
    com_.QueryInterface(kGZIID_cIGZSystemService, reinterpret_cast<void**>(&comService));
    AddSystemService(comService);

    ServiceList copy;
    MakeSystemServiceListCopy(copy, false);
    for (auto& service : copy) {
        if (service->GetServicePriority() < -1999999) break;
        if (service->Init()) service->SetServiceRunning(true);
    }
    if (comService) comService->Release();
    return true;
}

bool cGZFramework::AppInit() {
    ServiceList copy;
    MakeSystemServiceListCopy(copy, false);
    bool ok = true;
    for (auto& service : copy) {
        int priority = service->GetServicePriority();
        if (priority < -1999999) {
            if (priority < -2999999) break;
            if (!service->Init()) {
                void* app = nullptr;
                if (service->QueryInterface(kGZIID_cIGZApp, &app)) {   // the application itself failed to start
                    static_cast<cIGZUnknown*>(app)->Release();
                    ok = false;
                    break;
                }
            } else {
                service->SetServiceRunning(true);
            }
        }
    }
    return ok;
}

bool cGZFramework::PostAppInit() {
    ServiceList copy;
    MakeSystemServiceListCopy(copy, false);
    for (auto& service : copy) {
        if (service->GetServicePriority() < -2999999) {
            if (service->Init()) service->SetServiceRunning(true);
        }
    }
    return true;
}

bool cGZFramework::PreAppShutdown() {
    ServiceList copy;
    MakeSystemServiceListCopy(copy, true);
    bool ok = true;
    for (auto& service : copy) {
        if (-3000000 < service->GetServicePriority()) break;
        if (!service->Shutdown()) ok = false;
        else service->SetServiceRunning(false);
    }
    return ok;
}

bool cGZFramework::AppShutdown() {
    ServiceList copy;
    MakeSystemServiceListCopy(copy, true);
    bool ok = true;
    for (auto& service : copy) {
        int priority = service->GetServicePriority();
        if (-3000000 < priority) {
            if (-2000000 < priority) break;
            if (!service->Shutdown()) ok = false;
            else service->SetServiceRunning(false);
        }
    }
    return ok;
}

bool cGZFramework::PostAppShutdown() {
    ServiceList copy;
    MakeSystemServiceListCopy(copy, true);
    bool ok = true;
    for (auto& service : copy) {
        if (-2000000 < service->GetServicePriority()) {
            if (!service->Shutdown()) ok = false;
            else service->SetServiceRunning(false);
        }
    }
    return ok;
}

void cGZFramework::SetApplication(cIGZApp* app) {
    Locked guard(lock_);
    sApp = app;
}

void cGZFramework::SetCommandLine(const cRZCmdLine& commandLine) {
    Locked guard(lock_);
    commandLine_ = commandLine;
}

bool cGZFramework::AddSystemService(cIGZSystemService* service) {
    if (servicesLocked_) return false;
    if (!service) return false;
    if (idMapLocked_) return false;
    Locked guard(lock_);
    int priority = service->GetServicePriority();
    std::uint32_t id = service->GetServiceID();
    if (servicesById_.find(id) != servicesById_.end()) return false;
    services_.emplace(priority, cRZAutoRefCount<cIGZSystemService>(service));
    servicesById_.emplace(id, cRZAutoRefCount<cIGZSystemService>(service));
    return true;
}

bool cGZFramework::RemoveSystemService(cIGZSystemService* service) {
    Locked guard(lock_);
    if (servicesLocked_ || idMapLocked_) return false;
    cRZAutoRefCount<cIGZSystemService> keep(service);   // the removals below may drop the last reference
    for (auto it = idleList_.begin(); it != idleList_.end(); ++it) {
        if (it->get() == service) { idleList_.erase(it); break; }
    }
    for (auto it = tickList_.begin(); it != tickList_.end(); ++it) {
        if (it->get() == service) { tickList_.erase(it); break; }
    }
    servicesLocked_ = true;
    for (auto it = services_.begin(); it != services_.end(); ++it) {
        if (it->second.get() == service) { services_.erase(it); break; }
    }
    servicesLocked_ = false;
    idMapLocked_ = true;
    for (auto it = servicesById_.begin(); it != servicesById_.end(); ++it) {
        if (it->second.get() == service) { servicesById_.erase(it); break; }
    }
    idMapLocked_ = false;
    return true;
}

bool cGZFramework::GetSystemService(std::uint32_t serviceId, std::uint32_t iid, void** out) {
    Locked guard(lock_);
    auto it = servicesById_.find(serviceId);
    idMapLocked_ = true;
    bool found = it != servicesById_.end() && it->second->QueryInterface(iid, out);
    idMapLocked_ = false;
    return found;
}

bool cGZFramework::EnumSystemServices(cIGZUnknownEnumerator& enumerator, cIGZUnknown* context, std::uint32_t flags) {
    ServiceList copy;
    MakeSystemServiceListCopy(copy, false);
    for (auto& service : copy) {
        void* unknown = nullptr;
        if (service->QueryInterface(kGZIID_cIGZUnknown, &unknown)) {
            enumerator.Enumerate(static_cast<cIGZUnknown*>(unknown), context, flags);
            static_cast<cIGZUnknown*>(unknown)->Release();
        }
    }
    return true;
}

// Shared body of AddToTick / AddToOnIdle: ids are unique within a list; ordered by descending tick priority
// (the interface answers 0 for everyone, so this appends).
bool cGZFramework::AddToServiceList(ServiceList& list, ServiceList&, bool& locked, int&, cIGZSystemService* service) {
    Locked guard(lock_);
    if (locked) return false;
    int priority = service->GetServiceTickPriority();
    std::uint32_t id = service->GetServiceID();
    auto position = list.begin();
    for (; position != list.end(); ++position) {
        if (id == (*position)->GetServiceID()) return true;
        if (priority > (*position)->GetServiceTickPriority()) break;
    }
    list.insert(position, cRZAutoRefCount<cIGZSystemService>(service));
    return true;
}

bool cGZFramework::AddToTick(cIGZSystemService* service) {
    return AddToServiceList(tickList_, tickPending_, tickLocked_, tickDepth_, service);
}

bool cGZFramework::AddToOnIdle(cIGZSystemService* service) {
    return AddToServiceList(idleList_, idlePending_, idleLocked_, idleDepth_, service);
}

bool cGZFramework::RemoveFromTick(cIGZSystemService* service) {
    Locked guard(lock_);
    if (tickLocked_) return false;
    if (tickDepth_ > 0) tickPending_.push_back(cRZAutoRefCount<cIGZSystemService>(service));   // keep alive for the pass
    for (auto it = tickList_.begin(); it != tickList_.end(); ++it) {
        if (it->get() == service) { tickList_.erase(it); return true; }
    }
    return false;
}

bool cGZFramework::RemoveFromOnIdle(cIGZSystemService* service) {
    Locked guard(lock_);
    if (idleLocked_) return false;
    if (idleDepth_ > 0) idlePending_.push_back(cRZAutoRefCount<cIGZSystemService>(service));
    for (auto it = idleList_.begin(); it != idleList_.end(); ++it) {
        if (it->get() == service) { idleList_.erase(it); return true; }
    }
    return false;
}

void cGZFramework::OnTick(std::uint32_t tickNumber) {
    lock_.Lock();
    ++tickDepth_;
    std::vector<cIGZSystemService*> snapshot;
    tickLocked_ = true;
    for (auto& service : tickList_) snapshot.push_back(service.get());
    tickLocked_ = false;
    lock_.Unlock();
    for (cIGZSystemService* service : snapshot) service->OnTick(tickNumber);
    if (tickDepth_ == 1) tickPending_.clear();
    --tickDepth_;
}

void cGZFramework::OnIdle() {
    OnTick(++idleCounter_);
    lock_.Lock();
    ++idleDepth_;
    if (idleCounter_ % static_cast<std::uint32_t>(idleInterval_) == 0) {
        std::uint32_t serviceTick = ++idleServiceCounter_;
        std::vector<cIGZSystemService*> snapshot;
        idleLocked_ = true;
        for (auto& service : idleList_) snapshot.push_back(service.get());
        idleLocked_ = false;
        lock_.Unlock();
        for (cIGZSystemService* service : snapshot) service->OnIdle(serviceTick);
    } else {
        lock_.Unlock();
    }
    if (idleDepth_ == 1) idlePending_.clear();
    --idleDepth_;
}

void cGZFramework::TickServices(const std::uint32_t* serviceIds, std::size_t count) {
    const auto listed = [&](cIGZSystemService* service) {
        const std::uint32_t id = service->GetServiceID();
        for (std::size_t i = 0; i < count; ++i) if (serviceIds[i] == id) return true;
        return false;
    };
    // The snapshots hold a reference each: a service may leave the list (or be released) from inside a call.
    std::vector<cRZAutoRefCount<cIGZSystemService>> tick, idle;
    lock_.Lock();
    const std::uint32_t tickNumber = ++partialTickCounter_;
    for (auto& service : tickList_) if (listed(service.get())) tick.push_back(service);
    const bool idleNow = tickNumber % static_cast<std::uint32_t>(idleInterval_) == 0;
    if (idleNow) for (auto& service : idleList_) if (listed(service.get())) idle.push_back(service);
    lock_.Unlock();
    for (auto& service : tick) service->OnTick(tickNumber);
    for (auto& service : idle) service->OnIdle(tickNumber / static_cast<std::uint32_t>(idleInterval_));
}

void cGZFramework::MakeHookListCopy(HookList& out) {
    Locked guard(lock_);
    hooksLocked_ = true;
    for (auto& hook : hooks_) out.push_back(hook);
    hooksLocked_ = false;
}

bool cGZFramework::AddHook(cIGZFrameWorkHooks* hooks) {
    Locked guard(lock_);
    if (hooksLocked_) return false;
    for (auto& existing : hooks_) {
        if (existing.get() == hooks) return false;
    }
    hooks_.push_back(cRZAutoRefCount<cIGZFrameWorkHooks>(hooks));
    return true;
}

bool cGZFramework::RemoveHook(cIGZFrameWorkHooks* hooks) {
    Locked guard(lock_);
    if (hooksLocked_) return false;
    for (auto it = hooks_.begin(); it != hooks_.end(); ++it) {
        if (it->get() == hooks) { hooks_.erase(it); return true; }
    }
    return false;
}

// phase: 0 PreFrameWorkInit, 1 PreAppInit, 2 PostAppInit
bool cGZFramework::RunHooks(int phase) {
    if (hooksLocked_) return false;
    HookList copy;
    MakeHookListCopy(copy);
    for (auto& hook : copy) {
        if (phase == 0) hook->PreFrameWorkInit();
        else if (phase == 1) hook->PreAppInit();
        else hook->PostAppInit();
    }
    return true;
}

bool cGZFramework::HookPreFrameWorkInit() { return RunHooks(0); }
bool cGZFramework::HookPreAppInit() { return RunHooks(1); }
bool cGZFramework::HookPostAppInit() { return RunHooks(2); }

void cGZFramework::SetDebugLevel(int level) {
    debugLevel_ = level;
    char text[64];
    std::snprintf(text, sizeof text, "%d", level);
    setenv("GZFRAMEWORK_DEBUG_LEVEL", text, 1);   // RZSetEnvironmentVariable @ 0x10001aa03
}

cIGZUnknown* cGZFramework::ExceptionNotificationObj() const { return exceptionNotification_.get(); }

cIGZOStream* cGZFramework::StdOut() {
    Locked guard(lock_);
    if (!stdOut_) stdOut_ = static_cast<cIGZOStream*>(new cRZNullOStream());
    return stdOut_.get();
}

cIGZOStream* cGZFramework::StdErr() {
    Locked guard(lock_);
    if (!stdErr_) stdErr_ = static_cast<cIGZOStream*>(new cRZNullOStream());
    return stdErr_.get();
}

cIGZIStream* cGZFramework::StdIn() {
    Locked guard(lock_);
    if (!stdIn_) stdIn_ = static_cast<cIGZIStream*>(new cRZNullIStream());
    return stdIn_.get();
}

bool cGZFramework::GetStream(int which, std::uint32_t iid, void** out) {
    Locked guard(lock_);
    if (!out) return false;
    if (which == 0) return static_cast<cIGZUnknown*>(StdOut())->QueryInterface(iid, out);
    if (which == 1) return static_cast<cIGZUnknown*>(StdErr())->QueryInterface(iid, out);
    if (which == 2) return static_cast<cIGZUnknown*>(StdIn())->QueryInterface(iid, out);
    return false;
}

bool cGZFramework::SetStream(int which, cIGZUnknown* stream) {
    Locked guard(lock_);
    void** slot;
    std::uint32_t iid;
    if (which == 2) { stdIn_ = nullptr; slot = stdIn_.AsOutParam(); iid = kStdInIid; }
    else if (which == 1) { stdErr_ = nullptr; slot = stdErr_.AsOutParam(); iid = kStdOutIid; }
    else if (which == 0) { stdOut_ = nullptr; slot = stdOut_.AsOutParam(); iid = kStdOutIid; }
    else return false;
    if (!stream) return true;
    return stream->QueryInterface(iid, slot);
}

void cGZFramework::AbortiveQuit(int exitCode) {
    Quit(exitCode);
    if (sPlatform) sPlatform->ExitProcess(exitCode_);
    else std::_Exit(exitCode_);
}

void cGZFramework::Run() {
    if (sPlatform) sPlatform->BeginRun();
    if (!quit_) {
        do {
            if (sPlatform) sPlatform->PumpEvents(*this);
            if (!(sPlatform && sPlatform->IsSystemLocked())) OnIdle();
        } while (!quit_);
    }
    if (sPlatform) sPlatform->EndRun();
    if (IsTickEnabled()) OnIdle();
}

bool cGZFramework::Main(const cRZCmdLine& commandLine, bool install) {
    if (sInit(commandLine, install)) sRun();
    sReturnCode = 0;
    return false;
}

bool cGZFramework::sInit(const cRZCmdLine& commandLine, bool install) {
    cGZFramework* framework = sFrameWork;
    framework->state_ = kStateNew;
    framework->SetCommandLine(commandLine);
    cRZAppCmdLine::FixCommandLine(framework->commandLine_, nullptr);
    framework->installArgument_ = install;

    cGZCOM& com = framework->com_;
    if (com.RealInit()) com.SetServiceRunning(true);
    cIGZSystemService* comService = nullptr;
    if (com.QueryInterface(kGZIID_cIGZSystemService, reinterpret_cast<void**>(&comService))) {
        framework->AddSystemService(comService);
        comService->Release();
    }

    sApp->AddApplicationServicesHere();
    sApp->AddCOMDirectorsHere();
    sApp->AddDynamicLibrariesHere();
    framework->state_ = kStatePreInit;
    if (!sApp->PreFrameWorkInit()) {
        com.RealShutdown();
    } else {
        framework->HookPreFrameWorkInit();
        framework->state_ = kStatePreAppInit;
        framework->PreAppInit();
        framework->HookPreAppInit();
        framework->state_ = kStateAppInit;
        if (framework->AppInit()) {
            framework->ToggleTick(true);
            framework->state_ = kStatePostAppInit;
            framework->PostAppInit();
            framework->state_ = kStateRunning;
            sApp->PostFrameWorkInit();
            framework->HookPostAppInit();
            return sReturnCode == 0;
        }
    }
    sReturnCode = -1;
    return false;
}

bool cGZFramework::sRun() {
    cIGZApp* app = sFrameWork->Application();
    sFrameWork->state_ = kStateRun;
    if (app->GZRun() == 0) sFrameWork->Run();
    return sReturnCode == 0;
}
