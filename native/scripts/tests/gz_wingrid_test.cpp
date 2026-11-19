// cGZWinGrid test — construction defaults, cell data model, formats, selection, row/column sizes, headers,
// copying / deleting / making room, sorting, hit testing, the text-edit seam and a paint smoke test.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fake_drawcontext.h"
#include "fake_winmgr.h"
#include "gz/WinFont.h"
#include "gz/WinGrid.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;

namespace {

struct FakeFont final : IWinFont {
    int refs = 0;
    std::vector<std::string> drawn;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t DefaultTextColor() override { return 0; }
    std::uint32_t DefaultShadowColor() override { return 0; }
    std::int16_t LineHeight(int, int) override { return 8; }
    int CountLines(const char* text, std::uint32_t length, int) override {
        int lines = 1;
        for (std::uint32_t i = 0; i < length; ++i) if (text[i] == '\n') ++lines;
        return lines;
    }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 0; }
    void MeasureText(const char*, std::uint32_t, cRZRect&) override {}
    int TextWidth(const char*, std::uint32_t length) override { return static_cast<int>(length) * 5; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*,
                  int) override {}
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char* text, std::uint32_t length, std::uint32_t, std::uint32_t,
                     std::uint32_t, const cRZRect*, int) override {
        drawn.push_back(std::string(text, length));
    }
};

struct Fonts final : IWinFontSource {
    FakeFont font;
    IWinFont* Font(std::uint32_t) override { return &font; }
    bool Available() override { return true; }
};

struct FakeBuffer final : IWinBuffer {
    int refs = 1;
    cRZRect bounds{0, 0, 10, 10};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    const cRZRect* GetBounds() const override { return &bounds; }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (r << 16) | (g << 8) | b;
    }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
};

struct Target final : cGZWin {
    using cGZWin::cGZWin;
    std::vector<cGZMessage> got;
    bool DoMessage(cGZMessage& message) override { got.push_back(message); return false; }
};

// cIGZMessageTarget for the owner-draw notifications (the grid holds one reference, which a stack object ignores).
struct Sink final : cIGZMessageTarget {
    int refs = 1;
    std::vector<cGZMessage> got;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool DoMessage(cGZMessage& message) override { got.push_back(message); return false; }
};

struct Host final : IWinGridHost {
    int edits = 0, writes = 0;
    std::map<cIGZWin*, std::string> texts;
    bool GetCellWindowText(cIGZWin* window, cIGZString& out) override {
        auto it = texts.find(window);
        if (it == texts.end()) return false;
        out.AssignFromCString(it->second.c_str());
        return true;
    }
    bool SetCellWindowText(cIGZWin* window, const char* text, std::uint32_t length) override {
        if (!window) return false;
        texts[window] = std::string(text ? text : "", length == 0xffffffffu || !text ? std::strlen(text ? text : "")
                                                                                    : length);
        ++writes;
        return true;
    }
    cIGZWin* CreateCellTextEdit(cIGZWinMgr* mgr) override {
        ++edits;
        auto* window = new Target(mgr);
        window->AddRef();   // one reference, as the window control manager hands out
        return window;
    }
    gz::cIGZBuffer* AsScrollBarImage(IWinBuffer*) override { return nullptr; }
};

struct Grid final : cGZWinGrid {
    using cGZWinGrid::cGZWinGrid;
    void SetContext(cIGZDrawContext* context) { drawContext_ = context; }
    void FitDrawArea() { drawArea_ = cRZRect{0, 0, GetW(), GetH()}; }
    CellFormat& Default() { return defaultFormat_; }
    CellFormat& Heading() { return headingFormat_; }
    void SetRawFill(std::uint32_t rgb) { SetFillColorRGB(rgb); }
};

void SetText(Grid* grid, int column, int row, const char* text) {
    CHECK(grid->SetCellTextValue(column, row, text, 0xffffffffu, 1, false));
}

std::string Text(Grid* grid, int column, int row) {
    cRZString value;
    if (!grid->GetCellTextValue(column, row, value)) return "<none>";
    return value.Data();
}

struct Group {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    Fonts fonts;
    Host host;
    Target* parent;
    Group() {
        SetWinGraphics(&graphics);
        SetWinFontSource(&fonts);
        SetWinGridHost(&host);
        parent = new Target(&mgr);
        parent->AddRef();
        parent->SetArea(0, 0, 400, 300);
    }
    ~Group() {
        parent->Release();
        SetWinGridHost(nullptr);
        SetWinFontSource(nullptr);
        SetWinGraphics(nullptr);
    }
};

}  // namespace

int main() {
    Group group;
    auto* grid = new Grid(&group.mgr, 0);
    grid->AddRef();
    grid->SetID(0x41);
    grid->SetNotificationTarget(group.parent);
    grid->SetArea(0, 0, 200, 100);

    // --- construction defaults (0x10025086a) ---
    CHECK(grid->GetFlags() == 0);
    // (the original's constructor, Mac i386 0x001b8d03: +0x14c = 100, +0x150 = 0x14)
    CHECK(grid->GetDefaultColumnWidth() == 100 && grid->GetDefaultRowHeight() == 0x14);
    int headerHeight = 0, headerWidth = 0;
    CHECK(grid->GetHeaderSizes(headerHeight, headerWidth) && headerHeight == 20 && headerWidth == 5);
    int l = 0, t = 0, r = 0, b = 0;
    CHECK(grid->GetGutters(l, t, r, b) && l == 5 && t == 5 && r == 5 && b == 5);
    CHECK(grid->GetCellGutters(l, t, r, b) && l == 1 && t == 1 && r == 1 && b == 1);
    CHECK(grid->GetSelectionCount() == 0 && grid->GetDefaultTextAlignment() == 2);
    int columnRule = 0, selectionLimit = 0;
    CHECK(grid->GetSelectionRules(columnRule, selectionLimit) && columnRule == 1 && selectionLimit == 0x7fffffff);
    CHECK(grid->GetOutlineColor() == 0);
    CHECK(grid->GetCellLimits(l, r) && l == -1 && r == -1 && grid->IsColumnWithinBounds(1000));
    cRZRect used;
    CHECK(!grid->GetCellUsedRange(used) && used.left == 0 && used.right == 0);

    // --- Init: default formatting (0x100250f5a) ---
    CHECK(grid->Init() && grid->Init());   // the second call is a no-op
    group.parent->ChildAdd(grid);          // ChildAdd also calls Init, which is now a no-op
    CHECK(grid->GetDefaultFont() != nullptr && grid->GetDefaultFont() == &group.fonts.font);
    std::uint32_t color = 0;
    CHECK(grid->GetDefaultFontColor(color, 0) && color == 0xff000000u);
    CHECK(grid->GetDefaultFontColor(color, 1) && color == 0xffffffffu);
    CHECK(grid->GetDefaultFontColor(color, 2) && color == 0xffc0c0c0u);
    CHECK(grid->GetDefaultFontColor(color, 4) && color == 0xffffffffu);
    CHECK(grid->GetDefaultFontColor(color, 5) && color == 0xff000000u);
    CHECK(!grid->GetDefaultFontColor(color, 6) && !grid->SetDefaultFontColor(1, 6));
    CHECK(grid->SetDefaultFontColor(0x11223344u, 3) && grid->GetDefaultFontColor(color, 3) && color == 0x11223344u);
    int columnLine = 0, rowLine = 0;
    CHECK(grid->GetGridColors(columnLine, rowLine) && columnLine == static_cast<int>(0xff000000u) &&
          rowLine == static_cast<int>(0xff000000u));
    CHECK(grid->Default().flag == 1 && grid->Default().halign == 2 && grid->Default().valign == 5);
    CHECK(grid->Heading().valign == 6 && grid->Heading().flag == 1);

    // --- cells and text (0x100259ab8 / 0x1002599e6) ---
    SetText(grid, 2, 3, "hello");
    int type = -1;
    CHECK(grid->GetCellType(2, 3, type) && type == 2);
    CHECK(Text(grid, 2, 3) == "hello");
    int recentColumn = 0, recentRow = 0;
    CHECK(grid->GetMostRecentlyAddedCell(recentColumn, recentRow) && recentColumn == 2 && recentRow == 3);
    CHECK(grid->GetCellUsedRange(used) && used.left == 2 && used.top == 3 && used.right == 2 && used.bottom == 3);
    SetText(grid, 2, 3, "world");   // overwrites
    CHECK(Text(grid, 2, 3) == "world");
    // mode 4 inserts and shifts the cells after the insertion point right (mode 2 shifts the other way)
    SetText(grid, 1, 0, "x");
    CHECK(grid->SetCellTextValue(0, 0, "y", 1, 4, false));
    CHECK(Text(grid, 0, 0) == "y" && Text(grid, 1, 0) == "<none>" && Text(grid, 2, 0) == "x");
    // the 0x7fffffff sentinel: the new cell lands after the most used column's last row
    CHECK(grid->SetCellTextValue(0x7fffffff, 0x7fffffff, "tail", 4, 1, false));
    CHECK(Text(grid, 2, 4) == "tail");
    CHECK(!grid->GetCellType(9, 9, type));
    CHECK(grid->ClearCell(2, 3) && !grid->GetCellType(2, 3, type));

    // --- cell formats (0x100259096 / 0x1002589d2 / 0x1002586c4) ---
    CHECK(grid->SetCellColors(0, 0, 2, 0xaabbccu));
    CHECK(grid->SetCellAlignment(0, 0, 3, 6));
    CHECK(grid->SetCellBorder(0, 0, 1, 0x123456));
    CHECK(grid->SetCellFont(0, 0, &group.fonts.font));
    cIGZWinGrid::CellFormat format;
    CHECK(grid->GetCellFormat(0, 0, format));
    CHECK(format.colors[2] == 0xaabbccu && format.halign == 3 && format.valign == 6 && format.border == 1 &&
          format.borderColor == 0x123456u && format.font == &group.fonts.font);
    CHECK(group.fonts.font.refs >= 1);
    CHECK(grid->ClearCellFormat(0, 0));
    CHECK(grid->GetCellFormat(0, 0, format) && format.halign == 2 && format.colors[2] == 0xffc0c0c0u);
    CHECK(grid->GetCellType(0, 0, type) && type == 2);   // the text survived
    // column / row formats are merged under the cell format
    CHECK(grid->SetDefaultTextAlignmentByColumns(0, 1, 4) && grid->SetDefaultFontColorByColumns(0x00ff00u, 3, 0, 1));
    CHECK(grid->SetDefaultTextAlignmentByRows(0, 1, 7) && grid->SetDefaultFontColorByRows(0x0000ffu, 4, 0, 1));
    // the column format overrides the row format's horizontal alignment; the row colour survives
    CHECK(grid->GetCellFormat(0, 0, format) && format.halign == 4 && format.valign == 5 &&
          format.colors[3] == 0x00ff00u && format.colors[4] == 0x0000ffu);
    cIGZWinGrid::CellFormatExtra* extra = nullptr;
    grid->GetColCellFormatExtra(0, extra);
    CHECK(extra != nullptr);
    extra = nullptr;
    grid->GetRowCellFormatExtra(0, extra);
    CHECK(extra != nullptr);
    extra = nullptr;
    CHECK(grid->GetColRowCellFormatExtra(0, 0, extra) && extra != nullptr);
    // the whole-grid format
    cIGZWinGrid::CellFormat special = grid->Default();
    special.halign = 5;
    CHECK(grid->SetCellFormat(-1, -1, special) && grid->Default().halign == 5);

    // --- column widths / row heights (0x100256698 / 0x10025d328) ---
    CHECK(grid->SetColumnWidth(1, 2, 50));
    CHECK(grid->GetColumnWidth(1, 1) == 50 && grid->GetColumnWidth(2, 1) == 50 && grid->GetColumnWidth(0, 1) == 100);
    grid->SetDefaultColumnWidth(40);
    CHECK(grid->GetColumnWidth(5, 1) == 40);
    CHECK(grid->SetRowHeight(0, 1, 30) && grid->GetRowHeight(0, 1) == 30);
    grid->SetDefaultRowHeight(24);
    CHECK(grid->GetDefaultRowHeight() == 24);
    CHECK(grid->GetColumnWidth(0, 3) == 100 + 50 + 50);
    CHECK(grid->SetColumnWidth(3, 1, -5) && grid->GetColumnWidth(3, 1) == 0);   // clamped to 0

    // --- selection (0x100255dcc / 0x100255e62) ---
    CHECK(grid->SetSelectionRules(2, 5));
    CHECK(grid->SetSelection(1, 2) && grid->GetSelectionCount() == 1 && grid->GetCellIsSelected(1, 2));
    CHECK(grid->AddCellToSelection(3, 4, true) && grid->GetSelectionCount() == 2);
    cIGZWinGrid::CellCoordinates cells[4];
    int count = 4;
    CHECK(grid->GetSelection(cells, count) && count == 2 && cells[0].col == 1 && cells[0].row == 2 &&
          cells[1].col == 3 && cells[1].row == 4);   // oldest first (the list is appended to)
    int selectedColumn = 0, selectedRow = 0;
    CHECK(grid->GetSelection(selectedColumn, selectedRow) && selectedColumn == 1 && selectedRow == 2);
    CHECK(grid->AddCellToSelection(3, 4, false) && grid->GetSelectionCount() == 1);
    CHECK(grid->SetSelectionRules(2, 0) && !grid->SetSelection(5, 5));
    CHECK(grid->SetSelectionRules(2, 10) && grid->ClearSelection() && grid->GetSelectionCount() == 0);

    // --- headers (0x100256824 / 0x100256996) ---
    CHECK(!grid->EnableHeaders(true, true));   // quirk: the original answers false
    CHECK(grid->IsFlagSet(kGridColumnHeaders) && grid->IsFlagSet(kGridRowHeaders));
    CHECK(grid->SetColumnHeading(0, cRZString("Name")) && grid->SetRowHeading(2, cRZString("Row2")));
    cRZString heading;
    CHECK(grid->GetColumnHeading(0, heading) && std::string(heading.Data()) == "Name");
    CHECK(grid->GetRowHeading(2, heading) && std::string(heading.Data()) == "Row2");
    CHECK(grid->GetColumnHeading(7, heading) && std::string(heading.Data()) == "");
    CHECK(grid->ClearAllColumnHeadings() && grid->GetColumnHeading(0, heading) && heading.Strlen() == 0);
    CHECK(grid->ClearAllRowHeadings());
    CHECK(grid->SetHeaderSizes(30, 15));
    CHECK(grid->GetHeaderSizes(headerHeight, headerWidth) && headerHeight == 30 && headerWidth == 15);
    cRZRect headerArea;
    CHECK(grid->GetColumnHeaderArea(0, headerArea) && headerArea.bottom - headerArea.top == 30);
    CHECK(grid->GetRowHeaderArea(0, headerArea) && headerArea.right - headerArea.left == 15);
    CHECK(grid->GetColumnHeaderArea(-1, headerArea) && headerArea.right == grid->GetW() - 5);   // the whole band
    grid->Refresh();

    // --- extra data (0x10025b392 / 0x10025b480) ---
    int payload = 7;
    CHECK(grid->SetCellExtraData(1, 1, &payload) && grid->GetCellExtraData(1, 1) == &payload);
    CHECK(grid->SetColumnExtraData(1, &payload) && grid->GetColumnExtraData(1) == &payload);
    CHECK(grid->SetRowExtraData(1, &payload) && grid->GetRowExtraData(1) == &payload);
    CHECK(grid->SetExtraData(&payload) && grid->GetExtraData() == &payload);
    CHECK(grid->GetCellExtraData(9, 9) == nullptr);
    CHECK(grid->GetCellType(1, 1, type) && type == 0);

    // --- bitmaps / cell windows (0x10025a432 / 0x10025a624) ---
    FakeBuffer buffer;
    CHECK(grid->SetCellBitmap(4, 4, &buffer, 1));
    IWinBuffer* bitmap = nullptr;
    CHECK(grid->GetCellBitmap(4, 4, bitmap) && bitmap == &buffer && buffer.refs == 2);
    CHECK(grid->ClearCell(4, 4) && buffer.refs == 1);
    auto* window = new Target(&group.mgr);
    window->AddRef();
    window->SetArea(0, 0, 20, 10);
    CHECK(grid->SetCellWindow(2, 2, window, true, true, 1));
    cIGZWin* cellWindow = nullptr;
    CHECK(grid->GetCellWindow(2, 2, cellWindow) && cellWindow == window);
    int windowColumn = -1, windowRow = -1;
    CHECK(grid->IsWindowACellWindow(window, 0, &windowColumn, &windowRow) && windowColumn == 2 && windowRow == 2);
    CHECK(grid->ClearCell(2, 2));
    window->Release();

    // --- copy / delete / make room (0x100257bc8 / 0x100257062 / 0x100259eb4) ---
    auto* data = new Grid(&group.mgr, 0);
    data->AddRef();
    data->SetArea(0, 0, 300, 120);
    CHECK(data->Init());
    SetText(data, 0, 0, "a");
    SetText(data, 0, 1, "b");
    SetText(data, 1, 0, "c");
    CHECK(data->CopyColumns(4, 0, 2, false));   // copy two columns
    CHECK(Text(data, 4, 0) == "a" && Text(data, 4, 1) == "b" && Text(data, 5, 0) == "c");
    CHECK(Text(data, 0, 0) == "a");             // copy=false keeps the source
    CHECK(data->DeleteColumns(4, 1, 2));        // shift the remaining columns left
    CHECK(Text(data, 4, 0) == "c" && Text(data, 5, 0) == "<none>");
    CHECK(data->DeleteRows(0, 1, 3));           // shift the remaining rows up
    CHECK(Text(data, 0, 0) == "b");
    CHECK(data->ClearCellRect(0, 0, 10, 10) && Text(data, 0, 0) == "<none>");
    SetText(data, 1, 1, "clear");
    CHECK(data->ClearAllCells() && data->GetSelectionCount() == 0);

    // --- sorting (0x10025b758 / 0x10025bd9e) ---
    auto* sorted = new Grid(&group.mgr, 0);
    sorted->AddRef();
    sorted->SetArea(0, 0, 300, 120);
    CHECK(sorted->Init());
    SetText(sorted, 0, 0, "pear");
    SetText(sorted, 0, 1, "apple");
    SetText(sorted, 0, 2, "fig");
    sorted->SortColumn(0, 0, true, nullptr);
    CHECK(Text(sorted, 0, 0) == "apple" && Text(sorted, 0, 1) == "fig" && Text(sorted, 0, 2) == "pear");
    SetText(sorted, 1, 0, "c");
    SetText(sorted, 2, 0, "a");
    SetText(sorted, 3, 0, "b");
    sorted->SortRow(0, 0, true, nullptr);
    CHECK(Text(sorted, 0, 0) == "a" && Text(sorted, 1, 0) == "apple" && Text(sorted, 2, 0) == "b" &&
          Text(sorted, 3, 0) == "c");
    int foundColumn = -1, foundRow = -1;
    CHECK(sorted->FindCellText("fig", 3, 0, 1, 0, 10, foundColumn, foundRow, false) && foundColumn == 0 &&
          foundRow == 1);
    CHECK(!sorted->FindCellText("zzz", 3, 0, 10, 0, 10, foundColumn, foundRow, true));
    CHECK(sorted->FindCellText("apple", 5, 0, 10, 0, 10, foundColumn, foundRow, true) && foundRow == 0 &&
          foundColumn == 1);

    // --- hit testing, scrolling, mouse selection (0x100252c94 / 0x1002558f0 / 0x10025237e) ---
    auto* view = new Grid(&group.mgr, 0x2 | 0x4);   // scroll bars allowed, but the factory is absent
    view->AddRef();
    view->SetNotificationTarget(group.parent);
    view->SetArea(0, 0, 200, 100);
    CHECK(view->Init());
    grid->EnableHeaders(false, false);
    grid->SetFirstVisibleCell(0, 0);
    grid->Refresh();
    cIGZWinGrid::CursorHitTestResult hit = cIGZWinGrid::kCursorNothing;
    cIGZWinGrid::CellCoordinates hitCell{0, 0};
    CHECK(grid->DoCursorHitTest(50, 10, hit, hitCell) && hit == cIGZWinGrid::kCursorCell && hitCell.col == 0 &&
          hitCell.row == 0);
    CHECK(grid->DoCursorHitTest(-5, -5, hit, hitCell) && hit == cIGZWinGrid::kCursorOutsideGrid);
    CHECK(grid->GZOnMouseDownL(50, 10, 0) && grid->GetSelectionCount() == 1 && grid->GetCellIsSelected(0, 0));
    CHECK(grid->GZOnMouseUpL(50, 10, 0));
    CHECK(grid->GZOnMouseMove(50, 10, 0));
    CHECK(grid->GZOnMouseWheel(0, 0, 0, 120));
    int firstColumn = -1, firstRow = -1;
    CHECK(grid->GetFirstVisibleCell(firstColumn, firstRow) && firstColumn == 0 && firstRow == 0);
    float visibleColumns = 0.0f, visibleRows = 0.0f;
    CHECK(grid->GetVisibleColumnAndRowCount(visibleColumns, visibleRows) && visibleColumns > 0.0f);
    CHECK(grid->MakeCellVisible(1, 1));
    cRZRect cellRect;
    CHECK(grid->GetCellRect(0, 0, cellRect) && cellRect.left == 5 && cellRect.top == 5);
    CHECK(grid->IsCellVisible(0, 0));
    CHECK(!grid->DeleteCell(0, 0, 0, 0));   // quirk: always false
    CHECK(grid->EnableFixedRanges(false, false) && grid->GetCellLimits(l, r) && l == -1 && r == -1);
    CHECK(grid->SetCellLimits(4, 4) && !grid->IsColumnWithinBounds(4) && grid->IsColumnWithinBounds(3));
    CHECK(grid->SetCellLimits(-1, -1));
    CHECK(grid->SetCellLimits(4, 4) && grid->GetCellLimits(l, r) && l == 4 && r == 4);
    CHECK(grid->SetCellLimits(-1, -1));
    CHECK(grid->SetBackgroundColor(0xff102030u) && grid->GetFillColorRGB() == 0x00102030u);
    CHECK(grid->SetBackgroundBitmap(&buffer) && buffer.refs == 2);
    CHECK(grid->SetBackgroundBitmap(nullptr) && buffer.refs == 1);
    CHECK(grid->SetGridColors(0xff010203, 0xff040506));
    CHECK(grid->GetGridColors(columnLine, rowLine) && columnLine == static_cast<int>(0xff010203u) &&
          rowLine == static_cast<int>(0xff040506u));
    CHECK(grid->SetSoundID(0, 5) && !grid->SetSoundID(3, 5));
    CHECK(grid->MakeDropDown() && grid->IsFlagSet(0x40000));
    CHECK(grid->SetDefaultTextWrapping(true) && grid->GetDefaultTextWrapping());
    CHECK(grid->SetDefaultTextWrapping(false) && !grid->GetDefaultTextWrapping());
    grid->SetOutlineUse(true, 0xffaabbcc);
    CHECK(grid->GetOutlineColor() == 0xffaabbccu && grid->IsFlagSet(kGridOutline));
    grid->PauseRecalculations(true);
    grid->PauseRecalculations(false);

    // --- text-edit seam (0x100259ab8 with makeTextEdit) ---
    CHECK(grid->SetCellTextValue(6, 6, "editable", 8, 1, true));
    CHECK(group.host.edits == 1);
    cRZString editText;
    CHECK(grid->GetCellTextValue(6, 6, editText) && std::string(editText.Data()) == "editable");
    CHECK(grid->SetCellTextValue(6, 6, "again", 5, 1, true) && group.host.writes >= 2);
    CHECK(grid->ClearCell(6, 6));

    // --- owner draw notification and buffer (0x10025c864 / 0x10025c8a0) ---
    Sink sink;
    CHECK(grid->SetOwnerDrawNotification(&sink) && sink.refs == 2);
    IWinBuffer* drawBuffer = nullptr;
    cRZRect drawRect;
    CHECK(grid->GetBufferToDrawTo(drawBuffer, drawRect) && drawBuffer == nullptr);
    CHECK(grid->SetScrollBarImageVertical(&buffer, cRZRect{0, 0, 10, 10}) && buffer.refs == 2);
    CHECK(grid->SetScrollBarImageHorizontal(&buffer, cRZRect{0, 0, 10, 10}) && buffer.refs == 3);
    CHECK(grid->SetScrollBarImageVertical(nullptr, cRZRect{}) && buffer.refs == 2);
    CHECK(grid->SetScrollBarImageHorizontal(nullptr, cRZRect{}) && buffer.refs == 1);
    CHECK(grid->SetOwnerDrawNotification(nullptr) && sink.refs == 1);

    // --- a paint smoke test (0x100252f70) ---
    fake_dc::Ctx context;
    auto* painted = new Grid(&group.mgr, kGridBackground | kGridColumnHeaders | kGridRowHeaders | kGridColumnLines |
                                              kGridRowLines | kGridOutline | kGridCellAreaOutline);
    painted->AddRef();
    painted->SetArea(0, 0, 120, 80);
    CHECK(painted->Init());
    painted->SetContext(&context);   // Init resets the draw context through the buffer pipeline
    painted->SetHeaderSizes(12, 30);
    painted->FitDrawArea();
    painted->SetColumnHeading(0, cRZString("One"));
    painted->SetRowHeading(0, cRZString("R0"));
    painted->SetCellColors(0, 0, 2, 0xff00ff00u);
    SetText(painted, 0, 0, "cell");
    CHECK(painted->GZPaint());
    CHECK(!context.log.empty());
    CHECK(painted->GetDefaultFont() != nullptr);

    // --- interface / reference counting ---
    void* out = nullptr;
    CHECK(grid->QueryInterface(kGZIID_cIGZWinGrid, &out) && out == static_cast<cIGZWinGrid*>(grid));
    static_cast<cIGZUnknown*>(out)->Release();
    CHECK(grid->QueryInterface(kGZIID_cIGZWin, &out) && out == static_cast<cGZWin*>(grid));
    static_cast<cIGZUnknown*>(out)->Release();
    CHECK(!grid->QueryInterface(0x1234, &out));
    CHECK(grid->AsIGZWin() == static_cast<cGZWin*>(grid));
    bool sawCellChanged = false;
    for (const cGZMessage& message : group.parent->got) if (message.type == kGZMsgGridCellChanged) sawCellChanged = true;
    CHECK(sawCellChanged);   // the mouse-down selection notified the target

    painted->Release();
    view->Release();
    sorted->Release();
    data->Release();
    grid->Release();
    if (failures == 0) std::printf("gz_wingrid: all checks passed\n");
    return failures ? 1 : 0;
}
