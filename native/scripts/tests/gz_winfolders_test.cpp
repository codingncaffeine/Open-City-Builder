// cGZWinFolders test: the folder list, the tab measuring and the row building, the paint order, the click / selection
// protocol, the font / outline bitmap references, the relayout flag and the guarded paths.
//
// The tab rows (cGZWinFolderRow) are not recovered, so the test installs an IWinFoldersHost whose rows are a minimal
// cGZWin + cIGZWinFolderRow model: a row accepts tabs while its capacity lasts, counts the calls the control makes and
// remembers the tab it selected. That is enough to pin the layout the recovered RecalcTabs / GZPaint / handlers build.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fake_graph.h"
#include "fake_winmgr.h"
#include "gz/Buffer.h"
#include "gz/WinFolders.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace gz;

namespace {

struct TestBufferHost final : IBufferHost {
    bool HasGraphicsSystem() override { return true; }
    cGZBufferColorType DisplayColorType() override { return cGZBufferColorType{kBufferColorXRGB8888, 0x20}; }
    bool QueryTextureBitCount(std::uint32_t&) override { return false; }
};

// The outline bitmap with its reference count visible to the test.
struct TestBuffer final : cGZBuffer {
    std::uint32_t Refs() const { return refCount_; }
};

struct FakeRow;

// The control's seams: the default font / outline bitmap and the row factory.
struct Host final : IWinFoldersHost {
    FakeMgr* mgr = nullptr;
    cIGZFont* font = nullptr;
    cIGZBuffer* outline = nullptr;
    bool rowsAvailable = true;
    int requested = 0, live = 0;
    std::vector<FakeRow*> rows;
    cIGZFont* DefaultFont() override { return font; }
    cIGZBuffer* DefaultOutlineBitmap() override { return outline; }
    WinFolderRow CreateFolderRow(cIGZWinMgr* mgr, cIGZFont* font, cIGZBuffer* outline) override;
};

// One row of tabs: a cGZWin child plus the cIGZWinFolderRow interface (the real cGZWinFolderRow has the same shape).
struct FakeRow final : cGZWin, cIGZWinFolderRow {
    struct Tab { std::string name; std::uint32_t id; };

    explicit FakeRow(Host* h, cIGZWinMgr* mgr) : cGZWin(mgr), host(h) { AddRef(); }   // one reference, as the seam says
    ~FakeRow() override { --host->live; }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0x4a4f4c52) {   // stand-in iid for cIGZWinFolderRow in this test
            *out = static_cast<cIGZWinFolderRow*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }

    // A tab costs the control's measure: 6 pixels per character, 2 * gutter (4) and the 10 pixel frame.
    static int TabWidth(const cIGZString* name) {
        return (name ? static_cast<int>(name->Strlen()) : 0) * 6 + 2 * 4 + 10;
    }
    bool CanThisTabBeAdded(const cIGZString* name) override { return used + TabWidth(name) <= capacity; }
    bool AddTab(const cIGZString* name, std::uint32_t id) override {
        if (!CanThisTabBeAdded(name)) return false;
        used += TabWidth(name);
        tabs.push_back({name ? name->Data() : "", id});
        return true;
    }
    void StretchToFit() override { ++stretches; }
    void SetFont(cIGZFont* f) override { font = f; }
    void ClearSelection() override { selected = -1; ++clears; }
    int GetSelection() override { return selected; }
    bool GetSelectedTabName(cIGZString& out) override {
        if (selected < 0 || selected >= static_cast<int>(tabs.size())) return false;
        out.AssignFromCString(tabs[static_cast<std::size_t>(selected)].name.c_str());
        return true;
    }
    bool GetSelectedTabID(std::uint32_t& out) override {
        if (selected < 0 || selected >= static_cast<int>(tabs.size())) return false;
        out = tabs[static_cast<std::size_t>(selected)].id;
        return true;
    }
    bool SelectTab(const cIGZString& name) override {
        ++selectNameCalls;
        for (std::size_t i = 0; i < tabs.size(); ++i)
            if (tabs[i].name == name.Data()) { selected = static_cast<int>(i); return true; }
        return false;
    }
    bool SelectTab(std::uint32_t id) override {
        ++selectIdCalls;
        for (std::size_t i = 0; i < tabs.size(); ++i)
            if (tabs[i].id == id) { selected = static_cast<int>(i); return true; }
        return false;
    }
    bool Init() override { ++inits; return cGZWin::Init(); }
    bool Shutdown() override { ++shutdowns; return cGZWin::Shutdown(); }
    bool GZPaint() override { ++paints; paintY = GetT(); return true; }
    bool GZOnMouseDownL(int, int, std::uint32_t) override { return false; }
    void SetArea(int l, int t, int r, int b) override {
        cGZWin::SetArea(l, t, r, b);
        capacity = GetW();
    }

    Host* host;
    cIGZFont* font = nullptr;
    std::vector<Tab> tabs;
    int capacity = 0, used = 0, selected = -1;
    int clears = 0, stretches = 0, paints = 0, shutdowns = 0, inits = 0;
    int selectNameCalls = 0, selectIdCalls = 0;
    int paintY = -1;
};

WinFolderRow Host::CreateFolderRow(cIGZWinMgr* winMgr, cIGZFont* font, cIGZBuffer* outlineBmp) {
    CHECK(winMgr == mgr);   // the control hands over its own window manager
    (void)outlineBmp;   // the real factory hands it to cGZWinFolderRow::SetOutlineBmp
    ++requested;
    if (!rowsAvailable) return {};
    auto* row = new FakeRow(this, mgr);
    row->SetFont(font);   // the real factory's SetFont (vslot 0x30)
    rows.push_back(row);
    ++live;
    return {row, row};
}

// A window that only records the messages it receives (the control's notification target).
struct Target final : cGZWin {
    explicit Target(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    std::vector<cGZMessage> got;
    bool DoMessage(cGZMessage& message) override {
        got.push_back(message);
        return true;
    }
};

std::string Text(const cRZString& s) { return std::string(s.Data(), s.Strlen()); }

}  // namespace

int main() {
    TestBufferHost bufferHost;
    SetBufferHost(&bufferHost);

    FakeMgr mgr;
    graphfakes::Font font;
    auto* outline = new TestBuffer();
    outline->AddRef();
    CHECK(outline->Init(16, 12, cGZBufferColorType{kBufferColorXRGB8888, 0x20}));

    Host host;
    host.mgr = &mgr;
    SetWinFoldersHost(&host);

    auto* folders = new cGZWinFolders(&mgr);
    folders->AddRef();   // the original cGZWin starts with a zero count; the creator holds the first reference

    // ---- construction (0x100512b82) ----
    CHECK(folders->GetL() == 0x10 && folders->GetT() == 0x10);
    CHECK(folders->GetR() == 0x20 && folders->GetB() == 0x20);
    CHECK(folders->NeedsRecalc());
    CHECK(folders->TabCount() == 0 && folders->RowCount() == 0 && folders->RowCountField() == 0);
    CHECK(folders->Font() == nullptr && folders->OutlineBmp() == nullptr);
    CHECK(folders->AsIGZWin() == static_cast<cGZWin*>(folders));

    // ---- QueryInterface (0x100512cba) ----
    void* out = nullptr;
    CHECK(folders->QueryInterface(kGZIID_cIGZWinFolders, &out));
    CHECK(out == static_cast<cIGZWinFolders*>(folders));
    CHECK(static_cast<cIGZWinFolders*>(out)->Release() == 1);   // QueryInterface took the second reference
    CHECK(folders->QueryInterface(kGZIID_cIGZWin, &out));
    CHECK(out == static_cast<cIGZWin*>(folders));
    CHECK(static_cast<cIGZWin*>(out)->Release() == 1);
    CHECK(!folders->QueryInterface(0xdeadbeef, &out));

    // ---- AddFolder / GetCount / GetFolderByIndex (0x1005130de, 0x1005138cc, 0x10051388c) ----
    cRZString alpha("Alpha"), beta("Beta"), gamma("Gamma");
    CHECK(folders->AddFolder(alpha, 11));
    CHECK(folders->AddFolder(beta, 22));
    CHECK(folders->AddFolder(gamma, 33));
    std::uint32_t count = 0;
    folders->GetCount(count);
    CHECK(count == 3 && folders->TabCount() == 3);
    CHECK(folders->NeedsRecalc());
    cRZString name;
    CHECK(folders->GetFolderByIndex(0, name) && Text(name) == "Alpha");
    CHECK(folders->GetFolderByIndex(1, name) && Text(name) == "Beta");
    CHECK(folders->GetFolderByIndex(2, name) && Text(name) == "Gamma");
    name.AssignFromCString("keep");
    CHECK(!folders->GetFolderByIndex(3, name) && Text(name) == "keep");   // not found: the string is untouched
    CHECK(!folders->GetFolderByIndex(0xffffffffu, name));                 // the index + 1 wrap finds nothing

    // ---- Init (0x100512d02): the default font, the outline bitmap and the first layout ----
    folders->SetArea(0, 0, 100, 20);
    CHECK(folders->GetW() == 100);
    host.font = &font;
    host.outline = outline;
    CHECK(folders->Init());
    CHECK(folders->Font() == &font);
    CHECK(font.refs == 2);                         // the test's reference plus the one SetFont took
    CHECK(folders->OutlineBmp() == outline);
    CHECK(outline->Refs() == 2);                   // the test's reference plus the one Init took
    CHECK(folders->BitmapWidth() == 8);            // 16 / 2
    CHECK(folders->BitmapHeight() == 12);
    CHECK(folders->RowCountField() == 2);          // three 42 pixel tabs in a 100 pixel control -> ceil(126 / 100)
    CHECK(folders->RowCount() == 2 && host.live == 2);
    CHECK(!folders->NeedsRecalc());
    CHECK(folders->GetH() == (12 - 4) * 2 + 4);    // SetH((bitmapHeight - 4) * rows + 4)

    // Both rows are children of the control and got the control's width and the bitmap height.
    for (const WinFolderRow& row : folders->Rows()) {
        CHECK(row.window->GetParentWin() == static_cast<cIGZWin*>(folders));
        CHECK(row.row->GetSelection() == -1);
        auto* fake = static_cast<FakeRow*>(row.row);
        CHECK(fake->GetW() == 100 && fake->GetH() == 12);
        CHECK(fake->inits == 1 && fake->stretches == 1);
        CHECK((fake->GetFlag(kWinFlag800)));
        CHECK(fake->font == &font);
    }

    // The tabs are offered to the rows with one cursor that carries over from tab to tab and wraps when it runs past
    // the last row: Alpha goes to the first row, Beta to the next, Gamma wraps back to the first (42 + 42 = 84 of the
    // 100 pixels). The first row of the list is drawn at the bottom (see GZPaint).
    const WinFolderRow front = folders->Rows().front();   // +0x170 list front
    const WinFolderRow back = folders->Rows().back();
    auto* frontRow = static_cast<FakeRow*>(front.row);
    auto* backRow = static_cast<FakeRow*>(back.row);
    CHECK(frontRow->tabs.size() == 2);
    CHECK(frontRow->tabs[0].name == "Alpha" && frontRow->tabs[0].id == 11);
    CHECK(frontRow->tabs[1].name == "Gamma" && frontRow->tabs[1].id == 33);
    CHECK(backRow->tabs.size() == 1);
    CHECK(backRow->tabs[0].name == "Beta" && backRow->tabs[0].id == 22);

    // ---- GZPaint (0x100513046): the back of the row list is at the top ----
    CHECK(folders->GZPaint());
    CHECK(backRow->paints == 1 && backRow->paintY == 0);
    CHECK(frontRow->paints == 1 && frontRow->paintY == 12 - 4);

    // ---- GZOnMouseDownL (0x100512e40) ----
    auto* target = new Target(&mgr);
    target->AddRef();
    folders->SetNotificationTarget(target);

    // The top row: y / 8 = 0, so the hit index (counted from the front of the list) is 2 - 1 - 0 = 1, the top row.
    int frontClears = frontRow->clears, backClears = backRow->clears;
    CHECK(!folders->GZOnMouseDownL(5, 0, 0));
    CHECK(target->got.size() == 1);
    CHECK(target->got[0].type == kGZMsgCommand);
    CHECK(target->got[0].param1 == kFoldersCmdFolderSelected);
    CHECK(target->got[0].param2 == folders->GetID());
    CHECK(target->got[0].param3 == 0);
    CHECK(frontRow->clears == frontClears + 1);   // the row that was not hit is deselected
    CHECK(backRow->clears == backClears);         // the hit row keeps its selection
    // ... and the hit row is linked before the first node, so GZPaint draws it at the bottom next time.
    CHECK(folders->Rows().front().row == back.row);
    CHECK(folders->Rows().back().row == front.row);
    CHECK(folders->GZPaint());
    CHECK(backRow->paintY == 12 - 4 && frontRow->paintY == 0);

    // The bottom row: y / 8 = 1 -> 2 - 1 - 1 = 0, the front of the list, which does not move again.
    frontClears = frontRow->clears;
    backClears = backRow->clears;
    CHECK(!folders->GZOnMouseDownL(5, 8, 0));
    CHECK(target->got.size() == 2);
    CHECK(backRow->clears == backClears);         // the hit row (now the front one) is not cleared
    CHECK(frontRow->clears == frontClears + 1);   // the other row is
    CHECK(folders->Rows().front().row == back.row);

    // A click below the rows does nothing.
    frontClears = frontRow->clears;
    backClears = backRow->clears;
    CHECK(!folders->GZOnMouseDownL(5, 8 * 2 + 4, 0));
    CHECK(target->got.size() == 2);
    CHECK(backRow->clears == backClears && frontRow->clears == frontClears);

    // ---- selection (0x1005131ba / 0x100513270 / 0x10051316a / 0x100513192) ----
    // Rows are [Beta | Alpha, Gamma]. Alpha is not in the first row, which is deselected, and found in the second,
    // which then moves to the front of the list.
    int frontSelects = frontRow->selectNameCalls, backSelects = backRow->selectNameCalls;
    frontClears = frontRow->clears;
    backClears = backRow->clears;
    CHECK(folders->SetSelectedFolder(alpha));
    CHECK(backRow->selectNameCalls == backSelects + 1);
    CHECK(frontRow->selectNameCalls == frontSelects + 1);
    CHECK(backRow->clears == backClears + 1);     // the row that did not match was cleared
    CHECK(frontRow->clears == frontClears);
    CHECK(folders->Rows().front().row == front.row);
    std::uint32_t selectedId = 0;
    CHECK(folders->GetSelectedFolderID(selectedId) && selectedId == 11);
    CHECK(folders->GetSelectedFolderName(name) && Text(name) == "Alpha");

    // Selecting by id picks the row that holds it (Gamma is in the front row now, so nothing moves).
    CHECK(folders->SetSelectedFolder(33u));
    CHECK(folders->GetSelectedFolderID(selectedId) && selectedId == 33);
    CHECK(folders->GetSelectedFolderName(name) && Text(name) == "Gamma");

    // No folder matches: false and every row is deselected.
    cRZString missing("Missing");
    const int clearsBefore = backRow->clears + frontRow->clears;
    CHECK(!folders->SetSelectedFolder(missing));
    CHECK(backRow->clears + frontRow->clears == clearsBefore + 2);
    CHECK(!folders->GetSelectedFolderName(name));   // the front row has no selected tab after the sweep

    // ---- SetFont (0x1005138da) and SetOutlineBmp (0x10051393c) ----
    graphfakes::Font font2;
    folders->SetFont(&font2);
    CHECK(folders->Font() == &font2);
    CHECK(font.refs == 1);        // the control dropped the previous font
    CHECK(font2.refs == 2);       // and took a reference on the new one
    CHECK(folders->NeedsRecalc());
    folders->SetFont(&font2);     // the same font again: nothing happens
    CHECK(font2.refs == 2);

    folders->SetOutlineBmp(nullptr);
    CHECK(folders->OutlineBmp() == nullptr);
    CHECK(outline->Refs() == 1);   // only the test's reference is left
    CHECK(folders->BitmapWidth() == 0 && folders->BitmapHeight() == 0);
    CHECK(folders->NeedsRecalc());

    // Without a bitmap the click handler is guarded (the original divides by -4 and walks the row list).
    CHECK(!folders->GZOnMouseDownL(0, 0, 0));
    CHECK(target->got.size() == 2);

    // ---- the relayout rebuilds the rows (0x100512e40 guard, 0x100513326) ----
    folders->SetOutlineBmp(outline);   // back to 16 x 12; the rows are rebuilt by the next paint
    CHECK(folders->BitmapWidth() == 8 && folders->BitmapHeight() == 12);
    CHECK(outline->Refs() == 2);
    const int requestsBefore = host.requested;
    CHECK(folders->GZPaint());                       // recalcs first
    CHECK(!folders->NeedsRecalc());
    CHECK(host.requested > requestsBefore);          // the old rows were dropped and rebuilt
    CHECK(folders->RowCount() == 2);
    CHECK(folders->RowCountField() == 2);

    // A tab wider than the control cannot be laid out: RecalcTabs returns false and keeps the flag set.
    cRZString wide("WideFolderNameThatDoesNotFitAtAll");
    folders->AddFolder(wide, 44);
    CHECK(!folders->RecalcTabs());
    CHECK(folders->NeedsRecalc());
    CHECK(folders->RowCount() == 0 && folders->RowCountField() == 0);   // the old rows were already dropped
    CHECK(host.live == 0);

    // ---- DeleteAllFolders (0x100513868) ----
    folders->DeleteAllFolders();
    std::uint32_t countAfter = 99;
    folders->GetCount(countAfter);
    CHECK(countAfter == 0 && folders->TabCount() == 0);
    CHECK(folders->NeedsRecalc());
    CHECK(!folders->GetFolderByIndex(0, name));
    CHECK(!folders->GetSelectedFolderName(name));
    CHECK(!folders->GetSelectedFolderID(selectedId));

    // An empty folder list still lays out one (empty) row.
    CHECK(folders->RecalcTabs());
    CHECK(folders->RowCount() == 1 && host.live == 1);
    CHECK(folders->GetH() == 12);
    CHECK(!folders->NeedsRecalc());

    // ---- the row factory can fail (cGZWinFolderRow is not recovered) ----
    host.rowsAvailable = false;
    folders->AddFolder(alpha, 11);
    CHECK(!folders->RecalcTabs());
    CHECK(folders->NeedsRecalc());
    CHECK(folders->RowCount() == 0 && host.live == 0);
    host.rowsAvailable = true;

    // ---- Shutdown (0x100512ddc) ----
    CHECK(folders->Shutdown());
    CHECK(folders->Font() == nullptr && folders->OutlineBmp() == nullptr);
    CHECK(font2.refs == 1);       // the control's font reference is gone
    CHECK(outline->Refs() == 1);   // and so is its bitmap reference
    CHECK(folders->RowCount() == 0 && folders->RowCountField() == 0);
    CHECK(host.live == 0);        // the child windows were deleted
    CHECK(!folders->RecalcTabs());   // no longer initialised: the relayout refuses

    // ---- the destructor (0x100513b46) ----
    CHECK(folders->Release() == 0);
    CHECK(host.live == 0);
    CHECK(target->Release() == 0);

    // A second control: the folder name is copied into the list (changing the source afterwards does not matter),
    // and destroying a control that never ran Init but has tabs is safe.
    {
        auto* other = new cGZWinFolders(&mgr);
        other->AddRef();
        cRZString copyName("Copy");
        CHECK(other->AddFolder(copyName, 7));
        copyName.AssignFromCString("Changed");
        cRZString got;
        CHECK(other->GetFolderByIndex(0, got) && Text(got) == "Copy");
        std::uint32_t otherCount = 0;
        other->GetCount(otherCount);
        CHECK(otherCount == 1);
        CHECK(other->Release() == 0);
    }

    SetWinFoldersHost(nullptr);
    SetBufferHost(nullptr);
    outline->Release();

    if (failures == 0) std::printf("gz_winfolders: all checks passed\n");
    else std::printf("gz_winfolders: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
