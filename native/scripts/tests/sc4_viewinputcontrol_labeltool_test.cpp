// cSC4ViewInputControlLabelTool against OpenSC4 view-input-controls.md section 4: placing (the colour / height table), snapping
// to roads and avenues, Home / End turns, Delete, the selection's alpha and the press / drag / release states.
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

#include "sc4/TextDecalOccupant.h"
#include "sc4/ViewInputControlLabelTool.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// A W N E S edge word from four side kinds (bytes).
std::uint32_t Edges(std::uint32_t w, std::uint32_t n, std::uint32_t e, std::uint32_t s) { return w | n << 8 | e << 16 | s << 24; }

struct Piece final : ILabelNetworkPiece {
    std::uint32_t flags = 0;   // network flags (type bits)
    std::uint32_t type = 0;    // network type
    std::uint32_t edges = 0;
    bool HasAllNetworkFlags(std::uint32_t f) override { return (flags & f) == f; }
    bool HasAnyNetworkFlag(std::uint32_t f) override { return (flags & f) != 0; }
    bool IsOfType(std::uint32_t t) override { return t == type; }
    std::uint32_t EdgeWord(std::uint32_t t) override { return t == type ? edges : 0; }
};

struct Host final : ILabelToolHost {
    std::map<std::pair<int, int>, Piece> pieces;   // by cell
    std::map<std::pair<int, int>, bool> water;
    float hit[3] = {100.0f, 5.0f, 100.0f};
    int rotation = 0;
    cISC4TextDecalOccupant* labelAt = nullptr;
    int removed = 0, added = 0, dialogs = 0;
    bool HasRenderer() override { return true; }
    bool PickTerrain(int, int, float out[3]) override { for (int i = 0; i < 3; ++i) out[i] = hit[i]; return true; }
    int ViewRotation() override { return rotation; }
    void PositionToCell(float x, float z, int& cx, int& cz) override { cx = int(x / 16); cz = int(z / 16); }
    void CellCenter(int cx, int cz, float& x, float& z) override { x = cx * 16 + 8.0f; z = cz * 16 + 8.0f; }
    bool CellHasWater(int cx, int cz) override { return water[{cx, cz}]; }
    ILabelNetworkPiece* NetworkAtCell(int cx, int cz, std::uint32_t mask) override {
        auto it = pieces.find({cx, cz});
        return it != pieces.end() && (it->second.flags & mask) ? &it->second : nullptr;
    }
    ILabelNetworkPiece* NetworkAtPosition(const float* p, std::uint32_t mask) override {
        return NetworkAtCell(int(p[0] / 16), int(p[2] / 16), mask);
    }
    cISC4TextDecalOccupant* LabelAt(float, float) override { if (labelAt) labelAt->AddRef(); return labelAt; }
    cISC4TextDecalOccupant* CreateLabel() override { return nullptr; }
    void AddProp(cISC4Occupant*) override { ++added; }
    void RemoveProp(cISC4Occupant*) override { ++removed; }
    void ShowSignsAndLabels() override {}
    void PlaySound(std::uint32_t, std::uint32_t) override {}
    cIGZWin* View3DWindow() override { return nullptr; }
    cIGZWin* CreateWindowFromScript(std::uint32_t, std::uint32_t, cIGZWin*, std::uint32_t) override { ++dialogs; return nullptr; }
    cIGZWinMgr* WindowManager() override { return nullptr; }
};

}  // namespace

int main() {
    Host host;
    SetLabelToolHost(&host);
    auto* tool = new cSC4ViewInputControlLabelTool();
    tool->AddRef();
    auto* label = new cSC4TextDecalOccupant();
    cISC4TextDecalOccupant* view = label;
    view->AddRef();

    auto place = [&](float x, float z) {
        float p[3] = {x, 5.0f, z};
        tool->UpdateTextDecal(p, view);
        return std::make_pair(p[0], p[2]);
    };

    // ---- the colour / height table (4.3)
    place(100, 100);   // no network: green, height 2, the tool's orientation (1), the position kept
    CHECK(Near(label->Color()[0], 0.5f) && Near(label->Color()[1], 1.0f) && Near(label->Color()[2], 0.5f));
    CHECK(Near(label->TextHeight(), 2.0f) && label->Orientation() == 1);
    CHECK(Near(label->PositionX(), 100) && Near(label->PositionZ(), 100));
    host.water[{6, 6}] = true;
    place(100, 100);   // water: blue
    CHECK(Near(label->Color()[0], 0.5f) && Near(label->Color()[1], 0.5f) && Near(label->Color()[2], 1.0f));
    host.water.clear();

    // ---- roads (4.4): a street (type 3) running W-E through the cell: centre, orientation 1, height 0.64
    Piece& street = host.pieces[{6, 6}];
    street.flags = 1u << 3;
    street.type = 3;
    street.edges = Edges(2, 0, 2, 0);
    auto at = place(100, 100);
    CHECK(Near(label->Color()[0], 1.0f) && Near(label->Color()[1], 0.902f) && Near(label->Color()[2], 0.0f));
    CHECK(Near(label->TextHeight(), 0.64f));
    CHECK(Near(at.first, 104) && Near(at.second, 104) && label->Orientation() == 1);
    // a road (type 0, flag bit 0): height 0.8; N-S: orientation 0
    street.flags = 1u;
    street.type = 0;
    street.edges = Edges(0, 2, 0, 2);
    place(100, 100);
    CHECK(Near(label->TextHeight(), 0.8f) && label->Orientation() == 0);
    // one side only: N -> 0, W -> 1
    street.edges = Edges(0, 2, 0, 0);
    place(100, 100);
    CHECK(label->Orientation() == 0);
    street.edges = Edges(2, 0, 0, 0);
    place(100, 100);
    CHECK(label->Orientation() == 1);
    // the diagonal bends: W + N (N diagonal) -> 2 at (-4, -4); E + S (S diagonal) -> 2 at (4, 4);
    // W + S (S diagonal) -> 3 at (-4, 4); N + E (E diagonal) -> 3 at (4, -4)
    street.edges = Edges(2, 1, 0, 0);
    at = place(100, 100);
    CHECK(label->Orientation() == 2 && Near(at.first, 100) && Near(at.second, 100));
    street.edges = Edges(0, 0, 2, 3);
    at = place(100, 100);
    CHECK(label->Orientation() == 2 && Near(at.first, 108) && Near(at.second, 108));
    street.edges = Edges(2, 0, 0, 1);
    at = place(100, 100);
    CHECK(label->Orientation() == 3 && Near(at.first, 100) && Near(at.second, 108));
    street.edges = Edges(0, 2, 3, 0);
    at = place(100, 100);
    CHECK(label->Orientation() == 3 && Near(at.first, 108) && Near(at.second, 100));
    // rail wins over the other types for the edge word (type 1)
    street.flags = 1u << 1;
    street.type = 1;
    street.edges = Edges(0, 2, 0, 2);
    place(100, 100);
    CHECK(label->Orientation() == 0 && Near(label->TextHeight(), 0.8f));

    // ---- avenues (4.4): height 2, avenue edge word (type 6, 2 bits a side, bit 2 = inner)
    Piece& avenue = host.pieces[{6, 6}];
    avenue.flags = 1u << 6;
    avenue.type = 6;
    avenue.edges = Edges(2 | 4, 0, 2, 0);   // W-E, the inner side W: 8 m west of the centre, orientation 1
    at = place(100, 100);
    CHECK(Near(label->TextHeight(), 2.0f) && label->Orientation() == 1 && Near(at.first, 96) && Near(at.second, 104));
    avenue.edges = Edges(0, 2, 0, 2 | 4);   // N-S, inner S: 8 m south, orientation 0
    at = place(100, 100);
    CHECK(label->Orientation() == 0 && Near(at.first, 104) && Near(at.second, 112));
    avenue.edges = Edges(1, 1, 1, 1);       // four diagonal sides, W kind 1: orientation 2, no offset
    at = place(100, 100);
    CHECK(label->Orientation() == 2 && Near(at.first, 104) && Near(at.second, 104));
    avenue.edges = Edges(3, 0, 3, 0);       // diagonals only, W kind 3: orientation 3 at (-8, 8)
    at = place(100, 100);
    CHECK(label->Orientation() == 3 && Near(at.first, 96) && Near(at.second, 112));
    avenue.edges = Edges(2, 1, 1, 2);       // two diagonal of four, W orthogonal, S orthogonal: 2 at (-8, 8)
    at = place(100, 100);
    CHECK(label->Orientation() == 2 && Near(at.first, 96) && Near(at.second, 112));
    host.pieces.clear();

    // ---- Home / End turn the tool's orientation (Home 0 -> 3 -> 1 -> 2 -> 0) and re-place the selection
    tool->SetSelection(view);
    CHECK(Near(label->Alpha(), 0.7f));
    CHECK(tool->Orientation() == 1);
    CHECK(!tool->OnKeyDown(0x24, 0));   // Home: 1 -> 2
    CHECK(tool->Orientation() == 2 && label->Orientation() == 2);
    CHECK(!tool->OnKeyDown(0x23, 0));   // End: 2 -> 1
    CHECK(tool->Orientation() == 1);
    tool->OnKeyDown(0x23, 0);           // End: 1 -> 3
    CHECK(tool->Orientation() == 3);

    // ---- press on a label grabs it; moving drags it (dynamic); release ends the drag
    host.labelAt = view;
    host.hit[0] = 150.0f;
    host.hit[2] = 160.0f;
    CHECK(tool->OnMouseDownL(10, 10, 0));
    CHECK(tool->GetState() == cSC4ViewInputControlLabelTool::kPressed && tool->Selection() == view);
    tool->OnMouseMove(12, 12, 0);
    CHECK(tool->GetState() == cSC4ViewInputControlLabelTool::kDragging && label->Dynamic());
    CHECK(Near(label->PositionX(), 150) && Near(label->PositionZ(), 160));   // the raw hit (the press offset is unused)
    tool->OnMouseUpL(12, 12, 0);
    CHECK(tool->GetState() == cSC4ViewInputControlLabelTool::kIdle && !label->Dynamic());

    // ---- Delete while idle removes the selection without touching its alpha
    CHECK(tool->OnKeyDown(0x2e, 0));
    CHECK(host.removed == 1 && tool->Selection() == nullptr);
    CHECK(Near(label->Alpha(), 0.7f));

    // ---- hovering selects the label under the cursor (0.7) and leaving it puts it back (0.5)
    tool->OnMouseMove(5, 5, 0);
    CHECK(tool->Selection() == view && Near(label->Alpha(), 0.7f));
    host.labelAt = nullptr;
    tool->OnMouseMove(6, 6, 0);
    CHECK(tool->Selection() == nullptr && Near(label->Alpha(), 0.5f));

    tool->Release();
    view->Release();
    SetLabelToolHost(nullptr);
    if (failures == 0) std::printf("sc4_viewinputcontrol_labeltool: all passed\n");
    return failures == 0 ? 0 : 1;
}
