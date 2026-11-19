#include <cstdio>

#include "fake_winmgr.h"
#include "sc4/SCEntryView.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace sc4;

static bool Collect(cIGZWin*, std::uint32_t, void* child, void* context) {
    static_cast<std::vector<cIGZWin*>*>(context)->push_back(static_cast<cIGZWin*>(child));
    return true;
}

int main() {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    SetWinGraphics(&graphics);
    auto* view = new cSCStandardTypeEntryView(&mgr);
    view->AddRef();
    view->SetArea(0, 0, 120, 20);

    CHECK(view->TextColor() == 0xff000000u && view->SelectionColor() == 0xff000080u && view->BackgroundColor() == 0xffffffffu);
    std::uint32_t id = 77;
    view->GetPropertyID(id);
    CHECK(id == 0);
    view->SetPropertyID(0xabcd);
    view->GetPropertyID(id);
    CHECK(id == 0xabcd);
    cRZVariant value;
    value.SetValUint32(42);
    view->SetValue(value);
    std::uint32_t got = 0;
    CHECK(view->Value().GetValUint32(got) && got == 42);
    view->SetAdditionalViewParameters(cRZString("ignored"));

    void* iface = nullptr;
    const std::uint32_t refsBefore = view->AddRef();
    CHECK(view->QueryInterface(kGZIID_cISCStandardTypeEntryView, &iface) && iface == static_cast<cISCStandardTypeEntryView*>(view));
    CHECK(view->AddRef() == refsBefore + 2);   // the query took one reference
    CHECK(static_cast<cISCStandardTypeEntryView*>(view)->AsIGZWin() == view);
    CHECK(view->QueryInterface(kGZIID_cIGZWin, &iface) && iface == static_cast<cIGZWin*>(view));
    CHECK(!view->QueryInterface(0x1234, &iface));

    view->AddBackgroundFillControl();
    CHECK(view->GetChildCount() == 1);
    std::vector<cIGZWin*> children;
    view->EnumChildren(kGZIID_cIGZWin, Collect, &children);
    CHECK(children.size() == 1);
    if (children.size() == 1) {
        cIGZWin* rect = children[0];
        CHECK(rect->GetW() == 120 && rect->GetH() == 20 && rect->GetParentWin() == view);
        std::uint8_t r = 0, g = 0, b = 0;
        rect->GetFillColor(r, g, b);
        CHECK(r == 0xe0 && g == 0xe0 && b == 0xe0);
        CHECK(!rect->GetFlag(kWinFlag800));
        void* flat = nullptr;
        CHECK(rect->QueryInterface(gz::kGZIID_cIGZWinFlatRect, &flat));
        auto* flatRect = static_cast<gz::cIGZWinFlatRect*>(flat);
        CHECK(flatRect->GetOutlineFlags() == 0xf && flatRect->GetSizeMode() == 0);
        std::uint32_t l, t, rr, bb;
        flatRect->GetOutlineColors(l, t, rr, bb);
        CHECK(l == 0xff000000u && bb == 0xff000000u);
        flatRect->Release();
    }
    // the test's own AddRef, refsBefore, the CHECK AddRef and the two successful queries: five references in all
    for (int i = 0; i < 5; ++i) view->Release();
    SetWinGraphics(nullptr);
    std::printf(g_fail ? "FAILED %d\n" : "sc4_scentryview OK\n", g_fail);
    return g_fail ? 1 : 0;
}
