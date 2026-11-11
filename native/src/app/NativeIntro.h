// The start-up intro screen (copyright clip, Maxis logo, main intro movie, splash) on the native application: NativeIntro.cpp.
#pragma once
#include <functional>

class cIGZWin;
class cIGZWinMgr;

namespace native {

// What the intro screen reaches through SL::spWinSC4App and the graphic system.
struct IntroServices {
    std::function<void()> destroyCurrentScreen, createRegionScreen, createSplashScreen, clearScreenToBlack;
};
void SetIntroServices(const IntroServices& services);

// True when this build can decode the movies and Intro.dat holds the main intro.
bool IntroVideosAvailable();
// cWinSC4App::CreateIntroVideoScreen's window (no reference taken by the caller yet); null when the movies are unavailable.
cIGZWin* CreateIntroVideoScreen(cIGZWinMgr* manager);

}  // namespace native
