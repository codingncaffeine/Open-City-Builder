// cGZWinFolders implementation — every function here corresponds to one address of the original (noted on the
// definition). The tab model is the original's: a std::list of tFolderTab in add order and a std::list of rows that
// are child windows of the control; RecalcTabs throws the rows away and rebuilds them from the measured tab widths.
#include "gz/WinFolders.h"

#include <cmath>

#include "gz/Buffer.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinFolderRow.h"
#include "gz/WinFont.h"

namespace gz {

namespace {

// The engine side of the seam: RZ::FO() / RZ::WCM() through the recovered font and control-manager seams, and the
// real cGZWinFolderRow.
class DefaultWinFoldersHost final : public IWinFoldersHost {
public:
    cIGZFont* DefaultFont() override {
        // The original gates the lookup on RZ::FO() being present and then asks RZ::WCM() vslot 0x28 for style
        // 0x68963c4c; WinFontSource's Available() is that gate and Font() the lookup.
        IWinFontSource* source = WinFontSource();
        if (!source || !source->Available()) return nullptr;
        IWinFont* font = source->Font(kWinDefaultFontStyle);
        return font ? font->AsFont() : nullptr;
    }
    cIGZBuffer* DefaultOutlineBitmap() override {
        // RZ::WCM() vslot 0x38 (a stock image, borrowed) for 0xe8963edf.
        IWinCtrlFactory* factory = WinCtrlFactory();
        return factory ? factory->Image(kFoldersOutlineBitmapId) : nullptr;
    }
    WinFolderRow CreateFolderRow(cIGZWinMgr* mgr, cIGZFont* font, cIGZBuffer* outline) override {
        auto* row = new cGZWinFolderRow(mgr);
        row->AddRef();             // the one reference the seam hands over
        row->SetFont(font);        // vslot 0x30
        row->SetOutlineBmp(outline);
        return {static_cast<cIGZWinFolderRow*>(row), static_cast<cIGZWin*>(row)};
    }
};

DefaultWinFoldersHost g_defaultHost;
IWinFoldersHost* g_host = nullptr;

}  // namespace

IWinFoldersHost* WinFoldersHost() { return g_host ? g_host : &g_defaultHost; }
void SetWinFoldersHost(IWinFoldersHost* host) { g_host = host; }

// ---- construction -------------------------------------------------------------------------------------------------

cGZWinFolders::cGZWinFolders(cIGZWinMgr* mgr)   // @ 0x100512b82 (C2) / @ 0x100512c80 (C1)
    : cIGZWinFolders(), cGZWin(mgr) {
    // The original constructs the cGZWin at +8, links the two list sentinels, clears the two interfaces, sets the
    // relayout flag and finishes with a 16x16 area at (0x10, 0x10); the member initialisers above do the rest.
    needsRecalc_ = true;
    SetArea(0x10, 0x10, 0x20, 0x20);
}

cGZWinFolders::~cGZWinFolders() {   // @ 0x100513b46 (body) / @ 0x100513b9c (deleting)
    // Quirk kept: the original only clears the two lists here. It never releases font_ / outlineBmp_ (Shutdown does)
    // and never deletes the rows: they are child windows of the cGZWin base, whose destructor deletes them.
    rows_.clear();
    tabs_.clear();
}

// ---- cIGZUnknown --------------------------------------------------------------------------------------------------

bool cGZWinFolders::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x100512cba
    if (iid == kGZIID_cIGZWinFolders) {
        *out = static_cast<cIGZWinFolders*>(this);
        AddRef();
        return true;
    }
    return cGZWin::QueryInterface(iid, out);
}

// ---- lifetime of the control --------------------------------------------------------------------------------------

bool cGZWinFolders::Init() {   // @ 0x100512d02
    cGZWin::Init();
    if (!font_) {
        IWinFoldersHost* host = WinFoldersHost();
        SetFont(host->DefaultFont());   // vslot 0x50; SetFont ignores a null font when there is none yet
    }
    if (!outlineBmp_) {
        cIGZBuffer* bitmap = WinFoldersHost()->DefaultOutlineBitmap();
        outlineBmp_ = bitmap;
        if (outlineBmp_) {
            outlineBmp_->AddRef();
            bitmapWidth_ = static_cast<int>(outlineBmp_->Width() >> 1);   // vslot 0x58
            bitmapHeight_ = static_cast<int>(outlineBmp_->Height());      // vslot 0x60
        }
    }
    if (needsRecalc_) RecalcTabs();   // the original ignores the result
    return true;
}

bool cGZWinFolders::Shutdown() {   // @ 0x100512ddc
    if (outlineBmp_) {
        outlineBmp_->Release();
        outlineBmp_ = nullptr;
    }
    if (font_) {
        font_->Release();
        font_ = nullptr;
    }
    // Guarded: cGZWin::Shutdown deletes the child windows, so the row list would keep dangling pointers (the original
    // leaves it that way and only survives because RecalcTabs' init-flag check keeps it from being walked).
    rows_.clear();
    rowCount_ = 0;
    return cGZWin::Shutdown();
}

// ---- folder list --------------------------------------------------------------------------------------------------

bool cGZWinFolders::AddFolder(const cIGZString& name, std::uint32_t id) {   // @ 0x1005130de
    tFolderTab tab;
    tab.name.AssignFromIGZString(name);
    tab.id = id;
    tabs_.push_back(tab);
    needsRecalc_ = true;
    return true;
}

void cGZWinFolders::DeleteAllFolders() {   // @ 0x100513868
    tabs_.clear();
    needsRecalc_ = true;
}

void cGZWinFolders::GetCount(std::uint32_t& out) {   // @ 0x1005138cc
    out = static_cast<std::uint32_t>(tabs_.size());
}

bool cGZWinFolders::GetFolderByIndex(std::uint32_t index, cIGZString& out) {   // @ 0x10051388c
    // The original counts from the front of the list (the first folder added) with `index + 1` decremented per tab,
    // so 0xffffffff wraps and finds nothing.
    std::uint32_t remaining = index + 1;
    for (const tFolderTab& tab : tabs_) {
        if (--remaining == 0) {
            out.AssignFromIGZString(tab.name);
            return true;
        }
    }
    return false;
}

bool cGZWinFolders::GetSelectedFolderName(cIGZString& out) {   // @ 0x10051316a
    if (rows_.empty()) return false;
    return rows_.front().row->GetSelectedTabName(out);
}

bool cGZWinFolders::GetSelectedFolderID(std::uint32_t& out) {   // @ 0x100513192
    if (rows_.empty()) return false;
    return rows_.front().row->GetSelectedTabID(out);
}

bool cGZWinFolders::SetSelectedFolder(const cIGZString& name) {   // @ 0x1005131ba
    if (needsRecalc_) RecalcTabs();
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        if (it->row->SelectTab(name)) {
            // The original removes the row and links it before the first node: it becomes the bottom row (GZPaint
            // draws the front of the list last). It is not invalidated here.
            rows_.splice(rows_.begin(), rows_, it);
            return true;
        }
        it->row->ClearSelection();
    }
    return false;
}

bool cGZWinFolders::SetSelectedFolder(std::uint32_t id) {   // @ 0x100513270
    if (needsRecalc_) RecalcTabs();
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        if (it->row->SelectTab(id)) {
            rows_.splice(rows_.begin(), rows_, it);
            return true;
        }
        it->row->ClearSelection();
    }
    return false;
}

void cGZWinFolders::SetFont(cIGZFont* font) {   // @ 0x1005138da
    if (font_ == font) return;
    if (font) font->AddRef();
    if (font_) font_->Release();
    font_ = font;
    needsRecalc_ = true;
    InvalidateSelfAndParents();   // tail call of the original (cGZWin vslot 0x2e0)
}

void cGZWinFolders::SetOutlineBmp(cIGZBuffer* bitmap) {   // @ 0x10051393c
    if (outlineBmp_ == bitmap) return;
    if (bitmap) bitmap->AddRef();
    if (outlineBmp_) outlineBmp_->Release();
    outlineBmp_ = bitmap;
    if (bitmap == nullptr) {
        bitmapWidth_ = 0;
        bitmapHeight_ = 0;
    } else {
        bitmapWidth_ = static_cast<int>(bitmap->Width() >> 1);
        bitmapHeight_ = static_cast<int>(outlineBmp_->Height());
    }
    needsRecalc_ = true;
    InvalidateSelfAndParents();
}

// ---- layout -------------------------------------------------------------------------------------------------------

bool cGZWinFolders::CreateRow() {   // the row construction inlined at 0x1005134db and 0x100513697
    WinFolderRow row = WinFoldersHost()->CreateFolderRow(GetWindowManager(), font_, outlineBmp_);
    if (!row.row || !row.window) return false;
    row.window->SetArea(0, 0, GetW(), bitmapHeight_);   // cGZWin vslot 0x1b0
    // ChildAdd takes the reference the factory handed over and makes the row a child window. The original ignores
    // its result; a failure (a manager that does not know the row) releases the creation reference here.
    if (!ChildAdd(row.window)) {
        row.row->Release();
        return false;
    }
    row.window->SetFlag(kWinFlag800, true);            // cGZWin vslot 0x220
    rows_.push_back(row);
    ++rowCount_;
    row.row->Release();   // the child list keeps the only reference
    return true;
}

bool cGZWinFolders::RecalcTabs() {   // @ 0x100513326
    if (!needsRecalc_) return true;
    if (!get_init_flag()) return false;

    // Drop the rows: Shutdown then delete the child window (the original walks its list front to back).
    while (!rows_.empty()) {
        WinFolderRow row = rows_.front();
        rows_.pop_front();
        row.row->Shutdown();      // vslot 0x70
        ChildDelete(row.window);  // cGZWin vslot 0x80
    }
    rowCount_ = 0;

    // Measure the tabs in add order.
    int widest = 0, total = 0;
    if (!tabs_.empty()) {
        for (const tFolderTab& tab : tabs_) {
            const int textWidth = font_ ? font_->GetStringWidth(tab.name.Data(), tab.name.Strlen()) : 0;
            int gutter = bitmapWidth_ >> 3;   // +0x198 / 8
            if (gutter <= 4) gutter = 4;
            const int width = textWidth + 2 * gutter + 10;
            if (widest < width) widest = width;
            total += width;
        }
        if (total < 0) return false;   // the original refuses an overflowed total
    }

    const int windowWidth = GetW();
    if (widest > windowWidth) return false;   // a tab wider than the control: keep the relayout flag set

    // Rows = ceil(total / width) as a float division rounded up (roundss $10), at least one. The original truncates
    // the rounded quotient to an int and keeps 1 unless that is positive; the range check keeps the cast defined
    // (the original's CVTTSS2SI yields INT_MIN there, which is not positive either).
    int rowTotal = 1;
    if (total > 0 && windowWidth > 0) {
        const float ratio = static_cast<float>(total) / static_cast<float>(windowWidth);
        const float ceiled = std::ceil(ratio);
        if (ceiled > 0.0f && ceiled < 2147483648.0f) rowTotal = static_cast<int>(ceiled);
    }
    for (int i = 0; i < rowTotal; ++i)
        if (!CreateRow()) return false;   // guarded: a row the window manager refuses

    // Hand the tabs to the rows in add order, starting at the first row (the one GZPaint puts at the bottom).
    if (!tabs_.empty()) {
        auto rowIt = rows_.begin();
        for (const tFolderTab& tab : tabs_) {
            for (;;) {
                if (rowIt == rows_.end()) rowIt = rows_.begin();
                const bool added = (*rowIt).row->AddTab(&tab.name, tab.id);   // vslot 0x20
                ++rowIt;
                if (added) break;
                if (rowIt == rows_.end()) {
                    if (!CreateRow()) return false;   // appended: the new row becomes the top one
                    rowIt = rows_.begin();            // the original restarts its scan at the first row
                }
            }
        }
    }

    for (WinFolderRow& row : rows_) row.row->StretchToFit();   // vslot 0x28
    SetH((bitmapHeight_ - 4) * rowCount_ + 4);                 // cGZWin vslot 0x1a0
    InvalidateSelfAndParents();                                // cGZWin vslot 0x2e0
    needsRecalc_ = false;
    return true;
}

bool cGZWinFolders::GZPaint() {   // @ 0x100513046
    if (needsRecalc_) RecalcTabs();
    const int rowStep = bitmapHeight_ - 4;
    int index = 0;
    // The original walks from the back of the row list (y = 0) towards the front: the last row created is the top.
    for (auto it = rows_.rbegin(); it != rows_.rend(); ++it, ++index) {
        it->window->GZWinMoveTo(0, rowStep * index);   // cGZWin vslot 0x1c0
        it->row->GZPaint();                            // vslot 0x78
    }
    return true;
}

bool cGZWinFolders::GZOnMouseDownL(int x, int y, std::uint32_t modifiers) {   // @ 0x100512e40
    (void)x;
    (void)modifiers;
    const int rowStep = bitmapHeight_ - 4;
    // Guarded: the original divides by (bitmap height - 4) and dereferences the row list even when no outline bitmap
    // is set (rowStep <= 0) or the list is empty.
    if (rowStep <= 0 || rows_.empty()) return false;
    if (rowStep * rowCount_ + 4 <= y) return false;

    // Row index counted from the front of the list (the bottom row), reached by an unsigned division of the click y.
    const std::uint32_t quotient = static_cast<std::uint32_t>(y) / static_cast<std::uint32_t>(rowStep);
    int target = static_cast<int>(static_cast<std::uint32_t>(rowCount_) - quotient) - 1;
    if (target < 0) target = 0;

    int index = 0;
    for (WinFolderRow& row : rows_)
        if (index++ != target) row.row->ClearSelection();   // vslot 0x38

    if (target > 0) {
        auto it = rows_.begin();
        for (int i = 0; i < target && it != rows_.end(); ++i) ++it;
        if (it != rows_.end()) rows_.splice(rows_.begin(), rows_, it);   // the hit row becomes the bottom row
    }

    SendMsg(notify_, kGZMsgCommand, kFoldersCmdFolderSelected, GetID(), 0);   // cGZWin vslot 0x488
    InvalidateSelfAndParents();
    return false;   // the original always returns 0
}

}  // namespace gz
