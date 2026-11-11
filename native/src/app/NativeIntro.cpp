// The start-up intro on the native application: cSC4WinIntroVideoScreen (Mac i386 0x001ce900..0x001cf186) and the movie player
// window it owns, cSC4MadCowPlayerWin (0x004461e0..0x004465a0; the Mac one wraps QuickTime, the PC one EA's MAD player; this one
// decodes the MAD streams of Intro.dat through NativeMovie). Decompiles: scratch/s54/ghidra/intro.c.
//
// The sequence: the copyright clip {0x0a8b0e70, 1, 2} (cannot be skipped) -> the Maxis logo script window fades in and, after
// 5 s, out; at 6 s it goes -> the "Real2" clip {.., 3} (not shipped on the PC: skipped at once) -> the main intro {.., 1} (Esc or
// a click skips it, and the screen then goes to the normal splash screen) -> the splash script window, 2 s -> the region screen.
//
// Port choices (no original behaviour depends on them):
// - The original's player draws to the whole screen whatever its window size (QuickTime on the main port). Here the player
//   window covers the intro screen, and every clip keeps its aspect ratio: the original stretched the copyright clip over the
//   whole screen and fell back to a larger-than-the-screen size when the integer scale did not fit; on a wide or small display
//   both look broken, so a clip is scaled by the original's rounded integer factor when that fits, else to fit.
// - The transition messages the screen posts from GZPaint are posted once (the original posts on every paint until one is
//   handled; the window manager here keeps raw targets, so a second one would reach a destroyed screen).
#include "app/NativeIntro.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAudio.h"
#include "app/GameAudio.h"
#include "app/NativeMovie.h"
#include "app/NativeUi.h"
#include "gz/Buffer.h"
#include "gz/DrawContext.h"
#include "gz/Win.h"
#include "gz/cGZPersistResourceKey.h"

namespace native {
namespace {

bool Debug() { static const bool on = std::getenv("SC4_INTRODEBUG") != nullptr; return on; }
double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

constexpr std::uint32_t kMovieType = 0x0a8b0e70, kMovieGroup = 1;
constexpr std::uint32_t kMainIntroVideo = 1, kCopyrightVideo = 2, kMainIntroVideoReal2 = 3;   // the statics at 0x00c00140..
constexpr std::uint32_t kMsgCopyrightDone = 0x8aaafd43, kMsgPlayReal2 = 0xeaac1a96, kMsgPlayMainIntro = 0xeaac1a97,
                        kMsgIntroDone = 0x4a384f52, kMsgGoToRegion = 0xcaac6a52, kMsgUserStop = 0xeaad620a;
constexpr std::uint32_t kScriptGroup = 0x96a006b0, kLogoScript = 0xaaaaf3d1, kSplashScript = 0x8aa9aa14, kScriptChildId = 0xaaa9c9d9;
constexpr std::uint32_t kKeyEscape = 0x1b;

IntroServices g_services;

// cSC4MadCowPlayerWin.
class MoviePlayerWin final : public cGZWin {
public:
    explicit MoviePlayerWin(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    ~MoviePlayerWin() override { Shutdown(); }

    bool Init() override {   // 0x004461e0
        if (get_init_flag()) return true;
        SetFlag(0x8000, false);
        SetFlag(0x200000, true);
        return cGZWin::Init();
    }
    bool Shutdown() override {   // 0x00446244
        if (target_) {
            target_->Release();
            target_ = nullptr;
        }
        CloseMovie();
        if (frame_) {
            frame_->Release();
            frame_ = nullptr;
        }
        return cGZWin::Shutdown();
    }

    // QueueMovie (0x004465a0): only the intro clips {0x0a8b0e70, 1, 1..3}; flag 1 = cannot be stopped, flag 2 = scale to fit.
    bool QueueMovie(std::uint32_t instance, std::uint32_t flags) {
        if (instance < 1 || instance > 3) return false;
        stoppable_ = !(flags & 1);
        CloseMovie();
        std::vector<std::uint8_t> bytes;
        if (!LoadResourceBytes(kMovieType, kMovieGroup, instance, bytes) || !movie_.Open(std::move(bytes))) {
            if (Debug()) std::fprintf(stderr, "intro: clip %u not available\n", instance);
            return false;
        }
        if (!frame_ || static_cast<int>(frame_->Width()) != movie_.Width() || static_cast<int>(frame_->Height()) != movie_.Height()) {
            if (frame_) frame_->Release();
            frame_ = new cGZBuffer();
            frame_->AddRef();
            frame_->Init(static_cast<std::uint32_t>(movie_.Width()), static_cast<std::uint32_t>(movie_.Height()), cGZBufferColorType{kBufferColorARGB8888, 32});
        }
        startMs_ = -1.0;   // the clock starts with the first paint
        if (GameAudio* audio = CurrentGameAudio()) sound_.Open(audio->SoundGain());
        if (Debug()) std::fprintf(stderr, "intro: clip %u %dx%d stoppable %d\n", instance, movie_.Width(), movie_.Height(), int(stoppable_));
        InvalidateSelf();
        return true;
    }

    // SetDoneNotification (0x004463c6): with no movie playing the message goes out at once.
    void SetDoneNotification(cIGZWin* target, std::uint32_t message) {
        if (target) target->AddRef();
        if (target_) target_->Release();
        target_ = target;
        doneMessage_ = message;
        if (!movie_.IsOpen() && target) Post(target, message);
    }

    // StopMovie (0x0044644c): true when a stoppable movie was playing.
    bool StopMovie() {
        if (!stoppable_ || !movie_.IsOpen()) return false;
        CloseMovie();
        return true;
    }

    // GZPaint (0x00446482): play on; when the movie ends, close it and tell the target.
    bool GZPaint() override {
        if (!movie_.IsOpen()) return true;
        const double now = NowMs();
        if (startMs_ < 0) startMs_ = now;
        if (movie_.AdvanceTo((now - startMs_) / 1000.0)) CopyFrame();
        std::vector<std::int16_t> samples;
        movie_.TakeAudio(samples);
        sound_.Push(samples);
        DrawFrame();
        if (movie_.Finished()) {
            CloseMovie();
            if (target_) Post(target_, doneMessage_);
        }
        InvalidateSelf();
        return true;
    }

private:
    void Post(cIGZWin* target, std::uint32_t message) {
        if (cIGZWinMgr* mgr = GetWindowManager()) {
            cGZMessage m;
            m.type = message;
            mgr->PostMessageToTarget(target, m);
        }
    }
    void CloseMovie() {
        movie_.Close();
        sound_.Close();
    }
    void CopyFrame() {
        if (!frame_ || !movie_.Pixels() || !frame_->Lock(cIGZBuffer::kLockWrite)) return;   // the write lock bumps the change counter
        auto* bits = static_cast<std::uint8_t*>(frame_->GetColorSurfaceBits());
        const std::uint32_t stride = frame_->GetColorSurfaceStride();
        const std::size_t row = static_cast<std::size_t>(movie_.Width()) * 4;
        for (int y = 0; y < movie_.Height(); ++y) std::copy_n(movie_.Pixels() + row * static_cast<std::size_t>(y), row, bits + stride * static_cast<std::uint32_t>(y));
        frame_->Unlock(cIGZBuffer::kLockWrite);
    }
    // The clip's place on the screen (QueueMovie's bounds, aspect kept: see the top).
    cRZRect MovieRect() const {
        const int screenW = GetW(), screenH = GetH(), w = movie_.Width(), h = movie_.Height();
        int scale = static_cast<int>(std::min(std::floor(static_cast<float>(screenW) / static_cast<float>(w) + 0.5f),
                                              std::floor(static_cast<float>(screenH) / static_cast<float>(h) + 0.5f)));
        int dw = w * scale, dh = h * scale;
        if (scale < 1 || dw > screenW || dh > screenH) {
            const float fit = std::min(static_cast<float>(screenW) / static_cast<float>(w), static_cast<float>(screenH) / static_cast<float>(h));
            dw = static_cast<int>(static_cast<float>(w) * fit);
            dh = static_cast<int>(static_cast<float>(h) * fit);
        }
        const int x = (screenW - dw) / 2, y = (screenH - dh) / 2;
        return {x, y, x + dw, y + dh};
    }
    void DrawFrame() {
        auto* context = static_cast<gz::cIGZDrawContext*>(drawContext_);
        if (!context || !frame_) return;
        const cRZRect source{0, 0, movie_.Width(), movie_.Height()};
        context->Blt(frame_, source, MovieRect());
    }

    Movie movie_;
    MovieSound sound_;
    cGZBuffer* frame_ = nullptr;
    cIGZWin* target_ = nullptr;          // +0xe4
    std::uint32_t doneMessage_ = 0;      // +0xe8
    bool stoppable_ = false;             // +0x120
    double startMs_ = -1.0;
};

// cSC4WinIntroVideoScreen.
class IntroVideoScreen final : public cGZWin {
public:
    explicit IntroVideoScreen(cIGZWinMgr* mgr) : cGZWin(mgr) { SetFlag(kWinPrivateBuffer, false); }   // 0x001ce900
    ~IntroVideoScreen() override { Shutdown(); }

    bool Init() override {   // 0x001ce9f8
        if (get_init_flag()) return true;
        cGZWin::Init();
        cIGZWin* main = GetMainWindow();
        SetArea(0, 0, main->GetW(), main->GetH());
        player_ = new MoviePlayerWin(GetWindowManager());
        player_->AddRef();
        ChildAdd(player_);
        player_->SetArea(0, 0, GetW(), GetH());   // port: the whole screen (the original: 768x384, centred, but drawn full screen)
        player_->QueueMovie(kCopyrightVideo, 1);
        player_->SetDoneNotification(this, kMsgCopyrightDone);
        return true;
    }
    bool Shutdown() override {   // 0x001cebac
        if (!get_init_flag()) return true;
        for (cIGZWin** child : {&logo_, &splash_}) {
            if (!*child) continue;
            ChildDelete(*child);   // vslot 0x40, then the screen's own reference
            (*child)->Release();
            *child = nullptr;
        }
        if (player_) {
            ChildDelete(player_);
            player_->Release();
            player_ = nullptr;
        }
        return cGZWin::Shutdown();
    }

    bool DoMessage(cGZMessage& message) override {   // 0x001cef7c
        switch (message.type) {
        case kMsgGoToRegion:
            if (Debug()) std::fprintf(stderr, "intro: to the region screen\n");
            if (g_services.destroyCurrentScreen) g_services.destroyCurrentScreen();
            if (g_services.createRegionScreen) g_services.createRegionScreen();
            return true;
        case kMsgIntroDone:
            if (splash_) {
                splash_->PullToFront();
                splash_->ShowWindow();
            }
            timerStart_ = NowMs();
            return true;
        case kMsgCopyrightDone:
            logo_ = SetupScriptWindow(kLogoScript, 1000, 1000);   // SetupMaxisLogoScreen
            splash_ = SetupScriptWindow(kSplashScript, 2000, 0);   // SetupSplashScreen
            if (logo_) logo_->ShowWindow();
            else Post(kMsgPlayReal2);   // without the logo window nothing would move the sequence on
            timerStart_ = NowMs();
            return true;
        case kMsgPlayMainIntro:
            if (player_) {
                player_->QueueMovie(kMainIntroVideo, 2);
                player_->SetDoneNotification(this, kMsgIntroDone);
            }
            return true;
        case kMsgUserStop:
            if (Debug()) std::fprintf(stderr, "intro: skipped\n");
            if (g_services.destroyCurrentScreen) g_services.destroyCurrentScreen();
            if (g_services.createSplashScreen) g_services.createSplashScreen();
            return true;
        case kMsgPlayReal2:
            if (player_) {
                player_->QueueMovie(kMainIntroVideoReal2, 0);
                player_->SetDoneNotification(this, kMsgPlayMainIntro);
            }
            return true;
        default:
            return cGZWin::DoMessage(message);
        }
    }

    bool GZOnKeyDown(std::uint32_t key, std::uint32_t) override {   // 0x001cf154
        if (key == kKeyEscape) DoUserStop();
        return false;
    }
    bool GZOnMouseDownL(int, int, std::uint32_t) override {   // 0x001cf170
        DoUserStop();
        return false;
    }

    bool GZPaint() override {   // 0x001cf186
        if (paintCounter_ == 0) paintCounter_ = 2;
        if (g_services.clearScreenToBlack) g_services.clearScreenToBlack();
        const double elapsed = timerStart_ < 0 ? 0.0 : NowMs() - timerStart_;
        if (logo_) {
            if (logo_->IsVisible() && elapsed > 5000) logo_->HideWindow();   // fades out (1000 ms)
            if (elapsed > 6000) {
                ChildDelete(logo_);
                logo_->Release();
                logo_ = nullptr;
                Post(kMsgPlayReal2);
            }
            paintCounter_ = 2;
        }
        if (splash_ && splash_->IsVisible()) {
            if (elapsed > 2000 && !regionPosted_) {
                regionPosted_ = true;
                Post(kMsgGoToRegion);
            }
            paintCounter_ = 2;
        }
        if (--paintCounter_ != 0) InvalidateSelf();
        return true;
    }

private:
    // DoUserStop (0x001cf0ee): only once the logo is gone, and only a stoppable clip.
    void DoUserStop() {
        if (logo_ || !player_) return;
        if (player_->StopMovie()) Post(kMsgUserStop);
    }
    void Post(std::uint32_t type) {
        if (cIGZWinMgr* mgr = GetWindowManager()) {
            cGZMessage m;
            m.type = type;
            mgr->PostMessageToTarget(this, m);
        }
    }
    // SetupMaxisLogoScreen (0x001ced24) / SetupSplashScreen (0x001cee50): the script window centred (not above / left of the
    // screen), in front, hidden, fading with the given periods. One reference held.
    cIGZWin* SetupScriptWindow(std::uint32_t instance, int fadeIn, int fadeOut) {
        cIGZWin* window = CreateWindowFromScript(cGZPersistResourceKey{0, kScriptGroup, instance}, this, kScriptChildId);
        if (!window) return nullptr;
        const int x = std::max(0, (GetW() - window->GetW()) / 2), y = std::max(0, (GetH() - window->GetH()) / 2);
        window->GZWinMoveTo(x, y);
        window->PullToFront();
        window->HideWindow();
        window->SetFlag(kWinFade, true);
        window->SetFadeEffectPeriod(fadeIn, fadeOut);
        return window;
    }

    MoviePlayerWin* player_ = nullptr;   // +0xe4
    cIGZWin* logo_ = nullptr;            // +0xe8
    cIGZWin* splash_ = nullptr;          // +0xec
    double timerStart_ = -1.0;           // the cRZTimer at +0xf0 (milliseconds)
    int paintCounter_ = 0;               // +0x104
    bool regionPosted_ = false;
};

}  // namespace

void SetIntroServices(const IntroServices& services) { g_services = services; }

bool IntroVideosAvailable() {
    if (!Movie::Available()) return false;
    std::vector<std::uint8_t> bytes;
    return LoadResourceBytes(kMovieType, kMovieGroup, kMainIntroVideo, bytes) && !bytes.empty();
}

cIGZWin* CreateIntroVideoScreen(cIGZWinMgr* manager) {
    if (!IntroVideosAvailable()) {
        if (Debug()) std::fprintf(stderr, "intro: no movie decoder or no Intro.dat\n");
        return nullptr;
    }
    auto* screen = new IntroVideoScreen(manager);
    return static_cast<cIGZWin*>(static_cast<cGZWin*>(screen));
}

}  // namespace native
