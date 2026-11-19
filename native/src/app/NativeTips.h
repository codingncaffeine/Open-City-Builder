// The tip of the day dialog's host (NativeTips.cpp).
#pragma once

namespace native {
void InstallTipOfTheDayHost();
// cSC4View3DWin::DoPostCityInit's opener: the dialog once per run, when tips exist.
void ShowTipOfTheDayOnce();
}  // namespace native
