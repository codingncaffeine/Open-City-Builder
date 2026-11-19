// Test for cSCResourceCatalogView (src/sc4/ResourceCatalogView.cpp).
//
// The window is driven over the fake host the class reaches its unrecovered neighbours through (labels, buttons,
// the type combo, the instance grid, the type views, the persistence manager and the message server). The ini file
// registerTypes reads is written into a temporary directory and the "Add New Resource" rename runs against a real
// file, so the ini parsing and the cRZPath/cRZFile calls are exercised for real.
//
// Not covered: the image path of createButton (cSLBuffer with a loaded bitmap) needs a cIGZBuffer double, so only
// the no-image layout (26 x 16) is asserted.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fake_winmgr2.h"
#include "sc4/ResourceCatalogView.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { \
    std::printf("FAIL line %d: %s == %s\n", __LINE__, #a, #b); ++failures; } } while (0)

namespace {

// A plain window used as the visible object behind a seam control. It keeps one reference of its own (as the other
// window-control tests do) so a ChildAdd/Release cycle never deletes it under the test.
struct FakeWindow final : cGZWin {
    using cGZWin::cGZWin;
    cGZMessage last;
    int messages = 0;
    bool DoMessage(cGZMessage& message) override { last = message; ++messages; return true; }
};

FakeWindow* MakeChild(FakeMgr2& mgr, int w, int h) {
    auto* win = new FakeWindow(&mgr);
    win->AddRef();
    win->SetSize(w, h);
    return win;
}

struct FakeLabel final : IResourceCatalogLabel {
    FakeWindow* win;
    std::vector<std::pair<int, bool>> textFlags;
    std::uint32_t color = 0xffffffffu;
    gz::IWinFont* font = nullptr;
    int fits = 0;
    int refs = 1;
    explicit FakeLabel(FakeMgr2& mgr, int w = 40, int h = 10) : win(MakeChild(mgr, w, h)) {}
    cIGZWin* AsIGZWin() override { return win; }
    void FitWindowToText(bool, bool) override { ++fits; }
    void SetWinTextFlag(int flag, bool on) override { textFlags.push_back({flag, on}); }
    void SetTextColor(std::uint32_t native) override { color = native; }
    void SetFont(gz::IWinFont* value) override { font = value; }
    void Release() override { --refs; }
};

struct FakeButton final : IResourceCatalogButton {
    FakeWindow* win;
    std::vector<std::pair<int, bool>> flags;
    std::string tip;
    std::string caption;
    std::uint32_t buttonId = 0;
    cIGZBuffer* image = nullptr;
    bool haveArea = false;
    int fits = 0;
    int refs = 1;
    explicit FakeButton(FakeMgr2& mgr, int w = 30, int h = 12) : win(MakeChild(mgr, w, h)) {}
    cIGZWin* AsIGZWin() override { return win; }
    void SetBtnFlag(int flag, bool on) override { flags.push_back({flag, on}); }
    void FitSizeToContent() override { ++fits; }
    void SetTipText(const cIGZString& text) override { tip = text.ToChar(); }
    void Release() override { --refs; }
};

struct FakeBackdrop final : IResourceCatalogBackdrop {
    FakeWindow* win;
    int outlineFlags = -1, sizeMode = -1;
    std::uint32_t outlineColor = 0;
    int refs = 1;
    explicit FakeBackdrop(FakeMgr2& mgr) : win(MakeChild(mgr, 0, 0)) {}
    cIGZWin* AsIGZWin() override { return win; }
    void SetOutlineFlags(int flags) override { outlineFlags = flags; }
    void SetOutlineColor(std::uint32_t native) override { outlineColor = native; }
    void SetSizeMode(int mode) override { sizeMode = mode; }
    void Release() override { --refs; }
};

struct FakeCombo final : IResourceCatalogCombo {
    FakeWindow* win;
    std::vector<std::string> items;
    std::vector<std::pair<int, std::uint32_t>> fontColors;
    int selection = 0;
    int highlight = -1;
    bool editable = true;
    std::uint32_t highlightColor = 0;
    bool selectedNotify = true;
    int refs = 1;
    cRZRect area{};
    explicit FakeCombo(FakeMgr2& mgr) : win(MakeChild(mgr, 30, 18)) {}
    cIGZWin* AsIGZWin() override { return win; }
    void SetFontColor(int index, std::uint32_t native) override { fontColors.push_back({index, native}); }
    void SetHighlightColor(std::uint32_t native) override { highlightColor = native; }
    void SetEditable(bool value) override { editable = value; }
    void InsertString(const cIGZString& text, int index) override {
        if (index == -2) items.emplace_back(text.ToChar());
        else items.insert(items.begin() + index, text.ToChar());
    }
    void SetSelection(int index, bool notify) override { selection = index; selectedNotify = notify; }
    int GetSelection() override { return selection; }
    void Release() override { --refs; }
};

struct FakeView final : IResourceTypeView {
    FakeWindow* win;
    cGZPersistResourceKey key{};
    bool haveKey = false;
    int readOnlyCalls = 0, prompts = 0, shows = 0, hides = 0, refs = 1;
    bool readOnly = false;
    cRZRect area{};
    explicit FakeView(FakeMgr2& mgr) : win(MakeChild(mgr, 100, 50)) {}
    cIGZWin* AsIGZWin() override { return win; }
    void SetKeyToView(const cGZPersistResourceKey& value) override { key = value; haveKey = true; }
    void PromptToSave() override { ++prompts; }
    void SetReadOnly(bool value) override { readOnly = value; ++readOnlyCalls; }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
};

struct FakeGrid final : IResourceCatalogGrid {
    FakeWindow* win;
    struct Cell { std::string text; void* data = nullptr; };
    std::map<int, Cell> cells;                       // row -> cell (column 0 only)
    int clearAll = 0, sorts = 0, rowHeights = 0;
    int sortedColumn = -1, sortedStart = -1;
    bool sortedAscending = false;
    int lastRowHeight = 0;
    int selectionColumn = -1, selectionRow = -1;
    bool haveSelection = false;
    int defaultRowHeight = 0, refs = 1;
    std::vector<std::pair<std::uint32_t, int>> fontColors;
    std::vector<std::pair<int, int>> columnWidths;
    std::vector<int> clearedFlags;
    std::vector<std::pair<int, int>> listingFlags;
    cRZRect area{};
    explicit FakeGrid(FakeMgr2& mgr) : win(MakeChild(mgr, 100, 100)) {}
    cIGZWin* AsIGZWin() override { return win; }
    void SetArea(int l, int t, int r, int b) override { area = {l, t, r, b}; win->SetArea(l, t, r, b); }
    void ClearFlag(int flag) override { clearedFlags.push_back(flag); }
    void SetDefaultFontColor(std::uint32_t native, int index) override { fontColors.push_back({native, index}); }
    void ActLikeAListbox(int a, int b, bool c) override { listingFlags.push_back({a, b}); (void)c; }
    bool GetSelection(int& column, int& row) override {
        column = selectionColumn; row = selectionRow; return haveSelection;
    }
    bool SetSelection(int column, int row) override {
        selectionColumn = column; selectionRow = row; haveSelection = true; return true;
    }
    bool GetCellUsedRange(cRZRect& range) override { range = area; return true; }
    void SetColumnWidth(int column, int endColumn, int width) override { columnWidths.push_back({column, width}); (void)endColumn; }
    void SetDefaultRowHeight(int height) override { defaultRowHeight = height; }
    void SetRowHeight(int row, int endRow, int height) override { ++rowHeights; lastRowHeight = height; (void)row; (void)endRow; }
    void ClearAllCells() override { ++clearAll; cells.clear(); }
    void SetCellTextValue(int column, int row, const char* text, std::uint32_t length, int flags, bool) override {
        (void)column; (void)length; (void)flags;
        cells[row].text = text;
    }
    void SetCellExtraData(int column, int row, void* data) override { (void)column; cells[row].data = data; }
    void* GetCellExtraData(int column, int row) override {
        (void)column;
        auto it = cells.find(row);
        return it == cells.end() ? nullptr : it->second.data;
    }
    void SortColumn(int column, int startRow, bool ascending, CellCompare compare) override {
        ++sorts; sortedColumn = column; sortedStart = startRow; sortedAscending = ascending; (void)compare;
    }
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
};

struct FakeHost final : IResourceCatalogHost {
    FakeMgr2* mgr = nullptr;
    std::string dataDir;
    std::vector<FakeLabel*> labels;
    std::vector<FakeButton*> buttons;
    std::vector<FakeCombo*> combos;
    std::vector<FakeBackdrop*> backdrops;
    std::vector<FakeGrid*> grids;
    std::vector<FakeView*> views;
    std::vector<cGZPersistResourceKey> keys;             // storage the grid points into
    std::map<std::uint32_t, std::string> names;         // instance -> resource name
    int viewCreations = 0, fontAsks = 0;
    std::uint32_t fontStyle = 0;
    gz::IWinFont* font = nullptr;
    int posts = 0;
    cGZMessage lastPost;
    // addResource knobs
    bool dialogAccepted = true;
    cGZPersistResourceKey dialogKey{};
    std::string dialogFileName;
    bool hasKey = false;
    bool createNewResource = true;
    bool saveToSegment = true;
    std::string currentFileName;
    int testForKeyCalls = 0, createCalls = 0, saveCalls = 0, fileNameCalls = 0, dialogs = 0;

    IResourceCatalogLabel* CreateLabel() override {
        auto* label = new FakeLabel(*mgr);
        labels.push_back(label);
        return label;
    }
    IResourceCatalogButton* CreateFixedButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image,
                                              const cRZRect* area) override {
        auto* button = new FakeButton(*mgr);
        button->win->SetID(id);
        button->buttonId = id;
        button->caption = caption.ToChar();
        button->image = image;
        button->haveArea = area != nullptr;
        buttons.push_back(button);
        return button;
    }
    IResourceCatalogCombo* CreateTypeCombo() override {
        auto* combo = new FakeCombo(*mgr);
        combos.push_back(combo);
        return combo;
    }
    IResourceCatalogBackdrop* CreateBackdrop() override {
        auto* backdrop = new FakeBackdrop(*mgr);
        backdrops.push_back(backdrop);
        return backdrop;
    }
    IResourceCatalogGrid* CreateGrid(std::uint32_t flags) override {
        gridFlags = flags;
        auto* grid = new FakeGrid(*mgr);
        grids.push_back(grid);
        return grid;
    }
    gz::IWinFont* SystemFont(std::uint32_t style) override { ++fontAsks; fontStyle = style; return font; }
    IResourceTypeView* CreateTypeView() override {
        ++viewCreations;
        auto* view = new FakeView(*mgr);
        views.push_back(view);
        return view;
    }
    std::string DataDirectory() override { return dataDir; }
    const std::vector<cGZPersistResourceKey>& AvailableResources(std::uint32_t type) override {
        lastTypeAsked = type;
        ++resourceAsks;
        keys.clear();
        for (const cGZPersistResourceKey& key : allKeys)
            if (key.type == type) keys.push_back(key);
        return keys;
    }
    bool ResourceName(const cGZPersistResourceKey& key, cRZString& name) override {
        auto it = names.find(key.instance);
        if (it == names.end()) return false;
        name = cRZString(it->second.c_str());
        return true;
    }
    bool TestForKey(cGZPersistResourceKey& key) override { ++testForKeyCalls; (void)key; return hasKey; }
    bool CreateNewResource(const cGZPersistResourceKey& key) override { ++createCalls; lastNewKey = key; return createNewResource; }
    bool SaveResourceToGroupSegment(const cGZPersistResourceKey& key) override { ++saveCalls; lastSavedKey = key; return saveToSegment; }
    bool ResourceFileName(const cGZPersistResourceKey& key, cRZString& out) override {
        ++fileNameCalls;
        lastFileNameKey = key;
        out = cRZString(currentFileName.c_str());
        return !currentFileName.empty();
    }
    bool RunKeyInputDialog(const cGZPersistResourceKey& seed, cGZPersistResourceKey& key, cRZString& fileName) override {
        ++dialogs;
        dialogSeed = seed;
        if (!dialogAccepted) return false;
        key = dialogKey;
        fileName = cRZString(dialogFileName.c_str());
        return true;
    }
    void PostMessage(cGZMessage& message) override { ++posts; lastPost = message; }
    IResourceCatalogCombo* ComboFromWindow(cIGZWin* window) override {
        for (FakeCombo* combo : combos)
            if (combo->win == window) return combo;
        return nullptr;
    }

    // extra recording fields (kept at the bottom so the interface above stays readable)
    std::vector<cGZPersistResourceKey> allKeys;
    std::vector<cGZPersistResourceKey> seeds;
    std::uint32_t gridFlags = 0;
    std::uint32_t lastTypeAsked = 0;
    int resourceAsks = 0;
    cGZPersistResourceKey lastNewKey{}, lastSavedKey{}, lastFileNameKey{}, dialogSeed{};
};

struct TestView final : cSCResourceCatalogView {
    void SetDraw(IWinBuffer* buffer, const cRZRect& area) { drawBuffer_ = buffer; drawArea_ = area; }
    IWinBuffer* Draw() const { return drawBuffer_; }
};

struct FakeBuffer final : IWinBuffer {
    int locks = 0, unlocks = 0;
    std::uint32_t fillColor = 0xffffffffu;
    cRZRect fillRect{};
    int fills = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    const cRZRect* GetBounds() const override { return nullptr; }
    std::uint32_t MakeNativeColor(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    bool Lock(std::uint32_t flags) override { ++locks; return flags == 0x8001; }
    void Unlock(std::uint32_t) override { ++unlocks; }
    bool Fill(std::uint32_t color, const cRZRect& rect) override {
        ++fills; fillColor = color; fillRect = rect; return true;
    }
};

std::string MakeTempTree() {
    char tmpl[] = "/tmp/sc4rcvXXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) { std::perror("mkdtemp"); std::exit(1); }
    std::string root(dir);
    mkdir((root + "/Sys").c_str(), 0755);
    FILE* f = std::fopen((root + "/Sys/Ingred.ini").c_str(), "w");
    std::fputs("[ExamplerCategories]\n"
               "Residential=0x1000,0x2000\n"
               "Industrial=0x3000\n", f);
    std::fclose(f);
    return root;
}

void SetUpHost(FakeHost& host, FakeMgr2& mgr, const std::string& dataDir) {
    host.mgr = &mgr;
    host.dataDir = dataDir + "/";      // RZ::DDir answers the directory with a trailing separator
    host.allKeys = {
        {0x6534284a, 0x1000, 0x11},   // Residential exemplar
        {0x6534284a, 0x1000, 0x22},   // Residential exemplar, no name
        {0x6534284a, 0x9999, 0x33},   // another group: filtered out by updateKeyList
        {0x05342861, 0x2000, 0x44},   // Residential LTEXT
        {0x6534284a, 0x3000, 0x55},   // Industrial exemplar
    };
    host.names[0x11] = "Alpha";
    host.names[0x44] = "Beta";
}

}  // namespace

int main() {
    FakeMgr2 mgr;
    FakeWinGraphics2 graphics;
    SetWinGraphics(&graphics);
    const std::string dataDir = MakeTempTree();   // no trailing slash; the host appends one
    FakeHost host;
    SetUpHost(host, mgr, dataDir);
    SetResourceCatalogHost(&host);
    std::vector<FakeWindow*> extraWindows;

    // ---- object layout and construction ----------------------------------------------------------------
    CHECK_EQ(sizeof(cSCResourceCatalogView), static_cast<std::size_t>(0x1b8));
    {
        auto* view = new TestView();
        view->SetWindowManager(&mgr);
        CHECK_EQ(view->Standalone(), true);
        CHECK_EQ(view->ReadOnly(), false);
        CHECK_EQ(view->FilterForType(), 0u);
        CHECK_EQ(view->FilterForGroup(), 0u);
        CHECK_EQ(view->AddRef(), 1u);
        void* out = nullptr;
        CHECK(view->QueryInterface(kGZIID_cISCResourceCatalogView, &out));
        CHECK(out == static_cast<cISCResourceCatalogView*>(view));
        CHECK_EQ(view->Release(), 1u);                       // the interface reference taken by QueryInterface
        CHECK(view->QueryInterface(kGZIID_cIGZWin, &out));
        CHECK(out == static_cast<cGZWin*>(view));            // the window lives at +8
        CHECK_EQ(view->Release(), 1u);
        CHECK(!view->QueryInterface(0x12345678u, &out));
        view->SetStandalone(false);
        view->SetReadOnly(true);
        view->SetFilterForType(0x6534284a);
        view->SetFilterForGroup(0x3000);
        CHECK_EQ(view->Standalone(), false);
        CHECK_EQ(view->ReadOnly(), true);
        CHECK_EQ(view->FilterForType(), 0x6534284au);
        CHECK_EQ(view->FilterForGroup(), 0x3000u);
        CHECK_EQ(view->Release(), 0u);                       // last reference: deletes the object
    }

    // ---- Init without a host: everything the host creates is skipped, the window still initialises -------
    {
        SetResourceCatalogHost(nullptr);
        auto* view = new TestView();
        view->SetWindowManager(&mgr);
        view->AddRef();
        view->SetArea(0, 0, 400, 300);
        CHECK_EQ(view->Init(), true);
        CHECK(view->Instances() == nullptr);
        CHECK_EQ(view->Types().empty(), true);
        CHECK_EQ(view->Shutdown(), true);
        view->Release();
        SetResourceCatalogHost(&host);
    }

    TestView* view = new TestView();
    view->SetWindowManager(&mgr);
    view->AddRef();
    view->SetArea(0, 0, 400, 300);

    // ---- Init: controls, geometry, colours -------------------------------------------------------------
    {
        CHECK_EQ(view->Init(), true);
        CHECK_EQ(view->Init(), true);   // the second call returns early (get_init_flag)

        CHECK_EQ(host.backdrops.size(), static_cast<std::size_t>(1));
        if (!host.backdrops.empty()) {
            FakeBackdrop* backdrop = host.backdrops[0];
            CHECK_EQ(backdrop->outlineFlags, 0xf);
            CHECK_EQ(backdrop->sizeMode, 0);
            CHECK_EQ(backdrop->outlineColor, 0xff000000u);
            CHECK(backdrop->win->GetParentWin() == view);
            CHECK_EQ(backdrop->win->GetArea().right, 400);
            CHECK_EQ(backdrop->win->GetArea().bottom, 300);
            CHECK_EQ(backdrop->win->GetFillColor(), 0xfff0f0f0u);
        }
        // Four buttons in standalone mode, right to left along the bottom, all on one row.
        CHECK_EQ(host.buttons.size(), static_cast<std::size_t>(4));
        if (host.buttons.size() == 4) {
            const std::uint32_t expectedIds[4] = {kResCmdDone, kResCmdAdd, kResCmdRemove, kResCmdCancel};
            const char* expectedTips[4] = {"Done", "Add New Resource", "Remove Selected Resource", "Cancel"};
            int expectedX = 400;
            for (int i = 0; i < 4; ++i) {
                FakeButton* button = host.buttons[i];
                expectedX -= 5 + 26;
                CHECK_EQ(button->buttonId, expectedIds[i]);
                CHECK_EQ(button->caption, std::string(expectedTips[i]));
                CHECK_EQ(button->tip, std::string(expectedTips[i]));
                CHECK_EQ(button->win->GetL(), expectedX);
                CHECK_EQ(button->win->GetT(), 279);       // 300 - 5 - 16 (the "Done" height)
                CHECK_EQ(button->fits, 1);
                CHECK_EQ(button->image, static_cast<cIGZBuffer*>(nullptr));
                CHECK_EQ(button->haveArea, false);
                CHECK_EQ(button->win->GetW(), 26);        // the no-image layout
                CHECK_EQ(button->win->GetH(), 16);
                CHECK(button->win->GetParentWin() == view);
            }
            // tip flag on, the two unused tip placement flags off
            CHECK_EQ(host.buttons[0]->flags.size(), static_cast<std::size_t>(3));
            if (host.buttons[0]->flags.size() == 3) {
                CHECK(host.buttons[0]->flags[0].first == 0x100 && host.buttons[0]->flags[0].second);
                CHECK(host.buttons[0]->flags[1].first == 0x200 && !host.buttons[0]->flags[1].second);
                CHECK(host.buttons[0]->flags[2].first == 0x400 && !host.buttons[0]->flags[2].second);
            }
        }
        // Labels: "Select Type:", "Instances:", "Filename:", "<none>".
        CHECK_EQ(host.labels.size(), static_cast<std::size_t>(4));
        CHECK_EQ(host.fontAsks, 4);
        CHECK_EQ(host.fontStyle, kResourceLabelFontStyle);
        if (host.labels.size() == 4) {
            CHECK_EQ(std::string(host.labels[0]->win->GetCaption().ToChar()), std::string("Select Type:"));
            CHECK_EQ(host.labels[0]->win->GetL(), 5);
            CHECK_EQ(host.labels[0]->win->GetT(), 5);
            CHECK_EQ(host.labels[0]->color, 0xff000000u);
            CHECK_EQ(host.labels[0]->fits, 1);
            CHECK_EQ(host.labels[0]->textFlags.size(), static_cast<std::size_t>(2));
            if (host.labels[0]->textFlags.size() == 2) {
                CHECK(host.labels[0]->textFlags[0].first == 2 && !host.labels[0]->textFlags[0].second);
                CHECK(host.labels[0]->textFlags[1].first == 4 && !host.labels[0]->textFlags[1].second);
            }
            CHECK_EQ(std::string(host.labels[1]->win->GetCaption().ToChar()), std::string("Instances:"));
            CHECK_EQ(host.labels[1]->win->GetID(), kResourceInstancesLabelID);
            CHECK_EQ(host.labels[1]->win->GetL(), 5);
            CHECK_EQ(host.labels[1]->win->GetT(), 43);     // 10 (label height) + 33
            CHECK_EQ(std::string(host.labels[2]->win->GetCaption().ToChar()), std::string("Filename:"));
            CHECK_EQ(host.labels[2]->win->GetID(), kResourceFileNameLabelID);
            CHECK_EQ(host.labels[2]->win->GetL(), 0xdc);
            CHECK_EQ(host.labels[2]->win->GetT(), 0x1ae);
            CHECK_EQ(host.labels[2]->win->GetW(), 200);
            CHECK_EQ(std::string(host.labels[3]->win->GetCaption().ToChar()), std::string("<none>"));
            CHECK_EQ(host.labels[3]->win->GetID(), kResourceFileNameValueID);
            CHECK_EQ(host.labels[3]->win->GetL(), 0xe6);
            CHECK_EQ(host.labels[3]->win->GetT(), 0x1c2);
            CHECK_EQ(host.labels[3]->win->GetW(), 200);
        }
        // The type rows: the ini's two entries make three rows (exemplar, LTEXT, exemplar).
        CHECK_EQ(view->Types().size(), static_cast<std::size_t>(3));
        CHECK_EQ(host.viewCreations, 3);
        if (view->Types().size() == 3) {
            CHECK_EQ(view->Types()[0].type, kResourceExemplarType);
            CHECK_EQ(view->Types()[0].group, 0x1000u);
            CHECK_EQ(std::string(view->Types()[0].name.ToChar()), std::string("Residential"));
            CHECK_EQ(view->Types()[1].type, kResourceTextType);
            CHECK_EQ(view->Types()[1].group, 0x2000u);
            CHECK_EQ(std::string(view->Types()[1].name.ToChar()), std::string("Residential Cohort"));
            CHECK_EQ(view->Types()[2].type, kResourceExemplarType);
            CHECK_EQ(view->Types()[2].group, 0x3000u);
            CHECK_EQ(std::string(view->Types()[2].name.ToChar()), std::string("Industrial"));
        }
        // The combo lists every row and maps its items back to the rows.
        CHECK_EQ(host.combos.size(), static_cast<std::size_t>(1));
        if (!host.combos.empty()) {
            FakeCombo* combo = host.combos[0];
            CHECK_EQ(combo->items.size(), static_cast<std::size_t>(3));
            CHECK_EQ(combo->editable, false);
            CHECK_EQ(combo->win->GetID(), kResourceTypeComboID);
            CHECK_EQ(combo->win->GetL(), 5);
            CHECK_EQ(combo->win->GetT(), 20);            // 10 (label height) + 10
            CHECK_EQ(combo->win->GetW(), 133);           // 138 - 5
            CHECK_EQ(combo->win->GetH(), 18);            // 10 + 28 - 20
            CHECK_EQ(combo->selection, 0);
            CHECK_EQ(combo->selectedNotify, false);
            CHECK_EQ(combo->highlightColor, 0xff000080u);
            CHECK_EQ(combo->win->GetFillColor(), 0xfff0f0f0u);
            CHECK_EQ(combo->fontColors.size(), static_cast<std::size_t>(2));
            if (combo->fontColors.size() == 2) {
                CHECK(combo->fontColors[0].first == 0 && combo->fontColors[0].second == 0xff000000u);
                CHECK(combo->fontColors[1].first == 2 && combo->fontColors[1].second == 0xffffffffu);
            }
            CHECK(combo->win->GetParentWin() == view);
        }
        CHECK_EQ(view->ComboRows().size(), static_cast<std::size_t>(3));
        CHECK_EQ(view->SelectedRow(), 0u);
        // The view windows sit behind the combo area, hidden.
        if (view->Types().size() == 3) {
            FakeView* first = host.views[0];
            CHECK_EQ(first->readOnlyCalls, 1);
            CHECK_EQ(first->readOnly, false);
            CHECK_EQ(first->win->GetW(), 247);           // 400 - 20 - 133
            CHECK_EQ(first->win->GetH(), 285);           // 300 - 15
            CHECK_EQ(first->win->GetL(), 143);           // 133 + 10
            CHECK_EQ(first->win->GetT(), 5);
            CHECK_EQ(host.views[1]->win->IsVisible(), false);   // the rows that are not selected stay hidden
            CHECK(first->win->GetParentWin() == view);
        }
        // The instance grid.
        CHECK(view->Instances() != nullptr);
        CHECK_EQ(host.gridFlags, 0x19bffu);
        if (view->Instances() != nullptr && !host.grids.empty()) {
            FakeGrid* grid = host.grids[0];
            CHECK_EQ(grid->defaultRowHeight, 0xf);
            CHECK_EQ(grid->area.left, 5);
            CHECK_EQ(grid->area.top, 58);                // 43 + 10 + 5
            CHECK_EQ(grid->area.right, 138);
            CHECK_EQ(grid->area.bottom, 295);            // 300 - 5
            CHECK_EQ(grid->columnWidths.size(), static_cast<std::size_t>(1));
            if (!grid->columnWidths.empty()) {
                CHECK(grid->columnWidths[0].first == 0);
                CHECK_EQ(grid->columnWidths[0].second, 127);   // 133 - 6
            }
            CHECK_EQ(grid->clearedFlags.size(), static_cast<std::size_t>(2));
            if (grid->clearedFlags.size() == 2) {
                CHECK_EQ(grid->clearedFlags[0], 2);
                CHECK_EQ(grid->clearedFlags[1], 0x400);
            }
            CHECK_EQ(grid->listingFlags.size(), static_cast<std::size_t>(1));
            CHECK_EQ(grid->win->GetID(), kResourceInstanceGridID);
            CHECK_EQ(grid->win->GetFillColor(), 0xfff0f0f0u);
            CHECK_EQ(grid->refs, 2);                     // Init took its own reference
            CHECK_EQ(grid->fontColors.size(), static_cast<std::size_t>(3));
            if (grid->fontColors.size() == 3) {
                CHECK(grid->fontColors[0].first == 0xff000000u && grid->fontColors[0].second == 0);
                CHECK(grid->fontColors[1].first == 0xffffffffu && grid->fontColors[1].second == 4);
                CHECK(grid->fontColors[2].first == 0xff000080u && grid->fontColors[2].second == 5);
            }
        }
        // updateKeyList ran for the first row: two of the four exemplar keys share the row's group (0x1000).
        CHECK_EQ(host.resourceAsks, 1);
        CHECK_EQ(host.lastTypeAsked, kResourceExemplarType);
        if (!host.grids.empty()) {
            FakeGrid* grid = host.grids[0];
            CHECK_EQ(grid->cells.size(), static_cast<std::size_t>(2));
            CHECK_EQ(grid->cells[0].text, std::string("Alpha"));
            CHECK_EQ(grid->cells[1].text, std::string("0x00000022"));   // no name: the instance in hex
            CHECK(grid->cells[0].data == &host.keys[0]);
            CHECK(grid->cells[1].data == &host.keys[1]);
            CHECK_EQ(grid->sorts, 1);
            CHECK_EQ(grid->sortedColumn, 0);
            CHECK_EQ(grid->sortedStart, 0);
            CHECK_EQ(grid->sortedAscending, true);
            CHECK_EQ(grid->rowHeights, 1);
            CHECK_EQ(grid->lastRowHeight, 0xf);
            CHECK_EQ(grid->selectionRow, 0);
            CHECK_EQ(grid->selectionColumn, 0);
        }
        // ... and the first key went into the first row's view, which was shown.
        if (view->Types().size() == 3) {
            FakeView* first = host.views[0];
            CHECK_EQ(first->haveKey, true);
            CHECK_EQ(first->key.type, kResourceExemplarType);
            CHECK_EQ(first->key.group, 0x1000u);
            CHECK_EQ(first->key.instance, 0x11u);
            CHECK_EQ(first->win->IsVisible(), true);
            CHECK(view->SelectedView() == first);
        }
        CHECK(mgr.focus == static_cast<cIGZWin*>(view->Instances()->AsIGZWin()));
    }

    // ---- reading the selection back ---------------------------------------------------------------------
    {
        cGZPersistResourceKey key{};
        CHECK_EQ(view->GetSelectedKey(key), true);
        CHECK_EQ(key.instance, 0x11u);
        const int asks = host.resourceAsks;
        view->RefreshKeyList();                              // GetSelectedKey -> updateKeyList
        CHECK_EQ(host.resourceAsks, asks + 1);
    }

    // ---- the combo command re-maps the item to a row and refills the grid -------------------------------
    {
        host.combos[0]->selection = 1;                       // "Residential text" (LTEXT, group 0x2000)
        CHECK_EQ(view->GZOnCommand(kComboChangedCommand, kResourceTypeComboID), false);
        CHECK_EQ(view->SelectedRow(), 1u);
        CHECK_EQ(host.lastTypeAsked, kResourceTextType);
        if (!host.grids.empty()) {
            FakeGrid* grid = host.grids[0];
            CHECK_EQ(grid->cells.size(), static_cast<std::size_t>(1));
            CHECK_EQ(grid->cells[0].text, std::string("Beta"));
        }
        if (view->Types().size() == 3) {
            FakeView* second = host.views[1];
            CHECK_EQ(second->key.instance, 0x44u);
            CHECK_EQ(second->win->IsVisible(), true);
            CHECK_EQ(host.views[0]->prompts, 1);             // the outgoing view was asked to save
            CHECK_EQ(host.views[0]->win->IsVisible(), false);
        }
        // A view without the combo child answers true (the original's missing-child branch).
        auto* bare = new TestView();
        bare->SetWindowManager(&mgr);
        bare->AddRef();
        CHECK_EQ(bare->GZOnCommand(kComboChangedCommand, kResourceTypeComboID), true);
        bare->Release();
        // A command for another window id is ignored.
        CHECK_EQ(view->GZOnCommand(kComboChangedCommand, 0x11112222u), false);
        CHECK_EQ(view->GZOnCommand(0xdeadbeef, 0), false);
    }

    // ---- the grid notification selects the cell's key ---------------------------------------------------
    {
        FakeGrid* grid = host.grids[0];
        grid->SetSelection(0, 0);
        cGZMessage message;
        message.type = kGridSelectionChangedMsg;
        message.param1 = reinterpret_cast<std::uint64_t>(static_cast<IResourceCatalogGrid*>(grid));
        CHECK_EQ(view->DoMessage(message), true);
        CHECK_EQ(host.views[1]->key.instance, 0x44u);        // unchanged: the cell's key is 0x44
        // The focus-change message goes to the message server.
        cGZMessage focus;
        focus.type = kFocusChangeMsg;
        const int posts = host.posts;
        CHECK_EQ(view->DoMessage(focus), true);
        CHECK_EQ(host.posts, posts + 1);
        CHECK_EQ(host.lastPost.type, kFocusChangeMsg);
    }

    // ---- "Add New Resource": the key dialog plus the rename of the group's file --------------------------
    {
        char tmpl[] = "/tmp/sc4rcvfileXXXXXX";
        char* dir = mkdtemp(tmpl);
        CHECK(dir != nullptr);
        const std::string oldPath = std::string(dir) + "/old.Lot";
        FILE* f = std::fopen(oldPath.c_str(), "w");
        if (f) { std::fputs("x", f); std::fclose(f); }
        host.dialogAccepted = true;
        host.dialogKey = {kResourceExemplarType, 0x1000, 0x99};
        host.dialogFileName = "newname";
        host.currentFileName = oldPath;
        host.hasKey = false;
        CHECK_EQ(view->SelectedRow(), 1u);
        // Row 1 is the LTEXT row: the dialog seed takes its type/group and instance 1.
        CHECK_EQ(view->doButtonPress(kResCmdAdd), true);
        CHECK_EQ(host.dialogs, 1);
        CHECK_EQ(host.dialogSeed.type, kResourceTextType);
        CHECK_EQ(host.dialogSeed.group, 0x2000u);
        CHECK_EQ(host.dialogSeed.instance, 1u);
        CHECK_EQ(host.testForKeyCalls, 1);
        CHECK_EQ(host.createCalls, 1);
        CHECK_EQ(host.lastNewKey.instance, 0x99u);
        CHECK_EQ(host.saveCalls, 1);
        CHECK_EQ(host.fileNameCalls, 1);
        const std::string newPath = std::string(dir) + "/newname.Lot";
        CHECK_EQ(::access(newPath.c_str(), F_OK), 0);        // cRZFile::Rename moved it
        CHECK_EQ(::access(oldPath.c_str(), F_OK), -1);
        // A rejected dialog does nothing.
        host.dialogAccepted = false;
        CHECK_EQ(view->addResource(), false);
        host.dialogAccepted = true;
        // An already existing key stops before the file step.
        host.hasKey = true;
        CHECK_EQ(view->addResource(), false);
        CHECK_EQ(host.fileNameCalls, 1);
        host.hasKey = false;
        // "Remove Selected Resource" does nothing but answer true; "Cancel" kills with result 0.
        CHECK_EQ(view->doButtonPress(kResCmdRemove), true);
        CHECK_EQ(view->doButtonPress(0x12345678u), false);
        mgr.dialogs.clear();
        mgr.posted.clear();
        CHECK_EQ(view->GZOnCommand(kButtonActivatedCommand, kResCmdCancel), false);
        CHECK_EQ(mgr.dialogs.size(), static_cast<std::size_t>(0));   // not modal: it posts the delete-child message
        CHECK_EQ(mgr.posted.size(), static_cast<std::size_t>(1));
        if (!mgr.posted.empty()) {
            CHECK_EQ(mgr.posted[0].type, kGZMsgDeleteChild);
            CHECK_EQ(mgr.posted[0].param2, 0u);                      // result 0
        }
        // "Done" answers false and posts a delete-child message to the parent (not modal).
        FakeWindow* parent = MakeChild(mgr, 10, 10);
        extraWindows.push_back(parent);
        parent->ChildAdd(view);
        mgr.posted.clear();
        const int prompts = host.views[1]->prompts;
        CHECK_EQ(view->GZOnCommand(kButtonActivatedCommand, kResCmdDone), false);
        CHECK_EQ(host.views[1]->prompts, prompts + 1);
        CHECK_EQ(mgr.posted.size(), static_cast<std::size_t>(1));
        if (!mgr.posted.empty()) {
            CHECK_EQ(mgr.posted[0].type, kGZMsgDeleteChild);
            CHECK(mgr.posted[0].param1 == reinterpret_cast<std::uint64_t>(static_cast<cIGZWin*>(view)));
        }
        // As a modal window (flag 0x1000, which cGZWin does not set by default) the command ends the dialog.
        mgr.dialogs.clear();
        view->SetFlag(0x1000, true);
        CHECK_EQ(view->doButtonPress(kResCmdDone), false);
        CHECK_EQ(mgr.dialogs.size(), static_cast<std::size_t>(1));
        if (!mgr.dialogs.empty()) {
            CHECK_EQ(mgr.dialogs[0].code, 0xfu);
            CHECK_EQ(mgr.dialogs[0].window, static_cast<cIGZWin*>(view));
            CHECK_EQ(mgr.dialogs[0].result, 1);
        }
        view->SetFlag(0x1000, false);
    }

    // ---- keyboard forwarding, painting and dragging ------------------------------------------------------
    {
        FakeWindow* target = MakeChild(mgr, 10, 10);
        extraWindows.push_back(target);
        view->SetNotificationTarget(target);
        CHECK_EQ(view->GZOnKeyDown(0x41, 2), true);
        CHECK_EQ(target->messages, 1);
        CHECK_EQ(target->last.type, kGZMsgKeyDown);
        CHECK_EQ(target->last.param1, 0x41u);
        CHECK_EQ(target->last.param2, 2u);
        CHECK_EQ(view->GZOnKeyUp(0x41, 2), true);
        CHECK_EQ(target->last.type, kGZMsgKeyUp);

        FakeBuffer buffer;
        cRZRect drawArea{1, 2, 3, 4};
        view->SetDraw(&buffer, drawArea);
        CHECK_EQ(view->GZPaint(), true);
        CHECK_EQ(buffer.locks, 1);
        CHECK_EQ(buffer.unlocks, 1);
        CHECK_EQ(buffer.fills, 1);
        CHECK_EQ(buffer.fillColor, 0u);
        CHECK_EQ(buffer.fillRect.left, 1);
        CHECK_EQ(buffer.fillRect.bottom, 4);
        view->SetDraw(nullptr, cRZRect{});

        view->SetArea(10, 20, 410, 320);
        CHECK_EQ(view->GZOnMouseDownL(15, 25, 0), true);
        CHECK(mgr.capture == static_cast<cIGZWin*>(view));
        CHECK_EQ(view->GZOnMouseMove(50, 60, 0), true);
        CHECK_EQ(view->GetL(), 50 + 10 - 15);   // pointer + (window top-left - pointer) = same offset
        CHECK_EQ(view->GetT(), 60 + 20 - 25);
        CHECK_EQ(view->GZOnMouseUpL(0, 0, 0), true);
        CHECK(mgr.capture == nullptr);
        view->SetArea(0, 0, 400, 300);
    }

    // ---- registerTypesFromIni directly: the parser edge cases -------------------------------------------
    {
        auto* fresh = new TestView();
        fresh->SetWindowManager(&mgr);
        fresh->AddRef();
        const int before = host.viewCreations;
        cSCResourceCatalogView::registerTypesFromIni(cRZString("OnlyOne"), cRZString("0x77"), fresh);
        CHECK_EQ(host.viewCreations, before + 1);
        CHECK_EQ(fresh->Types().size(), static_cast<std::size_t>(1));
        if (!fresh->Types().empty()) {
            CHECK_EQ(fresh->Types()[0].group, 0x77u);
            CHECK_EQ(fresh->Types()[0].type, kResourceExemplarType);
        }
        cSCResourceCatalogView::registerTypesFromIni(cRZString("Two"), cRZString(" 0x10 , 0x20 "), fresh);
        CHECK_EQ(fresh->Types().size(), static_cast<std::size_t>(3));
        if (fresh->Types().size() == 3) {
            CHECK_EQ(fresh->Types()[1].group, 0x10u);
            CHECK_EQ(fresh->Types()[2].type, kResourceTextType);
            CHECK_EQ(fresh->Types()[2].group, 0x20u);
            CHECK_EQ(std::string(fresh->Types()[2].name.ToChar()), std::string("Two Cohort"));
        }
        // An entry written without a group ("Name=") makes an exemplar row of group 0.
        cSCResourceCatalogView::registerTypesFromIni(cRZString("Empty"), cRZString(""), fresh);
        CHECK_EQ(fresh->Types().size(), static_cast<std::size_t>(4));
        if (fresh->Types().size() == 4) CHECK_EQ(fresh->Types()[3].group, 0u);
        fresh->Release();
    }

    // ---- Shutdown: views and grid released, rows dropped, notification target cleared --------------------
    {
        FakeGrid* grid = host.grids[0];
        CHECK_EQ(grid->refs, 2);
        CHECK_EQ(view->Shutdown(), true);
        CHECK_EQ(view->Types().empty(), true);
        CHECK(view->Instances() == nullptr);
        CHECK(view->SelectedView() == nullptr);
        CHECK_EQ(grid->refs, 1);
        CHECK_EQ(view->GetNotificationTarget(), nullptr);
        // After Shutdown the grid is gone, so the selection helpers answer false.
        cGZPersistResourceKey key{};
        CHECK_EQ(view->GetSelectedKey(key), false);
        CHECK_EQ(view->Init(), true);                         // a second Init builds a fresh grid
        CHECK(view->Instances() != nullptr);
        CHECK_EQ(host.grids.size(), static_cast<std::size_t>(2));
    }

    view->Release();

    // ---- a filtered view only lists the rows that pass -------------------------------------------------
    {
        auto* filtered = new TestView();
        filtered->SetWindowManager(&mgr);
        filtered->AddRef();
        filtered->SetArea(0, 0, 400, 300);
        filtered->SetFilterForGroup(0x3000);
        filtered->SetReadOnly(true);
        const std::size_t combos = host.combos.size();
        CHECK_EQ(filtered->Init(), true);
        CHECK_EQ(host.combos.size(), combos + 1);
        if (host.combos.size() > combos) {
            FakeCombo* combo = host.combos.back();
            CHECK_EQ(combo->items.size(), static_cast<std::size_t>(1));
            if (!combo->items.empty()) CHECK_EQ(combo->items[0], std::string("Industrial"));
        }
        CHECK_EQ(filtered->ComboRows().size(), static_cast<std::size_t>(1));
        CHECK_EQ(filtered->SelectedRow(), 2u);               // the Industrial row of the shared ini
        if (!host.views.empty()) CHECK_EQ(host.views.back()->readOnly, true);
        filtered->Release();
    }

    // ---- a type filter is applied on top of the group filter -------------------------------------------
    {
        auto* filtered = new TestView();
        filtered->SetWindowManager(&mgr);
        filtered->AddRef();
        filtered->SetArea(0, 0, 400, 300);
        filtered->SetFilterForGroup(0x2000);
        filtered->SetFilterForType(kResourceTextType);
        const std::size_t combos = host.combos.size();
        CHECK_EQ(filtered->Init(), true);
        if (host.combos.size() > combos) {
            FakeCombo* combo = host.combos.back();
            CHECK_EQ(combo->items.size(), static_cast<std::size_t>(1));
            if (!combo->items.empty()) CHECK_EQ(combo->items[0], std::string("Residential Cohort"));
        }
        filtered->Release();
    }

    // ---- no ini file: no rows, no grid content ---------------------------------------------------------
    {
        host.dataDir = std::string(dataDir) + "/missing";
        auto* empty = new TestView();
        empty->SetWindowManager(&mgr);
        empty->AddRef();
        empty->SetArea(0, 0, 400, 300);
        const std::size_t gridCount = host.grids.size();
        CHECK_EQ(empty->Init(), true);
        CHECK_EQ(empty->Types().empty(), true);
        CHECK_EQ(host.grids.size(), gridCount + 1);         // the grid is created even without rows
        CHECK_EQ(empty->SelectedRow(), 0u);                 // map[0] inserts 0
        if (host.grids.size() > gridCount) {
            CHECK_EQ(host.grids.back()->cells.empty(), true);
            CHECK_EQ(host.grids.back()->clearAll, 0);   // updateKeyList returns before touching the grid
        }
        empty->Release();
        host.dataDir = dataDir;
    }

    // The windows MakeChild made carry one reference of their own; the fake controls own them and are freed last.
    for (FakeWindow* window : extraWindows) window->Release();
    for (FakeLabel* x : host.labels) { x->win->Release(); delete x; }
    for (FakeButton* x : host.buttons) { x->win->Release(); delete x; }
    for (FakeCombo* x : host.combos) { x->win->Release(); delete x; }
    for (FakeBackdrop* x : host.backdrops) { x->win->Release(); delete x; }
    for (FakeGrid* x : host.grids) { x->win->Release(); delete x; }
    for (FakeView* x : host.views) { x->win->Release(); delete x; }
    SetResourceCatalogHost(nullptr);
    SetWinGraphics(nullptr);
    std::remove((dataDir + "/Sys/Ingred.ini").c_str());
    rmdir((dataDir + "/Sys").c_str());
    rmdir(dataDir.c_str());
    std::printf(failures == 0 ? "ok\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
