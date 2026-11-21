// cGZWinFolderRow test: tab measuring and the full-row refusal, the round-robin stretch, selection by name / id /
// click, the paint (outline bitmap halves, caps, tiled middle, the selected tab's spread and draw order, the centred
// name), the click forwarded to the parent in its coordinates, the font / bitmap references, and cGZWinFolders laying
// real rows out through its default host.
#include <cstdio>
#include <string>
#include <vector>

#include "fake_graph.h"
#include "fake_winmgr.h"
#include "gz/Buffer.h"
#include "gz/DrawContext.h"
#include "gz/WinFolderRow.h"
#include "gz/WinFolders.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace gz;

namespace {

std::string R(const cRZRect& r) {
    return std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) + "," + std::to_string(r.bottom);
}

struct Ctx final : cIGZDrawContext {
    std::vector<std::string> log;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void PushTransform() override {} void ClipReset() override {} void Clip(const cRZRect&) override {}
    void ClipAbsolute(const cRZRect&) override {} void Offset(const cRZPoint&) override {}
    void OffsetAbsolute(const cRZPoint&) override {} void PopTransform() override {} void PushState() override {}
    void PopState() override {} cRZRect GetCanvasArea() override { return {}; } cRZRect GetClipRect() override { return {}; }
    cRZPoint GetOrigin() override { return {}; } int GetTransformStackDepth() override { return 0; }
    bool IsTargetAttached() override { return true; } void SetTarget(IDrawTargetBuffer*) override {}
    cIGZDrawEngine* GetEngine() override { return nullptr; } void ResetToDefaultState() override {}
    void SetColor(std::uint32_t) override {} void SetColor(std::uint32_t, std::uint32_t) override {} void SetAlphaBlend(int) override {}
    void SetBltModulate(bool) override {} void SetAntialiasing(bool) override {} void SetLineThickness(float) override {}
    void ClearGradient() override {} void SetGradientTransform(const float (*)[4], float, float) override {}
    void ClearBlt() override {} void SetBlt(cIGZUnknown*, float, float, float, float) override {}
    void ClearBltTiling() override {} void SetBltTiling(const cRZRect&) override {} void Clear() override {}
    void Point(int, int) override {} void Line(int, int, int, int) override {} void Fill(const cRZRect&) override {}
    void Ellipse(int, int, int, int) override {} void FillEllipse(int, int, int, int) override {}
    void Blt(IDrawTargetBuffer*, const cRZRect& s, const cRZRect& d) override { log.push_back("blt " + R(s) + ">" + R(d)); }
    void BltTiled(IDrawTargetBuffer*, const cRZRect& s, const cRZRect& d, int dx, int dy) override {
        log.push_back("tile " + R(s) + ">" + R(d) + " " + std::to_string(dx) + "," + std::to_string(dy));
    }
    void Flush() override {} bool IsSuspended() override { return false; } void Suspend() override {} void Resume() override {}
    void Draw(eDrawingOp, const float*, int) override {} void DrawNoTransform(eDrawingOp, const float*, int) override {}
    bool Lock(cGZBufferSurfaceDesc&, bool) override { return true; } void Unlock() override {}
};

// The window's draw buffer: the font draws into its cIGZBuffer.
struct WinBuf final : IWinBuffer {
    cGZBuffer* gz = nullptr;
    cRZRect bounds{0, 0, 200, 100};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    const cRZRect* GetBounds() const override { return &bounds; }
    std::uint32_t MakeNativeColor(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    cIGZBuffer* AsGZBuffer() override { return gz; }
};

struct Probe final : cGZWin {
    using cGZWin::cGZWin;
    std::vector<cGZMessage> got;
    bool DoMessage(cGZMessage& m) override { got.push_back(m); return true; }
};

struct Row final : cGZWinFolderRow {
    using cGZWinFolderRow::cGZWinFolderRow;
    void Attach(cIGZDrawContext* c, IWinBuffer* b) { drawContext_ = c; drawBuffer_ = b; }
    cRZRect DrawArea() const { return drawArea_; }
};

struct TabRec { int l, t, r, b; std::uint32_t id; std::string name; };
std::vector<TabRec> TabsOf(const cGZWinFolderRow& row) {
    std::vector<TabRec> out;
    for (const tFolderTab& t : row.Tabs()) out.push_back({t.area.left, t.area.top, t.area.right, t.area.bottom, t.id, t.name.Data()});
    return out;
}

}  // namespace

int main() {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    SetWinGraphics(&graphics);
    graphfakes::Font font;   // 6 pixels per character, 10 pixels high

    auto* outline = new cGZBuffer();   // 80 x 24: half width 40, caps max(40 / 8, 4) = 5
    outline->AddRef();
    outline->Init(80, 24, {kBufferColorARGB8888, 32});
    auto* textBuffer = new cGZBuffer();
    textBuffer->AddRef();
    textBuffer->Init(200, 100, {kBufferColorARGB8888, 32});

    auto* parent = new Probe(&mgr);
    parent->AddRef();
    parent->SetArea(0, 0, 200, 100);

    auto* row = new Row(&mgr);
    row->AddRef();
    void* qi = nullptr;
    CHECK(row->QueryInterface(kGZIID_cIGZWinFolderRow, &qi) && qi == static_cast<cIGZWinFolderRow*>(row));
    if (qi) static_cast<cIGZWinFolderRow*>(qi)->Release();
    CHECK(row->GetSelection() == -1 && row->TabCount() == 0);

    // references: SetFont / SetOutlineBmp take one, Shutdown gives them back
    const int fontRefs = font.refs;
    row->SetFont(&font);
    row->SetOutlineBmp(outline);
    CHECK(font.refs == fontRefs + 1 && row->HalfWidth() == 40 && row->BitmapHeight() == 24);
    row->SetFont(&font);   // same font: no second reference
    CHECK(font.refs == fontRefs + 1);

    row->SetArea(10, 20, 110, 44);   // 100 wide
    CHECK(parent->ChildAdd(row));     // Init keeps the font and bitmap it already has
    CHECK(row->Font() == &font && row->OutlineBmp() == outline);

    // measuring: text + 2 * cap + 10, appended at the current width; a tab past the row is refused
    cRZString ab("ab"), abcd("abcd"), a("a"), wide("abcdefghijklmnopqrst");
    CHECK(row->CanThisTabBeAdded(&wide));   // quirk: an empty row accepts a first tab wider than itself
    CHECK(!row->AddTab(&wide, 9));          // but AddTab refuses it
    CHECK(row->AddTab(&ab, 7));
    CHECK(row->AddTab(&abcd, 8));
    CHECK(!row->CanThisTabBeAdded(&a));     // 76 + 10 + 10 + 6 = 102 > 100
    CHECK(!row->AddTab(&a, 9));
    auto tabs = TabsOf(*row);
    CHECK(row->TabCount() == 2 && tabs.size() == 2);
    CHECK(tabs[0].l == 0 && tabs[0].t == 0 && tabs[0].r == 32 && tabs[0].b == 24 && tabs[0].id == 7 && tabs[0].name == "ab");
    CHECK(tabs[1].l == 32 && tabs[1].r == 76 && tabs[1].id == 8);

    // stretch: the 24 spare pixels go out one at a time, round robin
    row->StretchToFit();
    tabs = TabsOf(*row);
    CHECK(tabs[0].l == 0 && tabs[0].r == 44 && tabs[1].l == 44 && tabs[1].r == 100);

    // selection
    std::uint32_t id = 0;
    cRZString name;
    CHECK(!row->GetSelectedTabName(name) && !row->GetSelectedTabID(id));
    CHECK(row->SelectTab(abcd) && row->GetSelection() == 1);
    CHECK(row->GetSelectedTabName(name) && std::string(name.Data()) == "abcd");
    CHECK(row->SelectTab(7u) && row->GetSelection() == 0 && row->GetSelectedTabID(id) && id == 7);
    CHECK(!row->SelectTab(a) && !row->SelectTab(99u) && row->GetSelection() == 0);
    row->ClearSelection();
    CHECK(row->GetSelection() == -1);

    // paint: nothing without a draw context; then unselected tabs in order, the selected one last
    Ctx ctx;
    WinBuf winBuf;
    winBuf.gz = textBuffer;
    CHECK(row->GZPaint() && ctx.log.empty() && font.calls.empty());
    row->Attach(&ctx, &winBuf);
    CHECK(row->SelectTab(8u));
    row->GZPaint();
    const cRZRect da = row->DrawArea();
    const int x = da.left, y = da.top;
    auto at = [&](int l, int t, int r, int b) { return R({l, t, r, b}); };
    CHECK(ctx.log.size() == 6);
    if (ctx.log.size() == 6) {
        // tab 0, unselected (left half of the bitmap)
        CHECK(ctx.log[0] == "blt 0,0,5,24>" + at(x + 0, y, x + 5, y + 24));
        CHECK(ctx.log[1] == "tile 5,0,10,24>" + at(x + 5, y, x + 39, y + 24) + " 0,0");
        CHECK(ctx.log[2] == "blt 35,0,40,24>" + at(x + 39, y, x + 44, y + 24));
        // tab 1, selected and last: right half of the bitmap, 2 pixels over its left neighbor, none past the row end
        CHECK(ctx.log[3] == "blt 40,0,45,24>" + at(x + 42, y, x + 47, y + 24));
        CHECK(ctx.log[4] == "tile 45,0,50,24>" + at(x + 47, y, x + 95, y + 24) + " 0,0");
        CHECK(ctx.log[5] == "blt 75,0,80,24>" + at(x + 95, y, x + 100, y + 24));
    }
    CHECK(font.calls.size() == 2);
    if (font.calls.size() == 2) {
        CHECK(font.calls[0].text == "ab" && font.calls[0].align == 1 && font.calls[0].color == kFontDefaultColor);
        CHECK(R(font.calls[0].rect) == at(x + 0, y + 7, x + 44, y + 24));   // (24 - 10) / 2 = 7
        CHECK(font.calls[1].text == "abcd" && R(font.calls[1].rect) == at(x + 44, y + 7, x + 100, y + 24));
    }
    // a selected first tab spreads to the right only
    ctx.log.clear();
    row->SelectTab(7u);
    row->GZPaint();
    CHECK(ctx.log.size() == 6 && ctx.log[3] == "blt 40,0,45,24>" + at(x + 0, y, x + 5, y + 24) &&
          ctx.log[5] == "blt 75,0,80,24>" + at(x + 41, y, x + 46, y + 24));

    // click: select the tab under x (edges inclusive, first match), forward in the parent's coordinates
    parent->got.clear();
    CHECK(row->GZOnMouseDownL(50, 5, 0x10));
    CHECK(row->GetSelection() == 1);
    CHECK(parent->got.size() == 1 && parent->got[0].type == kGZMsgMouseDownL);
    if (parent->got.size() == 1) {
        CHECK(static_cast<int>(parent->got[0].param1) == 60 && static_cast<int>(parent->got[0].param2) == 25);
        CHECK(parent->got[0].param3 == 0x10);
    }
    row->GZOnMouseDownL(44, 5, 0);
    CHECK(row->GetSelection() == 0);
    row->GZOnMouseDownL(150, 5, 0);   // outside every tab: the selection stays, the click is still forwarded
    CHECK(row->GetSelection() == 0 && parent->got.size() == 3);

    // Shutdown hands back the font and the bitmap
    row->Shutdown();
    CHECK(font.refs == fontRefs && row->Font() == nullptr && row->OutlineBmp() == nullptr);
    CHECK(!row->CanThisTabBeAdded(&a) && !row->AddTab(&a, 1));   // guarded without a font
    parent->ChildRemove(row);
    row->Release();

    // cGZWinFolders through its default host: real rows, tabs dealt out, selection by name
    {
        auto* folders = new cGZWinFolders(&mgr);
        folders->AddRef();
        folders->SetArea(0, 0, 100, 40);
        folders->SetFont(&font);
        folders->SetOutlineBmp(outline);
        folders->AddFolder(ab, 1);     // 32
        folders->AddFolder(abcd, 2);   // 44
        folders->AddFolder(a, 3);      // 26 -> 102 in all: two rows
        CHECK(parent->ChildAdd(folders));
        CHECK(folders->RowCount() == 2 && !folders->NeedsRecalc());
        if (folders->RowCount() == 2) {
            auto* first = dynamic_cast<cGZWinFolderRow*>(folders->Rows().front().row);
            auto* second = dynamic_cast<cGZWinFolderRow*>(folders->Rows().back().row);
            CHECK(first && second);
            if (first && second) {
                auto t1 = TabsOf(*first), t2 = TabsOf(*second);
                CHECK(t1.size() == 2 && t2.size() == 1);   // "ab" and "a" share the first row, "abcd" the second
                if (t1.size() == 2 && t2.size() == 1) {
                    CHECK(t1[0].name == "ab" && t1[1].name == "a" && t2[0].name == "abcd");
                    CHECK(t1[0].l == 0 && t1[1].r == 100 && t2[0].l == 0 && t2[0].r == 100);   // stretched
                }
                CHECK(first->Font() == &font && first->OutlineBmp() == outline);
            }
        }
        CHECK(folders->SetSelectedFolder(abcd));
        cRZString picked;
        std::uint32_t pickedId = 0;
        CHECK(folders->GetSelectedFolderName(picked) && std::string(picked.Data()) == "abcd");
        CHECK(folders->GetSelectedFolderID(pickedId) && pickedId == 2);
        // The real window manager shuts each child down before deleting it (cGZWin::ChildDeleteAbsolute); FakeMgr only
        // removes it, so the rows give back their font and bitmap here.
        for (const WinFolderRow& r : folders->Rows()) r.row->Shutdown();
        parent->ChildRemove(folders);
        folders->Shutdown();
        folders->Release();
    }

    parent->Release();
    textBuffer->Release();
    outline->Release();
    std::printf("gz_winfolderrow: %s\n", failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}
