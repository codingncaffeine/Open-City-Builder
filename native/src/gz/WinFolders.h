// cGZWinFolders — the folder tab strip of the file browser (23 functions, 0x100512b82..0x100513cba; interface
// cIGZWinFolders iid 0xa13351b8 for the view at +0, cGZWin sub-object at +8). The file browser fills it with folder
// names (AddFolder), asks it to lay the tabs out (RecalcTabs) and reads back / sets the selected folder.
//
// Layout of the original (0x1a8 bytes; a cIGZWinFolders primary vtable at +0 and the 0x150-byte cGZWin at +8):
//   +0x158 std::list<tFolderTab>  the folders in add order (this libc++ keeps a node's prev / next at +0 / +8 and a
//                                 list object's last node at +0, its first node at +8 and the size at +0x10: so
//                                 +0x158 = last (newest) node, +0x160 = first (oldest) node, +0x168 = size; AddFolder
//                                 appends and every walk starts at +0x160, which is add order)
//   +0x170 std::list<cIGZWinFolderRow*>  the tab rows (+0x170 = last node, +0x178 = first node, +0x180 = size), every
//                                 row a child window of the cGZWin at +8; GZPaint walks from the last node (y = 0) to
//                                 the first one, so the last row created is the top row and the first node is the
//                                 bottom row
//   +0x188 cIGZFont*              the font the tabs are measured with (counted)
//   +0x190 cIGZBuffer*            the outline bitmap that frames each tab (counted)
//   +0x198 int                    that bitmap's width / 2
//   +0x19c int                    that bitmap's height (the row pitch is height - 4)
//   +0x1a0 bool                   the tabs must be laid out again (set by AddFolder / DeleteAllFolders / SetFont /
//                                 SetOutlineBmp, cleared at the end of RecalcTabs)
//   +0x1a4 int                    number of rows RecalcTabs built (mirrors the +0x170 list size)
//
// Layout model: RecalcTabs measures every tab (font text width + 10 for the two half-outline ends + 2 * gutter, the
// gutter being max(bitmap width / 8, 4)), refuses when the widest tab does not fit in the window, and otherwise builds
// ceil(total / window width) rows. Each row gets the control's width and the bitmap height and is added as a child
// window; the tabs are then offered to the rows in order with one cursor that carries over from tab to tab and wraps
// back to the first row (a new row is appended when it runs past the last one and still finds no room). The rows are
// placed by GZPaint from the last node of the list (y = 0) to the first (y = pitch * (count - 1)), so the first row
// created is the bottom one and the first tabs land there.
//
// Quirks kept: a click moves the row it hit to the front of the row list (GZPaint then draws it last, at the bottom)
// and only manages the row selection state — the row's own GZOnMouseDownL decides which tab was hit. Selecting a folder
// with SetSelectedFolder moves its row the same way but does not invalidate the window, so the new selection is only
// repainted with the next invalidation. GetFolderByIndex counts from the front (the first folder added), the
// destructor neither releases the font / outline bitmap (Shutdown does) nor deletes the rows (the cGZWin base deletes
// its children), and AddFolder copies 16 bytes of tFolderTab (+0x28 / +0x30) that the original never initialises and
// nothing reads (they are zero here). GZOnMouseDownL is guarded where the original divides by (bitmap height - 4) and
// dereferences the empty row list (no outline bitmap set), RecalcTabs is guarded where the original divides by the
// window width, Shutdown drops the row list before the base deletes the child windows (the original leaves it
// dangling, which a Shutdown + Init cycle would walk), and Init returns true even when RecalcTabs fails.
//
// The tab rows (cGZWinFolderRow, gz/WinFolderRow.h) are created through IWinFoldersHost (see docs/SEAMS.md), whose
// default makes the real row; the control only uses the cIGZWinFolderRow interface below.
#pragma once
#include <cstdint>
#include <list>

#include "gz/Font.h"
#include "gz/Win.h"
#include "rz/cRZString.h"

class cIGZBuffer;

namespace gz {

constexpr std::uint32_t kGZIID_cIGZWinFolders = 0xa13351b8;   // QueryInterface id of the +0 interface
constexpr std::uint32_t kFoldersOutlineBitmapId = 0xe8963edf; // RZ::WCM() vslot 0x38: the tab outline bitmap
// The command the control sends to its notification target when a row is clicked (cGZWin::SendMsg slot 0x488):
// param1 = this id, param2 = GetID() of the control, param3 = 0.
constexpr std::uint32_t kFoldersCmdFolderSelected = 0xe87111be;

// One tab (tFolderTab, 0x40 bytes in the original): the name, the tab's area inside its row and the caller's id. The
// control's AddFolder copies the area without setting it (nothing in the control reads it); cGZWinFolderRow::AddTab
// fills it with the tab's place in the row.
struct tFolderTab {
    cRZString name;                  // +0x00 (0x28 bytes)
    cRZRect area;                    // +0x28 (left, top, right, bottom)
    std::uint32_t id = 0;            // +0x38
};

// ---- seam: the tab row ----------------------------------------------------------------------------------------
// cIGZWinFolderRow (vtable 0x100ac07e0): the interface of one row of tabs (cGZWinFolderRow, gz/WinFolderRow.h);
// the control only calls the slots below.
class cIGZWinFolderRow : public cIGZUnknown {
public:
    virtual bool CanThisTabBeAdded(const cIGZString* name) = 0;            // [3]  0x18
    virtual bool AddTab(const cIGZString* name, std::uint32_t id) = 0;     // [4]  0x20 (false when the row is full)
    virtual void StretchToFit() = 0;                                       // [5]  0x28
    virtual void SetFont(cIGZFont* font) = 0;                              // [6]  0x30
    virtual void ClearSelection() = 0;                                     // [7]  0x38
    virtual int GetSelection() = 0;                                        // [8]  0x40 (-1 = none)
    virtual bool GetSelectedTabName(cIGZString& out) = 0;                  // [9]  0x48
    virtual bool GetSelectedTabID(std::uint32_t& out) = 0;                 // [10] 0x50
    virtual bool SelectTab(const cIGZString& name) = 0;                    // [11] 0x58
    virtual bool SelectTab(std::uint32_t id) = 0;                          // [12] 0x60
    virtual bool Init() = 0;                                               // [13] 0x68 (cGZWin::Init override)
    virtual bool Shutdown() = 0;                                           // [14] 0x70 (cGZWin::Shutdown override)
    virtual bool GZPaint() = 0;                                            // [15] 0x78
    virtual bool GZOnMouseDownL(int x, int y, std::uint32_t modifiers) = 0;   // [16] 0x80
    virtual ~cIGZWinFolderRow() = default;                                 // [17] / [18] 0x88 / 0x90
};

// A created row: the interface (row +0) and the row's window (the cGZWin sub-object at row +8, which the original
// hands straight to cGZWin::ChildAdd / ChildDelete / GZWinMoveTo).
struct WinFolderRow {
    cIGZWinFolderRow* row = nullptr;
    cIGZWin* window = nullptr;
};

// What the control needs from its unrecovered neighbours and the engine globals.
class IWinFoldersHost {
public:
    virtual ~IWinFoldersHost() = default;
    // RZ::FO() != 0, then RZ::WCM() vslot 0x28 with style 0x68963c4c: the default font, borrowed (SetFont takes a
    // reference). Null when the font manager is missing.
    virtual cIGZFont* DefaultFont() = 0;
    // RZ::WCM() vslot 0x38 with image 0xe8963edf: the tab outline bitmap, borrowed (Init takes a reference).
    virtual cIGZBuffer* DefaultOutlineBitmap() = 0;
    // new cGZWinFolderRow (0x198 bytes) + the cGZWin constructor + SetFont (vslot 0x30) + cGZWinFolderRow::
    // SetOutlineBmp (@0x100512260); returns one reference (the original allocates it with a zero count, AddRefs it,
    // child-adds it and Release()s it again, leaving the child list as the owner). `mgr` is the control's window
    // manager (the original's cGZWin constructor registers with the global one).
    virtual WinFolderRow CreateFolderRow(cIGZWinMgr* mgr, cIGZFont* font, cIGZBuffer* outline) = 0;
};
IWinFoldersHost* WinFoldersHost();
void SetWinFoldersHost(IWinFoldersHost* host);

// cIGZWinFolders (vtable 0x100b0db70, the concrete one at 0x100ac0d58): slots 3.. form the interface; the last four
// are the cGZWin overrides, which the original also keeps in the cGZWin vtable at +8 (slots 0x20 / 0x28 / 0x2c0 with
// this-adjusting thunks).
class cIGZWinFolders : public cIGZUnknown {
public:
    virtual cIGZWin* AsIGZWin() = 0;                                        // [3]  0x18
    virtual bool AddFolder(const cIGZString& name, std::uint32_t id) = 0;   // [4]  0x20
    virtual bool SetSelectedFolder(const cIGZString& name) = 0;             // [5]  0x28
    virtual bool SetSelectedFolder(std::uint32_t id) = 0;                   // [6]  0x30
    virtual void DeleteAllFolders() = 0;                                    // [7]  0x38
    virtual bool GetSelectedFolderName(cIGZString& out) = 0;                // [8]  0x40
    virtual bool GetSelectedFolderID(std::uint32_t& out) = 0;               // [9]  0x48
    virtual void SetFont(cIGZFont* font) = 0;                               // [10] 0x50
    virtual void SetOutlineBmp(cIGZBuffer* bitmap) = 0;                     // [11] 0x58
    virtual bool RecalcTabs() = 0;                                          // [12] 0x60
    virtual void GetCount(std::uint32_t& out) = 0;                          // [13] 0x68
    virtual bool GetFolderByIndex(std::uint32_t index, cIGZString& out) = 0;   // [14] 0x70
    virtual bool Init() = 0;                                                // [15] 0x78
    virtual bool Shutdown() = 0;                                            // [16] 0x80
    virtual bool GZPaint() = 0;                                             // [17] 0x88
    virtual bool GZOnMouseDownL(int x, int y, std::uint32_t modifiers) = 0; // [18] 0x90
    virtual ~cIGZWinFolders() = default;                                    // [19] / [20] 0x98 / 0xa0
};

class cGZWinFolders : public cIGZWinFolders, public cGZWin {
public:
    explicit cGZWinFolders(cIGZWinMgr* mgr);   // @ 0x100512b82 (C2) / @ 0x100512c80 (C1)
    ~cGZWinFolders() override;                 // @ 0x100513b46 (body) / @ 0x100513b9c (deleting)

    // cIGZUnknown (both interfaces; the original forwards to the cGZWin sub-object)
    bool QueryInterface(std::uint32_t iid, void** out) override;     // @ 0x100512cba
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }     // @ 0x100512c8a
    std::uint32_t Release() override { return cGZWin::Release(); }   // @ 0x100512ca2

    cIGZWin* AsIGZWin() override { return static_cast<cGZWin*>(this); }        // @ 0x1005130d4
    bool AddFolder(const cIGZString& name, std::uint32_t id) override;         // @ 0x1005130de
    bool SetSelectedFolder(const cIGZString& name) override;                   // @ 0x1005131ba
    bool SetSelectedFolder(std::uint32_t id) override;                         // @ 0x100513270
    void DeleteAllFolders() override;                                          // @ 0x100513868
    bool GetSelectedFolderName(cIGZString& out) override;                      // @ 0x10051316a
    bool GetSelectedFolderID(std::uint32_t& out) override;                     // @ 0x100513192
    void SetFont(cIGZFont* font) override;                                     // @ 0x1005138da
    void SetOutlineBmp(cIGZBuffer* bitmap) override;                           // @ 0x10051393c
    bool RecalcTabs() override;                                                // @ 0x100513326
    void GetCount(std::uint32_t& out) override;                                // @ 0x1005138cc
    bool GetFolderByIndex(std::uint32_t index, cIGZString& out) override;      // @ 0x10051388c
    bool Init() override;                                                      // @ 0x100512d02
    bool Shutdown() override;                                                  // @ 0x100512ddc
    bool GZPaint() override;                                                   // @ 0x100513046
    bool GZOnMouseDownL(int x, int y, std::uint32_t modifiers) override;       // @ 0x100512e40

    // state for the tests / the future control code
    std::size_t TabCount() const { return tabs_.size(); }
    std::size_t RowCount() const { return rows_.size(); }
    int RowCountField() const { return rowCount_; }        // +0x1a4
    int BitmapWidth() const { return bitmapWidth_; }       // +0x198
    int BitmapHeight() const { return bitmapHeight_; }     // +0x19c
    bool NeedsRecalc() const { return needsRecalc_; }      // +0x1a0
    cIGZFont* Font() const { return font_; }               // +0x188
    cIGZBuffer* OutlineBmp() const { return outlineBmp_; } // +0x190
    const std::list<WinFolderRow>& Rows() const { return rows_; }   // +0x170, front = bottom row

private:
    // The row construction the original inlines twice (0x1005134db and 0x100513697): create the row, give it the
    // control's width and the bitmap height, add it as a child window, flag it and append it to the row list.
    bool CreateRow();

    std::list<tFolderTab> tabs_;         // +0x158
    std::list<WinFolderRow> rows_;       // +0x170
    cIGZFont* font_ = nullptr;           // +0x188
    cIGZBuffer* outlineBmp_ = nullptr;   // +0x190
    int bitmapWidth_ = 0;                // +0x198
    int bitmapHeight_ = 0;               // +0x19c
    bool needsRecalc_ = true;            // +0x1a0
    int rowCount_ = 0;                   // +0x1a4
};

}  // namespace gz
