// cGZWinGrid implementation. Function addresses are the original's; the comments name the quirks and the places
// where the decompilation lost information (message construction, owner-draw rectangles).
#include "gz/WinGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "gz/RZDraw.h"
#include "platform/Unwired.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinFont.h"
#include "gz/WinScrollbar.h"

namespace gz {

namespace {

constexpr std::uint32_t kFormatUnset = 0x40000000;       // float 2.0 bits, the "not set" sentinel of every format field
constexpr std::uint8_t kFormatFlagUnset = 0xff;
constexpr std::uint8_t kFormatByteUnset = 0;
constexpr int kCellTypeWindow = 1, kCellTypeText = 2, kCellTypeBuffer = 4;
constexpr std::uint32_t kGridNumberHeaders = 0x100000;   // draw "Column #n" / "Row #n" when no heading is set
constexpr std::uint32_t kGridMsgCellHover = 0x9a1552d6;  // 0x100252930 sends the hover record to the target

IWinGridHost* g_gridHost = nullptr;

bool FormatColorSet(std::uint32_t color) { return color != kFormatUnset; }

// The two delete-mode tables (0x100a1ba80 columns, 0x100a1bab0 rows): mode -> how the remaining cells move.
int DeleteColumnAction(int mode) {
    static const int table[4] = {1, 2, 1, 4};
    const int index = mode == 0 ? 1 : mode - 1;
    return index >= 0 && index < 4 ? table[index] : 1;
}
int DeleteRowAction(int mode) {
    static const int table[5] = {1, 1, 3, 1, 5};
    const int index = mode == 0 ? 2 : mode - 1;
    return index >= 0 && index < 5 ? table[index] : 1;
}


}  // namespace

IWinGridHost* WinGridHost() { return g_gridHost; }
void SetWinGridHost(IWinGridHost* host) { g_gridHost = host; }

cGZWinGrid::cGZWinGrid(cIGZWinMgr* mgr, int flags) : cGZWin(mgr), flags_(static_cast<std::uint32_t>(flags)) {   // @ 0x10025086a
    // The three sparse matrices are default-constructed with the same limits the original writes:
    // rows/columns (-0x7fffffff..0x7fffffff), no cell, not initialised (0x10025086a at +0x238 / +0x268 / +0x298).
}

cGZWinGrid::~cGZWinGrid() {   // @ 0x100250c06
    cGZWinGrid::Shutdown();
    if (defaultFormat_.font) { defaultFormat_.font->Release(); defaultFormat_.font = nullptr; }
    if (headingFormat_.font) { headingFormat_.font->Release(); headingFormat_.font = nullptr; }
}

bool cGZWinGrid::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x100250e14
    if (!out) return false;
    if (iid == kGZIID_cIGZWinGrid) {
        *out = static_cast<cIGZWinGrid*>(this);
        AddRef();
        return true;
    }
    return cGZWin::QueryInterface(iid, out);
}

std::uint32_t cGZWinGrid::Release() {   // @ 0x100250e78 (Shutdown, then the deleting destructor)
    if (refCount_ > 1) return --refCount_;
    Shutdown();
    delete this;
    return 0;
}

bool cGZWinGrid::Init() {   // @ 0x100250f00
    if (!get_init_flag()) {
        cGZWin::Init();
        // The original asks RZ::LM() for its log level (vslot 0x38) and keeps 1 when the service is absent.
        logLevel_ = 1;
        SetDefaultFormatting();
        RecalculateAll();
    }
    return true;
}

void cGZWinGrid::SetDefaultFormatting() {   // @ 0x100250f5a
    if (!defaultFormat_.font) {
        IWinFont* font = WinFontSource() ? WinFontSource()->Font(kWinDefaultFontStyle) : nullptr;
        if (font) font->AddRef();
        defaultFormat_.font = font;
    }
    const std::uint32_t black = MakeFillColor(0, 0, 0);
    const std::uint32_t white = MakeFillColor(0xff, 0xff, 0xff);
    outlineColor_ = black;
    columnLineColor_ = 0xff000000;
    rowLineColor_ = 0xff000000;
    if (!FormatColorSet(defaultFormat_.colors[0])) defaultFormat_.colors[0] = black;
    if (!FormatColorSet(defaultFormat_.colors[1])) defaultFormat_.colors[1] = white;
    if (!FormatColorSet(defaultFormat_.colors[4])) defaultFormat_.colors[4] = white;
    if (!FormatColorSet(defaultFormat_.colors[5])) defaultFormat_.colors[5] = black;
    if (!FormatColorSet(defaultFormat_.colors[2])) defaultFormat_.colors[2] = MakeFillColor(0xc0, 0xc0, 0xc0);
    if (!FormatColorSet(defaultFormat_.colors[3])) defaultFormat_.colors[3] = white;
    if (defaultFormat_.flag == kFormatFlagUnset) defaultFormat_.flag = 1;
    if (defaultFormat_.halign == kFormatByteUnset) defaultFormat_.halign = 2;
    if (defaultFormat_.valign == kFormatByteUnset) defaultFormat_.valign = 5;
    if (defaultFormat_.border == kFormatByteUnset) defaultFormat_.border = 0;
    if (!FormatColorSet(defaultFormat_.borderColor)) defaultFormat_.borderColor = black;

    if (!headingFormat_.font) {
        IWinFont* font = WinFontSource() ? WinFontSource()->Font(kWinDefaultFontStyle) : nullptr;
        if (font) font->AddRef();
        headingFormat_.font = font;
    }
    if (!FormatColorSet(headingFormat_.colors[0])) headingFormat_.colors[0] = black;
    if (!FormatColorSet(headingFormat_.colors[1])) headingFormat_.colors[1] = white;
    if (!FormatColorSet(headingFormat_.colors[4])) headingFormat_.colors[4] = black;
    if (!FormatColorSet(headingFormat_.colors[5])) headingFormat_.colors[5] = MakeFillColor(0xdc, 0xdc, 0xff);
    if (!FormatColorSet(headingFormat_.colors[2])) headingFormat_.colors[2] = MakeFillColor(0x80, 0x80, 0x80);
    if (!FormatColorSet(headingFormat_.colors[3])) headingFormat_.colors[3] = MakeFillColor(0xdc, 0xdc, 0xdc);
    if (headingFormat_.flag == kFormatFlagUnset) headingFormat_.flag = 1;
    if (headingFormat_.halign == kFormatByteUnset) headingFormat_.halign = 2;
    if (headingFormat_.valign == kFormatByteUnset) headingFormat_.valign = 6;
    if (headingFormat_.border == kFormatByteUnset) headingFormat_.border = 0;
    if (!FormatColorSet(headingFormat_.borderColor)) headingFormat_.borderColor = black;
}

void cGZWinGrid::RecalculateAll() {   // @ 0x10025121c
    static int nAlreadyHere = 0;
    if (nAlreadyHere >= 2) return;
    ++nAlreadyHere;
    if (recalcPause_ < 1 && get_init_flag()) {
        cRZRect gridArea, cellArea;
        GetGridArea(gridArea);
        GetCellArea(cellArea);
        gridAreaWidth_ = gridArea.right - gridArea.left;
        gridAreaHeight_ = gridArea.bottom - gridArea.top;
        cellAreaWidth_ = cellArea.right - cellArea.left;
        cellAreaHeight_ = cellArea.bottom - cellArea.top;
        visibleColumns_ = 0.0f;
        int column = firstVisibleColumn_;
        int columnLimit = std::numeric_limits<int>::max();
        if (columnLimit_ > 0) columnLimit = columnLimit_;
        if (static_cast<std::int16_t>(flags_ & 0xffff) >= 0) {   // bounded columns: up to the last used one (vslot [50])
            cRZRect used{0, 0, 0, 0};
            GetCellUsedRange(used);
            if (used.right <= columnLimit) columnLimit = used.right + 1;
        }
        if (column < columnLimit) {
            int accumulated = 0;
            do {
                const int next = accumulated + GetColumnWidthInternal(column);
                if (cellAreaWidth_ < next) {
                    const float fraction = (static_cast<float>(cellAreaWidth_) - static_cast<float>(accumulated)) /
                                           (static_cast<float>(next) - static_cast<float>(accumulated));
                    if (std::fabs(fraction) > 0.05f) visibleColumns_ += fraction;
                    break;
                }
                visibleColumns_ += 1.0f;
                ++column;
                accumulated = next;
            } while (column < columnLimit);
        }
        visibleRows_ = 0.0f;
        int row = firstVisibleRow_;
        int rowLimit = std::numeric_limits<int>::max();
        if (rowLimit_ > 0) rowLimit = rowLimit_;
        if ((flags_ & kGridUnboundedRows) == 0) {   // bounded rows: up to the last used one (Mac i386 0x001ba6f8 calls vslot [50],
            cRZRect used{0, 0, 0, 0};                  // GetCellUsedRange; the visible range only ever grew by one row per pass)
            GetCellUsedRange(used);
            if (used.bottom <= rowLimit) rowLimit = used.bottom + 1;
        }
        if (row < rowLimit) {
            int accumulated = 0;
            do {
                const int next = accumulated + GetRowHeightInternal(row);
                if (cellAreaHeight_ < next) {
                    const float fraction = (static_cast<float>(cellAreaHeight_) - static_cast<float>(accumulated)) /
                                           (static_cast<float>(next) - static_cast<float>(accumulated));
                    if (std::fabs(fraction) > 0.05f) visibleRows_ += fraction;
                    break;
                }
                visibleRows_ += 1.0f;
                ++row;
                accumulated = next;
            } while (row < rowLimit);
        }
        const bool hadVertical = verticalScrollBar_ != nullptr;
        const bool hadHorizontal = horizontalScrollBar_ != nullptr;
        TestForPossibleScrollBarAdditionOrRemoval();
        if ((verticalScrollBar_ != nullptr) != hadVertical || (horizontalScrollBar_ != nullptr) != hadHorizontal) {
            RecalculateAll();   // the bars changed the cell area; the original recurses once
        }
        if (nAlreadyHere == 1) PlaceAllCellWindows();
    }
    recalculatePending_ = false;
    --nAlreadyHere;
}

bool cGZWinGrid::Shutdown() {   // @ 0x10025151e
    DisposeOfAllCellData();
    for (cIGZWinScrollbar** bar : {&verticalScrollBar_, &horizontalScrollBar_}) {
        if (!*bar) continue;
        if ((*bar)->AsIGZWin()->GetParentWin() == static_cast<cGZWin*>(this)) ChildRemove((*bar)->AsIGZWin());
        (*bar)->Release();
        *bar = nullptr;
    }
    for (IWinBuffer** image : {&verticalScrollImage_, &horizontalScrollImage_}) {
        if (*image) { (*image)->Release(); *image = nullptr; }
    }
    if (background_) { background_->Release(); background_ = nullptr; }
    if (ownerDrawTarget_) { ownerDrawTarget_->Release(); ownerDrawTarget_ = nullptr; }
    extraData_ = nullptr;   // the original does not release it either (0x10025151e)
    if (defaultFormat_.font) { defaultFormat_.font->Release(); defaultFormat_.font = nullptr; }
    if (headingFormat_.font) { headingFormat_.font->Release(); headingFormat_.font = nullptr; }
    columnHeadings_.clear();
    rowHeadings_.clear();
    columnExtraData_.clear();
    rowExtraData_.clear();
    cellWindows_.clear();
    columnFormats_.Shutdown();
    rowFormats_.Shutdown();
    cGZWin::Shutdown();
    return true;
}

void cGZWinGrid::DisposeOfAllCellData() {   // @ 0x100251626
    for (auto it = cells_.begin(); it != cells_.end(); ++it) {
        const rz::MatrixUserCell<CellData*> entry = *it;
        if (entry.value) DisposeOfCellData(entry.col, entry.row, entry.value, true);
    }
    // The original destroys the map nodes itself (0x100251626), so the matrix's "initialised" flag is irrelevant.
    cells_.SetLimits(rz::cRZSparseMatrix<CellData*>::kNoLimit, rz::cRZSparseMatrix<CellData*>::kLimitMax,
                     rz::cRZSparseMatrix<CellData*>::kNoLimit, rz::cRZSparseMatrix<CellData*>::kLimitMax);
    cells_.Shutdown();
}

bool cGZWinGrid::DoMessage(cGZMessage& message) {   // @ 0x100251710
    bool handled = false;
    if (message.type == kGZMsgCommand && message.param1 == kGridCmdScrollbar) {
        if (message.param2 == kGridHorizontalScrollBarId) {
            if (horizontalScrollBar_) {
                const int value = horizontalScrollBar_->GetValue();
                if (value != firstVisibleColumn_) {
                    firstVisibleColumn_ = value;
                    handled = true;
                }
            }
        } else if (message.param2 == kGridVerticalScrollBarId && verticalScrollBar_) {
            const int value = verticalScrollBar_->GetValue();
            if (value != firstVisibleRow_) {
                firstVisibleRow_ = value;
                handled = true;
            }
        }
    }
    if (handled) {
        recalculatePending_ = true;
        MarkInvalid();
    }
    return cGZWin::DoMessage(message);
}

bool cGZWinGrid::GetGridArea(cRZRect& out) {   // @ 0x1002517d4
    const int width = GetW(), height = GetH();
    out = cRZRect{0, 0, width, height};
    out.left = gutterLeft_;
    out.top = gutterTop_;
    out.right = width - gutterRight_;
    out.bottom = height - gutterBottom_;
    return true;
}

bool cGZWinGrid::GetCellArea(cRZRect& out) {   // @ 0x100251848
    const int width = GetW(), height = GetH();
    out = cRZRect{0, 0, width, height};
    const int left = gutterLeft_, top = gutterTop_;
    out.left = left;
    out.top = top;
    out.right = width - gutterRight_;
    out.bottom = height - gutterBottom_;
    if (flags_ & kGridColumnHeaders) out.top = top + columnHeaderHeight_;
    if (flags_ & kGridRowHeaders) out.left = left + rowHeaderWidth_;
    if (verticalScrollBar_) out.right -= verticalScrollBar_->AsIGZWin()->GetW();
    if (horizontalScrollBar_) out.bottom -= horizontalScrollBar_->AsIGZWin()->GetH();
    return true;
}

bool cGZWinGrid::GZOnCharacter(char character) {   // @ 0x100251928 (the original adds 8 to the character)
    return cGZWin::GZOnCharacter(static_cast<char>(character + 8));
}

bool cGZWinGrid::GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) {   // @ 0x100251940
    if (!IsEnabled()) return true;
    switch (key) {
        case 0x1b: {   // Escape: forward a key-down to the notification target
            cGZMessage message;
            message.type = kGZMsgKeyDown;
            message.param1 = key;
            message.param2 = modifiers;
            SendMsg(notify_, message);
            break;
        }
        case 0x21:   // Page Up
            if ((modifiers & 4) == 0) ScrollCellsVertically(-static_cast<int>(visibleRows_));
            else ScrollCellsHorizontally(-static_cast<int>(visibleColumns_));
            break;
        case 0x22:   // Page Down
            if ((modifiers & 4) == 0) ScrollCellsVertically(static_cast<int>(visibleRows_));
            else ScrollCellsHorizontally(static_cast<int>(visibleColumns_));
            break;
        case 0x23: {   // End
            cRZRect visible{0, 0, 0, 0};
            if ((flags_ & kGridUnboundedRows) == 0 || static_cast<std::int16_t>(flags_ & 0xffff) >= 0) {
                GetCellVisibleRange(visible);
            }
            if (static_cast<std::int16_t>(flags_ & 0xffff) < 0) {
                if (columnLimit_ > 0) ScrollToColumn(columnLimit_ - 1);
            } else {
                ScrollToColumn(visible.right);
            }
            if ((flags_ & kGridUnboundedRows) == 0) ScrollToRow(visible.bottom);
            else if (rowLimit_ > 0) ScrollToRow(rowLimit_ - 1);
            break;
        }
        case 0x24:   // Home
            ScrollToCell(0, 0);
            break;
        case 0x25: case 0x26: case 0x27: case 0x28:   // arrows
            DoArrowKey(key, modifiers);
            break;
        case 0x2d:   // Insert toggles the sort direction bit
            if ((flags_ & kGridSortDescending) == 0) flags_ |= kGridSortDescending;
            else flags_ &= ~kGridSortDescending;
            break;
        default:
            break;
    }
    // The original always sends its key notification (0x9a1552d5) with p1 = the grid, p2 = the key.
    cGZMessage notification;
    notification.type = kGZMsgGridKey;
    notification.param1 = reinterpret_cast<std::uint64_t>(static_cast<cIGZWinGrid*>(this));
    notification.param2 = key;
    SendMsg(notify_, notification);
    return true;
}

bool cGZWinGrid::DoArrowKey(std::uint32_t key, std::uint32_t modifiers) {   // @ 0x100251ba8
    // The cell the movement starts from: the freshly added selection entry, else (0, 1) as the original's
    // 0x100000000 literal. Ghidra dropped the message construction at 0x100251d20 (the grid notifies its target
    // before adding the cell); the recovered code keeps the selection and scroll behaviour.
    cRZRect visible{0, 0, 0, 0};
    GetCellVisibleRange(visible);
    CellCoordinates cell{0, 1};
    int row = 1, column = 0, columnPlus = 1;
    if (!selection_.empty()) {
        cell = selection_.back();
        column = cell.col;
        row = cell.row;
        columnPlus = column + 1;
    }
    int targetColumn = column, targetRow = row;
    bool moved = false;
    switch (key) {
        case 0x25:   // left
            if (column > 0) { targetColumn = column - 1; moved = true; }
            break;
        case 0x26:   // up
            if (row > 0) { targetRow = row - 1; moved = true; }
            break;
        case 0x27: {   // right
            // With no column limit and free horizontal scrolling the original moves unconditionally.
            if (columnLimit_ == -1 && static_cast<std::int16_t>(flags_ & 0xffff) < 0) {
                targetColumn = columnPlus;
                moved = true;
            } else {
                int last = columnLimit_ == -1 || columnLimit_ == 0 ? 1 : columnLimit_;
                if (column < last - 1) { targetColumn = column + 1; moved = true; }
            }
            break;
        }
        case 0x28: {   // down
            if (rowLimit_ == -1 && (flags_ & kGridUnboundedRows) != 0) {
                targetRow = row + 1;
                moved = true;
            } else {
                const int last = rowLimit_ == -1 || rowLimit_ == 0 ? 1 : rowLimit_;
                if (row < last - 1) { targetRow = row + 1; moved = true; }
            }
            break;
        }
        default:
            return false;
    }
    if (!moved) {
        PlaySoundA(2);   // the original plays the "can not move" sound (vslot 0x4e0 = PlaySoundA)
        return false;
    }
    if ((modifiers & 1) == 0 && !selection_.empty()) selection_.clear();
    AddCellToSelection(targetColumn, targetRow, true);
    switch (key) {
        case 0x25:
            if (columnLimit_ > targetColumn) ScrollToColumn(targetColumn);
            break;
        case 0x26:
            if (rowLimit_ > targetRow) ScrollToRow(targetRow);
            break;
        case 0x27:
            if (static_cast<float>(targetColumn) >= static_cast<float>(columnLimit_) + visibleColumns_) {
                ScrollToColumn(static_cast<int>((static_cast<float>(targetColumn) - visibleColumns_) + 1.0f));
            }
            break;
        case 0x28:
            if (static_cast<float>(targetRow) >= static_cast<float>(rowLimit_) + visibleRows_) {
                ScrollToRow(static_cast<int>((static_cast<float>(targetRow) - visibleRows_) + 1.0f));
            }
            break;
        default:
            break;
    }
    SetScrollBarValues();
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::GZOnKeyUp(std::uint32_t key, std::uint32_t modifiers) {   // @ 0x100251e8e
    if (key != 0x1b) return true;
    cGZMessage message;
    message.type = kGZMsgKeyUp;
    message.param1 = key;
    message.param2 = modifiers;
    return SendMsg(notify_, message);
}

void cGZWinGrid::SetScrollBarValues() {   // @ 0x100251f26
    if (verticalScrollBar_ && visibleRows_ > 0.0f) {
        int rowCount = rowLimit_;
        int lines = 0;
        if (rowLimit_ == -1 && (flags_ & kGridUnboundedRows) != 0) {
            verticalScrollBar_->SetMinMax(0, static_cast<int>(visibleRows_));
            lines = std::numeric_limits<int>::max();
            verticalScrollBar_->SetLineAndPageCount(static_cast<std::uint32_t>(lines),
                                                    static_cast<std::uint32_t>(lines));
        } else {
            if ((flags_ & kGridUnboundedRows) == 0) {
                cRZRect visible{0, 0, 0, 0};
                GetCellVisibleRange(visible);
                rowCount = visible.bottom + 1;
            }
            lines = rowCount - static_cast<int>(visibleRows_);
            if (lines < 0) lines = 0;
            verticalScrollBar_->SetMinMax(0, lines);
            const int pages = static_cast<int>(static_cast<float>(rowCount) / visibleRows_ + 1.0f);
            verticalScrollBar_->SetLineAndPageCount(static_cast<std::uint32_t>(lines),
                                                    static_cast<std::uint32_t>(pages));
        }
        verticalScrollBar_->SetValue(firstVisibleRow_);
    }
    if (!horizontalScrollBar_ || visibleColumns_ == 0.0f) return;
    int columnCount = columnLimit_;
    int lines = 0;
    if (columnLimit_ == -1 && static_cast<std::int16_t>(flags_ & 0xffff) < 0) {
        horizontalScrollBar_->SetMinMax(0, static_cast<int>(visibleColumns_));
        lines = std::numeric_limits<int>::max();
        horizontalScrollBar_->SetLineAndPageCount(static_cast<std::uint32_t>(lines),
                                                  static_cast<std::uint32_t>(lines));
    } else {
        if (static_cast<std::int16_t>(flags_ & 0xffff) >= 0) {
            cRZRect visible{0, 0, 0, 0};
            GetCellVisibleRange(visible);
            columnCount = visible.right + 1;
        }
        lines = columnCount - static_cast<int>(visibleColumns_);
        if (lines < 0) lines = 0;
        horizontalScrollBar_->SetMinMax(0, lines);
        const int pages = static_cast<int>(static_cast<float>(columnCount) / visibleColumns_ + 1.0f);
        horizontalScrollBar_->SetLineAndPageCount(static_cast<std::uint32_t>(lines),
                                                  static_cast<std::uint32_t>(pages));
    }
    horizontalScrollBar_->SetValue(firstVisibleColumn_);
}

bool cGZWinGrid::MakeDropDown() {   // @ 0x10025213a
    SetFlag(0x40000);
    return true;
}

bool cGZWinGrid::IsColumnWithinBounds(int column) {   // @ 0x1002563da (0 and -1 both count as "unlimited")
    if (columnLimit_ == -1 || columnLimit_ == 0) return true;
    return column >= 0 && column < columnLimit_;
}

bool cGZWinGrid::IsRowWithinBounds(int row) {   // @ 0x1002563fe
    if (rowLimit_ == -1 || rowLimit_ == 0) return true;
    return row >= 0 && row < rowLimit_;
}

bool cGZWinGrid::IsCellWithinBounds(int column, int row) {   // @ 0x100256422
    return IsColumnWithinBounds(column) && IsRowWithinBounds(row);
}

bool cGZWinGrid::DoCursorHitTest(int x, int y, CursorHitTestResult& result, CellCoordinates& cell) {   // @ 0x100252c94
    result = kCursorNothing;
    cell = CellCoordinates{0, 0};
    cRZRect gridArea, cellArea;
    GetGridArea(gridArea);
    if (x < gridArea.left || y < gridArea.top || gridArea.right < x || gridArea.bottom < y) {
        result = kCursorOutsideGrid;
        return true;
    }
    GetCellArea(cellArea);
    if (x >= cellArea.left && y >= cellArea.top && x <= cellArea.right && y <= cellArea.bottom) {
        result = kCursorCell;
    } else if ((flags_ & (kGridColumnHeaders | kGridRowHeaders)) == (kGridColumnHeaders | kGridRowHeaders)) {
        const bool inColumnHeader = y >= gutterTop_ && y < columnHeaderHeight_ + gutterTop_;
        const bool inRowHeader = x >= gutterLeft_ && x < rowHeaderWidth_ + gutterLeft_;
        if (inColumnHeader && !inRowHeader) result = kCursorColumnHeader;
        else if (inRowHeader && !inColumnHeader) result = kCursorRowHeader;
        else if (inColumnHeader && inRowHeader) result = kCursorCorner;
        if (result == kCursorCorner) return true;
    }
    // Find the cell under the cursor by walking the visible columns / rows.
    int count = static_cast<int>(visibleColumns_);
    if (count > 0) {
        int column = firstVisibleColumn_;
        const int last = column + count;
        while (column < last) {
            cRZRect rect;
            GetCellRect(column, 0, rect);
            if (x < rect.right) { cell.col = column; break; }
            ++column;
        }
    }
    count = static_cast<int>(visibleRows_);
    if (count > 0) {
        int row = firstVisibleRow_;
        const int last = row + count;
        while (row < last) {
            cRZRect rect;
            GetCellRect(0, row, rect);
            if (y < rect.bottom) { cell.row = row; break; }
            ++row;
        }
    }
    if (result == kCursorColumnHeader) cell.row = 0;
    else if (result == kCursorRowHeader) cell.col = 0;
    return result != kCursorNothing;
}

bool cGZWinGrid::GZOnSetFocus(cIGZWin* previous) {   // @ 0x100252ec4
    if ((flags_ & kGridFlag1) != 0 && !invalidatePending_) MarkInvalid();
    return cGZWin::GZOnSetFocus(previous);
}

bool cGZWinGrid::GZOnKillFocus(cIGZWin* next) {   // @ 0x100252f1a
    if ((flags_ & kGridFlag1) != 0 && !invalidatePending_) MarkInvalid();
    return cGZWin::GZOnKillFocus(next);
}

bool cGZWinGrid::GZOnMouseWheel(int, int, std::uint32_t, int delta) {   // @ 0x100252c3e
    ScrollCellsVertically(delta / 120);
    return true;
}

bool cGZWinGrid::GZOnMouseDownL(int x, int y, std::uint32_t modifiers) {   // @ 0x10025237e
    if (!IsEnabled()) return true;
    if (GZAmCapturing() && (flags_ & kGridCapture) != 0) {
        // The grid already owns the mouse: forward the click to the scrollbar under the cursor.
        cIGZWinScrollbar* bars[2] = {verticalScrollBar_, horizontalScrollBar_};
        for (cIGZWinScrollbar* bar : bars) {
            if (!bar) continue;
            int bx = x, by = y;
            bar->AsIGZWin()->WindowToWindowCoordinates(bar->AsIGZWin(), bx, by);
            if (bx >= 0 && by >= 0 && bx < bar->AsIGZWin()->GetW() && by < bar->AsIGZWin()->GetH()) {
                GZSetCapture();
                return true;
            }
        }
        return true;
    }
    bool doubleClick = false;
    if (!clickTimer_.IsTimerRunning() || clickTimer_.GetElapsedTimeU32() > 499) {
        clickTimer_.Restart();
    } else {
        clickTimer_.Reset();
        doubleClick = true;
    }
    CursorHitTestResult result = kCursorNothing;
    CellCoordinates cell{0, 0};
    DoCursorHitTest(x, y, result, cell);
    if (result == kCursorCell) {
        const std::uint32_t flags = modifiers & 7;
        if (flags == 0) {
            ClearSelection();
            SetSelection(cell.col, cell.row);
        } else if (flags == 2) {
            const bool selected = GetCellIsSelected(cell.col, cell.row);
            AddCellToSelection(cell.col, cell.row, !selected);
        } else if (flags == 1) {
            // Extend the selection to the rectangle between the first selected cell and the clicked one.
            if (selection_.empty()) {
                selection_.push_back(cell);
            } else {
                const CellCoordinates first = selection_.back();   // the most recent selection anchors the range
                const int col0 = std::min(first.col, cell.col), col1 = std::max(first.col, cell.col);
                const int row0 = std::min(first.row, cell.row), row1 = std::max(first.row, cell.row);
                for (int c = col0; c <= col1; ++c)
                    for (int r = row0; r <= row1; ++r) AddCellToSelection(c, r, true);
            }
        }
    } else if (result == kCursorColumnHeader) {
        ClearSelection();
        for (int r = static_cast<int>(visibleRows_) > 0 ? 0 : 0;
             r <= static_cast<int>(visibleRows_); ++r) {
            AddCellToSelection(cell.col, r, true);
        }
    } else if (result == kCursorRowHeader) {
        ClearSelection();
        for (int c = 0; c <= static_cast<int>(visibleColumns_); ++c) AddCellToSelection(c, cell.row, true);
    }
    // Mac i386 0x001be380: a double click on the same cell sends only the double-click notification; any other click
    // sends cell-changed and, when it hit a different cell, selection-changed (p2 = the cell).
    const bool changed = lastHitResult_ != result || lastHitColumn_ != cell.col || lastHitRow_ != cell.row;
    lastHitResult_ = result;
    lastHitColumn_ = cell.col;
    lastHitRow_ = cell.row;
    cGZMessage message;
    message.param1 = reinterpret_cast<std::uint64_t>(static_cast<cIGZWinGrid*>(this));
    if (!changed && doubleClick) {
        message.type = kGZMsgGridDoubleClick;
        message.param2 = reinterpret_cast<std::uint64_t>(&lastHitResult_);
        SendMsg(notify_, message);
    } else {
        message.type = kGZMsgGridCellChanged;
        message.param2 = reinterpret_cast<std::uint64_t>(&lastHitResult_);
        SendMsg(notify_, message);
        if (changed) {
            message.type = kGZMsgGridSelectionChanged;
            message.param2 = reinterpret_cast<std::uint64_t>(&cell);
            SendMsg(notify_, message);
            recalculatePending_ = true;
            MarkInvalid();
        }
    }
    if ((flags_ & kGridCapture) != 0) GZSetCapture();
    return true;
}

bool cGZWinGrid::GZOnMouseUpL(int x, int y, std::uint32_t) {   // @ 0x10025214e
    if (!IsEnabled()) return true;
    if (!GZAmCapturing()) return true;
    if ((flags_ & kGridCapture) == 0) return true;
    if (!IsPointInWindowWindowCoordinates(x, y)) return true;
    cIGZWinScrollbar* bars[2] = {verticalScrollBar_, horizontalScrollBar_};
    for (cIGZWinScrollbar* bar : bars) {
        if (!bar) continue;
        int bx = x, by = y;
        bar->AsIGZWin()->WindowToWindowCoordinates(bar->AsIGZWin(), bx, by);
        if (bx >= 0 && by >= 0 && bx < bar->AsIGZWin()->GetW() && by < bar->AsIGZWin()->GetH()) {
            GZSetCapture();
            return true;
        }
    }
    GZReleaseCapture();
    return true;
}

bool cGZWinGrid::GZOnMouseMove(int x, int y, std::uint32_t) {   // @ 0x100252930
    if (!GZAmCapturing() || (flags_ & kGridCapture) == 0) {
        CursorHitTestResult result = kCursorNothing;
        CellCoordinates cell{0, 0};
        DoCursorHitTest(x, y, result, cell);
        if (result != kCursorNothing &&
            (lastHoverResult_ != result || lastHoverColumn_ != cell.col || lastHoverRow_ != cell.row)) {
            cGZMessage message;
            message.type = kGridMsgCellHover;
            message.param1 = reinterpret_cast<std::uint64_t>(static_cast<cIGZWinGrid*>(this));
            message.param2 = reinterpret_cast<std::uint64_t>(&cell);
            message.param3 = static_cast<std::uint64_t>(result);
            SendMsg(notify_, message);
        }
        lastHoverResult_ = result;
        lastHoverColumn_ = cell.col;
        lastHoverRow_ = cell.row;
        return true;
    }
    CursorHitTestResult result = kCursorNothing;
    CellCoordinates cell{0, 0};
    DoCursorHitTest(x, y, result, cell);
    if (result != kCursorCell) return true;
    if (lastHitResult_ != kCursorCell || lastHitColumn_ != cell.col || lastHitRow_ != cell.row) {
        ClearSelection();
        AddCellToSelection(cell.col, cell.row, true);
    }
    lastHitResult_ = kCursorCell;
    lastHitColumn_ = cell.col;
    lastHitRow_ = cell.row;
    return true;
}

void cGZWinGrid::SetArea(int l, int t, int r, int b) {   // @ 0x100254b8c
    cGZWin::SetArea(l, t, r, b);
    if (get_init_flag()) {
        cRZRect gridArea, cellArea;
        GetGridArea(gridArea);
        GetCellArea(cellArea);
        gridAreaWidth_ = gridArea.right - gridArea.left;
        gridAreaHeight_ = gridArea.bottom - gridArea.top;
        cellAreaWidth_ = cellArea.right - cellArea.left;
        cellAreaHeight_ = cellArea.bottom - cellArea.top;
        recalculatePending_ = true;
        MarkInvalid();
    }
}

void cGZWinGrid::SetFlag(int flag) {   // @ 0x100254ce6
    if ((flags_ & static_cast<std::uint32_t>(flag)) == 0) SetFlags(static_cast<int>(flags_ | static_cast<std::uint32_t>(flag)));
}

void cGZWinGrid::ClearFlag(int flag) {   // @ 0x100254d04
    if ((flags_ & static_cast<std::uint32_t>(flag)) != 0) {
        SetFlags(static_cast<int>(flags_ & ~static_cast<std::uint32_t>(flag)));
    }
}

void cGZWinGrid::SetFlags(int flags) {   // @ 0x100254d34
    if (flags_ == static_cast<std::uint32_t>(flags)) return;
    const std::uint32_t changed = flags_ ^ static_cast<std::uint32_t>(flags);
    flags_ = static_cast<std::uint32_t>(flags);
    if ((changed & (kGridHorizontalScroll | kGridVerticalScroll)) != 0) {
        RemoveScrollBar((flags_ & kGridVerticalScroll) == 0, (flags_ & kGridHorizontalScroll) == 0);
        recalculatePending_ = true;
        MarkInvalid();
    }
    if ((changed & kGridCapture) != 0) {
        const bool capturing = GZAmCapturing();
        if ((flags_ & kGridCapture) == 0) {
            if (capturing) GZReleaseCapture();
        } else if (!capturing) {
            GZSetCapture();
        }
    }
}

std::uint32_t cGZWinGrid::SetFlag(std::uint32_t flag, bool on) {   // @ 0x100254c50
    if (flag == kWinEnabled) {
        if (verticalScrollBar_) verticalScrollBar_->AsIGZWin()->SetFlag(kWinEnabled, on);
        if (horizontalScrollBar_) horizontalScrollBar_->AsIGZWin()->SetFlag(kWinEnabled, on);
    }
    return cGZWin::SetFlag(flag, on);
}

bool cGZWinGrid::RemoveScrollBar(bool vertical, bool horizontal) {   // @ 0x100254de2
    bool removed = false;
    if (vertical && verticalScrollBar_) {
        ChildRemove(verticalScrollBar_->AsIGZWin());
        verticalScrollBar_->Release();
        verticalScrollBar_ = nullptr;
        removed = true;
    }
    if (horizontal && horizontalScrollBar_) {
        ChildRemove(horizontalScrollBar_->AsIGZWin());
        horizontalScrollBar_->Release();
        horizontalScrollBar_ = nullptr;
        removed = true;
    } else if (!removed) {
        return true;   // nothing was removed (the horizontal bar was not there)
    }
    recalculatePending_ = true;
    return true;
}

void cGZWinGrid::SetFillColor(std::uint32_t nativeColor) {   // @ 0x100254eac
    cGZWin::SetFillColor(nativeColor);
    if ((flags_ & kGridBackground) != 0 && !invalidatePending_) MarkInvalid();
}

void cGZWinGrid::SetDefaultFont(IWinFont* font) {   // @ 0x100254f00
    if (font == defaultFormat_.font) return;
    if (font) font->AddRef();
    if (defaultFormat_.font) defaultFormat_.font->Release();
    defaultFormat_.font = font;
    recalculatePending_ = true;
    MarkInvalid();
}

bool cGZWinGrid::SetDefaultFontColor(std::uint32_t color, int index) {   // @ 0x100254f80
    if (static_cast<std::uint32_t>(index) >= 6) return false;
    defaultFormat_.colors[index] = color;
    return true;
}

bool cGZWinGrid::GetDefaultFontColor(std::uint32_t& color, int index) {   // @ 0x100254f9c
    if (static_cast<std::uint32_t>(index) >= 6) return false;
    color = defaultFormat_.colors[index];
    return true;
}

bool cGZWinGrid::SetDefaultFontColorByColumns(std::uint32_t color, int index, int first, int count) {   // @ 0x100254fba
    if (static_cast<std::uint32_t>(index) >= 6) return false;
    for (int column = first; column < first + count; ++column) {
        CellFormatExtra* extra = nullptr;
        if (!columnFormats_.GetCellPtr(column, 0, &extra)) {
            columnFormats_.InsertCell(column, 0, CellFormatExtra{});
            columnFormats_.GetCellPtr(column, 0, &extra);
        }
        if (extra) extra->colors[index] = color;
    }
    return true;
}

bool cGZWinGrid::SetDefaultFontColorByRows(std::uint32_t color, int index, int first, int count) {   // @ 0x1002555ce
    if (static_cast<std::uint32_t>(index) >= 6) return false;
    for (int row = first; row < first + count; ++row) {
        CellFormatExtra* extra = nullptr;
        if (!rowFormats_.GetCellPtr(0, row, &extra)) {
            rowFormats_.InsertCell(0, row, CellFormatExtra{});
            rowFormats_.GetCellPtr(0, row, &extra);
        }
        if (extra) extra->colors[index] = color;
    }
    return true;
}

bool cGZWinGrid::SetDefaultTextWrapping(bool wrap) {   // @ 0x1002556a8
    if (IsFlagSet(kGridWrapText) != wrap) {
        if (wrap) SetFlag(kGridWrapText);
        else ClearFlag(kGridWrapText);
        recalculatePending_ = true;
        MarkInvalid();
    }
    return true;
}

bool cGZWinGrid::GetDefaultTextWrapping() {   // @ 0x10025570e
    return IsFlagSet(kGridWrapText);
}

bool cGZWinGrid::GetGutters(int& l, int& t, int& r, int& b) {   // @ 0x100255722
    l = gutterLeft_;
    t = gutterTop_;
    r = gutterRight_;
    b = gutterBottom_;
    return true;
}

bool cGZWinGrid::SetGutters(int l, int t, int r, int b) {   // @ 0x10025574c
    gutterLeft_ = l;
    gutterTop_ = t;
    gutterRight_ = r;
    gutterBottom_ = b;
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::GetCellGutters(int& l, int& t, int& r, int& b) {   // @ 0x100255792
    l = cellGutterLeft_;
    t = cellGutterTop_;
    r = cellGutterRight_;
    b = cellGutterBottom_;
    return true;
}

bool cGZWinGrid::SetCellGutters(int l, int t, int r, int b) {   // @ 0x1002557bc (no invalidation in the original)
    cellGutterLeft_ = l;
    cellGutterTop_ = t;
    cellGutterRight_ = r;
    cellGutterBottom_ = b;
    return true;
}

void cGZWinGrid::SetOutlineUse(bool on, std::uint32_t color) {   // @ 0x1002557de
    if (on) flags_ |= kGridOutline;
    else flags_ &= ~kGridOutline;
    outlineColor_ = color;
}

void cGZWinGrid::PauseRecalculations(bool pause) {   // @ 0x10025580a
    recalcPause_ += pause ? 1 : -1;
}

bool cGZWinGrid::ActLikeAListbox(int columnLimit, int selectionLimit, bool selectFirst) {   // @ 0x100255824
    flags_ = (flags_ & 0xfffc0000u) | 0x2ba6u;
    columnLimit_ = columnLimit;
    rowLimit_ = -1;
    selectionColumnRule_ = 1;
    selectionLimit_ = selectionLimit;
    if (selectFirst) {
        flags_ = (flags_ & 0xfffc0000u) | 0x3ba6u;
    } else if (selectionLimit < 1) {
        ClearSelection();
    } else {
        int column = 0, row = 0;
        if (!GetSelection(column, row)) {
            selection_.push_back(CellCoordinates{0, 0});
        }
    }
    return true;
}

bool cGZWinGrid::ScrollCellsVertically(int rows) {   // @ 0x1002558f0
    const int old = firstVisibleRow_;
    int settled = old;
    if (rowLimit_ == -1 && (flags_ & kGridUnboundedRows) != 0) {
        firstVisibleRow_ = std::max(0, old + rows);
        settled = firstVisibleRow_;
    } else {
        int limit = rowLimit_;
        int from = old;
        if ((flags_ & kGridUnboundedRows) == 0 || limit == -1) {
            cRZRect visible{0, 0, 0, 0};
            GetCellVisibleRange(visible);
            limit = visible.bottom + 1;
            from = firstVisibleRow_;
        }
        int wanted = limit - 1;
        if (from + rows < limit) wanted = from + rows;
        const int top = wanted < 0 ? 0 : wanted;
        firstVisibleRow_ = top;
        settled = 0;
        if (wanted >= 1 && top >= 0) {
            settled = wanted;
            for (;;) {
                if (cellAreaHeight_ <= 0) { settled = firstVisibleRow_; break; }
                int filled = 0;
                if (settled < limit) {
                    do {
                        filled += GetRowHeightInternal(settled);
                        if (cellAreaHeight_ <= filled) break;
                        ++settled;
                    } while (settled < limit);
                }
                settled = firstVisibleRow_;
                if (filled < cellAreaHeight_) { --settled; firstVisibleRow_ = settled; }
                if (settled < 0) { firstVisibleRow_ = 0; settled = 0; break; }
                if (cellAreaHeight_ <= filled) break;
            }
        }
        if (settled < 0) { firstVisibleRow_ = 0; settled = 0; }
    }
    if (old != settled) {
        MarkInvalid();
        recalculatePending_ = true;
    }
    return true;
}

bool cGZWinGrid::ScrollCellsHorizontally(int columns) {   // @ 0x100255a42
    const int old = firstVisibleColumn_;
    int settled = old;
    if (columnLimit_ == -1 && static_cast<std::int16_t>(flags_ & 0xffff) < 0) {
        firstVisibleColumn_ = std::max(0, old + columns);
        settled = firstVisibleColumn_;
    } else {
        int limit = columnLimit_;
        int from = old;
        if (static_cast<std::int16_t>(flags_ & 0xffff) >= 0 || limit == -1) {
            cRZRect visible{0, 0, 0, 0};
            GetCellVisibleRange(visible);
            limit = visible.right + 1;
            from = firstVisibleColumn_;
        }
        int wanted = limit - 1;
        if (from + columns < limit) wanted = from + columns;
        const int left = wanted < 0 ? 0 : wanted;
        firstVisibleColumn_ = left;
        settled = 0;
        if (wanted >= 1 && left >= 0) {
            settled = wanted;
            for (;;) {
                if (cellAreaWidth_ <= 0) { settled = firstVisibleColumn_; break; }
                int filled = 0;
                if (settled < limit) {
                    do {
                        filled += GetColumnWidthInternal(settled);
                        if (cellAreaWidth_ <= filled) break;
                        ++settled;
                    } while (settled < limit);
                }
                settled = firstVisibleColumn_;
                if (filled < cellAreaWidth_) { --settled; firstVisibleColumn_ = settled; }
                if (settled < 0) { firstVisibleColumn_ = 0; settled = 0; break; }
                if (cellAreaWidth_ <= filled) break;
            }
        }
        if (settled < 0) { firstVisibleColumn_ = 0; settled = 0; }
    }
    if (old != settled) {
        MarkInvalid();
        recalculatePending_ = true;
    }
    return true;
}

void cGZWinGrid::ScrollPagesVertically(int pages) {   // @ 0x100255b92
    ScrollCellsVertically(pages << 2);
}

void cGZWinGrid::ScrollPagesHorizontally(int pages) {   // @ 0x100255ba6
    ScrollCellsHorizontally(pages << 2);
}

bool cGZWinGrid::ScrollToColumn(int column) {   // @ 0x100255bba
    if (firstVisibleColumn_ == column) return true;
    int limit = columnLimit_;
    if (limit > 0 || static_cast<std::int16_t>(flags_ & 0xffff) >= 0) {
        if (static_cast<std::int16_t>(flags_ & 0xffff) >= 0) {
            cRZRect visible{0, 0, 0, 0};
            GetCellVisibleRange(visible);
            limit = visible.right + 1;
        }
        if (limit <= column) column = limit - 1;
    }
    firstVisibleColumn_ = column;
    if (!invalidatePending_) MarkInvalid();
    return true;
}

bool cGZWinGrid::ScrollToRow(int row) {   // @ 0x100255c50
    if (firstVisibleRow_ == row) return true;
    int limit = rowLimit_;
    if (limit > 0 || (flags_ & kGridUnboundedRows) == 0) {
        if ((flags_ & kGridUnboundedRows) == 0) {
            cRZRect visible{0, 0, 0, 0};
            GetCellVisibleRange(visible);
            limit = visible.bottom + 1;
        }
        if (limit <= row) row = limit - 1;
    }
    firstVisibleRow_ = row;
    if (!invalidatePending_) MarkInvalid();
    return true;
}

bool cGZWinGrid::ScrollToCell(int column, int row) {   // @ 0x100255cee
    ScrollToColumn(column);
    ScrollToRow(row);
    return true;
}

bool cGZWinGrid::GetSelectionRules(int& columnRule, int& selectionLimit) {   // @ 0x100255d1a
    // The second output is the selection limit (+0x210), not a row rule: ActLikeAListbox stores its second argument
    // there and SetSelection refuses to run when it is not positive.
    columnRule = selectionColumnRule_;
    selectionLimit = selectionLimit_;
    return true;
}

bool cGZWinGrid::SetSelectionRules(int columnRule, int selectionLimit) {   // @ 0x100255d32
    selectionColumnRule_ = columnRule;
    selectionLimit_ = selectionLimit;
    ClearSelection();
    return true;
}

bool cGZWinGrid::GetSelection(int& column, int& row) {   // @ 0x100255d5c (the oldest selection entry)
    if (selection_.empty()) return false;
    column = selection_.front().col;
    row = selection_.front().row;
    return true;
}

bool cGZWinGrid::GetSelection(CellCoordinates* cells, int& count) {   // @ 0x100255d8a (oldest first)
    const int size = static_cast<int>(selection_.size());
    if (size < count) count = size;
    if (cells && count > 0) {
        int index = 0;
        for (auto it = selection_.begin(); it != selection_.end() && index < count; ++it, ++index) cells[index] = *it;
    }
    return true;
}

bool cGZWinGrid::SetSelection(int column, int row) {   // @ 0x100255dcc
    if (selectionLimit_ <= 0) return false;
    if (GetCellIsSelected(column, row) && selection_.size() == 1) return false;
    ClearSelection();
    AddCellToSelection(column, row, true);
    if (!invalidatePending_) MarkInvalid();
    return true;
}

bool cGZWinGrid::AddCellToSelection(int column, int row, bool add) {   // @ 0x100255e62
    const bool selected = GetCellIsSelected(column, row);
    bool result = false;
    if (static_cast<unsigned char>(selected) != static_cast<unsigned char>(add)) {
        if (add) {
            if (static_cast<int>(selection_.size()) < selectionLimit_) {
                selection_.push_back(CellCoordinates{column, row});
                result = true;
            }
        } else if (selectionColumnRule_ == 1) {
            for (auto it = selection_.begin(); it != selection_.end();) {
                if (it->row == row) { it = selection_.erase(it); result = true; }
                else ++it;
            }
        } else if (selectionColumnRule_ == 0) {
            for (auto it = selection_.begin(); it != selection_.end();) {
                if (it->col == column) { it = selection_.erase(it); result = true; }
                else ++it;
            }
        } else {
            for (auto it = selection_.begin(); it != selection_.end();) {
                if (it->col == column && it->row == row) { it = selection_.erase(it); result = true; }
                else ++it;
            }
        }
    }
    if (!invalidatePending_) MarkInvalid();
    return result;
}

bool cGZWinGrid::GetCellIsSelected(int column, int row) {   // @ 0x100256040
    if (selectionColumnRule_ == 1) {
        for (const CellCoordinates& cell : selection_) if (cell.row == row) return true;
        return false;
    }
    if (selectionColumnRule_ == 0) {
        for (const CellCoordinates& cell : selection_) if (cell.col == column) return true;
        return false;
    }
    for (const CellCoordinates& cell : selection_) if (cell.col == column && cell.row == row) return true;
    return false;
}

bool cGZWinGrid::ClearSelection() {   // @ 0x1002560c6
    if (selection_.empty()) return true;
    selection_.clear();
    MarkInvalid();
    return true;
}

bool cGZWinGrid::EnableFixedRanges(bool columns, bool rows) {   // @ 0x100256110
    if (columns) SetFlag(kGridFixedColumns);
    else {
        ClearFlag(kGridFixedColumns);
        columnLimit_ = -1;
    }
    if (rows) SetFlag(kGridFixedRows);
    else {
        ClearFlag(kGridFixedRows);
        rowLimit_ = -1;
    }
    return true;
}

bool cGZWinGrid::SetCellLimits(int columns, int rows) {   // @ 0x100256174
    columnLimit_ = columns;
    rowLimit_ = rows;
    return true;
}

bool cGZWinGrid::GetCellLimits(int& columns, int& rows) {   // @ 0x100256188
    columns = columnLimit_;
    rows = rowLimit_;
    return true;
}

bool cGZWinGrid::GetCellUsedRange(cRZRect& out) {   // @ 0x1002561a0
    std::int32_t minColumn = rz::cRZSparseMatrix<CellData*>::kNoColumn;
    std::int32_t maxColumn = rz::cRZSparseMatrix<CellData*>::kNoColumn;
    cells_.GetMinUsedCol(minColumn);
    if (minColumn == rz::cRZSparseMatrix<CellData*>::kNoColumn) {
        out = cRZRect{0, 0, 0, 0};
        return false;
    }
    std::int32_t minRow = std::numeric_limits<std::int32_t>::max();
    std::int32_t maxRow = std::numeric_limits<std::int32_t>::min();
    for (auto it = cells_.begin(); it != cells_.end(); ++it) {
        const rz::MatrixUserCell<CellData*> entry = *it;
        minRow = std::min(minRow, entry.row);
        maxRow = std::max(maxRow, entry.row);
    }
    cells_.GetMaxUsedCol(maxColumn);
    out.left = minColumn;
    out.top = minRow;
    out.right = maxColumn;
    out.bottom = maxRow;
    return true;
}

bool cGZWinGrid::GetCellVisibleRange(cRZRect& out) {   // @ 0x10025639e
    out.left = firstVisibleColumn_;
    out.top = firstVisibleRow_;
    out.right = static_cast<int>(visibleColumns_) + firstVisibleColumn_;
    out.bottom = static_cast<int>(visibleRows_) + firstVisibleRow_;
    return true;
}

bool cGZWinGrid::GetMostRecentlyAddedCell(int& column, int& row) {   // @ 0x10025645a
    column = mostRecentColumn_;
    row = mostRecentRow_;
    return mostRecentColumn_ != -1;
}

bool cGZWinGrid::SetBackgroundColor(std::uint32_t native) {   // @ 0x100256476
    SetFillColor(native);
    return true;
}

bool cGZWinGrid::SetBackgroundBitmap(IWinBuffer* bitmap) {   // @ 0x100256488
    if (bitmap) bitmap->AddRef();
    if (background_) background_->Release();
    background_ = bitmap;
    return true;
}

bool cGZWinGrid::SetGridColors(int columnLineNative, int rowLineNative) {   // @ 0x1002564c4
    columnLineColor_ = ConvertNativeToPackedRGB(static_cast<std::uint32_t>(columnLineNative));
    rowLineColor_ = ConvertNativeToPackedRGB(static_cast<std::uint32_t>(rowLineNative));
    return true;
}

bool cGZWinGrid::GetGridColors(int& columnLine, int& rowLine) {   // @ 0x10003abc8
    columnLine = static_cast<int>(columnLineColor_);
    rowLine = static_cast<int>(rowLineColor_);
    return true;
}

bool cGZWinGrid::SetDefaultTextAlignment(int alignment) {   // @ 0x10025650e
    defaultTextAlignment_ = alignment;
    return true;
}

bool cGZWinGrid::SetDefaultTextAlignmentByColumns(int first, int count, int alignment) {   // @ 0x10025651c
    for (int column = first; column < first + count; ++column) {
        CellFormatExtra* extra = nullptr;
        if (!columnFormats_.GetCellPtr(column, 0, &extra)) {
            CellFormatExtra format;
            format.halign = static_cast<std::uint8_t>(alignment);
            columnFormats_.InsertCell(column, 0, format);
        } else {
            extra->halign = static_cast<std::uint8_t>(alignment);
        }
    }
    return true;
}

bool cGZWinGrid::SetDefaultTextAlignmentByRows(int first, int count, int alignment) {   // @ 0x1002565d4
    for (int row = first; row < first + count; ++row) {
        CellFormatExtra* extra = nullptr;
        if (!rowFormats_.GetCellPtr(0, row, &extra)) {
            CellFormatExtra format;
            format.halign = static_cast<std::uint8_t>(alignment);
            rowFormats_.InsertCell(0, row, format);
        } else {
            extra->halign = static_cast<std::uint8_t>(alignment);
        }
    }
    return true;
}

bool cGZWinGrid::SetColumnWidth(int first, int count, int width) {   // @ 0x100256698
    if (width < 1) width = 0;
    for (int column = first; column < first + count; ++column) {
        CellFormatExtra* extra = nullptr;
        if (!columnFormats_.GetCellPtr(column, 0, &extra)) {
            CellFormatExtra format;
            format.size = static_cast<std::int16_t>(width);
            format.actual = -1;
            columnFormats_.InsertCell(column, 0, format);
        } else {
            extra->size = static_cast<std::int16_t>(width);
        }
    }
    return true;
}

bool cGZWinGrid::SetRowHeight(int first, int count, int height) {   // @ 0x100256764
    if (height < 1) height = 0;
    for (int row = first; row < first + count; ++row) {
        CellFormatExtra* extra = nullptr;
        if (!rowFormats_.GetCellPtr(0, row, &extra)) {
            CellFormatExtra format;
            format.size = static_cast<std::int16_t>(height);
            format.actual = -1;
            rowFormats_.InsertCell(0, row, format);
        } else {
            extra->size = static_cast<std::int16_t>(height);
        }
    }
    return true;
}

int cGZWinGrid::GetColumnWidth(int first, int count) {   // @ 0x10025d328
    int total = 0;
    for (int column = first; column < first + count; ++column) total += GetColumnWidthInternal(column);
    return total;
}

int cGZWinGrid::GetRowHeight(int first, int count) {   // @ 0x10025d37a
    int total = 0;
    for (int row = first; row < first + count; ++row) total += GetRowHeightInternal(row);
    return total;
}

bool cGZWinGrid::GetColumnHeaderArea(int column, cRZRect& out) {   // @ 0x100256824
    if ((flags_ & kGridColumnHeaders) == 0) return false;
    if (column == -1) {
        const int width = GetW(), height = GetH();
        out = cRZRect{0, 0, width, height};
        out.left = gutterLeft_;
        out.top = gutterTop_;
        out.right = width - gutterRight_;
        out.bottom = gutterTop_ + columnHeaderHeight_;
        return true;
    }
    if (!GetCellRect(column, 0, out)) return false;
    out.top = gutterTop_;
    out.bottom = gutterTop_ + columnHeaderHeight_;
    return true;
}

bool cGZWinGrid::GetRowHeaderArea(int row, cRZRect& out) {   // @ 0x1002568d6
    if ((flags_ & kGridRowHeaders) == 0) return false;
    if (row == -1) {
        const int width = GetW(), height = GetH();
        out = cRZRect{0, 0, width, height};
        out.left = gutterLeft_;
        out.top = gutterTop_;
        out.right = gutterLeft_ + rowHeaderWidth_;
        out.bottom = height - gutterBottom_;
        return true;
    }
    if (!GetCellRect(0, row, out)) return false;
    out.left = gutterLeft_;
    out.right = gutterLeft_ + rowHeaderWidth_;
    return true;
}

bool cGZWinGrid::EnableHeaders(bool columns, bool rows) {   // @ 0x100256996 (the original answers false)
    if (columns) SetFlag(kGridColumnHeaders);
    else ClearFlag(kGridColumnHeaders);
    if (rows) SetFlag(kGridRowHeaders);
    else ClearFlag(kGridRowHeaders);
    return false;
}

bool cGZWinGrid::GetHeaderSizes(int& columnHeaderHeight, int& rowHeaderWidth) {   // @ 0x1002569e2
    columnHeaderHeight = columnHeaderHeight_;
    rowHeaderWidth = rowHeaderWidth_;
    return true;
}

bool cGZWinGrid::SetHeaderSizes(int columnHeaderHeight, int rowHeaderWidth) {   // @ 0x1002569fa
    columnHeaderHeight_ = columnHeaderHeight;
    rowHeaderWidth_ = rowHeaderWidth;
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::SetHeadingFormat(const CellFormat& format) {   // @ 0x100256b04
    if (format.font) format.font->AddRef();
    if (headingFormat_.font) headingFormat_.font->Release();
    headingFormat_ = format;
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::GetColumnHeading(int column, cIGZString& out) {   // @ 0x100256a9c
    out.AssignFromIGZString(columnHeadings_[column]);
    return true;
}

bool cGZWinGrid::SetColumnHeading(int column, const cIGZString& text) {   // @ 0x100256ad0
    columnHeadings_[column].AssignFromIGZString(text);
    return true;
}

bool cGZWinGrid::GetRowHeading(int row, cIGZString& out) {   // @ 0x100256a34
    out.AssignFromIGZString(rowHeadings_[row]);
    return true;
}

bool cGZWinGrid::SetRowHeading(int row, const cIGZString& text) {   // @ 0x100256a68
    rowHeadings_[row].AssignFromIGZString(text);
    return true;
}

bool cGZWinGrid::ClearAllRowHeadings() {   // @ 0x100256b9a
    rowHeadings_.clear();
    return true;
}

bool cGZWinGrid::ClearAllColumnHeadings() {   // @ 0x100256bdc
    columnHeadings_.clear();
    return true;
}

void cGZWinGrid::ApplyFormat(CellFormat& destination, const CellFormat& source) {
    if (source.font) destination.font = source.font;
    for (int i = 0; i < 6; ++i) if (FormatColorSet(source.colors[i])) destination.colors[i] = source.colors[i];
    if (source.flag != kFormatFlagUnset) destination.flag = source.flag;
    if (source.halign != kFormatByteUnset) destination.halign = source.halign;
    if (source.valign != kFormatByteUnset) destination.valign = source.valign;
    if (source.border != kFormatByteUnset) destination.border = source.border;
    if (FormatColorSet(source.borderColor)) destination.borderColor = source.borderColor;
}

bool cGZWinGrid::GetCellFormat(int column, int row, CellFormat& out) {   // @ 0x1002586c4
    out = defaultFormat_;
    CellFormatExtra* extra = nullptr;
    if (rowFormats_.GetCellPtr(0, row, &extra) && extra) ApplyFormat(out, *extra);
    if (columnFormats_.GetCellPtr(column, 0, &extra) && extra) ApplyFormat(out, *extra);
    if (CellData* cell = FindCell(column, row); cell && cell->format) ApplyFormat(out, *cell->format);
    return true;
}

void cGZWinGrid::GetColCellFormatExtra(int column, CellFormatExtra*& out) {   // @ 0x100259174
    out = nullptr;
    columnFormats_.GetCellPtr(column, 0, &out);
}

void cGZWinGrid::GetRowCellFormatExtra(int row, CellFormatExtra*& out) {   // @ 0x10025918e
    out = nullptr;
    rowFormats_.GetCellPtr(0, row, &out);
}

bool cGZWinGrid::GetColRowCellFormatExtra(int column, int row, CellFormatExtra*& out) {   // @ 0x1002591ac
    out = nullptr;
    if (columnFormats_.GetCellPtr(column, 0, &out) && out) return true;
    return rowFormats_.GetCellPtr(0, row, &out) && out;
}

cIGZWinGrid::CellData* cGZWinGrid::FindCell(int column, int row) {
    CellData** slot = nullptr;
    if (!cells_.GetCellPtr(column, row, &slot) || !slot) return nullptr;
    return *slot;
}

cIGZWinGrid::CellData** cGZWinGrid::FindCellSlot(int column, int row) {
    CellData** slot = nullptr;
    if (!cells_.GetCellPtr(column, row, &slot)) return nullptr;
    return slot;
}

cIGZWinGrid::CellData* cGZWinGrid::EnsureCell(int column, int row) {
    if (CellData* cell = FindCell(column, row)) return cell;
    CellData* created = new CellData();
    created->col = column;
    created->row = row;
    cells_.InsertCell(column, row, created);
    return created;
}

cIGZWinGrid::CellFormat* cGZWinGrid::EnsureCellFormat(CellData* cell) {
    if (!cell) return nullptr;
    if (!cell->format) cell->format = new CellFormat();
    return cell->format;
}

bool cGZWinGrid::SetCellFormat(int column, int row, const CellFormat& format) {   // @ 0x1002589d2
    if (column == -1 && row == -1) {
        if (format.font) format.font->AddRef();
        if (defaultFormat_.font) defaultFormat_.font->Release();
        defaultFormat_ = format;
        recalculatePending_ = true;
        MarkInvalid();
        return true;
    }
    if (column == -1 || row == -1) {
        const bool isRow = column == -1;
        rz::cRZSparseMatrix<CellFormatExtra>& matrix = isRow ? rowFormats_ : columnFormats_;
        const int key = isRow ? row : column;
        CellFormatExtra* extra = nullptr;
        if (!matrix.GetCellPtr(isRow ? 0 : column, isRow ? row : 0, &extra)) {
            matrix.InsertCell(isRow ? 0 : column, isRow ? row : 0, CellFormatExtra{});
            matrix.GetCellPtr(isRow ? 0 : column, isRow ? row : 0, &extra);
        }
        if (!extra) return false;
        if (format.font) format.font->AddRef();
        if (extra->font) extra->font->Release();
        static_cast<CellFormat&>(*extra) = format;
        extra->size = -1;
        extra->actual = -1;
        (void)key;
        recalculatePending_ = true;
        MarkInvalid();
        return true;
    }
    if (!IsCellWithinBounds(column, row)) return false;
    CellData* cell = EnsureCell(column, row);
    if (!cell) return false;
    if (!cell->format) {
        cell->format = new CellFormat(format);
        if (format.font) format.font->AddRef();
    } else {
        if (format.font) format.font->AddRef();
        if (cell->format->font) cell->format->font->Release();
        CellFormat copy = format;
        *cell->format = copy;
    }
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::ClearCellFormat(int column, int row) {   // @ 0x100259096
    if (CellData* cell = FindCell(column, row); cell && cell->format) {
        if (cell->format->font) cell->format->font->Release();
        delete cell->format;
        cell->format = nullptr;
        if (!cell->value) {
            // The original removes the entry but leaks the cell (RemoveCell with a null out pointer).
            if (cell->extraData && extraDataMode_ == 2) static_cast<cIGZUnknown*>(cell->extraData)->Release();
            CellData* removed = nullptr;
            cells_.RemoveCell(column, row, &removed);
            delete removed;
        }
        recalculatePending_ = true;
    }
    MarkInvalid();
    return true;
}

bool cGZWinGrid::SetCellFont(int column, int row, IWinFont* font) {   // @ 0x1002591fc
    if (!IsCellWithinBounds(column, row)) return false;
    CellData* cell = EnsureCell(column, row);
    CellFormat* format = EnsureCellFormat(cell);
    if (!format) return false;
    if (font) font->AddRef();
    if (format->font) format->font->Release();
    format->font = font;
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::SetCellColors(int column, int row, int index, std::uint32_t color) {   // @ 0x1002593b8
    if (!IsCellWithinBounds(column, row)) return false;
    CellFormat* format = EnsureCellFormat(EnsureCell(column, row));
    if (!format) return false;
    // No index check in the original either; the caller passes 0..5.
    format->colors[index] = color;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::SetCellWrapping(int column, int row, int wrapping) {   // @ 0x100259544
    if (!IsCellWithinBounds(column, row)) return false;
    CellFormat* format = EnsureCellFormat(EnsureCell(column, row));
    if (!format) return false;
    format->flag = static_cast<std::uint8_t>(wrapping);
    MarkInvalid();
    return true;
}

bool cGZWinGrid::SetCellAlignment(int column, int row, int hAlign, int vAlign) {   // @ 0x1002596c0
    if (!IsCellWithinBounds(column, row)) return false;
    CellFormat* format = EnsureCellFormat(EnsureCell(column, row));
    if (!format) return false;
    format->halign = static_cast<std::uint8_t>(hAlign);
    format->valign = static_cast<std::uint8_t>(vAlign);
    MarkInvalid();
    return true;
}

bool cGZWinGrid::SetCellBorder(int column, int row, int flags, int color) {   // @ 0x100259856
    if (!IsCellWithinBounds(column, row)) return false;
    CellFormat* format = EnsureCellFormat(EnsureCell(column, row));
    if (!format) return false;
    format->border = static_cast<std::uint8_t>(flags);
    format->borderColor = static_cast<std::uint32_t>(color);
    MarkInvalid();
    return true;
}

void cGZWinGrid::MarkDirty() { recalculatePending_ = true; }

void cGZWinGrid::MarkInvalid() {
    if (!invalidatePending_) {
        invalidatePending_ = true;
        InvalidateSelfAndParents();
    }
}

void cGZWinGrid::EmitNotification(std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) {
    cGZMessage message;
    message.type = type;
    message.param1 = p1;
    message.param2 = p2;
    message.param3 = p3;
    SendMsg(notify_, message);
}

bool cGZWinGrid::GetCellType(int column, int row, int& type) {   // @ 0x100256c1e
    CellData* cell = FindCell(column, row);
    if (!cell) return false;
    type = cell->type;
    return true;
}

bool cGZWinGrid::ClearCell(int column, int row) {   // @ 0x100256c54
    const bool wasSelected = GetCellIsSelected(column, row);
    CellData* removed = nullptr;
    const bool hadCell = cells_.RemoveCell(column, row, &removed);
    if (hadCell) {
        DisposeOfCellData(column, row, removed, true);
        recalculatePending_ = true;
    } else if (!wasSelected) {
        return true;
    }
    MarkInvalid();
    return true;
}

void cGZWinGrid::DisposeOfCellData(int column, int row, CellData* cell, bool freeCell) {   // @ 0x100256da6
    if (!cell) {
        cell = FindCell(column, row);
        if (!cell) return;
    }
    switch (cell->type) {
        case kCellTypeWindow:
            DoWindowRemoval(column, row, static_cast<cIGZWin*>(cell->value));
            cell->value = nullptr;
            break;
        case kCellTypeBuffer:
            if (cell->value) static_cast<IWinBuffer*>(cell->value)->Release();
            cell->value = nullptr;
            break;
        case kCellTypeText:
            if (cell->value) static_cast<cRZString*>(cell->value)->Release();
            cell->value = nullptr;
            break;
        default:
            break;
    }
    if (cell->format) {
        if (cell->format->font) cell->format->font->Release();
        delete cell->format;
        cell->format = nullptr;
    }
    if (cell->extraData && extraDataMode_ == 2) static_cast<cIGZUnknown*>(cell->extraData)->Release();
    cell->extraData = nullptr;
    if (freeCell) delete cell;
}

bool cGZWinGrid::ClearCellRect(int column, int row, int columns, int rows) {   // @ 0x100256ed4
    if (columns <= 0 || rows <= 0) return true;
    cRZRect visible{0, 0, 0, 0};
    GetCellVisibleRange(visible);
    const int firstColumn = std::max(column, static_cast<int>(visible.left));
    const int lastColumn = std::min(column + columns, static_cast<int>(visible.right) + 1);
    const int firstRow = std::max(row, static_cast<int>(visible.top));
    const int lastRow = std::min(row + rows, static_cast<int>(visible.bottom) + 1);
    bool cleared = false;
    for (int c = firstColumn; c < lastColumn; ++c) {
        for (int r = firstRow; r < lastRow; ++r) {
            if (IsCellWithinBounds(c, r)) {
                ClearCell(c, r);
                cleared = true;
            }
        }
    }
    if (cleared) MarkInvalid();
    return true;
}

bool cGZWinGrid::ClearAllCells() {   // @ 0x100256fd8
    cRZRect used;
    if (GetCellUsedRange(used)) {
        ClearCellRect(used.left, used.top, used.right - used.left + 1, used.bottom - used.top + 1);
        ClearSelection();
        MarkInvalid();
    }
    return true;
}

void cGZWinGrid::SetSizeCachedActualForColumn(int first, int count, int size) {   // @ 0x1002574fc
    int last = 0x7fffffff;
    if (columnLimit_ > 0) last = columnLimit_ - 1;
    if (count == 0x7fffffff) {
        count = last == 0x7fffffff ? 0x7fffffff - first : (1 - first) + last;
        if (size == -1) {
            for (auto it = columnFormats_.begin(); it != columnFormats_.end(); ++it) {
                rz::MatrixUserCell<CellFormatExtra> entry = *it;
                entry.value.actual = -1;
                columnFormats_.InsertCell(entry.col, entry.row, entry.value);
            }
            return;
        }
    }
    for (; count > 0; --count, ++first) {
        CellFormatExtra* extra = nullptr;
        if (!columnFormats_.GetCellPtr(first, 0, &extra)) {
            if (size == -1) continue;
            columnFormats_.InsertCell(first, 0, CellFormatExtra{});
            columnFormats_.GetCellPtr(first, 0, &extra);
        }
        if (extra) extra->actual = static_cast<std::int16_t>(size);
    }
}

void cGZWinGrid::SetSizeCachedActualForRow(int first, int count, int size) {   // @ 0x1002579dc
    int last = 0x7fffffff;
    if (rowLimit_ > 0) last = rowLimit_ - 1;
    if (count == 0x7fffffff) {
        count = last == 0x7fffffff ? 0x7fffffff - first : (1 - first) + last;
        if (size == -1) {
            for (auto it = rowFormats_.begin(); it != rowFormats_.end(); ++it) {
                rz::MatrixUserCell<CellFormatExtra> entry = *it;
                entry.value.actual = -1;
                rowFormats_.InsertCell(entry.col, entry.row, entry.value);
            }
            return;
        }
    }
    for (; count > 0; --count, ++first) {
        CellFormatExtra* extra = nullptr;
        if (!rowFormats_.GetCellPtr(0, first, &extra)) {
            if (size == -1) continue;
            rowFormats_.InsertCell(0, first, CellFormatExtra{});
            rowFormats_.GetCellPtr(0, first, &extra);
        }
        if (extra) extra->actual = static_cast<std::int16_t>(size);
    }
}

bool cGZWinGrid::DeleteColumns(int first, int count, int mode) {   // @ 0x100257062
    const int action = DeleteColumnAction(mode);
    if (count < 0) return false;
    if (count == 0) return true;
    if (first < 0) return false;
    const int lastColumn = first + count - 1;
    const int cells = cells_.GetCellCountForRange(first, lastColumn, std::numeric_limits<int>::min(),
                                                  std::numeric_limits<int>::max());
    std::vector<rz::MatrixUserCell<CellData*>> doomed(static_cast<std::size_t>(cells));
    cells_.GetCellRange(first, lastColumn, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
                        doomed.empty() ? nullptr : doomed.data());
    for (const auto& entry : doomed) DisposeOfCellData(entry.col, entry.row, entry.value, true);
    cells_.RemoveCol(first, count);
    if (action == 1) {
        SetSizeCachedActualForColumn(first, 1, -1);
    } else {
        if (action == 4) SetSizeCachedActualForColumn(0, first + 1, -1);
        else if (action == 2) SetSizeCachedActualForColumn(first, 0x7fffffff, -1);
        MakeRoomForColumn(first, -count, action);
    }
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::DeleteRows(int first, int count, int mode) {   // @ 0x1002577be
    const int action = DeleteRowAction(mode);
    if (count < 0) return false;
    if (count == 0) return true;
    if (first < 0) return false;
    const int lastRow = first + count - 1;
    const int cells = cells_.GetCellCountForRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
                                                  first, lastRow);
    std::vector<rz::MatrixUserCell<CellData*>> doomed(static_cast<std::size_t>(cells));
    cells_.GetCellRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), first, lastRow,
                        doomed.empty() ? nullptr : doomed.data());
    for (const auto& entry : doomed) DisposeOfCellData(entry.col, entry.row, entry.value, true);
    cells_.RemoveRow(first, count);
    if (action == 1) {
        SetSizeCachedActualForRow(first, 1, -1);
    } else {
        if (action == 5) SetSizeCachedActualForRow(0, first + 1, -1);
        else if (action == 3) SetSizeCachedActualForRow(first, 0x7fffffff, -1);
        MakeRoomForRow(first, -count, action);
    }
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::MakeRoomForColumn(int column, int delta, int mode) {   // @ 0x1002576f8
    const int action = mode ? mode : 2;
    if (action >= 5 || ((0x16 >> action) & 1) == 0) return false;
    cRZRect used;   // the original walks the used row range (GetCellUsedRange, slot 0x190)
    if (!GetCellUsedRange(used) || used.top > used.bottom) return false;
    for (int row = used.top; row <= used.bottom; ++row) {
        int minColumn = 0, maxColumn = 0;
        if (cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn)) MakeRoom(column, row, delta, action);
    }
    return true;
}

bool cGZWinGrid::MakeRoomForRow(int row, int delta, int mode) {   // @ 0x100257b2e
    const int action = mode ? mode : 3;
    if (action >= 6 || ((0x2a >> action) & 1) == 0) return false;
    cRZRect used;
    if (!GetCellUsedRange(used) || used.left > used.right) return false;
    for (int column = used.left; column <= used.right; ++column) {
        if (cells_.GetRowCountForCol(column) != 0) MakeRoom(column, row, delta, action);
    }
    return true;
}

bool cGZWinGrid::MakeRoom(int column, int row, int delta, int mode) {   // @ 0x100259eb4
    const int action = mode ? mode : 1;
    switch (action) {
        case 1:
            if (delta > 0) {
                SetSizeCachedActualForRow(row, 1, -1);
                CellData** slot = FindCellSlot(column, row);
                if (slot && *slot) {
                    DisposeOfCellData(column, row, *slot, true);
                    *slot = nullptr;
                }
            }
            break;
        case 2:
            if (delta < 1) {
                if (delta < 0) {
                    SetSizeCachedActualForRow(row, 1, -1);
                    int minColumn = 0, maxColumn = 0;
                    if (cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn) && column - delta <= maxColumn) {
                        int source = column - 1 - delta;
                        do {
                            ++source;
                            SetSizeCachedActualForColumn(source, 1, -1);
                            CopyCellData(source, row, column, row, false);
                            ++column;
                        } while (source < maxColumn);
                    }
                }
            } else {
                SetSizeCachedActualForRow(row, 1, -1);
                int minColumn = 0, maxColumn = 0;
                if (cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn)) {
                    int target = minColumn - delta;
                    if (target < 0) target = 0;
                    for (; target < column; ++target) {
                        SetSizeCachedActualForColumn(target, 1, -1);
                        CopyCellData(delta + target, row, target, row, false);
                    }
                }
            }
            break;
        case 3:
            if (delta < 1) {
                if (delta < 0) {
                    SetSizeCachedActualForRow(row, 1, -1);
                    int minRow = 0, maxRow = 0;
                    if (cells_.GetMinMaxUsedRowForCol(column, minRow, maxRow) && row - delta <= maxRow) {
                        int source = row - 1 - delta;
                        do {
                            ++source;
                            SetSizeCachedActualForRow(source, 1, -1);
                            CopyCellData(column, source, column, row, false);
                            ++row;
                        } while (source < maxRow);
                    }
                }
            } else {
                SetSizeCachedActualForRow(row, 1, -1);
                int minRow = 0, maxRow = 0;
                if (cells_.GetMinMaxUsedRowForCol(column, minRow, maxRow)) {
                    int target = minRow - delta;
                    if (target < 0) target = 0;
                    for (; target < row; ++target) {
                        SetSizeCachedActualForRow(target, 1, -1);
                        CopyCellData(column, delta + target, column, target, false);
                    }
                }
            }
            break;
        case 4:
            if (delta < 1) {
                if (delta < 0) {
                    SetSizeCachedActualForRow(row, 1, -1);
                    int minColumn = 0, maxColumn = 0;
                    if (cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn) && minColumn < column) {
                        for (;;) {
                            const int previous = column - 1;
                            SetSizeCachedActualForColumn(previous, 1, -1);
                            CopyCellData(previous, row, column + ~delta, row, false);
                            column = previous;
                            if (minColumn >= previous) break;
                        }
                    }
                }
            } else {
                SetSizeCachedActualForRow(row, 1, -1);
                int minColumn = 0, maxColumn = 0;
                if (cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn)) {
                    int high = maxColumn + delta;
                    if (columnLimit_ != -1 && columnLimit_ < high) high = columnLimit_;
                    for (; column <= high; --high) {
                        SetSizeCachedActualForColumn(high, 1, -1);
                        CopyCellData(high - delta, row, high, row, false);
                    }
                }
            }
            break;
        case 5:
            if (delta < 1) {
                if (delta < 0) {
                    SetSizeCachedActualForRow(row, 1, -1);
                    int minRow = 0, maxRow = 0;
                    if (cells_.GetMinMaxUsedRowForCol(column, minRow, maxRow) && minRow < row) {
                        for (;;) {
                            const int previous = row - 1;
                            SetSizeCachedActualForRow(previous, 1, -1);
                            CopyCellData(column, previous, column, row + ~delta, false);
                            row = previous;
                            if (minRow >= previous) break;
                        }
                    }
                }
            } else {
                SetSizeCachedActualForRow(row, 1, -1);
                int minRow = 0, maxRow = 0;
                if (cells_.GetMinMaxUsedRowForCol(column, minRow, maxRow)) {
                    int high = maxRow + delta;
                    if (rowLimit_ != -1 && rowLimit_ < high) high = rowLimit_;
                    for (; row <= high; --high) {
                        SetSizeCachedActualForRow(high, 1, -1);
                        CopyCellData(column, high - delta, column, high, false);
                    }
                }
            }
            break;
        default:
            return false;
    }
    recalculatePending_ = true;
    return true;
}

bool cGZWinGrid::CopyCellData(int sourceColumn, int sourceRow, int destColumn, int destRow, bool copy) {   // @ 0x100257f14
    if (!IsCellWithinBounds(destColumn, destRow)) return false;
    if (sourceColumn == destColumn && sourceRow == destRow) return true;
    CellData* source = nullptr;
    if (IsCellWithinBounds(sourceColumn, sourceRow)) {
        if (copy) source = FindCell(sourceColumn, sourceRow);
        else cells_.RemoveCell(sourceColumn, sourceRow, &source);
    }
    if (source) {
        CellData* dest = FindCell(destColumn, destRow);
        if (!dest) {
            dest = new CellData();
            cells_.InsertCell(destColumn, destRow, dest);
        }
        if (copy) {
            CopyCellData(sourceColumn, sourceRow, source, destColumn, destRow, dest);
        } else {
            DisposeOfCellData(destColumn, destRow, dest, false);
            *dest = *source;
            dest->col = destColumn;
            dest->row = destRow;
            if (dest->type == kCellTypeWindow) DoCellWindowMove(dest);
            delete source;
        }
        return true;
    }
    ClearCell(destColumn, destRow);
    return true;
}

void cGZWinGrid::CopyCellData(int, int, CellData* source, int destColumn, int destRow, CellData* dest) {   // @ 0x10025cbd6
    if (!source) return;
    if (dest) DisposeOfCellData(destColumn, destRow, dest, false);
    const int type = source->type;
    if (type == kCellTypeWindow) return;   // cell windows are not copied
    if (!dest) dest = new CellData();
    dest->type = type;
    dest->col = destColumn;
    dest->row = destRow;
    dest->value = nullptr;
    dest->format = nullptr;
    dest->extraData = nullptr;
    dest->windowFlags = source->windowFlags;
    if (type == kCellTypeBuffer) {
        dest->value = source->value;
        if (dest->value) static_cast<IWinBuffer*>(dest->value)->AddRef();
    } else if (type == kCellTypeText) {
        dest->value = source->value ? new cRZString(*static_cast<cRZString*>(source->value)) : nullptr;
    }
    if (source->format) {
        dest->format = new CellFormat(*source->format);
        if (dest->format->font) dest->format->font->AddRef();
    }
}

bool cGZWinGrid::CopyColumns(int destination, int source, int count, bool copy) {   // @ 0x100257bc8
    if (destination == source) return true;
    int dest = destination, src = source;
    int step = 1;
    const int destEnd = destination + count, sourceEnd = source + count;
    const bool overlap = (sourceEnd <= destination || destEnd < sourceEnd) && source < destEnd && destination < sourceEnd;
    if (overlap) {
        const bool sourceBefore = source < destEnd;
        dest = destEnd - 1;
        src = sourceEnd - 1;
        if (destEnd > sourceEnd || !sourceBefore) { dest = 0; src = 0; }
        step = (destEnd <= sourceEnd && sourceBefore) ? -1 : 1;
    }
    const int stop = (count - 1) * step + destination;
    for (;;) {
        int minRow = 0, maxRow = 0;
        if (cells_.GetMinMaxUsedRowForCol(src, minRow, maxRow)) {
            for (int row = minRow; row <= maxRow; ++row) {
                SetSizeCachedActualForRow(row, 1, -1);
                CopyCellData(src, row, dest, row, !copy);
            }
        }
        auto heading = columnHeadings_.find(src);
        if (heading != columnHeadings_.end()) {
            columnHeadings_[dest] = heading->second;
            if (copy) columnHeadings_.erase(heading);
        } else if (copy) {
            columnHeadings_.erase(dest);
        }
        auto extra = columnExtraData_.find(src);
        if (extra != columnExtraData_.end()) {
            columnExtraData_[dest] = extra->second;
            if (copy) columnExtraData_.erase(extra);
        }
        if (dest == stop) break;
        dest += step;
        src += step;
    }
    recalculatePending_ = true;
    return true;
}

bool cGZWinGrid::CopyRows(int destination, int source, int count, bool copy) {   // @ 0x1002581c4
    if (destination == source) return true;
    int dest = destination, src = source;
    int step = 1;
    const int destEnd = destination + count, sourceEnd = source + count;
    const bool overlap = (sourceEnd <= destination || destEnd < sourceEnd) && source < destEnd && destination < sourceEnd;
    if (overlap) {
        const bool sourceBefore = source < destEnd;
        dest = destEnd - 1;
        src = sourceEnd - 1;
        if (destEnd > sourceEnd || !sourceBefore) { dest = 0; src = 0; }
        step = (destEnd <= sourceEnd && sourceBefore) ? -1 : 1;
    }
    const int stop = (count - 1) * step + destination;
    for (;;) {
        int minColumn = 0, maxColumn = 0;
        if (cells_.GetMinMaxUsedColForRow(src, minColumn, maxColumn)) {
            for (int column = minColumn; column <= maxColumn; ++column) {
                SetSizeCachedActualForColumn(column, 1, -1);
                CopyCellData(column, src, column, dest, !copy);
            }
        }
        auto heading = rowHeadings_.find(src);
        if (heading != rowHeadings_.end()) {
            rowHeadings_[dest] = heading->second;
            if (copy) rowHeadings_.erase(heading);
        } else if (copy) {
            rowHeadings_.erase(dest);
        }
        auto extra = rowExtraData_.find(src);
        if (extra != rowExtraData_.end()) {
            rowExtraData_[dest] = extra->second;
            if (copy) rowExtraData_.erase(extra);
        }
        if (dest == stop) break;
        dest += step;
        src += step;
    }
    recalculatePending_ = true;
    return true;
}

bool cGZWinGrid::IsCellVisible(int column, int row) {   // @ 0x10025845a
    if (column < firstVisibleColumn_) return false;
    if (static_cast<float>(firstVisibleColumn_) + visibleColumns_ <= static_cast<float>(column)) return false;
    if (row < firstVisibleRow_) return false;
    return static_cast<float>(row) < static_cast<float>(firstVisibleRow_) + visibleRows_;
}

bool cGZWinGrid::MakeCellVisible(int column, int row) {   // @ 0x1002584aa
    if (recalculatePending_) RecalculateAll();
    if (!IsCellVisible(column, row)) {
        ScrollCellsVertically(row - firstVisibleRow_);
        if (!IsCellVisible(column, row)) ScrollCellsHorizontally(column - firstVisibleColumn_);
    }
    return true;
}

bool cGZWinGrid::GetCellRect(int column, int row, cRZRect& out) {   // @ 0x100258532
    int left = GetColumnWidth(0, column);
    out.left = left;
    out.right = left + GetColumnWidthInternal(column);
    int top = GetRowHeight(0, row);
    out.top = top;
    const int height = GetRowHeightInternal(row);
    out.bottom = top + height;
    if (firstVisibleColumn_ != 0) {
        const int offset = GetColumnWidth(0, firstVisibleColumn_);
        out.left -= offset;
        out.right -= offset;
    }
    if (firstVisibleRow_ != 0) {
        const int offset = GetRowHeight(0, firstVisibleRow_);
        out.top -= offset;
        out.bottom -= offset;
    }
    cRZRect cellArea;
    GetCellArea(cellArea);
    out.left += cellArea.left;
    out.top += cellArea.top;
    out.right += cellArea.left;
    out.bottom += cellArea.top;
    return true;
}

bool cGZWinGrid::GetFirstVisibleCell(int& column, int& row) {   // @ 0x10025864a
    column = firstVisibleColumn_;
    row = firstVisibleRow_;
    return true;
}

bool cGZWinGrid::SetFirstVisibleCell(int column, int row) {   // @ 0x100258662
    if (firstVisibleColumn_ == column && firstVisibleRow_ == row) return true;
    firstVisibleColumn_ = column;
    firstVisibleRow_ = row;
    recalculatePending_ = true;
    MarkInvalid();
    return true;
}

bool cGZWinGrid::GetVisibleColumnAndRowCount(float& columns, float& rows) {   // @ 0x1002586ac
    columns = visibleColumns_;
    rows = visibleRows_;
    return true;
}

bool cGZWinGrid::GetCellTextValue(int column, int row, cIGZString& out) {   // @ 0x1002599e6
    CellData* cell = FindCell(column, row);
    if (cell && cell->type == kCellTypeText && cell->value) {
        out.AssignFromChars(static_cast<cRZString*>(cell->value)->Data(), static_cast<cRZString*>(cell->value)->Strlen());
        return true;
    }
    cIGZWin* window = nullptr;
    if (GetCellWindow(column, row, window) && window && WinGridHost() && WinGridHost()->GetCellWindowText(window, out)) {
        return true;
    }
    return false;
}

bool cGZWinGrid::SetCellTextValue(int column, int row, const char* text, std::uint32_t length, int mode,
                                  bool makeTextEdit) {   // @ 0x100259ab8
    const int roomMode = mode ? mode : 1;
    CellData* existing = (column != 0x7fffffff && row != 0x7fffffff) ? FindCell(column, row) : nullptr;
    const bool haveCell = existing != nullptr;
    if (makeTextEdit) {
        if (existing && existing->type == kCellTypeWindow && existing->value && WinGridHost()) {
            if (WinGridHost()->SetCellWindowText(static_cast<cIGZWin*>(existing->value), text, length)) return true;
        }
        ClearCell(column, row);
        cIGZWin* edit = WinGridHost() ? WinGridHost()->CreateCellTextEdit(mgr_) : nullptr;
        if (!edit) return true;
        if (WinGridHost()) WinGridHost()->SetCellWindowText(edit, text, length);
        SetCellWindow(column, row, edit, true, true, 1);
        edit->Release();
        return true;
    }
    if (haveCell) ClearCell(column, row);
    if (column == 0x7fffffff && row == 0x7fffffff) {
        cells_.GetMaxUsedCol(column);
        if (column == rz::cRZSparseMatrix<CellData*>::kNoColumn) column = 0;
    }
    if (column == 0x7fffffff) {
        int minColumn = 0, maxColumn = 0;
        column = cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn) ? maxColumn + 1 : 0;
    }
    if (row == 0x7fffffff) {
        int minRow = 0, maxRow = 0;
        row = cells_.GetMinMaxUsedRowForCol(column, minRow, maxRow) ? maxRow + 1 : 0;
    }
    if (!IsCellWithinBounds(column, row)) {
        ClearCell(column, row);
        return true;
    }
    if (!text) {
        ClearCell(column, row);
        mostRecentColumn_ = column;
        mostRecentRow_ = row;
        return true;
    }
    const std::uint32_t used = (length == 0xffffffffu || length == 0) ? static_cast<std::uint32_t>(std::strlen(text))
                                                                      : length;
    cRZString* string = new cRZString();
    string->AssignFromChars(text, used);
    MakeRoom(column, row, 1, roomMode);
    CellData* cell = new CellData();
    cell->type = kCellTypeText;
    cell->col = column;
    cell->row = row;
    cell->value = string;
    cells_.InsertCell(column, row, cell);
    recalculatePending_ = true;
    MarkInvalid();
    mostRecentColumn_ = column;
    mostRecentRow_ = row;
    return true;
}

bool cGZWinGrid::GetCellBitmap(int column, int row, IWinBuffer*& out) {   // @ 0x10025a3f4
    CellData* cell = FindCell(column, row);
    if (!cell || cell->type != kCellTypeBuffer) return false;
    out = static_cast<IWinBuffer*>(cell->value);
    return true;
}

bool cGZWinGrid::SetCellBitmap(int column, int row, IWinBuffer* bitmap, int mode) {   // @ 0x10025a432
    const int roomMode = mode ? mode : 1;
    if (column == 0x7fffffff && row == 0x7fffffff) {
        cells_.GetMaxUsedCol(column);
        if (column == rz::cRZSparseMatrix<CellData*>::kNoColumn) column = 0;   // empty grid
    }
    if (column == 0x7fffffff) {
        int minColumn = 0, maxColumn = 0;
        column = cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn) ? maxColumn + 1 : 0;
    }
    if (row == 0x7fffffff) {
        int minRow = 0, maxRow = 0;
        row = cells_.GetMinMaxUsedRowForCol(column, minRow, maxRow) ? maxRow + 1 : 0;
    }
    if (!IsCellWithinBounds(column, row)) {
        ClearCell(column, row);
        return false;
    }
    if (!bitmap) {
        ClearCell(column, row);
    } else {
        bitmap->AddRef();
        MakeRoom(column, row, 1, roomMode);
        CellData* cell = new CellData();
        cell->type = kCellTypeBuffer;
        cell->col = column;
        cell->row = row;
        cell->value = bitmap;
        cells_.InsertCell(column, row, cell);
        recalculatePending_ = true;
        MarkInvalid();
    }
    mostRecentColumn_ = column;
    mostRecentRow_ = row;
    return true;
}

bool cGZWinGrid::GetCellWindow(int column, int row, cIGZWin*& out) {   // @ 0x10025a5e6
    CellData* cell = FindCell(column, row);
    if (!cell || cell->type != kCellTypeWindow) return false;
    out = static_cast<cIGZWin*>(cell->value);
    return true;
}

bool cGZWinGrid::SetCellWindow(int column, int row, cIGZWin* window, bool fitWidth, bool fitHeight, int mode) {   // @ 0x10025a624
    const int roomMode = mode ? mode : 1;
    if (window && IsWindowACellWindow(window, 0, nullptr, nullptr)) return false;
    int destRow = row;
    if (column == 0x7fffffff && destRow == 0x7fffffff) {
        cells_.GetMaxUsedCol(column);
        if (column == rz::cRZSparseMatrix<CellData*>::kNoColumn) column = 0;
    }
    if (column == 0x7fffffff) {
        int minColumn = 0, maxColumn = 0;
        column = cells_.GetMinMaxUsedColForRow(destRow, minColumn, maxColumn) ? maxColumn + 1 : 0;
    }
    if (destRow == 0x7fffffff) {
        int minRow = 0, maxRow = 0;
        destRow = cells_.GetMinMaxUsedRowForCol(column, minRow, maxRow) ? maxRow + 1 : 0;
    }
    if (!IsCellWithinBounds(column, destRow)) {
        ClearCell(column, destRow);
        return false;
    }
    if (!window) {
        ClearCell(column, destRow);
    } else {
        window->AddRef();
        MakeRoom(column, destRow, 1, roomMode);
        CellData* cell = new CellData();
        cell->type = kCellTypeWindow;
        cell->col = column;
        cell->row = destRow;
        cell->value = window;
        if (fitWidth || fitHeight) cell->windowFlags = static_cast<std::uint32_t>(fitWidth) | (fitHeight ? 2u : 0u);
        cells_.InsertCell(column, destRow, cell);
        DoWindowAddition(column, destRow, window, fitWidth, fitHeight);
        window->Release();
        recalculatePending_ = true;
        MarkInvalid();
    }
    mostRecentColumn_ = column;
    mostRecentRow_ = destRow;
    return true;
}

void cGZWinGrid::DoWindowAddition(int column, int row, cIGZWin* window, bool fitWidth, bool fitHeight) {   // @ 0x10025a884
    window->AddRef();
    window->SendToBack();
    window->SetFlag(0x800, false);
    ChildAdd(window);
    if (cellWindows_.find(window) == cellWindows_.end()) {
        CellWindow record;
        record.type = 1;
        record.column = column;
        record.row = row;
        record.window = window;
        cellWindows_[window] = record;
    }
    PlaceCellWindow(column, row, window, fitWidth, fitHeight);
}

bool cGZWinGrid::IsWindowACellWindow(cIGZWin* window, std::uint32_t id, int* column, int* row) {   // @ 0x10025a9c8
    for (auto& entry : cellWindows_) {
        const CellWindow& record = entry.second;
        bool match;
        if (!window) match = record.window && record.window->GetID() == id;
        else match = record.window == window;
        if (match) {
            if (column) *column = record.column;
            if (row) *row = record.row;
            return true;
        }
    }
    return false;
}

void cGZWinGrid::PlaceCellWindow(int column, int row, cIGZWin* window, bool fitWidth, bool fitHeight) {   // @ 0x10025ae02
    if (!get_init_flag() || !window) return;
    cRZRect visible{0, 0, 0, 0};
    GetCellVisibleRange(visible);
    if (column < visible.left || row < visible.top || visible.right < column || visible.bottom < row) {
        window->HideWindow();
        return;
    }
    cRZRect rect;
    GetCellRect(column, row, rect);
    rect.left += cellGutterLeft_;
    rect.top += cellGutterTop_;
    rect.right -= cellGutterRight_;
    rect.bottom -= cellGutterBottom_;
    window->ShowWindow();
    window->GZWinMoveTo(rect.left, rect.top);
    if (fitWidth) window->SetW(rect.right - rect.left);
    if (fitHeight) window->SetH(rect.bottom - rect.top);
    window->SetArea(rect.left, rect.top, rect.right, rect.bottom);
}

void cGZWinGrid::DoWindowRemoval(int, int, cIGZWin* window) {   // @ 0x10025af30
    if (!window) return;
    auto it = cellWindows_.find(window);
    if (it == cellWindows_.end()) return;
    cellWindows_.erase(it);
    ChildRemove(window);
    window->Release();
}

void cGZWinGrid::PlaceAllCellWindows() {   // @ 0x10025afa6
    if (!get_init_flag()) return;
    cRZRect visible{0, 0, 0, 0};
    GetCellVisibleRange(visible);
    for (auto& entry : cellWindows_) {
        CellWindow& record = entry.second;
        if (record.type != 1) continue;
        CellData* cell = FindCell(record.column, record.row);
        if (!cell || !cell->value) continue;
        cIGZWin* window = static_cast<cIGZWin*>(cell->value);
        if (record.column < visible.left || record.row < visible.top || visible.right < record.column ||
            visible.bottom < record.row) {
            window->HideWindow();
            continue;
        }
        cRZRect rect;
        GetCellRect(record.column, record.row, rect);
        rect.left += cellGutterLeft_;
        rect.top += cellGutterTop_;
        rect.right -= cellGutterRight_;
        rect.bottom -= cellGutterBottom_;
        window->ShowWindow();
        window->GZWinMoveTo(rect.left, rect.top);
        if ((cell->windowFlags & 1) != 0) window->SetW(rect.right - rect.left);
        if ((cell->windowFlags & 2) != 0) window->SetH(rect.bottom - rect.top);
    }
}

void cGZWinGrid::DoCellWindowMove(CellData* cell) {   // @ 0x10025b156
    if (!cell || cell->type != kCellTypeWindow) return;
    auto it = cellWindows_.find(static_cast<cIGZWin*>(cell->value));
    if (it == cellWindows_.end()) return;
    it->second.column = cell->col;
    it->second.row = cell->row;
}

void cGZWinGrid::ResizeColumnToFitCellWindows(int column, int count) {   // @ 0x10025aa82
    bool changed = false;
    for (int c = column; c < column + count; ++c) {
        int widest = -1;
        for (auto it = cells_.ColBegin(c); it != cells_.ColEnd(c); ++it) {
            rz::MatrixUserCell<CellData*> entry = *it;
            if (!entry.value || entry.value->type != kCellTypeWindow || !entry.value->value) continue;
            const cIGZWin* window = static_cast<cIGZWin*>(entry.value->value);
            widest = std::max(widest, window->GetW());
        }
        if (widest == -1) continue;
        CellFormatExtra* extra = nullptr;
        if (!columnFormats_.GetCellPtr(c, 0, &extra)) {
            columnFormats_.InsertCell(c, 0, CellFormatExtra{});
            columnFormats_.GetCellPtr(c, 0, &extra);
        }
        if (!extra) continue;
        if (extra->size == -1 || extra->size < widest) {
            extra->size = static_cast<std::int16_t>(widest);
            changed = true;
        }
    }
    if (changed) {
        SetSizeCachedActualForRow(0, 0x7fffffff, -1);
        recalculatePending_ = true;
        MarkInvalid();
    }
}

bool cGZWinGrid::SetCellExtraData(int column, int row, void* data) {   // @ 0x10025b392
    extraDataMode_ = 1;
    if (!IsCellWithinBounds(column, row)) return false;
    CellData* cell = FindCell(column, row);
    if (!cell) {
        if (!data) return true;
        cell = new CellData();
        cell->col = column;
        cell->row = row;
        cell->extraData = data;
        cells_.InsertCell(column, row, cell);
        return true;
    }
    if (!cell) return false;
    cell->extraData = data;
    return true;
}

void* cGZWinGrid::GetCellExtraData(int column, int row) {   // @ 0x10025b44a
    CellData* cell = FindCell(column, row);
    if (!cell) return nullptr;
    return cell->extraData;
}

bool cGZWinGrid::SetColumnExtraData(int column, void* data) {   // @ 0x10025b480
    extraDataMode_ = 1;
    if (!IsColumnWithinBounds(column)) return false;
    columnExtraData_[column] = data;
    return true;
}

void* cGZWinGrid::GetColumnExtraData(int column) {   // @ 0x10025b4d4 (the original creates a null entry)
    return columnExtraData_[column];
}

bool cGZWinGrid::SetRowExtraData(int row, void* data) {   // @ 0x10025b4fa
    extraDataMode_ = 1;
    if (!IsRowWithinBounds(row)) return false;
    rowExtraData_[row] = data;
    return true;
}

void* cGZWinGrid::GetRowExtraData(int row) {   // @ 0x10025b54e
    return rowExtraData_[row];
}

bool cGZWinGrid::SetExtraData(void* data) {   // @ 0x10025b574
    extraDataMode_ = 1;
    extraData_ = data;
    return true;
}

bool cGZWinGrid::GetCellExtraDataObject(int column, int row, std::uint32_t iid, void** out) {   // @ 0x10025b59c
    extraDataMode_ = 1;
    void* data = GetCellExtraData(column, row);
    extraDataMode_ = 2;
    if (!data) return false;
    return static_cast<cIGZUnknown*>(data)->QueryInterface(iid, out);
}

bool cGZWinGrid::SetCellExtraDataObject(int column, int row, cIGZUnknown* object) {   // @ 0x10025b5fa
    extraDataMode_ = 2;
    if (!IsCellWithinBounds(column, row)) return false;
    if (object) object->AddRef();
    extraDataMode_ = 1;
    cIGZUnknown* old = static_cast<cIGZUnknown*>(GetCellExtraData(column, row));
    extraDataMode_ = 2;
    if (old) old->Release();
    extraDataMode_ = 1;
    // The original passes +0x350 (the grid's extra-data pointer) here instead of the new object (0x10025b5fa),
    // which looks like a bug and would leak the reference it just took; we store the object.
    SetCellExtraData(column, row, object);
    extraDataMode_ = 2;
    return true;
}

bool cGZWinGrid::GetExtraDataObject(std::uint32_t iid, void** out) {   // @ 0x10025b6a8
    if (!extraData_) return false;
    return static_cast<cIGZUnknown*>(extraData_)->QueryInterface(iid, out);
}

bool cGZWinGrid::SetExtraDataObject(cIGZUnknown* object) {   // @ 0x10025b6c6
    extraDataMode_ = 2;
    if (object) object->AddRef();
    if (extraData_) static_cast<cIGZUnknown*>(extraData_)->Release();
    extraData_ = object;
    return true;
}

int cGZWinGrid::DefaultSortFunction(const CellData* first, const CellData* second) {   // @ 0x10025b70c
    if (!first || !second) return 0;
    if (first->type != second->type) return 0;
    if (first->type != kCellTypeText || !first->value || !second->value) return 0;
    const cRZString& a = *static_cast<const cRZString*>(first->value);
    const cRZString& b = *static_cast<const cRZString*>(second->value);
    return a.CompareToString(b, false);
}

void cGZWinGrid::SortColumn(int column, int row, bool whole, SortFunction compare) {   // @ 0x10025b758
    if (!compare) compare = &DefaultSortFunction;
    std::vector<CellData*> ordered;
    std::vector<int> rows;
    for (auto it = cells_.ColBegin(column); it != cells_.ColEnd(column); ++it) {
        const rz::MatrixUserCell<CellData*> entry = *it;
        if (!entry.value) continue;
        if (!whole && entry.row < row) continue;
        ordered.push_back(entry.value);
        rows.push_back(entry.row);
    }
    if (ordered.size() < 2) return;
    std::vector<CellData> values(ordered.size());
    for (std::size_t i = 0; i < ordered.size(); ++i) values[i] = *ordered[i];
    std::vector<std::size_t> order(values.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return compare(&values[a], &values[b]) < 0;
    });
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        CellData* target = ordered[i];
        *target = values[order[i]];
        target->col = column;
        target->row = rows[i];
    }
    for (CellData* cell : ordered) {
        if (cell->type == kCellTypeWindow) DoCellWindowMove(cell);
        SetSizeCachedActualForRow(cell->row, 1, -1);
    }
    recalculatePending_ = true;
    MarkInvalid();
}

void cGZWinGrid::SortRow(int row, int column, bool whole, SortFunction compare) {   // @ 0x10025bd9e
    if (!compare) compare = &DefaultSortFunction;
    std::vector<CellData*> ordered;
    std::vector<int> columns;
    for (auto it = cells_.begin(); it != cells_.end(); ++it) {
        const rz::MatrixUserCell<CellData*> entry = *it;
        if (entry.row != row || !entry.value) continue;
        if (!whole && entry.col < column) continue;
        ordered.push_back(entry.value);
        columns.push_back(entry.col);
    }
    if (ordered.size() < 2) return;
    std::vector<CellData> values(ordered.size());
    for (std::size_t i = 0; i < ordered.size(); ++i) values[i] = *ordered[i];
    std::vector<std::size_t> order(values.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return compare(&values[a], &values[b]) < 0;
    });
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        CellData* target = ordered[i];
        *target = values[order[i]];
        target->col = columns[i];
        target->row = row;
    }
    for (CellData* cell : ordered) {
        if (cell->type == kCellTypeWindow) DoCellWindowMove(cell);
        SetSizeCachedActualForColumn(cell->col, 1, -1);
    }
    recalculatePending_ = true;
    MarkInvalid();
}

bool cGZWinGrid::FindCellText(const char* text, std::uint32_t length, int firstColumn, int columnCount, int firstRow,
                              int rowCount, int& column, int& row, bool exact) {   // @ 0x10025c584
    if (!text) return false;
    const std::uint32_t used = (length == 0xffffffffu || length == 0) ? static_cast<std::uint32_t>(std::strlen(text))
                                                                      : length;
    const int lastColumn = columnCount == 0x7fffffff ? std::numeric_limits<int>::max() : firstColumn + columnCount;
    const int lastRow = rowCount == 0x7fffffff ? std::numeric_limits<int>::max() : firstRow + rowCount;
    for (auto it = cells_.begin(); it != cells_.end(); ++it) {
        const rz::MatrixUserCell<CellData*> entry = *it;
        if (entry.col < firstColumn || entry.col >= lastColumn) continue;
        if (entry.row < firstRow || entry.row >= lastRow) continue;
        if (!entry.value || entry.value->type != kCellTypeText || !entry.value->value) continue;
        const cRZString& value = *static_cast<cRZString*>(entry.value->value);
        if (exact && value.Strlen() != used) continue;
        if (value.FindChars(text, 0, true) < value.Strlen()) {
            column = entry.col;
            row = entry.row;
            return true;
        }
    }
    return false;
}

bool cGZWinGrid::SetOwnerDrawNotification(cIGZMessageTarget* target) {   // @ 0x10025c864
    if (target) target->AddRef();
    if (ownerDrawTarget_) ownerDrawTarget_->Release();
    ownerDrawTarget_ = target;
    return true;
}

bool cGZWinGrid::GetBufferToDrawTo(IWinBuffer*& buffer, cRZRect& area) {   // @ 0x10025c8a0
    buffer = drawBuffer_;
    area = drawArea_;
    return true;
}

bool cGZWinGrid::SetScrollBarImageVertical(IWinBuffer* image, const cRZRect& area) {   // @ 0x10025d1b8
    if (image) image->AddRef();
    if (verticalScrollImage_) verticalScrollImage_->Release();
    verticalScrollImage_ = image;
    verticalScrollImageArea_ = area;
    if (!image || !verticalScrollBar_) return true;
    cIGZBuffer* buffer = WinGridHost() ? WinGridHost()->AsScrollBarImage(image) : nullptr;
    if (!buffer) return true;
    if (area.left < area.right && area.top < area.bottom) {
        verticalScrollBar_->ImageSetup(area, buffer, 0);
    } else if (image->GetBounds()) {
        verticalScrollBar_->ImageSetup(*image->GetBounds(), buffer, 0);
    }
    return true;
}

bool cGZWinGrid::SetScrollBarImageHorizontal(IWinBuffer* image, const cRZRect& area) {   // @ 0x10025d270
    if (image) image->AddRef();
    if (horizontalScrollImage_) horizontalScrollImage_->Release();
    horizontalScrollImage_ = image;
    horizontalScrollImageArea_ = area;
    if (!image || !horizontalScrollBar_) return true;
    cIGZBuffer* buffer = WinGridHost() ? WinGridHost()->AsScrollBarImage(image) : nullptr;
    if (!buffer) return true;
    if (area.left < area.right && area.top < area.bottom) {
        horizontalScrollBar_->ImageSetup(area, buffer, 0);
    } else if (image->GetBounds()) {
        horizontalScrollBar_->ImageSetup(*image->GetBounds(), buffer, 0);
    }
    return true;
}

namespace {
void (*g_gridSoundPlayer)(std::uint32_t) = nullptr;
}  // namespace
void SetGridSoundPlayer(void (*player)(std::uint32_t soundId)) { g_gridSoundPlayer = player; }

bool cGZWinGrid::SetSoundID(int which, int soundId) {   // @ 0x10025c8c2
    if (which < 0 || which >= 3) return false;
    soundId_[which] = soundId;
    return true;
}

bool cGZWinGrid::PlaySoundA(int which) {   // @ 0x10025c8de
    if (which < 0 || which >= 3 || soundId_[which] == 0) return false;
    // RZ::SS() makes a sound object for the id (vslot 0x70) and plays it (vslot 0x28): the application's sound player.
    if (!g_gridSoundPlayer) return false;
    g_gridSoundPlayer(static_cast<std::uint32_t>(soundId_[which]));
    return true;
}

bool cGZWinGrid::Refresh() {   // @ 0x10025c9a8
    RecalculateAll();
    recalculatePending_ = true;
    return true;
}

bool cGZWinGrid::TestForPossibleScrollBarAdditionOrRemoval() {   // @ 0x10025c9c6
    if ((flags_ & kGridVerticalScroll) != 0) {
        // Mac i386 0x001b7d3a: unbounded rows without a limit always scroll; otherwise the rows that count are the limit
        // (unbounded rows) or the used rows (vslot [50]), and the bar shows while they exceed the visible rows.
        bool want = false;
        const bool noLimit = rowLimit_ == -1 || rowLimit_ == 0;
        if (noLimit && (flags_ & kGridUnboundedRows) != 0) {
            want = true;
        } else {
            int rows = rowLimit_;
            if ((flags_ & kGridUnboundedRows) == 0) {
                cRZRect used{0, 0, 0, 0};
                GetCellUsedRange(used);
                rows = used.bottom + 1;
            }
            want = static_cast<float>(rows) > visibleRows_;
        }
        if (want) {
            if (!verticalScrollBar_) AddScrollBar(true, false);
        } else if (verticalScrollBar_) {
            RemoveScrollBar(true, false);
        }
    }
    if ((flags_ & kGridHorizontalScroll) != 0) {
        bool want = false;
        if (columnLimit_ > 0) want = static_cast<float>(columnLimit_) > visibleColumns_;
        else if (columnLimit_ == -1 && static_cast<std::int16_t>(flags_ & 0xffff) < 0) want = true;
        if (want) {
            if (!horizontalScrollBar_) AddScrollBar(false, true);
        } else if (horizontalScrollBar_) {
            RemoveScrollBar(false, true);
        }
    }
    if (verticalScrollBar_ || horizontalScrollBar_) {
        SetScrollBarValues();
        SetScrollBarPositions();
    }
    return true;
}

bool cGZWinGrid::AddScrollBar(bool vertical, bool horizontal) {   // @ 0x10025cd88
    bool added = false;
    IWinCtrlFactory* factory = WinCtrlFactory();
    if (vertical && (flags_ & kGridVerticalScroll) != 0 && !verticalScrollBar_) {
        cIGZWinScrollbar* bar = factory ? factory->CreateScrollbar(kGridVerticalScrollBarId, 2, 0, 100, 0) : nullptr;
        verticalScrollBar_ = bar;
        if (!bar) {
            ClearFlag(kGridVerticalScroll);
        } else {
            if (verticalScrollImage_) {
                if (verticalScrollImageArea_.left < verticalScrollImageArea_.right &&
                    verticalScrollImageArea_.top < verticalScrollImageArea_.bottom) {
                    if (cIGZBuffer* image = WinGridHost() ? WinGridHost()->AsScrollBarImage(verticalScrollImage_) : nullptr)
                        bar->ImageSetup(verticalScrollImageArea_, image, 0);
                } else if (verticalScrollImage_->GetBounds()) {
                    if (cIGZBuffer* image = WinGridHost() ? WinGridHost()->AsScrollBarImage(verticalScrollImage_) : nullptr)
                        bar->ImageSetup(*verticalScrollImage_->GetBounds(), image, 0);
                }
            }
            ChildAdd(bar->AsIGZWin());
            cIGZWin* focus = GetFocus();
            SetFocusTo(focus);
        }
        added = true;
    }
    if (horizontal && (flags_ & kGridHorizontalScroll) != 0 && !horizontalScrollBar_) {
        cIGZWinScrollbar* bar = factory ? factory->CreateScrollbar(kGridHorizontalScrollBarId, 1, 0, 100, 0) : nullptr;
        horizontalScrollBar_ = bar;
        if (!bar) {
            ClearFlag(kGridHorizontalScroll);
        } else {
            if (horizontalScrollImage_) {
                if (horizontalScrollImageArea_.left < horizontalScrollImageArea_.right &&
                    horizontalScrollImageArea_.top < horizontalScrollImageArea_.bottom) {
                    if (cIGZBuffer* image = WinGridHost() ? WinGridHost()->AsScrollBarImage(horizontalScrollImage_) : nullptr)
                        bar->ImageSetup(horizontalScrollImageArea_, image, 0);
                } else if (horizontalScrollImage_->GetBounds()) {
                    if (cIGZBuffer* image = WinGridHost() ? WinGridHost()->AsScrollBarImage(horizontalScrollImage_) : nullptr)
                        bar->ImageSetup(*horizontalScrollImage_->GetBounds(), image, 0);
                }
            }
            ChildAdd(bar->AsIGZWin());
            cIGZWin* focus = GetFocus();
            SetFocusTo(focus);
        }
        added = true;
    }
    if (added) recalculatePending_ = true;
    return true;
}

void cGZWinGrid::SetScrollBarPositions() {   // @ 0x10025cfec
    const int width = GetW(), height = GetH();
    if (verticalScrollBar_) {
        cIGZWin* window = verticalScrollBar_->AsIGZWin();
        const int barWidth = window->GetW();
        int left = width - barWidth;
        int top = 0;
        if ((flags_ & kGridColumnHeaders) != 0) {
            top = static_cast<int>((flags_ >> 9) & 1) + columnHeaderHeight_ + gutterTop_;
        }
        int right = width;
        int bottom = height;
        if (horizontalScrollBar_) bottom = height - horizontalScrollBar_->AsIGZWin()->GetH();
        window->SetArea(left, top, right, bottom);
    }
    if (horizontalScrollBar_) {
        cIGZWin* window = horizontalScrollBar_->AsIGZWin();
        const int barHeight = window->GetH();
        const int top = height - barHeight;
        int left = 0;
        if ((flags_ & kGridRowHeaders) != 0) {
            left = gutterLeft_ + 1 + static_cast<int>((flags_ >> 9) & 1) + rowHeaderWidth_;
        }
        int right = width;
        if (verticalScrollBar_) right = width - verticalScrollBar_->AsIGZWin()->GetW();
        window->SetArea(left, top, right, height);
    }
}

int cGZWinGrid::GetColumnWidthInternal(int column) {   // @ 0x100254054
    CellFormatExtra* extra = nullptr;
    if (columnFormats_.GetCellPtr(column, 0, &extra) && extra) {
        if (extra->size != -1) return extra->size;
        if (extra->actual != -1) return extra->actual;
    } else if (cells_.GetRowCountForCol(column) == 0) {
        return defaultColumnWidth_;
    }
    SetSizeCachedActualForColumn(column, 1, defaultColumnWidth_);
    return defaultColumnWidth_;
}

int cGZWinGrid::GetRowHeightInternal(int row) {   // @ 0x1002540d4
    const int base = rowLimit_ > 0 ? cellAreaHeight_ / rowLimit_ : defaultRowHeight_;
    CellFormatExtra* extra = nullptr;
    const bool hasFormat = rowFormats_.GetCellPtr(0, row, &extra) && extra;
    if (hasFormat) {
        if (extra->size != -1) return extra->size;
        if (extra->actual != -1) return extra->actual;
    }
    int minColumn = 0, maxColumn = 0;
    const bool hasCells = cells_.GetMinMaxUsedColForRow(row, minColumn, maxColumn);
    if (!hasFormat && !hasCells) return defaultRowHeight_;
    int height = base;
    if (hasCells) {
        for (int column = minColumn; column <= maxColumn; ++column) {
            CellData* cell = FindCell(column, row);
            if (!cell) continue;
            height = std::max(height, CalculateCellHeight(column, row, cell));
        }
    }
    SetSizeCachedActualForRow(row, 1, height);
    return height;
}

int cGZWinGrid::CalculateCellHeight(int column, int row, CellData* cell) {   // @ 0x10025d3cc
    int height = defaultRowHeight_;
    if (!cell) cell = FindCell(column, row);
    if (!cell) {
        return height + cellGutterTop_ + cellGutterBottom_;
    }
    switch (cell->type) {
        case kCellTypeWindow:
            if (cell->value) height = static_cast<cIGZWin*>(cell->value)->GetH();
            break;
        case kCellTypeBuffer:
            if (cell->value) {
                const cRZRect* bounds = static_cast<IWinBuffer*>(cell->value)->GetBounds();
                if (bounds) height = bounds->bottom - bounds->top;
            }
            break;
        case kCellTypeText: {
            cRZString* text = static_cast<cRZString*>(cell->value);
            if (!text || text->Strlen() == 0) break;
            CellFormatExtra* extra = nullptr;
            if (!GetColRowCellFormatExtra(column, row, extra) || !extra || !extra->font) break;
            const int lineHeight = extra->font->LineHeight(0, 0);
            if (lineHeight == 0) break;
            int lines = 1;
            if ((flags_ & (kGridWrapText | kGridOwnerDrawCells)) == 0) {
                lines = extra->font->CountLines(text->Data(), text->Strlen(), GetColumnWidthInternal(column));
            }
            if (lines == 0) break;
            height = lines * lineHeight;
            break;
        }
        default:
            break;
    }
    return height + cellGutterTop_ + cellGutterBottom_;
}

void cGZWinGrid::DrawCellBackground(OwnerDrawInfo& info) {   // @ 0x100254376
    if (!info.cellRect || !info.context) return;
    cRZRect rect = *info.cellRect;
    if ((flags_ & kGridColumnLines) != 0 && FormatColorSet(columnLineColor_)) rect.left += lineOffsetX_;
    if ((flags_ & kGridRowLines) != 0 && FormatColorSet(rowLineColor_)) rect.top += lineOffsetY_;
    if (info.clipRect) {
        if (rect.left > info.clipRect->right || rect.top > info.clipRect->bottom || rect.right < info.clipRect->left ||
            rect.bottom < info.clipRect->top) {
            rect = cRZRect{0, 0, 0, 0};
        } else {
            rect.left = std::max(rect.left, info.clipRect->left);
            rect.top = std::max(rect.top, info.clipRect->top);
            rect.right = std::min(rect.right, info.clipRect->right);
            rect.bottom = std::min(rect.bottom, info.clipRect->bottom);
        }
    }
    const CellFormat* format = info.format ? info.format : &defaultFormat_;
    std::uint32_t color;
    if (!info.selected) {
        color = format->colors[1];
        if (!FormatColorSet(color)) return;
    } else {
        if ((flags_ & kGridNoCellBackground) != 0) return;
        color = format->colors[5];
        if (!FormatColorSet(color)) return;
    }
    info.context->SetColor(color);
    info.context->Fill(rect);
}

void cGZWinGrid::CalcCellDrawingParams(const cIGZString& text, const OwnerDrawInfo& info, cRZRect& rect,
                                       std::uint32_t& style, std::uint32_t& color, std::uint32_t& color2) {   // @ 0x1002544dc
    (void)text;
    const CellFormat* format = info.format ? info.format : &defaultFormat_;
    int lineHeight = 0;
    if (format->font) lineHeight = format->font->LineHeight(0, 0);
    if (format->halign == 3) style = 1;
    else if (format->halign == 4) style = 2;
    else style = 0;
    if (format->valign == 6) {
        const int middle = (rect.bottom + rect.top) / 2 - lineHeight / 2;
        rect.top = middle;
        rect.bottom = middle + lineHeight;
    } else if (format->valign != 5) {
        rect.top = rect.bottom - lineHeight;
    }
    if (!info.selected) {
        // an enabled grid draws with pair 0 / 1 (the pair DrawCellBackground fills with), a disabled one with 2 / 3
        // (Mac i386 0x001b5868: the info's +0x29 is the window's enabled flag, GZPaint 0x001babfe)
        if (info.enabled) {
            color = format->colors[0];
            color2 = format->colors[1];
        } else {
            color = format->colors[2];
            color2 = format->colors[3];
        }
    } else {
        color = format->colors[4];
        color2 = format->colors[5];
    }
}

void cGZWinGrid::DrawCell(OwnerDrawInfo& info) {   // @ 0x100254676 (text and bitmap paths; cell windows are placed, not drawn)
    if (!info.cellRect || !info.context) return;
    cRZRect rect = *info.cellRect;
    if ((flags_ & kGridColumnLines) != 0 && FormatColorSet(columnLineColor_)) rect.left += lineOffsetX_;
    if ((flags_ & kGridRowLines) != 0 && FormatColorSet(rowLineColor_)) rect.top += lineOffsetY_;
    if (info.clipRect) {
        if (rect.left > info.clipRect->right || rect.top > info.clipRect->bottom || rect.right < info.clipRect->left ||
            rect.bottom < info.clipRect->top) {
            rect = cRZRect{0, 0, 0, 0};
        } else {
            rect.left = std::max(rect.left, info.clipRect->left);
            rect.top = std::max(rect.top, info.clipRect->top);
            rect.right = std::min(rect.right, info.clipRect->right);
            rect.bottom = std::min(rect.bottom, info.clipRect->bottom);
        }
    }
    if ((flags_ & kGridOwnerDrawCells) == 0) DrawCellBackground(info);
    cRZRect content{rect.left + cellGutterLeft_, rect.top + cellGutterTop_, rect.right - cellGutterRight_,
                    rect.bottom - cellGutterBottom_};
    const CellData* cell = info.value ? info.value : info.cell;
    if (!cell) return;
    if (cell->type == kCellTypeBuffer && cell->value) {
        IWinBuffer* image = static_cast<IWinBuffer*>(cell->value);
        const cRZRect* bounds = image->GetBounds();
        if (!bounds || !image->AsDrawSource()) return;
        const int imageWidth = bounds->right - bounds->left;
        const int imageHeight = bounds->bottom - bounds->top;
        const CellFormat* format = info.format ? info.format : &defaultFormat_;
        int left = content.left;
        if (format->halign == 4) left = content.right - imageWidth;
        else if (format->halign == 3) left = (content.right - content.left) / 2 - imageWidth / 2 + content.left;
        int top = content.top;
        if (format->valign == 7) top = content.bottom - imageHeight;
        else if (format->valign == 6) top = (content.bottom - content.top) / 2 - imageHeight / 2 + content.top;
        const cRZRect source{bounds->left, bounds->top, bounds->left + imageWidth, bounds->top + imageHeight};
        const cRZRect destination{left, top, left + imageWidth, top + imageHeight};
        info.context->Blt(image->AsDrawSource(), source, destination);
        return;
    }
    if (cell->type == kCellTypeText && cell->value) {
        const cRZString& text = *static_cast<const cRZString*>(cell->value);
        if (text.Strlen() == 0) return;
        const CellFormat* format = info.format ? info.format : &defaultFormat_;
        IWinFont* font = format->font ? format->font : defaultFormat_.font;
        if (!font) return;
        std::uint32_t style = 0, color = 0, color2 = 0;
        cRZRect textRect = content;
        CalcCellDrawingParams(text, info, textRect, style, color, color2);
        font->DrawTextBox(drawBuffer_, textRect, text.Data(), text.Strlen(), style, color, color2, info.clipRect, 0);
    }
}

bool cGZWinGrid::GZPaint() {   // @ 0x100252f70 (simplified painter, see the PARTIAL note in the ledger)
    if (recalculatePending_) RecalculateAll();
    invalidatePending_ = false;
    if (!drawContext_) return true;
    cIGZDrawContext* context = static_cast<cIGZDrawContext*>(drawContext_);
    cRZRect area;
    GetCellArea(area);
    area.left += drawArea_.left;
    area.top += drawArea_.top;
    area.right += drawArea_.left;
    area.bottom += drawArea_.top;
    cRZRect visible;
    GetCellVisibleRange(visible);
    if ((flags_ & kGridBackground) != 0) {
        if (!background_) {
            context->SetColor(GetFillColorRGB());
            context->Fill(area);
        } else if (background_->GetBounds() && background_->AsDrawSource()) {
            context->Blt(background_->AsDrawSource(), *background_->GetBounds(), area);
        }
    }
    if ((flags_ & kGridColumnLines) != 0 && FormatColorSet(columnLineColor_)) {
        int x = area.left;
        for (int column = firstVisibleColumn_; column <= static_cast<int>(visible.right); ++column) {
            x += GetColumnWidthInternal(column);
            context->SetColor(columnLineColor_);
            context->Fill(cRZRect{x, area.top, x + lineOffsetX_, area.bottom});
        }
    }
    if ((flags_ & kGridRowLines) != 0 && FormatColorSet(rowLineColor_)) {
        int y = area.top;
        for (int row = firstVisibleRow_; row <= static_cast<int>(visible.bottom); ++row) {
            y += GetRowHeightInternal(row);
            context->SetColor(rowLineColor_);
            context->Fill(cRZRect{area.left, y, area.right, y + lineOffsetY_});
        }
    }
    if ((flags_ & kGridColumnHeaders) != 0) {
        for (int column = firstVisibleColumn_; column <= static_cast<int>(visible.right); ++column) {
            cRZRect rect;
            if (!GetColumnHeaderArea(column, rect)) break;
            rect.left += drawArea_.left;
            rect.right += drawArea_.left;
            rect.top += drawArea_.top;
            rect.bottom += drawArea_.top;
            CellFormatExtra heading;
            static_cast<CellFormat&>(heading) = headingFormat_;
            const CellFormat* format = &heading;
            OwnerDrawInfo info;
            info.cellRect = &rect;
            info.context = context;
            info.grid = this;
            info.format = &heading;
            if (ownerDrawTarget_) {
                cGZMessage message;
                message.type = kGZMsgGridDrawColumnHeader;
                message.param1 = reinterpret_cast<std::uint64_t>(static_cast<cIGZWinGrid*>(this));
                message.param2 = reinterpret_cast<std::uint64_t>(&info);
                if (ownerDrawTarget_->DoMessage(message)) continue;
            }
            DrawCellBackground(info);
            cRZString text = columnHeadings_[column];
            if (text.Strlen() == 0 && (flags_ & kGridNumberHeaders) != 0) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "Column #%d", column);
                text = cRZString(buffer);
            }
            if (text.Strlen() != 0 && format->font) {
                std::uint32_t style = 0, color = 0, color2 = 0;
                cRZRect textRect = rect;
                CalcCellDrawingParams(text, info, textRect, style, color, color2);
                format->font->DrawTextBox(drawBuffer_, textRect, text.Data(), text.Strlen(), style, color, color2,
                                          nullptr, 0);
            }
        }
    }
    if ((flags_ & kGridRowHeaders) != 0) {
        for (int row = firstVisibleRow_; row <= static_cast<int>(visible.bottom); ++row) {
            cRZRect rect;
            if (!GetRowHeaderArea(row, rect)) break;
            rect.left += drawArea_.left;
            rect.right += drawArea_.left;
            rect.top += drawArea_.top;
            rect.bottom += drawArea_.top;
            CellFormatExtra heading;
            static_cast<CellFormat&>(heading) = headingFormat_;
            const CellFormat* format = &heading;
            OwnerDrawInfo info;
            info.cellRect = &rect;
            info.context = context;
            info.grid = this;
            info.format = &heading;
            if (ownerDrawTarget_) {
                cGZMessage message;
                message.type = kGZMsgGridDrawRowHeader;
                message.param1 = reinterpret_cast<std::uint64_t>(static_cast<cIGZWinGrid*>(this));
                message.param2 = reinterpret_cast<std::uint64_t>(&info);
                if (ownerDrawTarget_->DoMessage(message)) continue;
            }
            DrawCellBackground(info);
            cRZString text = rowHeadings_[row];
            if (text.Strlen() == 0 && (flags_ & kGridNumberHeaders) != 0) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "Row #%d", row);
                text = cRZString(buffer);
            }
            if (text.Strlen() != 0 && format->font) {
                std::uint32_t style = 0, color = 0, color2 = 0;
                cRZRect textRect = rect;
                CalcCellDrawingParams(text, info, textRect, style, color, color2);
                format->font->DrawTextBox(drawBuffer_, textRect, text.Data(), text.Strlen(), style, color, color2,
                                          nullptr, 0);
            }
        }
    }
    for (int column = firstVisibleColumn_; column <= static_cast<int>(visible.right); ++column) {
        for (int row = firstVisibleRow_; row <= static_cast<int>(visible.bottom); ++row) {
            if (!IsCellVisible(column, row)) continue;
            CellData* cell = FindCell(column, row);
            if (!cell) continue;
            cRZRect rect;
            GetCellRect(column, row, rect);
            rect.left += drawArea_.left;
            rect.right += drawArea_.left;
            rect.top += drawArea_.top;
            rect.bottom += drawArea_.top;
            CellFormat format;
            GetCellFormat(column, row, format);
            CellFormatExtra extra;
            static_cast<CellFormat&>(extra) = format;
            OwnerDrawInfo info;
            info.cellRect = &rect;
            info.clipRect = &area;
            info.cell = cell;
            info.context = context;
            info.grid = this;
            info.format = &extra;
            info.value = cell;
            info.selected = GetCellIsSelected(column, row);
            info.enabled = IsEnabled();
            if (ownerDrawTarget_) {
                cGZMessage message;
                message.type = kGZMsgGridDrawCell;
                message.param1 = reinterpret_cast<std::uint64_t>(static_cast<cIGZWinGrid*>(this));
                message.param2 = reinterpret_cast<std::uint64_t>(&info);
                if (ownerDrawTarget_->DoMessage(message)) continue;
            }
            if ((flags_ & kGridOwnerDrawCells) != 0) DrawCellBackground(info);
            DrawCell(info);
        }
    }
    if ((flags_ & kGridOutline) != 0) {
        RZDrawRectangleOutline(context, cRZRect{0, 0, GetW(), GetH()}, outlineColor_, 1);
    }
    if ((flags_ & kGridCellAreaOutline) != 0) {
        RZDrawRectangleOutline(context, area, outlineColor_, 1);
    }
    return true;
}

}  // namespace gz
