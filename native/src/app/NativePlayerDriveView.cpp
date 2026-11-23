// U-Drive-It's view of the city view (SL::spView3D), the city camera and the city situation manager (app/NativePlayerDrive.cpp): the camera control the
// vehicle manager's camera follow moves (SL::spRenderer vslot 0x80, cSC4CameraControl) and the situation manager's countdown,
// CSI visibility and mission status. A translation unit of its own because the camera, renderer and situation headers do not
// combine with the automata headers.
#include <cstdint>

#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeCitySystems.h"
#include "gz/WinUtils.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControl.h"
#include "sc4/CameraControl.h"
#include "sc4/CommonIIDs.h"
#include "sc4/CitySituationManager.h"
#include "sc4/Render3D.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
sc4::cSC4CameraControl* Camera() { return CityRenderer() ? CityRenderer()->GetCameraControl() : nullptr; }
sc4::cSC4CitySituationManager* Situations() { return HostCitySituations(); }
}  // namespace

bool NativePlayerDriveCameraPresent() { return Camera() != nullptr; }
int NativePlayerDriveCameraZoom() { sc4::cSC4CameraControl* c = Camera(); return c ? c->GetVirtualZoom() : 0; }
bool NativePlayerDriveCameraSetZoom(float zoom) { sc4::cSC4CameraControl* c = Camera(); return c && c->SetZoom(zoom); }
void NativePlayerDriveCameraViewport(int& width, int& height) {
    width = height = 0;
    if (sc4::cSC4CameraControl* c = Camera()) c->GetViewportSize(width, height);
}
bool NativePlayerDriveCameraSetTarget(const float xyz[3]) {
    sc4::cSC4CameraControl* c = Camera();
    return c && c->SetViewTargetPosition(cS3DVector3{xyz[0], xyz[1], xyz[2]});
}
bool NativePlayerDriveCameraSetVelocity(const float xyz[3]) {
    sc4::cSC4CameraControl* c = Camera();
    return c && c->SetViewTargetVelocity(cS3DVector3{xyz[0], xyz[1], xyz[2]});
}
// The camera's view target (+0x4c), what the camera follow reads as its current target.
bool NativePlayerDriveCameraTarget(float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    sc4::cSC4CameraControl* c = Camera();
    if (!c) return false;
    const cS3DVector3& target = c->ViewTarget();
    out[0] = target.x; out[1] = target.y; out[2] = target.z;
    return true;
}
bool NativeSituationCountdownInProgress() { sc4::cSC4CitySituationManager* m = Situations(); return m && m->CountdownIsInProgress(); }
bool NativeSituationCSIVisible() { sc4::cSC4CitySituationManager* m = Situations(); return !m || m->GetVisible(); }
bool NativeSituationStatus(std::uint32_t& status) {
    sc4::cSC4CitySituationManager* m = Situations();
    if (!m) return false;
    status = m->GetCurrentStatus();
    return true;
}

// ---- the city view (SL::spView3D) as the tool reaches it ---------------------------------------------------------------
void NativeDriveViewMinimizeUI(bool minimize) { if (sc4::cSC4View3DWin* v = CityView3DWin()) v->MinimizeUI(minimize); }
void NativeDriveViewSetZoomLimits(int minimum, int maximum) { if (sc4::cSC4View3DWin* v = CityView3DWin()) v->SetZoomLimits(minimum, maximum); }
void NativeDriveViewSetZoom(int zoom) { if (sc4::cSC4View3DWin* v = CityView3DWin()) v->SetZoom(zoom); }
bool NativeDriveViewCanZoom(bool in) {
    sc4::cSC4View3DWin* v = CityView3DWin();
    return v && (in ? v->CanZoomIn() : v->CanZoomOut());
}
void NativeDriveViewClearCursorText(std::uint32_t id) { if (sc4::cSC4View3DWin* v = CityView3DWin()) v->ClearCursorText(id); }
// The view as a window (view vslot 0xc): the dashboard's parent (borrowed; null outside a city).
cIGZWin* NativeDriveViewWindow() {
    sc4::cSC4View3DWin* v = CityView3DWin();
    return v ? static_cast<cIGZWin*>(static_cast<cGZWin*>(v)) : nullptr;
}
// op 1 shows panel `id`, 0 hides it, 2 updates it (view vslots 0xe0 / 0xe4 / 0xe8).
void NativeDriveViewMissionPanel(int op, std::uint32_t id) {
    sc4::cSC4View3DWin* v = CityView3DWin();
    if (!v) return;
    if (op == 1) v->ShowMissionStatusPanel(id);
    else if (op == 0) v->HideMissionStatusPanel();
    else v->UpdateMissionStatusPanel();
}
void NativeDriveViewHideWindow(std::uint32_t id) {
    sc4::cSC4View3DWin* v = CityView3DWin();
    if (cIGZWin* window = v ? v->AsIGZWin() : nullptr) nsGZWinUtils::SetWindowVisible(window, id, false, false);
}
// The view's accelerator (vslot 0x7c) is kept, counted, and `own` installed (vslot 0x80).
bool NativeDriveViewSwapAccelerator(cIGZWinKeyAccelerator* own, cIGZWinKeyAccelerator*& saved) {
    sc4::cSC4View3DWin* v = CityView3DWin();
    if (!v || !own) return false;
    saved = v->GetKeyAccelerator();
    if (saved) saved->AddRef();
    v->SetKeyAccelerator(own);
    return true;
}
void NativeDriveViewRestoreAccelerator(cIGZWinKeyAccelerator* saved) {
    if (sc4::cSC4View3DWin* v = CityView3DWin()) v->SetKeyAccelerator(saved);
    if (saved) saved->Release();
}
void NativeDriveViewPushTool(sc4::cSC4ViewInputControl* tool, int operation) {
    if (sc4::cSC4View3DWin* v = CityView3DWin()) v->SetCurrentViewInputControl(tool, operation);
}
std::uint32_t NativeDriveViewCurrentToolId() {
    sc4::cSC4View3DWin* v = CityView3DWin();
    sc4::cSC4ViewInputControl* current = v ? v->GetCurrentViewInputControl() : nullptr;
    return current ? current->GetID() : 0;
}

}  // namespace native
