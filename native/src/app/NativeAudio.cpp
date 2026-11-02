#include "app/NativeAudio.h"

#include "app/GameAudio.h"
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
GameAudio* g_audio = nullptr;

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
    void ShowTip(const gz::BtnTipRequest&) override {}
    void HideTip() override {}
    bool HasWindowColorManager() override { gz::IWinFontSource* fonts = gz::WinFontSource(); return fonts && fonts->Available(); }
};
ButtonHost g_buttons;
}  // namespace

void SetGameAudio(GameAudio* audio) { g_audio = audio; }
GameAudio* CurrentGameAudio() { return g_audio; }
void ApplyAudioPreferences(const std::uint8_t* preferences) {
    if (!g_audio || !preferences) return;
    using sc4::cSC4AudioPreferencesWinProc;
    const float music = cSC4AudioPreferencesWinProc::MusicGain(preferences), sound = cSC4AudioPreferencesWinProc::SoundGain(preferences);
    g_audio->SetMusic(music > 0.0f, music);
    g_audio->SetSound(sound > 0.0f, sound);
    if (auto* system = dynamic_cast<cGZSndSys*>(Audio::SoundSystem()))
        system->StoreMasterVolume(static_cast<std::int32_t>(sound * 0x400 + 0.5f));   // the game's own sounds
    g_audio->SetSongFilter([](const std::string& path) { return cSC4AudioPreferencesWinProc::IsSongEnabled(path); });
}
void PlayGameSound(std::uint32_t soundId) {
    if (!soundId) return;
    // The game's own path first: the sound manager builds the sound from its track definition and the hit manager plays it.
    if (cIGZSndMgr* manager = Audio::SoundManagerService()) {
        cIGZSnd* sound = nullptr;
        if (manager->GetSoundById(soundId, 0, nullptr, kGZIID_cIGZSnd, reinterpret_cast<void**>(&sound)) && sound) {
            const bool played = sound->PlayWithVolumeAndLoop(0x400, false, 0);
            if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: sound %08x played %d\n", soundId, int(played));
            sound->Release();
            if (played) return;
        } else if (std::getenv("SC4_DEBUG")) {
            std::fprintf(stderr, "audio: sound %08x not built by the sound manager\n", soundId);
        }
    }
    if (g_audio) g_audio->PlayTrack(soundId);   // the direct track lookup when the sound system is not up
}
void InstallButtonSounds() { gz::SetWinButtonHost(&g_buttons); }

}  // namespace native
