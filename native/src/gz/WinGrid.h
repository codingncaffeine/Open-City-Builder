// cGZWinGrid — the grid control (189 symbols, 0x10025086a..0x10025ef00, plus two accessors at 0x10003abbc): rows,
// columns, cells, cell formats, selection, scrollbars, headers, owner draw and sorting.
//
// The object is a cIGZWinGrid (the grid interface, its own vptr at +0) followed by a cGZWin (the window, vptr at +8),
// exactly like the original: the vtable at 0x100a8a100 has the grid's 140 entries, the one at 0x100a8a608 the window's.
// Layout (0x404+ bytes):
//   +0x158 uint32 flags                +0x15c int log level (RZ::LM() answer at Init, 1 when absent)
//   +0x160 bool recalculate pending    +0x164 int recalculation pause count
//   +0x168 bool invalidate pending     +0x16c gutters (l, t, r, b)            +0x17c cell gutters (l, t, r, b)
//   +0x18c int column header height (20)   +0x190 int row header width (5)
//   +0x194 grid area width, +0x198 grid area height, +0x19c cell area width, +0x1a0 cell area height
//   +0x1a4 first visible column        +0x1a8 first visible row
//   +0x1ac float visible columns       +0x1b0 float visible rows
//   +0x1b4 column limit (-1 none)      +0x1b8 row limit (-1 none)
//   +0x1bc default column width (100)  +0x1c0 default row height (0x140)
//   +0x1c4 outline colour (packed)     +0x1c8 column line colour, +0x1cc row line colour (packed)
//   +0x1d0 / +0x1d4 line offset x / y (1)   +0x1d8 / +0x1dc most recently added cell (-1, -1)
//   +0x1e0 background buffer           +0x1e8 owner-draw message target
//   +0x1f0 click timer (cRZTimer, 0x18)     +0x208 int extra-data mode (1 raw pointer, 2 COM object)
//   +0x20c selection rule (0 columns, 1 rows, else cells)   +0x210 int selection limit
//   +0x218 std::list<CellCoordinates> selection (+0x228 size)   +0x230 default text alignment
//   +0x238 column format matrix (CellFormatExtra)   +0x268 row format matrix   +0x298 cell matrix (CellData*)
//   +0x2c8 default CellFormat (font, colors[6], flag byte, halign, valign, border, border colour)
//   +0x2f0 std::map<cIGZWin*, CellWindow> cell windows   +0x308 last cursor hit (result, col, row)
//   +0x314 last reported hover (result, col, row)   +0x328 row extra data map   +0x338 column extra data map
//   +0x350 void* extra data            +0x358 vertical scrollbar (id 0x1a7c4511), +0x360 horizontal (0x1a7c4510)
//   +0x368 / +0x370 scrollbar images, +0x378 / +0x388 their source rectangles
//   +0x398 int sound ids[3]            +0x3a8 column headings, +0x3c0 row headings (map<int, cRZString>)
//   +0x3d8 heading CellFormat (same 0x28 layout as the default format)
//
// A cell is only stored when set (cRZSparseMatrix), so a 1000x1000 grid costs what its cells cost. Cell backgrounds
// and text come from the cell format merged over the row format, the column format and the default format; every
// unset field of a format is the 0x40000000 sentinel (0xff for the flag byte, 0 for the alignment bytes).
//
// Neighbour seams (the same shape the other recovered controls use): fonts are IWinFont (gz/WinFont.h) where the
// original holds cIGZFont*, buffers are the window-control IWinBuffer (gz/Win.h) where it holds cIGZBuffer*, and
// IWinGridHost above covers the two places the grid reaches outside the control family (the text of a cell window,
// which is a cGZWinTextEdit, and turning a scrollbar image IWinBuffer into the cIGZBuffer the scrollbar wants).
//
// Quirks kept: GZOnCharacter adds 8 to the character before forwarding it to the window (0x100251928);
// EnableHeaders answers false (0x100256996); DeleteCell always answers false (0x10025705a); ClearCellFormat removes
// the whole cell when it has no value even if it still holds extra data (0x100259096; we free it, the original leaks
// it); the click timer only compares
// against the 500 ms window and is never re-armed for a double click (0x10025237e); RemoveScrollBar always answers
// true (0x100254de2); the row-header width defaults to 5 and the row height to 0x140 (0x10025086a).
#pragma once
#include <cstdint>
#include <list>
#include <map>

#include "gz/Win.h"
#include "rz/SparseMatrix.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"

namespace gz {

class IWinFont;
class cIGZBuffer;
class cIGZDrawContext;
class cIGZMessageTarget;
class cIGZWinScrollbar;

constexpr std::uint32_t kGZIID_cIGZWinGrid = 0xdaa6b9be;

// Grid flags (cGZWinGrid +0x158; the ctor argument). The default formatting does not change them.
enum WinGridFlag : std::uint32_t {
    kGridFlag1 = 0x1,
    kGridHorizontalScroll = 0x2,        // horizontal scrollbar allowed
    kGridVerticalScroll = 0x4,          // vertical scrollbar allowed
    kGridColumnHeaders = 0x8,
    kGridRowHeaders = 0x10,
    kGridColumnLines = 0x20,            // draw a line at every column boundary
    kGridRowLines = 0x40,               // draw a line at every row boundary
    kGridSortDescending = 0x80,         // toggled by the '-' key (GZOnKeyDown)
    kGridBackground = 0x100,            // paint the background colour / bitmap
    kGridOutline = 0x200,               // outline the whole window
    kGridCellAreaOutline = 0x400,       // outline the cell area
    kGridWrapText = 0x800,              // default text wrapping
    kGridFlag1000 = 0x1000,
    kGridFixedColumns = 0x2000,
    kGridFixedRows = 0x4000,
    kGridUnboundedColumns = 0x8000,     // columns scroll freely (else the visible range clamps them)
    kGridUnboundedRows = 0x10000,       // rows scroll freely
    kGridOwnerDrawCells = 0x20000,      // cells draw their own background (owner draw)
    kGridCapture = 0x40000,             // the grid takes the mouse capture
    kGridNoCellBackground = 0x80000,    // skip the selected cell background
};

// cIGZMessageTarget ([3] DoMessage at +0x18; the grid posts owner-draw notifications to it).
class cIGZMessageTarget : public cIGZUnknown {
public:
    virtual bool DoMessage(cGZMessage& message) = 0;    // [3] +0x18
};

// cIGZWinGrid (iid 0xdaa6b9be). Slots in the original vtable order (cGZWinGrid vtable 0x100a8a100); the address in
// each comment is the cGZWinGrid implementation.
class cIGZWinGrid : public cIGZUnknown {
public:
    enum CursorHitTestResult : std::int32_t {
        kCursorNothing = 0, kCursorOutsideGrid = 1, kCursorRowHeader = 2, kCursorColumnHeader = 3, kCursorCorner = 4,
        kCursorCell = 5,
    };
    struct CellCoordinates {                // 8 bytes
        std::int32_t col = 0;               // +0
        std::int32_t row = 0;               // +4
    };
    // cIGZWinGrid::CellFormat, 0x28 bytes. `font` is the original's cIGZFont*; the recovered text controls use the
    // IWinFont adapter (gz/WinFont.h) so the grid does not need the whole font interface. The six colours are
    // 0 text, 1 background, 2 disabled text, 3 disabled background, 4 selected text, 5 selected background.
    struct CellFormat {
        IWinFont* font = nullptr;           // +0x00
        std::uint32_t colors[6] = {0x40000000, 0x40000000, 0x40000000, 0x40000000, 0x40000000, 0x40000000};  // +0x08
        std::uint8_t flag = 0xff;           // +0x20 (set to 1 by SetDefaultFormatting)
        std::uint8_t halign = 0;            // +0x21 (2 left, 3 center, 4 right)
        std::uint8_t valign = 0;            // +0x22 (5 top, 6 middle, 7 bottom)
        std::uint8_t border = 0;            // +0x23
        std::uint32_t borderColor = 0x40000000;   // +0x24
    };
    // The per-column / per-row formats add two cached sizes (+0x28 requested, +0x2a actual; -1 when unset).
    struct CellFormatExtra : CellFormat {
        std::int16_t size = -1;             // +0x28
        std::int16_t actual = -1;           // +0x2a
    };
    // A cell, 0x30 bytes. type: 0 none (format / extra data only), 1 window, 2 text, 4 buffer.
    struct CellData {
        std::int32_t type = 0;              // +0x00
        std::int32_t col = 0;               // +0x04
        std::int32_t row = 0;               // +0x08
        std::uint32_t reserved = 0;         // +0x0c
        void* value = nullptr;              // +0x10 (cRZString*, cIGZWin*, IWinBuffer*)
        CellFormat* format = nullptr;       // +0x18
        void* extraData = nullptr;          // +0x20
        std::uint32_t windowFlags = 0;      // +0x28 (1 fit width, 2 fit height)
    };
    // Passed to owner-draw notification messages 0xfa785cb0 / b1 / b2 (column header / row header / cell). The
    // pointer members are borrowed for the duration of the message.
    struct OwnerDrawInfo {
        std::int32_t reserved0 = 0;         // +0x00
        std::int32_t reserved1 = 0;         // +0x04
        std::int32_t reserved2 = 0;         // +0x08
        std::int32_t reserved3 = 0;         // +0x0c
        const cRZRect* cellRect = nullptr;  // +0x10
        const cRZRect* clipRect = nullptr;  // +0x18
        CellData* cell = nullptr;           // +0x20
        cIGZDrawContext* context = nullptr; // +0x28
        void* grid = nullptr;               // +0x30 (the cGZWinGrid)
        CellFormatExtra* format = nullptr;  // +0x38 (the merged format of the row / column / cell)
        CellData* value = nullptr;          // +0x40
        bool selected = false;              // +0x48
        bool enabled = false;               // +0x49
    };

    // --- vtable, offsets relative to the cIGZWinGrid vptr ---
    virtual bool Init() = 0;                                                            // [3]  0x18  0x100250f00
    virtual bool Shutdown() = 0;                                                        // [4]  0x20  0x10025151e
    virtual cIGZWin* AsIGZWin() = 0;                                                    // [5]  0x28  0x100250ef6
    virtual bool Refresh() = 0;                                                         // [6]  0x30  0x10025c9a8
    virtual void SetArea(int l, int t, int r, int b) = 0;                               // [7]  0x38  0x100254b8c
    virtual bool GetGridArea(cRZRect& out) = 0;                                         // [8]  0x40  0x1002517d4
    virtual bool GetCellArea(cRZRect& out) = 0;                                         // [9]  0x48  0x100251848
    virtual void SetFlag(int flag) = 0;                                                 // [10] 0x50  0x100254ce6
    virtual void SetFlags(int flags) = 0;                                               // [11] 0x58  0x100254d34
    virtual void ClearFlag(int flag) = 0;                                               // [12] 0x60  0x100254d04
    virtual bool IsFlagSet(int flag) = 0;                                               // [13] 0x68  0x100254d24
    virtual int GetFlags() = 0;                                                         // [14] 0x70  0x100254ea0
    virtual void SetDefaultFont(IWinFont* font) = 0;                                    // [15] 0x78  0x100254f00
    virtual IWinFont* GetDefaultFont() = 0;                                             // [16] 0x80  0x100254f72
    virtual bool SetDefaultFontColor(std::uint32_t color, int index) = 0;               // [17] 0x88  0x100254f80
    virtual bool GetDefaultFontColor(std::uint32_t& color, int index) = 0;              // [18] 0x90  0x100254f9c
    virtual bool SetDefaultFontColorByColumns(std::uint32_t color, int index, int first, int count) = 0;   // [19] 0x98
    virtual bool SetDefaultFontColorByRows(std::uint32_t color, int index, int first, int count) = 0;      // [20] 0xa0
    virtual bool SetDefaultTextWrapping(bool wrap) = 0;                                 // [21] 0xa8  0x1002556a8
    virtual bool GetDefaultTextWrapping() = 0;                                          // [22] 0xb0  0x10025570e
    virtual bool GetGutters(int& l, int& t, int& r, int& b) = 0;                        // [23] 0xb8  0x100255722
    virtual bool SetGutters(int l, int t, int r, int b) = 0;                            // [24] 0xc0  0x10025574c
    virtual bool GetCellGutters(int& l, int& t, int& r, int& b) = 0;                    // [25] 0xc8  0x100255792
    virtual bool SetCellGutters(int l, int t, int r, int b) = 0;                        // [26] 0xd0  0x1002557bc
    virtual void SetOutlineUse(bool on, std::uint32_t color) = 0;                       // [27] 0xd8  0x1002557de
    virtual std::uint32_t GetOutlineColor() = 0;                                        // [28] 0xe0  0x10003abbc
    virtual void PauseRecalculations(bool pause) = 0;                                   // [29] 0xe8  0x10025580a
    virtual bool ActLikeAListbox(int columnLimit, int selectionLimit, bool selectFirst) = 0;   // [30] 0xf0
    virtual bool ScrollCellsVertically(int rows) = 0;                                   // [31] 0xf8  0x1002558f0
    virtual bool ScrollCellsHorizontally(int columns) = 0;                              // [32] 0x100 0x100255a42
    virtual void ScrollPagesVertically(int pages) = 0;                                  // [33] 0x108 0x100255b92
    virtual void ScrollPagesHorizontally(int pages) = 0;                                // [34] 0x110 0x100255ba6
    virtual bool ScrollToColumn(int column) = 0;                                        // [35] 0x118 0x100255bba
    virtual bool ScrollToRow(int row) = 0;                                              // [36] 0x120 0x100255c50
    virtual bool ScrollToCell(int column, int row) = 0;                                 // [37] 0x128 0x100255cee
    virtual bool GetSelectionRules(int& columnRule, int& selectionLimit) = 0;        // [38] 0x130 0x100255d1a
    virtual bool SetSelectionRules(int columnRule, int selectionLimit) = 0;          // [39] 0x138 0x100255d32
    virtual int GetSelectionCount() = 0;                                                // [40] 0x140 0x100255d50
    virtual bool GetSelection(int& column, int& row) = 0;                               // [41] 0x148 0x100255d5c
    virtual bool GetSelection(CellCoordinates* cells, int& count) = 0;                  // [42] 0x150 0x100255d8a
    virtual bool SetSelection(int column, int row) = 0;                                 // [43] 0x158 0x100255dcc
    virtual bool AddCellToSelection(int column, int row, bool add) = 0;                 // [44] 0x160 0x100255e62
    virtual bool GetCellIsSelected(int column, int row) = 0;                            // [45] 0x168 0x100256040
    virtual bool ClearSelection() = 0;                                                  // [46] 0x170 0x1002560c6
    virtual bool EnableFixedRanges(bool columns, bool rows) = 0;                        // [47] 0x178 0x100256110
    virtual bool SetCellLimits(int columns, int rows) = 0;                              // [48] 0x180 0x100256174
    virtual bool GetCellLimits(int& columns, int& rows) = 0;                            // [49] 0x188 0x100256188
    virtual bool GetCellUsedRange(cRZRect& out) = 0;                                    // [50] 0x190 0x1002561a0
    virtual bool GetCellVisibleRange(cRZRect& out) = 0;                                 // [51] 0x198 0x10025639e
    virtual bool IsColumnWithinBounds(int column) = 0;                                  // [52] 0x1a0 0x1002563da
    virtual bool IsRowWithinBounds(int row) = 0;                                        // [53] 0x1a8 0x1002563fe
    virtual bool IsCellWithinBounds(int column, int row) = 0;                           // [54] 0x1b0 0x100256422
    virtual bool GetMostRecentlyAddedCell(int& column, int& row) = 0;                   // [55] 0x1b8 0x10025645a
    virtual bool SetBackgroundColor(std::uint32_t native) = 0;                          // [56] 0x1c0 0x100256476
    virtual bool SetBackgroundBitmap(IWinBuffer* bitmap) = 0;                           // [57] 0x1c8 0x100256488
    virtual bool SetGridColors(int columnLineNative, int rowLineNative) = 0;            // [58] 0x1d0 0x1002564c4
    virtual bool GetGridColors(int& columnLine, int& rowLine) = 0;                      // [59] 0x1d8 0x10003abc8
    virtual bool SetDefaultTextAlignment(int alignment) = 0;                            // [60] 0x1e0 0x10025650e
    virtual bool SetDefaultTextAlignmentByColumns(int first, int count, int alignment) = 0;   // [61] 0x1e8
    virtual bool SetDefaultTextAlignmentByRows(int first, int count, int alignment) = 0;      // [62] 0x1f0
    virtual int GetDefaultTextAlignment() = 0;                                          // [63] 0x1f8 0x10025d558
    virtual void SetDefaultColumnWidth(int width) = 0;                                  // [64] 0x200 0x10025668c
    virtual bool SetColumnWidth(int first, int count, int width) = 0;                   // [65] 0x208 0x100256698
    virtual void SetDefaultRowHeight(int height) = 0;                                   // [66] 0x210 0x100256758
    virtual bool SetRowHeight(int first, int count, int height) = 0;                    // [67] 0x218 0x100256764
    virtual int GetDefaultColumnWidth() = 0;                                            // [68] 0x220 0x10025d564
    virtual int GetDefaultRowHeight() = 0;                                              // [69] 0x228 0x10025d570
    virtual int GetColumnWidth(int first, int count) = 0;                               // [70] 0x230 0x10025d328
    virtual int GetRowHeight(int first, int count) = 0;                                 // [71] 0x238 0x10025d37a
    virtual bool GetColumnHeaderArea(int column, cRZRect& out) = 0;                     // [72] 0x240 0x100256824
    virtual bool GetRowHeaderArea(int row, cRZRect& out) = 0;                           // [73] 0x248 0x1002568d6
    virtual bool EnableHeaders(bool columns, bool rows) = 0;                            // [74] 0x250 0x100256996
    virtual bool GetHeaderSizes(int& columnHeaderHeight, int& rowHeaderWidth) = 0;      // [75] 0x258 0x1002569e2
    virtual bool SetHeaderSizes(int columnHeaderHeight, int rowHeaderWidth) = 0;        // [76] 0x260 0x1002569fa
    virtual bool SetHeadingFormat(const CellFormat& format) = 0;                        // [77] 0x268 0x100256b04
    virtual bool GetColumnHeading(int column, cIGZString& out) = 0;                     // [78] 0x270 0x100256a9c
    virtual bool SetColumnHeading(int column, const cIGZString& text) = 0;              // [79] 0x278 0x100256ad0
    virtual bool GetRowHeading(int row, cIGZString& out) = 0;                           // [80] 0x280 0x100256a34
    virtual bool SetRowHeading(int row, const cIGZString& text) = 0;                    // [81] 0x288 0x100256a68
    virtual bool ClearAllRowHeadings() = 0;                                             // [82] 0x290 0x100256b9a
    virtual bool ClearAllColumnHeadings() = 0;                                          // [83] 0x298 0x100256bdc
    virtual bool GetCellFormat(int column, int row, CellFormat& out) = 0;               // [84] 0x2a0 0x1002586c4
    virtual bool SetCellFormat(int column, int row, const CellFormat& format) = 0;      // [85] 0x2a8 0x1002589d2
    virtual bool ClearCellFormat(int column, int row) = 0;                              // [86] 0x2b0 0x100259096
    virtual void GetColCellFormatExtra(int column, CellFormatExtra*& out) = 0;          // [87] 0x2b8 0x100259174
    virtual void GetRowCellFormatExtra(int row, CellFormatExtra*& out) = 0;             // [88] 0x2c0 0x10025918e
    virtual bool GetColRowCellFormatExtra(int column, int row, CellFormatExtra*& out) = 0;   // [89] 0x2c8
    virtual bool SetCellFont(int column, int row, IWinFont* font) = 0;                  // [90] 0x2d0 0x1002591fc
    virtual bool SetCellColors(int column, int row, int index, std::uint32_t color) = 0;   // [91] 0x2d8 0x1002593b8
    virtual bool SetCellWrapping(int column, int row, int wrapping) = 0;                // [92] 0x2e0 0x100259544
    virtual bool SetCellAlignment(int column, int row, int hAlign, int vAlign) = 0;     // [93] 0x2e8 0x1002596c0
    virtual bool SetCellBorder(int column, int row, int flags, int color) = 0;          // [94] 0x2f0 0x100259856
    virtual bool GetCellType(int column, int row, int& type) = 0;                       // [95] 0x2f8 0x100256c1e
    virtual bool ClearCell(int column, int row) = 0;                                    // [96] 0x300 0x100256c54
    virtual bool ClearCellRect(int column, int row, int columns, int rows) = 0;         // [97] 0x308 0x100256ed4
    virtual bool ClearAllCells() = 0;                                                   // [98] 0x310 0x100256fd8
    virtual bool DeleteCell(int column, int row, int, int) = 0;                         // [99] 0x318 0x10025705a
    virtual bool DeleteColumns(int first, int count, int mode) = 0;                     // [100] 0x320 0x100257062
    virtual bool DeleteRows(int first, int count, int mode) = 0;                        // [101] 0x328 0x1002577be
    virtual bool CopyColumns(int destination, int source, int count, bool copy) = 0;    // [102] 0x330 0x100257bc8
    virtual bool CopyRows(int destination, int source, int count, bool copy) = 0;       // [103] 0x338 0x1002581c4
    virtual bool IsCellVisible(int column, int row) = 0;                                // [104] 0x340 0x10025845a
    virtual bool MakeCellVisible(int column, int row) = 0;                              // [105] 0x348 0x1002584aa
    virtual bool GetCellRect(int column, int row, cRZRect& out) = 0;                    // [106] 0x350 0x100258532
    virtual bool GetFirstVisibleCell(int& column, int& row) = 0;                        // [107] 0x358 0x10025864a
    virtual bool SetFirstVisibleCell(int column, int row) = 0;                          // [108] 0x360 0x100258662
    virtual bool GetVisibleColumnAndRowCount(float& columns, float& rows) = 0;          // [109] 0x368 0x1002586ac
    virtual bool DoCursorHitTest(int x, int y, CursorHitTestResult& result, CellCoordinates& cell) = 0;   // [110] 0x370
    virtual bool GetCellTextValue(int column, int row, cIGZString& out) = 0;            // [111] 0x378 0x1002599e6
    virtual bool SetCellTextValue(int column, int row, const char* text, std::uint32_t length, int mode,
                                  bool makeTextEdit) = 0;                               // [112] 0x380 0x100259ab8
    virtual bool GetCellBitmap(int column, int row, IWinBuffer*& out) = 0;              // [113] 0x388 0x10025a3f4
    virtual bool SetCellBitmap(int column, int row, IWinBuffer* bitmap, int mode) = 0;  // [114] 0x390 0x10025a432
    virtual bool GetCellWindow(int column, int row, cIGZWin*& out) = 0;                 // [115] 0x398 0x10025a5e6
    virtual bool SetCellWindow(int column, int row, cIGZWin* window, bool fitWidth, bool fitHeight, int mode) = 0;  // [116]
    virtual bool IsWindowACellWindow(cIGZWin* window, std::uint32_t id, int* column, int* row) = 0;   // [117] 0x3a8
    virtual void ResizeColumnToFitCellWindows(int column, int count) = 0;               // [118] 0x3b0 0x10025aa82
    virtual bool SetCellExtraData(int column, int row, void* data) = 0;                 // [119] 0x3b8 0x10025b392
    virtual void* GetCellExtraData(int column, int row) = 0;                            // [120] 0x3c0 0x10025b44a
    virtual bool SetColumnExtraData(int column, void* data) = 0;                        // [121] 0x3c8 0x10025b480
    virtual void* GetColumnExtraData(int column) = 0;                                   // [122] 0x3d0 0x10025b4d4
    virtual bool SetRowExtraData(int row, void* data) = 0;                              // [123] 0x3d8 0x10025b4fa
    virtual void* GetRowExtraData(int row) = 0;                                         // [124] 0x3e0 0x10025b54e
    virtual bool SetExtraData(void* data) = 0;                                          // [125] 0x3e8 0x10025b574
    virtual void* GetExtraData() = 0;                                                   // [126] 0x3f0 0x10025b58e
    virtual bool GetCellExtraDataObject(int column, int row, std::uint32_t iid, void** out) = 0;   // [127] 0x3f8
    virtual bool SetCellExtraDataObject(int column, int row, cIGZUnknown* object) = 0;  // [128] 0x400 0x10025b5fa
    virtual bool GetExtraDataObject(std::uint32_t iid, void** out) = 0;                 // [129] 0x408 0x10025b6a8
    virtual bool SetExtraDataObject(cIGZUnknown* object) = 0;                           // [130] 0x410 0x10025b6c6
    using SortFunction = int (*)(const CellData*, const CellData*);
    virtual void SortColumn(int column, int row, bool whole, SortFunction compare) = 0; // [131] 0x418 0x10025b758
    virtual void SortRow(int row, int column, bool whole, SortFunction compare) = 0;    // [132] 0x420 0x10025bd9e
    virtual bool FindCellText(const char* text, std::uint32_t length, int firstColumn, int columnCount, int firstRow,
                              int rowCount, int& column, int& row, bool exact) = 0;      // [133] 0x428 0x10025c584
    virtual bool SetOwnerDrawNotification(cIGZMessageTarget* target) = 0;               // [134] 0x430 0x10025c864
    virtual bool GetBufferToDrawTo(IWinBuffer*& buffer, cRZRect& area) = 0;             // [135] 0x438 0x10025c8a0
    virtual bool SetScrollBarImageVertical(IWinBuffer* image, const cRZRect& area) = 0; // [136] 0x440 0x10025d1b8
    virtual bool SetScrollBarImageHorizontal(IWinBuffer* image, const cRZRect& area) = 0;   // [137] 0x448 0x10025d270
    virtual bool SetSoundID(int which, int soundId) = 0;                                // [138] 0x450 0x10025c8c2
    virtual bool MakeDropDown() = 0;                                                    // [139] 0x458 0x10025213a
    virtual ~cIGZWinGrid() = default;                                                   // [140] 0x460 / [141] 0x468
};

// The text of a cell window (the original queries the window for cGZWinTextEdit, iid 0x231a1c57, and calls its
// vslot 0x170 / 0x180) and the construction of the text edit a cell gets when SetCellTextValue asks for one. Both
// are seams: cGZWinTextEdit is recovered but constructing it here would pull the whole editor into every grid user
// (original: 0x100259ab8 -> operator new(0x3c8) + cGZWinTextEdit::cGZWinTextEdit(0x11ff)).
class IWinGridHost {
public:
    virtual ~IWinGridHost() = default;
    virtual bool GetCellWindowText(cIGZWin* window, cIGZString& out) = 0;                        // QI 0x231a1c57 + vslot 0x170
    virtual bool SetCellWindowText(cIGZWin* window, const char* text, std::uint32_t length) = 0;   // vslot 0x180
    virtual cIGZWin* CreateCellTextEdit(cIGZWinMgr* mgr) = 0;                                    // 0x100259ab8
    // The scrollbar image is a cIGZBuffer in the original; the grid holds the window-control IWinBuffer view.
    virtual cIGZBuffer* AsScrollBarImage(IWinBuffer* image) = 0;                                  // 0x10025cd88 / 0x10025d1b8
};
IWinGridHost* WinGridHost();
void SetWinGridHost(IWinGridHost* host);

// Owner-draw notification ids (cGZMessage.type, p1 = the grid as cIGZWinGrid, p2 = OwnerDrawInfo*).
constexpr std::uint32_t kGZMsgGridDrawColumnHeader = 0xfa785cb0;
constexpr std::uint32_t kGZMsgGridDrawRowHeader = 0xfa785cb1;
constexpr std::uint32_t kGZMsgGridDrawCell = 0xfa785cb2;
// Selection / mouse notifications sent to the notification target (p1 = the grid, p2..).
constexpr std::uint32_t kGZMsgGridSelectionChanged = 0x9a1552d2;
constexpr std::uint32_t kGZMsgGridCellChanged = 0x9a1552d3;
constexpr std::uint32_t kGZMsgGridDoubleClick = 0x9a1552d4;
constexpr std::uint32_t kGZMsgGridKey = 0x9a1552d5;
constexpr std::uint32_t kGZMsgGridMouseDown = 0x9a1552d7;
// Scrollbar window ids and command id.
constexpr std::uint32_t kGridVerticalScrollBarId = 0x1a7c4511, kGridHorizontalScrollBarId = 0x1a7c4510;
constexpr std::uint32_t kGridCmdScrollbar = 0x887113a3;

// RZ::SS(): plays a sound by id for the grid's clicks (the application installs it).
void SetGridSoundPlayer(void (*player)(std::uint32_t soundId));

class cGZWinGrid : public cIGZWinGrid, public cGZWin {
public:
    explicit cGZWinGrid(cIGZWinMgr* mgr, int flags = 0);          // 0x10025086a (the original only takes the flags)
    ~cGZWinGrid() override;                                       // 0x100250c06 (+ deleting form 0x100250dd8)

    bool QueryInterface(std::uint32_t iid, void** out) override;  // 0x100250e14
    std::uint32_t AddRef() override { return ++refCount_; }        // 0x100250e5c
    std::uint32_t Release() override;                              // 0x100250e78
    bool Init() override;                                          // 0x100250f00
    bool Shutdown() override;                                      // 0x10025151e
    cIGZWin* AsIGZWin() override { return static_cast<cGZWin*>(this); }   // 0x100250ef6
    bool Refresh() override;                                       // 0x10025c9a8

    void SetArea(int l, int t, int r, int b) override;             // 0x100254b8c
    using cGZWin::SetArea;
    bool GetGridArea(cRZRect& out) override;                       // 0x1002517d4
    bool GetCellArea(cRZRect& out) override;                       // 0x100251848
    void SetFlag(int flag) override;                               // 0x100254ce6
    void SetFlags(int flags) override;                             // 0x100254d34
    void ClearFlag(int flag) override;                             // 0x100254d04
    bool IsFlagSet(int flag) override { return (flags_ & static_cast<std::uint32_t>(flag)) != 0; }   // 0x100254d24
    int GetFlags() override { return static_cast<int>(flags_); }   // 0x100254ea0
    std::uint32_t SetFlag(std::uint32_t flag, bool on) override;   // 0x100254c50
    using cGZWin::SetFlag;

    void SetDefaultFont(IWinFont* font) override;                  // 0x100254f00
    IWinFont* GetDefaultFont() override { return defaultFormat_.font; }   // 0x100254f72
    bool SetDefaultFontColor(std::uint32_t color, int index) override;    // 0x100254f80
    bool GetDefaultFontColor(std::uint32_t& color, int index) override;   // 0x100254f9c
    bool SetDefaultFontColorByColumns(std::uint32_t color, int index, int first, int count) override;   // 0x100254fba
    bool SetDefaultFontColorByRows(std::uint32_t color, int index, int first, int count) override;      // 0x1002555ce
    bool SetDefaultTextWrapping(bool wrap) override;               // 0x1002556a8
    bool GetDefaultTextWrapping() override;                        // 0x10025570e
    bool GetGutters(int& l, int& t, int& r, int& b) override;      // 0x100255722
    bool SetGutters(int l, int t, int r, int b) override;          // 0x10025574c
    bool GetCellGutters(int& l, int& t, int& r, int& b) override;  // 0x100255792
    bool SetCellGutters(int l, int t, int r, int b) override;      // 0x1002557bc
    void SetOutlineUse(bool on, std::uint32_t color) override;     // 0x1002557de
    std::uint32_t GetOutlineColor() override { return outlineColor_; }    // 0x10003abbc
    void PauseRecalculations(bool pause) override;                 // 0x10025580a
    bool ActLikeAListbox(int columnLimit, int selectionLimit, bool selectFirst) override;   // 0x100255824
    bool ScrollCellsVertically(int rows) override;                 // 0x1002558f0
    bool ScrollCellsHorizontally(int columns) override;            // 0x100255a42
    void ScrollPagesVertically(int pages) override;                // 0x100255b92
    void ScrollPagesHorizontally(int pages) override;              // 0x100255ba6
    bool ScrollToColumn(int column) override;                      // 0x100255bba
    bool ScrollToRow(int row) override;                            // 0x100255c50
    bool ScrollToCell(int column, int row) override;               // 0x100255cee
    bool GetSelectionRules(int& columnRule, int& selectionLimit) override;   // 0x100255d1a
    bool SetSelectionRules(int columnRule, int selectionLimit) override;  // 0x100255d32
    int GetSelectionCount() override { return static_cast<int>(selection_.size()); }   // 0x100255d50
    bool GetSelection(int& column, int& row) override;             // 0x100255d5c
    bool GetSelection(CellCoordinates* cells, int& count) override;   // 0x100255d8a
    bool SetSelection(int column, int row) override;               // 0x100255dcc
    bool AddCellToSelection(int column, int row, bool add) override;   // 0x100255e62
    bool GetCellIsSelected(int column, int row) override;          // 0x100256040
    bool ClearSelection() override;                                // 0x1002560c6
    bool EnableFixedRanges(bool columns, bool rows) override;      // 0x100256110
    bool SetCellLimits(int columns, int rows) override;            // 0x100256174
    bool GetCellLimits(int& columns, int& rows) override;          // 0x100256188
    bool GetCellUsedRange(cRZRect& out) override;                  // 0x1002561a0
    bool GetCellVisibleRange(cRZRect& out) override;               // 0x10025639e
    bool IsColumnWithinBounds(int column) override;                // 0x1002563da
    bool IsRowWithinBounds(int row) override;                      // 0x1002563fe
    bool IsCellWithinBounds(int column, int row) override;         // 0x100256422
    bool GetMostRecentlyAddedCell(int& column, int& row) override; // 0x10025645a
    bool SetBackgroundColor(std::uint32_t native) override;        // 0x100256476
    bool SetBackgroundBitmap(IWinBuffer* bitmap) override;         // 0x100256488
    bool SetGridColors(int columnLineNative, int rowLineNative) override;   // 0x1002564c4
    bool GetGridColors(int& columnLine, int& rowLine) override;    // 0x10003abc8
    bool SetDefaultTextAlignment(int alignment) override;          // 0x10025650e
    bool SetDefaultTextAlignmentByColumns(int first, int count, int alignment) override;   // 0x10025651c
    bool SetDefaultTextAlignmentByRows(int first, int count, int alignment) override;      // 0x1002565d4
    int GetDefaultTextAlignment() override { return defaultTextAlignment_; }   // 0x10025d558
    void SetDefaultColumnWidth(int width) override { defaultColumnWidth_ = width; }   // 0x10025668c
    bool SetColumnWidth(int first, int count, int width) override;    // 0x100256698
    void SetDefaultRowHeight(int height) override { defaultRowHeight_ = height; }      // 0x100256758
    bool SetRowHeight(int first, int count, int height) override;     // 0x100256764
    int GetDefaultColumnWidth() override { return defaultColumnWidth_; }   // 0x10025d564
    int GetDefaultRowHeight() override { return defaultRowHeight_; }       // 0x10025d570
    int GetColumnWidth(int first, int count) override;              // 0x10025d328
    int GetRowHeight(int first, int count) override;                // 0x10025d37a
    bool GetColumnHeaderArea(int column, cRZRect& out) override;    // 0x100256824
    bool GetRowHeaderArea(int row, cRZRect& out) override;          // 0x1002568d6
    bool EnableHeaders(bool columns, bool rows) override;           // 0x100256996
    bool GetHeaderSizes(int& columnHeaderHeight, int& rowHeaderWidth) override;   // 0x1002569e2
    bool SetHeaderSizes(int columnHeaderHeight, int rowHeaderWidth) override;     // 0x1002569fa
    bool SetHeadingFormat(const CellFormat& format) override;       // 0x100256b04
    bool GetColumnHeading(int column, cIGZString& out) override;    // 0x100256a9c
    bool SetColumnHeading(int column, const cIGZString& text) override;   // 0x100256ad0
    bool GetRowHeading(int row, cIGZString& out) override;          // 0x100256a34
    bool SetRowHeading(int row, const cIGZString& text) override;   // 0x100256a68
    bool ClearAllRowHeadings() override;                            // 0x100256b9a
    bool ClearAllColumnHeadings() override;                         // 0x100256bdc
    bool GetCellFormat(int column, int row, CellFormat& out) override;   // 0x1002586c4
    bool SetCellFormat(int column, int row, const CellFormat& format) override;   // 0x1002589d2
    bool ClearCellFormat(int column, int row) override;            // 0x100259096
    void GetColCellFormatExtra(int column, CellFormatExtra*& out) override;   // 0x100259174
    void GetRowCellFormatExtra(int row, CellFormatExtra*& out) override;      // 0x10025918e
    bool GetColRowCellFormatExtra(int column, int row, CellFormatExtra*& out) override;   // 0x1002591ac
    bool SetCellFont(int column, int row, IWinFont* font) override;   // 0x1002591fc
    bool SetCellColors(int column, int row, int index, std::uint32_t color) override;   // 0x1002593b8
    bool SetCellWrapping(int column, int row, int wrapping) override;   // 0x100259544
    bool SetCellAlignment(int column, int row, int hAlign, int vAlign) override;   // 0x1002596c0
    bool SetCellBorder(int column, int row, int flags, int color) override;        // 0x100259856
    bool GetCellType(int column, int row, int& type) override;     // 0x100256c1e
    bool ClearCell(int column, int row) override;                  // 0x100256c54
    bool ClearCellRect(int column, int row, int columns, int rows) override;   // 0x100256ed4
    bool ClearAllCells() override;                                 // 0x100256fd8
    bool DeleteCell(int, int, int, int) override { return false; }  // 0x10025705a (a stub in the original)
    bool DeleteColumns(int first, int count, int mode) override;   // 0x100257062
    bool DeleteRows(int first, int count, int mode) override;      // 0x1002577be
    bool CopyColumns(int destination, int source, int count, bool copy) override;   // 0x100257bc8
    bool CopyRows(int destination, int source, int count, bool copy) override;      // 0x1002581c4
    bool IsCellVisible(int column, int row) override;              // 0x10025845a
    bool MakeCellVisible(int column, int row) override;            // 0x1002584aa
    bool GetCellRect(int column, int row, cRZRect& out) override;  // 0x100258532
    bool GetFirstVisibleCell(int& column, int& row) override;      // 0x10025864a
    bool SetFirstVisibleCell(int column, int row) override;        // 0x100258662
    bool GetVisibleColumnAndRowCount(float& columns, float& rows) override;   // 0x1002586ac
    bool DoCursorHitTest(int x, int y, CursorHitTestResult& result, CellCoordinates& cell) override;   // 0x100252c94
    bool GetCellTextValue(int column, int row, cIGZString& out) override;   // 0x1002599e6
    bool SetCellTextValue(int column, int row, const char* text, std::uint32_t length, int mode,
                          bool makeTextEdit) override;             // 0x100259ab8
    bool GetCellBitmap(int column, int row, IWinBuffer*& out) override;   // 0x10025a3f4
    bool SetCellBitmap(int column, int row, IWinBuffer* bitmap, int mode) override;   // 0x10025a432
    bool GetCellWindow(int column, int row, cIGZWin*& out) override;      // 0x10025a5e6
    bool SetCellWindow(int column, int row, cIGZWin* window, bool fitWidth, bool fitHeight, int mode) override;   // 0x10025a624
    bool IsWindowACellWindow(cIGZWin* window, std::uint32_t id, int* column, int* row) override;   // 0x10025a9c8
    void ResizeColumnToFitCellWindows(int column, int count) override;   // 0x10025aa82
    bool SetCellExtraData(int column, int row, void* data) override;      // 0x10025b392
    void* GetCellExtraData(int column, int row) override;                 // 0x10025b44a
    bool SetColumnExtraData(int column, void* data) override;             // 0x10025b480
    void* GetColumnExtraData(int column) override;                        // 0x10025b4d4
    bool SetRowExtraData(int row, void* data) override;                   // 0x10025b4fa
    void* GetRowExtraData(int row) override;                              // 0x10025b54e
    bool SetExtraData(void* data) override;                               // 0x10025b574
    void* GetExtraData() override { return extraData_; }                  // 0x10025b58e
    bool GetCellExtraDataObject(int column, int row, std::uint32_t iid, void** out) override;   // 0x10025b59c
    bool SetCellExtraDataObject(int column, int row, cIGZUnknown* object) override;   // 0x10025b5fa
    bool GetExtraDataObject(std::uint32_t iid, void** out) override;      // 0x10025b6a8
    bool SetExtraDataObject(cIGZUnknown* object) override;                // 0x10025b6c6
    void SortColumn(int column, int row, bool whole, SortFunction compare) override;   // 0x10025b758
    void SortRow(int row, int column, bool whole, SortFunction compare) override;      // 0x10025bd9e
    bool FindCellText(const char* text, std::uint32_t length, int firstColumn, int columnCount, int firstRow,
                      int rowCount, int& column, int& row, bool exact) override;   // 0x10025c584
    bool SetOwnerDrawNotification(cIGZMessageTarget* target) override;    // 0x10025c864
    bool GetBufferToDrawTo(IWinBuffer*& buffer, cRZRect& area) override;  // 0x10025c8a0
    using cGZWin::GetBufferToDrawTo;
    bool SetScrollBarImageVertical(IWinBuffer* image, const cRZRect& area) override;   // 0x10025d1b8
    bool SetScrollBarImageHorizontal(IWinBuffer* image, const cRZRect& area) override; // 0x10025d270
    bool SetSoundID(int which, int soundId) override;                     // 0x10025c8c2
    virtual bool PlaySoundA(int which);                                   // 0x10025c8de (primary vtable [156])
    bool MakeDropDown() override;                                         // 0x10025213a
    virtual bool GetMatrixData(rz::cRZSparseMatrix<CellData*>*& out) { out = &cells_; return true; }   // [155] 0x4d8

    // window overrides the original also places in the grid vtable
    bool DoMessage(cGZMessage& message) override;                  // 0x100251710
    bool GZPaint() override;                                       // 0x100252f70 (simplified, see the cpp)
    bool GZOnCharacter(char character) override;                   // 0x100251928
    bool GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;   // 0x100251940
    bool GZOnKeyUp(std::uint32_t key, std::uint32_t modifiers) override;     // 0x100251e8e
    bool GZOnMouseDownL(int x, int y, std::uint32_t modifiers) override;     // 0x10025237e
    bool GZOnMouseUpL(int x, int y, std::uint32_t modifiers) override;       // 0x10025214e
    bool GZOnMouseMove(int x, int y, std::uint32_t modifiers) override;      // 0x100252930
    bool GZOnMouseWheel(int x, int y, std::uint32_t modifiers, int delta) override;   // 0x100252c3e
    bool GZOnSetFocus(cIGZWin* previous) override;                 // 0x100252ec4
    bool GZOnKillFocus(cIGZWin* next) override;                    // 0x100252f1a
    void SetFillColor(std::uint32_t nativeColor) override;         // 0x100254eac

protected:
    // --- original helpers (not in the interface) ---
    void SetDefaultFormatting();                                    // 0x100250f5a
    void RecalculateAll();                                          // 0x10025121c
    void DisposeOfAllCellData();                                    // 0x100251626
    bool DoArrowKey(std::uint32_t key, std::uint32_t modifiers);    // 0x100251ba8
    void SetScrollBarValues();                                      // 0x100251f26
    void SetScrollBarPositions();                                   // 0x10025cfec
    bool TestForPossibleScrollBarAdditionOrRemoval();               // 0x10025c9c6
    bool AddScrollBar(bool vertical, bool horizontal);              // 0x10025cd88
    bool RemoveScrollBar(bool vertical, bool horizontal);           // 0x100254de2
    int GetColumnWidthInternal(int column);                         // 0x100254054
    int GetRowHeightInternal(int row);                              // 0x1002540d4
    int CalculateCellHeight(int column, int row, CellData* cell);   // 0x10025d3cc
    void DrawCellBackground(OwnerDrawInfo& info);                   // 0x100254376
    void CalcCellDrawingParams(const cIGZString& text, const OwnerDrawInfo& info, cRZRect& rect,
                               std::uint32_t& style, std::uint32_t& color, std::uint32_t& color2);   // 0x1002544dc
    void DrawCell(OwnerDrawInfo& info);                             // 0x100254676
    void SetSizeCachedActualForColumn(int first, int count, int size);   // 0x1002574fc
    void SetSizeCachedActualForRow(int first, int count, int size);      // 0x1002579dc
    bool MakeRoomForColumn(int column, int delta, int mode);        // 0x1002576f8
    bool MakeRoomForRow(int row, int delta, int mode);              // 0x100257b2e
    bool MakeRoom(int column, int row, int delta, int mode);        // 0x100259eb4
    bool CopyCellData(int sourceColumn, int sourceRow, int destColumn, int destRow, bool copy);   // 0x100257f14
    void CopyCellData(int sourceColumn, int sourceRow, CellData* source, int destColumn, int destRow,
                      CellData* dest);                              // 0x10025cbd6
    void DisposeOfCellData(int column, int row, CellData* cell, bool freeCell);   // 0x100256da6
    void DoWindowAddition(int column, int row, cIGZWin* window, bool fitWidth, bool fitHeight);   // 0x10025a884
    void DoWindowRemoval(int column, int row, cIGZWin* window);     // 0x10025af30
    void PlaceCellWindow(int column, int row, cIGZWin* window, bool fitWidth, bool fitHeight);    // 0x10025ae02
    void PlaceAllCellWindows();                                     // 0x10025afa6
    void DoCellWindowMove(CellData* cell);                          // 0x10025b156
    static int DefaultSortFunction(const CellData* first, const CellData* second);   // 0x10025b70c

    // format helpers
    static void ApplyFormat(CellFormat& destination, const CellFormat& source);
    CellData* FindCell(int column, int row);                       // the stored cell, null when unset
    CellData** FindCellSlot(int column, int row);                  // the matrix slot of a cell, null when unset
    CellData* EnsureCell(int column, int row);                     // creates an empty cell entry when missing
    CellFormat* EnsureCellFormat(CellData* cell);                  // creates the cell's format when missing
    void EmitNotification(std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3);
    void MarkDirty();
    void MarkInvalid();

    // --- state (offsets of the original object) ---
    std::uint32_t flags_ = 0;                       // +0x158
    std::int32_t logLevel_ = 1;                     // +0x15c
    bool recalculatePending_ = true;                // +0x160
    std::int32_t recalcPause_ = 0;                  // +0x164
    bool invalidatePending_ = false;                // +0x168
    std::int32_t gutterLeft_ = 5, gutterTop_ = 5, gutterRight_ = 5, gutterBottom_ = 5;   // +0x16c
    std::int32_t cellGutterLeft_ = 1, cellGutterTop_ = 1, cellGutterRight_ = 1, cellGutterBottom_ = 1;   // +0x17c
    std::int32_t columnHeaderHeight_ = 20, rowHeaderWidth_ = 5;   // +0x18c, +0x190
    std::int32_t gridAreaWidth_ = 1, gridAreaHeight_ = 1;         // +0x194, +0x198
    std::int32_t cellAreaWidth_ = 1, cellAreaHeight_ = 1;         // +0x19c, +0x1a0
    std::int32_t firstVisibleColumn_ = 0, firstVisibleRow_ = 0;   // +0x1a4, +0x1a8
    float visibleColumns_ = 1.0f, visibleRows_ = 1.0f;            // +0x1ac, +0x1b0
    std::int32_t columnLimit_ = -1, rowLimit_ = -1;               // +0x1b4, +0x1b8
    std::int32_t defaultColumnWidth_ = 100, defaultRowHeight_ = 0x14;    // +0x1bc, +0x1c0 (20: Mac i386 ctor 0x001b8be8 stores 0x14)
    std::uint32_t outlineColor_ = 0;                              // +0x1c4
    std::uint32_t columnLineColor_ = 0, rowLineColor_ = 0;        // +0x1c8, +0x1cc
    std::int32_t lineOffsetX_ = 1, lineOffsetY_ = 1;              // +0x1d0, +0x1d4
    std::int32_t mostRecentColumn_ = -1, mostRecentRow_ = -1;     // +0x1d8, +0x1dc
    IWinBuffer* background_ = nullptr;                            // +0x1e0
    cIGZMessageTarget* ownerDrawTarget_ = nullptr;                // +0x1e8
    cRZTimer clickTimer_{cRZTimer::kMilliseconds};                // +0x1f0
    std::int32_t extraDataMode_ = 0;                              // +0x208 (1 raw, 2 COM)
    std::int32_t selectionColumnRule_ = 1;                        // +0x20c
    std::int32_t selectionLimit_ = 0x7fffffff;                    // +0x210
    std::list<CellCoordinates> selection_;                        // +0x218 (+0x228 size)
    std::int32_t defaultTextAlignment_ = 2;                       // +0x230
    rz::cRZSparseMatrix<CellFormatExtra> columnFormats_;          // +0x238 (key row 0, column index)
    rz::cRZSparseMatrix<CellFormatExtra> rowFormats_;             // +0x268 (key column 0, row index)
    CellFormat defaultFormat_;                                    // +0x2c8
    struct CellWindow {                                           // value of cellWindows_ (+0x30 is the key window)
        std::int32_t type = 1;                                    // +0x20
        std::int32_t column = 0;                                  // +0x24
        std::int32_t row = 0;                                     // +0x28
        std::int16_t reserved = 0;                                // +0x2c
        std::int16_t reserved2 = 0;                               // +0x2e
        cIGZWin* window = nullptr;                                // +0x30
        std::int32_t flags = 0;                                   // +0x38
        std::int32_t reserved4 = 0;                               // +0x3c
        void* reserved5 = nullptr;                                // +0x40
        std::int32_t reserved6 = 0;                               // +0x48
    };
    rz::cRZSparseMatrix<CellData*> cells_;                        // +0x298
    std::map<cIGZWin*, CellWindow> cellWindows_;                  // +0x2f0
    std::int32_t lastHitResult_ = 0, lastHitColumn_ = -1, lastHitRow_ = -1;      // +0x308, +0x30c, +0x310
    std::int32_t lastHoverResult_ = -1, lastHoverColumn_ = -1, lastHoverRow_ = -1;   // +0x314, +0x318, +0x31c
    std::map<std::int32_t, void*> rowExtraData_;                  // +0x328
    std::map<std::int32_t, void*> columnExtraData_;               // +0x338
    void* extraData_ = nullptr;                                   // +0x350
    cIGZWinScrollbar* verticalScrollBar_ = nullptr;                // +0x358
    cIGZWinScrollbar* horizontalScrollBar_ = nullptr;              // +0x360
    IWinBuffer* verticalScrollImage_ = nullptr;                    // +0x368
    IWinBuffer* horizontalScrollImage_ = nullptr;                  // +0x370
    cRZRect verticalScrollImageArea_{0, 0, 0, 0};                  // +0x378
    cRZRect horizontalScrollImageArea_{0, 0, 0, 0};                // +0x388
    std::int32_t soundId_[3] = {0, 0, 0};                          // +0x398
    std::map<std::int32_t, cRZString> columnHeadings_;             // +0x3a8
    std::map<std::int32_t, cRZString> rowHeadings_;                // +0x3c0
    CellFormat headingFormat_;                                    // +0x3d8
};
static_assert(sizeof(cIGZWinGrid::CellFormat) == 0x28, "CellFormat is 0x28 bytes");
static_assert(sizeof(cIGZWinGrid::CellData) == 0x30, "CellData is 0x30 bytes");
static_assert(sizeof(cIGZWinGrid::CellFormatExtra) == 0x30, "CellFormatExtra is 0x30 bytes");

}  // namespace gz
