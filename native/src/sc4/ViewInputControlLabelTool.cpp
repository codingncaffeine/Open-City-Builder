// cSC4ViewInputControlLabelTool (Mac i386 0x0049110c..0x00492224; spec OpenSC4 view-input-controls.md section 4). See the header.
#include "sc4/ViewInputControlLabelTool.h"

#include <cstdio>
#include <cstdlib>

#include "gz/WinTextEdit.h"

namespace sc4 {

namespace {
ILabelToolHost* g_labelHost = nullptr;

bool Trace() {
    static const bool on = std::getenv("SC4_SIGNDEBUG") != nullptr;
    return on;
}

// OnKeyDown's kNextOrientation[orientation][key != Home]: Home 0 -> 3 -> 1 -> 2 -> 0, End 0 -> 2 -> 1 -> 3 -> 0.
constexpr int kNextOrientation[4][2] = {{3, 2}, {2, 3}, {0, 1}, {1, 0}};
// FindAvenueCenterAndOrientation's kEdgeOffset: the step toward each inner side, W N E S.
constexpr float kEdgeOffset[4][2] = {{-8.0f, 0.0f}, {0.0f, -8.0f}, {8.0f, 0.0f}, {0.0f, 8.0f}};
constexpr std::uint32_t kKeyEnd = 0x23, kKeyHome = 0x24, kKeyDelete = 0x2e;
}  // namespace

ILabelToolHost* LabelToolHost() { return g_labelHost; }
void SetLabelToolHost(ILabelToolHost* host) { g_labelHost = host; }

cSC4ViewInputControlLabelTool::cSC4ViewInputControlLabelTool() = default;

cSC4ViewInputControlLabelTool::~cSC4ViewInputControlLabelTool() {
    if (selection_) {
        cISC4TextDecalOccupant* old = selection_;
        selection_ = nullptr;
        old->Release();
    }
}

bool cSC4ViewInputControlLabelTool::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kGZIID_cISC4ViewInputControl) {
        *out = static_cast<cSC4ViewInputControl*>(this);
        AddRef();
        return true;
    }
    return false;
}

// 0x0049110c: id 0x8b3ce416, cursor 0xeb72fef4; signs and labels are shown while the tool is up.
bool cSC4ViewInputControlLabelTool::Init() {
    if (!initialized_) {
        id_ = kLabelToolId;
        cursorId_ = kLabelToolCursor;
        if (g_labelHost) g_labelHost->ShowSignsAndLabels();
        cSC4ViewInputControl::Init();
    }
    return true;
}

// 0x004912c8
bool cSC4ViewInputControlLabelTool::Shutdown() {
    if (initialized_) {
        SetSelection(nullptr);
        cSC4ViewInputControl::Shutdown();
    }
    return true;
}

// 0x0049126e: the new selection at alpha 0.7, the old one back to 0.5.
void cSC4ViewInputControlLabelTool::SetSelection(cISC4TextDecalOccupant* label) {
    if (selection_ == label) return;
    if (label) label->SetAlpha(0.7f);
    if (selection_) selection_->SetAlpha(0.5f);
    if (label) label->AddRef();
    cISC4TextDecalOccupant* old = selection_;
    selection_ = label;
    if (old) old->Release();
}

// 0x004912fc: the occupant manager's first label at (x, z); the manager keeps it (no reference for the caller).
cISC4TextDecalOccupant* cSC4ViewInputControlLabelTool::GetDecalOccupantAt(const float* position) {
    cISC4TextDecalOccupant* label = g_labelHost ? g_labelHost->LabelAt(position[0], position[2]) : nullptr;
    if (label) label->Release();
    return label;
}

// 0x004913f0: on an avenue piece, the position moves to the cell's centre plus the offset its connections give.
void cSC4ViewInputControlLabelTool::FindAvenueCenterAndOrientation(float* position, int& orientation) {
    orientation = 1;
    int cellX = 0, cellZ = 0;
    g_labelHost->PositionToCell(position[0], position[2], cellX, cellZ);
    ILabelNetworkPiece* piece = g_labelHost->NetworkAtPosition(position, 0x40);
    if (!piece) return;
    float offset[3] = {0.0f, 0.0f, 0.0f};
    std::uint32_t edges = piece->EdgeWord(6);
    std::uint32_t kind[4];
    bool inner[4];
    int connected = 0, diagonal = 0;
    for (int side = 0; side < 4; ++side) {
        inner[side] = ((edges >> 2) & 1) != 0;
        kind[side] = edges & 3;
        if (kind[side] == 1 || kind[side] == 3) ++diagonal;
        if (kind[side] != 0) ++connected;
        edges >>= 8;
    }
    constexpr int W = 0, E = 2, S = 3;
    auto set = [&](float x, float z) { offset[0] = x; offset[2] = z; };
    if (connected == diagonal) {
        if (connected == 4) {
            orientation = kind[W] != 1 ? 3 : 2;
        } else if (kind[W] == 1) {
            orientation = 2;
            set(-8.0f, -8.0f);
        } else if (kind[W] == 3) {
            orientation = 3;
            set(-8.0f, 8.0f);
        } else if (kind[E] == 1) {
            orientation = 2;
            set(8.0f, 8.0f);
        } else if (kind[E] == 3) {
            orientation = 3;
            set(8.0f, -8.0f);
        }
    } else if (diagonal == 2 && connected == 4) {
        if (kind[W] == 2) {
            if (kind[S] != 2) { orientation = 3; set(-8.0f, -8.0f); }
            else { orientation = 2; set(-8.0f, 8.0f); }
        } else if (kind[E] == 2) {
            if (kind[S] != 2) { orientation = 2; set(8.0f, -8.0f); }
            else { orientation = 3; set(8.0f, 8.0f); }
        }
    } else {
        for (int side = 0; side < 4; ++side) {
            if (!inner[side]) continue;
            offset[0] += kEdgeOffset[side][0];
            offset[2] += kEdgeOffset[side][1];
        }
        orientation = (kind[W] == 2 || kind[E] == 2) ? 1 : 0;
    }
    g_labelHost->CellCenter(cellX, cellZ, position[0], position[2]);
    position[0] += offset[0];
    position[1] += offset[1];
    position[2] += offset[2];
}

// 0x004916cc: on a road, rail, one-way road or street, the cell's centre (and a quarter-cell step on a diagonal bend).
void cSC4ViewInputControlLabelTool::FindRoadCenterAndOrientation(float* position, int& orientation) {
    orientation = 1;
    int cellX = 0, cellZ = 0;
    g_labelHost->PositionToCell(position[0], position[2], cellX, cellZ);
    float dx = 0.0f, dz = 0.0f;
    if (ILabelNetworkPiece* piece = g_labelHost->NetworkAtCell(cellX, cellZ, 0x40b)) {
        std::uint32_t type = piece->IsOfType(0) ? 0 : 3;   // road, else street
        if (piece->IsOfType(10)) type = 10;                // one-way road
        if (piece->IsOfType(1)) type = 1;                  // rail
        std::uint32_t edges = piece->EdgeWord(type);
        bool connected[4], diagonal[4];
        int count = 0;
        for (int side = 0; side < 4; ++side) {
            const std::uint32_t kind = edges & 7;
            connected[side] = kind != 0;
            if (kind != 0) ++count;
            diagonal[side] = kind == 1 || kind == 3;
            edges >>= 8;
        }
        constexpr int W = 0, N = 1, E = 2, S = 3;
        if (count == 1) {
            orientation = (!connected[N] && !connected[S]) ? 1 : 0;
        } else if (count == 2 || count == 3) {
            if (connected[W] && connected[E] && !diagonal[E]) orientation = 1;
            else if (connected[N] && connected[S] && !diagonal[N]) orientation = 0;
            else if (connected[W] && connected[N] && diagonal[N]) { orientation = 2; dx = dz = -4.0f; }
            else if (connected[E] && connected[S] && diagonal[S]) { orientation = 2; dx = dz = 4.0f; }
            else if (connected[W] && connected[S] && diagonal[S]) { orientation = 3; dx = -4.0f; dz = 4.0f; }
            else if (connected[N] && connected[E] && diagonal[E]) { orientation = 3; dx = 4.0f; dz = -4.0f; }
        }
    }
    g_labelHost->CellCenter(cellX, cellZ, position[0], position[2]);
    position[0] += dx;
    position[2] += dz;
}

// 0x00491972: the label's colour, height, position and orientation from what lies under it.
void cSC4ViewInputControlLabelTool::UpdateTextDecal(float* position, cISC4TextDecalOccupant* label) {
    if (!label || !g_labelHost) return;
    int cellX = 0, cellZ = 0;
    g_labelHost->PositionToCell(position[0], position[2], cellX, cellZ);
    const bool water = g_labelHost->CellHasWater(cellX, cellZ);
    ILabelNetworkPiece* piece = g_labelHost->NetworkAtCell(cellX, cellZ, 0x44b);
    float color[3];
    float height = 2.0f;
    int orientation = orientation_;
    if (water) {
        color[0] = 0.5f; color[1] = 0.5f; color[2] = 1.0f;
    } else if (!piece) {
        color[0] = 0.5f; color[1] = 1.0f; color[2] = 0.5f;
    } else {
        color[0] = 1.0f; color[1] = 0.902f; color[2] = 0.0f;   // 0x3f66e6e7
        if (!piece->HasAllNetworkFlags(0x40)) {
            height = piece->HasAnyNetworkFlag(0x403) ? 0.8f : 0.64f;
            FindRoadCenterAndOrientation(position, orientation);
        } else {
            FindAvenueCenterAndOrientation(position, orientation);
        }
    }
    label->SetTextColor(color);
    label->SetTextHeight(height);
    label->AsOccupant()->SetPosition(position);
    label->SetOrientation(orientation);
    if (Trace()) {   // which neighbouring cells carry a road (where the click was relative to the network)
        std::fprintf(stderr, "label tool: networks around %d,%d:", cellX, cellZ);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) std::fprintf(stderr, "%c", g_labelHost->NetworkAtCell(cellX + dx, cellZ + dz, 0x44b) ? '#' : '.');
        std::fprintf(stderr, "\n");
    }
    if (Trace()) std::fprintf(stderr, "label tool: placed at %.1f,%.1f,%.1f cell %d,%d water %d network %d height %.2f orientation %d\n",
                              position[0], position[1], position[2], cellX, cellZ, water, piece != nullptr, height, orientation);
}

// 0x00491b3e: the edit dialog, modal; OK with text keeps it, OK empty or Cancel fails, anything else succeeds.
bool cSC4ViewInputControlLabelTool::DoEditLabelUI(cISC4TextDecalOccupant* label) {
    if (!label || !g_labelHost) return true;
    cIGZWin* view = g_labelHost->View3DWindow();
    cIGZWin* dialog = g_labelHost->CreateWindowFromScript(kLabelDialogScriptGroup, kLabelDialogScriptInstance, view, kLabelDialogWindow);
    if (!dialog) return true;
    cIGZWinMgr* manager = g_labelHost->WindowManager();
    dialog->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));
    void* raw = nullptr;
    if (dialog->GetChildAsRecursive(kLabelDialogTextField, gz::kGZIID_cIGZWinTextEdit, &raw) && raw) {
        auto* field = static_cast<gz::cIGZWinTextEdit*>(raw);
        cRZString text;
        label->GetText(text);
        field->SetText(text, false);
        field->SetSelection(0, text.Strlen());
        field->SetMaximumTextToAllow(0x20);
        field->Release();
    }
    if (manager) manager->GZSetFocus(manager->GetMainWindow());
    const int result = manager ? manager->RunPopup(dialog) : 0;   // DoModalWin
    bool ok = true;
    if (static_cast<std::uint32_t>(result) == kLabelDialogOk) {
        raw = nullptr;
        if (dialog->GetChildAsRecursive(kLabelDialogTextField, gz::kGZIID_cIGZWinTextEdit, &raw) && raw) {
            auto* field = static_cast<gz::cIGZWinTextEdit*>(raw);
            cRZString text;
            field->GetText(text);
            ok = text.Strlen() != 0;
            label->SetText(text);
            field->Release();
        }
    } else if (static_cast<std::uint32_t>(result) == kLabelDialogCancel) {
        ok = false;
    }
    if (Trace()) {
        cRZString text;
        label->GetText(text);
        std::fprintf(stderr, "label tool: dialog result %08x -> %s, text \"%s\"\n", static_cast<std::uint32_t>(result), ok ? "kept" : "failed", text.ToChar());
    }
    if (manager) manager->GZSetFocus(view);
    dialog->Shutdown();
    dialog->Release();
    return ok;
}

// 0x00491154: the dialog's filter. Its focus goes to the text field; Enter in the field is OK; OK, Cancel and the close
// button end the modal loop with their id.
bool cSC4ViewInputControlLabelTool::DoMessage(cIGZWin* window, cGZMessage& message) {
    if (Trace() && message.type != kGZMsgMouseMove)
        std::fprintf(stderr, "label tool: dialog message %08x %llx %llx %llx\n", message.type, static_cast<unsigned long long>(message.param1),
                     static_cast<unsigned long long>(message.param2), static_cast<unsigned long long>(message.param3));
    if (message.type == kGZMsgSetFocus) {
        void* raw = nullptr;
        if (window && window->GetChildAsRecursive(kLabelDialogTextField, gz::kGZIID_cIGZWinTextEdit, &raw) && raw) {
            auto* field = static_cast<gz::cIGZWinTextEdit*>(raw);
            if (cIGZWinMgr* manager = window->GetWindowManager()) manager->GZSetFocus(field->AsIGZWin());
            field->Release();
            return true;
        }
        return false;
    }
    // Enter: the keyboard message the dialog's filters see first (type 0x1a, the inner message's type in param2 and the message
    // itself in param3): the Enter character (type 4, character 0xd).
    const auto* inner = reinterpret_cast<const cGZMessage*>(message.param3);
    const bool enter = message.type == 0x1a && message.param2 == kGZMsgCharacter && inner && inner->param1 == 0xd;
    if (enter) {
        if (window) {
            if (cIGZWinMgr* manager = window->GetWindowManager()) manager->EndDialog(0xf, window, kLabelDialogOk);
        }
        return false;
    }
    if (message.type == kGZMsgCommand && message.param1 == 0x287259f6 &&
        (message.param2 == kLabelDialogOk || message.param2 == kLabelDialogCancel || message.param2 == kLabelDialogClose)) {
        if (window) {
            if (cIGZWinMgr* manager = window->GetWindowManager()) manager->EndDialog(0xf, window, static_cast<std::int64_t>(message.param2));
        }
        return true;
    }
    return false;
}

// 0x00491dc8: Delete removes the selected label (idle only); Home / End turn the orientation and re-place the label.
bool cSC4ViewInputControlLabelTool::OnKeyDown(std::uint32_t key, std::uint32_t) {
    if (key == kKeyDelete && selection_ && state_ == kIdle) {
        cISC4TextDecalOccupant* label = selection_;
        selection_ = nullptr;   // the reference moves here (no alpha change)
        if (g_labelHost) g_labelHost->RemoveProp(label->AsOccupant());
        label->Release();
        return true;
    }
    if (key - kKeyEnd < 2 && selection_) {
        orientation_ = kNextOrientation[orientation_ & 3][key != kKeyHome ? 1 : 0];
        float position[3] = {0, 0, 0};
        selection_->AsOccupant()->GetPosition(position);
        UpdateTextDecal(position, selection_);
    }
    return false;
}

// 0x00491edc: a click on a label grabs it; elsewhere a new label is made, named in the dialog and placed.
bool cSC4ViewInputControlLabelTool::OnMouseDownL(int x, int y, std::uint32_t) {
    if (!g_labelHost || !g_labelHost->HasRenderer()) return true;
    float hit[3] = {0, 0, 0};
    if (!g_labelHost->PickTerrain(x, y, hit)) return false;
    cISC4TextDecalOccupant* label = GetDecalOccupantAt(hit);
    if (!label) {
        cISC4TextDecalOccupant* made = g_labelHost->CreateLabel();
        if (!made) return true;
        g_labelHost->AddProp(made->AsOccupant());
        if (!DoEditLabelUI(made)) {
            g_labelHost->RemoveProp(made->AsOccupant());
        } else {
            const int rotation = g_labelHost->ViewRotation();
            orientation_ = (rotation == 0 || rotation == 2) ? 1 : 0;
            float position[3] = {hit[0], hit[1], hit[2]};
            UpdateTextDecal(position, made);
            SetSelection(made);
        }
        made->Release();
        return true;
    }
    label->AddRef();
    state_ = kPressed;
    SetSelection(label);
    selection_->AsOccupant()->GetPosition(pressOffset_);
    for (int i = 0; i < 3; ++i) pressOffset_[i] -= hit[i];
    g_labelHost->PlaySound(kLabelToolSoundGroup, kLabelToolSoundGrab);
    label->Release();
    return true;
}

// 0x0049212a: a click (no drag) edits the label (a failed edit removes it); a drag ends.
bool cSC4ViewInputControlLabelTool::OnMouseUpL(int, int, std::uint32_t) {
    if (state_ == kPressed) {
        if (!DoEditLabelUI(selection_)) {
            cISC4TextDecalOccupant* label = selection_;
            if (label) label->AddRef();
            SetSelection(nullptr);
            if (label && g_labelHost) g_labelHost->RemoveProp(label->AsOccupant());
            if (label) label->Release();
        }
    } else if (state_ == kDragging) {
        if (selection_) selection_->SetDynamic(false);
        if (g_labelHost) g_labelHost->PlaySound(kLabelToolSoundGroup, kLabelToolSoundDrop);
    }
    state_ = kIdle;
    return true;
}

// 0x00492224: pressed + moved starts a drag; a drag follows the terrain; idle hovering selects the label under the cursor.
bool cSC4ViewInputControlLabelTool::OnMouseMove(int x, int y, std::uint32_t) {
    if (state_ == kPressed && selection_) {
        state_ = kDragging;
        selection_->SetDynamic(true);
    }
    if (state_ == kDragging && g_labelHost && g_labelHost->HasRenderer()) {
        float hit[3] = {0, 0, 0};
        if (g_labelHost->PickTerrain(x, y, hit)) UpdateTextDecal(hit, selection_);
    }
    if (state_ == kIdle && g_labelHost && g_labelHost->HasRenderer()) {
        float hit[3] = {0, 0, 0};
        cISC4TextDecalOccupant* hovered = nullptr;
        if (g_labelHost->PickTerrain(x, y, hit)) hovered = GetDecalOccupantAt(hit);
        if (hovered) hovered->AddRef();
        SetSelection(hovered);
        if (hovered) hovered->Release();
    }
    return true;
}

}  // namespace sc4
