// cSC4View3DWin — see View3DWin.h for the layout, the vtable groups, the seams and the quirks.
#include "sc4/View3DWin.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "gz/CommandParameterSet.h"
#include "gz/cIGZVariant.h"
#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinUtils.h"
#include "sc4/WinAuraAlert.h"
#include "sc4/SC4UI.h"
#include "sc4/SLBuffer.h"
#include "sc4/WinCatalogView.h"
#include "sc4/WinTertiaryMenuFrame.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/SCResParser.h"
#include "s3d/ModelInstance.h"

namespace sc4 {

namespace {

IView3DWinHost* g_host = nullptr;

// cRZMessage2ChangeInputControl (vtable 0x100b18710, 0x58 bytes): a cRZMessage2Standard with one extra
// referenced tool at +0x50. Only the part cSC4View3DWin::SendViewInputControlChangeMessage uses is here.
class cRZMessage2ChangeInputControl : public cRZMessage2Standard {
public:
    cRZMessage2ChangeInputControl() = default;                       // @ 0x10004136a + the vtable store
    ~cRZMessage2ChangeInputControl() override { SetNext(nullptr); }   // @ 0x100041906 releases the tool
    void SetNext(cSC4ViewInputControl* next) {
        if (next == next_) return;
        if (next) next->AddRef();
        cSC4ViewInputControl* old = next_;
        next_ = next;
        if (old) old->Release();
    }

private:
    cSC4ViewInputControl* next_ = nullptr;      // +0x50
};

}  // namespace

const std::uint32_t kView3DMessageTypes[kView3DMessageTypeCount] = {
    // Same table, with external linkage (the header declares it).
    0xa6b79602, 0xa6b79612, 0xa6b79621, 0x06cdb65b, 0xaa7fb7e0, 0x4a7fb7e2, 0x4a7fb7e9, 0xabbbca64, 0x26d31ec0,
    0x26d31ec1, 0x26d31ec4, 0x26d31ec5, 0x0ab99380, 0x26c63341, 0x26c63343, 0x26c63344, 0x0772fad4, 0x08dac3e8,
    0x08dac3e7, 0x66956816, 0x66956814, 0x44778231, 0x44778233, 0x26d31ec2, 0xaa5cf1e3, 0xca1b1e64, 0xaa1b2002,
    0x4a44667f, 0xac083d7d, 0x6a935c34, 0x6a935ca4, 0x6a935ca8, 0x6a935cab, 0x6a935cad, 0x6a935cb0, 0x6a935cb2,
    0x6a935cb5, 0x6a935cb7, 0x6a935cba, 0x6a935cbd, 0x6a935cbf, 0x6a935cc2, 0x6a935cc4, 0x6a935cc7, 0x2bcbd5f1,
    0x6a935cc9, 0x6a935ccc, 0x6a935cd5, 0x6a935cd1, 0x6a935cd8, 0x2a9484da, 0x2a948275, 0x6a935cdd, 0x2a9484f2,
    0x2a948272, 0x6a935ce0, 0x2a9484ff, 0x2a94826e, 0x6a935ce2, 0x2a94850b, 0x2a94826b, 0x6a935ce5, 0x6a935ce8,
    0x6a935cf1, 0x6a935cf3, 0x6a935cf4, 0x6a935cf7, 0x6a935d0b, 0x6a935d0e, 0x6a935d11, 0x6a935d17, 0x6a935d1a,
    0x6a935d1d, 0x6a935d25, 0x6a935d2b, 0x6a935d28, 0x6a935d2e, 0x6a935d36, 0x6a935d39, 0xca935dbd, 0x6a935dc0,
    0x6a935dc2, 0x6a935dc5, 0x6b730f78, 0x6a935dc9, 0x6a935dda, 0x2b79e72b, 0x6a935ddd, 0x6a935ddf, 0x2bfe11c9,
    0x6a935de3, 0x6a935de6, 0x6a935deb, 0x6a935dee, 0x6a935df1, 0x6a935df4, 0x2bfe0bea, 0x6a935df7, 0x6a935dfa,
    0x6a935dfd, 0x6a935e0a, 0x6a935e0d, 0x6a935e14, 0x6a935e11, 0x6a935e17, 0x2bfe1095, 0xec3e82f8, 0x6a935e1e,
    0x6a935e21, 0x6a935e27, 0x6a935e2a, 0x6a935e2d, 0x6a935e34, 0x6a935e45, 0x6a935e4b, 0x6a935e36, 0x6a935e37,
    0x6a935e39, 0x6a935e3c, 0x6a935e3f, 0x2a94a04b, 0x2a94a5b6, 0x6aa9fe51, 0x0b980581, 0x0b980587, 0x6b98058a,
    0x6a9391a0, 0x6a9753ea, 0x6a9753ed, 0x6a9757c2, 0xcab22c64, 0xabcdee11};

IView3DWinHost* View3DWinHost() { return g_host; }
void SetView3DWinHost(IView3DWinHost* host) { g_host = host; }

// ==================================================================================================== lifecycle

cSC4View3DWin::cSC4View3DWin(cIGZWinMgr* mgr) : cGZWin(mgr) {
    host_ = g_host;
    if (host_) {
        notifications_ = host_->NotificationHelper();
        if (notifications_) notifications_->AddMessageTypes(kView3DMessageTypes, kView3DMessageTypeCount);
    }
    // The remaining constructor stores are the member initialisers above: the original zeroes
    // +0x168..+0x6aa in this same order and leaves the rest of the GUI blocks at +0x270..+0x4d0
    // (bzero 0xb8 at +0x270, the seven panel vectors at +0x298, the five windows at +0x328, the
    // panel ids at +0x3e4..+0x430) for AddGUI, which is not recovered.
    if (host_) host_->OnView3DCreate(this);
}

cSC4View3DWin::~cSC4View3DWin() {
    // The original's destructor (0x10077f6f6) releases only the objects it owns outright: the mission
    // panel (+0x640), the validation window (+0x648), the coverage helper's destructor (+0x5f8) and the
    // cursor-text / cost-indicator windows the port does not model (+0x5c0..+0x5f0). The camera, the
    // renderer, the model maker, the key accelerator and the progress screen are released by Shutdown,
    // and the two tool vectors are freed without releasing their tools.
    if (missionPanel_) { missionPanel_->Release(); missionPanel_ = nullptr; }
    if (validationWindow_) { validationWindow_->Release(); validationWindow_ = nullptr; }
    if (coverageHelper_) { coverageHelper_->Shutdown(); coverageHelper_ = nullptr; }
    persistentTools_.clear();
    tools_.clear();
    deferredMessages_.clear();
}

bool cSC4View3DWin::QueryInterface(std::uint32_t iid, void** out) {
    void* result = nullptr;
    if (static_cast<std::int32_t>(iid) < 1) {                  // 0x10078145c: the id is compared as signed
        if (iid == kGZIID_cIGZWinMessageFilter) {
            result = static_cast<cIGZWinMessageFilter*>(this);
        } else if (iid != kGZIID_cISC4View3DWin) {
            return false;
        } else {
            result = static_cast<cISC4View3DWin*>(this);
        }
    } else if (iid == 1) {
        result = static_cast<cISC4View3DWin*>(this);
    } else if (iid == kGZIID_cIGZWin) {
        result = static_cast<cIGZWin*>(static_cast<cGZWin*>(this));
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        result = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    *out = result;
    AddRef();
    return true;
}

// ==================================================================================================== Init

bool cSC4View3DWin::Init() {
    if (cGZWin::get_init_flag()) return initialized_;
    if (notifications_) notifications_->AddAllNotifications(static_cast<cIGZMessageTarget2*>(this));
    if (host_) host_->RegisterCommandTable();
    if (host_ && host_->CheatCodeManagerPresent()) {
        host_->AddCheatCodeTarget(static_cast<cIGZWin*>(static_cast<cGZWin*>(this)));
        for (int i = 0; i < 10; ++i) host_->RegisterCheatCode(kView3DCheatCodes[i], kView3DCheatStrings[i]);
        // 0x10077f5a0: the lot / building plop and toll booth cheats only exist when the app allows them.
        if (host_->LotPlopCheatsEnabled())
            for (int i = 10; i < 13; ++i) host_->RegisterCheatCode(kView3DCheatCodes[i], kView3DCheatStrings[i]);
    }
    initialized_ = cGZWin::Init();
    SetCaption(cRZString("cSC4View3DWin"));                                  // vslot 0x250, 0x10077fe0a
    SetID(kView3DWindowId);                                                 // vslot 0x200
    cIGZWin* main = GetMainWindow();
    SetArea(0, 0, main ? main->GetW() : 0, main ? main->GetH() : 0);        // vslot 0x1b0
    if (cIGZWinMgr* manager = GetWindowManager())
        if (cIGZCursorManager* cursors = manager->GetCursorManager())
            SetCursor(cursors->LoadCursor(kView3DCursorId), false);         // vslot 0x2a8(id, false)
    if (coverageHelper_) coverageHelper_->Init();
    paintEnabled_ = false;                                                  // +0x244
    frameTimer_.Start();
    if (host_) {
        keyAccelerator_ = host_->CreateKeyAccelerator();                     // exemplar 0x42e3ea4b + GZCOM
        CreateProgressScreen(host_->ProgressScreenWanted());                 // @ 0x100780910
    }
    return initialized_;
}

bool cSC4View3DWin::Shutdown() {
    if (!initialized_) return true;
    paintEnabled_ = false;                                                  // +0x244
    if (validationWindow_) {
        if (host_) host_->ShutdownValidationWindow(validationWindow_);
        validationWindow_->Release();
        validationWindow_ = nullptr;
    }
    if (host_ && host_->CheatCodeManagerPresent()) {
        host_->RemoveCheatCodeTarget(static_cast<cIGZWin*>(static_cast<cGZWin*>(this)));
        for (std::uint32_t id : kView3DCheatCodes) host_->UnregisterCheatCode(id);
    }
    RemoveAllViewInputControls(true);
    RemoveAllPersistentViewInputControls();
    if (keyAccelerator_) { keyAccelerator_->Release(); keyAccelerator_ = nullptr; }
    if (cIGZWin* recorder = GetChildWindowFromID(kView3DAnimationRecorderId)) ChildRemove(recorder);
    if (host_) host_->ShutdownLotConfigurationEditors();                    // +0x270 / +0x278
    if (coverageHelper_) coverageHelper_->Shutdown();
    RemoveGUI();                                                            // @ 0x100780eaa
    if (progressScreen_) {
        if (host_) host_->ShutdownProgressScreen(progressScreen_);
        progressScreen_->Release();
        progressScreen_ = nullptr;
    }
    if (host_) host_->UnregisterCommandTable();
    if (notifications_) notifications_->RemoveAllNotifications(static_cast<cIGZMessageTarget2*>(this));
    if (host_) host_->OnView3DShutdown();
    if (windowService_) { windowService_->Release(); windowService_ = nullptr; }
    if (modelMaker_) {
        if (host_) host_->ShutdownModelMaker(modelMaker_);
        modelMaker_->Release();
        modelMaker_ = nullptr;
    }
    if (simulationClock_) {
        if (host_) host_->ShutdownSimulationClock(simulationClock_);
        simulationClock_->Release();
        simulationClock_ = nullptr;
    }
    if (renderer_) { renderer_->Shutdown(); renderer_->Release(); renderer_ = nullptr; }
    if (filterInfo_) { filterInfo_->Release(); filterInfo_ = nullptr; }
    if (cameraControl_) { cameraControl_->Shutdown(); cameraControl_->Release(); cameraControl_ = nullptr; }
    if (cameraObject_) {
        if (host_) host_->ShutdownCameraObject(cameraObject_);
        cameraObject_->Release();
        cameraObject_ = nullptr;
    }
    if (simulatorToPause_) { simulatorToPause_->Release(); simulatorToPause_ = nullptr; }
    initialized_ = false;
    return cGZWin::Shutdown();
}

// ==================================================================================================== accessors

cIGZWin* cSC4View3DWin::AsIGZWin() {
    // The original answers this+0x18, its cGZWin subobject.
    return static_cast<cIGZWin*>(static_cast<cGZWin*>(this));
}

void cSC4View3DWin::AttachRenderStack(IRenderer3D* renderer, cIGZUnknown* camera, cSC4CameraControl* control) {
    if (renderer) renderer->AddRef();
    if (renderer_) renderer_->Release();
    renderer_ = renderer;
    if (camera) camera->AddRef();
    if (cameraObject_) cameraObject_->Release();
    cameraObject_ = camera;
    if (control) control->AddRef();
    if (cameraControl_) cameraControl_->Release();
    cameraControl_ = control;
}

void* cSC4View3DWin::GetRenderer() { return renderer_; }                    // +0x188
cIGZUnknown* cSC4View3DWin::GetModelMaker() { return modelMaker_; }         // +0x190 (400 decimal)
cIGZUnknown* cSC4View3DWin::GetCoverageHelper() { return coverageHelper_; } // +0x5f8

cIGZUnknown* cSC4View3DWin::GetViewManager() {
    if (!viewManagerSource_) return nullptr;
    void* manager = nullptr;
    if (!viewManagerSource_->QueryInterface(kView3DViewManagerIid, &manager)) return nullptr;
    auto* unknown = static_cast<cIGZUnknown*>(manager);
    unknown->AddRef();
    return unknown;
}

void cSC4View3DWin::DisplayTerrainPickDebugString(int, int) {}              // @ 0x100783d74: empty in the original
void cSC4View3DWin::SetErrorReportString(const char*) {}                    // @ 0x100781454: empty in the original
bool cSC4View3DWin::GetTerrainQueryEnabled() { return terrainQueryEnabled_; }   // +0x1ca

std::uint32_t cSC4View3DWin::GetLastSelectedBuilding() {
    return hasLastSelectedBuilding_ ? lastSelectedBuilding_ : 0;            // 0x100793ae0
}
std::uint32_t cSC4View3DWin::GetLastSelectedLot() {
    return hasLastSelectedLot_ ? lastSelectedLot_ : 0;                      // 0x100793afa
}
void cSC4View3DWin::SetLastSelectedBuilding(std::uint32_t building) {
    lastSelectedBuilding_ = building;                                        // 0x100793b14
    hasLastSelectedBuilding_ = true;
}
void cSC4View3DWin::SetLastSelectedLot(std::uint32_t lot) {
    lastSelectedLot_ = lot;                                                  // 0x100793b28
    hasLastSelectedLot_ = true;
}

// ==================================================================================================== camera

bool cSC4View3DWin::SetZoom(int zoom) {
    // 0x100781e1a: clamp to +0x520 / +0x524, then ask the renderer.
    int clamped = zoom;
    if (zoomMin_ > zoom) clamped = zoomMin_;
    else if (zoomMax_ <= zoom) clamped = zoomMax_;
    if (!renderer_) return false;
    const bool changed = renderer_->SetZoom(clamped);
    if (changed) DoTemporarySimHiddenPause();
    return changed;
}

bool cSC4View3DWin::SetZoomLimits(int minZoom, int maxZoom) {
    zoomMin_ = minZoom;
    zoomMax_ = maxZoom;
    if (maxZoom > 4) zoomMax_ = host_ ? host_->VirtualZoomLimit() : 5;
    if (!cameraControl_) return true;
    const int virtualZoom = cameraControl_->GetVirtualZoom();
    // Quirk (0x100781e96): the comparison uses the argument, not the stored +0x524.
    int target = maxZoom;
    if (virtualZoom <= maxZoom) target = virtualZoom;
    if (virtualZoom < minZoom) target = minZoom;
    if (target != virtualZoom) SetZoom(target);
    return true;
}

bool cSC4View3DWin::CanZoomIn() {
    return cameraControl_ && cameraControl_->GetVirtualZoom() < zoomMax_;    // 0x100781ef2
}
bool cSC4View3DWin::CanZoomOut() {
    return cameraControl_ && zoomMin_ < cameraControl_->GetVirtualZoom();    // 0x100781f18
}

bool cSC4View3DWin::SetRotation(int rotation) {
    if (!renderer_) return false;                                           // 0x100781f3e
    const bool changed = renderer_->SetRotation(rotation);
    if (changed) DoTemporarySimHiddenPause();
    return changed;
}
bool cSC4View3DWin::RotateLeft() {
    if (!renderer_) return false;                                           // 0x100781f6e
    const bool changed = renderer_->RotateLeft();
    if (changed) DoTemporarySimHiddenPause();
    return changed;
}
bool cSC4View3DWin::RotateRight() {
    if (!renderer_) return false;                                           // 0x100781f9e
    const bool changed = renderer_->RotateRight();
    if (changed) DoTemporarySimHiddenPause();
    return changed;
}
bool cSC4View3DWin::ZoomIn() {
    if (!cameraControl_ || !renderer_) return false;                        // 0x100781fce
    if (cameraControl_->GetVirtualZoom() >= zoomMax_) return false;
    if (!renderer_->ZoomIn()) return false;
    DoTemporarySimHiddenPause();
    return true;
}
bool cSC4View3DWin::ZoomOut() {
    if (!cameraControl_ || !renderer_) return false;                        // 0x100782014
    if (zoomMin_ >= cameraControl_->GetVirtualZoom()) return false;
    if (!renderer_->ZoomOut()) return false;
    DoTemporarySimHiddenPause();
    return true;
}
bool cSC4View3DWin::SetZoomAndRotation(int zoom, int rotation) {
    if (!renderer_) return false;                                           // 0x10078205a
    int clamped = zoom;
    if (zoomMin_ > zoom) clamped = zoomMin_;
    else if (zoomMax_ <= zoom) clamped = zoomMax_;
    const bool changed = renderer_->SetZoomAndRotation(clamped, rotation);   // only the zoom is clamped
    if (changed) DoTemporarySimHiddenPause();
    return changed;
}

bool cSC4View3DWin::ProcessCommand(std::uint32_t command, cIGZCommandParameterSet&, cIGZCommandParameterSet& out) {   // 0x10078e2e0
    // The commands that read parameters take them from the second set: parameter 0 / 1 as bools (vslot 0x50), parameter 0 as the
    // lot of 0xec3e82f8 (vslot 0x258).
    cIGZVariant* first = out.GetParameterCount() > 0 ? out.GetParameter(0) : nullptr;
    cIGZVariant* second = out.GetParameterCount() > 1 ? out.GetParameter(1) : nullptr;
    const bool param0 = first && first->GetValBool();
    const bool param1 = second && second->GetValBool();
    const std::uint32_t item = first ? first->AsUint32() : 0;   // vslot 0x258: the stored word, no conversion
    return ProcessViewCommand(command, param0, param1, item);
}

namespace {
// The lots the 0x6a2b245f catalog commands place (InvokeCatalogItemCommand(0x6a2b245f, lot)).
struct PlaceLotCommand { std::uint32_t command, lot; };
constexpr PlaceLotCommand kPlaceLotCommands[] = {
    {0x6a935dfdu, 0x01000007u}, {0x6a935e0au, 0x2a2735a0u}, {0x6a935e0du, 0xea273867u}, {0x6a935e11u, 0x4a2af441u},
    {0x6a935e14u, 0x89dd094eu}, {0x6a935e17u, 0x4a2829d1u}, {0x6a935dc9u, 0x05000001u}, {0x6a935dddu, 0x6a8a358fu},
    {0x6a935ddfu, 0x6a3855d4u}, {0x6a935de6u, 0x05000002u}, {0x6a935df1u, 0x69de426bu}, {0x6a935df4u, 0x01000005u},
    {0x6a935df7u, 0x01000003u}, {0x6a935dfau, 0xa9dd1e4cu}, {0x2bfe0beau, 0x6bb942d7u}, {0x2bfe1095u, 0x0b96ae64u},
    {0x2bfe11c9u, 0x4bafba9fu},
};
// The network tools (new cSC4ViewInputControlNetworkTool(type), stacked with operation 2 when SL::spCity vslot 0xb0 allows it).
struct NetworkCommand { std::uint32_t command; int type; };
constexpr NetworkCommand kNetworkCommands[] = {
    {0x6a935dc0u, 0}, {0x6a935ddau, 1}, {0x6a935dc5u, 2}, {0x6a935dc2u, 3}, {0x6a935deeu, 4}, {0x6a935debu, 5}, {0x6b730f78u, 6},
    {0x6a935de3u, 7}, {0x2b79e72bu, 8}, {0x8be098f4u, 9}, {0x4be098f7u, 10}, {0x6be098fau, 11}, {0x4be098fdu, 12},
};
// The zoning tools (InvokeZoningTool(type)).
struct ZoneCommand { std::uint32_t command, type; };
constexpr ZoneCommand kZoneCommands[] = {
    {0x6a935d0bu, 2}, {0x6a935d0eu, 3}, {0x6a935d11u, 4}, {0x6a935d17u, 5}, {0x6a935d1au, 6}, {0x6a935d1du, 7}, {0x6a935d25u, 8},
    {0x6a935d28u, 0xe}, {0x6a935d2bu, 9}, {0x6a935d2eu, 0}, {0x6a935cf7u, 1},
};
// The panel toggles the keyboard reaches (the button found by id, iid 0x8810, then flipped; its notification does the rest).
struct PanelToggleCommand { std::uint32_t command, button; };
constexpr PanelToggleCommand kPanelToggleCommands[] = {
    {0x6a935c34u, 0x2988bc85u}, {0x6a935ca4u, 0xc988bc79u}, {0x6a935ca8u, 0x4988bc6au}, {0x6a935cabu, 0x8988bc94u},
};
}  // namespace

// ProcessCommand's body (0x10078e2e0). The original's binary-search tree of compares is a flat switch here; every command returns 0.
bool cSC4View3DWin::ProcessViewCommand(std::uint32_t command, bool param0, bool param1, std::uint32_t item) {
    const bool keyboardIdle = keyboardScrollX_ == 0 && keyboardScrollY_ == 0;   // +0x504 / +0x508
    auto stepScroll = [&](float angle) {
        if (keyboardIdle && cameraControl_) cameraControl_->ScrollByViewportAngle(angle, 1.0f);
        return false;
    };
    auto keyboardScroll = [&](int& axis, int value) {
        if (!keyboardIdle) return false;   // a held direction blocks the others until it is released
        axis = value;
        RecalcScrollVelocity();
        return false;
    };
    auto cityAllowsTools = [&] { return host_ && host_->CanInvokeZoningTool(); };   // SL::spCity vslot 0xb0
    // new tool, AddRef, SetCurrentViewInputControl(tool, operation), Release
    auto stack = [&](cSC4ViewInputControl* tool, int operation) {
        if (!tool) return false;
        tool->AddRef();
        SetCurrentViewInputControl(tool, operation);
        tool->Release();
        return false;
    };
    auto speed = [&](int value) {                                        // 0x6a935cb0 / cb2 / cb5
        if (!simulatorToPause_) return false;
        simulatorToPause_->SetSpeed(value);                             // vslot 0xe0
        if (simulatorToPause_->IsPaused()) simulatorToPause_->UserResume();   // vslot 0x78 -> 0x60
        UpdateSpeedButtons();
        return false;
    };

    for (const PlaceLotCommand& entry : kPlaceLotCommands)
        if (entry.command == command) { InvokeCatalogItemCommand(0x6a2b245fu, entry.lot, false); return false; }
    for (const NetworkCommand& entry : kNetworkCommands)
        if (entry.command == command) return cityAllowsTools() ? stack(host_->CreateNetworkTool(entry.type), 2) : false;
    for (const ZoneCommand& entry : kZoneCommands)
        if (entry.command == command) { InvokeZoningTool(entry.type); return false; }
    for (const PanelToggleCommand& entry : kPanelToggleCommands) {
        if (entry.command != command) continue;
        void* raw = nullptr;
        if (panelWindow_ && panelWindow_->GetChildAsRecursive(entry.button, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
            auto* button = static_cast<gz::cIGZWinBtn*>(raw);
            if (!button->GetBtnFlag(1)) button->ToggleOn();             // vslot 0x20(1) -> 0x30 / 0x38
            else button->ToggleOff();
            button->Release();
        }
        return false;
    }

    switch (command) {
        // --- scrolling, zoom, rotation
        case 0x6a935cd8: return keyboardScroll(keyboardScrollX_, -1);   // scroll left
        case 0x6a935cdd: return keyboardScroll(keyboardScrollY_, -1);   // scroll up
        case 0x6a935ce0: return keyboardScroll(keyboardScrollX_, 1);    // scroll right
        case 0x6a935ce2: return keyboardScroll(keyboardScrollY_, 1);    // scroll down
        case 0x2a94826b: case 0x2a94826e: case 0x2a948272: case 0x2a948275:   // a scroll key went up
            keyboardScrollX_ = 0;
            keyboardScrollY_ = 0;
            RecalcScrollVelocity();
            return false;
        case 0x2a9484da: return stepScroll(3.1415927f);
        case 0x2a9484f2: return stepScroll(4.712389f);
        case 0x2a9484ff: return stepScroll(0.0f);
        case 0x2a94850b: return stepScroll(1.5707964f);
        case 0x6a935ce5: {                                              // centre the view on the terrain under the cursor
            int x = 0, y = 0;
            if (!host_ || !renderer_ || !cameraControl_) return false;
            host_->CursorOverView(this, x, y);                          // RZ::WM() vslot 0x110 (screen to window)
            cS3DVector3 target;                                         // nS3D::k_zero x3
            if (!renderer_->PickTerrain(x, y, &target.x, false, true)) return false;   // vslot 0x1e8
            cameraControl_->SetViewTargetPosition(target);
            return false;
        }
        case 0x6a935ce8:                                                // post 0x277a9364 on SL::spMS2
            if (cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr) {
                auto* message = new cRZMessage2Standard();
                message->AddRef();
                message->SetType(0x277a9364u);
                server->MessagePost(message, false);
                message->Release();
            }
            return false;
        case 0x6a935cb7: ZoomIn(); return false;                        // vslot 0x48
        case 0x6a935cba: ZoomOut(); return false;                       // vslot 0x50
        case 0x6a935cbd: SetZoom(0); return false;                      // vslot 0x40
        case 0x6a935cbf: SetZoom(1); return false;
        case 0x6a935cc2: SetZoom(2); return false;
        case 0x6a935cc4: SetZoom(3); return false;
        case 0x6a935cc7: SetZoom(4); return false;
        case 0x2bcbd5f1: SetZoom(5); return false;
        case 0x6a935cc9: RotateLeft(); return false;                    // vslot 0x78
        case 0x6a935ccc: RotateRight(); return false;                   // vslot 0x80
        case 0x6a9391a0: if (renderer_) renderer_->ForceFullRedraw(); return false;   // vslot 0xa0

        // --- the simulation speed
        case 0x6a935cad:                                                // pause / resume
            if (simulatorToPause_) {
                if (!simulatorToPause_->IsPaused()) simulatorToPause_->UserPause();   // vslot 0x78 -> 0x48
                else simulatorToPause_->UserResume();                                 // -> 0x60
            }
            return false;
        case 0x6a935cb0: return speed(0);
        case 0x6a935cb2: return speed(1);
        case 0x6a935cb5: return speed(2);

        // --- the query and demolish tools (operation 0), after dropping a current tool that does not stack
        case 0x6a935cf3: case 0x6a935cf4: case 0x6a935d36: {
            if (!cityAllowsTools()) return false;
            cSC4ViewInputControl* current = GetCurrentViewInputControl();
            if (current && !current->ShouldStack()) {                   // vslot 0x60
                RemoveCurrentViewInputControl(false);                   // vslot 0xd0
                current = GetCurrentViewInputControl();
            }
            if (command == 0x6a935cf3) {
                if (current && current->GetID() == 0xc7af928eu) return false;   // already running
                return stack(host_->CreateQueryTool(0, 0x32), 0);
            }
            if (command == 0x6a935cf4) {
                if (current && current->GetID() == 0xc7af928fu) return false;
                return stack(host_->CreateQueryTool(1, 0x32), 0);
            }
            if (current && current->GetID() == 0x46ddb5f1u) return false;
            return stack(host_->CreateDemolishTool(), 0);
        }
        case 0x6a935cf1:                                                // escape: the tool, else the tertiary then the secondary menu
            if (!tools_.empty()) RemoveAllViewInputControls(false);
            else if (tertiaryMenu_) CloseTertiaryMenu();
            else if (secondaryMenu_) CloseSecondaryMenu();
            return false;

        // --- dispatch, signs, labels
        case 0xca935dbd: case 0x6a935d39:                               // the emergency dispatch tools 0 / 1
            if (!cityAllowsTools()) return false;
            return stack(host_ ? host_->CreateDispatchTool(command != 0xca935dbdu ? 1 : 0) : nullptr, 2);
        case 0x8c0b0e46: case 0xec0b0e6a:                               // dispatch tools 5 / 6 (no city check)
            return stack(host_ ? host_->CreateDispatchTool(command == 0xec0b0e6au ? 6 : 5) : nullptr, 2);
        case 0x0b980581: return stack(host_ ? host_->CreateSignTool() : nullptr, 2);
        case 0x0b980587: return stack(host_ ? host_->CreateLabelTool() : nullptr, 2);
        case 0xabcdee11: return stack(host_ ? host_->CreateSignLabelDemolishTool() : nullptr, 2);
        case 0x6b98058a: {                                              // signs and labels on / off
            if (!host_) return false;
            if (cSC4ViewInputControl* current = GetCurrentViewInputControl()) {
                const std::uint32_t id = current->GetID();
                if (host_->SignsAndLabelsVisible() && (id == 0x8b3ce416u || id == 0xcbcc987eu || id == 0x8b4ce5a0u))
                    RemoveAllViewInputControls(false);                  // a sign / label tool goes with them
            }
            host_->ToggleSignsAndLabels();
            return false;
        }

        // --- the city
        case 0x2a94a5b6: if (cityAllowsTools()) ObliterateCity(); return false;
        case 0x2a94a04b: ToggleContourDisplay(); return false;
        case 0xcab22c64: AddWinAnimationRecorder(true); return false;
        case 0xec3e82f8: if (item != 0) InvokeCatalogItemCommand(0x6a2b245fu, item, false); return false;
        case 0x6a935e1e: DoDayNightMenu(0xca35cb76u); return false;
        case 0x6a935e21: DoDayNightMenu(0xca35cb78u); return false;
        case 0x6a935e27: DoDayNightMenu(0xca35cb74u); return false;
        case 0x6a935e3f: if (panelWindow_) MinimizeUI(panelWindow_->IsVisible()); return false;   // vslot 0x1b8(panel vslot 0x238)
        case 0x6aa9fe51:                                                // SL::spPreferences byte 0xf0a flips
            if (std::uint8_t* preferences = host_ ? host_->Preferences() : nullptr) preferences[0xf0a] = preferences[0xf0a] == 0;
            return false;

        // --- the animation system (only with byte +0x1c8 set)
        case 0x6a9753ea:
            if ((unknown1c8_ & 0xff) && host_) host_->PauseAnimationSystem(!host_->AnimationSystemPaused());
            return false;
        case 0x6a9753ed:
            if ((unknown1c8_ & 0xff) && host_) host_->StepAnimationSystem();
            return false;

        // --- dialogs and the app
        case 0x6a935e2a: if (host_) host_->RunPreferencesDialog(0); return false;   // game
        case 0x6a935e2d: if (host_) host_->RunPreferencesDialog(1); return false;   // video
        case 0x6a935e34: if (host_) host_->RunPreferencesDialog(2); return false;   // audio
        case 0x6a935e45: if (host_) host_->ShowSnapshotAlbum(this); return false;
        case 0x6a935e4b: if (host_) host_->CreateSnapshotTaker(this); return false;
        case 0x6a935e36: case 0x6a935e37:                               // save (0x37: the alternate save without the key)
            if (host_) host_->RequestSaveCity(!param0, command == 0x6a935e37 ? true : host_->IsKeyDownNow(0x12));
            return false;
        case 0x6a935e39: if (host_) host_->RequestGoToRegionView(!param0); return false;
        case 0x6a935e3c:
            if (host_ && host_->PopupModalDialogsSafe()) host_->RequestQuit(!param0, param1);
            return false;
        default: return false;
    }
}

// ObliterateCity (0x10078fe78): the confirmation dialog, then the obliterate button goes grey, the budget panel closes and the
// cSC4ObliterateCity tool takes the view.
void cSC4View3DWin::ObliterateCity() {
    if (nSC4UI::DoScriptedModalDialog(0x2a41436cu, 0x27df05beu) != 0x6a41bbb5u) return;
    if (panelWindow_) nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelObliterateButtonId, false);
    if (currentPanel_ == kPanelBudgetIndex) HidePanel(kPanelBudgetIndex);   // +0x350
    if (cSC4ViewInputControl* tool = host_ ? host_->CreateObliterateCityTool() : nullptr) {
        tool->AddRef();
        SetCurrentViewInputControl(tool, 2);
        tool->Release();
    }
}

cIGZWinKeyAccelerator* cSC4View3DWin::GetKeyAccelerator() const { return keyAccelerator_; }   // 0x100781ab2

void cSC4View3DWin::SetKeyAccelerator(cIGZWinKeyAccelerator* accelerator) {   // 0x100781ac0
    cIGZWinKeyAccelerator* old = keyAccelerator_;
    if (old == accelerator) return;
    if (accelerator) accelerator->AddRef();
    if (old) old->Release();
    keyAccelerator_ = accelerator;
}

// ==================================================================================================== scrolling

void cSC4View3DWin::SetScrolling(bool scrolling, float angle, float speed) {   // 0x100781b06
    if (!scrolling_ && scrolling) DoScrollBegin();
    else if (scrolling_ && !scrolling) DoScrollEnd();
    scrolling_ = scrolling;
    scrollAngle_ = angle;
    scrollSpeed_ = speed;
    scrollStopping_ = false;
}

bool cSC4View3DWin::ScrollStop() {                                          // 0x100781cb4
    if (!scrolling_) return true;
    scrollStopping_ = true;
    float speed = scrollStopFactor_;
    if (scrollStopScaled_) speed *= scrollSpeed_;
    scrollStopSpeed_ = speed;
    return true;
}

void cSC4View3DWin::KillKeyboardScrolling() {                               // 0x100781cee
    keyboardScrollX_ = 0;
    keyboardScrollY_ = 0;
    RecalcScrollVelocity();
}

void cSC4View3DWin::RecalcScrollVelocity() {                                // 0x100781d04
    const int keyboardX = keyboardScrollX_;
    int x = edgeScrollX_;
    int y = edgeScrollY_;
    const bool keyboard = (keyboardX != 0) || (keyboardScrollY_ != 0);
    const int zoom = cameraControl_ ? cameraControl_->Zoom() : 0;
    float speedScale = 0.0f;
    if (cameraControl_) speedScale = std::pow(kScrollSpeedBase, static_cast<float>(zoom));
    if (host_) {
        if (const std::uint8_t* preferences = host_->Preferences())
            if (preferences[kPrefsEdgeScrollEnabled]) edgeScrollX_ = 0, edgeScrollY_ = 0;
    }
    if (keyboardX != 0) x = keyboardX;
    if (keyboardScrollY_ != 0) y = keyboardScrollY_;
    if (x != 0 || y != 0) {
        const float speed = keyboard ? keyboardScrollSpeed_ : edgeScrollSpeed_;
        const float angle = kScrollDirectionAngle[y + 1][x + 1];
        SetScrolling(true, angle, speedScale * speed);
        return;
    }
    if (!scrollStopping_) ScrollStop();
}

void cSC4View3DWin::DoScrollBegin() {                                       // 0x100781b86
    if (host_) host_->PostGameMessage(kMsgViewScrollBegin);
    cSC4RenderProperties* global = host_ ? host_->GlobalRenderProperties() : nullptr;
    if (!global || !global->BoolValue(kPropNoPartialBackingStoreCopies)) return;
    cSC4RenderProperties* app = host_ ? host_->AppRenderProperties() : nullptr;
    if (app && app != &renderProperties_) {
        renderProperties_ = *app;   // +0x540 := the live properties (the snapshot DoScrollEnd restores)
        renderPropertiesAux_ = 0;   // the dword at the source's +0x10 is not modelled by the port's class
    }
    // The switches are turned off on the *live* object (0x100781bf4 onwards), not on the snapshot.
    if (app) {
        app->SetBoolValue(kPropRenderShadows, false);
        app->SetBoolValue(kPropRenderDecals, false);
        app->SetBoolValue(kPropRenderAutomata, false);
        app->SetBoolValue(kPropRenderParticleEffects, false);
        app->SetFloatValue(kPropRenderModelThreshold, app->FloatValue(kPropRenderModelThresholdScroll));
    }
    if (host_) host_->SetScrollLightingHighlight();
}

void cSC4View3DWin::DoScrollEnd() {                                         // 0x100781908
    if (host_) host_->PostGameMessage(kMsgViewScrollEnd);
    cSC4RenderProperties* global = host_ ? host_->GlobalRenderProperties() : nullptr;
    if (!global || !global->BoolValue(kPropNoPartialBackingStoreCopies)) return;
    if (global != &renderProperties_) {
        renderPropertiesAux_ = 0;
        *global = renderProperties_;
    }
    if (host_) host_->RestoreLighting();
    if (renderer_) renderer_->ForceFullRedraw();
}

void cSC4View3DWin::DoTemporarySimHiddenPause() {                           // 0x1007819e0
    if (!simHiddenPaused_ && simulatorToPause_) {
        simHiddenPaused_ = true;
        simulatorToPause_->Pause();
    }
    simHiddenTimer_.Restart();
}

void cSC4View3DWin::DoTemporarySimHiddenResume() {                          // 0x100781a20
    if (simHiddenPaused_) {
        simHiddenPaused_ = false;
        if (simulatorToPause_) simulatorToPause_->Resume();
    }
    simHiddenTimer_.Reset();
}

// ==================================================================================================== view information

void cSC4View3DWin::GetCurrentViewFilterInfo(cSC4ViewFilterInformation& out) {   // 0x1007820a4
    out.filter = filterInfo_;
    if (filterInfo_) filterInfo_->AddRef();
}

bool cSC4View3DWin::SetCurrentViewFilterInfo(cSC4ViewFilterInformation& info) {   // 0x1007820ba
    cIGZUnknown* saved = filterInfo_;
    if (saved) saved->AddRef();
    if (cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMsgViewFilterInfo);
        message->SetVoid1(&saved);
        message->SetVoid2(&info);
        server->MessageSend(message);
        message->Release();
    }
    cIGZUnknown* next = info.filter;
    if (filterInfo_ != next) {
        if (next) next->AddRef();
        if (filterInfo_) filterInfo_->Release();
        filterInfo_ = next;
    }
    if (cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMsgViewFilterInfoDone);
        message->SetVoid1(&saved);
        message->SetVoid2(&info);
        server->MessageSend(message);
        message->Release();
    }
    if (saved) saved->Release();
    return true;
}

void cSC4View3DWin::GetCurrentViewLevelInfo(cSC4ViewLevelInformation& out) {   // 0x100782230
    out.level = viewLevel_;
}

void cSC4View3DWin::SetCurrentViewLevelInfo(cSC4ViewLevelInformation& info) {  // 0x10078223e
    const std::uint32_t previous = viewLevel_;
    if (previous == info.level) return;
    viewLevel_ = info.level;
    if (cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr) {
        std::uint32_t oldLevel = previous;
        std::uint32_t newLevel = info.level;
        for (std::uint32_t type : {kMsgViewLevelInfo, kMsgViewLevelInfoDone}) {
            auto* message = new cRZMessage2Standard();
            message->AddRef();
            message->SetType(type);
            message->SetVoid1(&oldLevel);
            message->SetVoid2(&newLevel);
            server->MessageSend(message);
            message->Release();
        }
    }
}

std::uint32_t cSC4View3DWin::GetCurrentViewLevel() { return viewLevel_; }   // 0x10078235c

void cSC4View3DWin::SetCurrentViewLevel(std::uint32_t level) {              // 0x100782368
    if (viewLevel_ == level) return;
    cSC4ViewLevelInformation info;
    info.level = level;
    SetCurrentViewLevelInfo(info);
}

// ==================================================================================================== tool stack

cSC4ViewInputControl* cSC4View3DWin::GetCurrentViewInputControl() {         // 0x100782390
    return tools_.empty() ? nullptr : tools_.back();
}

bool cSC4View3DWin::SetCurrentViewInputControl(cSC4ViewInputControl* control, int operation) {   // 0x1007823ae
    if (control) control->SetWindow(static_cast<cIGZWin*>(static_cast<cGZWin*>(this)));
    cSC4ViewInputControl* previous = GetCurrentViewInputControl();
    if (previous) previous->AddRef();
    SendViewInputControlChangeMessage(previous, control);
    if (operation == 2) RemoveAllViewInputControls(false);
    else if (operation == 1) RemoveCurrentViewInputControl(false);
    cSC4ViewInputControl* current = GetCurrentViewInputControl();
    cSC4ViewInputControl* displaced = previous;
    if (previous != current) {
        if (current) current->AddRef();
        displaced = current;
        if (previous) previous->Release();
    }
    if (displaced) {
        displaced->OnMouseExit();       // 0xb0: the tool that was on top stops seeing the mouse
        displaced->Deactivate();        // 0xc0
    }
    if (control && control->Init()) {
        control->AddRef();
        tools_.push_back(control);
        control->Activate();            // 0xb8
        if (cIGZCursor* cursor = control->GetCursor()) SetCursor(cursor, true);
    }
    if (displaced) displaced->Release();
    return true;
}

void cSC4View3DWin::SendViewInputControlChangeMessage(cSC4ViewInputControl* previous,
                                                     cSC4ViewInputControl* next) {   // 0x100782556
    cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2ChangeInputControl();
    message->AddRef();
    message->SetType(kMsgViewInputControlChanged);
    message->SetVoid1(previous);
    message->SetIGZUnknown(previous);
    message->SetVoid2(next);
    message->SetNext(next);             // the message's own +0x50 reference
    server->MessagePost(message, false);
    message->Release();
}

bool cSC4View3DWin::RemoveCurrentViewInputControl(bool keepCursor) {        // 0x100782660
    cSC4ViewInputControl* control = GetCurrentViewInputControl();
    if (!control) return false;
    control->AddRef();
    GZReleaseCapture();
    tools_.pop_back();                  // the reference the vector held is released by the two Releases below
    control->Deactivate();
    control->Shutdown();
    control->Release();
    if (!keepCursor) {
        cSC4ViewInputControl* next = GetCurrentViewInputControl();
        if (!next) {
            if (cIGZWinMgr* manager = GetWindowManager())
                if (cIGZCursorManager* cursors = manager->GetCursorManager())
                    if (cIGZCursor* cursor = cursors->LoadCursor(kView3DCursorId)) SetCursor(cursor, true);
        } else {
            next->AddRef();
            SetCursor(next->GetCursor(), true);
            SendViewInputControlChangeMessage(control, next);
            next->Release();
        }
    }
    control->Release();
    return true;
}

bool cSC4View3DWin::RemoveAllViewInputControls(bool keepCursor) {           // 0x100782790
    while (RemoveCurrentViewInputControl(keepCursor)) {}
    return false;                       // the original returns 0
}

bool cSC4View3DWin::AddPersistentViewInputControl(cSC4ViewInputControl* control) {   // 0x1007827b8
    if (control && control->Init()) {
        control->SetWindow(static_cast<cIGZWin*>(static_cast<cGZWin*>(this)));
        control->AddRef();
        persistentTools_.push_back(control);
        control->Activate();
    }
    return true;
}

bool cSC4View3DWin::RemovePersistentViewInputControl(cSC4ViewInputControl* control) {   // 0x10078283e
    bool removed = false;
    for (std::size_t i = 0; i < persistentTools_.size();) {
        if (persistentTools_[i] != control) {
            ++i;
            continue;
        }
        if (control) {
            control->Deactivate();
            control->Shutdown();
            control->Release();
        }
        persistentTools_.erase(persistentTools_.begin() + static_cast<std::ptrdiff_t>(i));
        removed = true;
    }
    return removed;
}

bool cSC4View3DWin::RemoveAllPersistentViewInputControls() {                // 0x1007828c6
    while (!persistentTools_.empty()) {
        cSC4ViewInputControl* control = persistentTools_.back();
        if (control) {
            control->Deactivate();
            control->Shutdown();
            control->Release();
        }
        persistentTools_.pop_back();
    }
    return true;
}

// ==================================================================================================== dispatch

bool cSC4View3DWin::GZOnCharacter(char c) {                                 // 0x1007829f8
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnCharacter(c);
        if (control) control->Release();
        if (handled) return true;
        if (i == 0) break;
    }
    const std::size_t count = persistentTools_.size();
    if (count == 0) return false;
    bool result = false;
    for (std::size_t index = count; index-- > 0;) {
        // Quirk (0x100782ab8): the bound is the *current* stack's size.
        if (tools_.size() <= index) return result;
        cSC4ViewInputControl* control = persistentTools_[index];
        if (control) control->AddRef();
        const bool handled = control && control->OnCharacter(c);
        if (control) control->Release();
        if (handled) result = true;
    }
    return result;
}

bool cSC4View3DWin::ProcessKeyDown(std::uint32_t key, std::uint32_t modifiers) {   // 0x100782ba2
    lastMessageModifiers_ = modifiers;
    bool result = false;
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnKeyDown(key, modifiers);
        if (control) control->Release();
        if (handled) { result = true; break; }
        if (i == 0) break;
    }
    bool persistentResult = result;
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnKeyDown(key, modifiers);
        if (control) control->Release();
        persistentResult = handled ? true : persistentResult;
        if (i == 0) break;
    }
    return (persistentResult & 0x101) != 0;
}

bool cSC4View3DWin::ProcessKeyUp(std::uint32_t key, std::uint32_t modifiers) {   // 0x100782d20
    lastMessageModifiers_ = modifiers;
    bool result = false;
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnKeyUp(key, modifiers);
        if (control) control->Release();
        if (handled) { result = true; break; }
        if (i == 0) break;
    }
    bool persistentResult = result;
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnKeyUp(key, modifiers);
        if (control) control->Release();
        persistentResult = handled ? true : persistentResult;
        if (i == 0) break;
    }
    return (persistentResult & 0x101) != 0;
}

bool cSC4View3DWin::GZOnMouseWheel(int x, int y, std::uint32_t modifiers, int delta) {   // 0x100783ae2
    lastMessageModifiers_ = modifiers;
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseWheel(x, y, modifiers, delta);
        if (control) control->Release();
        if (handled) return true;
        if (i == 0) break;
    }
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        if (control) control->OnMouseWheel(x, y, modifiers, delta);   // the result is ignored
        if (control) control->Release();
        if (i == 0) break;
    }
    if (delta < 1) ZoomOut();
    else ZoomIn();
    return true;
}

bool cSC4View3DWin::GZOnMouseExit(cIGZWin*) {                               // 0x100783c7a
    edgeScrollX_ = 0;
    edgeScrollY_ = 0;
    RecalcScrollVelocity();
    if (host_) host_->UpdateCursorTextPositions(-10000, -10000);
    if (!tools_.empty()) {
        cSC4ViewInputControl* control = tools_.back();
        if (control) { control->AddRef(); control->OnMouseExit(); control->Release(); }
    }
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        if (control) control->OnMouseExit();
        if (control) control->Release();
        if (i == 0) break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- the mouse
// The four button handlers and the move handler route the point through the city's mouse router
// (SL::spCity vslot 0x290) first, then offer it to the persistent tools and the input-tool stack, and
// finally do the window's own work (scrolling, cursor text, camera target).

bool cSC4View3DWin::GZOnMouseDownL(int x, int y, std::uint32_t modifiers) {   // 0x100782e9e
    lastMessageModifiers_ = modifiers;
    if (host_) {
        if (ICityMouseRouter* router = host_->CityMouseRouter()) {
            if (router->IsActive() && !router->ProcessMouse(x, y, 0xffffffffu)) return true;
        }
    }
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseDownL(x, y, modifiers);
        if (control) control->Release();
        if (handled) return true;
    }
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseDownL(x, y, modifiers);
        if (control) control->Release();
        if (i < 1) return true;           // quirk (0x100783022): the bottom tool's answer is ignored
        if (handled) return true;
    }
    return true;
}

bool cSC4View3DWin::GZOnMouseDownR(int x, int y, std::uint32_t modifiers) {   // 0x100783068
    lastMessageModifiers_ = modifiers;
    GZSetCapture();
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseDownR(x, y, modifiers);
        if (control) control->Release();
        if (handled) return true;
    }
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseDownR(x, y, modifiers);
        if (control) control->Release();
        if (handled) return true;
    }
    mouseDownX_ = x;
    mouseDownY_ = y;
    draggedPastDeadZone_ = false;
    if (host_) {
        // new cSC4ViewInputControlCityScroll @ 0x1002c13a2, then SetMouseScroll @ 0x1002c1930.
        if (cSC4ViewInputControl* tool = host_->CreateCityScrollTool()) {
            tool->AddRef();
            SetCurrentViewInputControl(tool, 0);
            host_->CityScrollSetMouseScroll(tool, x, y);
            tool->Release();
        }
    }
    return true;
}

bool cSC4View3DWin::GZOnMouseUpL(int x, int y, std::uint32_t modifiers) {   // 0x100783268
    lastMessageModifiers_ = modifiers;
    cSC4ViewInputControl* current = GetCurrentViewInputControl();
    if (GZAmCapturing() && current && !current->IsSelfScrollingView() && current->AmCapturing())
        RecalcScrollVelocity();
    if (host_) {
        if (ICityMouseRouter* router = host_->CityMouseRouter()) {
            if (router->IsActive() && !router->ProcessMouse(x, y, 1)) return true;
        }
    }
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseUpL(x, y, modifiers);
        if (control) control->Release();
        if (handled) return true;
    }
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseUpL(x, y, modifiers);
        if (control) control->Release();
        if (i < 1) return true;           // quirk (0x1007833f8): the bottom tool's answer is ignored
        if (handled) return true;
    }
    return true;
}

bool cSC4View3DWin::GZOnMouseUpR(int x, int y, std::uint32_t modifiers) {   // 0x100783474
    lastMessageModifiers_ = modifiers;
    if (GZAmCapturing()) GZReleaseCapture();
    if ((modifiers & 7) == 0 && !draggedPastDeadZone_) {
        cS3DVector3 terrainPoint;                       // nS3D::k_zero, k_zero, k_zero
        s3d::cIS3DModelInstance* model = nullptr;
        if (renderer_ && renderer_->Pick(x, y, nullptr, model)) {
            cS3DBoundingBox box;                        // the original's constant: {1,1,1}, {0,0,0}
            box.min[0] = box.min[1] = box.min[2] = 1.0f;
            box.max[0] = box.max[1] = box.max[2] = 0.0f;
            if (model) model->GetWorldBounds(box);
            const float centreX = (box.min[0] + box.max[0]) * 0.5f;
            const float centreZ = (box.min[2] + box.max[2]) * 0.5f;
            // Quirk (0x100783561 / 0x100783574): both terrain heights are queried and both answers dropped.
            if (ITerrainEditor* terrain = host_ ? host_->TerrainEditor() : nullptr) {
                terrain->TerrainHeight(centreX, centreZ);
                terrain->TerrainHeightAt(centreX, centreZ);
            }
            const cS3DVector3 target{(box.min[0] + box.max[0]) * 0.5f,
                                     (box.min[1] + box.max[1]) * 0.5f,
                                     (box.min[2] + box.max[2]) * 0.5f};
            if (cameraControl_) cameraControl_->SetViewTargetPosition(target);
        } else if (renderer_ && renderer_->PickTerrain(x, y, &terrainPoint.x, false, true)) {
            if (cameraControl_) cameraControl_->SetViewTargetPosition(terrainPoint);
        }
        if (model) model->Release();
    }
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseUpR(x, y, modifiers);
        if (control) control->Release();
        if (handled) return true;
    }
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseUpR(x, y, modifiers);
        if (control) control->Release();
        if (i < 1) return true;
        if (handled) return true;
    }
    return true;
}

bool cSC4View3DWin::GZOnMouseMove(int x, int y, std::uint32_t modifiers) {   // 0x100783786
    lastMessageModifiers_ = modifiers;
    bool allowEdgeScroll = true;
    if (host_) {
        if (ICityMouseRouter* router = host_->CityMouseRouter()) {
            if (router->IsActive()) {
                if (!router->ProcessMouse(x, y, 0)) return true;
                allowEdgeScroll = router->AllowsEdgeScroll();
            }
        }
    }
    const int dx = mouseDownX_ - x;
    if (std::abs(dx) >= 4) {
        draggedPastDeadZone_ = true;
    } else {
        const int dy = mouseDownY_ - y;
        if (std::abs(dy) >= 4) draggedPastDeadZone_ = true;
    }
    UpdateCursorTextPositions(x, y);
    for (std::size_t i = persistentTools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = persistentTools_[i];
        if (control) control->AddRef();
        if (control) control->OnMouseMove(x, y, modifiers);       // the result is ignored
        if (control) control->Release();
        if (i < 1) break;
    }
    for (std::size_t i = tools_.size(); i-- > 0;) {
        cSC4ViewInputControl* control = tools_[i];
        if (control) control->AddRef();
        const bool handled = control && control->OnMouseMove(x, y, modifiers);
        if (control) control->Release();
        if (i < 1 || handled) break;
    }
    cSC4ViewInputControl* current = GetCurrentViewInputControl();
    bool updateEdgeScroll = true;
    if (allowEdgeScroll && host_ && host_->Preferences()[kPrefsEdgeScrollEnabled]) {
        if (!current || !current->IsSelfScrollingView()) {
            int edgeX = -1;
            if (x > 1) edgeX = (GetW() - 1 <= x) ? 1 : 0;
            edgeScrollX_ = edgeX;
            int edgeY = -1;
            if (y > 1) edgeY = (GetH() - 1 <= y) ? 1 : 0;
            edgeScrollY_ = edgeY;
            updateEdgeScroll = false;
        } else {
            if (host_->GraphicsSystemAllowsSelfScroll()) return true;
            if (edgeScrollX_ == 0 && edgeScrollY_ == 0) return true;
            updateEdgeScroll = false;
        }
    }
    if (updateEdgeScroll) {
        edgeScrollX_ = 0;
        edgeScrollY_ = 0;
    }
    RecalcScrollVelocity();
    return true;
}

bool cSC4View3DWin::GZOnKeyDown(std::uint32_t, std::uint32_t) { return false; }   // 0x100782b82
bool cSC4View3DWin::GZOnKeyUp(std::uint32_t, std::uint32_t) { return false; }     // 0x100782b92
bool cSC4View3DWin::GZOnCommand(std::uint32_t command, std::uint64_t param) {     // 0x10078abdc
    return cGZWin::GZOnCommand(command, param);
}

// ==================================================================================================== picking

bool cSC4View3DWin::PickTerrain(int x, int y, float* out, bool arg4) {      // 0x10078292c
    if (!renderer_) {
        // Quirk (0x100782952): two words, not three.
        out[0] = 0.0f;
        out[1] = 0.0f;
        return false;
    }
    return renderer_->PickTerrain(x, y, out, false, arg4);
}

bool cSC4View3DWin::PickOccupant(int x, int y, cISC4Occupant*& out) {       // 0x100782962
    out = nullptr;
    if (!renderer_) return false;
    s3d::cIS3DModelInstance* model = nullptr;
    if (!renderer_->Pick(x, y, nullptr, model)) return false;
    bool found = false;
    if (model) {
        if (cIGZUnknown* owner = model->GetOwner()) {
            void* occupant = nullptr;
            if (owner->QueryInterface(0x26bb767e, &occupant)) {
                out = reinterpret_cast<cISC4Occupant*>(occupant);
                found = true;
            }
        }
        model->Release();
    }
    return found;
}

// ==================================================================================================== cursor text

void cSC4View3DWin::UpdateCursorTextPositions(int x, int y) {               // 0x100783a60
    if (cursorTextBox_) {
        // The original only repositions the box while it is visible (0x100783a7f..0x100783aaa).
        cIGZWin* boxWindow = cursorTextBox_->AsIGZWin();
        if (boxWindow && boxWindow->IsVisible()) {
            const cRZPoint point{x, y};
            cursorTextBox_->SetPointOfInterest(point);      // vslot 0x78
        }
    }
    cursorTextX_ = x;                       // +0x5b0
    cursorTextY_ = y;                       // +0x5b4
    UpdateCostIndicator(0.0f);              // the cost indicator follows the cursor too
}

void cSC4View3DWin::UpdateCostIndicator(float dt) {                         // 0x10078178a (PARTIAL)
    // The cost-indicator windows (+0x5c0 bitmap / +0x5c8 text) are not recovered, so the whole body is
    // still the host's; see notes/ledger_pending/cSC4View3DWin.txt.
    if (host_) host_->UpdateCostIndicator(dt);
}

void cSC4View3DWin::CursorTextChanged() {                                   // 0x10079155e
    if (!cursorTextBox_) return;
    if (!cursorText_.empty()) {
        const CursorTextInfo& info = cursorText_.back();   // the original reads the sentinel's back link
        cursorTextBox_->SetTitle(info.line1);              // vslot 0x30
        cursorTextBox_->SetText(info.line2);               // vslot 0x40
        cursorTextBox_->SetIcon(info.icon == 1 ? cursorTextIcon_ : nullptr);   // vslot 0x60
        cIGZWin* boxWindow = cursorTextBox_->AsIGZWin();
        if (boxWindow && !boxWindow->IsVisible()) {
            int x = 0;
            int y = 0;
            if (cIGZWinMgr* manager = GetWindowManager())
                manager->GetCursorPos(static_cast<cIGZWin*>(static_cast<cGZWin*>(this)), x, y);
            const cRZPoint point{x, y};
            cursorTextBox_->SetPointOfInterest(point);     // vslot 0x78
            boxWindow->ShowWindow();                       // vslot 0x228 on the cGZWin subobject
        }
        return;
    }
    if (cIGZWin* boxWindow = cursorTextBox_->AsIGZWin()) {
        boxWindow->HideWindow();                           // vslot 0x230
        boxWindow->InvalidateSelfAndParents();             // vslot 0x2e0
    }
}

void cSC4View3DWin::ClearCursorText(std::uint32_t id) {                     // 0x100791772
    for (auto it = cursorText_.rbegin(); it != cursorText_.rend(); ++it) {
        if (it->key != id) continue;
        cursorText_.erase(std::next(it).base());
        CursorTextChanged();
        return;
    }
    CursorTextChanged();
}

void cSC4View3DWin::SetCursorTextPosition3D(const cS3DVector3& position) {  // 0x100791cc4
    cS3DVector3 projected = position;
    if (!host_ || !host_->ProjectPointToViewport(projected)) return;
    UpdateCursorTextPositions(static_cast<int>(projected.x), static_cast<int>(projected.y));
}

void cSC4View3DWin::SetCursorText(std::uint32_t id, std::uint32_t order, const cIGZString* line1,
                                  const cIGZString* line2, int icon) {    // 0x10079132c
    const bool hasLine1 = line1 && line1->Strlen() != 0;
    const bool hasLine2 = line2 && line2->Strlen() != 0;
    if (!hasLine1 && !hasLine2) {
        ClearCursorText(id);
        CursorTextChanged();                // quirk (0x1007913c6): ClearCursorText already called it
        return;
    }
    auto found = cursorText_.end();
    for (auto it = cursorText_.rbegin(); it != cursorText_.rend(); ++it) {
        if (it->key != id) continue;
        found = std::next(it).base();
        break;
    }
    if (found != cursorText_.end()) {
        if (line1) found->line1.AssignFromIGZString(*line1);
        else found->line1.Erase(0, 0xffffffffu);
        // Quirk (0x10079144a): a missing second line erases the *first* string.
        if (line2) found->line2.AssignFromIGZString(*line2);
        else found->line1.Erase(0, 0xffffffffu);
        found->icon = icon;
    } else {
        // The insert scans back from the last node and inserts before the first whose order is <= this
        // one (0x10079140a); when every node has a larger order it appends at the end, which is the
        // original's behaviour (0x100791419 inserts before the sentinel).
        auto position = cursorText_.end();
        for (auto it = cursorText_.end(); it != cursorText_.begin();) {
            --it;
            if (it->order <= order) { position = it; break; }
        }
        CursorTextInfo info;
        info.key = id;
        info.order = order;
        if (line1) info.line1.AssignFromIGZString(*line1);
        if (line2) info.line2.AssignFromIGZString(*line2);
        info.icon = icon;
        cursorText_.insert(position, info);
    }
    CursorTextChanged();
}

// ==================================================================================================== panels

void cSC4View3DWin::HideMissionStatusPanel() {                              // 0x100792e7e
    if (!missionPanel_) return;
    ChildDelete(missionPanel_);
    cIGZWin* panel = missionPanel_;
    missionPanel_ = nullptr;
    if (panel) panel->Release();
}

void cSC4View3DWin::CloseSecondaryMenu() {                                  // 0x100791d18
    cIGZWin* menu = secondaryMenu_;
    if (!menu) return;
    secondaryMenu_ = nullptr;
    secondaryMenuState_ = 0;
    CloseTertiaryMenu();
    menu->HideWindow();
    // The original Releases before it ChildDeletes (0x100791d52..0x100791d60); the window manager owns
    // a reference there, while the port has to destroy it first or the pointer dangles.
    ChildDelete(menu);
    menu->Release();
}

void cSC4View3DWin::CloseTertiaryMenu() {                                   // 0x10079280e
    cIGZWin* menu = tertiaryMenu_;
    if (!menu) return;
    if (tertiaryState_) {
        // the catalog inside the frame keeps its scroll position and selection for the next time
        void* catalog = nullptr;
        if (menu->GetChildAs(0x8a2cad8bu, kGZIID_cISC4WinCatalogView, &catalog) && catalog) {
            auto* view = static_cast<cISC4WinCatalogView*>(catalog);
            tertiaryState_->scroll = view->GetScrollPosition();
            tertiaryState_->selected = view->GetSelectedItemID();
            view->Release();
        }
    }
    tertiaryMenu_ = nullptr;
    tertiaryMenuState_ = 0;
    menu->HideWindow();
    ChildDelete(menu);
    menu->Release();
}

// InvokeTertiaryMenu with a catalog (0x10078d1be): the palette frame to the right of `sender`'s button `alignId`, the
// catalog list inside it. The frame scrolls the catalog; the catalog reports a chosen item to the view.
void cSC4View3DWin::InvokeTertiaryMenu(std::uint32_t menuId, cIGZWin* sender, std::uint32_t alignId,
                                       cISC4WinCatalogView* catalog, std::uint32_t backgroundImage, CatalogState* state) {
    if (!catalog || !sender || !host_) return;
    const std::uint32_t current = tertiaryMenuState_;
    if (current != 0) {
        CloseTertiaryMenu();
        if (current == menuId) return;                                  // the same button closes its palette
    }
    catalog->SetItemSize(0x2c, 0x2c, 5);                                // vslot 0x60
    cIGZWin* catalogWindow = catalog->AsIGZWin();
    cIGZWin* self = static_cast<cIGZWin*>(static_cast<cGZWin*>(this));
    catalogWindow->SetNotificationTarget(self);                         // vslot 0x2b0
    std::uint32_t rows = static_cast<std::uint32_t>(catalog->GetItemCount());
    if (rows == 0) rows = 1;
    if (rows > 7) rows = 8;
    std::uint32_t shown = rows < 7 ? rows : 6;
    if (GetH() > 600) shown = rows;                                     // a tall window shows the whole list
    const cRZPoint size = catalog->CalcPreferredSize(shown);            // vslot 0x68
    if (cISC4WinCalloutBox* callout = host_->CreateCalloutBox()) {      // the item names' callout
        callout->SetTextFontStyle(0xa85f1a83);
        callout->SetTitleFontStyle(0xe9c86c9d);
        callout->AsIGZWin()->HideWindow();
        if (cursorTextBox_) {
            callout->SetBackgroundImage(cursorTextBox_->GetBackgroundImage(), 8, 5);
            callout->SetConstraintRect(cursorTextBox_->GetConstraintRect());
        }
        catalog->SetCalloutBox(callout);                                // vslot 0xb0
        callout->Release();
    }
    RemoveAllViewInputControls(false);
    cIGZWin* button = sender->GetChildWindowFromID(alignId);
    if (!button) return;
    const int anchorX = button->GetW() / 2 + button->GetL() + sender->GetL();
    const int anchorY = button->GetH() / 2 + button->GetT() + sender->GetT();
    const int toolbarTop = panelWindow_ ? panelWindow_->GetT() : GetH();
    cSLBuffer image(kView3DImageType, kView3DImageGroup, backgroundImage, 1, false);
    image.Load();
    auto* frame = new cSC4WinTertiaryMenuFrame(GetWindowManager());
    frame->AddRef();
    cISC4WinTertiaryMenuFrame* frameInterface = frame;
    cIGZWin* frameWindow = frameInterface->AsIGZWin();
    frameWindow->SetID(0x8a6e61e0);
    ChildAdd(frameWindow);
    frameInterface->SetBackgroundImage(image.GetBuffer(), 0x35, 0x19, 0x50, 0x35, 4, 0x1b, 0x1d);   // vslot 0x20
    frameInterface->PositionFrame(size.x, size.y, anchorX, anchorY, 10, toolbarTop - 10);          // vslot 0x28
    frameWindow->PullToFront();
    frameWindow->ShowWindow();
    tertiaryMenu_ = frameWindow;
    frameWindow->AddRef();
    tertiaryMenuState_ = menuId;
    cRZRect content;
    frameInterface->GetContentRect(content);                            // vslot 0x30
    catalogWindow->SetArea(content);
    catalogWindow->SetID(0x8a2cad8b);
    frameWindow->ChildAdd(catalogWindow);
    frameWindow->SetNotificationTarget(catalogWindow);
    tertiaryState_ = state;
    if (state) {
        catalog->SetScrollPosition(state->scroll);
        catalog->SetSelectedItemByID(state->selected);
    }
    frame->Release();
}

// InvokeSecondaryMenu with a catalog (0x10078d762): the catalog palette in the secondary slot. The callout and the catalog are set up
// before the open menu is closed; the same button again closes it (the original then keeps the catalog's extra reference).
void cSC4View3DWin::InvokeSecondaryMenu(std::uint32_t menuId, cIGZWin* sender, std::uint32_t alignId, cISC4WinCatalogView* catalog,
                                        std::uint32_t backgroundImage, CatalogState* state) {
    if (!catalog || !sender || !host_) return;
    catalog->AddRef();
    catalog->SetItemSize(0x2c, 0x2c, 5);                                // vslot 0x60
    cIGZWin* catalogWindow = catalog->AsIGZWin();
    cIGZWin* self = static_cast<cIGZWin*>(static_cast<cGZWin*>(this));
    catalogWindow->SetNotificationTarget(self);                         // vslot 0x2b0
    std::uint32_t rows = static_cast<std::uint32_t>(catalog->GetItemCount());
    if (rows == 0) rows = 1;
    if (rows > 5) rows = 6;
    const cRZPoint size = catalog->CalcPreferredSize(rows);             // vslot 0x68
    if (cISC4WinCalloutBox* callout = host_->CreateCalloutBox()) {      // new cSC4WinCalloutBox
        callout->SetTextFontStyle(0xa85f1a83);
        callout->SetTitleFontStyle(0xe9c86c9d);
        callout->AsIGZWin()->HideWindow();
        if (cursorTextBox_) {
            callout->SetBackgroundImage(cursorTextBox_->GetBackgroundImage(), 8, 5);
            callout->SetConstraintRect(cursorTextBox_->GetConstraintRect());
        }
        catalog->SetCalloutBox(callout);                                // vslot 0xb0
        callout->Release();
    }
    RemoveAllViewInputControls(false);
    const std::uint32_t current = secondaryMenuState_;
    if (current != 0) {
        CloseSecondaryMenu();
        if (current == menuId) return;
    }
    cIGZWin* button = sender->GetChildWindowFromID(alignId);
    if (!button) { catalog->Release(); return; }
    const int anchorX = button->GetW() / 2 - 1 + sender->GetL() + button->GetL();
    const int anchorY = button->GetH() / 2 + button->GetT() + sender->GetT();
    const int toolbarTop = panelWindow_ ? panelWindow_->GetT() : GetH();
    cSLBuffer image(kView3DImageType, kView3DImageGroup, backgroundImage, 1, false);
    image.Load();
    auto* frame = new cSC4WinTertiaryMenuFrame(GetWindowManager());
    frame->AddRef();
    cISC4WinTertiaryMenuFrame* frameInterface = frame;
    cIGZWin* frameWindow = frameInterface->AsIGZWin();
    ChildAdd(frameWindow);
    frameInterface->SetBackgroundImage(image.GetBuffer(), 0x35, 0x19, 0x5e, 0x3e, 6, 0x28, 0x22);   // vslot 0x20
    frameInterface->PositionFrame(size.x, size.y, anchorX, anchorY, 10, toolbarTop - 10);          // vslot 0x28
    frameWindow->PullToFront();
    frameWindow->ShowWindow();
    cRZRect content;
    frameInterface->GetContentRect(content);                            // vslot 0x30
    catalogWindow->SetArea(content);
    frameWindow->ChildAdd(catalogWindow);
    frameWindow->SetNotificationTarget(catalogWindow);
    catalog->Release();
    tertiaryState_ = state;                                             // +0x370 (shared with the tertiary palette)
    if (state) {
        catalog->SetScrollPosition(state->scroll);
        catalog->SetSelectedItemByID(state->selected);
    }
    host_->TutorialCatalogOpened(catalog);
    secondaryMenu_ = frameWindow;                                       // the frame's creation reference
    secondaryMenuState_ = menuId;
}

// DoZoneCatalogView (0x100790c4a): the zone-type list of a zoning button.
void cSC4View3DWin::DoZoneCatalogView(cIGZWin* sender, std::uint32_t alignId, std::uint32_t propertyFilter, std::uint32_t catalogId,
                                      CatalogState* state) {
    cSC4WinCatalogView* catalog = nSC4UI::CreateCatalogView(0x2a3858e4, catalogId, 0, propertyFilter);
    if (catalog) catalog->AddRef();
    InvokeTertiaryMenu(propertyFilter + 0x8a3f003fu, sender, alignId, catalog, 0x14215ed1u, state);
    if (catalog) catalog->Release();
}

// InvokeCatalogItemCommand (0x10078410a): a catalog item of each family starts its tool. closeMenu closes the tertiary menu first
// (the global terrain tools close the secondary one).
void cSC4View3DWin::InvokeCatalogItemCommand(std::uint32_t itemType, std::uint32_t id, bool closeMenu) {
    auto stack = [&](cSC4ViewInputControl* tool) {
        if (!tool) return;
        tool->AddRef();
        SetCurrentViewInputControl(tool, 2);
        tool->Release();
    };
    const bool cityAllowsTools = host_ && host_->CanInvokeZoningTool();   // SL::spCity vslot 0xb0
    if (getenv("SC4_DEBUG")) fprintf(stderr, "city view: catalog item %#x type %#x\n", id, itemType);
    switch (itemType) {
        case 0x8a313c52u:                                               // a terrain brush without the scrubber's flag
            if (closeMenu) CloseTertiaryMenu();
            InvokeLandscapeTool(id, false);
            break;
        case 0xea5ad08du:                                               // a tree / flora brush
            if (closeMenu) CloseTertiaryMenu();
            if (host_) stack(host_->CreateFloraTool(id));
            break;
        case 0x0bb2e43eu:                                               // an earned vehicle: the city situation indicator follows it
            if (host_ && host_->FocusVehicle(id)) CloseTertiaryMenu();
            break;
        case 0x4a2b2462u:                                               // a terrain tool
            if (closeMenu) CloseTertiaryMenu();
            InvokeLandscapeTool(id, true);
            break;
        case 0x4a2b2464u:                                               // a disaster
            if (closeMenu) CloseTertiaryMenu();
            if (host_) host_->InvokeDisaster(id);
            break;
        case 0x4a2b2466u:                                               // a zone type
            if (closeMenu) CloseTertiaryMenu();
            if (id < 0x10 && cityAllowsTools) InvokeZoningTool(id);
            break;
        case 0x4a2b2467u: {                                             // a network, or an intersection (0x6a47xxxx)
            if (closeMenu) CloseTertiaryMenu();
            if (!cityAllowsTools) break;
            struct NetworkItem { std::uint32_t item; int type; };
            static constexpr NetworkItem kNetworkItems[] = {
                {0x8c329937u, 0}, {0x00000029u, 1}, {0x00000031u, 2}, {0xea6e379du, 3}, {0x2992f72cu, 4}, {0x00000032u, 5},
                {0x2b730c5bu, 6}, {0x299237bfu, 7}, {0x2b79e398u, 8}, {0x4be099deu, 9}, {0x0be099c1u, 10}, {0x2be099a9u, 11},
                {0x0be0998fu, 12},
            };
            for (const NetworkItem& item : kNetworkItems)
                if (item.item == id) { stack(host_->CreateNetworkTool(item.type)); return; }
            if ((id & 0xffff0000u) == 0x6a470000u) stack(host_->CreateNetworkIntxTool(id & 0xffffu));
            break;
        }
        case 0x6a5ac478u:                                               // a global terrain tool
            if (closeMenu) CloseSecondaryMenu();
            RunGlobalTerrainTool(static_cast<int>(id));
            break;
        case 0x6a2b245fu: {                                             // a lot: the place-lot tool, under the cursor at once
            if (closeMenu) CloseTertiaryMenu();
            if (!cityAllowsTools) break;
            cSC4ViewInputControl* tool = host_->CreatePlaceLotTool(id);   // cSC4ViewInputControlPlaceLot(0, id, 0, false)
            if (!tool) break;
            tool->AddRef();
            SetCurrentViewInputControl(tool, 2);
            int x = 0, y = 0;
            if (host_->CursorOverView(this, x, y)) tool->OnMouseMove(x, y, 0);   // vslot 0xa0
            tool->Release();
            break;
        }
        default:                                                        // 0x4a2b2463 / 0x4a2b2465 and the rest: nothing
            break;
    }
}

// DoZoningMenu (0x10078cb24): the three density lists and the de-zone / zone buttons of the zoning palette.
void cSC4View3DWin::DoZoningMenu(cIGZWin* sender, std::uint32_t button) {
    switch (button) {
        case 0xa998af42u: DoZoneCatalogView(sender, button, 0x8a3f14dcu, 0x4a2b2466u, &catalogStates_[1]); break;
        case 0xc998af00u: DoZoneCatalogView(sender, button, 0x8a3f1502u, 0x4a2b2466u, &catalogStates_[2]); break;
        case 0x29920899u: DoZoneCatalogView(sender, button, 0x8a3f1505u, 0x4a2b2466u, &catalogStates_[0]); break;
        case 0x30u: InvokeZoningTool(0); break;                                                                       // de-zone
        case 0x991ee13u: InvokeSecondaryMenu(0xe9949936u, 0x69923479u, 0x991ee13u); break;
        default: break;
    }
}

// DoDayNightMenu (0x10078ca8c): the three day / night buttons set the 24-hour clock's display mode (0xca35cb74 day-night cycle,
// 0xca35cb76 always day, 0xca35cb78 always night); the clock's answer goes back into the preferences, then the buttons follow.
void cSC4View3DWin::DoDayNightMenu(std::uint32_t button) {
    int mode = -1;
    if (button == 0xca35cb78u) mode = 2;
    else if (button == 0xca35cb76u) mode = 1;
    else if (button == 0xca35cb74u) mode = 0;
    if (host_) host_->SetDayNightMode(mode);
    UpdateDayNightButtons();
}

// DoLandscapeMenu (0x10078c690): the terraform palette. Buttons open the terrain-tool, flora and secondary menus, pick a brush, run a
// global terrain tool (0xaa44502..5 -> 5, 6, 3, 4) or reconcile the edges with the neighbours (0x4a551a6b).
void cSC4View3DWin::DoLandscapeMenu(cIGZWin* sender, std::uint32_t button) {
    switch (button) {
        case 0x8991ee08u: InvokeSecondaryMenu(0x09923283u, 0x49923239u, 0x8991ee08u); return;
        case 0x49e95d2bu: InvokeSecondaryMenu(0xe9923283u, 0x49923239u, 0x49e95d2bu); return;
        case 0x8a32dddbu: InvokeSecondaryMenu(0xaaa44448u, 0xca35cbedu, 0x8a32dddbu); return;
        case 0x36u: InvokeSecondaryMenu(0xaaa44448u, 0xca35cbedu, 0x36u); return;
        case 0x899231e6u: case 0x0a32ddd2u:                              // the two default brushes (raise / the other)
            CloseTertiaryMenu();
            ApplyDefaultTerrainBrush(6, button == 0x899231e6u);
            return;
        case 0xc99231dau: DoLandscapeCatalogView(sender, 0xc99231dau, 0x6a33d682u, 0x4a2b2462u, &catalogStates_[19]); return;
        case 0x152231f3u: DoLandscapeCatalogView(sender, 0x152231f3u, 0xea33d694u, 0x4a2b2462u, &catalogStates_[20]); return;
        case 0x6a3d638cu: DoLandscapeCatalogView(sender, 0x6a3d638cu, 0x8a3f131bu, 0x4a2b2462u, &catalogStates_[21]); return;
        case 0x29923215u: DoLandscapeCatalogView(sender, 0x29923215u, 0x8a3143b7u, 0x4a2b2462u, &catalogStates_[22]); return;
        case 0x4a22ea06u: DoFloraCatalogView(sender, 0x4a22ea06u, 0x8a2602bbu, 0xea5ad08du, &catalogStates_[24]); return;
        case 0xea32ddbbu: CloseTertiaryMenu(); InvokeLandscapeTool(0xea33e2fdu, false); return;
        case 0xea32dde5u: CloseTertiaryMenu(); InvokeLandscapeTool(0x4a524342u, false); return;
        case 0x4a32ddc7u: CloseTertiaryMenu(); InvokeLandscapeTool(0x0a33e37bu, false); return;
        case 0x0aa44502u: RunGlobalTerrainTool(5); return;
        case 0x0aa44503u: RunGlobalTerrainTool(6); return;
        case 0x0aa44504u: RunGlobalTerrainTool(3); return;
        case 0x0aa44505u: RunGlobalTerrainTool(4); return;
        case 0x4a551a6bu:
            CloseSecondaryMenu();
            if (host_) host_->ReconcileTerrainEdges();
            return;
        default: return;
    }
}

// DoDisasterCopingMenu (0x10078dbb0): the emergency palette. 0x1 opens its tertiary menu, 0x992fd36 jumps to the next disaster, the
// others pick a dispatch tool (0x33 -> 0, 0x69a30679 -> 1, 0x4bfc8c2f -> 6, 0x6bfc8c55 -> 5).
void cSC4View3DWin::DoDisasterCopingMenu(cIGZWin* sender, std::uint32_t button) {
    (void)sender;
    int type = -1;
    switch (button) {
        case 0x1u: InvokeTertiaryMenu(0x09de3002u, 0x09de8798u, 0x1u); return;
        case 0x992fd36u: if (host_) host_->GoToNextDisaster(); return;
        case 0x33u: type = 0; break;
        case 0x69a30679u: type = 1; break;
        case 0x4bfc8c2fu: type = 6; break;
        case 0x6bfc8c55u: type = 5; break;
        default: return;
    }
    if (!host_) return;
    if (cSC4ViewInputControl* tool = host_->CreateDispatchTool(type)) {
        tool->AddRef();
        SetCurrentViewInputControl(tool, 2);
        tool->Release();
    }
}

// DoVehicleMenu (0x10078e108): the vehicle palette (0xabb27a7a, with the city-situation indicator toggle 0xebfac720 set from the
// manager), the earned-vehicle catalogs (cars / aircraft / boats), the indicator toggle itself, and the My Street sim button.
void cSC4View3DWin::DoVehicleMenu(cIGZWin* sender, std::uint32_t button) {
    switch (button) {
        case 0xebfac717u: DoEarnedVehicleCatalogView(sender, 0xebfac717u, host_ ? host_->VehicleCatalogManager(0) : nullptr, nullptr); return;
        case 0xebfac718u: DoEarnedVehicleCatalogView(sender, 0xebfac718u, host_ ? host_->VehicleCatalogManager(1) : nullptr, nullptr); return;
        case 0xebfac719u: DoEarnedVehicleCatalogView(sender, 0xebfac719u, host_ ? host_->VehicleCatalogManager(2) : nullptr, nullptr); return;
        case 0xebfac720u:
            if (host_) host_->SetCitySituationIndicatorVisible(nsGZWinUtils::GetButtonToggleState(secondaryMenu_, 0xebfac720u));
            return;
        case 0xebf9e342u: if (host_) host_->ShowMyStreetSim(); return;
        case 0xabb27a7au:
            if (!sender) return;
            InvokeSecondaryMenu(0x6bb27447u, 0x8bb27c12u, 0xabb27a7au);
            if (secondaryMenu_ && host_) nsGZWinUtils::SetButtonToggleState(secondaryMenu_, 0xebfac720u, host_->CitySituationIndicatorVisible());
            return;
        default: return;
    }
}

// DoTransportMenu (0x10078dd12): the transport building catalogs. Each button picks the lot item type of the catalog and, for some,
// one building occupant group to include; the same button again closes its palette.
void cSC4View3DWin::DoTransportMenu(cIGZWin* sender, std::uint32_t button) {
    std::set<std::uint32_t> include, exclude;
    std::uint32_t lotItemType = 0;
    CatalogState* state = nullptr;
    switch (button) {
        case 0x29u: include.insert(0x1300u); lotItemType = 0x4001u; state = &catalogStates_[15]; break;
        case 0x31u: lotItemType = 0x4005u; state = &catalogStates_[14]; break;
        case 0x299237bfu: include.insert(0x130au); lotItemType = 0x4002u; state = &catalogStates_[16]; break;
        case 0x2b730c5bu: lotItemType = 0x4000u; break;
        case 0x6999bf56u: lotItemType = 0x4000u; state = &catalogStates_[13]; break;
        case 0xa99234a6u: include.insert(0x1519u); state = &catalogStates_[18]; break;
        case 0xe99234b3u: include.insert(0x1508u); state = &catalogStates_[17]; break;
        default: break;   // the original falls through with an empty catalog
    }
    if (tertiaryMenuState_ == button) {
        CloseTertiaryMenu();
        return;
    }
    cSC4WinCatalogView* catalog = CreateBuildingMenu(0x07bddf1cu, include, exclude, lotItemType, -1);
    InvokeTertiaryMenu(button, sender, button, catalog, 0x14215ed2u, state);
    if (catalog) catalog->Release();
}

// DoUtilitiesMenu (0x10078cbb4): the utility building catalogs (power 0x35, water 0x39, garbage 0x40) and the power-line tool (0x32).
// The power catalog excludes group 0x1402 and, when the ordinance 0xe0d0722e is on, 0x6a650ffe; the garbage catalog shows the
// zone item type 0x2a49d7ca as well.
void cSC4View3DWin::DoUtilitiesMenu(cIGZWin* sender, std::uint32_t button) {
    if (button == 0x32u) {
        CloseTertiaryMenu();
        if (host_) {
            if (cSC4ViewInputControl* tool = host_->CreateNetworkTool(5)) {   // the power-line tool
                tool->AddRef();
                SetCurrentViewInputControl(tool, 2);
                tool->Release();
            }
        }
        return;
    }
    if (button != 0x35u && button != 0x39u && button != 0x40u) return;
    std::set<std::uint32_t> include, exclude;
    std::uint32_t lotItemType = 0;
    int zoneItemType = -1;
    CatalogState* state = nullptr;
    if (button == 0x40u) {
        include.insert(0x1402u);
        exclude.insert(0x1401u);
        state = &catalogStates_[12];
        zoneItemType = 0x2a49d7ca;
    } else if (button == 0x39u) {
        include.insert(0x1401u);
        state = &catalogStates_[11];
        lotItemType = 0x4004u;
    } else {
        include.insert(0x1400u);
        exclude.insert(0x1402u);
        if (host_ && host_->OrdinanceOn(0xe0d0722eu)) exclude.insert(0x6a650ffeu);
        state = &catalogStates_[10];
        lotItemType = 0x4003u;
    }
    if (tertiaryMenuState_ == button) {
        CloseTertiaryMenu();
        return;
    }
    cSC4WinCatalogView* catalog = CreateBuildingMenu(0xc8dbccbau, include, exclude, lotItemType, zoneItemType);
    InvokeTertiaryMenu(button, sender, button, catalog, 0x14215ed3u, state);
    if (catalog) catalog->Release();
}

// DoLandscapeCatalogView (0x100790aa0) / DoFloraCatalogView (0x100790b98): a catalog of terrain tools (item type 0x6a01fc2a) or of
// flora (0xe83e0437) in a palette beside the button.
void cSC4View3DWin::DoLandscapeCatalogView(cIGZWin* sender, std::uint32_t alignId, std::uint32_t menuId, std::uint32_t catalogId,
                                           CatalogState* state) {
    cSC4WinCatalogView* catalog = nSC4UI::CreateCatalogView(0x6a01fc2au, catalogId, 0, menuId);
    if (catalog) catalog->AddRef();
    InvokeTertiaryMenu(menuId, sender, alignId, catalog, 0x14215ed0u, state);
    if (catalog) catalog->Release();
}

void cSC4View3DWin::DoFloraCatalogView(cIGZWin* sender, std::uint32_t alignId, std::uint32_t menuId, std::uint32_t catalogId,
                                       CatalogState* state) {
    cSC4WinCatalogView* catalog = nSC4UI::CreateCatalogView(0xe83e0437u, catalogId, 0, menuId);
    if (catalog) catalog->AddRef();
    InvokeTertiaryMenu(menuId + 0xea5ada53u, sender, alignId, catalog, 0x14215ed0u, state);
    if (catalog) catalog->Release();
}

// DoEarnedVehicleCatalogView (0x100790cfc): the vehicles the city has earned, from one automata manager.
void cSC4View3DWin::DoEarnedVehicleCatalogView(cIGZWin* sender, std::uint32_t button, cIGZUnknown* manager, CatalogState* state) {
    if (!manager) return;
    auto* catalog = new cSC4WinCatalogView(GetWindowManager());   // WinCatalogViewConstructThunk: the global window manager
    catalog->AddRef();
    std::multimap<int, cISC4CatalogItem*> items;
    if (host_) host_->AddAutomataToItemList(manager, items);
    for (auto& entry : items) {
        catalog->AddItem(entry.second);
        entry.second->Release();
    }
    InvokeTertiaryMenu(button + 0x6bb2e4bcu, sender, button, catalog, 0x14215eddu, state);
    catalog->Release();
}

// CreateBuildingMenu (0x10078d074): a catalog of the lot (0x4a2b2467) and zone (0x4a2b2466) items of the given types plus the buildings
// AddBuildingsToItemList picks; the items are added in the multimap's order and the list's references dropped.
cSC4WinCatalogView* cSC4View3DWin::CreateBuildingMenu(std::uint32_t menuId, const std::set<std::uint32_t>& include,
                                                      const std::set<std::uint32_t>& exclude, std::uint32_t lotItemType, int zoneItemType) {
    std::multimap<int, cISC4CatalogItem*> items;
    auto* catalog = new cSC4WinCatalogView(GetWindowManager());   // WinCatalogViewConstructThunk: the global window manager
    catalog->AddRef();
    if (lotItemType != 0) nSC4UI::CreateCatalogItemList(items, catalog, 0x2a3858e4u, 0x4a2b2467u, 0, lotItemType);
    if (0 < zoneItemType) nSC4UI::CreateCatalogItemList(items, catalog, 0x2a3858e4u, 0x4a2b2466u, 0, static_cast<std::uint32_t>(zoneItemType));
    if (host_) host_->AddBuildingsToItemList(menuId, include, exclude, items);
    for (auto& entry : items) {
        catalog->AddItem(entry.second);
        entry.second->Release();
    }
    return catalog;
}

// The two menu invokers build a script window (group 0x96a006b0) as a child of the view, route its commands to the
// view, align it to the toolbar button and show it. The tertiary one is the same with the other slot.
void cSC4View3DWin::InvokeSecondaryMenu(std::uint32_t script, std::uint32_t windowId, std::uint32_t alignTo) {
    RemoveAllViewInputControls(false);                                  // vslot 0xd8
    const std::uint32_t current = secondaryMenuState_;
    if (current != 0) {
        CloseSecondaryMenu();
        if (current == windowId) return;                                // the same button closes its menu
    }
    if (!host_) return;
    cGZPersistResourceKey key{0, 0x96a006b0u, script};
    cIGZWin* menu = host_->CreateWindowFromScript(key, static_cast<cIGZWin*>(static_cast<cGZWin*>(this)), windowId);
    if (!menu) return;
    void* gen = nullptr;
    if (menu->QueryInterface(kGZIID_cIGZWinGenView, &gen) && gen) {
        auto* view = static_cast<gz::cIGZWinGen*>(gen);
        menu->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));    // vslot 0x3a0
        view->SetWinGenFlag(4, false);                                  // cIGZWinGen vslot 0x30 (4, 0), (1, 0)
        view->SetWinGenFlag(1, false);
        view->Release();
    }
    host_->AlignWindowToWindow(menu->GetID(), 0xaaaau, alignTo);         // the new window's own id (vslot 0x1f8)
    menu->PullToFront();                                                // vslot 0xb8
    menu->ShowWindow();                                                 // vslot 0x228
    secondaryMenu_ = menu;
    secondaryMenuState_ = windowId;
}

void cSC4View3DWin::InvokeTertiaryMenu(std::uint32_t script, std::uint32_t windowId, std::uint32_t alignTo) {
    RemoveAllViewInputControls(false);
    const std::uint32_t current = tertiaryMenuState_;
    if (current != 0) {
        CloseTertiaryMenu();
        if (current == windowId) return;
    }
    if (!host_) return;
    cGZPersistResourceKey key{0, 0x96a006b0u, script};
    cIGZWin* menu = host_->CreateWindowFromScript(key, static_cast<cIGZWin*>(static_cast<cGZWin*>(this)), windowId);
    if (!menu) return;
    void* gen = nullptr;
    if (menu->QueryInterface(kGZIID_cIGZWinGenView, &gen) && gen) {
        auto* view = static_cast<gz::cIGZWinGen*>(gen);
        menu->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));
        view->SetWinGenFlag(4, false);
        view->SetWinGenFlag(1, false);
        view->Release();
    }
    host_->AlignWindowToWindow(menu->GetID(), 0xaaaau, alignTo);
    menu->PullToFront();                                                // vslot 0xb8
    menu->ShowWindow();                                                 // vslot 0x228
    tertiaryMenu_ = menu;
    tertiaryMenuState_ = windowId;
}

void cSC4View3DWin::CreateProgressScreen(bool flag) {                       // 0x100780910
    if (!host_) return;
    // new cSC4ProgressScreen(GetArea(), flag) @ 0x10032cecc: the host owns the class, the view holds one
    // reference and initialises it (0x10032d23e).
    progressScreen_ = host_->CreateProgressScreen(static_cast<cIGZWin*>(static_cast<cGZWin*>(this)),
                                                 GetArea(), flag);
    if (!progressScreen_) return;
    progressScreen_->AddRef();
    host_->InitProgressScreen(progressScreen_);
}

void cSC4View3DWin::UpdateProgressMessage() {                               // 0x100788d98
    if (!progressScreen_ || !host_) return;
    cRZString empty;                        // the original passes a default-constructed string
    host_->SetProgressCaption(progressScreen_, 1, empty);
}

void cSC4View3DWin::AddWinAnimationRecorder(bool enable) {                  // 0x100780d78
    cIGZWin* existing = GetChildWindowFromID(kView3DAnimationRecorderId);
    if (!enable) {
        if (existing) ChildRemove(existing);
        return;
    }
    if (existing || !host_) return;
    cIGZWin* recorder = host_->CreateWinAnimationRecorder();     // new + ctor + AddRef, 0x1003728a4
    if (!recorder) return;
    cRZString directory;
    host_->UserDataDirectory(directory);                        // SL::UserDataDirectory @ 0x100040556
    directory.AppendChars("RecordedAnimations", 18);
    directory.AppendChars("/", 1);
    host_->RecorderSetAnimationOutputDirectory(recorder, directory, true);
    recorder->SetID(kView3DAnimationRecorderId);
    ChildAdd(recorder);
    recorder->Release();
}

// ==================================================================================================== the GUI

// AddGUI (0x1007892fc, 5411 bytes): builds the whole city UI out of nine UI-script windows, three GZCOM
// objects, two image buffers and the five window procedures, in this order:
//  1. the toolbar panel (+0x280) and its cIGZWinGen view (+0x288), placed at (w/3+5, h+0xb-panelH) when the
//     view is at least three times as wide as the panel is tall, else at (5, ...);
//  2. the GZCOM toolbar button (+0x290, a cIGZWinBtn) with its image skin, tip text, id 0x43 and the panel as
//     its notification target;
//  3. the five panel windows (+0x2b0..+0x328 + panelAttach_), each wrapped by nSC4UI's child-set alignment;
//     panel 1 additionally takes the child 0xe9889775 (its own win proc) and panel 2 the MySim child 0xabb26b0e;
//  4. the three text controls of the toolbar (date / budget / population);
//  5. the news ticker window, its control host child 0xaa231508 and its Init;
//  6. the four panel windows are pulled to the front, then the map view (+0x498), the town-hall window (+0x4b0),
//     the graphs window (+0x4a0, + the graphs proc) and the building-select window (+0x4a8, + its proc);
//  7. the budget proc, the cursor-text callout box (+0x5d0) with its two font styles and background image, its
//     cursor icon (+0x5f0), the cost-indicator window (+0x5c8) and its 0x80x0x20 buffer (+0x5c0);
//  8. the advisor popup tracker (+0x4d0, APT-6), the compass window (GZCOM) and its compass win proc;
//  9. finally, when the city does not exist yet, panel 0 is opened and the "new city" / "mayor" buttons are
//     disabled.
// Quirks kept: the panel interface's win proc is detached (SetWinProc(nullptr)) and its user area cleared
// before the panel is sized; every script window is created as a child of the view's cGZWin (+0x18) and gets
// this view as a message filter; the compass window's four flags are set on the *gen* view while the alert
// border's image is attached to the child 0xea8cad14; the advisor popup tracker is stored in the +0x4d0 slot
// the header used to call the tool tip manager.
void cSC4View3DWin::AddGUI() {
    cIGZWin* parent = static_cast<cIGZWin*>(static_cast<cGZWin*>(this));    // this + 0x18
    cIGZWinMgr* mgr = GetWindowManager();
    const int viewW = GetW();
    const int viewH = GetH();
    // guiWindows_ holds one reference per window (the original's vector of cRZAutoRefCount<cIGZWin>).
    auto ownWindow = [this](cIGZWin* window) {
        if (window) guiWindows_.push_back(window);
    };
    auto clearFocus = [this](cIGZWin* window) { if (host_ && window) host_->ClearAcceptFocusFlagRecursive(window); };
    auto alignSet = [this](const std::set<cIGZWin*>& windows) { if (host_) host_->AlignWindowSetToWindow(windows, nullptr); };
    auto childrenSet = [this](cIGZWin* window, std::set<cIGZWin*>& out) {
        if (host_) host_->GetWindowChildrenSet(window, out);
    };
    auto childrenNotInSet = [this](cIGZWin* window, const std::set<cIGZWin*>& excluded, std::set<cIGZWin*>& out) {
        if (host_) host_->GetWindowChildrenNotInSet(window, excluded, out);
    };
    // ---- 1. the toolbar panel
    {
        cGZPersistResourceKey key{0, kView3DGuiGroup, kPanelWindowInstance};
        if (host_) panelWindow_ = host_->CreateWindowFromScript(key, parent, kPanelWindowId);
    }
    if (panelWindow_) {
        void* gen = nullptr;
        if (panelWindow_->QueryInterface(kPanelWindowGenIid, &gen))
            panelInterface_ = static_cast<gz::cIGZWinGen*>(gen);
        panelWindow_->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));   // vslot 0x3a0
        if (panelInterface_) {
            panelInterface_->SetWinProc(nullptr);                       // vslot 0x38
            cRZRect empty;                                              // {0, 0, 0, 0}
            panelInterface_->SetUserArea(empty);                        // vslot 0x58
        }
        const int panelW = panelWindow_->GetW();
        (void)panelW;
        const int panelH = panelWindow_->GetH();
        int x = 5;
        if (static_cast<std::uint32_t>(viewH * 3) < static_cast<std::uint32_t>(viewW)) x = viewW / 3 + 5;
        panelWindow_->GZWinMoveTo(x, (viewH + 0xb) - panelH);           // vslot 0x1c0
        panelWindow_->ShowWindow();                                     // vslot 0x228
        panelWindow_->SetFlag(kWinFlag800, false);                      // vslot 0x220
        clearFocus(panelWindow_);
    }
    // ---- 2. the GZCOM toolbar button (+0x290)
    if (host_) toolbarWindow_ = host_->CreateToolbarButton();
    if (toolbarWindow_) {
        void* buttonView = nullptr;
        if (toolbarWindow_->QueryInterface(gz::kGZIID_cIGZWinBtn, &buttonView)) {
            auto* button = static_cast<gz::cIGZWinBtn*>(buttonView);
            if (cIGZBuffer* skin = host_->LoadImageBuffer(kView3DImageType, kView3DImageGroup,
                                                          kToolbarButtonSkinInstance)) {
                // Magenta is the button skin's colour key: MakeFillColor(0xff, 0, 0xff) on the view's own
                // cGZWin (0x1007894f6), then the image's transparency (buffer vslot 0xc8).
                cIGZWin* viewWindow = static_cast<cIGZWin*>(static_cast<cGZWin*>(this));
                skin->SetTransparency(viewWindow->MakeFillColor(0xff, 0, 0xff));
                button->SetStyle(static_cast<int>(kToolbarButtonStyle), skin, nullptr);   // vslot 0x60
                skin->Release();
            }
            button->SetBtnFlag(kToolbarButtonFitToContent, true);       // vslot 0x28
            cIGZWin* buttonWindow = button->AsIGZWin();                 // vslot 0x18
            if (buttonWindow) buttonWindow->SetFlag(kWinFlag4, true);   // vslot 0x220
            if (host_) {
                cRZString tip;
                host_->LocalisedString(kToolbarButtonTipGroup, kToolbarButtonTipInstance, tip);
                button->SetTipText(tip);                                // vslot 0x118
            }
            // The button's notification target is the panel, its id 0x43, and it sits 0x1c above the view's
            // bottom edge at x = 0xc.
            buttonWindow->SetNotificationTarget(panelWindow_);          // vslot 0x2b0 (on the cIGZWin)
            buttonWindow->SetID(kToolbarButtonId);                      // vslot 0x200
            buttonWindow->GZWinMoveTo(0xc, viewH - 0x1c);               // vslot 0x1c0
            if (parent) parent->ChildAdd(buttonWindow);                 // vslot 0x70
            buttonWindow->HideWindow();                                 // vslot 0x230
            button->Release();                                          // the QueryInterface reference
        }
        clearFocus(toolbarWindow_);
    }
    // ---- 3. the five panel windows
    if (host_) panelUIWinProc_ = host_->CreatePanelUIWinProc();         // +0x448
    if (host_) newsTickerWinProc_ = host_->CreateNewsTickerWinProc();   // +0x450
    for (int i = 0; i < 5; ++i) {
        std::set<cIGZWin*> children;
        childrenSet(parent, children);
        cGZPersistResourceKey key{0, kView3DGuiGroup, kPanelScriptInstances[i]};
        cIGZWin* window = host_ ? host_->CreateWindowFromScript(key, parent, kPanelScriptIds[i]) : nullptr;
        if (!window) continue;
        ownWindow(window);                                              // the factory's reference becomes the vector's
        panelWindows_[i].push_back(window);                             // borrowed
        void* gen = nullptr;
        if (window->QueryInterface(kGZIID_cIGZWinGenView, &gen)) {
            panelAttach_[i] = static_cast<gz::cIGZWinGen*>(gen);
            panelAttach_[i]->SetWinProc(panelUIWinProc_);               // vslot 0x38
            cRZRect empty;
            panelAttach_[i]->SetUserArea(empty);                        // vslot 0x58
        }
        window->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));   // vslot 0x3a0
        clearFocus(window);
        if (i == 1) {
            // Panel 1's own child window procedure: the child found from the view, 0xe9889775.
            void* childGen = nullptr;
            if (parent->GetChildAsRecursive(kPanel1ChildId, kGZIID_cIGZWinGenView, &childGen)) {
                auto* child = static_cast<gz::cIGZWinGen*>(childGen);
                cIGZWin* childWindow = child->AsIGZWin();               // vslot 0x18
                if (childWindow) {
                    childWindow->AddRef();                              // the reference the vector keeps
                    ownWindow(childWindow);
                    panelWindows_[i].push_back(childWindow);
                    child->SetWinProc(panelUIWinProc_);
                    childWindow->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));
                    childWindow->SetFlag(kWinFlag800, true);
                    childWindow->SendToBack();                          // vslot 0xc0
                    childWindow->SetFlag(kWinFlag800, false);
                    clearFocus(childWindow);
                }
                child->Release();                                       // GetChildAsRecursive's reference
            }
        } else if (i == 2) {
            if (host_) mySimWinProc_ = host_->CreateMySimWinProc(panelAttach_[i]);   // +0x460
            if (mySimWinProc_) {
                mySimWinProc_->Init();
                if (panelAttach_[i]) panelAttach_[i]->SetWinProc(mySimWinProc_);
            }
            void* childGen = nullptr;
            if (parent->GetChildAsRecursive(kPanel2ChildId, kGZIID_cIGZWinGenView, &childGen)) {
                auto* child = static_cast<gz::cIGZWinGen*>(childGen);
                cIGZWin* childWindow = child->AsIGZWin();
                if (childWindow) {
                    childWindow->AddRef();                              // the reference the vector keeps
                    ownWindow(childWindow);
                    panelWindows_[i].push_back(childWindow);
                    child->SetWinProc(panelUIWinProc_);
                    childWindow->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));
                    childWindow->SetFlag(kWinFlag800, true);
                    childWindow->SendToBack();
                    childWindow->SetFlag(kWinFlag800, false);
                    clearFocus(childWindow);
                }
                child->Release();
            }
        } else if (i > 3) {
            // Quirk (0x100789a97): panel 4 gets its win proc attached a second time.
            if (panelAttach_[i]) panelAttach_[i]->SetWinProc(panelUIWinProc_);
        }
        std::set<cIGZWin*> added;
        childrenNotInSet(parent, children, added);
        alignSet(added);
        window->HideWindow();                                           // vslot 0x230
    }
    (void)mgr;
    // ---- 4. the three text controls
    if (panelWindow_) {
        void* text = nullptr;
        if (panelWindow_->GetChildAsRecursive(kDateTextId, kGZIID_cIGZWinText, &text))
            dateText_ = static_cast<gz::cIGZWinText*>(text);            // +0x4b8 (one reference)
    }
    if (parent) {
        void* text = nullptr;
        if (parent->GetChildAsRecursive(kBudgetTextId, kGZIID_cIGZWinText, &text))
            budgetText_ = static_cast<gz::cIGZWinText*>(text);          // +0x4c0
        text = nullptr;
        if (parent->GetChildAsRecursive(kPopulationTextId, kGZIID_cIGZWinText, &text))
            populationText_ = static_cast<gz::cIGZWinText*>(text);      // +0x4c8
    }
    // ---- 5. the news ticker
    if (host_) {
        std::set<cIGZWin*> children;
        childrenSet(parent, children);
        gz::cIGZWinGen* gen = nullptr;
        cGZPersistResourceKey key{0, kView3DGuiGroup, kNewsTickerScriptInstance};
        if (host_->CreateWindowFromScript(key, parent, kNewsTickerScriptId, kGZIID_cIGZWinGenView, (void**)&gen)) {
            if (newsTickerWinProc_) newsTickerWinProc_->Init(gen);
            cIGZWin* ticker = gen->AsIGZWin();
            if (ticker) {
                std::set<cIGZWin*> added;
                childrenNotInSet(parent, children, added);
                alignSet(added);
                ticker->HideWindow();
                ticker->AddRef();                                       // the reference the vector keeps
                ownWindow(ticker);
                panelWindows_[1].push_back(ticker);                     // +0x2c8
                panelWindows_[2].push_back(ticker);                     // +0x2e0
                if (cIGZWin* tickerParent = ticker->GetParentWin()) {
                    if (cIGZWin* control = tickerParent->GetChildWindowFromIDRecursive(kNewsTickerControlId)) {
                        control->HideWindow();
                        clearFocus(control);
                        control->AddRef();                              // the reference the vector keeps
                        ownWindow(control);
                    }
                }
            }
            gen->Release();                                             // CreateWindowFromScript's reference
        }
    }
    // ---- 6. the four panel windows to the front
    for (int i = 0; i < 4; ++i) {
        if (panelWindows_[i].empty()) continue;
        cIGZWin* window = panelWindows_[i][0];
        if (!window) continue;
        window->SetFlag(kWinFlag800, true);
        window->PullToFront();                                          // vslot 0xb8
        window->SetFlag(kWinFlag800, false);
    }
    // ---- 7. the map view, the town hall, the graphs and the building-select windows
    // The four "plain" script windows are created with iid 0x22ba0121 (cIGZWin), so the out pointer is the
    // window itself and the view keeps the reference the factory handed over.
    auto createScriptWindow = [&](std::uint32_t instance, std::uint32_t id, cIGZWin*& out) {
        if (!host_ || !parent) return;
        std::set<cIGZWin*> children;
        childrenSet(parent, children);
        if (out) { cIGZWin* old = out; out = nullptr; old->Release(); }
        void* created = nullptr;
        cGZPersistResourceKey key{0, kView3DGuiGroup, instance};
        if (host_->CreateWindowFromScript(key, parent, id, kGZIID_cIGZWin, &created))
            out = static_cast<cIGZWin*>(created);
        std::set<cIGZWin*> added;
        childrenNotInSet(parent, children, added);
        alignSet(added);
    };
    createScriptWindow(kMapViewScriptInstance, kMapViewScriptId, viewManagerSource_);       // +0x498
    if (viewManagerSource_) {
        viewManagerSource_->PullToFront();
        if (host_) host_->InitMapView(viewManagerSource_);               // cSC4WinMapView::InitWindow
        viewManagerSource_->HideWindow();
        clearFocus(viewManagerSource_);
    }
    createScriptWindow(kMayorPanelScriptInstance, kMayorPanelScriptId, window4b0_);         // +0x4b0
    if (window4b0_) {
        clearFocus(window4b0_);
        window4b0_->PullToFront();
        window4b0_->HideWindow();
    }
    createScriptWindow(kGraphsScriptInstance, kGraphsScriptId, window4a0_);                 // +0x4a0
    if (window4a0_) {
        window4a0_->PullToFront();
        window4a0_->HideWindow();
        clearFocus(window4a0_);
        if (host_) {
            IGraphsViewWindow* proc = host_->CreateGraphsViewWinProc(window4a0_);
            if (proc != graphsViewWinProc_) {
                IGraphsViewWindow* old = graphsViewWinProc_;
                graphsViewWinProc_ = proc;
                if (old) old->Release();
            }
            if (graphsViewWinProc_) graphsViewWinProc_->Init();
        }
    }
    createScriptWindow(kBuildingSelectScriptInstance, kBuildingSelectScriptId, window4a8_);  // +0x4a8
    if (window4a8_) {
        window4a8_->PullToFront();
        window4a8_->HideWindow();
        clearFocus(window4a8_);
        if (host_) {
            IBuildingSelectWindow* proc = host_->CreateBuildingSelectWinProc(window4a8_);
            if (proc != buildingSelectWinProc_) {
                IBuildingSelectWindow* old = buildingSelectWinProc_;
                buildingSelectWinProc_ = proc;
                if (old) old->Release();
            }
            if (buildingSelectWinProc_) buildingSelectWinProc_->Init();
        }
    }
    // ---- 8. the budget proc, the cursor-text box and the cost indicator
    if (host_) {
        budgetUIWinProc_ = host_->CreateBudgetUIWinProc();              // +0x468
        if (budgetUIWinProc_) budgetUIWinProc_->Init();
        if (cISC4WinCalloutBox* box = host_->CreateCalloutBox()) {       // +0x5d0
            if (cursorTextBox_) cursorTextBox_->Release();
            cursorTextBox_ = box;
            box->SetTextFontStyle(kCursorTextTextFont);                 // vslot 0x58
            box->SetTitleFontStyle(kCursorTextTitleFont);               // vslot 0x50
            if (cIGZWin* boxWindow = box->AsIGZWin()) boxWindow->HideWindow();   // vslot 0x230 on +8
            if (cIGZBuffer* background = host_->LoadImageBuffer(kView3DImageType, kView3DImageGroup,
                                                                kCursorTextBoxImageInstance)) {
                box->SetBackgroundImage(background, -1, -1);            // vslot 0x20
                background->Release();
            }
            if (cIGZBuffer* icon = host_->LoadImageBuffer(kView3DImageType, kView3DImageGroup,
                                                          kCursorTextIconInstance)) {
                if (cursorTextIcon_ != icon) {
                    icon->AddRef();
                    if (cursorTextIcon_) cursorTextIcon_->Release();
                    cursorTextIcon_ = icon;                             // +0x5f0
                }
                icon->Release();
            }
            if (cIGZWin* boxWindow = box->AsIGZWin()) parent->ChildAdd(boxWindow);   // vslot 0x70
        }
        // The cost indicator's text window (+0x5c8) and its backing buffer (+0x5c0).
        if (gz::cIGZWinBMP* bmp = host_->CreateWinBMP()) {
            if (costIndicatorText_) costIndicatorText_->Release();
            costIndicatorText_ = bmp;
            bmp->SetWinBMPFlag(6, true);                                // vslot 0x58
            if (cIGZWin* bmpWindow = bmp->AsIGZWin()) {
                bmpWindow->SetFlag(kWinIgnoreCursor, true);             // 0x200000
                bmpWindow->SetFlag(kWinFlag4, true);
                bmpWindow->HideWindow();
            }
            if (cIGZBuffer* buffer = host_->CreateImageBuffer()) {
                if (costIndicatorBuffer_) costIndicatorBuffer_->Release();
                costIndicatorBuffer_ = buffer;
                buffer->Init(kCostIndicatorBufferW, kCostIndicatorBufferH,
                             static_cast<cGZBufferColorType>(kCostIndicatorColorType));
                const cRZRect* area = buffer->GetBufferArea();          // vslot 0x68
                bmp->SetBMP(buffer);                                    // vslot 0x20
                if (area) {
                    cRZRect image{0, 0, area->right - area->left, area->bottom - area->top};
                    bmp->SetImageArea(image);                           // vslot 0x40
                }
                buffer->Release();
            }
            if (cIGZWin* bmpWindow = bmp->AsIGZWin()) parent->ChildAdd(bmpWindow);
        }
        // ---- the advisor popup queue (+0x4d0)
        advisorPopups_ = host_->CreateAdvisorPopupTracker();
        if (advisorPopups_) advisorPopups_->Init();
        // ---- the compass window and its win proc
        if (cIGZWin* compass = host_->CreateCompassWindow()) {
            if (cIGZWin* compassView = compass) {
                const cRZRect& viewArea = GetArea();
                compassView->SetArea(0, 0, viewArea.right - viewArea.left, viewArea.bottom - viewArea.top);
                compassView->SetID(kCompassWindowId);
                compassView->SetFlag(kWinPrivateBuffer, false);         // 0x10000
                compassView->SetFlag(kWinIgnoreCursor, true);
                compassView->SetFlag(kWinFlag4, true);
                if (parent) parent->ChildAdd(compassView);
            }
            compass->Release();
        }
        {
            void* compassGen = nullptr;
            if (parent && parent->GetChildAsRecursive(kCompassChildId, kGZIID_cIGZWinGenView, &compassGen)) {
                auto* gen = static_cast<gz::cIGZWinGen*>(compassGen);
                host_->AttachCompassWinProc(gen);
                gen->Release();
            }
        }
    }
    // ---- 9. the empty-city start state
    if (host_ && !host_->CanInvokeZoningTool()) {
        currentPanel_ = 0;
        ShowPanel(0);
        if (panelWindow_) {
            nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelNewCityButtonId, false);
            nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelMayorButtonId, false);
        }
    }
}

// ShowPanel (0x10078ac02): show every window of panel `panel`, then run the panel's own
// open/close steps, and hide the news ticker unless one of the three sub-panels is up.
void cSC4View3DWin::ShowPanel(std::uint32_t panel) {
    for (cIGZWin* window : panelWindows_[panel < 5 ? panel : 0]) {
        if (window) window->SetFlag(kWinVisible, true);                 // vslot 0x220
    }
    switch (panel) {
        case 1:
            panelShown_[1] = false;                                     // the word store at 0x441 clears +0x441 / +0x442
            panelShown_[2] = false;
            ShowMayorSubPanel(mayorSubPanel_);                          // +0x52c
            panelShown_[0] = true;                                      // +0x440
            break;
        case 2: {
            if (host_) {
                if (IAdvisorWindow* advisor = host_->AdvisorWindow()) {
                    advisor->HideAdvisorWindow();                       // vslot 0xe0
                    advisor->Release();
                }
            }
            panelShown_[0] = false;
            panelShown_[2] = false;
            if (mySimWinProc_) mySimWinProc_->Show();                   // @ 0x10031b54a
            panelShown_[1] = true;
            break;
        }
        case 4:
            panelShown_[2] = true;                                      // falls through
            [[fallthrough]];
        case 0:
            if (mySimWinProc_) {
                mySimWinProc_->Hide();                                  // @ 0x10031b4d8
                panelShown_[1] = false;
            }
            panelShown_[0] = false;
            break;
        default:
            break;
    }
    if (!panelShown_[1] && !panelShown_[0] && !panelShown_[2]) {
        if (newsTickerWinProc_) newsTickerWinProc_->HideTicker();       // vslot 0x40
    }
}

// HidePanel (0x10078aade): disconnect the current secondary menu, hide every window of the panel that is not
// flagged 0x1000, then run the panel's close step and hide the ticker when no sub-panel is left.
void cSC4View3DWin::HidePanel(std::uint32_t panel) {
    CloseSecondaryMenu();                                               // vslot 0x1d8
    for (cIGZWin* window : panelWindows_[panel < 5 ? panel : 0]) {
        if (!window) continue;
        if (!window->GetFlag(0x1000)) window->SetFlag(kWinVisible, false);   // vslot 0x218 / 0x220
    }
    if (panel == 1) {
        ShowMayorSubPanel(0);
        panelShown_[0] = false;
    } else if (panel == 4) {
        panelShown_[2] = false;
    } else if (panel == 2) {
        if (mySimWinProc_) mySimWinProc_->Hide();
        panelShown_[1] = false;
    }
    if (!panelShown_[1] && !panelShown_[0] && !panelShown_[2]) {
        if (newsTickerWinProc_) newsTickerWinProc_->HideTicker();
    }
}

// ShowMayorSubPanel (0x10078ad68): open / close the mayor's sub-panels. Only the panel `panel` is shown; the
// others (the advisor window, the budget, the map view, the graphs, the town hall, the building select panel)
// are hidden, and the function remembers the last panel in +0x52c / +0x530. Quirk: the state word is only
// written when the panel changes, and panel 0 is a no-op while a sub-panel state is pending.
void cSC4View3DWin::ShowMayorSubPanel(std::uint32_t panel) {
    bool allHidden = true;
    if (mayorSubPanelState_ == 0) {
        if (host_) {
            if (IAdvisorWindow* advisor = host_->AdvisorWindow()) {
                if (panel == 0x49edf9b7u) {
                    advisor->ShowAdvisorWindow();                       // vslot 0xd8
                    panelShown_[0] = true;
                } else if (advisor->IsAdvisorWindowVisible()) {          // vslot 0xe8
                    advisor->HideAdvisorWindow();                        // vslot 0xe0
                    allHidden = false;
                }
                advisor->Release();
            }
        }
        if (budgetUIWinProc_) {
            if (panel == 0x41u) budgetUIWinProc_->ShowWindow();          // vslot 0x38
            else if (budgetUIWinProc_->IsVisible()) {                    // vslot 0x50
                budgetUIWinProc_->HideWindow();                          // vslot 0x40
                allHidden = false;
            }
        }
        if (viewManagerSource_) {                                        // +0x498
            if (panel == 0x99887755u) viewManagerSource_->ShowWindow();  // vslot 0x228
            else { viewManagerSource_->HideWindow(); allHidden = false; }
        }
        if (graphsViewWinProc_) {                                        // +0x488
            if (panel == 0x15200002u) graphsViewWinProc_->ShowWindow();
            else if (graphsViewWinProc_->IsVisible()) { graphsViewWinProc_->HideWindow(); allHidden = false; }
        }
        if (window4b0_) {                                                // +0x4b0
            if (panel == 0x15200003u) window4b0_->ShowWindow();
            else { window4b0_->HideWindow(); allHidden = false; }
        }
        if (buildingSelectWinProc_) {                                    // +0x490
            if (panel == 0xabc54125u) buildingSelectWinProc_->ShowWindow();
            else if (buildingSelectWinProc_->IsVisible()) { buildingSelectWinProc_->HideWindow(); allHidden = false; }
        } else {
            goto stateCheck;                                            // 0x10078aed6 -> 0x10078ad85
        }
    }
    mayorSubPanel_ = panel;
    mayorSubPanelState_ = 0;
    return;
stateCheck:
    if (panel == 0) {
        if (allHidden) mayorSubPanelState_ = 1;
        return;
    }
    mayorSubPanel_ = panel;
    mayorSubPanelState_ = 0;
}

// UpdateSpeedButtons (0x100787896): the three simulation-speed buttons of the toolbar follow the simulator's
// current speed (vslot 0xd8) and its paused flag (vslot 0x78); the pressed button's image area is cut in half
// (the top half when running, the bottom half when paused).
void cSC4View3DWin::UpdateSpeedButtons() {
    int speed = 0;
    bool paused = true;
    if (simulatorToPause_) {
        speed = simulatorToPause_->Speed();                             // vslot 0xd8
        paused = simulatorToPause_->IsPaused();                         // vslot 0x78
    }
    if (!panelWindow_) return;
    for (int i = 0; i < 3; ++i) {
        void* view = nullptr;
        if (!panelWindow_->GetChildAs(kSpeedButtonIds[i], gz::kGZIID_cIGZWinBtn, &view)) continue;
        auto* button = static_cast<gz::cIGZWinBtn*>(view);
        button->SetBtnFlag(gz::kBtnOn, speed == kSpeedButtonModes[i]);   // vslot 0x28
        cRZRect area;
        cIGZBuffer* image = nullptr;
        button->GetImageArea(area);                                     // vslot 0x80
        button->GetBuffer(image);                                       // vslot 0x90 (borrowed: the original releases only the button)
        if (!image) { button->Release(); continue; }
        const int half = static_cast<int>(image->Height()) >> 1;        // vslot 0x60
        const int offset = paused ? half : 0;
        if (area.top != offset || area.bottom != half + offset) {
            cRZRect image2{0, offset, static_cast<int>(image->Width()), half + offset};   // vslot 0x58
            const int style = button->GetStyle();                       // vslot 0x58
            button->SetStyle(style, image, &image2);                    // vslot 0x60
        }
        button->Release();
    }
}

// UpdateAlertBorder (0x100787a1c): the compass window's alert border shows one of three images - the disaster
// layer (0x14315e60), the city situation manager (0x14315e62) or the simulator being busy (0x14315e61) - in
// that order, and no image at all when none applies.
void cSC4View3DWin::UpdateAlertBorder() {
    void* view = nullptr;
    if (!AsIGZWin()->GetChildAsRecursive(kCompassWindowId, kGZIID_cISC4WinAlertBorder, &view)) return;
    auto* border = static_cast<cISC4WinAlertBorder*>(view);
    std::uint32_t image = 0;
    if (host_) {
        if (host_->DisasterAlertActive()) image = kAlertImageDisaster;          // spCity 0x240 -> 0x50
        else if (host_->SituationAlertActive()) image = kAlertImageSituation;   // spCity 0x188 -> 0x30
        else if (host_->SimulatorBusy()) image = kAlertImageBusy;               // spSimulator 0x78
    }
    if (image) {
        if (cIGZBuffer* buffer = host_->LoadImageBuffer(kView3DImageType, kView3DImageGroup, image)) {
            border->SetImage(buffer);                                           // vslot 0x20
            buffer->Release();
        }
    } else {
        border->SetImage(nullptr);
    }
    border->Release();
}

// MinimizeUI (0x100792b4e): hide (or show) the toolbar, the panel that is open, the ticker, the secondary menu,
// the town-hall window and the view's own compass child, and record the state in the compass flag.
void cSC4View3DWin::MinimizeUI(bool minimize) {
    if (!panelWindow_) return;
    const bool visible = panelWindow_->IsVisible();                     // vslot 0x238
    bool showCompass = false;
    if (minimize) {
        if (!visible) return;
        panelWindow_->HideWindow();                                     // vslot 0x230
        if (currentPanel_ != -1) HidePanel(static_cast<std::uint32_t>(currentPanel_));
        if (newsTickerWinProc_) newsTickerWinProc_->HideTicker();
        CloseSecondaryMenu();                                           // vslot 0x1d8
        if (window470_) window470_->HideWindow();
        if (toolbarWindow_) toolbarWindow_->ShowWindow();               // vslot 0x228
        showCompass = false;
    } else {
        if (visible) return;
        panelWindow_->ShowWindow();
        if (currentPanel_ != -1) ShowPanel(static_cast<std::uint32_t>(currentPanel_));
        if (window470_) window470_->ShowWindow();
        if (panelShown_[1] || panelShown_[0] || panelShown_[2]) {
            if (newsTickerWinProc_) newsTickerWinProc_->ShowTicker();   // vslot 0x38
        }
        if (toolbarWindow_) toolbarWindow_->HideWindow();
        showCompass = true;
    }
    nsGZWinUtils::SetWindowVisible(AsIGZWin(), kCompassChildId, showCompass, false);
}

// DoCityObliterated (0x100786fbe): the city is gone, so the obliterate button is disabled, the budget panel
// closes and the obliterate toggle comes up.
void cSC4View3DWin::DoCityObliterated() {
    if (panelWindow_) {
        nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelObliterateButtonId, false);
        if (currentPanel_ == kPanelBudgetIndex) HidePanel(kPanelBudgetIndex);
        nsGZWinUtils::SetButtonToggleState(panelWindow_, kPanelObliterateToggleId, false);
    }
}

void cSC4View3DWin::UpdateZoomButtons() {                                   // 0x1007872e8
    if (!panelWindow_) return;
    nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelZoomInButtonId, CanZoomIn());     // vslot 0x60
    nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelZoomOutButtonId, CanZoomOut());   // vslot 0x68
}

void cSC4View3DWin::DoCityEstablished() {                                   // 0x100786f3e
    if (panelWindow_) {
        nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelNewCityButtonId, true);
        nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelMayorButtonId, true);
        nsGZWinUtils::SetWindowEnabled(panelWindow_, kPanelObliterateButtonId, true);
    }
    // The original only drops the tool that is active when the city is established (vslot 0xc0 / 0xd0).
    if (GetCurrentViewInputControl()) RemoveCurrentViewInputControl(false);
    UpdateBudget();
    UpdateCityName();
}

void cSC4View3DWin::UpdateCityName() {                                      // 0x10078700a
    cRZString name;
    if (host_) host_->GetCityName(name);                     // SL::spCity vslot 0x60
    if (panelWindow_) nsGZWinUtils::SetWindowCaption(panelWindow_, kPanelCityNameId, name);
}

void cSC4View3DWin::UpdateSimulationClock() {                               // 0x100787e12
    if (!dateText_ || !host_) return;
    int day = 0, month = 0, year = 0, startYear = 0;
    const bool ok = host_->SimulationDate(day, month, year, startYear);
    if (!ok) return;
    // the year since the simulation started (wrapped to four digits)
    int clamp = startYear - 1 - year;
    if (clamp < -10001) clamp = -10001;
    const int shown = static_cast<int>(static_cast<unsigned>(year + 10000 + clamp - startYear) / 10000u) * -10000 + (year - startYear);
    cRZString text;
    host_->FormatDate(day, month, shown, text);
    dateText_->SetCaption(text);                             // vslot 0x20
}

void cSC4View3DWin::UpdateDayNightButtons() {                               // 0x10078aa58
    const int mode = host_ ? host_->VisualTimeMode() : 0;    // SL::sp24HourClock vslot 0x30: 0 real, 1 noon, 2 midnight
    if (!secondaryMenu_) return;
    nsGZWinUtils::SetButtonToggleState(secondaryMenu_, kPanelRealTimeButtonId, mode == 0);
    for (int i = 0; i < 2; ++i)
        nsGZWinUtils::SetButtonToggleState(secondaryMenu_, kPanelVisualModeButtonIds[i],
                                           mode == kPanelVisualModeButtonModes[i]);
}

void cSC4View3DWin::UpdatePopulation() {                                    // 0x100787fb6
    if (!populationText_) return;
    const std::int64_t population = host_ ? host_->CityPopulation() : 0;   // SL::spResidentialSim vslot 0x80
    cRZString text;
    if (host_) host_->FormatNumber(population, text);        // RZ::LM() string 0 -> vslot 0x90
    populationText_->SetCaption(text);                       // vslot 0x20
}

void cSC4View3DWin::UpdateBudget() {                                        // 0x1007887bc
    if (budgetText_) {
        if (!host_ || !host_->CityExists()) return;          // the original returns when SL::spCity is null
        const std::int64_t funds = host_->CityFunds();      // SL::spCity vslot 0x1a0 -> vslot 0x30
        cRZString money;
        host_->MakeMoneyString(funds, money);               // SC4MakeMoneyString @ 0x1000392b0
        budgetText_->SetCaption(money);                     // vslot 0x20
        // Red when the treasury is negative, blue-grey otherwise (0x77,0,0 / 0x44,0x55,0x66).
        budgetText_->SetTextColor(gz::cIGZWinText::kColorText,
                                  funds < 0 ? host_->MakeNativeColor(0x77, 0, 0)
                                            : host_->MakeNativeColor(0x44, 0x55, 0x66));
        // An affordable cost indicator is re-rendered against the new treasury (vslot 0x150).
        if (costState_ == 1 && costColorState_ == 1) SetCostIndicator(costValue_);
    }
    if (budgetUIWinProc_ && host_ && host_->BudgetDialogOpen(budgetUIWinProc_))
        host_->RefreshBudgetDialog(budgetUIWinProc_);
}

// The "RenderProp" / "Render Properties" cheat's single property setter (0x10079064e). The name is looked up
// in the bool, int and float tables in that order; a bool with an empty value is toggled, and every accepted
// property posts 0xc9f775bb so the renderer picks the change up. Returns false when no table knows the name.
bool cSC4View3DWin::SetRenderProperties(const cIGZString& name, const cIGZString& value) {
    if (!host_) return false;
    cSC4RenderProperties* properties = host_->GlobalRenderProperties();   // SL::spRenderProperties
    if (!properties) return false;
    const char* key = name.ToChar();
    const std::string text = value.ToChar();
    std::uint32_t index = properties->BoolPropertyIDFromName(key);        // vslot 0x60
    if (static_cast<std::int32_t>(index) >= 0) {
        if (!text.empty()) properties->SetBoolValue(index, nSCRes::ParseBool(text));      // vslot 0x48
        else properties->SetBoolValue(index, !properties->BoolValue(index));              // vslot 0x30
    } else if (static_cast<std::int32_t>(index = properties->IntPropertyIDFromName(key)) >= 0) {   // vslot 0x68
        properties->SetIntValue(index, nSCRes::ParseInt(text));                          // vslot 0x50
    } else if (static_cast<std::int32_t>(index = properties->FloatPropertyIDFromName(key)) >= 0) { // vslot 0x70
        properties->SetFloatValue(index, nSCRes::ParseFloat(text));                      // vslot 0x58
    } else {
        return false;
    }
    host_->PostGameMessage(kMsgRenderPropertiesChanged);
    return true;
}

void cSC4View3DWin::SetRenderPropertiesStatic(const cRZString& name, const cRZString& value, void* context) {
    // cRZIniFile::EnumEntries callback with the view as its context (0x100792b3a).
    if (auto* view = static_cast<cSC4View3DWin*>(context)) view->SetRenderProperties(name, value);
}

// The whole GUI teardown (0x100780eaa, 1404 bytes): the toolbar panel and the GZCOM button are destroyed
// through the window manager and shut down, the guiWindows_ vector removes this view's message filter from
// every window, destroys and shuts it down, and then releases its references in reverse; the five panel
// interface references and the four window procedures get their Shutdown / Release; the three text controls,
// the cursor-text box, the cost-indicator buffer / window and the cursor icon are released; and the advisor
// popup queue (the +0x4d0 object, APT-6) is shut down and released last.
void cSC4View3DWin::RemoveGUI() {
    cIGZWinMgr* mgr = GetWindowManager();
    auto destroyWindow = [mgr](cIGZWin*& window) {
        if (!window) return;
        cIGZWin* released = window;
        window = nullptr;
        if (mgr) mgr->DestroyWindow(released);
        released->Shutdown();
        released->Release();
    };
    destroyWindow(panelWindow_);                                            // +0x280
    if (panelInterface_) { panelInterface_->Release(); panelInterface_ = nullptr; }   // +0x288
    destroyWindow(toolbarWindow_);                                          // +0x290
    for (cIGZWin* window : guiWindows_) {                                   // +0x298
        if (!window) continue;
        window->RemoveMessageFilter(static_cast<cIGZWinMessageFilter*>(this));   // vslot 0x3a8
        if (mgr) mgr->DestroyWindow(window);
        window->Shutdown();                                                 // vslot 0x28
    }
    for (auto it = guiWindows_.rbegin(); it != guiWindows_.rend(); ++it) {
        if (*it) (*it)->Release();                                          // the vector's own references
    }
    guiWindows_.clear();
    for (gz::cIGZWinGen*& attach : panelAttach_) {                           // +0x328..+0x350
        if (attach) { attach->Release(); attach = nullptr; }
    }
    if (newsTickerWinProc_) {                                               // +0x450
        if (host_) host_->ShutdownNewsTickerWinProc(newsTickerWinProc_);
        newsTickerWinProc_->Release();
        newsTickerWinProc_ = nullptr;
    }
    if (mySimWinProc_) {                                                    // +0x460
        if (host_) host_->ShutdownMySimWinProc(mySimWinProc_);
        mySimWinProc_->Release();
        mySimWinProc_ = nullptr;
    }
    if (panelUIWinProc_) { panelUIWinProc_->Release(); panelUIWinProc_ = nullptr; }   // +0x448
    if (window470_) { window470_->Shutdown(); window470_->Release(); window470_ = nullptr; }   // +0x470
    if (window478_) { window478_->Release(); window478_ = nullptr; }        // +0x478
    destroyWindow(window4b0_);                                              // +0x4b0
    destroyWindow(viewManagerSource_);                                      // +0x498
    destroyWindow(window4a0_);                                              // +0x4a0
    destroyWindow(window4a8_);                                              // +0x4a8
    if (graphsViewWinProc_) {                                               // +0x488
        if (host_) host_->ShutdownGraphsViewWinProc(graphsViewWinProc_);
        IGraphsViewWindow* proc = graphsViewWinProc_;
        graphsViewWinProc_ = nullptr;
        proc->Release();
    }
    if (buildingSelectWinProc_) {                                           // +0x490
        if (host_) host_->ShutdownBuildingSelectWinProc(buildingSelectWinProc_);
        IBuildingSelectWindow* proc = buildingSelectWinProc_;
        buildingSelectWinProc_ = nullptr;
        proc->Release();
    }
    if (budgetUIWinProc_) {                                                 // +0x468
        if (host_) host_->ShutdownBudgetUIWinProc(budgetUIWinProc_);
        budgetUIWinProc_->Release();
        budgetUIWinProc_ = nullptr;
    }
    for (gz::cIGZWinText** text : {&dateText_, &budgetText_, &populationText_}) {   // +0x4b8 / +0x4c0 / +0x4c8
        if (!*text) continue;
        cIGZWin* window = (*text)->AsIGZWin();                              // vslot 0x18
        if (window) {
            if (mgr) mgr->DestroyWindow(window);
            window->Shutdown();                                             // vslot 0x28 on the cIGZWin
        }
        gz::cIGZWinText* released = *text;
        *text = nullptr;
        released->Release();
    }
    if (cursorTextBox_) {                                                   // +0x5d0
        if (cIGZWin* boxWindow = cursorTextBox_->AsIGZWin()) ChildDelete(boxWindow);   // vslot 0x80
        cISC4WinCalloutBox* box = cursorTextBox_;
        cursorTextBox_ = nullptr;
        box->Release();
    }
    if (costIndicatorBuffer_) { costIndicatorBuffer_->Release(); costIndicatorBuffer_ = nullptr; }   // +0x5c0
    if (costIndicatorText_) { costIndicatorText_->Release(); costIndicatorText_ = nullptr; }         // +0x5c8
    cursorText_.clear();                                                    // +0x5d8
    if (cursorTextIcon_) { cursorTextIcon_->Release(); cursorTextIcon_ = nullptr; }                  // +0x5f0
    if (advisorPopups_) {                                                   // +0x4d0
        advisorPopups_->Shutdown();
        advisorPopups_->Release();
        advisorPopups_ = nullptr;
    }
}

// The panel commands (0x10078af10 DoMessage(cIGZWin*, cGZMessage&), 206 bytes): the tutorial system filters the
// message, a command from the button sender 0x287259f6 goes to HandleButtonActivated, a command from
// 0x287259f7 is the same button's "changed" notification (handled here), and the parent receives nothing else.
bool cSC4View3DWin::DoMessage(cIGZWin* window, cGZMessage& message) {
    if (host_ && !host_->FilterUiCommandMessage(message)) return true;
    if (window && window->GetFlag(0x1000)) return false;
    if (message.type == 0x12) return true;                              // the size-limit notification
    if (message.type != kGZMsgCommand) return false;
    if (message.param1 == 0x287259f7u) {
        // The button changed its state: the four panel toggles open or close their panel, the rest is ignored.
        switch (message.param2) {
            case 0x2988bc85u: DoPanelCommandGUI(0x6a935c34u); break;
            case 0x4988bc6au: DoPanelCommandGUI(0x6a935ca8u); break;
            case 0xc988bc79u: DoPanelCommandGUI(0x6a935ca4u); break;
            case 0x8988bc94u: DoPanelCommandGUI(0x6a935cabu); break;
            default: break;
        }
        return true;
    }
    if (message.param1 == 0x287259f6u) HandleButtonActivated(window, message);
    return true;
}

// DoPanelCommandGUI (0x10078c2dc, 837 bytes). PARTIAL: the four panel toggles are recovered, but the
// "establish city" path needs cSC4View3DWin::DoEstablishCityDialog (0x10078ff30, 1625 bytes, not recovered) and
// the panel commands are driven from HandleButtonActivated (also PARTIAL), so the function is registered as
// recovered-partial rather than claiming the whole body.
void cSC4View3DWin::DoPanelCommandGUI(std::uint32_t command) {
    // The panel's own toggle button (GetChildAsRecursive(id, cIGZWinBtn); the caller releases it).
    auto panelButton = [this](std::uint32_t id) -> gz::cIGZWinBtn* {
        void* view = nullptr;
        if (!panelWindow_ || !panelWindow_->GetChildAsRecursive(id, gz::kGZIID_cIGZWinBtn, &view)) return nullptr;
        return static_cast<gz::cIGZWinBtn*>(view);
    };
    auto buttonState = [&panelButton](std::uint32_t id) {
        gz::cIGZWinBtn* button = panelButton(id);
        if (!button) return false;
        const bool on = button->GetBtnFlag(gz::kBtnOn);
        button->Release();
        return on;
    };
    bool toggle = false;
    std::uint32_t panel = 0;
    switch (command) {
        case 0x6a935c34u: {                                             // the mayor / panel-0 button
            toggle = buttonState(0x2988bc85u);
            // Quirk (0x10078c3b8): shift + control + alt *and* the byte at +0x1c9 open panel 0, otherwise the
            // panel the city allows (4 = the disappearance panel when zoning is allowed, else 0).
            const bool alt = host_ && host_->IsKeyDownNow(0x10) && host_->IsKeyDownNow(0x11) &&
                             host_->IsKeyDownNow(0x12) && (unknown1c8_ >> 8) != 0;
            if (alt) panel = 0;
            else panel = (host_ && host_->CanInvokeZoningTool()) ? 4u : 0u;
            break;
        }
        case 0x6a935ca4u:                                               // the "new city" button (panel 1)
            toggle = buttonState(0xc988bc79u);
            panel = 1;
            // Turning mayor mode on in a city not yet established asks for its names first (0x10078c4a0).
            if (toggle && host_ && !host_->CanInvokeZoningTool() && !host_->ShowEstablishCityDialog()) {
                // The dialog was refused: the button's toggle is put back and nothing opens (0x10078c4b6).
                if (gz::cIGZWinBtn* button = panelButton(0xc988bc79u)) {
                    button->SetBtnFlag(gz::kBtnOn, false);
                    button->Release();
                }
                return;
            }
            break;
        case 0x6a935ca8u:                                               // panel 2
            toggle = buttonState(0x4988bc6au);
            panel = 2;
            break;
        case 0x6a935cabu:                                               // panel 3
            toggle = buttonState(0x8988bc94u);
            panel = 3;
            break;
        default:
            return;
    }
    CloseSecondaryMenu();                                               // vslot 0x1d8
    if (!toggle) {
        if (currentPanel_ == static_cast<int>(panel)) {
            currentPanel_ = -1;
            HidePanel(panel);
        }
    } else {
        if (currentPanel_ != -1) {
            HidePanel(static_cast<std::uint32_t>(currentPanel_));
            std::uint32_t button = 0;
            if (static_cast<std::uint32_t>(currentPanel_) < 5u) button = kPanelToggleButtonIds[currentPanel_];
            if (panelWindow_ && button) nsGZWinUtils::SetButtonToggleState(panelWindow_, button, false);
        }
        currentPanel_ = static_cast<int>(panel);
        ShowPanel(panel);
    }
    if (panelShown_[1] || panelShown_[0] || panelShown_[2]) {
        if (newsTickerWinProc_) newsTickerWinProc_->ShowTicker();
    }
}

// HandleButtonActivated (0x10078afde, 4659 bytes): the toolbar and menu buttons. A nested binary search (and a jump table for the ids
// 0x29..0x44) picks: a command-server command, one of the menus, the inlined civic-building catalogs, the mayor sub-panels, the
// mission panel toggles, the signs palette, the network tool's escape, the earned-items catalog; every path ends with the tutorial hook.
namespace {
struct ButtonCommand { std::uint32_t button, command; };
constexpr ButtonCommand kButtonCommands[] = {   // SL::spCommandServer vslot 0x98(command, 0, 0)
    {0x8998bbdfu, 0x6a935cb5u}, {0x8a4fbaeau, 0x6a935cccu}, {0xa98f4f88u, 0x6a935e34u}, {0x8b96b73eu, 0x6a935cf4u},
    {0x99887766u, 0x6a935cf3u}, {0xaba7ed0au, 0xabcdee11u}, {0xaa5543a1u, 0x2a94a5b6u}, {0xca58ebb7u, 0x6a935e2au},
    {0xcb95412cu, 0x0b980587u}, {0xe999c820u, 0x6a935d36u}, {0xe9920494u, 0x6a935cbau}, {0xe99237b4u, 0x6a935dc9u},
    {0xeb9541ebu, 0x6b98058au}, {0x2a4fbb08u, 0x6a935cc9u}, {0x4992046au, 0x6a935cb7u}, {0x6a4fbb31u, 0x6a935cb0u},
    {0x4a4fbb60u, 0x6a935cb2u}, {0x098f4f6cu, 0x6a935e2du}, {0x0b954123u, 0x0b980581u}, {0xc998bb81u, 0x6a935cadu},
    {0x00000044u, 0x6a935e3fu},
};
struct SecondaryMenu { std::uint32_t button, script, window; };   // InvokeSecondaryMenu(script, window, sender, button)
constexpr SecondaryMenu kSecondaryMenus[] = {
    {0xa994824du, 0xe99237ffu, 0xc99237a0u}, {0xe991ee2fu, 0x4992f764u, 0xe992f711u}, {0x6991ee42u, 0x899302fcu, 0x0992fd17u},
    {0x0991ee13u, 0xe9949936u, 0x69923479u}, {0x0991ee39u, 0xc9930681u, 0x699306edu},
};
struct CivicCatalog { std::uint32_t button; std::uint32_t groups[4]; int count; int state; };   // the inlined building catalogs
constexpr CivicCatalog kCivicCatalogs[] = {
    {0x00000003u, {0x1006u}, 1, 7},                 {0x00000034u, {0x150bu}, 1, -1},
    {0x00000037u, {0x1500u, 0x1501u}, 2, 3},        {0x00000038u, {0x1502u}, 1, 4},
    {0x00000042u, {0x1503u, 0x1504u, 0x1505u, 0x1506u}, 4, 6},
    {0x89dd5405u, {0x1507u}, 1, 5},                 {0x09930709u, {0x150au}, 1, 8},
};
bool In(std::uint32_t value, std::initializer_list<std::uint32_t> list) {
    for (std::uint32_t v : list) if (v == value) return true;
    return false;
}
}  // namespace

void cSC4View3DWin::HandleButtonActivated(cIGZWin* sender, cGZMessage& message) {
    const auto button = static_cast<std::uint32_t>(message.param2);
    AddRef();
    auto command = [this](std::uint32_t id) { if (host_) host_->ExecuteCommand(id); };
    bool done = false;
    for (const ButtonCommand& entry : kButtonCommands)
        if (entry.button == button) { command(entry.command); done = true; break; }
    if (!done) for (const SecondaryMenu& menu : kSecondaryMenus)
        if (menu.button == button) { if (sender) InvokeSecondaryMenu(menu.script, menu.window, button); done = true; break; }
    if (!done) for (const CivicCatalog& civic : kCivicCatalogs) {
        if (civic.button != button) continue;
        done = true;
        if (!sender) break;
        std::set<std::uint32_t> include(civic.groups, civic.groups + civic.count), exclude;
        CatalogState* state = civic.state >= 0 ? &catalogStates_[civic.state] : nullptr;
        if (tertiaryMenuState_ == button) {
            CloseTertiaryMenu();
        } else {
            cSC4WinCatalogView* catalog = CreateBuildingMenu(0xc8dbccbau, include, exclude, 0, -1);
            InvokeTertiaryMenu(button, sender, button, catalog, 0x14215ed4u, state);
            if (catalog) catalog->Release();
        }
        break;
    }
    if (!done) {
        done = true;
        if (In(button, {0x8991ee08u, 0x899231e6u, 0x152231f3u, 0x4a551a6bu, 0x29923215u, 0x6a3d638cu, 0xea32ddbbu, 0x8a32dddbu,
                        0xc99231dau, 0x4a22ea06u, 0x4a32ddc7u, 0x0a32ddd2u, 0xea32dde5u, 0x49e95d2bu, 0x36u, 0x0aa44502u, 0x0aa44503u,
                        0x0aa44504u, 0x0aa44505u})) {
            if (sender) DoLandscapeMenu(sender, button);
        } else if (In(button, {0xabb27a7au, 0xabb27b91u, 0xebf9e342u})) {
            DoVehicleMenu(sender, button);
        } else if (In(button, {0x29920899u, 0xa998af42u, 0xc998af00u, 0x30u})) {
            if (sender) DoZoningMenu(sender, button);
        } else if (In(button, {0x99887755u, 0xabc54125u, 0x49edf9b7u, 0x41u, 0x15200002u, 0x15200003u})) {
            ShowMayorSubPanel(button);
        } else if (In(button, {0xe99234b3u, 0x299237bfu, 0xa99234a6u, 0x2b730c5bu, 0x2b79e398u, 0xea6e379du, 0x6999bf56u, 0x29u, 0x31u})) {
            if (sender) DoTransportMenu(sender, button);
        } else if (In(button, {0x29d27ec0u, 0x49d27ed0u, 0x09d27eb0u, 0xaa9211b3u})) {   // the graph buttons
            ShowMayorSubPanel(0x15200002u);
            if (graphsViewWinProc_) graphsViewWinProc_->ShowGraphType(0xe);
        } else if (In(button, {0x4bfc8c2fu, 0x0992fd36u, 0x1u, 0x09de87aeu, 0xe9de87a7u, 0x69a30679u, 0x6bfc8c55u, 0x33u})) {
            DoDisasterCopingMenu(sender, button);
        } else if (In(button, {0x32u, 0x35u, 0x39u, 0x40u})) {
            if (sender) DoUtilitiesMenu(sender, button);
        } else {
            switch (button) {
                case 0x2992f72cu:                                       // a tertiary menu's own close button
                    CloseTertiaryMenu();
                    command(0x6a935deeu);
                    break;
                case 0xa9ed5617u:                                       // the day / night menu
                    InvokeSecondaryMenu(0xaa356502u, 0xca35cbedu, 0xa9ed5617u);
                    UpdateDayNightButtons();
                    break;
                case 0xab9537b7u:                                       // the signs palette, its toggle following the prop developer
                    if (!sender) break;
                    InvokeTertiaryMenu(0xcb95403eu, 0xab954023u, 0xab9537b7u);
                    if (tertiaryMenu_ && host_) nsGZWinUtils::SetButtonToggleState(tertiaryMenu_, 0xeb9541ebu, host_->SignsAndLabelsVisible());
                    break;
                case 0x69b9324au:                                       // the earned-items catalog
                    if (sender) InvokeSecondaryMenu(0x4a2b2fe2u, sender, 0x69b9324au,
                                                    nSC4UI::CreateCatalogView(0x29b6c670u, 0x4a2b2464u, 0, 0x8a416a99u), 0x8a2564a2u, nullptr);
                    break;
                case 0x4bb92c20u:                                       // the mission panel: details hidden, summary shown
                    if (missionPanel_) {
                        nsGZWinUtils::SetWindowVisible(missionPanel_, 0xcc1a6983u, false, false);
                        nsGZWinUtils::SetWindowVisible(missionPanel_, 0x10000005u, true, false);
                    }
                    break;
                case 0x4bb92c21u:                                       // and back
                    if (missionPanel_) {
                        nsGZWinUtils::SetWindowVisible(missionPanel_, 0xcc1a6983u, true, false);
                        nsGZWinUtils::SetWindowVisible(missionPanel_, 0x10000005u, false, false);
                    }
                    break;
                case 0x4bb92c1fu:                                       // escape out of the tool 0x2b709676
                    if (cSC4ViewInputControl* tool = GetCurrentViewInputControl())
                        if (tool->GetID() == 0x2b709676u) tool->OnKeyDown(0x1b, 0);
                    break;
                case 0xea6fa936u:                                       // the mayor panel's window closes
                    if (window4b0_) window4b0_->HideWindow();
                    break;
                case 0x43u:                                             // a click sound, then the command 0x44 runs too
                    if (host_) host_->PlayUISound(0x4a5c31d7u);
                    command(0x6a935e3fu);
                    break;
                default:
                    done = false;
                    break;
            }
        }
    }
    if (getenv("SC4_DEBUG")) fprintf(stderr, "city view: button %#x%s\n", button, done ? "" : " (no handler)");
    if (host_) host_->AfterButtonActivated();                           // SL::spCity vslot 0x290 (tutorial) -> vslot 0x40 / 0x70
    Release();
}

// ==================================================================================================== tool invokers

void cSC4View3DWin::InvokeZoningTool(std::uint32_t type) {                  // 0x100783d86
    if (!host_ || !host_->CanInvokeZoningTool()) return;        // SL::spCity vslot 0xb0 (0x100783d92)
    if (cSC4ViewInputControl* tool = host_->CreateZoningTool(type)) {   // cSC4ViewInputControlZone
        tool->AddRef();
        SetCurrentViewInputControl(tool, 2);
        tool->Release();
    }
}

void cSC4View3DWin::InvokeLandscapeTool(std::uint32_t type, bool flag) {    // 0x10078408e
    if (!host_) return;
    if (cSC4ViewInputControl* tool = host_->CreateTerrainScrubberTool(type, flag)) {
        tool->AddRef();
        SetCurrentViewInputControl(tool, 2);
        tool->Release();
    }
}

void cSC4View3DWin::ApplyDefaultTerrainBrush(int brush, bool flag) {        // 0x100790b4a
    if (brush == 7 || !host_) return;                           // 7 = no default brush
    ITerrainEditor* terrain = host_->TerrainEditor();
    if (!terrain) return;
    std::uint32_t landscapeType = 0;
    if (terrain->TerrainTypeForBrush(brush, landscapeType)) InvokeLandscapeTool(landscapeType, flag);
}

void cSC4View3DWin::ChangeTerrainType() {                                   // 0x1007928da
    if (!host_) return;
    ITerrainEditor* terrain = host_->TerrainEditor();
    if (!terrain) return;
    int type = terrain->TerrainType();                          // vslot 0x150
    host_->RunChangeTerrainDialog(type);                        // cSTEChangeTerrainUI + DoDialog
    terrain->SetTerrainType(type);                              // vslot 0x148
}

void cSC4View3DWin::RunGlobalTerrainTool(int tool) {                        // 0x1007849bc
    if (host_) host_->PlayGlobalTerrainEffectSound();           // SL::spSS / spViewUtilities / SFX block
    ITerrainEditor* terrain = host_ ? host_->TerrainEditor() : nullptr;
    switch (tool) {
        case 0:
            ChangeTerrainType();
            break;
        case 1:
            if (terrain) terrain->CreateFlatAreas(true);
            break;
        case 2:
            if (terrain) terrain->CreateShoreCliffs(true);
            break;
        case 3:
            if (terrain) terrain->ChangeTerrainLevel(true, terrain->DefaultRaiseLowerAmount());
            break;
        case 4:
            if (terrain) terrain->ChangeTerrainLevel(false, terrain->DefaultRaiseLowerAmount());
            break;
        case 5:
        case 6:
            if (cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr) {
                auto* message = new cRZMessage2Standard();
                message->AddRef();
                message->SetType(kMsgTerrainErosion);
                message->SetData1(tool == 6 ? 1 : 0);
                server->MessagePost(message, false);
                message->Release();
            }
            break;
        case 7:
            if (host_) {
                if (cSC4ViewInputControl* levelTool = host_->CreateLevelTerrainTool()) {
                    levelTool->AddRef();
                    SetCurrentViewInputControl(levelTool, 2);
                    levelTool->Release();
                }
            }
            break;
        default:
            break;
    }
}

bool cSC4View3DWin::ToggleContourDisplay() {                                // 0x10078fe00
    contourDisplay_ = !contourDisplay_;
    if (!host_) return contourDisplay_;
    if (!contourDisplay_) {
        if (contourCallback_ != -1) {
            host_->RemoveContourCallback(contourCallback_);
            contourCallback_ = -1;
        }
    } else if (contourCallback_ == -1) {
        contourCallback_ = host_->AddContourCallback();
    }
    return contourDisplay_;
}

// ==================================================================================================== global effects

bool cSC4View3DWin::GetGlobalEffectTriggerState(std::uint32_t effectId) {   // 0x100792974
    return globalEffects_.find(effectId) != globalEffects_.end();
}

void cSC4View3DWin::AssertGlobalEffectTrigger(std::uint32_t effectId) {     // 0x10079299e
    auto it = globalEffects_.find(effectId);
    if (it != globalEffects_.end()) {
        ++it->second;
        return;
    }
    globalEffects_[effectId] = 1;
    if (cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMsgViewGlobalEffectTrigger);
        message->SetData1(effectId);
        message->SetData2(1);
        server->MessageSend(message);
        message->Release();
    }
}

void cSC4View3DWin::RetractGlobalEffectTrigger(std::uint32_t effectId) {    // 0x100792a6c
    auto it = globalEffects_.find(effectId);
    if (it == globalEffects_.end()) return;
    if (it->second >= 2) {
        --it->second;
        return;
    }
    globalEffects_.erase(it);
    if (cIGZMessageServer2* server = host_ ? host_->MessageServer2() : nullptr) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kMsgViewGlobalEffectTrigger);
        message->SetData1(effectId);
        message->SetData2(0);
        server->MessageSend(message);
        message->Release();
    }
}

// ==================================================================================================== the frame

bool cSC4View3DWin::GZPaint() {                                             // 0x100781590
    if (paintEnabled_) {
        std::uint32_t elapsed = frameTimer_.GetElapsedTimeU32();
        const float threshold = frameTimeThreshold_;
        if ((threshold == 0.0f) || (threshold < static_cast<float>(elapsed))) {
            frameTimer_.Restart();
            float dt = static_cast<float>(elapsed) / 1000000.0f;
            if (dt >= 0.5f) dt = 0.5f;
            UpdateCostIndicator(dt);
            if (scrolling_ && cameraControl_) {
                const bool scrolled = cameraControl_->ScrollByViewportAngle(scrollAngle_,
                                                                            dt * scrollSpeed_ * kScrollSpeedPerFrame);
                if (scrollStopping_) {
                    const float previous = scrollSpeed_;
                    scrollSpeed_ = previous - scrollStopSpeed_;
                    if (previous - scrollStopSpeed_ <= 0.0f) {
                        if (scrolling_) DoScrollEnd();
                        scrolling_ = false;
                    }
                }
                if (scrolled) DoTemporarySimHiddenPause();
            }
            if (simHiddenPaused_ && simHiddenTimeoutMs_ < simHiddenTimer_.GetElapsedTimeU32())
                DoTemporarySimHiddenResume();
            std::uint32_t frame = 0;
            if (!deferredPumpEnabled_ && host_) frame = host_->SimulationFrameCounter();
            if (renderer_) {
                void* drawContext = renderer_->GetDrawContext();
                if (host_) host_->SetDrawContextFrameMarker(drawContext, frame);
                renderer_->Draw();
            }
        }
    }
    if (!deferredMessages_.empty() && host_ && host_->PopupModalDialogsSafe()) {
        cIGZMessage2* message = deferredMessages_.front().get();
        if (message) message->AddRef();
        deferredMessages_.pop_front();
        if (host_->PopupModalDialogsEnabled())
            if (cIGZMessageServer2* server = host_->MessageServer2()) server->MessagePost(message, false);
        if (message) message->Release();
    }
    return true;
}

// ==================================================================================================== message routing

bool cSC4View3DWin::DoMessage(cGZMessage& message) {                         // 0x100784ca4
    if (host_) host_->FilterUiCommandMessage(message);   // the tutorial filter's answer is not used here
    const std::int32_t type = static_cast<std::int32_t>(message.type);
    if (type <= 0x1a) {
        if (type > 0x0c) {
            switch (type) {
                case 0x10:                                   // cGZMsgSetFocus
                    if (host_ && host_->CityExists()) {
                        edgeScrollX_ = 0;
                        edgeScrollY_ = 0;
                        keyboardScrollX_ = 0;
                        keyboardScrollY_ = 0;
                        RecalcScrollVelocity();
                        ScrollStop();
                        GZOnMouseExit(nullptr);
                    }
                    return true;
                case 0x13:                                   // cGZMsgMouseEnter
                    focusState_ = 1;
                    if (host_) host_->Update24HourClockHover();
                    break;
                case 0x14:                                   // cGZMsgMouseExit
                    focusState_ = 0;
                    if (host_) host_->Update24HourClockHover();
                    break;
                case 0x0d:                                   // cGZMsgMouseMove
                    if (host_) host_->Update24HourClockHover();
                    break;
                default:
                    break;
            }
        } else if (message.type == kMsgViewHtmlLink) {       // -0x1b57a26f
            const bool handled = host_ && host_->HandleUrl(reinterpret_cast<cIGZString*>(message.param2));
            return !handled;
        } else if (message.type == kMsgViewAdviceLink) {     // -0x15a42044
            if (host_) host_->HandleAdviceLink(message);
        } else if (message.type == kMsgViewCommand && message.param1 == kCmdViewCatalogItem) {
            cGZWin::SetFocus();
            InvokeCatalogItemCommand(static_cast<std::uint32_t>(message.param2), static_cast<std::uint32_t>(message.param3), true);
            return true;
        }
    } else if (type > 0x6a5bdfcc) {
        if (message.type == kMsgViewAdviceLink2 || message.type == kMsgViewAdviceLink3) {
            if (host_) host_->HandleAdviceLink(message);
        }
    } else if (type == kMsgViewKey) {                        // cGZMsgKey
        const std::uint64_t sub = message.param2;
        if (sub >= 4 && sub <= 6) {
            const auto* keyInfo = reinterpret_cast<const std::uint32_t*>(message.param3);
            std::uint32_t key = 0;
            std::uint32_t modifiers = 0;
            if (keyInfo) {
                key = keyInfo[2];                            // +0x8
                modifiers = keyInfo[4];                      // +0x10
            }
            bool handled = true;
            if (sub == 5) {
                if (ProcessKeyDown(key, modifiers)) return true;
            } else if (sub == 4) {
                modifiers |= kKeyModifierKeyUp;
                if (ProcessKeyDown(key, modifiers)) return true;
            } else {
                if (ProcessKeyUp(key, modifiers)) return true;
                handled = false;
            }
            std::uint32_t command = 0;
            if (!keyAccelerator_ || !keyAccelerator_->TestForAccelerator(key, modifiers, handled, command)) return false;
            if (host_) host_->ExecuteAcceleratorCommand(command);
            return true;
        }
    } else if (message.type == kMsgViewCheatCode) {          // 0x230e27ac
        // vslot 0x38 of the object in param2 is cIGZString::ToChar (0x100784d5e).
        const char* text = nullptr;
        if (message.param2) text = reinterpret_cast<cIGZString*>(message.param2)->ToChar();
        if (host_) host_->ProcessCheatCode(static_cast<std::uint32_t>(message.param1), text ? text : "");
        return true;
    }
    return cGZWin::DoMessage(message);
}

// PARTIAL: the engine-notification entry point (0x100785a5e, 1657 bytes) drives the advisors, the
// city name / budget / population panels and the mission progress out of ~46 message types.
// DoMessage(cIGZMessage2*) (0x100785a5e): the view's message-server handler. A command (a message answering the command-message
// interface 0x4b99446a, sent by the command dispatcher) runs ProcessCommand with its parameter sets; the city's life cycle, load / save,
// the clock, the simulation, the budget, the alerts and the mission messages update the windows.
bool cSC4View3DWin::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    void* command = nullptr;
    if (message->QueryInterface(0x4b99446au, &command) && command) {
        // the command message's sets (vslots 0x50 / 0x78); the original does not Release the interface it asked for
        auto* in = static_cast<cIGZCommandParameterSet*>(standard->GetVoid1());
        auto* out = static_cast<cIGZCommandParameterSet*>(standard->GetVoid2());
        if (in && out) ProcessCommand(type, *in, *out);
        else ProcessViewCommand(type);
        return true;
    }
    auto updateSpeedAndAlerts = [&]() {
        // the pause button follows the simulator (SetBtnFlag 1, and a flash while paused)
        if (simulatorToPause_ && panelWindow_) {
            void* raw = nullptr;
            if (panelWindow_->GetChildAs(0xc998bb81u, gz::kGZIID_cIGZWinBtn, &raw) && raw) {
                auto* button = static_cast<gz::cIGZWinBtn*>(raw);
                const bool paused = simulatorToPause_->IsPaused();
                button->SetBtnFlag(1, paused);
                button->Flash(simulatorToPause_->IsPaused() ? 0xfffffff0u : 0u, 1000);
                button->Release();
            }
        }
        UpdateSpeedButtons();
        UpdateAlertBorder();
        if (type == 0xabbbca64u) UpdateMissionProgress(standard->GetIGZUnknown(), static_cast<std::uint32_t>(standard->GetData1()));
    };
    auto alertButton = [&]() {
        if (cIGZWin* button = GetChildWindowFromIDRecursive(0x8991ee08u)) button->SetFlag(2, type == 0xaa1b2002u);
        UpdateAlertBorder();
    };
    switch (type) {
        case 0xca1b1e64u:                                               // the disaster ended: the disaster menu closes
            if (secondaryMenuState_ == 0x49923239u) CloseSecondaryMenu();
            alertButton();
            return true;
        case 0xaa1b2002u: alertButton(); return true;               // a disaster began
        case 0x06cdb65bu: if (renderer_) renderer_->ForceFullRedraw(); return true;   // renderer vslot 0xa0
        case 0x0772fad4u: case 0x08dac3e7u: case 0x08dac3e8u: UpdateBudget(); return true;
        case 0x0ab99380u: UpdateCityName(); return true;
        case 0x26c63341u:                                               // the city was read
            DoLoad(static_cast<cIGZPersistDBSegment*>(standard->GetVoid1()), static_cast<cIGZVariant*>(standard->GetVoid3()));
            return true;
        case 0x26c63344u: DoSave(static_cast<cIGZPersistDBSegment*>(standard->GetVoid1())); return true;
        case 0x26c63343u: RemoveAllViewInputControls(false); return true;
        case 0x26d31ec0u: DoPreCityInit(); return true;
        case 0x26d31ec1u: DoPostCityInit(); return true;
        case 0x26d31ec2u:                                               // the city shuts down
            deferredMessages_.clear();
            RemoveAllViewInputControls(true);
            RemoveAllPersistentViewInputControls();
            return true;
        case 0x26d31ec3u: return true;
        case 0x26d31ec4u: DoCityEstablished(); return true;
        case 0x26d31ec5u: DoCityObliterated(); return true;
        case 0xa6b79602u: case 0xa6b79612u: {                           // the view changed (zoom / rotation)
            if (host_ && host_->GlobalRenderProperties() && host_->GlobalRenderProperties()->BoolValue(28)) return true;   // render properties +0x738 (bool 28: 0x40-byte records, value at +0x38)
            if (type == 0xa6b79602u) {
                UpdateZoomButtons();
                AddBackgroundImageToRenderer();
            }
            DoTemporarySimHiddenPause();
            if (focusState_ == 0) return true;
            int x = 0, y = 0;
            if (cIGZWinMgr* mgr = GetWindowManager()) mgr->GetMousePosition(x, y);
            GZOnMouseMove(x, y, lastMessageModifiers_);
            return true;
        }
        case 0xa6b79621u: DoTemporarySimHiddenPause(); return true;
        case 0xaa5cf1e3u: {                                             // the budget failed: a dialog now, or once dialogs are safe
            if (host_ && host_->PopupModalDialogsSafe()) {
                DoBudgetFailure();
                return true;
            }
            for (const auto& queued : deferredMessages_)
                if (queued && queued->GetType() == type) return true;
            deferredMessages_.push_back(cRZAutoRefCount<cIGZMessage2>(message));
            return true;
        }
        case 0xaa7fb7e0u: updateSpeedAndAlerts(); return true;
        case 0xabbbca64u: updateSpeedAndAlerts(); return true;
        case 0xac083d7du: UpdateMissionProgress(standard->GetIGZUnknown(), 1); return true;
        case 0x44778231u:
            Update24HourClock(static_cast<std::uint32_t>(standard->GetData2()), static_cast<std::uint32_t>(standard->GetData3()));
            return true;
        case 0x44778233u: UpdateAmbientLightLevel(); return true;
        case 0x4a44667fu: {                                             // a progress caption
            if (!progressScreen_ || !host_) return true;
            void* raw = nullptr;
            if (standard->GetString(0x089b7dc8u, &raw) && raw) {
                auto* text = static_cast<cIGZString*>(raw);
                host_->SetProgressCaption(progressScreen_, static_cast<int>(standard->GetData1()), *text);
                text->Release();
            }
            return true;
        }
        case 0x4a7fb7e2u: updateSpeedAndAlerts(); return true;
        case 0x4a7fb7e9u: UpdateSpeedButtons(); return true;
        case 0x66956814u: UpdateSimulationClock(); UpdatePopulation(); return true;
        case 0x66956816u: UpdateMayorRatingBar(); return true;
        default: return true;
    }
}

// ---- DoMessage's helpers that are not recovered yet: the host's hooks stand in (PARTIAL) -------------------------------------------
void cSC4View3DWin::DoPreCityInit() { if (host_) host_->PreCityInit(); }                                   // 0x10078616c
// DoPostCityInit (0x1007867d8): the view keeps SL::spSimulator at +0x238 (the speed buttons and the hidden pause use it); the rest of
// the body (the camera's city size, the clock, the panels) is the host's.
void cSC4View3DWin::DoPostCityInit() {
    if (!simulatorToPause_ && host_) {
        simulatorToPause_ = host_->Simulator();
        if (simulatorToPause_) simulatorToPause_->AddRef();
    }
    if (host_) host_->PostCityInit();
}
void cSC4View3DWin::DoLoad(cIGZPersistDBSegment* segment, cIGZVariant* version) { if (host_) host_->LoadViewState(segment, version); }   // 0x100787092
void cSC4View3DWin::DoSave(cIGZPersistDBSegment* segment) { if (host_) host_->SaveViewState(segment); }   // 0x1007871e4
void cSC4View3DWin::DoBudgetFailure() { if (host_) host_->BudgetFailure(); }                               // 0x100788d0e
void cSC4View3DWin::Update24HourClock(std::uint32_t hour, std::uint32_t minute) { if (host_) host_->ClockChanged(hour, minute); }   // 0x100788912
void cSC4View3DWin::UpdateAmbientLightLevel() { if (host_) host_->AmbientLightChanged(); }                 // 0x100788bfc
void cSC4View3DWin::UpdateMayorRatingBar() { if (host_) host_->MayorRatingChanged(); }                     // 0x100788084
void cSC4View3DWin::UpdateMissionProgress(cIGZUnknown* mission, std::uint32_t state) { if (host_) host_->MissionProgress(mission, state); }   // 0x100787b66
void cSC4View3DWin::AddBackgroundImageToRenderer() { if (host_) host_->AddBackgroundImage(); }            // 0x100787334

}  // namespace sc4
