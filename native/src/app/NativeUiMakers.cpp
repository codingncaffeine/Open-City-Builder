// The script control classes made by name for the window COM director (gz::DirectorMake), apart from NativeUi.cpp because
// their headers declare gz::cIGZBuffer.
#include "gz/WinCombo.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinMsgBox.h"
#include "gz/WinOptGrp.h"
#include "gz/WinScrollbar.h"
#include "gz/WinScrollbar2.h"
#include "gz/WinSpinner.h"
#include "rz/COMDirectors.h"

namespace native {
namespace {
cIGZWinMgr* g_makerMgr = nullptr;
template <class T>
cIGZUnknown* MakeControl() { return static_cast<cIGZUnknown*>(static_cast<cIGZWin*>(new T(g_makerMgr))); }
}  // namespace

// The spinner (clsid 0x612ce0c4, 31 in 5 scripts), the combo box (0x59b), the option group (0xa1336cc1), the scroll bars
// (0x61325a2e, 0x4b42da5d) and the message box (0x593): the director lists them, but without makers the scripts built bare
// windows in their place and the controls' script properties found no control to apply to.
void RegisterScriptControlMakers(cIGZWinMgr* mgr) {
    g_makerMgr = mgr;
    gz::RegisterDirectorMaker("cGZWinSpinner", +[]() -> cIGZUnknown* { return MakeControl<gz::cGZWinSpinner>(); });
    gz::RegisterDirectorMaker("cGZWinCombo", +[]() -> cIGZUnknown* { return MakeControl<gz::cGZWinCombo>(); });
    gz::RegisterDirectorMaker("cGZWinOptGrp", +[]() -> cIGZUnknown* { return MakeControl<gz::cGZWinOptGrp>(); });
    gz::RegisterDirectorMaker("cGZWinScrollbar", +[]() -> cIGZUnknown* { return MakeControl<gz::cGZWinScrollbar>(); });
    gz::RegisterDirectorMaker("cGZWinScrollbar2", +[]() -> cIGZUnknown* { return MakeControl<gz::cGZWinScrollbar2>(); });
    gz::RegisterDirectorMaker("cGZWinMsgBox", +[]() -> cIGZUnknown* { return MakeControl<gz::cGZWinMsgBox>(); });
}

// RZ::WCM()->CreateCtrl: the control manager's creators and stock classes (null without a control manager).
bool CreateStockControl(std::uint32_t classId, std::uint32_t iid, void** out) {
    gz::cIGZWinCtrlMgr* controls = gz::WinCtrlMgr();
    return controls && controls->CreateCtrl(classId, iid, out) && out && *out;
}

}  // namespace native
