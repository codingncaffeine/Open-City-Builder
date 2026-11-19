// The sign and label tools' hosts (NativeSignLabel.cpp).
#pragma once

namespace sc4 { class cSC4ViewInputControl; }

namespace native {
void InstallSignToolHost();   // replaces the generated sign tool, signpost and text decal (label) stubs
sc4::cSC4ViewInputControl* CreateLabelTool();   // new cSC4ViewInputControlLabelTool (the city view's label command)
}  // namespace native
