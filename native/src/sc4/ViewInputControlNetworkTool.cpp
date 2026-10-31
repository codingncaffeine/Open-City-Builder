#include "sc4/ViewInputControlNetworkTool.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sc4/NetworkTool.h"
#include "sc4/NetworkToolUI.h"

namespace sc4 {

namespace {
INetworkInputToolHost* g_host = nullptr;
constexpr std::uint32_t kKeyEscape = 0x1b;
constexpr std::uint32_t kButtonLeftDown = 0x10;   // the modifiers' left-button bit
bool Trace() { static const bool on = std::getenv("SC4_NETDEBUG") != nullptr; return on; }
}  // namespace

INetworkInputToolHost* NetworkInputToolHost() { return g_host; }
void SetNetworkInputToolHost(INetworkInputToolHost* host) { g_host = host; }

cSC4ViewInputControlNetworkTool::cSC4ViewInputControlNetworkTool(std::uint32_t networkType) : networkType_(networkType) {}

cSC4ViewInputControlNetworkTool::~cSC4ViewInputControlNetworkTool() {
    if (ui_) ui_->Release();
    if (tool_) tool_->Release();
    if (outline_) outline_->Release();
    if (terrainView_) terrainView_->Release();
}

cISC4View3DWin* cSC4ViewInputControlNetworkTool::View() const { return g_host ? g_host->View() : nullptr; }

bool cSC4ViewInputControlNetworkTool::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0x452294aa) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return cSC4ViewInputControl::QueryInterface(iid, out);
}

void cSC4ViewInputControlNetworkTool::StopOutline() {
    if (outline_ && outline_->IsStarted()) outline_->Stop(0);
}

bool cSC4ViewInputControlNetworkTool::Init() {
    if (initialized_) return true;
    if (!g_host) return false;
    tool_ = g_host->CreateNetworkTool(networkType_);
    if (!tool_) return false;
    tool_->Init();
    if (ui_) {
        cISC4NetworkToolUI* old = ui_;
        ui_ = nullptr;
        old->Release();
    }
    tool_->GetUITool(&ui_);
    id_ = ui_->ViewInputControlId();
    cursorId_ = ui_->CursorId();
    const bool ok = cSC4ViewInputControl::Init();
    if (ok) {
        ui_->AttachToView(View());
        ui_->EnterUI();
    }
    if (cIGZMessageServer2* server = g_host->MessageServer()) server->AddNotification(this, kMsgViewInputControlChanged);
    cellSize_ = g_host->CellSize();
    terrainView_ = g_host->TerrainView();
    if (terrainView_) terrainView_->AddRef();
    localGrid_.Init();
    if (!outline_) {
        if (ILocalGridHost* grid = LocalGridHost(); grid && grid->Effects()) outline_ = grid->Effects()->Create("local_tile_outline");
    }
    initialized_ = true;
    return ok;
}

bool cSC4ViewInputControlNetworkTool::Shutdown() {
    if (!initialized_) return true;
    if (cISC4View3DWin* view = View()) {
        view->ClearCursorText(kNetworkToolCursorText);
        view->ClearCostIndicator(false);
    }
    localGrid_.Shutdown();
    if (terrainView_) {
        terrainView_->Release();
        terrainView_ = nullptr;
    }
    if (outline_) {
        StopOutline();
        outline_->Release();
        outline_ = nullptr;
    }
    if (g_host) {
        if (cIGZMessageServer2* server = g_host->MessageServer()) server->RemoveNotification(this, kMsgViewInputControlChanged);
    }
    ui_->LeaveUI();
    ui_->AttachToView(nullptr);
    tool_->Shutdown();
    tool_->Release();
    tool_ = nullptr;
    ui_->Shutdown();
    ui_->Release();
    ui_ = nullptr;
    return cSC4ViewInputControl::Shutdown();
}

bool cSC4ViewInputControlNetworkTool::OnMouseExit() {
    ReleaseCapture();
    StopOutline();
    localGrid_.ShowEffect(false);
    if (cISC4View3DWin* view = View()) {
        view->ClearCursorText(kNetworkToolCursorText);
        view->ClearCostIndicator(false);
    }
    return true;
}

bool cSC4ViewInputControlNetworkTool::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    if (cISC4View3DWin* view = View()) view->KillKeyboardScrolling();
    SetCapture();
    mouseDown_ = true;
    StopOutline();
    localGrid_.ShowEffect(false);
    const bool handled = ui_->OnMouseDownL(x, y, modifiers);
    SetCursor(ui_->CursorId());
    return handled;
}

bool cSC4ViewInputControlNetworkTool::OnMouseUpL(int x, int y, std::uint32_t modifiers) {
    if (Trace()) std::fprintf(stderr, "network input: mouse up at %d,%d (down %d)\n", x, y, mouseDown_ ? 1 : 0);
    if (!mouseDown_) return false;
    // The original releases the capture first (0x002ae4d2). The native window manager sends a synthetic mouse move
    // from inside ReleaseCapture, without the button bit, which OnMouseMove would take for "released outside the
    // window" and cancel the drag; so the flag drops first.
    mouseDown_ = false;
    ReleaseCapture();
    if (outline_ && !outline_->IsStarted()) outline_->Start(0);
    const bool handled = ui_->OnMouseUpL(x, y, modifiers);
    SetCursor(ui_->CursorId());
    return handled;
}

bool cSC4ViewInputControlNetworkTool::OnKeyDown(std::uint32_t key, std::uint32_t) {
    if (!IsOnTop() || key != kKeyEscape) return false;
    if (ui_ && ui_->IsActive()) ui_->CancelInput();
    else EndInput();
    return true;
}

bool cSC4ViewInputControlNetworkTool::DoMessage(cIGZMessage2* message) {
    if (message->GetType() != kMsgViewInputControlChanged) return false;
    void* standard = nullptr;
    cSC4ViewInputControl* next = nullptr;
    if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) && standard) {
        next = static_cast<cSC4ViewInputControl*>(static_cast<cIGZMessage2Standard*>(standard)->GetVoid2());
        static_cast<cIGZMessage2Standard*>(standard)->Release();
    }
    if (next && next != this && next->GetID() != kNetworkIntxToolId) {
        if (ui_) ui_->CancelInput();
        OnMouseExit();
        mouseDown_ = false;
    }
    return true;
}

bool cSC4ViewInputControlNetworkTool::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    cISC4View3DWin* view = View();
    float position[3] = {lastPosition_.x, lastPosition_.y, lastPosition_.z};
    const bool picked = view && view->PickTerrain(x, y, position, false);
    if (picked) {
        // The cell centre: floor(p / cell) * cell + cell * 0.5, in double as the original.
        const double cell = cellSize_;
        position[0] = static_cast<float>(std::floor(static_cast<double>(position[0]) / cell) * cell + cell * 0.5);
        position[2] = static_cast<float>(cell * std::floor(static_cast<double>(position[2]) / cell) + cell * 0.5);
    }
    const bool moved = position[0] != lastPosition_.x || position[1] != lastPosition_.y || position[2] != lastPosition_.z;
    lastPosition_ = cS3DVector3{position[0], position[1], position[2]};
    if (picked && !AmCapturing()) {
        if (moved) {
            localGrid_.SetPosition(lastPosition_);
            localGrid_.ShowEffect(!(terrainView_ && terrainView_->GridShown()));
            if (outline_) {
                cS3DTransform transform;
                int penX = 0, penZ = 0;
                ui_->PenWidth(penX, penZ);
                if (penX > 1) {
                    const float offset = static_cast<float>(penX - 1) * cellSize_ * -0.5f;
                    position[0] += offset;
                    position[2] += offset;
                }
                transform.scale = static_cast<float>(penX) * cellSize_;
                transform.flags = static_cast<std::uint8_t>(transform.scale != 1.0f ? (transform.flags | 2) : (transform.flags & 0xfd));
                transform.translation[0] = position[0];
                transform.translation[1] = position[1];
                transform.translation[2] = position[2];
                transform.flags |= 1;
                transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 2);
                outline_->SetEffectTransform(transform);
                if (!outline_->IsStarted()) outline_->Start(0);
            }
        }
    } else {
        StopOutline();
        localGrid_.ShowEffect(false);
    }
    bool handled = ui_->OnMouseMove(x, y, modifiers);
    if (picked && mouseDown_) {
        const std::uint32_t base = ui_->DirectionalCursorBase();
        if ((modifiers & kButtonLeftDown) == 0) {
            if (Trace()) std::fprintf(stderr, "network input: move without the button (modifiers %x): cancel\n", modifiers);
            // The button went up outside the window.
            if (ui_ && ui_->IsActive()) ui_->CancelInput();
            handled = true;
        } else if (base != 0) {
            const int direction = ui_->LastDragDirection();
            if (direction >= 0) {
                const int rotation = g_host ? g_host->ViewRotation() : 0;
                SetCursor(base + (static_cast<std::uint32_t>(direction - rotation * 2) & 7u));
            }
        }
    }
    return handled;
}

}  // namespace sc4
