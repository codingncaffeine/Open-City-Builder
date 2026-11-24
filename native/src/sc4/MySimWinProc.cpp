#include "sc4/MySimWinProc.h"

#include <algorithm>
#include <iterator>
#include <string>

#include "gz/RZServices.h"
#include "gz/WinUtils.h"
#include "rz/cRZVariant.h"
#include "sc4/GameDataRegistry.h"
#include "sc4/GameDataRegistryHelpers.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SLString.h"

namespace sc4 {

namespace {

IMySimWinProcHost* g_host = nullptr;

// ---- the static HTML fragments of updateBodyTextCtrl (all in the unit's BSS, initialised by its guard blocks) ---------
// The headers are the colour part only: kHeaderFont completes them with the face/size and the <B> tag, so a header is
// always `<FONT COLOR="#rrggbb"` + kHeaderFont.
constexpr const char* kExpiredPriorityHeader = "<FONT COLOR=\"#8e9499\"";
constexpr const char* kLowPriorityHeader = "<FONT COLOR=\"#3f4967\"";
constexpr const char* kMediumPriorityHeader = "<FONT COLOR=\"#09a225\"";
constexpr const char* kHighPriorityHeader = "<FONT COLOR=\"#b00000\"";
constexpr const char* kHighestPriorityHeader = "<BLINK><FONT COLOR=\"#f70a0b\"";
[[maybe_unused]] constexpr const char* kStandardPriorityHeader = "<FONT COLOR=\"#0f0f0f\"";
constexpr const char* kPriorityFooter = "</B></FONT>";
constexpr const char* kBodyFooter = "</I></FONT>";
constexpr const char* kHighestPriorityFooter = "</B></FONT></BLINK>";
constexpr const char* kHeaderFont = " FACE=\"StocletITC TT\" SIZE=3><B>";
constexpr const char* kBodyFont = "<FONT COLOR=\"#3f4967\" FACE=\"Arial CE\" SIZE=3><I>";
constexpr const char* kBodyFontDeactive = "<FONT COLOR=\"#8e9949\" FACE=\"Arial CE\" SIZE=3><I>";
// The four priority-severity images (alarm / bad / neutral / good x dot / right arrow / down arrow / x).
constexpr const char* kAlarmDot = "<IMG SRC=\"sc4://HTML/46a006b0/14416250\">";
constexpr const char* kAlarmRightArrow = "<IMG SRC=\"sc4://HTML/46a006b0/14416251\">";
constexpr const char* kAlarmDownArrow = "<IMG SRC=\"sc4://HTML/46a006b0/14416252\">";
constexpr const char* kAlarmX = "<IMG SRC=\"sc4://HTML/46a006b0/14416253\">";
constexpr const char* kBadDot = "<IMG SRC=\"sc4://HTML/46a006b0/14416254\">";
constexpr const char* kBadRightArrow = "<IMG SRC=\"sc4://HTML/46a006b0/14416255\">";
constexpr const char* kBadDownArrow = "<IMG SRC=\"sc4://HTML/46a006b0/14416256\">";
constexpr const char* kBadX = "<IMG SRC=\"sc4://HTML/46a006b0/14416257\">";
constexpr const char* kNeutralDot = "<IMG SRC=\"sc4://HTML/46a006b0/14416258\">";
constexpr const char* kNeutralRightArrow = "<IMG SRC=\"sc4://HTML/46a006b0/14416259\">";
constexpr const char* kNeutralDownArrow = "<IMG SRC=\"sc4://HTML/46a006b0/1441625A\">";
constexpr const char* kNeutralX = "<IMG SRC=\"sc4://HTML/46a006b0/1441625B\">";
constexpr const char* kGoodDot = "<IMG SRC=\"sc4://HTML/46a006b0/1441625C\">";
constexpr const char* kGoodRightArrow = "<IMG SRC=\"sc4://HTML/46a006b0/1441625D\">";
constexpr const char* kGoodDownArrow = "<IMG SRC=\"sc4://HTML/46a006b0/1441625E\">";
constexpr const char* kGoodX = "<IMG SRC=\"sc4://HTML/46a006b0/1441625F\">";
constexpr const char* kAutomataDefaultName = "(Automata needs a name!)";

cIGZWin* PanelWindow(gz::cIGZWinGen* panel) { return panel ? panel->AsIGZWin() : nullptr; }
cIGZWin* ControlsOf(gz::cIGZWinGen* panel) {
    cIGZWin* window = PanelWindow(panel);
    return window ? window->GetParentWin() : nullptr;
}

void Assign(cRZString& out, const cIGZString& in) { out.AssignFromChars(in.Data(), in.Strlen()); }

// The four display strings of one advice: header (with the font), body (with its font), the three icons and the footer.
struct AdviceBody {
    std::string header;
    std::string body;
    std::string dot;
    std::string rightArrow;
    std::string downArrow;
    std::string x;
    std::string footer = kPriorityFooter;
};

void SelectPriority(AdviceBody& body, int priority) {
    switch (priority) {
        case 0:   // highest
            body.footer = kHighestPriorityFooter;
            body.header = kHighestPriorityHeader;
            body.dot = kAlarmDot; body.rightArrow = kAlarmRightArrow;
            body.downArrow = kAlarmDownArrow; body.x = kAlarmX;
            break;
        case 1:   // high
            body.header = kHighPriorityHeader;
            body.dot = kBadDot; body.rightArrow = kBadRightArrow;
            body.downArrow = kBadDownArrow; body.x = kBadX;
            break;
        case 2:
        case 3:
        case 4:
        default:  // medium (also 6 and above)
            body.header = kMediumPriorityHeader;
            body.dot = kNeutralDot; body.rightArrow = kNeutralRightArrow;
            body.downArrow = kNeutralDownArrow; body.x = kNeutralX;
            break;
        case 5:   // low
            body.header = kLowPriorityHeader;
            body.dot = kGoodDot; body.rightArrow = kGoodRightArrow;
            body.downArrow = kGoodDownArrow; body.x = kGoodX;
            break;
    }
}

// The picture a slot shows when the sim has no bitmap of its own: "<MySim directory><bitmap file>".
cRZString PortraitPath(const cISC4MySim* sim) {
    cRZString path;
    std::string combined;
    if (g_host) {
        cRZString directory;
        if (g_host->MySimDirectory(directory)) combined.assign(directory.ToChar(), directory.Strlen());
    }
    const cRZString& file = sim->GetBitmapFile();
    combined.append(file.Data(), file.Strlen());
    path.AssignFromCString(combined.c_str());
    return path;
}

}  // namespace

IMySimWinProcHost* MySimWinProcHost() { return g_host; }
void SetMySimWinProcHost(IMySimWinProcHost* host) { g_host = host; }

// ---- the game-data callbacks -----------------------------------------------------------------------------------------

namespace {

// Both callbacks answer the value of "pop_select_{car,pedestrian}_dialog": reading it (write == false) with a context
// posts the matching "model chosen" message and reports true; writing it only checks the handle.
bool SelectModel(void* handle, cIGZVariant& value, bool write, cISC4GameDataRegistry* registry, std::uint32_t message) {
    if (!registry) return true;
    const EntryId entry = registry->GetEntryID(handle);           // vslot 0x48
    void* context = registry->GetEntryContext(entry);             // vslot 0x90
    if (context && !write) {
        if (g_host) g_host->PostMessage2(message, 0, 0, 0, 0, nullptr);
        value.SetValBool(true);                                   // vslot 0x58
    }
    return true;
}

}  // namespace

bool s_sim_select_car(void* handle, cIGZVariant& value, bool write, cISC4GameDataRegistry* registry) {
    return SelectModel(handle, value, write, registry, kMySimMsgCarChosen);
}

bool s_sim_select_pedestrian(void* handle, cIGZVariant& value, bool write, cISC4GameDataRegistry* registry) {
    return SelectModel(handle, value, write, registry, kMySimMsgPedestrianChosen);
}

// ---- life cycle ------------------------------------------------------------------------------------------------------

// @ 0x100315330. Stores the panel (taking a reference), clears the five slots and the two lists and points the message
// list's sentinel at itself.
cSC4MySimWinProc::cSC4MySimWinProc(gz::cIGZWinGen* panel) : panel_(panel) {
    if (panel_) panel_->AddRef();
}

// @ 0x100315536. Releases the transport exemplar, destroys the advice map and list, drops the five sims and then the
// panel / advice list / unknown slots. (The original does not release +0x60/+0x68 here: Shutdown does.)
cSC4MySimWinProc::~cSC4MySimWinProc() {
    if (transportExemplar_) { transportExemplar_->Release(); transportExemplar_ = nullptr; }
    messageMap_.clear();
    messages_.clear();
    for (int i = 4; i >= 0; --i) {
        if (sims_[i]) { sims_[i]->Release(); sims_[i] = nullptr; }
    }
    if (adviceList_) { adviceList_->Release(); adviceList_ = nullptr; }
    if (unknown28_) { unknown28_->Release(); unknown28_ = nullptr; }
    if (panel_) { panel_->Release(); panel_ = nullptr; }
}

// @ 0x1003160f6: 0x452294aa -> the +8 cIGZMessageTarget2 view, 0x22e85d8e -> the +0 cIGZWinProc view, everything else
// (including iid 1) to the cRZUnknown at +0x10.
bool cSC4MySimWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    if (iid == gz::kGZIID_cIGZWinProc) {
        *out = static_cast<gz::cIGZWinProc*>(this);
        AddRef();
        return true;
    }
    return cRZUnknown::QueryInterface(iid, out);
}

// @ 0x10031571e. Hides every page, points the seven page notification targets at the panel, resets the five slot
// portraits to the placeholder image, subscribes to the six messages, and registers the "mysims_ui" node plus the two
// model-selection values in the game-data registry.
void cSC4MySimWinProc::Init() {
    cIGZWin* panel = PanelWindow(panel_);
    cIGZWin* controls = ControlsOf(panel_);
    simCount_ = 0;
    if (controls) {
        nsGZWinUtils::SetWindowVisible(controls, kMySimPanelWindow, false, false);
        SetDialogState(0);
        nsGZWinUtils::SetWindowVisible(controls, kMySimMainWindow, false, false);
        if (panel) {
            nsGZWinUtils::SetWindowNotificationTargetRecursive(controls, kMySimDialogMain, panel);
            nsGZWinUtils::SetWindowNotificationTargetRecursive(controls, kMySimDialogLarge, panel);
            nsGZWinUtils::SetWindowNotificationTargetRecursive(controls, kMySimDialogStat, panel);
            nsGZWinUtils::SetWindowNotificationTargetRecursive(controls, kMySimDialogSmall, panel);
            nsGZWinUtils::SetWindowNotificationTargetRecursive(controls, kMySimDialogTransport, panel);
            nsGZWinUtils::SetWindowNotificationTargetRecursive(controls, kMySimDialogStatAlt, panel);
            nsGZWinUtils::SetWindowNotificationTargetRecursive(controls, kMySimMainWindow, panel);
        }
        cSLBuffer placeholder(0, kMySimPortraitImageGroup, kMySimPlaceholderPortrait, 1, false);
        for (int slot = 0; slot < 5; ++slot) {
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameBitmaps[slot], false, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtons[slot], false, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtonsNew[slot], false, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtonsEmpty[slot], true, false);
            gz::cIGZWinBMP* portrait = nullptr;
            if (controls->GetChildAsRecursive(kMySimNameBitmapBase + slot, gz::kGZIID_cIGZWinBMP,
                                              reinterpret_cast<void**>(&portrait))) {
                portrait->SetBMP(placeholder.GetBuffer());
                portrait->Release();
            }
        }
    }

    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        static const std::uint32_t kMessages[] = {kMessageMySimsChanged, kMySimMsgAdviceChanged, kMySimMsgAdviceAdded,
                                                  kMySimMsgAdviceRemoved, kMySimMsgCarChosen, kMySimMsgPedestrianChosen};
        for (std::uint32_t message : kMessages) server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
    }

    if (cISC4GameDataRegistry* registry = g_host ? g_host->GameDataRegistry() : nullptr) {
        nsISC4GameDataRegistry::AddNode<cSC4MySimWinProc, cIGZUnknown>(this, "mysims_ui", nullptr, 0, nullptr, registry);
        nsISC4GameDataRegistry::AddValue(nullptr, "pop_select_car_dialog", &s_sim_select_car, this, 2, this, registry);
        nsISC4GameDataRegistry::AddValue(nullptr, "pop_select_pedestrian_dialog", &s_sim_select_pedestrian, this, 2,
                                         this, registry);
    }
}

// @ 0x100315b20: shows only the page whose id matches, every other page is hidden.
void cSC4MySimWinProc::SetDialogState(std::uint32_t state) {
    cIGZWin* controls = ControlsOf(panel_);
    if (!controls) return;
    nsGZWinUtils::SetWindowVisible(controls, kMySimDialogMain, state == kMySimDialogMain, false);
    nsGZWinUtils::SetWindowVisible(controls, kMySimDialogSmall, state == kMySimDialogSmall, false);
    nsGZWinUtils::SetWindowVisible(controls, kMySimDialogLarge, state == kMySimDialogLarge, false);
    nsGZWinUtils::SetWindowVisible(controls, kMySimDialogStat, state == kMySimDialogStat, false);
    nsGZWinUtils::SetWindowVisible(controls, kMySimDialogStatAlt, state == kMySimDialogStatAlt, false);
    nsGZWinUtils::SetWindowVisible(controls, kMySimDialogTransport, state == kMySimDialogTransport, false);
}

// @ 0x100315eb2. Clears the advice list, removes the "mysims_ui" registry node, unsubscribes, destroys the panel window,
// releases everything, then drops the reference it took on entry (net zero: the handler's own reference keeps the object
// alive, View3DWin's RemoveGUI is what releases it).
bool cSC4MySimWinProc::Shutdown() {
    AddRef();
    if (adviceList_) adviceList_->ClearList();
    if (cISC4GameDataRegistry* registry = g_host ? g_host->GameDataRegistry() : nullptr) {
        cIGZUnknown* node = nullptr;
        if (QueryInterface(1, reinterpret_cast<void**>(&node)) && node) {
            registry->RemoveChildEntry(node);
            node->Release();
        }
    }
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        static const std::uint32_t kMessages[] = {kMessageMySimsChanged, kMySimMsgAdviceChanged, kMySimMsgAdviceAdded,
                                                  kMySimMsgAdviceRemoved, kMySimMsgCarChosen, kMySimMsgPedestrianChosen};
        for (std::uint32_t message : kMessages) {
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), message);
        }
    }
    if (cIGZWin* panel = PanelWindow(panel_)) {
        if (cIGZWinMgr* manager = panel->GetWindowManager()) manager->DestroyWindow(panel);
    }
    if (panel_) { panel_->Release(); panel_ = nullptr; }
    if (unknown28_) { unknown28_->Release(); unknown28_ = nullptr; }
    if (adviceList_) { adviceList_->Release(); adviceList_ = nullptr; }
    for (int i = 0; i < 5; ++i) {
        if (sims_[i]) { sims_[i]->Release(); sims_[i] = nullptr; }
    }
    if (pickSim_) {
        pickSim_->Shutdown();
        pickSim_->Release();
        pickSim_ = nullptr;
    }
    if (agentSim_) { agentSim_->Release(); agentSim_ = nullptr; }
    Release();
    return true;
}

// ---- command handling ------------------------------------------------------------------------------------------------

// @ 0x100316192. The panel's own commands select a sim slot / a page or switch the small, large and stat layouts; the
// transport dialog's commands filter the model icons, change the page and end the dialog. Panel commands answer false
// (they keep propagating), dialog commands true.
bool cSC4MySimWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    cIGZWin* panel = PanelWindow(panel_);
    if (panel && window == panel) {
        if (message.type == kGZMsgCommand) {
            if (message.param1 != kMySimCommandSender) return false;
            if (agentSim_) agentSim_->CancelMoveIn();   // 0x1003162f2: every panel command ends a pending move-in
            int target = -1;
            bool select = false;
            bool stat = false;
            bool small = false;
            bool large = false;
            switch (static_cast<std::uint32_t>(message.param2)) {
                case 0x2a1f1f24: case 0xaa1f1e85:
                    mode_ = 0;
                    SetMySimModeWindow();
                    break;
                case 0x2bbaaa68:
                    ShowChooseTransportDialog(false);
                    break;
                case 0x2bbaaa79:
                    if (g_host && sims_[currentSim_]) g_host->CreateChangeHouseTool(sims_[currentSim_]);
                    break;
                case 0x4a1f1dea: case 0xca9df380:
                    select = true; target = 4;
                    break;
                case 0x4bba8aa1: {
                    // The original dereferences the current sim here without a check; the port skips a missing one.
                    SetDialogState(kMySimDialogTransport);
                    cRZString name;
                    if (cISC4MySim* sim = sims_[currentSim_]) {
                        Assign(name, sim->AsAdvisor()->GetPersonalName());
                        if (cIGZWin* controls = ControlsOf(panel_)) {
                            nsGZWinUtils::SetWindowCaption(controls, kMySimTransportCaption, name);
                        }
                    }
                    break;
                }
                case 0x4bbaaa50:
                    if (sims_[currentSim_]) sims_[currentSim_]->CreateDispatchTool();
                    break;
                case 0x6a1f1de0: case 0xca9df381:
                    select = true; target = 3;
                    break;
                case 0x6a57da68: case 0xaa1f2ea: case 0xaa1f2eb: case 0xea628abc:
                    stat = true;
                    break;
                case 0x6bbaaa5d:
                    if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) {
                        if (panel) manager->GZSetFocus(panel);
                    }
                    if (sims_[currentSim_]) sims_[currentSim_]->EnterUDriveIt();
                    break;
                case 0x8a1f1dc3: case 0x8a8f410c: case 0xca9df384:
                    select = true; target = 0;
                    break;
                case 0x8a1f1dcc: case 0x8a8f410d: case 0xca9df383:
                    select = true; target = 1;
                    break;
                case 0x8a1f1e93: case 0xaa90b2c9:
                    large = true;
                    break;
                case 0x8a8f410e: case 0xaa1f1dd6: case 0xca9df382:
                    select = true; target = 2;
                    break;
                case 0x8a8f410f:
                    select = true; target = 3;
                    break;
                case 0x8a8f4120:
                    select = true; target = 4;
                    break;
                case 0x8bbaaa80:
                    if (g_host && sims_[currentSim_]) g_host->CreateChangeJobTool(sims_[currentSim_]);
                    break;
                case 0xaa1d9734: case 0xaa90b2ca:
                    small = true;
                    break;
                case 0xaa1f1e96:
                    SetMySimMainWindow();
                    break;
                case 0xaa1f1ea3:
                    SetDialogState(kMySimDialogStat);
                    break;
                case 0xca1f1e9b:
                    if (sims_[currentSim_]) sims_[currentSim_]->Find();
                    break;
                case 0xea57da69:
                    BootSim();
                    break;
                case 0xea628abd: case 0xea628abe:
                    if (large_ == 0) small = true; else large = true;
                    break;
                case 0xebbaaa70:
                    ShowChooseTransportDialog(true);
                    break;
                default:
                    break;
            }
            if (select) SetCurrentSim(target, true, true);
            if (stat) SetMySimStatWindow();
            if (small) SetMySimSmallWindow();
            if (large) SetMySimLargeWindow();
        } else if (message.type == kMySimMsgForwardToAdviceList) {
            if (adviceList_) {
                if (cIGZWin* body = adviceList_->AsIGZWin()) body->DoMessage(message);
            }
        }
        return false;
    }

    if (message.type != kGZMsgCommand || message.param1 != kMySimCommandSender) return false;
    const std::uint32_t id = static_cast<std::uint32_t>(message.param2);
    bool displayIcons = false;
    bool handled = true;
    // The icon buttons come first; the six page buttons jump straight to the refresh. Everything else (the wealth filter
    // and the dialog's OK / cancel buttons) shares the "common" dispatch the original reaches through LAB_100316325.
    if (id >= kMySimIconButtonBase && id <= 0x3456001b) {
        selectedIconButton_ = id;
        nsGZWinUtils::ExcludeButtonByRange(window, kMySimIconButtonBase, iconsPerPage_, id);
    } else if (id - kMySimPageButtonBase - 1 <= 5) {
        iconPage_ = iconPageOffset_ + static_cast<std::int32_t>(id - kMySimPageButtonBase - 1);
        displayIcons = true;
    } else if (id - kMySimWealthButtonBase < 3) {
        wealthFilter_ = id - kMySimWealthButtonBase;
        nsGZWinUtils::ExcludeButtonByRange(window, kMySimWealthButtonBase, 3, id);
        displayIcons = true;
        handled = false;   // the wealth buttons let the command propagate
    } else if (id == 0x2bf3269b || id == 0xabf326da) {
        if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) manager->EndDialog(0xf, window, 0);
    } else if (id == 0x6bf326d1) {
        if (cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr) manager->EndDialog(0xf, window, 1);
    } else if (id == 0x6bfafa68) {
        --iconPage_;
        displayIcons = true;
    } else if (id == 0x6bfafa6e) {
        ++iconPage_;
        displayIcons = true;
    } else {
        handled = false;
    }
    if (displayIcons) DisplayVehicleIcons(window);
    return handled;
}

// @ 0x100316900. Fills the transport dialog's model icon buttons. The models come from the vehicle (or pedestrian)
// manager, filtered by the wealth button (vehicles) or the sim's sex (pedestrians), sorted by the model's sort-order
// property; page `iconPage_` shows `iconsPerPage_` of them and the six page buttons are captioned 1..6 from
// `iconPageOffset_`.
void cSC4MySimWinProc::DisplayVehicleIcons(cIGZWin* dialog) {
    if (!dialog) return;
    IModelManager* manager = nullptr;
    if (g_host) manager = pedestrianFilter_ ? g_host->PedestrianManager() : g_host->VehicleManager();
    std::map<int, cISCResExemplar*> bySortOrder;
    bool male = false;
    if (sims_[currentSim_]) male = sims_[currentSim_]->IsMale();   // 0x90
    if (manager) {
        std::vector<cISCResExemplar*> models;
        manager->ModelExemplars(models);
        for (cISCResExemplar* model : models) {
            if (!model) continue;
            cISCPropertyHolder* holder = model->AsISCPropertyHolder();   // 0x30
            if (!holder) continue;
            std::int32_t key = 0;
            if (!cSCPropertyHelp::GetPropertyValue(holder, kMySimPropertyWealth, key)) continue;
            if (!pedestrianFilter_) {
                if (static_cast<std::uint32_t>(key) != wealthFilter_) continue;
            } else {
                std::int32_t maleProperty = 0;
                if (cSCPropertyHelp::GetPropertyValue(holder, kMySimPropertyMale, maleProperty)) {
                    if (male != (maleProperty == 0)) continue;
                }
            }
            std::int32_t sortOrder = 0;
            cSCPropertyHelp::GetPropertyValue(holder, kMySimPropertySortOrder, sortOrder);
            bySortOrder[sortOrder] = model;   // the original inserts one node per model (duplicate keys collapse here)
        }
    }

    const int perPage = static_cast<int>(iconsPerPage_);
    const int total = static_cast<int>(bySortOrder.size());
    int pageCount = perPage != 0 ? (total - 1 + perPage) / perPage : 0;
    if (pageCount == 0) pageCount = 1;
    const int wanted = iconPage_ < 0 ? 0 : iconPage_;
    int selected = pageCount - 1;
    if (wanted < pageCount) selected = wanted;
    totalIcons_ = static_cast<std::uint32_t>(total);
    if (iconPage_ < 0 || pageCount <= wanted) iconPage_ = selected;
    int first = selected * perPage;
    if (total < first) first = total;

    auto it = bySortOrder.begin();
    std::advance(it, first);
    int shown = 0;
    for (; shown < perPage && it != bySortOrder.end(); ++shown, ++it) {
        cISCResExemplar* model = it->second;
        cISCPropertyHolder* holder = model ? model->AsISCPropertyHolder() : nullptr;
        if (!holder) continue;
        gz::cIGZWinBtn* button = nullptr;
        if (dialog->GetChildAsRecursive(kMySimIconButtonBase + static_cast<std::uint32_t>(shown), gz::kGZIID_cIGZWinBtn,
                                        reinterpret_cast<void**>(&button))) {
            button->AsIGZWin()->ShowWindow();
            cRZString name;
            if (!cSCPropertyHelp::GetPropertyValue(holder, kMySimPropertyName, name) &&
                !(g_host && g_host->ExemplarString(name, holder, kMySimPropertyNameAlt)) &&
                !cSCPropertyHelp::GetPropertyValue(holder, kMySimPropertyNameAlt2, name)) {
                name.AssignFromCString(kAutomataDefaultName);
            }
            button->SetTipText(name);   // 0x118
            // The exemplar rides along in the button as an object param; GetParam hands it back when the dialog closes.
            cRZVariant* variant = new cRZVariant(static_cast<cIGZUnknown*>(model));
            button->AsIGZWin()->SetParam(kMySimExemplarParam, variant);   // 0x390
            button->Release();
        }
        gz::cIGZWinBMP* bitmap = nullptr;
        if (dialog->GetChildAsRecursive(kMySimIconBitmapBase + static_cast<std::uint32_t>(shown), gz::kGZIID_cIGZWinBMP,
                                        reinterpret_cast<void**>(&bitmap))) {
            bitmap->AsIGZWin()->ShowWindow();
            std::uint32_t bitmapId = 0;
            if (cSCPropertyHelp::GetPropertyValue(holder, kMySimPropertyBitmap, bitmapId)) {
                cSLBuffer image(0, kMySimIconImageGroup, bitmapId, 1, false);
                if (image.GetBuffer()) bitmap->SetBMP(image.GetBuffer());
            }
            bitmap->Release();
        }
    }
    while (shown < static_cast<int>(iconsPerPage_)) {
        nsGZWinUtils::SetWindowVisible(dialog, kMySimIconButtonBase + static_cast<std::uint32_t>(shown), false, false);
        nsGZWinUtils::SetWindowVisible(dialog, kMySimIconBitmapBase + static_cast<std::uint32_t>(shown), false, false);
        ++shown;
    }

    iconPageOffset_ = 0;
    if (iconPage_ > 5) iconPageOffset_ = iconPage_ - 5;
    for (std::uint32_t page = 1; page <= 6; ++page) {
        cRZString text;
        text.Sprintf("%d", iconPageOffset_ + static_cast<int>(page));
        gz::cIGZWinBtn* button = nullptr;
        if (dialog->GetChildAsRecursive(kMySimPageButtonBase + page, gz::kGZIID_cIGZWinBtn,
                                        reinterpret_cast<void**>(&button))) {
            cIGZWin* win = button->AsIGZWin();
            win->SetCaption(text);                                             // 0x250
            win->SetFlag(kWinEnabled, iconPageOffset_ + static_cast<int>(page) - 1 < pageCount);   // 0x220
            const std::uint32_t color =
                (iconPageOffset_ + static_cast<int>(page) - 1 == iconPage_) ? 0xffffffffu : 0xff0000ffu;
            button->SetFontColor(0, gz::cRZColor{color});                      // 0xc0
            button->Release();
        }
    }
}

// ---- the panel pages -------------------------------------------------------------------------------------------------

// @ 0x100317168: switches to the main page, refreshes its four captions from the current sim's personal name and, when a
// new sim appeared, re-selects the previous index.
void cSC4MySimWinProc::SetMySimMainWindow() {
    cIGZWin* controls = ControlsOf(panel_);
    SetDialogState(kMySimDialogMain);
    const int oldIndex = static_cast<int>(currentSim_);
    const std::uint32_t oldCount = simCount_;
    GetCurrentSims();
    cISC4MySim* sim = sims_[currentSim_];
    if (!sim || !sim->IsActive()) return;
    cRZString name;
    Assign(name, sim->AsAdvisor()->GetPersonalName());
    if (controls) {
        nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption, name);
        nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption2, name);
        nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption3, name);
        nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption4, name);
    }
    if (oldCount < simCount_) SetCurrentSim(oldIndex, true, false);
}

// @ 0x100317338: shows the main frame, refreshes the captions and rebuilds whichever page the mode selects, then loads
// the large portrait (either the sim's bitmap or "<city name><bitmap file>").
void cSC4MySimWinProc::SetMySimModeWindow() {
    cIGZWin* controls = ControlsOf(panel_);
    if (!controls) return;
    nsGZWinUtils::SetWindowVisible(controls, kMySimMainWindow, true, false);
    cISC4MySim* sim = sims_[currentSim_];
    if (!sim) return;
    cRZString name;
    Assign(name, sim->AsAdvisor()->GetPersonalName());
    nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption, name);
    nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption2, name);
    nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption3, name);
    nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption4, name);

    if (mode_ == 1) {
        if (large_ == 0) SetMySimSmallWindow(); else SetMySimLargeWindow();
    } else if (mode_ == 2) {
        SetMySimStatWindow();
    } else {
        SetDialogState(0);
    }

    const std::uint32_t bitmapId = sim->GetBitmapID();   // 0xb0
    gz::cIGZWinBMP* portrait = nullptr;
    if (bitmapId == 0) {
        cRZString path = PortraitPath(sim);
        cSLBuffer image(path, 8, false);
        if (controls->GetChildAsRecursive(kMySimPortrait, gz::kGZIID_cIGZWinBMP, reinterpret_cast<void**>(&portrait))) {
            portrait->SetBMP(image.GetBuffer());
            portrait->Release();
        }
    } else {
        cSLBuffer image(0, kMySimPortraitImageGroup, bitmapId, 1, false);
        if (controls->GetChildAsRecursive(kMySimPortrait, gz::kGZIID_cIGZWinBMP, reinterpret_cast<void**>(&portrait))) {
            portrait->SetBMP(image.GetBuffer());
            portrait->Release();
        }
    }
}

// @ 0x1003176de: the large page. Its advice list sits under the 0xaa1f1ec5 page; the current sim's advisor supplies the
// message list and the body text is rebuilt from it.
void cSC4MySimWinProc::SetMySimLargeWindow() {
    cIGZWin* controls = ControlsOf(panel_);
    if (!controls) return;
    SetDialogState(kMySimDialogLarge);
    if (cIGZWin* page = controls->GetChildWindowFromIDRecursive(kMySimDialogLarge)) {
        if (adviceList_) { adviceList_->Release(); adviceList_ = nullptr; }
        page->GetChildAsRecursive(kMySimBodyLarge, kGZIID_cIGZWinAdviceList, reinterpret_cast<void**>(&adviceList_));
    }
    cISC4Advisor* advisor = sims_[currentSim_] ? sims_[currentSim_]->AsAdvisor() : nullptr;
    if (advisor) advisor->AddRef();
    GetMessageList(advisor);
    if (advisor) advisor->Release();
    updateBodyTextCtrl();
    mode_ = 1;
    large_ = 1;
}

// @ 0x1003177ec: the small page, same body handling as the large one (the list control id differs).
void cSC4MySimWinProc::SetMySimSmallWindow() {
    cIGZWin* controls = ControlsOf(panel_);
    if (!controls) return;
    SetDialogState(kMySimDialogSmall);
    if (cIGZWin* page = controls->GetChildWindowFromIDRecursive(kMySimDialogSmall)) {
        if (adviceList_) { adviceList_->Release(); adviceList_ = nullptr; }
        page->GetChildAsRecursive(kMySimBodySmall, kGZIID_cIGZWinAdviceList, reinterpret_cast<void**>(&adviceList_));
    }
    cISC4Advisor* advisor = sims_[currentSim_] ? sims_[currentSim_]->AsAdvisor() : nullptr;
    if (advisor) advisor->AddRef();
    GetMessageList(advisor);
    if (advisor) advisor->Release();
    updateBodyTextCtrl();
    mode_ = 1;
    large_ = 0;
}

// @ 0x1003178f4: enables the "boot" button, removes the current sim from the city and returns to the main page.
void cSC4MySimWinProc::BootSim() {
    if (cIGZWin* controls = ControlsOf(panel_)) {
        if (cIGZWin* button = controls->GetChildWindowFromIDRecursive(0xea1f1dba)) {
            button->SetFlag(kWinEnabled, true);   // 0x220
        }
    }
    if (sims_[currentSim_]) sims_[currentSim_]->DeleteFromGame();   // 0x48
    SetMySimMainWindow();
}

// @ 0x100317956: the stats page: wealth as one § per level, zodiac sign, age, home, job title, job, EQ/HQ and the bio.
void cSC4MySimWinProc::SetMySimStatWindow() {
    cIGZWin* controls = ControlsOf(panel_);
    if (!controls) return;
    SetDialogState(kMySimDialogStatAlt);
    cISC4MySim* sim = sims_[currentSim_];
    if (!sim || !sim->IsActive()) return;
    cRZString header;
    Assign(header, sim->AsAdvisor()->GetPersonalName());
    nsGZWinUtils::SetWindowCaption(controls, kMySimNameCaption4, header);

    // The wealth pips: one § for wealth 1, two for 2 and three for 3 (none for 0).
    cRZString wealth;
    wealth.AssignFromCString("\xc2\xa7");
    std::string pips;
    const int level = sim->GetWealth();   // 0x98
    if (level >= 1 && level <= 3) {
        for (int i = 0; i < level; ++i) pips.append(wealth.ToChar(), wealth.Strlen());
    }
    cRZString pipText;
    pipText.AssignFromCString(pips.c_str());
    nsGZWinUtils::SetWindowCaption(controls, kMySimStatWealth, pipText);

    const std::uint8_t sign = sim->GetSign();   // 0x80
    cSLString signName(kMySimSignGroup, kMySimSigns[sign % 12], true);
    nsGZWinUtils::SetWindowCaption(controls, kMySimStatSign, signName);

    cRZString age;
    age.Sprintf("%d", static_cast<int>(sim->GetAge()));   // 0x88
    nsGZWinUtils::SetWindowCaption(controls, kMySimStatAge, age);

    nsGZWinUtils::SetWindowCaption(controls, kMySimStatHome, sim->GetHomeName());       // 0xc8
    nsGZWinUtils::SetWindowCaption(controls, kMySimStatJobTitle, sim->GetJobTitle());   // 0xd8
    nsGZWinUtils::SetWindowCaption(controls, kMySimStatJob, sim->GetJobName());         // 0xd0

    cRZString text;
    if (sim->GetEQAsText(text)) nsGZWinUtils::SetWindowCaption(controls, kMySimStatEQ, text);    // 0xe0
    text = cRZString();
    if (sim->GetHQAsText(text)) nsGZWinUtils::SetWindowCaption(controls, kMySimStatHQ, text);    // 0xe8
    nsGZWinUtils::SetEditText(controls, kMySimStatBio, sim->GetBio());                        // 0xa8
    mode_ = 2;
}

// @ 0x100317eb8. Runs the model chooser dialog from the script window {0, 0x96a006b0, instance}: it sets the wealth
// filter buttons, fills and pages the model icons, restores/saves the dialog position and, when the modal loop answers 1,
// applies the chosen exemplar to the sim (car or pedestrian) and updates the transport button image.
void cSC4MySimWinProc::ShowChooseTransportDialog(bool pedestrian) {
    if (transportExemplar_) { transportExemplar_->Release(); transportExemplar_ = nullptr; }
    pedestrianFilter_ = pedestrian;
    cISC4MySim* sim = sims_[currentSim_];
    if (!sim) return;
    sim->AddRef();
    cSC4AutoGamePause pause(false, true, true, true);
    cGZPersistResourceKey key{0, kMySimPortraitImageGroup,
                              pedestrian ? kMySimTransportWindowPedestrians : kMySimTransportWindowVehicles};
    gz::cIGZWinGen* dialog = nullptr;
    cIGZWin* controls = ControlsOf(panel_);
    if (g_host && g_host->CreateTransportDialog(key, controls, kMySimTransportDialogId, gz::kGZIID_cIGZWinGen,
                                                reinterpret_cast<void**>(&dialog))) {
        cIGZWin* dialogWindow = dialog->AsIGZWin();
        dialog->SetWinProc(this);   // 0x38
        cRZString name;
        Assign(name, sim->AsAdvisor()->GetPersonalName());
        if (controls) nsGZWinUtils::SetWindowCaption(controls, kMySimTransportCaption2, name);
        if (dialogWindow && dialogWindow->GetChildWindowFromIDRecursive(kMySimWealthButtonBase)) {
            const int wealth = sim->GetWealth();
            nsGZWinUtils::SetWindowEnabled(dialogWindow, kMySimWealthButtonBase, true);
            nsGZWinUtils::SetWindowEnabled(dialogWindow, kMySimWealthButtonBase + 1, 1 < wealth);
            nsGZWinUtils::SetWindowEnabled(dialogWindow, kMySimWealthButtonBase + 2, 2 < wealth);
            int filter = static_cast<int>(wealthFilter_);
            if (wealth <= filter) {
                filter = wealth - 1;
                wealthFilter_ = static_cast<std::uint32_t>(filter);
            }
            nsGZWinUtils::ExcludeButtonByRange(dialogWindow, kMySimWealthButtonBase, 3,
                                               kMySimWealthButtonBase + static_cast<std::uint32_t>(filter));
        }
        if (dialogWindow) nsGZWinUtils::SetWindowNotificationTargetRecursive(dialogWindow, dialogWindow);
        iconPage_ = 0;
        iconsPerPage_ = pedestrian ? kMySimIconsPerPagePedestrians : kMySimIconsPerPageVehicles;
        DisplayVehicleIcons(dialogWindow);
        selectedIconButton_ = kMySimIconButtonBase;
        nsGZWinUtils::ExcludeButtonByRange(dialogWindow, kMySimIconButtonBase, iconsPerPage_, kMySimIconButtonBase);
        if (g_host) g_host->RestoreWindowLocation(dialogWindow);           // 0x48
        cIGZWinMgr* manager = g_host ? g_host->WindowManager() : nullptr;
        const int result = manager ? manager->RunPopup(dialogWindow) : 0;   // 0x148
        if (g_host) g_host->SaveWindowLocation(dialogWindow);               // 0x28
        if (result != 0) {
            gz::cIGZWinBtn* button = nullptr;
            if (dialogWindow->GetChildAsRecursive(selectedIconButton_, gz::kGZIID_cIGZWinBtn,
                                                  reinterpret_cast<void**>(&button))) {
                cIGZVariant* variant = nullptr;
                if (button->AsIGZWin()->GetParam(kMySimExemplarParam, &variant)) {   // 0x388
                    cISCResExemplar* exemplar = nullptr;
                    if (variant->GetValUnknown(kMySimTransportExemplarIid, reinterpret_cast<void**>(&exemplar))) {   // 0x1f0
                        if (transportExemplar_) transportExemplar_->Release();
                        transportExemplar_ = exemplar;
                        if (sim->IsActive()) {   // 0xa0
                            // The original ignores the SetCar / SetPedestrian answer and always refreshes the button
                            // image from the exemplar's bitmap property (kept).
                            if (pedestrian) sim->SetPedestrian(transportExemplar_);
                            else sim->SetCar(transportExemplar_);
                            cISCPropertyHolder* holder = transportExemplar_->AsISCPropertyHolder();
                            std::uint32_t bitmapId = 0;
                            if (holder && cSCPropertyHelp::GetPropertyValue(holder, kMySimPropertyBitmap, bitmapId)) {
                                SetTransportButtonImage(bitmapId, pedestrian);
                            }
                        }
                    }
                    variant->Release();   // the original keeps the reference GetParam took; the port drops it
                }
                button->Release();
            }
        }
        if (manager) manager->DestroyWindow(dialogWindow);   // 0xb8
        dialog->Release();
    }
    sim->Release();
}

// @ 0x1003183e0: makes `index` the current sim, tells the agent simulator which panel sim is active and refreshes the
// two transport button images from the sim's model bitmap ids.
void cSC4MySimWinProc::SetCurrentSim(int index, bool showMode, bool addIfMissing) {
    if (static_cast<std::uint32_t>(index) >= 5) return;
    currentSim_ = static_cast<std::uint32_t>(index);
    if (showMode && sims_[index]) {
        SetMySimModeWindow();
    } else if (addIfMissing && !sims_[index]) {
        AddSim(static_cast<std::uint32_t>(index));
    }
    if (!agentSim_) {
        agentSim_ = g_host ? g_host->MySimAgentSimulator() : nullptr;
        if (agentSim_) agentSim_->AddRef();
    }
    if (!agentSim_) return;
    cISC4MySim* sim = sims_[currentSim_];
    if (!sim) {
        agentSim_->SetActiveUIPanelSim(-1);   // 0x58
        return;
    }
    agentSim_->SetActiveUIPanelSim(static_cast<int>(currentSim_));
    SetTransportButtonImage(sim->GetModelButtonBitmapID(true), true);    // 0x1d8
    SetTransportButtonImage(sim->GetModelButtonBitmapID(false), false);
}

// @ 0x1003184ac. Reacts to the advisor / MySim message traffic: advice added / changed / removed refresh the body text;
// "my sims changed" re-reads the five slots (and falls back to the main page); the two model-selection messages feed the
// panel's own command handler.
bool cSC4MySimWinProc::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (type == kMySimMsgAdviceAdded) {
        cISC4Advice* advice = nullptr;
        cIGZUnknown* payload = static_cast<cIGZMessage2Standard*>(message)->GetIGZUnknown();   // 0x100
        if (payload) payload->QueryInterface(kMySimAdviceIid, reinterpret_cast<void**>(&advice));
        if (advice && sims_[currentSim_]) {
            if (advice->GetAdvisorID() == sims_[currentSim_]->AsAdvisor()->GetID()) AddMessage(advice);
        }
        updateBodyTextCtrl();
        if (advice) advice->Release();
        return true;
    }
    if (type == kMySimMsgAdviceChanged) {
        updateBodyTextCtrl();
        return true;
    }
    if (type == kMessageMySimsChanged) {
        if (!shown_) return true;
        const int oldIndex = static_cast<int>(currentSim_);
        const std::uint32_t oldCount = simCount_;
        GetCurrentSims();
        if (oldCount < simCount_) SetCurrentSim(oldIndex, true, false);
        if (sims_[currentSim_]) return true;
        SetMySimMainWindow();
        return true;
    }
    if (type == kMySimMsgAdviceRemoved) {
        if (!sims_[currentSim_]) return true;
        cIGZUnknown* payload = static_cast<cIGZMessage2Standard*>(message)->GetIGZUnknown();
        if (!payload) return true;
        cISC4Advice* advice = nullptr;
        payload->QueryInterface(kMySimAdviceIid, reinterpret_cast<void**>(&advice));
        if (advice) {
            if (advice->GetAdvisorID() == sims_[currentSim_]->AsAdvisor()->GetID()) RemoveMessage(advice);
            advice->Release();
        }
        updateBodyTextCtrl();
        return true;
    }
    if (type == kMySimMsgCarChosen || type == kMySimMsgPedestrianChosen) {
        cGZMessage command;
        command.type = kGZMsgCommand;
        command.param1 = kMySimCommandSender;
        command.param2 = type == kMySimMsgCarChosen ? 0x2bbaaa68u : 0xebbaaa70u;
        DoWinProcMessage(PanelWindow(panel_), command);   // the original calls its own vtable slot 0x18
        return true;
    }
    return true;
}

// @ 0x10031873a. Releases the five slots, clears their name / portrait controls, then walks the agent simulator's active
// sims: registers each sim by its slot, shows that slot's controls, sets its name caption and portrait, and finally
// enables or disables the "boot" button.
void cSC4MySimWinProc::GetCurrentSims() {
    cIGZWin* controls = ControlsOf(panel_);
    simCount_ = 0;
    for (int i = 0; i < 5; ++i) {
        if (sims_[i]) { sims_[i]->Release(); sims_[i] = nullptr; }
    }
    if (controls) {
        cSLBuffer placeholder(0, kMySimPortraitImageGroup, kMySimPlaceholderPortrait, 1, false);
        for (int slot = 0; slot < 5; ++slot) {
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameBitmaps[slot], false, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtons[slot], false, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtonsNew[slot], false, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtonsEmpty[slot], true, false);
            gz::cIGZWinBMP* portrait = nullptr;
            if (controls->GetChildAsRecursive(kMySimNameBitmapBase + slot, gz::kGZIID_cIGZWinBMP,
                                              reinterpret_cast<void**>(&portrait))) {
                portrait->SetBMP(placeholder.GetBuffer());
                portrait->Release();
            }
        }
    }

    if (!agentSim_) {
        agentSim_ = g_host ? g_host->MySimAgentSimulator() : nullptr;
        if (agentSim_) agentSim_->AddRef();
    }
    if (!agentSim_) return;

    std::vector<cRZAutoRefCount<cISC4MySim>> sims;
    agentSim_->GetActiveMySims(sims);   // 0x28
    for (cRZAutoRefCount<cISC4MySim>& ref : sims) {
        cISC4MySim* sim = ref.get();
        if (!sim) continue;
        const std::uint8_t slot = sim->GetSlot();   // 0x50
        if (slot >= 5) continue;
        if (sims_[slot] != sim) {
            sim->AddRef();
            cISC4MySim* old = sims_[slot];
            sims_[slot] = sim;
            if (old) old->Release();
        }
        cRZString name;
        Assign(name, sim->AsAdvisor()->GetPersonalName());
        if (controls) {
            if (cIGZWin* text = controls->GetChildWindowFromIDRecursive(kMySimNameTexts[slot])) {
                text->SetCaption(name);   // 0x250
            }
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameBitmaps[slot], true, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtons[slot], true, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtonsNew[slot], true, false);
            nsGZWinUtils::SetWindowVisible(controls, kMySimNameButtonsEmpty[slot], false, false);
        }
        const std::uint32_t bitmapId = sim->GetBitmapID();   // 0xb0
        gz::cIGZWinBMP* portrait = nullptr;
        if (!controls) { /* nothing to fill */ }
        else if (bitmapId == 0) {
            cRZString path = PortraitPath(sim);
            cSLBuffer image(path, 8, false);
            if (controls->GetChildAsRecursive(kMySimNameBitmapBase + slot, gz::kGZIID_cIGZWinBMP,
                                              reinterpret_cast<void**>(&portrait))) {
                portrait->SetBMP(image.GetBuffer());
                portrait->Release();
            }
        } else {
            cSLBuffer image(0, kMySimPortraitImageGroup, bitmapId, 1, false);
            if (controls->GetChildAsRecursive(kMySimNameBitmapBase + slot, gz::kGZIID_cIGZWinBMP,
                                              reinterpret_cast<void**>(&portrait))) {
                portrait->SetBMP(image.GetBuffer());
                portrait->Release();
            }
        }
        ++simCount_;
    }
    if (controls) nsGZWinUtils::SetWindowEnabled(controls, 0xea1f1dba, simCount_ < 5);
    agentSim_->TrackLocation(nullptr);   // 0x38
}

// @ 0x100318e14. Rebuilds the advice list: one entry per shown advice, with its priority header, body, three icons and
// footer; an advice without the "current" flag 2 is drawn greyed out. Priority 7 uses the alternate layout (icon in the
// title slot, the title in the body slot).
void cSC4MySimWinProc::updateBodyTextCtrl() {
    if (!adviceList_) return;
    adviceList_->ClearList();             // 0x38
    adviceList_->SetbgImage(kMySimAdviceImage);   // 0x30
    cISC4MySim* sim = sims_[currentSim_];
    if (!sim || !sim->IsActive()) return;
    for (const cRZAutoRefCount<cISC4Advice>& ref : messages_) {
        cISC4Advice* advice = ref.get();
        if (!advice) continue;
        advice->AddRef();
        const int priority = advice->GetPriority();   // 0x38
        AdviceBody body;
        SelectPriority(body, priority);
        std::string font = kBodyFont;
        if (!advice->CheckFlags(2)) {   // 0x78: expired
            body.header = kExpiredPriorityHeader;
            body.body = kBodyFontDeactive;
            body.dot = kNeutralDot;
            body.rightArrow = kNeutralRightArrow;
            body.downArrow = kNeutralDownArrow;
            body.x = kNeutralX;
            font = kBodyFontDeactive;
        }
        std::string title = body.header + kHeaderFont;
        title += advice->GetTextTitle().ToChar();   // 0x90
        title += body.footer;
        cRZString titleText;
        titleText.AssignFromCString(title.c_str());
        cRZString dotText; dotText.AssignFromCString(body.dot.c_str());
        cRZString rightText; rightText.AssignFromCString(body.rightArrow.c_str());
        cRZString downText; downText.AssignFromCString(body.downArrow.c_str());
        cRZString xText; xText.AssignFromCString(body.x.c_str());
        if (priority == 7) {
            adviceList_->AddEntry(dotText, dotText, titleText, titleText, false, advice, xText, false,
                                  kMySimAdviceImage);
        } else {
            std::string text = font;
            text += advice->GetTextMessage().ToChar();   // 0xa0
            text += kBodyFooter;
            cRZString bodyText;
            bodyText.AssignFromCString(text.c_str());
            const bool enabled = advice->CheckFlags(0x20);   // 0x78
            adviceList_->AddEntry(downText, rightText, bodyText, titleText, enabled, advice, xText, false,
                                  kMySimAdviceImage);
        }
        advice->Release();
    }
    adviceList_->RebuildHTML(true);   // 0x28
}

// @ 0x10031af44: erases the first list entry that holds exactly this advice (the original compares the raw pointers).
void cSC4MySimWinProc::RemoveMessage(cISC4Advice* advice) {
    for (auto it = messages_.begin(); it != messages_.end(); ++it) {
        if (it->get() == advice) {
            messages_.erase(it);
            return;
        }
    }
}

// @ 0x10031afda: appends the advice (taking a reference) and flags it as shown (SetFlags(0x20, true)).
void cSC4MySimWinProc::AddMessage(cISC4Advice* advice) {
    if (!advice) return;
    messages_.emplace_back(advice);
    advice->SetFlags(0x20, true);   // 0x70
}

// @ 0x10031b06a. Hides the large and small pages, then shows the modal pick-sim dialog: the original creates it (if it
// is not there), Init's it — which is the modal loop — and then always Shutdown's and releases it.
void cSC4MySimWinProc::AddSim(std::uint32_t index) {
    if (cIGZWin* controls = ControlsOf(panel_)) {
        if (cIGZWin* large = controls->GetChildWindowFromIDRecursive(kMySimDialogLarge)) large->SetFlag(1, false);
        if (cIGZWin* small = controls->GetChildWindowFromIDRecursive(kMySimDialogSmall)) small->SetFlag(1, false);
    }
    if (!pickSim_ && g_host) {
        pickSim_ = g_host->CreatePickSimWinProc(index);
        if (pickSim_) {
            pickSim_->AddRef();
            pickSim_->Init();
        }
        if (!pickSim_) return;
    }
    if (!pickSim_) return;
    pickSim_->Shutdown();
    pickSim_->Release();
    pickSim_ = nullptr;
}

// @ 0x10031b158: loads the transport button bitmap {0, 0x4c06f888, bitmapId} into the car (or pedestrian) image window.
void cSC4MySimWinProc::SetTransportButtonImage(std::uint32_t bitmapId, bool pedestrian) {
    cSLBuffer image(0, kMySimIconImageGroup, bitmapId, 1, false);
    cIGZWin* controls = ControlsOf(panel_);
    if (!controls) return;
    gz::cIGZWinBMP* bitmap = nullptr;
    if (controls->GetChildAsRecursive(kMySimTransportCarBmp + (pedestrian ? 8u : 0u), gz::kGZIID_cIGZWinBMP,
                                      reinterpret_cast<void**>(&bitmap))) {
        if (image.GetBuffer()) bitmap->SetBMP(image.GetBuffer());
        bitmap->Release();
    }
}

// @ 0x10031b22a. Drops the "shown" flag from every advice that is no longer current, replaces the cached advice map with
// the advisor's current one and re-fills the message list from it (in map order).
void cSC4MySimWinProc::GetMessageList(cISC4Advisor* advisor) {
    for (const cRZAutoRefCount<cISC4Advice>& ref : messages_) {
        cISC4Advice* advice = ref.get();
        if (advice && !advice->CheckFlags(2)) advice->SetFlags(0x20, false);   // 0x70
    }
    messageMap_.clear();
    if (advisor) messageMap_ = advisor->GetCurrentAdvices();   // 0x58
    messages_.clear();
    for (const auto& entry : messageMap_) AddMessage(entry.first.get());
}

// @ 0x10031b4d8: hides the panel and the main frame, clears the page, and asks the agent simulator to stop tracking /
// cancel a pending move-in.
void cSC4MySimWinProc::Hide() {
    cIGZWin* controls = ControlsOf(panel_);
    if (controls) {
        nsGZWinUtils::SetWindowVisible(controls, kMySimPanelWindow, false, false);
        SetDialogState(0);
        nsGZWinUtils::SetWindowVisible(controls, kMySimMainWindow, false, false);
    }
    if (agentSim_) {
        agentSim_->HideLocation();   // 0x40
        agentSim_->CancelMoveIn();   // 0x50
    }
    shown_ = false;
}

// @ 0x10031b54a: re-reads the five slots, shows the panel, selects the main page and starts tracking no location.
void cSC4MySimWinProc::Show() {
    cIGZWin* controls = ControlsOf(panel_);
    GetCurrentSims();
    if (controls) {
        nsGZWinUtils::SetWindowVisible(controls, kMySimPanelWindow, true, false);
        SetDialogState(kMySimDialogMain);
        nsGZWinUtils::SetWindowVisible(controls, kMySimMainWindow, false, false);
    }
    if (agentSim_) agentSim_->TrackLocation(nullptr);   // 0x38
    shown_ = true;
}

}  // namespace sc4
