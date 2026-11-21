// The window control manager of the native build (see NativeWinControls.cpp).
#pragma once

class cIGZWinMgr;
namespace gz { class cIGZWinCtrlMgr; }

namespace native {

// cSC4App::SetupWinCtrlMgr's resources for `existing` (RZ::WCM(); null on a host without the service) or, without one, a cGZWinCtrlMgr of the
// process's own on `windows`; either becomes gz::WinCtrlMgr().
void InstallWindowControlManager(cIGZWinMgr* windows, gz::cIGZWinCtrlMgr* existing);

}  // namespace native
