#include <cstdio>
#include <string>
#include <vector>

#include "fake_winmgr2.h"
#include "sc4/WinCatalogView.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct TestFont final : gz::IWinFont {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t DefaultTextColor() override { return 0; }
    std::uint32_t DefaultShadowColor() override { return 0; }
    std::int16_t LineHeight(int, int) override { return 0; }
    int CountLines(const char*, std::uint32_t, int) override { return 0; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 0; }
    void MeasureText(const char*, std::uint32_t, cRZRect&) override {}
    int TextWidth(const char*, std::uint32_t) override { return 0; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
};
struct FontSrc final : gz::IWinFontSource {
    TestFont font; std::uint32_t asked = 0;
    gz::IWinFont* Font(std::uint32_t style) override { asked = style; return &font; }
    bool Available() override { return true; }
};
struct Icon final : IWinBuffer {
    struct Blt_ { cRZRect s, d; }; std::vector<Blt_> blts; int locks = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    const cRZRect* GetBounds() const override { return nullptr; }
    std::uint32_t MakeNativeColor(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
};
struct Target final : IWinBuffer {   // the window's draw buffer: records Blt(source) calls
    std::vector<std::pair<IWinBuffer*, std::pair<cRZRect, cRZRect>>> blts; int locks = 0, unlocks = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    const cRZRect* GetBounds() const override { return nullptr; }
    std::uint32_t MakeNativeColor(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    bool Lock(std::uint32_t f) override { ++locks; return f == 0x8010; }
    void Unlock(std::uint32_t) override { ++unlocks; }
    bool Blt(IWinBuffer* s, const cRZRect& a, const cRZRect& b) override { blts.push_back({s, {a, b}}); return true; }
};
struct Callout final : cISC4WinCalloutBox {
    cGZWin* win; std::string title, text; cRZPoint poi; cRZRect avoid, constraint{1, 2, 3, 4}; int refs = 1;
    explicit Callout(cGZWin* w) : win(w) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZWin* AsIGZWin() override { return win; }
    void SetBackgroundImage(cIGZBuffer*, int, int) override {}
    cIGZBuffer* GetBackgroundImage() const override { return nullptr; }
    void SetTitle(const cIGZString& t) override { title = t.ToChar(); }
    const cIGZString& GetTitle() const override { static cRZString s; return s; }
    void SetText(const cIGZString& t) override { text = t.ToChar(); }
    const cIGZString& GetText() const override { static cRZString s; return s; }
    void SetTitleFontStyle(std::uint32_t) override {}
    void SetTextFontStyle(std::uint32_t) override {}
    void SetIcon(cIGZBuffer*) override {}
    cIGZBuffer* GetIcon() const override { return nullptr; }
    void SetIconMeter(cIGZBuffer*, std::uint32_t, float) override {}
    void SetPointOfInterest(const cRZPoint& p) override { poi = p; }
    const cRZPoint& GetPointOfInterest() const override { return poi; }
    void SetAvoidanceRect(const cRZRect& r) override { avoid = r; }
    const cRZRect& GetAvoidanceRect() const override { return avoid; }
    void SetConstraintRect(const cRZRect& r) override { constraint = r; }
    const cRZRect& GetConstraintRect() const override { return constraint; }
};
struct Parent final : cGZWin, ICatalogScrollTarget {
    bool up = false, down = false; int calls = 0;
    using cGZWin::cGZWin;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kCatalogViewScrollTargetIID) { *out = static_cast<ICatalogScrollTarget*>(this); cGZWin::AddRef(); return true; }
        return cGZWin::QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    void SetScrollButtonsEnabled(bool u, bool d) override { up = u; down = d; ++calls; }
};
struct Host final : ICatalogViewHost {
    std::vector<std::uint32_t> sounds;
    std::string MakeMoneyString(std::int64_t v) override { return "$" + std::to_string(v); }
    void PlaySound(std::uint32_t id) override { sounds.push_back(id); }
};
struct View final : cSC4WinCatalogView {
    using cSC4WinCatalogView::cSC4WinCatalogView;
    void SetDraw(IWinBuffer* b, cRZRect a) { drawBuffer_ = b; drawArea_ = a; }
};
cSC4CatalogItem* MakeItem(const char* name, std::uint32_t id, std::uint32_t cost, Icon* icon = nullptr) {
    auto* i = new cSC4CatalogItem; i->AddRef();
    i->SetName(cRZString(name)); i->SetDescription(cRZString((std::string("desc ") + name).c_str())); i->SetItemID(id); i->SetItemType(7); i->SetCost(cost);
    if (icon) i->SetIcon(icon);
    return i;
}
}

int main() {
    FakeMgr2 m; Host host; SetCatalogViewHost(&host); FontSrc fonts; gz::SetWinFontSource(&fonts);
    auto* parent = new Parent(&m); parent->AddRef();
    auto* v = new View(&m); v->AddRef();
    v->SetArea(0, 0, 40, 100);
    v->SetItemSize(32, 20, 4);
    parent->ChildAdd(v);
    v->Init();
    CHECK(fonts.asked == 0x68963c4c && fonts.font.refs == 2);
    CHECK(parent->calls >= 1 && !parent->up);

    CHECK(v->CalcPreferredSize(0).y == 0 && v->CalcPreferredSize(0).x == 32);
    CHECK(v->CalcPreferredSize(3).y == 3 * 24 - 4);

    Icon icon;
    cSC4CatalogItem* items[6];
    for (int i = 0; i < 6; ++i) items[i] = MakeItem(i == 1 ? "Park" : "Road", 100 + i, i == 1 ? 250 : 0, &icon);
    for (auto* it : items) v->AddItem(it);
    CHECK(v->GetItemCount() == 6);

    CHECK(v->GetItem(0) == nullptr);
    CHECK(v->GetItemByID(103) == items[3] && v->GetItemByID(5) == nullptr);
    // visible rows: (100 + 4) / 24 = 4; max scroll 2
    v->SetScrollPosition(5);
    CHECK(v->GetScrollPosition() == 2 && parent->up && !parent->down);
    v->SetScrollPosition(-3);
    CHECK(v->GetScrollPosition() == 0 && !parent->up && parent->down);
    v->ChangeScrollPos(1);
    CHECK(v->GetScrollPosition() == 1 && host.sounds.size() == 1 && host.sounds[0] == 0x2a5c322b);
    v->ChangeScrollPos(0);
    CHECK(host.sounds.size() == 1);

    // wheel: one notch of 120 scrolls... delta 600 -> -2 rows; delta -300 -> +1
    v->SetScrollPosition(2);
    v->GZOnMouseWheel(0, 0, 0, 600);
    CHECK(v->GetScrollPosition() == 0);
    v->GZOnMouseWheel(0, 0, 0, -300);
    CHECK(v->GetScrollPosition() == 1);
    v->SetScrollPosition(0);

    // item queries
    int x = 0, y = 0;
    CHECK(v->GetItemPositionByID(102, x, y) && x == 0x30 && y == 10 + 24 * 2);
    CHECK(!v->GetItemPositionByID(999, x, y));
    CHECK(v->SetEnabledAllItemsExceptItemWithID(104, false));
    CHECK(!items[0]->GetEnabled() && items[4]->GetEnabled());
    CHECK(!v->SetEnabledAllItemsExceptItemWithID(999, true) && items[0]->GetEnabled());
    v->SetSelectedItemByID(103); CHECK(v->GetSelectedItem() == items[3] && v->GetSelectedItemID() == 103);
    v->SetSelectedItemByID(999); CHECK(v->GetSelectedItem() == nullptr && v->GetSelectedItemID() == 0);

    // make visible
    v->MakeItemVisible(105); CHECK(v->GetScrollPosition() == 2);   // 1 - 4 + 5
    v->MakeItemVisible(100); CHECK(v->GetScrollPosition() == 0);
    v->MakeItemVisible(102); CHECK(v->GetScrollPosition() == 0);
    CHECK(!v->MakeItemVisible(7));

    // callout
    auto* cw = new cGZWin(&m); cw->AddRef();
    Callout callout(cw);
    v->GZOnMouseExit(nullptr);
    v->SetCalloutBox(&callout);
    CHECK(callout.refs == 2 && cw->GetParentWin() == v);
    CHECK(v->GZOnMouseMove(5, 30, 0));      // row (30 - 2)/24 = 1
    CHECK(v->HighlightedIndex() == 1 && v->GetHighlightedItem() == items[1]);
    CHECK(callout.title == "Park - $250" && callout.text == "desc Park");
    CHECK(callout.poi.x == 0x30 && callout.poi.y == 10 + 24);
    CHECK(callout.constraint.left == 0x30 && callout.constraint.top == 2 && callout.constraint.right == 3);
    CHECK(callout.avoid.left == 0 && callout.avoid.right == 0 && cw->IsVisible());
    v->GZOnMouseMove(5, 4, 0);   // row 0
    CHECK(callout.title == "Road");
    v->GZOnMouseMove(5, 99999, 0); CHECK(v->HighlightedIndex() == -1 && !cw->IsVisible());
    v->GZOnMouseMove(5, 4, 0);
    v->GZOnMouseExit(nullptr); CHECK(v->HighlightedIndex() == -1);

    // click: enabled item posts the command, disabled plays the error sound
    m.posted.clear(); host.sounds.clear();
    v->GZOnMouseMove(5, 30, 0);
    v->GZOnMouseUpL(5, 30, 0);
    CHECK(m.posted.size() == 1 && m.posted[0].type == 3 && m.posted[0].param1 == 0x49d993d4 && m.posted[0].param2 == 7 && m.posted[0].param3 == 101);
    CHECK(v->SelectedIndex() == 1 && host.sounds.size() == 1 && host.sounds[0] == 0x2a5c322b);
    items[2]->SetEnabled(false);
    v->GZOnMouseMove(5, 60, 0); v->GZOnMouseUpL(5, 60, 0);
    CHECK(m.posted.size() == 1 && host.sounds.back() == 0xaa62c9d5 && v->SelectedIndex() == 1);

    // paint: frames 0 disabled, 1 normal, 2 selected, 3 highlighted
    Target target; v->SetDraw(&target, cRZRect{10, 5, 42, 105});
    v->GZOnMouseMove(5, 4, 0);   // highlight row 0
    v->GZPaint();
    CHECK(target.blts.size() == 4);
    CHECK(target.blts[0].second.first.left == 96 && target.blts[0].second.first.right == 128 && target.blts[0].second.first.bottom == 20);
    CHECK(target.blts[0].second.second.left == 10 && target.blts[0].second.second.top == 5 && target.blts[0].second.second.right == 42 && target.blts[0].second.second.bottom == 25);
    CHECK(target.blts[1].second.first.left == 64);    // selected
    CHECK(target.blts[2].second.first.left == 0);     // disabled
    CHECK(target.blts[3].second.first.left == 32);    // normal
    CHECK(target.blts[3].second.second.top == 5 + 24 * 3);
    CHECK(target.blts[0].first == &icon);

    // flash: item 3 is lit while the countdown is odd
    CHECK(v->FlashItemByID(103, 3, 50) && v->FlashTimer().IsRunning());
    target.blts.clear();
    v->GZOnMouseExit(nullptr);
    v->GZPaint();
    CHECK(target.blts[3].second.first.left == 96);
    cGZMessage tick; tick.type = 0xaa5187a4;
    v->DoMessage(tick); CHECK(v->FlashCountdown() == 2);
    v->DoMessage(tick); v->DoMessage(tick); CHECK(v->FlashCountdown() == 0 && v->FlashTimer().IsRunning());
    v->DoMessage(tick); CHECK(!v->FlashTimer().IsRunning());
    CHECK(!v->FlashItemByID(777, 1, 10));
    CHECK(!v->FlashItemByID(0xffffffff, 5, 10) && v->FlashCountdown() == 5);
    cSC4WinCatalogView::FlashTimerCallback(v);   // posts the tick to itself
    CHECK(m.posted.back().type == 0xaa5187a4);

    // scroll message
    cGZMessage scroll; scroll.type = 0xca35e4ba; scroll.param1 = 1;
    CHECK(v->DoMessage(scroll) && v->GetScrollPosition() == 1);

    // QueryInterface
    void* p = nullptr;
    CHECK(v->QueryInterface(0x49d98264, &p) && p == static_cast<cISC4WinCatalogView*>(v));
    v->Release();
    CHECK(v->AsIGZWin() == static_cast<cIGZWin*>(static_cast<cGZWin*>(v)));

    v->SetCalloutBox(nullptr);
    CHECK(callout.refs == 1);
    cw->Release();   // the test's own reference; the view dropped its child reference
    v->Clear();
    CHECK(v->GetItemCount() == 0);
    for (auto* it : items) it->Release();   // the reference MakeItem took; AddItem took its own
    v->Shutdown();
    CHECK(fonts.font.refs == 1);
    parent->Release();
    SetCatalogViewHost(nullptr);
    gz::SetWinFontSource(nullptr);
    if (!failures) std::printf("wincatalogview OK\n");
    return failures;
}
