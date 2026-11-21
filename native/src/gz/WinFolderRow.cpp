// cGZWinFolderRow implementation — every function here corresponds to one address of the original (noted on the
// definition; Mac i386 decompile).
#include "gz/WinFolderRow.h"

#include "gz/Buffer.h"
#include "gz/DrawContext.h"
#include "gz/FontSys.h"
#include "gz/WinCtrlFactory.h"
#include "rz/cRZSysServPtr.h"

namespace gz {

cGZWinFolderRow::cGZWinFolderRow(cIGZWinMgr* mgr)   // @ i386 0x0039b076
    : cIGZWinFolderRow(), cGZWin(mgr) {}

bool cGZWinFolderRow::QueryInterface(std::uint32_t iid, void** out) {   // @ i386 0x00399f8c
    if (iid == kGZIID_cIGZWinFolderRow) {
        *out = static_cast<cIGZWinFolderRow*>(this);
        AddRef();
        return true;
    }
    return cGZWin::QueryInterface(iid, out);
}

// ---- lifetime -----------------------------------------------------------------------------------------------------

bool cGZWinFolderRow::Init() {   // @ i386 0x00399fcc
    cGZWin::Init();
    if (!font_) {
        // The font system's FontAcquire(type 9, size 15, style 0, color 0, reuse) (vslot 0x5c i386); the acquire's
        // reference is the row's.
        cRZSysServPtr<cIGZFontSys, kGZFontSysDefaultServiceID, kGZIID_cIGZFontSys> fontSys;
        if (fontSys) font_ = fontSys->FontAcquire(9, 0xf, 0, 0, true);
    }
    if (!outlineBmp_) {
        // RZ::WCM() stock image 0xe8963edf (vslot 0x1c i386), borrowed and AddRef'd here.
        IWinCtrlFactory* factory = WinCtrlFactory();
        outlineBmp_ = factory ? factory->Image(kFoldersOutlineBitmapId) : nullptr;
        if (outlineBmp_) {
            outlineBmp_->AddRef();
            halfWidth_ = static_cast<int>(outlineBmp_->Width() >> 1);
            bitmapHeight_ = static_cast<int>(outlineBmp_->Height());
        }
    }
    return true;
}

bool cGZWinFolderRow::Shutdown() {   // @ i386 0x0039a0b6
    if (outlineBmp_) {
        outlineBmp_->Release();
        outlineBmp_ = nullptr;
    }
    if (font_) {
        font_->Release();
        font_ = nullptr;
    }
    return cGZWin::Shutdown();
}

void cGZWinFolderRow::SetOutlineBmp(cIGZBuffer* bitmap) {   // @ i386 0x0039a10c
    if (outlineBmp_ == bitmap) return;
    if (bitmap) bitmap->AddRef();
    if (outlineBmp_) outlineBmp_->Release();
    outlineBmp_ = bitmap;
    if (!bitmap) {
        halfWidth_ = 0;
        bitmapHeight_ = 0;
    } else {
        halfWidth_ = static_cast<int>(bitmap->Width() >> 1);
        bitmapHeight_ = static_cast<int>(bitmap->Height());
    }
}

void cGZWinFolderRow::SetFont(cIGZFont* font) {   // @ i386 0x0039a18c
    if (font_ == font) return;
    if (font) font->AddRef();
    if (font_) font_->Release();
    font_ = font;
}

// ---- tabs ---------------------------------------------------------------------------------------------------------

int cGZWinFolderRow::CurrentTabsWidth() const {   // @ i386 0x0039a662
    int width = 0;
    for (const tFolderTab& tab : tabs_) width += tab.area.right - tab.area.left;
    return width;
}

int cGZWinFolderRow::CapWidth() const {
    // The original compares (int)_kFolderOutlineWidth with (int)(half width >> 3) and takes the larger as a float.
    const int fromBitmap = static_cast<int>(static_cast<std::uint32_t>(halfWidth_) >> 3);
    return fromBitmap > kFolderOutlineWidth ? fromBitmap : kFolderOutlineWidth;
}

bool cGZWinFolderRow::CanThisTabBeAdded(const cIGZString* name) {   // @ i386 0x0039a704
    if (!font_ || !name) return false;   // guarded: the original calls through the font without a check
    const int rowWidth = GetW();
    const int textWidth = font_->GetStringWidth(name->Data(), name->Strlen());
    if (textWidth < rowWidth || tabCount_ != 0)
        return CurrentTabsWidth() + CapWidth() * 2 + 10 + textWidth <= rowWidth;
    return true;   // a first tab wider than the row
}

bool cGZWinFolderRow::AddTab(const cIGZString* name, std::uint32_t id) {   // @ i386 0x0039ad22
    if (!font_ || !name) return false;
    tFolderTab tab;
    tab.area.left = CurrentTabsWidth();
    const int textWidth = font_->GetStringWidth(name->Data(), name->Strlen());
    tab.area.top = 0;
    tab.area.right = tab.area.left + CapWidth() * 2 + 10 + textWidth;
    tab.area.bottom = bitmapHeight_;
    if (tab.area.right > GetW()) return false;
    tab.name.AssignFromIGZString(*name);
    tab.id = id;
    tabs_.push_back(tab);
    ++tabCount_;
    return true;
}

void cGZWinFolderRow::StretchToFit() {   // @ i386 0x0039a68c
    // The spare width goes out one pixel per pass: the current tab grows by one, every tab after it moves right by
    // one, and the cursor moves on (wrapping from the last tab to the first). The comparison is unsigned.
    std::uint32_t total = static_cast<std::uint32_t>(CurrentTabsWidth());
    auto it = tabs_.begin();
    while (static_cast<std::uint32_t>(GetW()) > total && it != tabs_.end()) {
        it->area.right += 1;
        ++it;
        for (auto later = it; later != tabs_.end(); ++later) {
            later->area.left += 1;
            later->area.right += 1;
        }
        ++total;
        if (it == tabs_.end()) it = tabs_.begin();   // the list sentinel's next node is the first tab
    }
}

bool cGZWinFolderRow::GetSelectedTabName(cIGZString& out) {   // @ i386 0x0039a4ac
    if (selection_ < 0) return false;
    auto it = tabs_.begin();
    for (int i = 0; i != selection_; ++i) ++it;
    out.AssignFromIGZString(it->name);   // cIGZString vslot 0x38 i386
    return true;
}

bool cGZWinFolderRow::GetSelectedTabID(std::uint32_t& out) {   // @ i386 0x0039a4f4
    if (selection_ < 0) return false;
    auto it = tabs_.begin();
    for (int i = 0; i != selection_; ++i) ++it;
    out = it->id;
    return true;
}

bool cGZWinFolderRow::SelectTab(const cIGZString& name) {   // @ i386 0x0039a5c6
    int index = 0;
    for (const tFolderTab& tab : tabs_) {
        if (tab.name.IsEqualToStringRef(name, true)) {
            selection_ = index;
            return true;
        }
        ++index;
    }
    return false;
}

bool cGZWinFolderRow::SelectTab(std::uint32_t id) {   // @ i386 0x0039a626
    int index = 0;
    for (const tFolderTab& tab : tabs_) {
        if (tab.id == id) {
            selection_ = index;
            return true;
        }
        ++index;
    }
    return false;
}

// ---- paint and input ----------------------------------------------------------------------------------------------

void cGZWinFolderRow::DrawTab(const tFolderTab& tab, bool selected, bool first, bool last) {   // @ i386 0x0039a876
    auto* context = static_cast<cIGZDrawContext*>(drawContext_);
    const int half = halfWidth_;
    // DrawTab compares half >> 3 with 5 instead of the constant; both pick the same width.
    const int cap = (static_cast<std::uint32_t>(half) >> 3) < 5 ? kFolderOutlineWidth
                                                                : static_cast<int>(static_cast<std::uint32_t>(half) >> 3);
    // The selected tab reaches 2 pixels over its neighbors, except at the ends of the row.
    const int growLeft = (selected && !first) ? -2 : 0;
    const int growRight = (selected && !last) ? 2 : 0;
    const int x = drawArea_.left, y = drawArea_.top;
    const int height = bitmapHeight_;

    // The left half of the outline bitmap frames an unselected tab, the right half the selected one.
    cRZRect source{selected ? half : 0, 0, (selected ? half : 0) + cap, height};
    cRZRect dest{growLeft + tab.area.left + x, y, cap + growLeft + tab.area.left + x, height + y};
    context->Blt(outlineBmp_, source, dest);   // left cap (vslot 0x98 i386)

    source.left += cap;
    source.right += cap;
    dest = {growLeft + tab.area.left + cap + x, y, x + growRight + tab.area.right - cap, y + height};
    context->BltTiled(outlineBmp_, source, dest, 0, 0);   // the middle, tiled (vslot 0x9c i386)

    const int step = half - cap * 2;
    source.left += step;
    source.right += step;
    dest = {x + growRight + tab.area.right - cap, y, x + growRight + tab.area.right, y + height};
    context->Blt(outlineBmp_, source, dest);   // right cap

    if (font_) {
        // The name centred in the tab (alignment 1), vertically centred on the font height, default colors.
        const int fontHeight = font_->CharHeight(nullptr, 0);
        cRZRect text;
        text.left = tab.area.left + x;
        text.right = tab.area.right + x;
        text.top = y + tab.area.top + ((tab.area.bottom - tab.area.top) - fontHeight) / 2;
        text.bottom = tab.area.bottom + y;
        font_->DrawTextParagraph(FontTarget(drawBuffer_), text, tab.name.Data(), tab.name.Strlen(), 1,
                                 kFontDefaultColor, kFontDefaultColor, nullptr, nullptr);
    }
}

bool cGZWinFolderRow::GZPaint() {   // @ i386 0x0039ab1e
    if (!drawContext_ || !outlineBmp_) return true;   // guarded: the original draws through both unchecked
    const int count = tabCount_;
    int index = 0;
    const tFolderTab* selected = nullptr;
    for (const tFolderTab& tab : tabs_) {
        if (index == selection_) selected = &tab;
        else DrawTab(tab, false, index == 0, index == count - 1);
        ++index;
    }
    if (selected) DrawTab(*selected, true, selection_ == 0, selection_ == count - 1);
    return true;
}

bool cGZWinFolderRow::GZOnMouseDownL(int x, int y, std::uint32_t modifiers) {   // @ i386 0x0039a52a
    int index = 0;
    for (const tFolderTab& tab : tabs_) {
        if (tab.area.left <= x && x <= tab.area.right) {
            selection_ = index;
            break;
        }
        ++index;
    }
    // Forward the click to the parent control in its coordinates (cGZWin vslots 0xf4 / 0x244 i386): the control
    // picks the row and notifies its target.
    WindowToWindowCoordinates(parent_, x, y);
    cGZMessage message;
    message.type = kGZMsgMouseDownL;
    message.param1 = static_cast<std::uint64_t>(static_cast<std::int64_t>(x));
    message.param2 = static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
    message.param3 = modifiers;
    return SendMsg(parent_, message);
}

}  // namespace gz
