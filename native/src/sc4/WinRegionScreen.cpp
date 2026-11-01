#include "sc4/WinRegionScreen.h"
#include "sc4/RegionDialogs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

#include "gz/WinBtn.h"
#include "gz/WinUtils.h"
#include "sc4/SC4UI.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

IRegionScreenHost* g_host = nullptr;

// cGZWin flags the screen toggles (cIGZWin::tWinFlag).
constexpr std::uint32_t kFlagPrivateBuffer = 0x10000;
constexpr std::uint32_t kFlagIgnoreCursor = 0x200000;
constexpr std::uint32_t kGZIIDMessageFilter = 0xc6ae7085;   // cIGZWinMessageFilter (the +0x150 view)

// The two magic-number rounding constants: the original rounds by adding 12582912.0f (0x4b400000) to the float and
// reading the *bits* of the result (movd, not cvttss2si), then subtracting the bias as an integer
// (0x100237583..0x1002375a8). For |v| < 2^22 the bits of (v + 12582912.0f) are 0x4b400000 + round(v).
constexpr float kRoundBias = 12582912.0f;
constexpr std::int32_t kRoundBiasBits = 0x4b400000;

std::int32_t RoundThroughBits(float value) {
    const float biased = value + kRoundBias;
    std::int32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof bits);
    return bits;
}

constexpr std::uint32_t kSignpostTypeCity = 0xca539340;   // an occupied city
constexpr std::uint32_t kSignpostTypeEmpty = 0x0a8cd184;  // an empty city
constexpr std::uint32_t kSignpostSubType = 0x0a551c50;
constexpr std::uint32_t kUiSoundSignpost = 0x2a5c322b;

void ReleaseWindow(cIGZWin*& window) {
    if (window) {
        window->Release();
        window = nullptr;
    }
}

void ReleaseUnknown(cIGZUnknown*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void ReleaseBuffer(IWinBuffer*& buffer) {
    if (buffer) {
        buffer->Release();
        buffer = nullptr;
    }
}

}  // namespace

IRegionScreenHost* RegionScreenHost() { return g_host; }
void SetRegionScreenHost(IRegionScreenHost* host) { g_host = host; }

// ===================================================================================================================
// Construction / destruction
// ===================================================================================================================

cSC4WinRegionScreen::cSC4WinRegionScreen(cIGZWinMgr* mgr) : cGZWin(mgr) {   // @ 0x100235b78
    // +0x2b0..+0x2cf defaults and the two timers are member initialisers; the rest is the original's store sequence.
    SetFlag(kFlagIgnoreCursor, true);
    SetFlag(kFlagPrivateBuffer, false);
    id_ = kRegionScreenWindowId;
    if (IRegionScreenHost* host = RegionScreenHost()) {
        notifications_ = host->CreateNotificationHelper();
    }
    if (notifications_) notifications_->AddMessageTypes(kRegionScreenMsgTypes, 17);
}

cSC4WinRegionScreen::~cSC4WinRegionScreen() {   // @ 0x100235edc
    Shutdown();
    // The original releases whatever Shutdown left behind, in its field order.
    if (scroll_) {
        ChildDeleteAbsolute(scroll_);
        scroll_->Release();
        scroll_ = nullptr;
    }
    ReleaseWindow(unused280_);
    if (terrainMap_) {
        terrainMap_->Release();
        terrainMap_ = nullptr;
    }
    renderer_ = nullptr;   // borrowed from the host (see IRegionScreenHost)
    if (cameraControl_) {
        cameraControl_->Release();
        cameraControl_ = nullptr;
    }
    if (camera_) {
        if (camera_->Release() == 0) delete camera_;
        camera_ = nullptr;
    }
    effects_ = nullptr;       // borrowed
    frameService_ = nullptr;  // borrowed
    ReleaseUnknown(graphicSystem_);
    for (IWinBuffer*& buffer : snapshots_) ReleaseBuffer(buffer);
    cities_.clear();
    notifications_ = nullptr;   // owned by the host (the original has the helper inline at +0x1b8)
    if (accelerator_) {
        accelerator_->Release();
        accelerator_ = nullptr;
    }
    ReleaseUnknown(unknown1a8_);
    for (cIGZWin*& button : panelButtons_) ReleaseWindow(button);
    for (cIGZWin*& panel : panels_) ReleaseWindow(panel);
    ReleaseWindow(backdrop_);
    if (clouds_) {
        ChildDeleteAbsolute(clouds_);
        clouds_->Release();
        clouds_ = nullptr;
    }
    delete view_;
    view_ = nullptr;
}

bool cSC4WinRegionScreen::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x100239f72
    if (iid == kGZIID_cIGZWin || iid == 1) {
        *out = static_cast<cIGZWin*>(this);
    } else if (iid == kGZIIDMessageFilter) {
        *out = static_cast<cIGZWinMessageFilter*>(this);   // +0x150
    } else {
        // Quirk kept: the base window accepts 0xe98b2f57 and 0x452294aa, this override does not (0x100239f72).
        return false;
    }
    AddRef();
    return true;
}

// ===================================================================================================================
// Messages
// ===================================================================================================================

bool cSC4WinRegionScreen::DoMessage(cGZMessage& message) {   // @ 0x100239fcc
    const std::uint32_t type = message.type;
    if (type == kRegionScreenScrollStart) {
        if (view_) view_->SetScrollIndicator(true);
        return true;
    }
    if (type == kRegionScreenScrollStop) {
        if (view_) view_->SetScrollIndicator(false);
        return true;
    }
    if (type == kRegionScreenScrollDelta) {
        targetX_ += static_cast<float>(static_cast<std::int32_t>(message.param1)) * 0.00390625f;
        targetY_ += static_cast<float>(static_cast<std::int32_t>(message.param2)) * 0.00390625f;
        return true;
    }
    if (type == 0x1b && message.param2 - 5 <= 1) {
        // A key message: param3 points at a cGZMessage-style record whose +8 / +0x10 words are the key and modifiers.
        // (The original dereferences param3 without a check; the port guards so a malformed message cannot crash.)
        if (message.param3 != 0 && accelerator_) {
            const auto* keyMessage = reinterpret_cast<const cGZMessage*>(static_cast<std::uintptr_t>(message.param3));
            std::uint32_t command = 0;
            if (accelerator_->TestForAccelerator(static_cast<std::uint32_t>(keyMessage->param1),
                                                 static_cast<std::uint32_t>(keyMessage->param2),
                                                 message.param2 == 5, command)) {
                if (IRegionScreenHost* host = RegionScreenHost()) {
                    if (cIGZCommandServer* server = host->CommandServer()) {
                        server->ExecuteCommand(command, nullptr, nullptr);
                    }
                }
            }
        }
        return true;
    }
    return cGZWin::DoMessage(message);
}

bool cSC4WinRegionScreen::DoMessage(cIGZMessage2* message) {   // @ 0x10023a0dc
    const std::uint32_t type = message->GetType();
    void* dispatcher = nullptr;
    if (message->QueryInterface(kRegionScreenCommandMessage, &dispatcher)) {
        // The dispatcher message is a cRZMessage2Standard, so slots 0x50 / 0x78 are the payload accessors. The port
        // releases the reference QueryInterface took; the original drops it (a small leak at 0x10023a0df).
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        auto* input = static_cast<cIGZCommandParameterSet*>(standard->GetVoid1());
        auto* output = static_cast<cIGZCommandParameterSet*>(standard->GetVoid2());
        ProcessCommand(type, input, output);
        message->Release();
        return true;
    }
    if (type == kMsgRegionCityViewChanged) {
        if (cameraControl_) cameraControl_->SendViewChangeMessage();
        if (backdrop_) backdrop_->SetFlag(kWinVisible, true);
    } else if (type == kMsgRegionCloudsPreference) {
        bool visible = false;
        if (IRegionScreenHost* host = RegionScreenHost()) {
            if (const std::uint8_t* preferences = host->Preferences()) visible = preferences[kPrefsCloudsVisible] != 0;
        }
        if (clouds_) clouds_->SetFlag(kWinVisible, visible);
    }
    return true;
}

bool cSC4WinRegionScreen::DoMessage(cIGZWin*, cGZMessage&) {   // @ 0x10023a65c
    return false;
}

// ===================================================================================================================
// Commands
// ===================================================================================================================

bool cSC4WinRegionScreen::ProcessCommand(std::uint32_t command, cIGZCommandParameterSet* input,
                                        cIGZCommandParameterSet* output) {   // @ 0x10023a1fc
    IRegionScreenHost* host = RegionScreenHost();
    switch (command) {
    case kCmdRegionBitmapLoad:
        MakeWholeRegion();
        return false;
    case kCmdRegionSetExpandedToolTips: {
        bool value;
        if (cIGZVariant* variant = input ? input->GetParameter(0) : nullptr) {
            value = variant->GetValBool();
        } else {
            const std::uint8_t* preferences = host ? host->Preferences() : nullptr;
            value = !preferences || preferences[kPrefsExpandedToolTips] == 0;   // the original toggles
        }
        if (std::uint8_t* preferences = host ? host->Preferences() : nullptr) {
            preferences[kPrefsExpandedToolTips] = value ? 1 : 0;
        }
        return false;
    }
    case kCmdRegionCancel:
        if (view_) view_->ClearSelection();
        return false;
    case kCmdRegionQuitGame: {
        if (input == nullptr) return false;
        const bool flag = input->operator[](0).GetValBool();
        SetUserPreferencesToCurrentRegion();
        SetUserPreferencesToCurrentPosition();
        if (host) host->QuitToRegionSelect(flag ^ 1);
        return false;
    }
    case kCmdRegionScrollLeft:
        if (scroll_) scroll_->SetEdgeVector(-scroll_->EdgeSpeed(), scroll_->EdgeVectorY());
        return false;
    case kCmdRegionScrollLeftStop:
        if (scroll_) scroll_->SetEdgeVector(scroll_->EdgeSpeed() * 0.0f, scroll_->EdgeVectorY());
        return false;
    case kCmdRegionScrollRight:
        if (scroll_) scroll_->SetEdgeVector(scroll_->EdgeSpeed(), scroll_->EdgeVectorY());
        return false;
    case kCmdRegionScrollRightStop:
        if (scroll_) scroll_->SetEdgeVector(scroll_->EdgeSpeed() * 0.0f, scroll_->EdgeVectorY());
        return false;
    case kCmdRegionScrollUp:
        if (scroll_) scroll_->SetEdgeVector(scroll_->EdgeVectorX(), -scroll_->EdgeSpeed());
        return false;
    case kCmdRegionScrollUpStop:
        if (scroll_) scroll_->SetEdgeVector(scroll_->EdgeVectorX(), scroll_->EdgeSpeed() * 0.0f);
        return false;
    case kCmdRegionScrollDown:
        if (scroll_) scroll_->SetEdgeVector(scroll_->EdgeVectorX(), scroll_->EdgeSpeed());
        return false;
    case kCmdRegionScrollDownStop:
        if (scroll_) scroll_->SetEdgeVector(scroll_->EdgeVectorX(), scroll_->EdgeSpeed() * 0.0f);
        return false;
    case kCmdRegionLoadCity: {
        if (input == nullptr) return false;
        cRZString cityName;
        input->operator[](0).GetValString(cityName);
        std::uint32_t size = 0;
        if (!input->CompareParameterString(1, "small", false)) {
            size = 1;
            if (!input->CompareParameterString(1, "medium", false)) {
                size = 2;
                if (!input->CompareParameterString(1, "large", false)) {
                    size = 3;
                    if (!input->CompareParameterString(1, "any", false)) size = input->operator[](1).GetValUint32();
                }
            }
        }
        const bool empty = input->CompareParameterString(2, "empty", false);
        cRZString location;
        if (!LaunchCity(cityName, size, location, empty)) {
            if (output) output->SetStatusParameterValue(2);
        } else if (output) {
            output->SetParameter(0, cRZVariant(static_cast<const cIGZString&>(location)));
        }
        return false;
    }
    case kCmdRegionLoadRegion: {
        if (input == nullptr) return false;
        cRZString name;
        input->operator[](0).GetValString(name);
        if (!LoadRegion(name) && output) output->SetStatusParameterValue(2);
        return false;
    }
    default:
        return false;
    }
}

// ===================================================================================================================
// Init / Shutdown (the three big init/load routines are partial)
// ===================================================================================================================

bool cSC4WinRegionScreen::Init() {   // @ 0x100237bf2 (PARTIAL)
    if (get_init_flag()) return true;
    IRegionScreenHost* host = RegionScreenHost();
    PostRegionScreenMessage(kRegionScreenMsgLeavingApp);
    cGZWin::Init();
    SetID(kRegionScreenWindowId);

    if (host) {
        // The keyboard accelerator: GZCOM class 0x42e967be / iid 0xe2c1b3c4 (0x100237c4a..0x100237cde).
        accelerator_ = host->CreateKeyAccelerator();
        if (accelerator_) accelerator_->Init();

        // cISC4App::GetNation() (vslot 0x110) -> nation vslot 0x30 / 0x50 / 0x38: the current region index, and
        // the flag + previous index that gate the saved-view restore further down (0x100237ce1..0x100237d2c).
        host->UpdateRegionViewState();
        int previousRegionIndex = 0;
        const bool havePreviousRegion = host->PreviousRegionIndex(previousRegionIndex);
        currentRegionIndex_ = host->CurrentRegionIndex();
        const bool restoreSavedView = havePreviousRegion && currentRegionIndex_ == previousRegionIndex;

        // The region-music request: a cRZMessage2Standard of type 0x2a3ad653 with Data1 = 1, posted to SL::spMS2
        // with MessagePost(message, false) (0x100237d30..0x100237d84). cSC4AudioEventHandler::HandleRadioMessages
        // answers Data1 = 1 with cIGZRadio::SetStation("krgn", fade) - the Region/Music/*.mp3 playlist - and
        // Data1 = 0 with the mayor station "kmay".
        if (cIGZMessage2Standard* message = host->CreateStandardMessage()) {
            message->SetType(kRegionScreenMsgRegionMusic);
            message->SetData1(1);
            if (cIGZMessageServer2* server = host->MessageServer2()) server->MessagePost(message, false);
            message->Release();
        }

        // The screen takes the main window's area (0x100237d87: GetMainWindow() = vslot 0x50, then its GetW / GetH). An earlier
        // reading used the screen's own (still empty) size, so every child was laid out in a 0 x 0 window.
        if (cIGZWin* main = GetMainWindow()) SetArea(0, 0, main->GetW(), main->GetH());
        else SetArea(0, 0, GetW(), GetH());

        // SL::spGS, one reference held (0x100237dcc..0x100237e0a).
        if (cIGZUnknown* graphicSystem = host->GraphicSystemObject()) {
            if (graphicSystem != graphicSystem_) {
                graphicSystem->AddRef();
                ReleaseUnknown(graphicSystem_);
                graphicSystem_ = graphicSystem;
            }
        } else {
            ReleaseUnknown(graphicSystem_);
        }
        InitApplicationServices();

        // The backdrop at +0x170: a plain window behind everything (0x100237e16..0x100237ebf).
        {
            auto* backdrop = new cGZWin(GetWindowManager());
            backdrop->AddRef();
            backdrop_ = backdrop;
            backdrop->SetArea(0, 0, GetW(), GetH());
            backdrop->SetFlag(kWinPrivateBuffer, false);
            ChildAdd(backdrop);
            backdrop->SendToBack();
            backdrop->SetFlag(kWinFlag800, false);
            backdrop->SetFlag(kFlagIgnoreCursor, true);
            backdrop->SetFlag(kWinVisible, false);
        }

        // The region view at +0x160 (0x100237ec0..0x100238087). The original constructs cSC4WinRegionView inline;
        // the port asks the host, because the view reaches its own engine neighbours through IRegionViewHost.
        view_ = host->CreateRegionView();
        if (view_) {
            if (cIGZWin* viewWindow = view_->AsWindow()) {
                ChildAdd(viewWindow);
                viewWindow->SetArea(0, 0, GetW(), GetH());
                viewWindow->PullToFront();
                viewWindow->SetFlag(kWinFlag800, false);
                viewWindow->SetFlag(kWinHitTestTransparency, true);
                viewWindow->SetFlag(kWinVisible, true);
            }
            view_->SetRegionIndex(currentRegionIndex_);
            view_->SetLabelColours(labelColor1_, labelColor2_);
        }

        // The 12 city snapshot buffers: six image + mask pairs, one pair per city size code for the large and the
        // small set (0x100238088..0x10023829f). Each image is a cSLBuffer lookup; when either half of a pair is
        // missing the original replaces the pair with blank 1 x 1 display buffers from SL::spGS (Init 1,1 /
        // Lock 0x8001 / Fill 0 / Unlock 0x8001). The host owns both calls.
        for (int i = 0; i < 6; ++i) {
            IWinBuffer* image = host->LoadRegionScreenSnapshot(kRegionScreenSnapshotInstances[i][0]);
            IWinBuffer* mask = host->LoadRegionScreenSnapshot(kRegionScreenSnapshotInstances[i][1]);
            if (!image || !mask) {
                ReleaseBuffer(image);
                ReleaseBuffer(mask);
                image = host->CreateBlankDisplayBuffer();
                mask = host->CreateBlankDisplayBuffer();
                for (IWinBuffer* buffer : {image, mask}) {
                    if (!buffer) continue;
                    buffer->Init(1, 1);
                    if (buffer->Lock(0x8001)) {
                        buffer->Fill(0, cRZRect{0, 0, 1, 1});
                        buffer->Unlock(0x8001);
                    }
                }
            }
            SetSnapshotBuffer(i * 2, image);
            SetSnapshotBuffer(i * 2 + 1, mask);
            ReleaseBuffer(image);
            ReleaseBuffer(mask);
        }

        // The scroll window at +0x288 (0x1002382a0..0x1002383a4).
        {
            scroll_ = new cSC4WinRegionScroll(GetWindowManager());
            scroll_->AddRef();
            scroll_->SetArea(0, 0, GetW(), GetH());
            scroll_->SetFlag(kWinPrivateBuffer, false);
            ChildAdd(scroll_);
            scroll_->PullToFront();
            scroll_->SetFlag(kWinFlag800, false);
            scroll_->SetFlag(kFlagIgnoreCursor, true);
        }

        // The cloud shadows at +0x168 (0x1002383a5..0x100238481).
        {
            clouds_ = new cSC4WinRegionClouds(GetWindowManager());
            clouds_->AddRef();
            ChildAdd(clouds_);
            clouds_->PullToFront();
            clouds_->SetFlag(kWinFlag800, false);
            bool cloudsVisible = false;
            if (const std::uint8_t* preferences = host->Preferences()) {
                cloudsVisible = preferences[kPrefsCloudsVisible] != 0;
            }
            clouds_->SetFlag(kWinVisible, cloudsVisible);
            clouds_->SetDriftVector(90.51f, 18.75f);
        }

        PutCitiesInView();
        ReadTunableValues();
        targetX_ = static_cast<float>((boundMaxX_ + boundMinX_) / 2);
        targetY_ = static_cast<float>((boundMaxY_ + boundMinY_) / 2);
        // The saved view position (0x1002384e2..0x100238579): only when the previous region index is the current
        // one and the nation's validity flag is set; the original then ignores an unset X preference (INT32_MIN).
        if (restoreSavedView) {
            if (const std::uint8_t* preferences = host->Preferences()) {
                std::int32_t savedX = 0, savedY = 0;
                std::memcpy(&savedX, preferences + kPrefsViewX, sizeof savedX);
                std::memcpy(&savedY, preferences + kPrefsViewY, sizeof savedY);
                if (savedX != std::numeric_limits<std::int32_t>::min()) {
                    targetX_ = static_cast<float>(GetW()) * -0.5f + static_cast<float>(savedX) * 0.00390625f;
                    targetY_ = static_cast<float>(GetH()) * -0.5f + static_cast<float>(savedY) * 0.00390625f;
                }
            }
        }

        if (notifications_) notifications_->AddAllNotifications(static_cast<cIGZMessageTarget2*>(this));
        if (cIGZCommandServer* server = host->CommandServer()) {
            for (int i = 0; i < 14; ++i) {
                server->RegisterCommand(kRegionScreenCommands[i], kRegionScreenCommandNames[i], nullptr);
            }
        }
        LoadGUI();
        hideCountdown_ = 2;
        // PARTIAL: the "region_global" effect at +0x280 (0x1002386ec..0x1002387e9): the original asks the
        // cSC4EffectsManager (vslot 0x30) for the effect named "region_global", gives it a transform built from
        // SL::spTerrainMap (vslots 0x30 / 0x38 / 0x40 -> cell size and cell counts) and starts it (effect vslot
        // 0x18, transition 0). The effects-manager seam here only covers Init / Shutdown, and cISC4VisualEffect is
        // not wired into it, so the +0x280 window stays null for now.
    }
    PostRegionScreenMessage(kRegionScreenMsgEnteredApp);
    return true;
}

bool cSC4WinRegionScreen::Shutdown() {   // @ 0x100239aa0
    if (!get_init_flag()) return true;
    IRegionScreenHost* host = RegionScreenHost();
    PostRegionScreenMessage(kRegionScreenMsgLeavingRegion);
    if (backdrop_) {
        ChildDeleteAbsolute(backdrop_);
        backdrop_->Release();
        backdrop_ = nullptr;
    }
    if (view_) {
        cIGZWin* viewWindow = view_->AsWindow();
        if (viewWindow) ChildDeleteAbsolute(viewWindow);
        delete view_;
        view_ = nullptr;
    }
    if (notifications_) notifications_->RemoveAllNotifications(static_cast<cIGZMessageTarget2*>(this));
    if (host) {
        if (cIGZCommandServer* server = host->CommandServer()) {
            for (std::uint32_t id : kRegionScreenCommands) server->UnregisterCommand(id);
        }
    }
    // The original destructs every city in place and then resets the vector's end pointer.
    cities_.clear();
    ShutdownApplicationServices();
    ReleaseUnknown(graphicSystem_);
    if (clouds_) {
        // See the scroll window below: ChildDeleteAll in the original has already dropped the child-list reference.
        ChildDeleteAbsolute(clouds_);
        clouds_->Release();
        clouds_ = nullptr;
    }
    ReleaseUnknown(unknown1a8_);
    if (scroll_) {
        // The original reaches this point after ChildDeleteAll, which has already dropped the child-list reference;
        // the port removes the window explicitly so a later Release cannot leave a dangling child.
        ChildDeleteAbsolute(scroll_);
        scroll_->Release();
        scroll_ = nullptr;
    }
    if (accelerator_) {
        accelerator_->Release();
        accelerator_ = nullptr;
    }
    for (cIGZWin*& panel : panels_) ReleaseWindow(panel);
    for (cIGZWin*& button : panelButtons_) ReleaseWindow(button);
    for (IWinBuffer*& buffer : snapshots_) ReleaseBuffer(buffer);
    if (host) host->FlushResources();
    cGZWin::Shutdown();
    PostRegionScreenMessage(kRegionScreenMsgLeftRegion);
    return true;
}

void cSC4WinRegionScreen::InitApplicationServices() {   // @ 0x100238972 (PARTIAL)
    IRegionScreenHost* host = RegionScreenHost();
    if (host) {
        frameService_ = host->CreateFrameService();
        if (frameService_) frameService_->Init();
    }
    int left = 0, top = 0, right = 0, bottom = 0;
    if (host) host->RegionCellBounds(left, top, right, bottom);
    // The original clamps the cell counts to 0x20 and multiplies them by 0x10 (16 vertices per region cell).
    int cellsX = std::min((right - left) + 1, 0x20);
    int cellsZ = std::min((bottom - top) + 1, 0x20);
    terrainMap_ = new cSC4RegionalTerrainMap(270.0f, 0.0f, cellsX * 0x10, cellsZ * 0x10, 64.0f);
    terrainMap_->Init();
    if (host) host->SetTerrainMap(terrainMap_);

    if (!camera_) {
        camera_ = new s3d::cS3DCamera();
        camera_->AddRef();
    }
    camera_->Init();
    if (!cameraControl_) {
        cameraControl_ = new cSC4CameraControl();
        cameraControl_->AddRef();
    }
    cameraControl_->SetViewportSize(GetW(), GetH());
    cameraControl_->SetCitySize(static_cast<float>((right - left) * 0x400 + 0x400),
                                static_cast<float>((bottom - top) * 0x400 + 0x400));
    cameraControl_->Init(cS3DVector3{64.0f, 64.0f, 0.0f}, camera_);
    renderer_ = host ? host->CreateRenderer() : nullptr;
    if (renderer_) renderer_->Init(GetW(), GetH());
    if (host) host->SetViewUtilities(renderer_);
    UpdateRendererPosition();
    if (host) {
        effects_ = host->CreateEffectsManager();
        if (effects_) effects_->Init();
        host->PostEngineMessage(kMsgRegionCityViewChanged);
        host->PostEngineMessage(kRegionScreenCameraGridMessage);
    }
    // PARTIAL: the original also builds cSC4RegionalTerrainMap from the region rect, creates the cS3DCamera /
    // cSC4CameraControl through GZCOM, configures cSC43DRender (vslots 0x18/0x28/0x30/0x38/0x60/0x78/0x88/0x100/0x1b8),
    // adds the cSC4RegionalTerrainView and assigns SL::spViewUtilities / spLightingManager / spRenderer. The renderer
    // and the two services come from the host here.
}

// (anonymous namespace)::SetupScript @ 0x10023d1b6: clears the accept-focus flag of every window of a script and points the
// notification target of its buttons (iid 0x8810) and of windows answering iid 0xa1336cc0 at the screen; windows that answer
// the second iid are not descended into.
bool RegionScreenSetupScript(cIGZWin*, std::uint32_t, void* child, void* context) {
    auto* window = static_cast<cIGZWin*>(child);
    window->SetFlag(0x8000, false);
    cIGZUnknown* button = nullptr;
    if (window->QueryInterface(0x8810, reinterpret_cast<void**>(&button))) {
        window->SetNotificationTarget(static_cast<cIGZWin*>(context));
        button->Release();
    }
    cIGZUnknown* special = nullptr;
    if (window->QueryInterface(0xa1336cc0, reinterpret_cast<void**>(&special))) {
        window->SetNotificationTarget(static_cast<cIGZWin*>(context));
    } else {
        window->EnumChildren(0x22ba0121, RegionScreenSetupScript, context);
    }
    if (special) special->Release();
    return true;
}

// Builds the screen's windows from the game's window scripts (96a006b0/aa920991 = the toolbar strip, /abc0ed33 = the panel
// toggles) and wires their buttons to the screen. Recovered from the 1955-byte body; `this` is passed to the scripts as the
// notification target through the cIGZWin view.
void cSC4WinRegionScreen::LoadGUI() {   // @ 0x10023927a
    IRegionScreenHost* host = RegionScreenHost();
    nSC4UI::INSC4UIHost* ui = nSC4UI::Host();
    cIGZWin* self = static_cast<cIGZWin*>(static_cast<cGZWin*>(this));

    std::set<cIGZWin*> before;
    nSC4UI::GetWindowChildrenSet(self, before);
    if (ui) {
        const cGZPersistResourceKey toolbarKey{0, 0x96a006b0, 0xaa920991};
        if (cIGZWin* root = ui->CreateWindowFromScript(toolbarKey, self, 0x09ebe9ee)) {
            const int width = GetW(), height = GetH();
            int x = 5;
            if (static_cast<std::uint32_t>(height * 3) < static_cast<std::uint32_t>(width)) x = static_cast<int>(static_cast<std::uint32_t>(width) / 3) + 5;
            root->GZWinMoveTo(x, (height + 2) - root->GetH());
            root->PullToFront();
            root->EnumChildren(0x22ba0121, RegionScreenSetupScript, self);
            root->SetFlag(0x800, false);
            root->Release();
        }
    }
    std::set<cIGZWin*> added;
    nSC4UI::GetWindowChildrenNotInSet(self, before, added);
    nSC4UI::AlignWindowSetToWindow(added, self);

    for (std::uint32_t id : {0x6a91dc16u, 0x6a91dc15u})
        if (cIGZWin* window = GetChildWindowFromIDRecursive(id)) window->EnumChildren(0x22ba0121, RegionScreenSetupScript, self);

    // the three info panels and their buttons (ids from DAT_100a1b808; each button is the id after its panel)
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t id = kRegionScreenPanelIds[i];
        if (cIGZWin* panel = GetChildWindowFromID(id)) {
            SetMemberWindow(i, panel);
            panel->HideWindow();
            panel->EnumChildren(0x22ba0121, RegionScreenSetupScript, self);
            if (cIGZWin* button = GetChildWindowFromID(id + 1)) {
                SetMemberWindow(3 + i, button);
                button->HideWindow();
            }
        }
    }
    if (cIGZWin* root = GetChildWindowFromID(kRegionScreenRootChildId)) {
        root->SetFlag(0x800, true);
        root->PullToFront();
        root->SetFlag(0x800, false);
    }
    const int regionCount = host ? host->RegionCount() : 0;
    if (regionCount < 2) {
        if (cIGZWin* window = GetChildWindowFromIDRecursive(0x2a5b0002)) window->SetFlag(2, false);
    }

    cIGZWin* toggles = nullptr;
    if (ui) {
        const cGZPersistResourceKey togglesKey{0, 0x96a006b0, 0xabc0ed33};
        toggles = ui->CreateWindowFromScript(togglesKey, self, 0x0bb0f5e7);
    }
    if (toggles) {
        toggles->GZWinMoveTo((GetW() - 5) - toggles->GetW(), (GetH() - 5) - toggles->GetH());
        toggles->PullToFront();
        toggles->EnumChildren(0x22ba0121, RegionScreenSetupScript, self);
        toggles->SetFlag(0x800, false);
        toggles->HideWindow();
        toggles->Release();
    }
    cIGZWin* sibling = nullptr;
    if (toggles && nsGZWinUtils::FindSiblingAs(toggles, 0x6bb92bca, 0x22ba0121, reinterpret_cast<void**>(&sibling))) {
        sibling->GZWinMoveTo((GetW() - 10) - sibling->GetW(), (GetH() - 10) - sibling->GetH());
        sibling->PullToFront();
        sibling->EnumChildren(0x22ba0121, RegionScreenSetupScript, self);
        sibling->SetFlag(0x800, false);
        sibling->HideWindow();
    }
    if (sibling) sibling->Release();

    UpdatePopulation();
    if (host) {
        if (cIGZWin* caption = GetChildWindowFromIDRecursive(0xea5bd179)) {
            if (const char* name = host->RegionNameAt(currentRegionIndex_)) caption->SetCaption(cRZString(name));
        }
        const std::uint8_t* preferences = host->Preferences();
        if (preferences) {
            const bool grid = preferences[kPrefsShowGrid] != 0;
            const bool signposts = preferences[kPrefsSignpostMode] != 0;
            signpostMode_ = signposts ? 1 : 0;
            if (view_) view_->EnableGrid(grid);
            nsGZWinUtils::SetButtonToggleState(self, kRegionScreenGridCheckBoxId, grid);
            nsGZWinUtils::SetButtonToggleState(self, kRegionScreenSignpostCheckBoxId, signposts);
            int mapMode = preferences[kPrefsMapMode];
            if (mapMode > 2) mapMode = 0;
            for (int i = 0; i < 3; ++i) nsGZWinUtils::SetButtonToggleState(self, kRegionScreenMapModeButtonIds[i], mapMode == i);
            if (view_) view_->SetMapMode(mapMode);
            nsGZWinUtils::SetWindowVisible(self, 0x0bb0f5e7, mapMode != 0 && !panelsVisible_, true);
            nsGZWinUtils::SetWindowVisible(self, 0x6bb92bca, panelsVisible_ && mapMode != 0, true);
        }
    }
    RestoreSignposts();
    if (cIGZWin* window = GetChildWindowFromIDRecursive(0x4a779a1a)) {
        window->SetFlag(1, false);
        window->SetFlag(2, false);
    }
}

void cSC4WinRegionScreen::PutCitiesInView() {   // @ 0x100236398 (PARTIAL)
    // One record per city place of the active region, in the original's formulas: the cell corner (x - 1 + 2^size, z - 1 + 2^size),
    // the map position (-37.49 * (z + 2^size) + 90.51 * x, 45.25 * (z + 2^size) + 18.75 * (x + 2^size)) and the default snapshots of
    // the city's size. PARTIAL: the draw-order sort (cCityDrawingOrderSort), the city file thumbnails (SC4ReadCityRegionViewThumbnails)
    // and their resampling are not recovered, so every city keeps the default picture.
    IRegionScreenHost* host = RegionScreenHost();
    if (host) {
        std::vector<cISC4Region::cLocation> locations;
        host->CityLocations(locations);
        cities_.clear();
        for (const cISC4Region::cLocation& location : locations) {
            RegionViewCity city;
            const int edge = 1 << (location.size & 0x1f);
            city.left = location.x - 1 + edge;
            city.top = location.z - 1 + edge;
            city.x = location.x;
            city.z = location.z;
            city.size = static_cast<std::uint8_t>(location.size);
            const float zs = static_cast<float>(edge + location.z);
            city.mapX = zs * -37.49f + static_cast<float>(location.x) * 90.51f;
            city.mapY = zs * 45.25f + static_cast<float>(edge + location.x) * 18.75f;
            SetCitySnapshotsToDefault(city);
            // The city's own picture replaces the default one. (The original also resamples it by the fraction of its map position and
            // turns the mask into the alpha channel and three span lists - RecalculateImagesFromCitySnapshots, still PARTIAL; the picture
            // here already carries its alpha.)
            IWinBuffer* image = nullptr;
            IWinBuffer* mask = nullptr;
            if (host->LoadCityThumbnails(location.x, location.z, &image, &mask) && image) {
                city.snapshot = image;
                city.mask = mask;
            }
            ReleaseBuffer(image);
            ReleaseBuffer(mask);
            cities_.push_back(std::move(city));
        }
    }
    if (view_) {
        for (const RegionViewCity& city : cities_) view_->AddCity(city);
        view_->SetSignpostType(0, 0);
    }
    ComputeBaseLimits();
}

void cSC4WinRegionScreen::RecalculateImagesFromCitySnapshots(RegionViewCity&) {   // @ 0x100236c36 (PARTIAL)
    // PARTIAL: Resample (0x10023c642), SC4MoveMaskToAlpha (0x10023ce10) and SC4CreateSpanList (0x10023cf28) work on
    // cIGZBuffer lock bits and are not recovered; the city snapshots (the double resample by (1 - fract(x)) /
    // (1 - fract(y))), the alpha masks and the three span lists stay unimplemented.
}

void cSC4WinRegionScreen::SetCitySnapshotsToDefault(RegionViewCity& city) {   // @ 0x100236b66
    const int size = city.size;
    IRegionScreenHost* host = RegionScreenHost();
    const int snapshotSet = host ? host->CitySnapshotSet() : 0;
    // +0x218 / +0x220 for the small set, +0x1e8 / +0x1f0 for the large one; both are two pointers per size code.
    const int base = snapshotSet == 1 ? 6 : 0;
    if (size >= 0 && size < 3) {
        city.snapshot = snapshots_[base + size * 2];
        city.mask = snapshots_[base + size * 2 + 1];
    } else {
        city.snapshot = nullptr;
        city.mask = nullptr;
    }
    if (city.snapshotSmall) {
        city.snapshotSmall->Release();
        city.snapshotSmall = nullptr;
    }
    if (city.maskSmall) {
        city.maskSmall->Release();
        city.maskSmall = nullptr;
    }
}

void cSC4WinRegionScreen::MakeWholeRegion() {   // @ 0x10023a66c (PARTIAL)
    // PARTIAL: the 1855-byte function opens cSC4WinLoadHeightField, validates the chosen bitmap against the region
    // size (^64 cells + 1) and then copies it cell by cell into a fresh cISC4RegionalCity per city. It needs
    // cSC4WinLoadHeightField's location saver / file browser wiring and the regional-city image API (vslots
    // 0x800/0x8080/0xc0), which are not recovered.
}

// ===================================================================================================================
// Region / city helpers
// ===================================================================================================================

RegionCityRef* cSC4WinRegionScreen::GetOrCreateRegionalCity(const RegionViewCity& city, bool create) {   // @ 0x10023ae9c (PARTIAL)
    // PARTIAL: the original builds the city name from the region name (region vslot 0x28 vslots 0x40 / 0x48 +
    // city-list vslot 0x28) and then creates the cSC4RegionalCity through its constructor, Init (vslot 0x18),
    // SetCitySize (0x40), SetPosition (0x30), SetName (0x110) and AddCity (list 0x60). cSC4RegionalCity is not
    // recovered, so the find-or-create flow goes through the host; the lookup itself is the same slot the original
    // uses first (city list vslot 0x58).
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return nullptr;
    if (RegionCityRef* found = host->FindCity(city.x, city.z)) return found;
    if (!create) return nullptr;
    return host->FindOrCreateCity(city, create);
}

bool cSC4WinRegionScreen::LoadRegion(const cRZString& name) {   // @ 0x10023b130
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return false;
    const int count = host->RegionCount();
    if (count == 0) return false;
    for (int i = 0; i < count; ++i) {
        const char* regionName = host->RegionNameAt(i);
        if (!regionName) continue;
        const std::size_t length = std::strlen(regionName);
        // The original compares the raw character block (memcmp) after comparing the lengths (0x10023b1e6..0x10023b290).
        if (name.Strlen() != length || std::memcmp(name.Data(), regionName, length) != 0) continue;
        if (i != currentRegionIndex_) {
            host->SelectRegion(i);
            return host->ReloadRegion();
        }
        return true;
    }
    return false;
}

bool cSC4WinRegionScreen::LaunchCity(const cRZString& name, std::uint32_t size, cRZString& out, bool empty) {   // @ 0x10023b366
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return false;
    const std::uint32_t nameLength = name.Strlen();
    int wantedCells = 0;
    if (size < 3) wantedCells = kRegionScreenCityCellCounts[size];
    for (RegionViewCity& city : cities_) {
        RegionCityRef* cityRef = host->FindCity(city.x, city.z);
        IRegionalCity* regional = cityRef ? cityRef->get() : nullptr;
        const bool occupied = regional && regional->IsOccupied();
        if (nameLength == 0) {
            // No city name: launch the first city that is not in use and matches the requested size.
            if (occupied) continue;
            int cells = 0;
            if (!regional || empty) {
                // The original uses the *city's* size code here, not the requested one (0x10023b4a8).
                cells = (1 << city.size) << city.size;
            } else {
                int cellsX = 0, cellsZ = 0;
                regional->GetSize(cellsX, cellsZ);
                cells = cellsZ * cellsX;
            }
            if (wantedCells != 0 && wantedCells != cells) continue;
            RegionCityRef* created = GetOrCreateRegionalCity(city, true);
            if (!created) continue;
            if (!LaunchRegionalCity(created)) continue;
            if (created->get()) created->get()->GetName(out);
            return true;
        }
        if (!occupied) continue;
        cRZString cityName;
        regional->GetName(cityName);
        if (cityName.Strlen() != nameLength || std::memcmp(cityName.Data(), name.Data(), nameLength) != 0) continue;
        if (!LaunchRegionalCity(cityRef)) continue;
        out = cityName;
        return true;
    }
    return false;
}

bool cSC4WinRegionScreen::LaunchRegionalCity(RegionCityRef* cityRef) {   // @ 0x10023b63c
    if (!cityRef) return false;
    IRegionScreenHost* host = RegionScreenHost();
    AddRef();   // the original AddRefs itself for the duration of the launch (0x10023b61e)
    SetUserPreferencesToCurrentPosition();
    IRegionalCity* city = cityRef->get();
    if (!city) {
        Release();
        return false;
    }
    city->AddRef();
    cRZString name;
    city->GetName(name);
    bool launched = false;
    if (name.Strlen() != 0) {
        if (host && host->LaunchRegionCityByName(name, cityRef)) {
            launched = true;
        }
    }
    if (!launched) {
        if (host) {
            host->ClearRegionViewSlot();
            launched = host->LaunchCityFromRegion(cityRef) != 0;
        }
    }
    Release();
    city->Release();
    return launched;
}

void cSC4WinRegionScreen::SetUserPreferencesToCurrentRegion() {   // @ 0x10023b7bc
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return;
    std::uint8_t* preferences = host->Preferences();
    if (!preferences) return;
    const char* regionName = host->RegionName();
    // The original strncpy's exactly 0x40 bytes (it does not force a NUL); keep the padding.
    std::strncpy(reinterpret_cast<char*>(preferences + kPrefsRegionName), regionName ? regionName : "", 0x40);
    const std::int32_t never = std::numeric_limits<std::int32_t>::min();
    std::memcpy(preferences + kPrefsViewX, &never, sizeof never);
    std::memcpy(preferences + kPrefsViewY, &never, sizeof never);
    host->SavePreferences();
}

void cSC4WinRegionScreen::SetUserPreferencesToCurrentPosition() {   // @ 0x10023b846
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return;
    std::uint8_t* preferences = host->Preferences();
    if (!preferences) return;
    const std::int32_t viewX = static_cast<std::int32_t>((static_cast<float>(GetW()) * 0.5f + targetX_) * 256.0f);
    const std::int32_t viewY = static_cast<std::int32_t>((static_cast<float>(GetH()) * 0.5f + targetY_) * 256.0f);
    std::memcpy(preferences + kPrefsViewX, &viewX, sizeof viewX);
    std::memcpy(preferences + kPrefsViewY, &viewY, sizeof viewY);
    host->SavePreferences();
}

void cSC4WinRegionScreen::UpdatePopulation() {   // @ 0x10023c54c
    int total = 0;
    for (const RegionViewCity& city : cities_) {
        RegionCityRef* cityRef = GetOrCreateRegionalCity(city, false);
        if (cityRef && cityRef->get()) total += cityRef->get()->GetPopulation();
    }
    IRegionScreenHost* host = RegionScreenHost();
    cRZString text;
    if (host) host->FormatNumber(total, text);
    cIGZWin* child = GetChildWindowFromIDRecursive(kRegionScreenPopulationChildId);
    if (child) child->SetCaption(text);
}

// ===================================================================================================================
// Layout / paint
// ===================================================================================================================

void cSC4WinRegionScreen::ComputeBaseLimits() {   // @ 0x100237106
    IRegionScreenHost* host = RegionScreenHost();
    int left = 0, top = 0, right = 0, bottom = 0;
    if (host) host->RegionCellBounds(left, top, right, bottom);
    const float mapWidth = static_cast<float>((right + 1) - left);
    const float mapHeight = static_cast<float>((bottom + 1) - top);
    float heightPart = mapHeight * 37.49f;
    const int halfX = cityHalfSizeX_;
    int totalWidth = static_cast<int>(static_cast<float>(halfX * 2) + mapWidth * 90.51f + heightPart);
    float totalHeight = static_cast<float>(
        static_cast<int>(static_cast<float>(cityHalfSizeY_ * 2) + mapHeight * 45.25f + mapWidth * 18.75f));
    int width = GetW();
    const float originPartX = static_cast<float>(-halfX - width / 2) - heightPart;
    const int halfY = cityHalfSizeY_;
    const int height = GetH();
    float originPartY = static_cast<float>(-halfY - height / 2) + -45.25f;
    float widthExcess = 0.0f;
    int heightExcess = 0;
    width = GetW();
    if (width < totalWidth) {
        width = GetW();
        widthExcess = static_cast<float>(totalWidth - width);
        totalWidth = GetW();
    }
    originPartY = static_cast<float>(static_cast<int>(originPartY));
    const int originX = totalWidth / 2 + static_cast<int>(originPartX);
    int mapHeightPixels = GetH();
    if (mapHeightPixels < static_cast<int>(totalHeight)) {
        mapHeightPixels = GetH();
        heightExcess = static_cast<int>(totalHeight) - mapHeightPixels;
        totalHeight = static_cast<float>(GetH());
    }
    const int originY = static_cast<int>(totalHeight) / 2 + static_cast<int>(originPartY);
    const int maxX = static_cast<int>(widthExcess) + originX;
    boundMinX_ = originX;
    boundMinY_ = originY;
    boundMaxX_ = maxX;
    boundMaxY_ = heightExcess + originY;
    if (clouds_) {
        clouds_->SetCloudArea(static_cast<float>(originX), static_cast<float>(originY),
                              static_cast<float>(GetW() + maxX), static_cast<float>(GetH() + boundMaxY_));
    }
}

void cSC4WinRegionScreen::UpdateRendererPosition() {   // @ 0x100237632
    RegionCameraBasis basis;
    IRegionScreenHost* host = RegionScreenHost();
    const bool haveBasis = host && host->CameraBasis(basis);
    const float worldX = (static_cast<float>(GetW()) * 0.5f + targetX_) * basis.worldPerUnit;
    const float rightX = basis.right[0] * worldX;
    const float rightZ = basis.right[2] * worldX;
    const float rightY = basis.right[1] * worldX;
    const float worldY = (static_cast<float>(GetH()) * 0.5f + targetY_) * basis.worldPerUnit;
    cS3DVector3 position{rightX - basis.up[0] * worldY, rightZ - basis.up[2] * worldY,
                         rightY - basis.up[1] * worldY};
    // The original calls the control unconditionally (a missing view-utilities object crashes it); the port skips the
    // call when the host has no basis.
    if (haveBasis && cameraControl_) cameraControl_->SetViewTargetPosition(position);
}

bool cSC4WinRegionScreen::GZPaint() {   // @ 0x100237382
    if (hideCountdown_ != 0) {
        --hideCountdown_;
        if (hideCountdown_ == 0) SetFlag(kFlagIgnoreCursor, false);
    }
    if (!paintTimer_.IsTimerRunning()) {
        paintTimer_.Start();
    } else {
        const std::uint32_t elapsed = paintTimer_.GetElapsedTimeU32();
        paintTimer_.Restart();
        if (smoothing_) {
            const int milliseconds = elapsed < 0xc9 ? static_cast<int>(elapsed) : 200;
            const float targetX = smoothTargetX_;
            const float targetY = smoothTargetY_;
            float currentX = targetX_;
            float currentY = targetY_;
            const float deltaX = targetX - currentX;
            const float deltaY = targetY - currentY;
            const float length = std::sqrt(deltaY * deltaY + deltaX * deltaX);
            float speed = length * 5.0f;
            if (speed > 1200.0f) speed = 1200.0f;   // minss against 1200.0f
            const float step = (static_cast<float>(milliseconds) / 1000.0f) * (speed / length);
            currentX = step * deltaX + currentX;
            currentY = step * deltaY + currentY;
            targetX_ = currentX;
            targetY_ = currentY;
            if (std::fabs(currentY - targetY) + std::fabs(currentX - targetX) <= 2.0f) smoothing_ = false;
        }
    }
    if (targetX_ < static_cast<float>(boundMinX_)) {
        targetX_ = static_cast<float>(boundMinX_);
        smoothing_ = false;
    } else if (targetX_ > static_cast<float>(boundMaxX_)) {
        targetX_ = static_cast<float>(boundMaxX_);
        smoothing_ = false;
    }
    if (targetY_ < static_cast<float>(boundMinY_)) {
        targetY_ = static_cast<float>(boundMinY_);
        smoothing_ = false;
    } else if (targetY_ > static_cast<float>(boundMaxY_)) {
        targetY_ = static_cast<float>(boundMaxY_);
        smoothing_ = false;
    }
    UpdateRendererPosition();
    float screenX = 0, screenY = 0;
    if (renderer_) renderer_->GetScreenOffsets(screenX, screenY);
    float planeX = 0, planeY = 0;
    RegionCameraBasis basisProbe;
    IRegionScreenHost* basisHost = RegionScreenHost();
    if (cameraControl_ && basisHost && basisHost->CameraBasis(basisProbe)) {
        const cS3DVector2 offset = cameraControl_->ScreenOffset();   // camera control +0xa0 / +0xa4
        planeX = offset.x;
        planeY = offset.y;
    } else {
        // No camera basis (the host has no 3-D renderer): the screen offset is the target plus half the window, y up.
        planeX = targetX_ + static_cast<float>(GetW()) * 0.5f;
        planeY = -(targetY_ + static_cast<float>(GetH()) * 0.5f);
    }
    // Quirk: x rounds through the float bits with a +12582912 bias, y with 12582912 - value (a negated result).
    const int scrollX = RoundThroughBits(planeX) - GetW() / 2 - kRoundBiasBits;
    const int scrollY = RoundThroughBits(-planeY) - GetH() / 2 - kRoundBiasBits;
    if (view_) view_->SetScrollPos(scrollX, scrollY);
    if (clouds_) clouds_->SetOffset(cRZPoint{scrollX, scrollY});
    if (frameService_) frameService_->EndFrame();
    return true;
}

// ===================================================================================================================
// Mouse / keyboard
// ===================================================================================================================

bool cSC4WinRegionScreen::GZOnMouseEnter(cIGZWin*) {   // @ 0x100237748
    int x = 0, y = 0;
    if (mgr_) mgr_->GetCursorPos(this, x, y);
    GZOnMouseMove(x, y, 0);
    return true;
}

bool cSC4WinRegionScreen::GZOnMouseExit(cIGZWin*) {   // @ 0x10023b78e
    if (!signpostActive_ && view_) view_->ClearSelection();
    return true;
}

bool cSC4WinRegionScreen::GZOnMouseMove(int x, int y, std::uint32_t) {   // @ 0x100237794
    if (!signpostActive_ && view_) view_->SelectCity(x, y);
    return true;
}

bool cSC4WinRegionScreen::GZOnMouseDownR(int x, int y, std::uint32_t modifiers) {   // @ 0x100237bda
    if (scroll_) scroll_->GZOnMouseDownR(x, y, modifiers);
    return true;
}

bool cSC4WinRegionScreen::GZOnMouseDownL(int x, int y, std::uint32_t) {   // @ 0x1002377b2
    if (false) fprintf(stderr, "region click %d,%d signpost=%d city=%p\n", x, y, int(signpostActive_), view_ ? (void*)view_->GetCityAtPoint(x, y) : nullptr);
    IRegionScreenHost* host = RegionScreenHost();
    if (signpostActive_) {
        RegionViewCity* city = view_ ? view_->GetCityAtPoint(x, y) : nullptr;
        if (city && city == view_->GetSelectedCity()) {
            const std::uint32_t elapsed = doubleClickTimer_.GetElapsedTimeU32();
            const int threshold = host ? host->DoubleClickTimeMs() : 0;
            if (static_cast<int>(elapsed) < threshold) {
                GZOnCommand(0x287259f6, 0x4a560000);   // open the selected city
                return true;
            }
        }
        if (view_) view_->SelectCity(x, y);
        if (!view_ || view_->GetSelectedCity() == nullptr) {
            RestoreSignposts();
            return true;
        }
    }
    RegionViewCity* city = view_ ? view_->GetSelectedCity() : nullptr;
    if (!city) return true;
    if (host) host->PlayUiSound(kUiSoundSignpost);
    ShowPanel(-1);
    if (view_) {
        view_->EnableTutorialArrow(true);
        view_->SetSignpostMode(false, true);
        view_->EnableLabels(signpostMode_ == 1, false);
    }
    RegionCityRef* cityRef = host ? host->FindCity(city->x, city->z) : nullptr;
    const bool occupied = cityRef && cityRef->get() && cityRef->get()->IsOccupied();
    if (view_) view_->SetSignpostType(occupied ? kSignpostTypeCity : kSignpostTypeEmpty, kSignpostSubType);
    signpostActive_ = true;
    doubleClickTimer_.Restart();
    if (view_) {
        cRZRect rect;
        view_->GetRegionPlayfieldRect(city, rect);
    }
    return true;
}

bool cSC4WinRegionScreen::GZOnKeyDown(std::uint32_t, std::uint32_t) {   // @ 0x10023b7ac
    return false;
}

bool cSC4WinRegionScreen::GZOnKeyUp(std::uint32_t, std::uint32_t) {   // @ 0x10023b7b4
    return false;
}

// ===================================================================================================================
// Signposts / panels / GUI
// ===================================================================================================================

void cSC4WinRegionScreen::RestoreSignposts() {   // @ 0x1002379ae
    if (view_) view_->EnableTutorialArrow(true);
    if (signpostMode_ == 1) {
        if (view_) {
            view_->SetSignpostMode(true, false);
            view_->SetSignpostType(0, 0);
            view_->EnableLabels(true, true);
        }
    } else if (signpostMode_ == 0) {
        if (view_) {
            view_->SetSignpostMode(false, true);
            view_->SetSignpostType(0, 0);
            view_->EnableLabels(false, true);
        }
    }
    signpostActive_ = false;
    if (view_) view_->ClearSelection();
}

void cSC4WinRegionScreen::ShowPanel(int panel) {   // @ 0x100237a58
    IRegionScreenHost* host = RegionScreenHost();
    cIGZWin* rootChild = GetChildWindowFromID(kRegionScreenRootChildId);
    if (panel >= 0 && panels_[panel] && panels_[panel]->IsVisible()) {
        // Clicking the visible panel's button hides the panel and its button.
        panels_[panel]->HideWindow();
        if (panelButtons_[panel]) panelButtons_[panel]->HideWindow();
    } else {
        for (int i = 0; i < 3; ++i) {
            if (panels_[i]) panels_[i]->SetFlag(kWinVisible, i == panel);
            if (panelButtons_[i]) {
                if (i == panel) {
                    if (host) host->ActivatePanelButton(panelButtons_[i], rootChild);
                    panelButtons_[i]->ShowWindow();
                } else {
                    panelButtons_[i]->HideWindow();
                }
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        void* button = nullptr;
        if (GetChildAs(kRegionScreenPanelButtonIds[i], gz::kGZIID_cIGZWinBtn, &button) && button) {
            static_cast<gz::cIGZWinBtn*>(button)->SetChecked(i == panel);
            static_cast<gz::cIGZWinBtn*>(button)->Release();
        }
    }
}

bool cSC4WinRegionScreen::GZOnCommand(std::uint32_t command, std::uint64_t param) {   // @ 0x10023b92a (PARTIAL)
    IRegionScreenHost* host = RegionScreenHost();
    if (getenv("SC4_DEBUG")) fprintf(stderr, "region screen: command %#x param %#llx\n", command, static_cast<unsigned long long>(param));
    if (command == 0x287259f7) {   // a check box changed
        if (param != kRegionScreenGridCheckBoxId) return true;
        void* raw = nullptr;
        bool checked = false;
        if (GetChildAs(kRegionScreenGridCheckBoxId, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
            auto* button = static_cast<gz::cIGZWinBtn*>(raw);
            checked = button->IsChecked();
            button->Release();
        }
        if (view_) view_->EnableGrid(checked);
        if (std::uint8_t* preferences = host ? host->Preferences() : nullptr) {
            preferences[kPrefsShowGrid] = checked ? 1 : 0;
        }
        return true;
    }
    if (command != 0x287259f6) return true;   // PARTIAL: only the button command is wired
    switch (param) {
    case 0x4a560000: {   // open the selected city
        if (getenv("SC4_DEBUG")) fprintf(stderr, "open city: selected=%p\n", view_ ? (void*)view_->GetSelectedCity() : nullptr);
        RegionViewCity* selected = view_ ? view_->GetSelectedCity() : nullptr;
        // PARTIAL: the original also sets the difficulty level on the application (cISC4App vslot 0x58) first.
        RegionCityRef* cityRef = selected ? GetOrCreateRegionalCity(*selected, true) : nullptr;
        if (getenv("SC4_DEBUG")) fprintf(stderr, "open city: ref=%p\n", (void*)cityRef);
        if (cityRef && !LaunchRegionalCity(cityRef)) RestoreSignposts();
        return true;
    }
    case 0x4a560001:
        RestoreSignposts();
        return true;
    case 0x2a5b0001:   // load region: the chosen region becomes the active one and the region screen is rebuilt for it
    case 0x2a5b0002: { // delete region: the active one goes, then the same rebuild
        if (!host) return true;
        if (param == 0x2a5b0001) {
            RegionDialogNation nation;
            nation.active = host->CurrentRegionIndex();
            for (int i = 0; i < host->RegionCount(); ++i) nation.names.push_back(host->RegionNameAt(i) ? host->RegionNameAt(i) : "");
            const int index = SC4DisplayLoadRegionDialog(this, nation);
            if (getenv("SC4_DEBUG")) fprintf(stderr, "region screen: load region dialog -> %d\n", index);
            if (index < 0) return true;
            host->SelectRegion(index);                  // cISC4Nation vslot 0x24 (SetActiveRegion)
        } else {
            if (!SC4DisplayDeleteRegionDialog(this)) return true;
            host->DeleteRegion(currentRegionIndex_);    // cISC4Nation vslot 0x30
        }
        SetUserPreferencesToCurrentRegion();
        host->ReloadRegion();                           // cISC4App vslot 0x30 (0): back to a fresh region view
        return true;
    }
    case 0x26c10a3e: {   // exit: remember the region and the view, ask the app to quit (it asks the player), unpress the button
        SetUserPreferencesToCurrentRegion();
        SetUserPreferencesToCurrentPosition();
        if (host) host->RequestQuitFromRegion(true);   // cISC4App vslot 0x2c (1)
        void* raw = nullptr;
        if (GetChildAs(0x26c10a3e, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
            auto* button = static_cast<gz::cIGZWinBtn*>(raw);
            button->SetChecked(false);
            button->Release();
        }
        return true;
    }
    case 0x2a5b0000:   // new region: cSC4NewRegionDialog is not ported yet
        if (getenv("SC4_DEBUG")) fprintf(stderr, "region screen: the new-region dialog is not ported yet\n");
        return true;
    case 0x0a5510a9:   // the options menu's game options button
        if (host) host->RunGameOptionsDialog();
        return true;
    case 0xa98f4f88:   // the options menu's audio options button
        if (host) host->RunAudioOptionsDialog();
        return true;
    case 0x098f4f6c:   // the options menu's graphic options button
        if (host) host->RunGraphicOptionsDialog();
        return true;
    case kRegionScreenPanelButtonIds[0]:
        ShowPanel(0);
        return true;
    case kRegionScreenPanelButtonIds[1]:
        ShowPanel(1);
        return true;
    case kRegionScreenPanelButtonIds[2]:
        ShowPanel(2);
        return true;
    case 0x4bb92c1f:   // hide the info panels
    case 0xebb91356: {   // show the info panels
        const bool visible = param == 0xebb91356;
        panelsVisible_ = visible;
        nsGZWinUtils::SetWindowVisible(this, 0xbb0f5e7, !visible, true);
        nsGZWinUtils::SetWindowVisible(this, 0x6bb92bca, visible, true);
        return true;
    }
    case kRegionScreenSignpostCheckBoxId: {   // signpost-mode check box
        void* raw = nullptr;
        bool checked = false;
        if (GetChildAs(kRegionScreenSignpostCheckBoxId, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
            auto* button = static_cast<gz::cIGZWinBtn*>(raw);
            checked = button->IsChecked();
            button->Release();
        }
        signpostMode_ = checked ? 1 : 0;
        RestoreSignposts();
        if (std::uint8_t* preferences = host ? host->Preferences() : nullptr) {
            preferences[kPrefsSignpostMode] = checked ? 1 : 0;
        }
        return true;
    }
    case kRegionScreenMapModeButtonIds[0]:
    case kRegionScreenMapModeButtonIds[1]:
    case kRegionScreenMapModeButtonIds[2]: {
        for (int i = 0; i < 3; ++i) {
            const std::uint32_t id = kRegionScreenMapModeButtonIds[i];
            if (id == param) {
                if (view_) view_->SetMapMode(i);
                if (std::uint8_t* preferences = host ? host->Preferences() : nullptr) {
                    preferences[kPrefsMapMode] = static_cast<std::uint8_t>(i);
                }
            }
            void* raw = nullptr;
            if (GetChildAs(id, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
                auto* button = static_cast<gz::cIGZWinBtn*>(raw);
                button->SetChecked(id == param);
                button->Release();
            }
        }
        nsGZWinUtils::SetWindowVisible(this, 0xbb0f5e7, param != kRegionScreenMapModeButtonIds[0] && !panelsVisible_,
                                       true);
        nsGZWinUtils::SetWindowVisible(this, 0x6bb92bca, panelsVisible_ && param != kRegionScreenMapModeButtonIds[0],
                                       true);
        return true;
    }
    default:
        if (getenv("SC4_DEBUG")) fprintf(stderr, "region screen: button %#llx not wired\n", static_cast<unsigned long long>(param));
        break;
    }
    // PARTIAL: the new/load/delete region dialogs, the import/export city dialogs, the video / audio / game
    // preferences windows, the snapshot album and the web links (0x10023b92a is 2739 bytes of that dispatch) are not
    // recovered.
    return true;
}

// ===================================================================================================================
// Tuning / preferences
// ===================================================================================================================

void cSC4WinRegionScreen::ReadTunableValues() {   // @ 0x10023904e
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return;
    cISCPropertyHolder* holder = host->CreateTuningProperties();
    if (!holder) return;
    if (scroll_) {
        std::uint32_t deadZone = 0, maxDistance = 0;
        float speedDivisor = 0.0f;
        cSCPropertyHelp::GetPropertyValue(holder, kTuningScrollDeadZone, deadZone);
        cSCPropertyHelp::GetPropertyValue(holder, kTuningScrollMaxDistance, maxDistance);
        cSCPropertyHelp::GetPropertyValue(holder, kTuningScrollSpeedDivisor, speedDivisor);
        scroll_->SetTunables(deadZone, maxDistance, speedDivisor);
    }
    cSCPropertyHelp::GetPropertyValue(holder, kTuningCityHalfSizeX, cityHalfSizeX_);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningCityHalfSizeY, cityHalfSizeY_);
    std::uint32_t erosionClicks = 0, erosionFlowSteps = 0, erosionSmoothing = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kTuningWaterErosionClicks, erosionClicks);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningWaterErosionFlowSteps, erosionFlowSteps);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningWaterErosionSmoothing, erosionSmoothing);
    host->SetWaterErosionTunables(erosionClicks, erosionFlowSteps, erosionSmoothing);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningLabelColor1, labelColor1_);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningLabelColor2, labelColor2_);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningBaseTerrainTypeHeight, baseTerrainTypeHeight_);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningRegionHeightByte, regionHeightByte_);
    host->SetRegionHeightGlobals(baseTerrainTypeHeight_, regionHeightByte_);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningSmoothDistance, smoothDistance_);
    cSCPropertyHelp::GetPropertyValue(holder, kTuningSmoothSeconds, smoothSeconds_);
    std::uint8_t opacityLow = 0, opacityHigh = 0;
    if (host->CloudOpacityBytes(holder, opacityLow, opacityHigh) && clouds_) {
        clouds_->SetOpacityRange(opacityLow, opacityHigh);
    }
    if (view_) view_->SetLabelColours(labelColor1_, labelColor2_);
    holder->Release();
}

void cSC4WinRegionScreen::PostRegionScreenMessage(std::uint32_t type) {   // @ 0x1002388d2
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return;
    cIGZMessageServer2* server = host->MessageServer2();
    if (!server) return;
    cIGZMessage2Standard* message = host->CreateStandardMessage();
    if (!message) return;
    message->SetType(type);
    server->MessagePost(message, false);
    message->Release();
}

void cSC4WinRegionScreen::ShutdownApplicationServices() {   // @ 0x100239dac
    IRegionScreenHost* host = RegionScreenHost();
    if (!host) return;
    host->ClearEffectsManagerGlobal();
    if (effects_) {
        effects_->Shutdown();
        effects_ = nullptr;
    }
    host->ClearViewGlobals();
    if (renderer_) {
        renderer_->Shutdown();
        renderer_ = nullptr;
    }
    if (cameraControl_) {
        cameraControl_->Shutdown();
        cameraControl_->Release();
        cameraControl_ = nullptr;
    }
    if (camera_) {
        camera_->Shutdown();
        if (camera_->Release() == 0) delete camera_;
        camera_ = nullptr;
    }
    host->ClearTerrainMapGlobal();
    if (terrainMap_) {
        terrainMap_->Shutdown();
        terrainMap_->Release();
        terrainMap_ = nullptr;
    }
    if (frameService_) {
        frameService_->Shutdown();
        frameService_ = nullptr;
    }
}

// ===================================================================================================================
// Test access
// ===================================================================================================================

void cSC4WinRegionScreen::SetBounds(int minX, int minY, int maxX, int maxY) {
    boundMinX_ = minX;
    boundMinY_ = minY;
    boundMaxX_ = maxX;
    boundMaxY_ = maxY;
}

void cSC4WinRegionScreen::SetSnapshotBuffer(int slot, IWinBuffer* buffer) {
    if (slot < 0 || slot >= static_cast<int>(snapshots_.size())) return;
    if (buffer) buffer->AddRef();
    ReleaseBuffer(snapshots_[slot]);
    snapshots_[slot] = buffer;
}

void cSC4WinRegionScreen::SetMemberWindow(int which, cIGZWin* window) {
    cIGZWin** slot = nullptr;
    if (which >= 0 && which < 3) {
        slot = &panels_[which];
    } else if (which >= 3 && which < 6) {
        slot = &panelButtons_[which - 3];
    } else if (which == 6) {
        slot = &backdrop_;
    } else if (which == 7) {
        slot = &unused280_;
    }
    if (!slot) return;
    if (window) window->AddRef();
    ReleaseWindow(*slot);
    *slot = window;
}

void cSC4WinRegionScreen::SetClouds(cSC4WinRegionClouds* clouds) {
    if (clouds == clouds_) return;
    if (clouds) clouds->AddRef();
    if (clouds_) {
        ChildDeleteAbsolute(clouds_);   // it is a child whenever Init created it
        clouds_->Release();
    }
    clouds_ = clouds;
}

void cSC4WinRegionScreen::SetScroll(cSC4WinRegionScroll* scroll) {
    if (scroll == scroll_) return;
    if (scroll) scroll->AddRef();
    if (scroll_) {
        ChildDeleteAbsolute(scroll_);   // it is a child whenever Init created it
        scroll_->Release();
    }
    scroll_ = scroll;
}

void cSC4WinRegionScreen::SetView(IRegionView* view) {
    if (view == view_) return;
    if (view_) {
        if (cIGZWin* window = view_->AsWindow()) ChildDeleteAbsolute(window);
        delete view_;
    }
    view_ = view;
}

}  // namespace sc4
