// The window control manager of the native build. The original's cSC4App::SetupWinCtrlMgr fills RZ::WCM() with the control
// colors, the stock images (dialog background, button, slider and scroll bar art) and a captions table (seven localized
// button captions and the ellipsis). Natively the RZ::WCM() service does not exist yet; the generic dialog
// (cGZWinGenDlg: the cheat code box), the message box and the text window reach the manager through gz::WinCtrlMgr(), and
// this installs a cGZWinCtrlMgr filled the same way there.
#include "app/NativeWinControls.h"

#include <cstdio>

#include "app/NativeUi.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinCtrlMgr.h"
#include "rz/cRZString.h"
#include "sc4/App.h"

namespace native {
namespace {

// {caption control id, LTEXT instance in group 0x6a231eaa} (cSC4App resource setup, PC 0x0044e657..0x0044e8c7).
constexpr std::pair<std::uint32_t, std::uint32_t> kCaptions[] = {
    {0x5301814c, 0x0a56b09e}, {0x53018149, 0x8a56b0af}, {0x5301814a, 0x4a56b0b4}, {0x53018147, 0x6a56b0bb},
    {0x53018146, 0x2a56b0be}, {0x5301814b, 0xaa56b0c3}, {0x53018148, 0xca56b0c8},
};
constexpr std::uint32_t kEllipsisCaption = 0x5377be31;
constexpr std::uint32_t kCaptionGroup = 0x6a231eaa;

}  // namespace

void InstallWindowControlManager(cIGZWinMgr* windows, gz::cIGZWinCtrlMgr* existing) {
    gz::cIGZWinCtrlMgr* controls = existing;
    if (!controls) {
        auto* made = new gz::cGZWinCtrlMgr(windows);
        made->AddRef();   // the process's own reference: the manager lives for the session
        made->Init();     // the manager's own default colors
        cSC4App::ApplyControlColors(made);
        controls = made;
    }
    int images = 0;
    ForEachStockControlImage([controls, &images](std::uint32_t id, cGZBuffer* image) {
        controls->SetSystemBMP(static_cast<int>(id), image);
        ++images;
    });
    int captions = 0;
    // The manager keeps the strings it is given (a reference each; cRZString::Release deletes at one): heap strings it owns.
    for (const auto& [id, instance] : kCaptions) {
        cRZString text;
        if (!LoadLocalizedString(kCaptionGroup, instance, text)) continue;
        controls->SetDefaultText(static_cast<int>(id), new cRZString(text));
        ++captions;
    }
    controls->SetDefaultText(static_cast<int>(kEllipsisCaption), new cRZString("\xe2\x80\xa6"));   // a literal U+2026
    gz::SetWinCtrlMgr(controls);
    std::printf("window controls: %d stock images, %d captions (%s manager)\n", images, captions, existing ? "the framework's" : "own");
}

}  // namespace native
