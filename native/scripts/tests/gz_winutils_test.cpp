#include <cstdio>
#include <string>
#include <vector>

#include "fake_drawcontext.h"
#include "fake_winmgr.h"
#include "gz/WinUtils.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;

namespace {
struct FakeFont final : IWinFont {
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    std::uint32_t DefaultTextColor() override { return 0; }
    std::uint32_t DefaultShadowColor() override { return 0; }
    std::int16_t LineHeight(int, int) override { return 12; }
    int CountLines(const char*, std::uint32_t, int) override { return 1; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 40; }
    void MeasureText(const char*, std::uint32_t length, cRZRect& out) override { out = {0, 0, int(length) * 6, 12}; }
    int TextWidth(const char*, std::uint32_t length) override { return int(length) * 6; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
};
struct Host final : IWinButtonHost {
    FakeFont font;
    IWinButtonFont* Font(std::uint32_t) override { return &font; }
    cIGZBuffer* Image(std::uint32_t) override { return nullptr; }
    std::uint32_t DefaultSoundId(int) override { return 0; }
    void PlaySound(std::uint32_t) override {}
    void ShowTip(const BtnTipRequest&) override {}
    void HideTip() override {}
    bool HasWindowColorManager() override { return true; }
};
struct Target final : cGZWin {
    using cGZWin::cGZWin;
    bool DoMessage(cGZMessage&) override { return true; }
};
}  // namespace

int main() {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    Host host;
    SetWinGraphics(&graphics);
    SetWinButtonHost(&host);
    auto* root = new Target(&mgr);
    root->AddRef();
    root->SetArea(0, 0, 300, 300);
    auto* panel = new Target(&mgr);
    panel->SetID(1);
    panel->SetArea(0, 0, 100, 100);
    root->ChildAdd(panel);
    auto* inner = new Target(&mgr);
    inner->SetID(2);
    inner->SetArea(0, 0, 50, 50);
    panel->ChildAdd(inner);
    auto* sibling = new Target(&mgr);
    sibling->SetID(3);
    sibling->SetArea(0, 0, 10, 10);
    root->ChildAdd(sibling);
    for (cGZWin* w : {static_cast<cGZWin*>(root), static_cast<cGZWin*>(panel), static_cast<cGZWin*>(inner), static_cast<cGZWin*>(sibling)}) w->Init();

    // visibility
    CHECK(nsGZWinUtils::GetWindowVisible(root, 2));
    CHECK(nsGZWinUtils::SetWindowVisible(root, 2, false, true) && !nsGZWinUtils::GetWindowVisible(root, 2));
    CHECK(nsGZWinUtils::SetWindowVisible(root, 2, false, false));                // already hidden: untouched
    CHECK(nsGZWinUtils::SetWindowVisible(root, 2, true, false) && nsGZWinUtils::GetWindowVisible(root, 2));
    CHECK(!nsGZWinUtils::SetWindowVisible(root, 99, true, true) && !nsGZWinUtils::GetWindowVisible(root, 99));
    // enabled / caption
    CHECK(nsGZWinUtils::SetWindowEnabled(root, 2, false) && !inner->IsEnabled());
    CHECK(nsGZWinUtils::SetWindowEnabled(root, 2, true) && inner->IsEnabled() && !nsGZWinUtils::SetWindowEnabled(root, 77, true));
    CHECK(nsGZWinUtils::SetWindowCaption(root, 2, cRZString("hello")) && std::string(inner->GetCaption().ToChar()) == "hello");
    CHECK(!nsGZWinUtils::SetWindowCaption(root, 77, cRZString("x")));
    // notification targets
    CHECK(nsGZWinUtils::SetWindowNotificationTarget(root, 2, sibling) && inner->GetNotificationTarget() == sibling);
    CHECK(!nsGZWinUtils::SetWindowNotificationTarget(root, 77, sibling));
    nsGZWinUtils::SetWindowNotificationTargetRecursive(root, root);
    CHECK(panel->GetNotificationTarget() == root && inner->GetNotificationTarget() == root && sibling->GetNotificationTarget() == root);
    CHECK(nsGZWinUtils::SetWindowNotificationTargetRecursive(root, 1, sibling));
    CHECK(inner->GetNotificationTarget() == sibling && panel->GetNotificationTarget() == root);   // the window itself is skipped
    CHECK(!nsGZWinUtils::SetWindowNotificationTargetRecursive(root, 88, sibling));
    // siblings
    void* found = nullptr;
    CHECK(nsGZWinUtils::FindSiblingAs(panel, 3, kGZIID_cIGZWin, &found) && found == static_cast<cIGZWin*>(sibling));
    static_cast<cIGZUnknown*>(found)->Release();
    CHECK(!nsGZWinUtils::FindSiblingAs(panel, 55, kGZIID_cIGZWin, &found));
    // controls the child is not
    CHECK(!nsGZWinUtils::SetButtonToggleState(root, 2, true) && !nsGZWinUtils::GetButtonToggleState(root, 2));
    CHECK(!nsGZWinUtils::SetEditText(root, 2, cRZString("x")) && !nsGZWinUtils::SetSpinnerValue(root, 2, 4));

    // buttons: toggle state, exclusive range
    std::vector<cGZWinBtn*> buttons;
    for (std::uint32_t id = 10; id < 13; ++id) {
        auto* b = new cGZWinBtn(&mgr);
        b->SetID(id);
        b->SetArea(0, 0, 20, 20);
        b->Init();
        panel->ChildAdd(b);
        buttons.push_back(b);
    }
    CHECK(!nsGZWinUtils::GetButtonToggleState(root, 10));
    CHECK(nsGZWinUtils::SetButtonToggleState(root, 10, true) && nsGZWinUtils::GetButtonToggleState(root, 10) && buttons[0]->IsOn());
    CHECK(nsGZWinUtils::SetButtonToggleState(root, 10, false) && !buttons[0]->IsOn());
    nsGZWinUtils::ExcludeButtonByRange(root, 10, 3, 11);
    CHECK(!buttons[0]->IsOn() && buttons[1]->IsOn() && !buttons[2]->IsOn());
    nsGZWinUtils::ExcludeButtonByRange(root, 10, 3, 12);
    CHECK(!buttons[0]->IsOn() && !buttons[1]->IsOn() && buttons[2]->IsOn());
    nsGZWinUtils::ExcludeButtonByRange(root, 10, 0, 12);                          // an empty range touches nothing
    CHECK(buttons[2]->IsOn());
    root->Release();   // deletes the tree: the children hold the only other references
    std::printf(failures ? "FAILED\n" : "gz_winutils ok\n");
    return failures != 0;
}
