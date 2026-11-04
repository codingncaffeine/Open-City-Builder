#include "sc4/ViewInputControlDispatch.h"

#include "rz/cRZMessage2Standard.h"

namespace sc4 {

namespace {
IDispatchToolHost* g_host = nullptr;
constexpr std::uint32_t kGZIID_cIGZMessageTarget2Id = 0x452294aa;
constexpr std::uint32_t kMsgOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kMsgBegin = 0xe94671eb, kMsgActive = 0xe94671ec, kMsgInactive = 0xe94671ed, kMsgEnd = 0x894671f6;
constexpr std::uint32_t kQueryToolId = 0x48e2a1f8;   // a tool that takes the top without hiding this one's text
}  // namespace

IDispatchToolHost* DispatchToolHost() { return g_host; }
void SetDispatchToolHost(IDispatchToolHost* host) { g_host = host; }

cSC4ViewInputControlDispatch::cSC4ViewInputControlDispatch(std::uint32_t type) : type_(type) {}

cSC4ViewInputControlDispatch::~cSC4ViewInputControlDispatch() {
    if (hover_) hover_->Release();
}

// 0x002da83c: iid 1 and cIGZMessageTarget2 answer the message-target view (+0x28); the base answers the tool view.
bool cSC4ViewInputControlDispatch::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kGZIID_cIGZMessageTarget2Id) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    return cSC4ViewInputControl::QueryInterface(iid, out);
}

std::uint32_t cSC4ViewInputControlDispatch::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}

// 0x002db222: the tool's id and cursor (fire types 1 / 5, police 0 / 6), BeginDispatchTool, the five messages, the cursor text.
bool cSC4ViewInputControlDispatch::Init() {
    if (initialized_) return true;
    id_ = kDispatchToolId;
    cursorId_ = 1;
    if (type_ == 1 || type_ == 5) cursorId_ = kDispatchToolCursorFire;
    else if (type_ == 0 || type_ == 6) cursorId_ = kDispatchToolCursorPolice;
    toolCursor_ = cursorId_;
    cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr;
    if (manager) manager->BeginDispatchTool(static_cast<tDispatchType>(type_));
    timer_.SetUnits(cRZTimer::kMilliseconds);
    cSC4ViewInputControl::Init();
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        auto* target = static_cast<cIGZMessageTarget2*>(this);
        server->AddNotification(target, kDispatchToolMsgTopChanged);
        server->AddNotification(target, kMsgBegin);
        server->AddNotification(target, kMsgActive);
        server->AddNotification(target, kMsgInactive);
        server->AddNotification(target, kMsgEnd);
    }
    onTop_ = true;
    UpdateCursorText();
    return true;
}

// 0x002db36e: the text and the messages go, a picked-up call box goes back where it was, EndDispatchTool.
bool cSC4ViewInputControlDispatch::Shutdown() {
    if (!initialized_) return true;
    if (g_host) g_host->ClearCursorText(kDispatchToolCursorTextId);
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        auto* target = static_cast<cIGZMessageTarget2*>(this);
        server->RemoveNotification(target, kDispatchToolMsgTopChanged);
        server->RemoveNotification(target, kMsgBegin);
        server->RemoveNotification(target, kMsgActive);
        server->RemoveNotification(target, kMsgInactive);
        server->RemoveNotification(target, kMsgEnd);
    }
    if (hover_) {
        hover_->Release();
        hover_ = nullptr;
    }
    if (cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr) {
        RestorePickedUpPylon(manager);
        manager->EndDispatchTool(static_cast<tDispatchType>(type_));
    }
    cSC4ViewInputControl::Shutdown();
    return true;
}

void cSC4ViewInputControlDispatch::RestorePickedUpPylon(cISC4DispatchManager* manager) {
    IPylonOccupant* active = manager->GetActivePylon();
    if (!active) return;
    active->AsOccupant()->SetPosition(pickedUpPosition_);
    active->SetDispatchState(static_cast<tDispatchState>(pickedUpState_));
    manager->SetActivePylon(nullptr);
}

// 0x002da570: every key but Tab that the base passes on goes to the view.
bool cSC4ViewInputControlDispatch::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    return !(key != 9 && IsKeyToBePassed(key, modifiers));
}

// 0x002da5a4
bool cSC4ViewInputControlDispatch::PickPylonOccupant(int x, int y, IPylonOccupant*& out) {
    out = g_host ? g_host->PickPylon(x, y) : nullptr;
    return out != nullptr;
}

// 0x002da648: Dispatch(type, x, z) != 0, for a real type (8 = none) at an OK location.
bool cSC4ViewInputControlDispatch::InitiateDispatch(std::uint32_t type, const float position[3], bool ok) {
    cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr;
    return type != kDispatchTypeNone && manager && ok && manager->Dispatch(static_cast<tDispatchType>(type), position[0], position[2]) != 0;
}

// 0x002da69a: the old hovered call box back to UI state 1, the new one (held) to 2.
void cSC4ViewInputControlDispatch::SetHoverPylon(IPylonOccupant* pylon) {
    if (hover_ == pylon) return;
    if (hover_) {
        hover_->SetUIState(1);
        hover_->Release();
        hover_ = nullptr;
    }
    hover_ = pylon;
    if (pylon) {
        pylon->AddRef();
        pylon->SetUIState(2);
    }
}

// 0x002da706: the call box's screen position minus the mouse, so a drag keeps the grab point.
void cSC4ViewInputControlDispatch::CalculateDragPylonOffset(IPylonOccupant* pylon, int x, int y) {
    float position[3] = {0, 0, 0};
    pylon->AsOccupant()->GetPosition(position);
    float sx = 0, sy = 0;
    if (g_host) g_host->ProjectToViewport(position, sx, sy);
    dragOffsetX_ = static_cast<int>(sx - static_cast<float>(x));
    dragOffsetY_ = static_cast<int>(sy - static_cast<float>(y));
}

// 0x002da772: the LTEXT format with the free unit count, as cursor text 0x49d02c4d.
void cSC4ViewInputControlDispatch::UpdateCursorText() {
    if (!g_host) return;
    cISC4DispatchManager* manager = g_host->DispatchManager();
    cRZString format;
    g_host->CursorTextFormat(format);
    const int available = manager ? manager->GetAvailablePylonCount(static_cast<tDispatchType>(type_)) : 0;
    cursorText_.Sprintf(format.ToChar(), available);
    g_host->SetCursorText(kDispatchToolCursorTextId, cursorText_);
}

// 0x002da8ca: posted on spMS2 with the pylon as IGZUnknown and data 0.
void cSC4ViewInputControlDispatch::SendDispatchMessage(std::uint32_t type, IPylonOccupant* pylon) {
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetIGZUnknown(pylon);
    message->SetData1(0);
    message->SetData2(0);
    message->SetData3(0);
    message->SetData4(0);
    server->MessagePost(message, false);
    message->Release();
}

// 0x002daafc
bool cSC4ViewInputControlDispatch::LocationIsOKForDispatch(std::uint32_t type, const float position[3], bool ok) {
    cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr;
    return manager && ok && manager->CanDispatch(static_cast<tDispatchType>(type), position[0], position[2]);
}

// 0x002da994
bool cSC4ViewInputControlDispatch::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    if (type == kMsgOccupantRemoved) {
        if (hover_ && standard->GetVoid1() == hover_->AsOccupant()) {
            hover_->Release();
            hover_ = nullptr;
        }
        return true;
    }
    if (type == kDispatchToolMsgTopChanged) {
        auto* left = static_cast<cIGZUnknown*>(standard->GetVoid1());
        auto* entered = static_cast<cIGZUnknown*>(standard->GetVoid2());
        auto* self = static_cast<cSC4ViewInputControl*>(this);
        if (left == self) {
            if (entered == self) return true;
            if (auto* tool = dynamic_cast<cSC4ViewInputControl*>(entered); tool && tool->GetID() == kQueryToolId) return true;
            onTop_ = false;
            if (g_host) g_host->ClearCursorText(kDispatchToolCursorTextId);
            SetHoverPylon(nullptr);
            if (cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr) RestorePickedUpPylon(manager);
            return true;
        }
        if (entered != self) return true;
        onTop_ = true;
        UpdateCursorText();
        return true;
    }
    if (type == kMsgBegin || type == kMsgActive || type == kMsgInactive || type == kMsgEnd) {
        if (onTop_) UpdateCursorText();
        return true;
    }
    return false;
}

// 0x002dab42: hovering a call box shows the hand; with one picked up it follows the mouse (after the 500 ms double-click
// window) and turns state 3 where it could go, 1 where it could not.
bool cSC4ViewInputControlDispatch::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    bool ok = false;
    if (view3D_ && g_host) {
        cISC4DispatchManager* manager = g_host->DispatchManager();
        IPylonOccupant* active = manager ? manager->GetActivePylon() : nullptr;
        bool pick = true;
        if (!active) {
            IPylonOccupant* picked = nullptr;
            if (!PickPylonOccupant(x, y, picked)) {
                SetCursor(toolCursor_);
                SetHoverPylon(nullptr);
            } else {
                if (picked->GetUIState() == 0) {
                    SetHoverPylon(nullptr);
                    SetCursor(toolCursor_);
                } else {
                    SetHoverPylon(picked);
                    SetCursor(kDispatchToolHandCursor);
                }
                picked->Release();
            }
        } else {
            SetCursor(kDispatchToolHandCursor);
            if (timer_.IsTimerRunning()) {
                if (timer_.GetElapsedTimeU32() < 500) return true;
                timer_.Reset();
            }
            x += dragOffsetX_;
            y += dragOffsetY_;
        }
        float position[3] = {0, 0, 0};
        if (pick && g_host->PickTerrain(x, y, position)) {
            position[1] = g_host->Altitude(position[0], position[2]);
            if (LocationIsOKForDispatch(type_, position, ((modifiers >> 1) & 1) == 0)) {
                if (active) {
                    active->AsOccupant()->SetPosition(position);
                    active->SetDispatchState(kDispatchStateDispatched);
                }
                ok = true;
            } else if (active) {
                active->SetDispatchState(static_cast<tDispatchState>(1));
            }
        }
    }
    locationOK_ = ok;
    return true;
}

// 0x002dad52: dropped on its own station's lot (cells widened by one at the far edge) the unit goes home; anywhere the unit may
// go it is sent there. The active call box is let go either way.
bool cSC4ViewInputControlDispatch::DropActivePylon() {
    cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr;
    IPylonOccupant* active = manager ? manager->GetActivePylon() : nullptr;
    if (!active) return false;
    float position[3] = {0, 0, 0};
    active->AsOccupant()->GetPosition(position);
    bool result = false;
    cISC4Occupant* station = active->GetSourceOccupant();
    bool home = false;
    if (station) {
        SC4CellRect cells;
        station->GetBoundingCityCells(cells);
        float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        g_host->CellToWorld(cells.x0, cells.z0, x0, z0);
        g_host->CellToWorld(cells.x1 + 1, cells.z1 + 1, x1, z1);
        home = !(position[0] < x0 || x1 < position[0] || position[2] < z0 || z1 < position[2]);
    }
    if (home) {
        manager->RevokeDispatchByPylon(active);
    } else if (LocationIsOKForDispatch(type_, position, true)) {
        result = InitiateDispatch(type_, position, true);
    }
    manager->SetActivePylon(nullptr);
    return result;
}

// 0x002daeb6
bool cSC4ViewInputControlDispatch::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    if (!view3D_ || !g_host) {
        PlayResultSound(0);
        return false;
    }
    cISC4DispatchManager* manager = g_host->DispatchManager();
    if (!manager) {
        PlayResultSound(0);
        return false;
    }
    // A second click within 500 ms: drop the call box and follow its vehicle.
    if (timer_.IsTimerRunning() && timer_.GetElapsedTimeU32() < 500) {
        timer_.Reset();
        IPylonOccupant* active = manager->GetActivePylon();
        DropActivePylon();
        if (IDispatchAutomaton* vehicle = manager->GetAutomatonFromPylon(active)) {
            g_host->FollowAutomaton(vehicle);
            SendDispatchMessage(kDispatchToolMsgFollow, active);
        }
        PlayResultSound(0);
        return true;
    }
    timer_.Restart();
    IPylonOccupant* picked = nullptr;
    if (PickPylonOccupant(x, y, picked)) {
        bool handled = false;
        if (manager->GetActivePylon() != picked) {
            if ((modifiers & 1) == 0) {
                DropActivePylon();
                manager->SetActivePylon(picked);
                picked->AsOccupant()->GetPosition(pickedUpPosition_);
                pickedUpState_ = static_cast<std::uint32_t>(picked->GetDispatchState());
                CalculateDragPylonOffset(picked, x, y);
                g_host->FollowAutomaton(nullptr);
                SendDispatchMessage(kDispatchToolMsgPickUp, picked);
            } else {
                DropActivePylon();
                if (IDispatchAutomaton* vehicle = manager->GetAutomatonFromPylon(picked)) {
                    g_host->FollowAutomaton(vehicle);
                    SendDispatchMessage(kDispatchToolMsgFollow, picked);
                }
            }
            handled = true;
        }
        picked->Release();
        if (handled) {
            PlayResultSound(0);
            return true;
        }
    }
    IPylonOccupant* active = manager->GetActivePylon();
    if (active) {
        x += dragOffsetX_;
        y += dragOffsetY_;
    }
    float position[3] = {0, 0, 0};
    if (g_host->PickTerrain(x, y, position)) {
        position[1] = g_host->Altitude(position[0], position[2]);
        if (!active) {
            if (LocationIsOKForDispatch(type_, position, true) && InitiateDispatch(type_, position, true)) {
                PlayResultSound(0);
                return true;
            }
        } else {
            active->AsOccupant()->SetPosition(position);
            DropActivePylon();
        }
    }
    PlayResultSound(0);
    return false;
}

}  // namespace sc4
