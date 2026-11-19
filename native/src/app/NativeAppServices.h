// The application's own services on the native build (NativeAppServices.cpp).
#pragma once

class cSC4App;

namespace native {

// The window location saver, the feature manager and the tool tip manager as the recovered classes (the app's service factories),
// the feature manager's resources, the callout boxes' buffers, and the app's initialize hook that runs each one's Init where
// cSC4App::Init does.
void InstallAppServices(cSC4App& app);

}  // namespace native
