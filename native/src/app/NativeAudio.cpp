#include "app/NativeAudio.h"

#include "app/NativeAudioDirector.h"
#include "gz/RZServices.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ToolTipManager.h"
#include <cstdio>
#include <cstdlib>

#include "audio/AudioServices.h"
#include "audio/GZSndSys.h"
#include "audio/interfaces/cIGZSnd.h"
#include "gz/WinBtn.h"
#include "gz/WinCtrlFactory.h"
#include "sc4/AudioPreferencesWinProc.h"
#include "gz/WinFont.h"

namespace native {
namespace {
// RZ::SS() / RZ::WCM for the window buttons: the sounds play; the captions take the game's font styles (the same source the
// text windows use), so the colour manager counts as present once that source is up; its stock images come from the
// control factory's table (cSC4App::SetupWinCtrlMgr's).
class ButtonHost final : public gz::IWinButtonHost {
public:
    gz::IWinButtonFont* Font(std::uint32_t style) override {
        gz::IWinFontSource* fonts = gz::WinFontSource();
        return fonts && fonts->Available() ? fonts->Font(style) : nullptr;
    }
    cIGZBuffer* Image(std::uint32_t id) override { gz::IWinCtrlFactory* stock = gz::WinCtrlFactory(); return stock ? stock->Image(id) : nullptr; }
    std::uint32_t DefaultSoundId(int) override { return 0; }
    void PlaySound(std::uint32_t soundId) override { PlayGameSound(soundId); }
    // RZ::MS(): the tip request goes to the tool tip manager (cSC4ToolTipManager) as message 0x22c010d0, the hide as 0x22c010d2.
    void ShowTip(const gz::BtnTipRequest& request) override {
        if (cIGZMessageServer* messages = RZ::MS()) messages->MessageSend(cGZMessage{kMsgToolTipShow, reinterpret_cast<std::uint64_t>(&request), 0, 0});
    }
    void HideTip() override {
        if (cIGZMessageServer* messages = RZ::MS()) messages->MessageSend(cGZMessage{kMsgToolTipHide, 0, 0, 0});
    }
    bool HasWindowColorManager() override { gz::IWinFontSource* fonts = gz::WinFontSource(); return fonts && fonts->Available(); }
};
ButtonHost g_buttons;
}  // namespace

void PlayGameSound(std::uint32_t soundId) {
    if (!soundId || !AudioRunning()) return;
    auto* system = static_cast<cIGZSndSys*>(RZ::SS());
    if (!system) return;
    const bool posted = system->PostEventIds(1, soundId, 0, 0, 0);
    if (std::getenv("SC4_AUDIODEBUG")) std::fprintf(stderr, "audio: sound %08x posted %d\n", soundId, int(posted));
}
float EffectsGain() {
    return AudioRunning() ? sc4::cSC4AudioPreferencesWinProc::SoundGain(SL::spPreferences) : 0.0f;
}
void InstallButtonSounds() { gz::SetWinButtonHost(&g_buttons); }

}  // namespace native
