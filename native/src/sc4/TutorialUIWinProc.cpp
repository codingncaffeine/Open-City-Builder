#include "platform/CrashLog.h"
#include "sc4/TutorialUIWinProc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinHTML.h"
#include "gz/WinTextEdit.h"
#include "gz/cIGZVariant.h"
#include "lua/ISCLua.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/Occupant.h"
#include "sc4/WinCatalogView.h"

namespace sc4 {
namespace {

ITutorialUIHost* g_host = nullptr;

constexpr std::uint32_t kUIScriptGroup = 0x96a006b0;
constexpr std::uint32_t kArrowAScript = 0x0a41be3e, kArrowAId = 0x0a41c7b2, kArrowBScript = 0x0a41be3f, kArrowBId = 0x0a41c7b3;
constexpr std::uint32_t kArrowAImage = 0xea496354;              // the image window of an arrow (+1 for the second arrow)
constexpr std::uint32_t kIidWin = 0x22ba0121, kIidButton = 0x8810, kIidCatalogView = 0x49d98264;
constexpr std::uint32_t kIidTextEdit = 0x231a1c57;
constexpr std::uint32_t kExitTextA = 0xca62395f, kExitTextB = 0xca623979;
constexpr std::uint32_t kMsgTutorialFinished = 0xaa3840e6;
constexpr std::uint32_t kMsgWindowShown = 0xa2bf8ad5;           // the window procedure's "attached" message (-0x5d40752b)
constexpr std::uint32_t kMsgHtmlLink = 0xe4a85d91;              // a link was clicked in the HTML control (-0x1b57a26f)
constexpr std::uint32_t kCommandButton = 0x287259f6;            // a button command (cGZMessage param1)
constexpr std::uint32_t kDisasterTutorial = 0x621cda33;
constexpr int kFlagVisible = 1, kFlagEnabled = 2;
constexpr int kHtmlFlagScrollBar = 0x40;
const char* const kTutorialTaskTableName = "tutorialtasks";
const char* const kTutorialAddress = "sc4://tutorial";
const char* const kNextTutorialLink = "sc4://action/next_tutorial";

constexpr std::uint32_t kMessages[] = {kTutorialUIMsgZoned, kTutorialUIMsgPlaced, kTutorialUIMsgDispatch, kTutorialUIMsgDisaster,
                                       kTutorialUIMsgPopulation, kTutorialUIMsgPlaced2, kTutorialUIMsgLot};

// Task kinds (cISC4TutorialTask::GetWhichFunction).
enum : int {
    kTaskZone = 1, kTaskNetwork = 2, kTaskBuilding = 3, kTaskButton = 4, kTaskLot = 5, kTaskViewOnly = 8,
    kTaskNoFlashB = 0xb, kTaskDisaster = 0xc, kTaskPopulation = 0xd, kTaskWindow = 0x10, kTaskPower = 0x11,
};

// sEnumeratedForAvailability: how often each window id was visited by the button enumerations (each at most twice).
std::map<std::uint32_t, std::uint32_t>& Enumerated() {
    static auto* visited = new std::map<std::uint32_t, std::uint32_t>();
    return *visited;
}

template <class T> void Drop(T*& p) {
    if (p) {
        T* old = p;
        p = nullptr;
        old->Release();
    }
}
template <class T> void Assign(T*& slot, T* value) {
    if (slot == value) return;
    if (value) value->AddRef();
    T* old = slot;
    slot = value;
    if (old) old->Release();
}

// sEnumFuncEnableButtons (0x00088524): every button and catalog of the tree enabled again.
bool EnableButtons(cIGZWin*, std::uint32_t, void* child, void* context) {
    auto* window = static_cast<cIGZWin*>(child);
    if (!window) return true;
    window->AddRef();
    std::uint32_t& count = Enumerated()[window->GetID()];
    if (count < 2) {
        ++count;
        gz::cIGZWinBtn* button = nullptr;
        if (window->QueryInterface(kIidButton, reinterpret_cast<void**>(&button)) && button) window->SetFlag(kFlagEnabled, true);
        cISC4WinCatalogView* catalog = nullptr;
        if (window->QueryInterface(kIidCatalogView, reinterpret_cast<void**>(&catalog)) && catalog)
            catalog->SetEnabledAllItemsExceptItemWithID(0xffffffffu, true);
        if (catalog) catalog->Release();
        if (button) button->Release();
    }
    window->EnumChildren(kIidWin, &EnableButtons, context);
    window->Release();
    return true;
}

// sEnumFuncDisableButtons (0x000886e6): the buttons the task does not name are disabled (the named ones and their children
// enabled), and a catalog keeps only the task's item.
bool DisableButtons(cIGZWin*, std::uint32_t, void* child, void* context) {
    auto* window = static_cast<cIGZWin*>(child);
    auto* task = static_cast<cISC4TutorialTask*>(context);
    if (!window) return true;
    window->AddRef();
    if (task) {
        std::uint32_t& count = Enumerated()[window->GetID()];
        if (count < 2) {
            ++count;
            const bool named = task->HasButtonGUID(window->GetID());
            gz::cIGZWinBtn* button = nullptr;
            if (window->QueryInterface(kIidButton, reinterpret_cast<void**>(&button)) && button)
                window->SetFlag(kFlagEnabled, named);
            if (named) window->EnumChildren(kIidWin, &EnableButtons, context);
            cISC4WinCatalogView* catalog = nullptr;
            if (window->QueryInterface(kIidCatalogView, reinterpret_cast<void**>(&catalog)) && catalog)
                catalog->SetEnabledAllItemsExceptItemWithID(task->GetCatalogButtonItemID(), false);
            if (button) button->Release();
            if (catalog) catalog->Release();
        }
    }
    window->EnumChildren(kIidWin, &DisableButtons, context);
    window->Release();
    return true;
}

bool RectContains(const SC4TutorialRect& r, int x, int z) { return !(x < r.x0 || z < r.z0 || r.x1 < x || r.z1 < z); }

}  // namespace

ITutorialUIHost* TutorialUIHost() { return g_host; }
void SetTutorialUIHost(ITutorialUIHost* host) { g_host = host; }

cSC4TutorialUIWinProc::cSC4TutorialUIWinProc() = default;

cSC4TutorialUIWinProc::~cSC4TutorialUIWinProc() {
    for (cSC4TutorialTask* task : tasks_) task->Release();
    tasks_.clear();
    Drop(lua_);
    Drop(flashing_);
    Drop(catalog_);
    Drop(exitDialog_);
    Drop(arrowB_);
    Drop(arrowA_);
    Drop(arrow_);
    Drop(html_);
    Drop(window_);
}

bool cSC4TutorialUIWinProc::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kTutorialUIWinProcIid) *out = static_cast<gz::cIGZWinProc*>(this);
    else if (iid == kTutorialUIMessageTargetIid) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1) *out = static_cast<gz::cIGZWinProc*>(this);   // cRZUnknown answers the unknown
    if (!*out) return false;
    AddRef();
    return true;
}

std::uint32_t cSC4TutorialUIWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

cIGZWin* cSC4TutorialUIWinProc::Window() const { return window_ ? window_->AsIGZWin() : nullptr; }

cISC4TutorialTask* cSC4TutorialUIWinProc::GetCurrentTask() {
    return taskIndex_ < tasks_.size() ? tasks_[taskIndex_] : nullptr;
}

void cSC4TutorialUIWinProc::Subscribe(bool on) {
    cIGZMessageServer2* messages = g_host ? g_host->Messages() : nullptr;
    if (!messages) return;
    for (std::uint32_t type : kMessages) {
        if (on) messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), type);
        else messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), type);
    }
}

// ================================================================ life cycle

void cSC4TutorialUIWinProc::Init() {
    Drop(window_);
    if (g_host) {
        cGZPersistResourceKey key{0, kUIScriptGroup, kTutorialUIScript};
        if (g_host->CreateWindowFromScript(key, g_host->MainWindow(), kTutorialUIWindowId, kTutorialUIWindowIid,
                                           reinterpret_cast<void**>(&window_)) && window_) {
            window_->SetWinProc(static_cast<gz::cIGZWinProc*>(this));
            cISC4TutorialSystem* system = g_host->TutorialSystem();
            Assign(lua_, system ? system->GetScriptingContext() : nullptr);
            windowY_ = Window()->GetT();
            windowX_ = Window()->GetL();
            int first = 0, count = 0;
            if (system) g_host->TutorialItem(system->GetTutorialGUID(), first, count);
            taskIndex_ = static_cast<std::uint32_t>(first - 1 + (system ? system->GetTutorialTaskNumber() : 0));
            if (load_task_script() && window_) {
                if (cIGZWin* rect = Window()->GetChildWindowFromIDRecursive(kTutorialUIHtmlRect)) {
                    Drop(html_);
                    html_ = g_host->CreateHtmlControl();
                    if (html_) {
                        html_->AsIGZWin()->SetArea(0, 0, rect->GetW(), rect->GetH());
                        html_->ClearFlag(kHtmlFlagScrollBar);
                        rect->ChildAdd(html_->AsIGZWin());
                        html_->AsIGZWin()->SetNotificationTarget(Window());
                    }
                }
            }
        }
    }
    Drop(arrowA_);
    LoadArrow(kArrowAScript, kArrowAId, arrowA_);
    Drop(arrowB_);
    LoadArrow(kArrowBScript, kArrowBId, arrowB_);
    Subscribe(true);
    ReStartCurrentTask();
}

void cSC4TutorialUIWinProc::Shutdown() {
    if (cIGZWin* view = g_host ? g_host->ViewWindow() : nullptr) {
        view->AddRef();
        Enumerated().clear();
        view->EnumChildren(kIidWin, &EnableButtons, nullptr);
        view->Release();
    }
    cISC4TutorialTask* task = GetCurrentTask();
    if (task) {
        task->AddRef();
        task->EndTask();
    }
    Subscribe(false);
    if (window_) {
        if (cIGZWinMgr* manager = Window()->GetWindowManager()) manager->DestroyWindow(Window());
        Drop(window_);
    }
    KillArrow();
    StopFlashing();
    dispatchDone_ = false;
    if (task) task->Release();
}

bool cSC4TutorialUIWinProc::load_task_script() {
    if (!lua_) return false;
    lua_->GetTop();
    lua_->GetGlobal(kTutorialTaskTableName);
    if (!lua_->IsTable(-1)) {
        lua_->Pop(1);
        return false;
    }
    const int count = lua_->GetN(-1);
    lua_->Pop(1);
    tasks_.reserve(static_cast<std::size_t>(std::max(count, 0)));
    if (count <= 0) return false;
    for (int number = 1; number < count + 1; ++number) {
        auto* task = new cSC4TutorialTask();
        task->AddRef();
        task->Init(lua_, number);
        tasks_.push_back(task);
    }
    return true;
}

// ================================================================ tasks

bool cSC4TutorialUIWinProc::ReStartCurrentTask() {
    cISC4TutorialTask* task = GetCurrentTask();
    if (!task) return false;
    setInstructionText(cRZString(task->GetInstructionMsg()));
    task->ExecuteTask();
    ShowUIHintsForUsers();
    if (g_host && g_host->SimulatorPaused()) g_host->ResumeSimulator();
    if (cIGZWin* view = g_host ? g_host->ViewWindow() : nullptr) {
        g_host->SetViewZoom(task->GetZoomLevel());
        const int viewWidth = view->GetW();
        const int windowWidth = Window() ? Window()->GetW() : 0;
        windowY_ = 0;
        windowX_ = viewWidth - windowWidth;
        if (Window()) Window()->GZWinMoveTo(windowX_, windowY_);
    }
    if (g_host) g_host->SetClockVisualTimeMode(1);
    return true;
}

void cSC4TutorialUIWinProc::moveToNext() {
    cISC4TutorialTask* task = GetCurrentTask();
    if (task) task->AddRef();
    if (!task) return;
    HideArrow();
    cISC4TutorialSystem* system = g_host ? g_host->TutorialSystem() : nullptr;
    if (system) {
        int first = 0, count = 0;
        g_host->TutorialItem(system->GetTutorialGUID(), first, count);
        const int last = first + count - 1;
        const int base = first - 1;
        const int index = system->GetTutorialTaskNumber() + base;
        const auto next = static_cast<std::uint32_t>(index + 1);
        const auto size = static_cast<std::uint32_t>(tasks_.size());
        if (next < size && 0 < static_cast<int>(next) && static_cast<int>(next) < last) {
            platform::Breadcrumb("tutorial: task %d done (kind %d), next %u", index, task->GetWhichFunction(), next);
            if (task->GetWhichFunction() != kTaskViewOnly) g_host->EndViewInput();
            if (catalog_) {
                catalog_->FlashItemByID(0xffffffffu, 0, 0);
                Drop(catalog_);
            }
            task->EndTask();
            if (task->GetWhichFunction() == kTaskButton) HideArrow();
            system->IncreaseTutorialNumber();
            taskIndex_ = static_cast<std::uint32_t>(system->GetTutorialTaskNumber() + base);
            Assign(task, GetCurrentTask());
            if (task) {
                setInstructionText(cRZString(task->GetInstructionMsg()));
                g_host->SetViewZoom(task->GetZoomLevel());
                task->ExecuteTask();
            }
            int x = windowX_ + (task ? task->GetWindowOffsetX() : 0);
            int y = windowY_ + (task ? task->GetWindowOffsetY() : 0);
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (Window()) Window()->GZWinMoveTo(x, y);
            tick_self(2, 0, 0, nullptr, nullptr);
            const auto after = static_cast<std::uint32_t>(index + 2);
            lastTask_ = !(static_cast<int>(after) < last && after < size);
        } else {
            finish_tutorial(true);
        }
    }
    if (task) task->Release();
}

int cSC4TutorialUIWinProc::ResetTutorialNumAndIndex(int guid, int index) {
    int first = 0, count = 0;
    if (g_host) g_host->TutorialItem(guid, first, count);
    const int base = first - 1;
    const auto target = static_cast<std::uint32_t>(base + index);
    if (target < tasks_.size() && -1 < static_cast<int>(target) && static_cast<int>(target) < first + count - 1) {
        if (cISC4TutorialTask* task = GetCurrentTask()) {
            HideArrow();
            task->EndTask();
            taskIndex_ = target;
            ReStartCurrentTask();
            return static_cast<int>(taskIndex_) - base;
        }
    }
    return -1;
}

void cSC4TutorialUIWinProc::finish_tutorial(bool lastTask) {
    cISC4TutorialTask* task = GetCurrentTask();
    if (!task) return;
    task->AddRef();
    task->EndTask();
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    task->EndTask();   // quirk kept: the original ends the task twice
    message->SetType(kMsgTutorialFinished);
    message->SetData1(lastTask ? 0 : 1);
    if (cIGZMessageServer2* messages = g_host ? g_host->Messages() : nullptr) messages->MessagePost(message, false);
    message->Release();
    task->Release();
}

void cSC4TutorialUIWinProc::handleButtons(std::uint32_t command) {
    clickPending_ = false;
    cISC4TutorialTask* task = GetCurrentTask();
    if (task && !tasks_.empty() && task->GetWhichFunction() == kTaskButton && task->handleButtons(command)) moveToNext();
}

bool cSC4TutorialUIWinProc::OnTutorialEvent(std::uint32_t event) {
    cISC4TutorialTask* task = GetCurrentTask();
    if (task) {
        task->AddRef();
        if (event == 1 && task->GetWhichFunction() == kTaskZone) {
            task->ShowHintsForUsers();
            setInstructionText(cRZString(task->GetTryAgainMsg()));
        }
        task->Release();
    }
    return true;
}

void cSC4TutorialUIWinProc::ShowTryAgain(cISC4TutorialTask* task) {
    setInstructionText(cRZString(task->GetTryAgainMsg()));
}

// ================================================================ instruction text

void cSC4TutorialUIWinProc::setInstructionText(const cRZString& text) {
    static const bool trace = std::getenv("SC4_TUTORIALDEBUG") != nullptr;   // dev: the task and the start of its text
    if (trace) std::fprintf(stderr, "tutorial: task %u text \"%.60s\"\n", taskIndex_, text.Data());
    if (!html_) return;
    html_->SetDocument(std::string(text.Data(), text.Strlen()), cRZString(kTutorialAddress));
    DoSpecialDocumentProcessing(text);
}

// "<!-- legacy id=0x... winflag_visible=yes|no winflag_enabled=yes|no href="..." -->": a control of the tutorial window
// shown / hidden, enabled / disabled, given a link (its window param 0x0a8fe6cb). Other comments are ignored.
void cSC4TutorialUIWinProc::DoSpecialDocumentProcessing(const cRZString& text) {
    if (!window_) return;
    const std::string doc(text.Data(), text.Strlen());
    std::size_t open = doc.find("<!--");
    while (open != std::string::npos) {
        const std::size_t close = doc.find("-->", open);
        if (close == std::string::npos) break;
        cRZString comment(doc.substr(open, close - open).c_str());
        comment.Trim();
        cIGZWin* control = nullptr;
        int field = 0;
        cRZString token;
        while (comment.SplitTokenSeparated(' ', token)) {
            const std::string word(token.Data(), token.Strlen());
            if (word == "<!--" || word == "-->") continue;
            if (field == 0) {
                cRZString lower(token);
                lower.MakeLower();
                if (std::string(lower.Data(), lower.Strlen()) != "legacy") break;
                field = 1;
                continue;
            }
            cRZString key;
            if (!token.SplitTokenSeparated('=', key)) continue;
            key.Trim();
            key.MakeLower();
            token.Trim();
            const std::string name(key.Data(), key.Strlen());
            std::string value(token.Data(), token.Strlen());
            std::string lower = value;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name == "id") {
                cIGZWin* found = Window()->GetChildWindowFromIDRecursive(static_cast<std::uint32_t>(std::strtoul(lower.c_str(), nullptr, 16)));
                Assign(control, found);
            } else if (name == "winflag_visible") {
                if (control) control->SetFlag(kFlagVisible, lower == "yes");
            } else if (name == "winflag_enabled") {
                if (control) control->SetFlag(kFlagEnabled, lower == "yes");
            } else if (name == "href" && control) {
                if (!value.empty() && value.front() == '"') value.erase(0, 1);
                if (!value.empty() && value.back() == '"') value.pop_back();
                if (!value.empty()) {
                    auto* link = new cRZVariant(cRZString(value.c_str()));
                    link->AddRef();
                    control->SetParam(kTutorialUILinkParam, link);
                    link->Release();
                }
            }
            ++field;
        }
        if (control) control->Release();
        open = doc.find("<!--", close);
    }
}

// ================================================================ hints, arrows, catalog

void cSC4TutorialUIWinProc::StopFlashing() {
    if (!flashing_) return;
    gz::cIGZWinBtn* button = nullptr;
    if (flashing_->QueryInterface(kIidButton, reinterpret_cast<void**>(&button)) && button) button->Flash(0, 200);
    Drop(flashing_);
    if (button) button->Release();
}

void cSC4TutorialUIWinProc::ShowUIHintsForUsers() {
    cISC4TutorialTask* task = GetCurrentTask();
    cIGZWin* view = g_host ? g_host->ViewWindow() : nullptr;
    if (!task || !view) return;
    Enumerated().clear();
    view->EnumChildren(kIidWin, &DisableButtons, task);
    if (catalogHandled_) {
        catalogHandled_ = false;
        return;
    }
    const int kind = task->GetWhichFunction();
    if (kind == kTaskNoFlashB || kind == kTaskPopulation || kind == kTaskDisaster) {
        StopFlashing();
        return;
    }
    if (kind == kTaskWindow) {
        cIGZWin* target = nullptr;
        if (view->GetChildAsRecursive(task->GetTargetGUID(), kIidWin, reinterpret_cast<void**>(&target)) && target) {
            task->GetRegion();
            tick_self(4, 0, 0, target, target);
            target->SetFlag(kFlagEnabled, true);
        }
        if (target) target->Release();
        return;
    }
    cIGZWin* target = task->GetTargetWindow();
    if (!target) return;
    target->AddRef();
    tick_self(4, 0, 0, target, target);
    target->SetFlag(kFlagEnabled, true);
    if (flashing_ != target) {
        StopFlashing();
        gz::cIGZWinBtn* button = nullptr;
        if (target->QueryInterface(kIidButton, reinterpret_cast<void**>(&button)) && button) button->Flash(0x7fffffff, 200);
        Assign(flashing_, target);
        if (button) button->Release();
    }
    target->Release();
}

void cSC4TutorialUIWinProc::HandleCatalogView(cISC4WinCatalogView* view) {
    clickPending_ = false;
    if (!view || flashing_ == view->AsIGZWin()) return;
    StopFlashing();
    Assign(flashing_, view->AsIGZWin());
    cISC4TutorialTask* task = GetCurrentTask();
    const std::uint32_t item = task ? task->GetCatalogButtonItemID() : 0;
    if (catalog_) catalog_->FlashItemByID(0xffffffffu, 0, 0);
    catalogHandled_ = true;
    view->MakeItemVisible(item);
    tick_self(3, item, 1, view, reinterpret_cast<cIGZUnknown*>(view));
}

bool cSC4TutorialUIWinProc::LoadArrow(std::uint32_t instance, std::uint32_t id, cIGZWin*& out) {
    out = nullptr;
    if (!g_host) return false;
    cGZPersistResourceKey key{0, kUIScriptGroup, instance};
    if (!g_host->CreateWindowFromScript(key, nullptr, id, kIidWin, reinterpret_cast<void**>(&out)) || !out) return false;
    out->HideWindow();
    out->SetFlag(0x200000, true);   // the arrow ignores the cursor
    return true;
}

void cSC4TutorialUIWinProc::HideArrow() {
    if (arrow_) arrow_->HideWindow();
}

void cSC4TutorialUIWinProc::ShowArrow() {
    if (!arrow_) return;
    arrow_->PullToFront();
    arrow_->ShowWindow();
}

void cSC4TutorialUIWinProc::KillArrow() {
    Drop(arrow_);
    for (cIGZWin** arrow : {&arrowA_, &arrowB_}) {
        if (!*arrow) continue;
        if (cIGZWinMgr* manager = (*arrow)->GetWindowManager()) manager->DestroyWindow(*arrow);
        Drop(*arrow);
    }
}

// Points an arrow at the screen rectangle left..right at `top`: the first arrow when it fits to the right of `left`, else
// the second one ends at `left`. Only for button and window tasks.
void cSC4TutorialUIWinProc::DrawArrow(int left, int top, int right) {
    HideArrow();
    Drop(arrow_);
    if (!arrowA_ || !arrowB_) return;
    cISC4TutorialTask* task = GetCurrentTask();
    if (!task) return;
    task->AddRef();
    const int kind = task->GetWhichFunction();
    if (!((kind == kTaskButton && task->GetExtraData() != 0) || kind == kTaskWindow)) {
        task->Release();
        return;
    }
    cRZRect screen{};
    if (cIGZWin* parent = arrowA_->GetWindowManager() ? arrowA_->GetWindowManager()->GetMainWindow() : nullptr)
        parent->GetAreaAbsolute(screen);
    int x;
    if (right + left < screen.right + screen.left) {
        Assign(arrow_, arrowA_);
        x = right;
    } else {
        Assign(arrow_, arrowB_);
        x = left - arrow_->GetW();
    }
    const int height = arrow_->GetH();
    gz::cIGZWinBtn* image = nullptr;
    arrow_->GetChildAsRecursive(kArrowAImage + (arrow_ != arrowA_ ? 1u : 0u), kIidButton, reinterpret_cast<void**>(&image));
    if (image) {
        image->Flash(0x7fffffff, 200);
        arrow_->GZWinMoveTo(x, top - height);
    }
    ShowArrow();
    if (image) image->Release();
    task->Release();
}

void cSC4TutorialUIWinProc::DrawArrow(cIGZWin* target) {
    if (!target) return;
    int left = 0, top = 0;
    int right = target->GetW() - 1, bottom = target->GetH() - 1;
    target->WindowToScreenCoordinates(left, top);
    target->WindowToScreenCoordinates(right, bottom);
    DrawArrow(left, top, right);
}

void cSC4TutorialUIWinProc::tick_self(std::uint32_t kind, std::int64_t data2, std::int64_t data3, void* window, cIGZUnknown* object) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kTutorialUITick);
    message->SetData1(kind);
    message->SetData2(static_cast<std::uint64_t>(data2));
    message->SetData3(static_cast<std::uint64_t>(data3));
    message->SetVoid4(window);
    message->SetIGZUnknown(object);
    if (cIGZMessageServer2* messages = g_host ? g_host->Messages() : nullptr)
        messages->GeneralMessagePostToTarget(message, static_cast<cIGZMessageTarget2*>(this));
    message->Release();
}

// ================================================================ the pointer

bool cSC4TutorialUIWinProc::FilterLeftMouseClick(int& x, int& y, int flags) {
    if (!g_host || !g_host->ViewWindow()) return false;
    float position[3] = {};
    if (!g_host->PickTerrain(x, y, position)) {
        if (flags == 1 && clickPending_) clickPending_ = false;
        return true;
    }
    int cellX = 0, cellZ = 0;
    g_host->PositionToCell(position[0], position[2], cellX, cellZ);
    cISC4TutorialTask* task = GetCurrentTask();
    if (!task) return true;
    task->AddRef();
    bool result = true;
    const int kind = task->GetWhichFunction();
    if (kind == kTaskZone || kind == kTaskBuilding || kind == kTaskNetwork || kind == kTaskPower || kind == kTaskLot) {
        const SC4TutorialRect area = task->GetRegion();
        const bool inside = RectContains(area, cellX, cellZ);
        // DIVERGENCE (2026-11-23, user reports: the tutorial zone drag could not be completed): while a drag that started inside
        // the region is held, the original drops every move outside it (the preview freezes as soon as the pointer crosses
        // one edge of the slanted grid) and rewrites a release outside it to the region's far corner as CELL numbers
        // (x1 - 1, z1 - 1) written into VIEW coordinates, so the tool picked an unrelated cell. Both now go to the view point
        // over the nearest cell inside the region (the drag follows the pointer along the grid's edge); without that point
        // (off screen) a move is dropped as before and a release uses the last view point inside the region.
        auto clampIntoRegion = [&]() -> bool {
            const int cx = std::min(std::max(cellX, area.x0), area.x1), cz = std::min(std::max(cellZ, area.z0), area.z1);
            int vx = 0, vy = 0;
            if (!g_host->CellViewPoint(cx, cz, vx, vy)) return false;
            x = vx;
            y = vy;
            return true;
        };
        if (flags == 0) {
            if (clickPending_ && kind != kTaskBuilding) result = inside || clampIntoRegion();
            if (clickPending_ && inside) lastInsideX_ = x, lastInsideY_ = y;
        } else if (flags == 1) {
            if (clickPending_) {
                clickPending_ = false;
                if (!inside && !clampIntoRegion()) {
                    x = lastInsideX_;
                    y = lastInsideY_;
                }
            }
        } else if (flags == -1) {
            if (!clickPending_) clickPending_ = inside;
            if (clickPending_ && inside) lastInsideX_ = x, lastInsideY_ = y;
            result = clickPending_;
        }
    }
    task->Release();
    return result;
}

bool cSC4TutorialUIWinProc::IsRightBuildingPosition(float x, float z) {
    cISC4TutorialTask* task = GetCurrentTask();
    int cellX = 0, cellZ = 0;
    if (!g_host || !task || !g_host->PositionToCell(x, z, cellX, cellZ)) return false;
    SC4TutorialRect lot{};
    if (!g_host->LotRectAtCell(cellX, cellZ, lot)) return false;
    const SC4TutorialRect& area = task->GetRegion();
    return area.x0 == lot.x0 && area.z0 == lot.z0 && area.x1 == lot.x1 && area.z1 == lot.z1;
}

// ================================================================ the exit dialog and the window's own messages

void cSC4TutorialUIWinProc::exit_to_region() {
    cISC4TutorialTask* task = GetCurrentTask();
    if (task) task->AddRef();
    if (window_ && g_host) {
        cIGZWin* tutorial = Window();
        if (!exitDialog_) {
            cGZPersistResourceKey key{0, kUIScriptGroup, kTutorialUIExitScript};
            cIGZWin* dialog = nullptr;
            if (g_host->CreateWindowFromScript(key, tutorial, kTutorialUIExitWindow, kIidWin, reinterpret_cast<void**>(&dialog)))
                exitDialog_ = dialog;
            if (exitDialog_) {
                exitDialog_->PullToFront();
                exitDialog_->GZWinMoveTo(0x32, 0x78);
                exitDialog_->SetNotificationTarget(tutorial);
                for (std::uint32_t id : {kTutorialUIExitSave, kTutorialUIExitCancel, kTutorialUIExitFinish})
                    if (cIGZWin* button = exitDialog_->GetChildWindowFromIDRecursive(id)) button->SetNotificationTarget(tutorial);
                for (std::uint32_t id : {kExitTextA, kExitTextB}) {
                    gz::cIGZWinTextEdit* edit = nullptr;
                    if (exitDialog_->GetChildAsRecursive(id, kIidTextEdit, reinterpret_cast<void**>(&edit)) && edit) edit->ClearFlag(1);
                    if (edit) edit->Release();
                }
            }
        }
        if (exitDialog_) {
            exitDialog_->ShowWindow();
            if (cIGZWinMgr* manager = exitDialog_->GetWindowManager()) manager->RunPopup(exitDialog_);
        }
    }
    if (task) task->Release();
}

bool cSC4TutorialUIWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (!window_ || Window() != window) return false;
    if (message.type == kMsgWindowShown) {
        cIGZWin* tutorial = Window();
        tutorial->AddRef();
        if (cIGZWin* exit = tutorial->GetChildWindowFromIDRecursive(kTutorialUIExitButton)) exit->SetNotificationTarget(tutorial);
        tutorial->Release();
        return false;
    }
    if (message.type == kMsgHtmlLink) {
        auto* link = reinterpret_cast<cIGZString*>(static_cast<std::uintptr_t>(message.param2));
        if (link && link->IsEqualToStringRef(cRZString(kNextTutorialLink), false)) {
            moveToNext();
            return false;
        }
        return !(g_host && g_host->DoURL(link));
    }
    if (message.type != kGZMsgCommand) return false;
    cISC4TutorialTask* task = GetCurrentTask();
    if (!task) return false;
    task->AddRef();
    if (static_cast<std::uint32_t>(message.param1) == kCommandButton) {
        const auto id = static_cast<std::uint32_t>(message.param2);
        if (id == kTutorialUIExitButton) {
            exit_to_region();
        } else if (id >= kTutorialUIExitSave && id <= kTutorialUIExitCancel && exitDialog_) {
            if (cIGZWinMgr* manager = exitDialog_->GetWindowManager()) manager->EndDialog(0xf, exitDialog_, 1);
            exitDialog_->HideWindow();
            if (id == kTutorialUIExitSave) {   // leave for the region; the tutorial ends with the city
                task->EndTask();
                auto* finished = new cRZMessage2Standard();
                finished->AddRef();
                finished->SetType(kMsgTutorialFinished);
                finished->SetData1(lastTask_ ? 0 : 1);
                if (cIGZMessageServer2* messages = g_host ? g_host->Messages() : nullptr) messages->MessageSend(finished);
                if (g_host) {
                    g_host->PauseSimulator();
                    g_host->RequestGoToRegionView(false);
                }
                finished->Release();
            } else if (id == kTutorialUIExitFinish) {
                finish_tutorial(lastTask_);
            }
        } else if (html_) {   // a control with a link (the "next" button)
            if (cIGZWin* control = Window()->GetChildWindowFromIDRecursive(id)) {
                cIGZVariant* link = nullptr;
                if (control->GetParam(kTutorialUILinkParam, &link) && link) {
                    cRZString text;
                    if (link->GetValString(text)) html_->DoLinkJump(text);
                    link->Release();
                }
            }
        }
        task->Release();
        return true;
    }
    task->Release();
    return false;
}

// ================================================================ the game's events

bool cSC4TutorialUIWinProc::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    cISC4TutorialTask* task = GetCurrentTask();
    if (!task) return true;
    task->AddRef();
    const int kind = task->GetWhichFunction();
    bool next = false;
    switch (type) {
    case kTutorialUIMsgDisaster:   // a disaster ended: the disaster task moves on
        if (dispatchDone_) {
            if (standard->GetData1() == 1) {
                if (kind == kTaskDisaster) moveToNext();
                dispatchDone_ = false;
            }
        }
        break;
    case kTutorialUITick: {
        const std::uint64_t what = standard->GetData1();
        if (what == 2) {
            ShowUIHintsForUsers();
        } else if (what == 3) {   // point at a catalog item once the catalog has laid it out
            const std::uint64_t delay = standard->GetData3();
            auto* view = static_cast<cISC4WinCatalogView*>(standard->GetVoid4());
            if (delay == 0) {
                int x = 0, y = 0;
                const auto item = static_cast<std::uint32_t>(standard->GetData2());
                if (view) {
                    view->GetItemPositionByID(item, x, y);
                    view->FlashItemByID(item, 0x7fffffff, 200);
                }
                Assign(catalog_, view);
                DrawArrow(x, y, x);
            } else {
                standard->SetData3(delay - 1);
                if (cIGZMessageServer2* messages = g_host ? g_host->Messages() : nullptr)
                    messages->GeneralMessagePostToTarget(message, static_cast<cIGZMessageTarget2*>(this));
            }
        } else if (what == 4) {
            DrawArrow(static_cast<cIGZWin*>(standard->GetVoid4()));
        } else if (what == 1 && dispatchDone_ && kind == kTaskDisaster) {
            if (g_host && g_host->DisasterPlaying(kDisasterTutorial)) tick_self(1, 0, 0, nullptr, nullptr);
            else {
                dispatchDone_ = false;
                next = true;
            }
        }
        break;
    }
    case kTutorialUIMsgPopulation:
        if (kind == kTaskPopulation && g_host && g_host->ResidentialPopulation() > static_cast<int>(task->GetExtraData())) next = true;
        break;
    case kTutorialUIMsgZoned: {   // a zone was drawn over (void2) with zone type data3: it must cover the region
        auto* zone = static_cast<const std::int32_t*>(standard->GetVoid2());
        const auto zoneType = static_cast<std::int32_t>(standard->GetData3());
        if (kind != kTaskZone || zoneType < 1 || !zone) break;
        const SC4TutorialRect& area = task->GetRegion();
        if (area.x0 < zone[0] || zone[2] < area.x1 || zone[3] < area.z1 || area.z0 < zone[1] ||
            zoneType != static_cast<std::int32_t>(task->GetExtraData())) {
            if (zoneType == 0xf) break;
            task->ShowHintsForUsers();
            ShowTryAgain(task);
            break;
        }
        next = true;
        break;
    }
    case kTutorialUIMsgLot: {   // a lot was built (data2 6): it must be the region's lot
        if (kind != kTaskLot) break;
        SC4TutorialRect lot{};
        if (standard->GetData2() == 6 && g_host && g_host->LotBoundingRect(standard->GetIGZUnknown(), lot)) {
            const SC4TutorialRect& area = task->GetRegion();
            if (area.x0 == lot.x0 && area.z0 == lot.z0 && area.x1 == lot.x1 && area.z1 == lot.z1) next = true;
            else ShowTryAgain(task);
        }
        break;
    }
    case kTutorialUIMsgDispatch: {   // a dispatch (data2) was sent: it must land in the region
        if (standard->GetData1() == 1 && g_host) {
            float x = 0, z = 0;
            int cellX = 0, cellZ = 0;
            g_host->DispatchPosition(static_cast<std::uint32_t>(standard->GetData2()), x, z);
            g_host->PositionToCell(x, z, cellX, cellZ);
            if (!RectContains(task->GetRegion(), cellX, cellZ)) {
                task->ShowHintsForUsers();
                ShowTryAgain(task);
            } else {
                dispatchDone_ = true;
                tick_self(1, 0, 0, nullptr, nullptr);
                moveToNext();
            }
        }
        setInstructionText(cRZString(task->GetInstructionMsg()));   // quirk kept: the old task's text
        break;
    }
    case kTutorialUIMsgPlaced: {
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        auto* concrete = dynamic_cast<cSC4TutorialTask*>(task);
        if (!g_host || !occupant || !concrete) break;
        int x = 0, z = 0;
        if (kind == kTaskNetwork) {
            const int piece = g_host->NetworkPiece(occupant, task->GetExtraData(), concrete->TallerThanWide(), x, z);
            if (piece < 0) break;
            if (piece == 1 && concrete->checkGridDataJobFinished(x, z)) next = true;
            else ShowTryAgain(task);
        } else if (kind == kTaskPower) {
            if (!g_host->PowerPieceCell(occupant, x, z)) break;
            if (concrete->checkGridDataJobFinished(x, z)) next = true;
            else ShowTryAgain(task);
        } else if (kind == kTaskBuilding) {
            std::uint32_t buildingType = 0;
            if (!g_host->BuildingType(occupant, buildingType) || buildingType != task->GetTargetGUID()) break;
            float position[3] = {};
            occupant->GetPosition(position);
            if (IsRightBuildingPosition(position[0], position[2])) {
                next = true;
            } else {   // a building in the wrong place is bulldozed
                SC4CellRect cells{};
                occupant->GetBoundingCityCells(cells);
                g_host->DemolishCells(SC4TutorialRect{cells.x0, cells.z0, cells.x1, cells.z1});
                ShowTryAgain(task);
            }
        }
        break;
    }
    default:
        break;
    }
    if (next) moveToNext();
    task->Release();
    return true;
}

}  // namespace sc4
