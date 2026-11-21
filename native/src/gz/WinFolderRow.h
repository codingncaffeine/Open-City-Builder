// cGZWinFolderRow — one row of folder tabs inside cGZWinFolders (21 functions, Mac x86_64 0x100512260..0x100513b46,
// Mac i386 0x00399f70..0x0039b076; interface cIGZWinFolderRow iid 0xa13351a3 at +0, cGZWin sub-object behind it).
//
// The row owns its own tab list: the parent control offers each folder with AddTab (which refuses a tab that would
// run past the row's width), then asks the row to StretchToFit (the leftover pixels are handed out one at a time,
// round robin from the first tab, every later tab moving right by the same pixel). The row paints the tabs from the
// outline bitmap: its left half frames an unselected tab, its right half the selected one, each half being a left
// cap, a middle that is tiled and a right cap, the caps max(half / 8, 4) pixels wide. The selected tab is painted last
// and spreads 2 pixels into its neighbors (not past the row's ends). A click selects the tab under it and is then
// forwarded to the parent control in the parent's coordinates, which picks the row and notifies its target.
//
// Layout of the original (i386 offsets; the x86_64 build keeps the same order):
//   +0x00 cIGZWinFolderRow vtable, +0x04 cGZWin
//   +0xe8 cIGZBuffer*  outline bitmap (counted)    +0xec int  bitmap width / 2    +0xf0 int  bitmap height
//   +0xf4 std::list<tFolderTab> the tabs           +0xfc int  5 (set by the constructor, never read by the row)
//   +0x100 cIGZFont*   the tab font (counted)      +0x104 int selected tab (-1 = none)    +0x108 int tab count
//
// Quirks kept: Init falls back to FontAcquire(9, 15, 0, 0, reuse) and the stock outline bitmap only when none was set;
// CanThisTabBeAdded accepts a first tab wider than the row (AddTab, which is what the parent calls, still refuses it);
// a click outside every tab keeps the previous selection and is still forwarded; the destructor only frees the list
// (Shutdown releases the font and the bitmap). Guards: CanThisTabBeAdded answers false without a font, and GZPaint
// draws nothing without a draw context or an outline bitmap (the original dereferences all three). A forwarded click
// carries the modifiers unshifted in param3, so the parent reads them as 0 (the original's SendMsg call does the same).
#pragma once
#include <cstdint>
#include <list>

#include "gz/WinFolders.h"

namespace gz {

constexpr std::uint32_t kGZIID_cIGZWinFolderRow = 0xa13351a3;
constexpr int kFolderOutlineWidth = 4;   // _kFolderOutlineWidth, the smallest cap width (a float 4.0 in the original)

class cGZWinFolderRow : public cIGZWinFolderRow, public cGZWin {
public:
    explicit cGZWinFolderRow(cIGZWinMgr* mgr);   // @ i386 0x0039b076
    ~cGZWinFolderRow() override = default;       // @ i386 0x0074d058 / 0x0074d0b0 (list, then the cGZWin base)

    // cIGZUnknown (forwarded to the cGZWin sub-object)
    bool QueryInterface(std::uint32_t iid, void** out) override;    // @ i386 0x00399f8c
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }    // @ i386 0x00399f70
    std::uint32_t Release() override { return cGZWin::Release(); }  // @ i386 0x00399f7e

    bool CanThisTabBeAdded(const cIGZString* name) override;            // @ i386 0x0039a704
    bool AddTab(const cIGZString* name, std::uint32_t id) override;     // @ i386 0x0039ad22
    void StretchToFit() override;                                       // @ i386 0x0039a68c
    void SetFont(cIGZFont* font) override;                              // @ i386 0x0039a18c
    void ClearSelection() override { selection_ = -1; }                 // @ i386 0x0074ccd0
    int GetSelection() override { return selection_; }                  // @ i386 0x0074cce2
    bool GetSelectedTabName(cIGZString& out) override;                  // @ i386 0x0039a4ac
    bool GetSelectedTabID(std::uint32_t& out) override;                 // @ i386 0x0039a4f4
    bool SelectTab(const cIGZString& name) override;                    // @ i386 0x0039a5c6
    bool SelectTab(std::uint32_t id) override;                          // @ i386 0x0039a626
    bool Init() override;                                               // @ i386 0x00399fcc
    bool Shutdown() override;                                           // @ i386 0x0039a0b6
    bool GZPaint() override;                                            // @ i386 0x0039ab1e
    bool GZOnMouseDownL(int x, int y, std::uint32_t modifiers) override;   // @ i386 0x0039a52a

    void SetOutlineBmp(cIGZBuffer* bitmap);   // @ i386 0x0039a10c (x86_64 0x100512260), not in the interface

    // state for the tests
    const std::list<tFolderTab>& Tabs() const { return tabs_; }
    int TabCount() const { return tabCount_; }
    cIGZFont* Font() const { return font_; }
    cIGZBuffer* OutlineBmp() const { return outlineBmp_; }
    int HalfWidth() const { return halfWidth_; }
    int BitmapHeight() const { return bitmapHeight_; }

private:
    int CurrentTabsWidth() const;                                       // @ i386 0x0039a662
    int CapWidth() const;                                               // max(half width / 8, kFolderOutlineWidth)
    void DrawTab(const tFolderTab& tab, bool selected, bool first, bool last);   // @ i386 0x0039a876

    cIGZBuffer* outlineBmp_ = nullptr;   // +0xe8
    int halfWidth_ = 0;                  // +0xec
    int bitmapHeight_ = 0;               // +0xf0
    std::list<tFolderTab> tabs_;         // +0xf4
    int unused5_ = 5;                    // +0xfc
    cIGZFont* font_ = nullptr;           // +0x100
    int selection_ = -1;                 // +0x104
    int tabCount_ = 0;                   // +0x108
};

}  // namespace gz
