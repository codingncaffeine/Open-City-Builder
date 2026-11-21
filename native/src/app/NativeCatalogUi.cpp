// The services of the city view's tool palettes (the tertiary menu frame, the catalog lists and their callout boxes) on the
// native application.
#include "app/NativeCatalogUi.h"

#include <string>

#include "app/NativeAudio.h"
#include "app/NativeGraphics.h"
#include "app/NativeWinMgr.h"
#include "gz/Buffer.h"
#include "sc4/WinCalloutBox.h"
#include "sc4/WinCatalogView.h"
#include "sc4/WinTertiaryMenuFrame.h"

namespace native {
namespace {

class TertiaryFrameHost final : public sc4::IWinTertiaryMenuFrameHost {
public:
    cIGZBuffer* CreateBuffer() override { auto* buffer = new cGZBuffer(); buffer->AddRef(); return buffer; }
    // The window's draw buffer is the native buffer; its cIGZBuffer view draws through the window's draw context.
    cIGZBuffer* WindowBuffer(IWinBuffer* buffer) override { return buffer ? static_cast<Buffer*>(buffer)->AsBuffer() : nullptr; }
};

class CatalogHost final : public sc4::ICatalogViewHost {
public:
    std::string MakeMoneyString(std::int64_t amount) override {
        std::string digits = std::to_string(amount < 0 ? -amount : amount), out;
        for (std::size_t i = 0; i < digits.size(); ++i) {
            if (i && (digits.size() - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return std::string(amount < 0 ? "-\xc2\xa7" : "\xc2\xa7") + out;   // the simoleon sign
    }
    void PlaySound(std::uint32_t id) override { PlayGameSound(id); }
    bool BlitIcon(IWinBuffer* target, cIGZUnknown* icon, const cRZRect& source, const cRZRect& destination) override {
        if (!target || !icon) return false;
        static_cast<Buffer*>(target)->AsBuffer()->Blt(static_cast<cIGZBuffer*>(icon), source, destination, nullptr);
        return true;
    }
};

cIGZWinMgr* g_manager = nullptr;
TertiaryFrameHost g_frame;
CatalogHost g_catalog;

}  // namespace

void InstallCatalogUi(cIGZWinMgr* manager) {
    g_manager = manager;
    sc4::SetWinTertiaryMenuFrameHost(&g_frame);
    sc4::SetCatalogViewHost(&g_catalog);
}

sc4::cISC4WinCalloutBox* CreateCalloutBox() {
    // The view's DoPreCityInit makes its cursor-text box before the catalog is installed: the active window manager then.
    auto* box = new sc4::cSC4WinCalloutBox(g_manager ? g_manager : static_cast<cIGZWinMgr*>(ActiveWinMgr()));
    box->AddRef();
    return box;
}

}  // namespace native
