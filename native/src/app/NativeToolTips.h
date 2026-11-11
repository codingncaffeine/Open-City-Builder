// The game's tool tips on the native application: cSC4ToolTipManager (the service that shows a button's tip text in a
// callout box near the pointer) and the callout boxes' buffer service (see NativeToolTips.cpp).
#pragma once
#include <cstdint>

class cIGZWinMgr;
class cIGZString;

namespace native {

// Installs the callout boxes' buffer service and the tip manager on `manager` (the tip window is made on first use).
void InstallToolTips(cIGZWinMgr* manager);
// cSC4ToolTipManager::DoMessage 0x22c010d0 / 0x22c010d1: a window asks for its tip (text "title|body" or plain).
void ShowToolTip(const cIGZString& text);
// 0x22c010d2 / 0x22c010d8: the tip goes away.
void HideToolTip();
// The manager's 750 ms periodic timer (cRZCallbackTimer in the original), driven by the frame loop.
void TickToolTips();

}  // namespace native
