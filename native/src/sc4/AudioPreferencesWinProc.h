// cSC4AudioPreferencesWinProc — the "Audio Options" dialog (25 functions; Mac i386 0x002c4d52..0x002c7d9c, PC
// 0x004f1f60..0x004f5200; written from those two decompilations, not byte-matched). Script {0, 0x96a006b0, 0xca53f06e},
// window id 0xea53f5db.
//
// Controls: Music / Sound check boxes (0x8a550c50 / 0x8a550c52), their sliders 0..1000 (0x8a550c51 / 0x8a550c53), the
// playlist grid (0x8a550c56: column 0 the song's name, column 1 a check box per song), Preview / Stop (0x8a550c57 /
// 0x8a550c58), Custom Tunes (0x8a550c5a), Accept / Cancel (0xea5509e4 / 0xea5509e3, Escape = Cancel).
// Every control writes the live preference blob as it changes (the sound follows at once); the dialog keeps the audio
// preferences it opened with and Cancel puts them back. Accept saves the preferences and stops the playing song when it
// was just switched off; both post 0x231bbf91 (preferences changed).
//
// The sound follows through the game's own audio messages, all posted on message server 2 (the audio event handler
// listens): UpdateSoundSys sends the sound manager's master volume event 0x2a5b69c0 to the sound system and posts
// 0xaa650860 (music on / off, music volume); UpdateExclusionList posts 0xa611b34 (the station, its song list, the
// playlist mode); Preview posts 0xaa611b6e with the song's path; Stop posts 0x2a611b51; the first Preview or Stop
// posts 0xaa650861 with 1 (the radio plays on demand) and EndDialog posts it with 0.
//
// The two playlists (one per radio station) come from the tuning exemplars {0x6534284a, 0xca25875d, 0x2a5e5854 /
// 0x2a5e5855}: properties 0x8a5e7b50 + i (i < 30) = {name string id, file string id, enabled by default}; the strings are
// in group 0x8a635d24. Their enable bytes live in the preference blob at +0x4a (playlist 0) and +0x2c (playlist 1).
// Not modelled: the Mac build's iTunes mode and custom tunes (the check box is shown disabled: there is no custom tunes
// folder natively yet).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "gz/WinGen.h"
#include "gz/WinProc.h"

namespace sc4 {

struct AudioPreferencesServices {
    std::function<cIGZWinMgr*()> windowManager;
    std::function<std::uint8_t*()> preferences;                 // SL::spPreferences (the 0x1734-byte blob)
    std::function<void()> savePreferences;                      // SL::spSC4 vslot 0x44
    std::function<void(bool begin)> gamePause;
    std::function<std::string()> dataDirectory;                 // "<GameData>/"
};
AudioPreferencesServices& GetAudioPreferencesServices();

class cSC4AudioPreferencesWinProc final : public gz::cIGZWinProc {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;

    bool ShowDialog();                                           // false when the script did not load or it is already up
    bool DoModalWin();
    void EndDialog();
    static bool Displayed();

    // Whether the radio may play `path` (a song of either playlist whose box is off answers false; files outside the
    // playlists answer true), and the gain (0..1) the blob's effects volume means (the intro movies' sound).
    static bool IsSongEnabled(const std::string& path);
    static float SoundGain(const std::uint8_t* preferences);

    // The statics the audio event handler calls at start-up (Mac i386 0x002c623c, 0x002c52f0, 0x002c4e66, 0x002c53c4,
    // 0x002c642e).
    enum eAudioType : std::uint32_t { kAudioTypeSound = 0, kAudioTypeMusic = 1 };
    static void StaticInit();                                    // the two playlists, once
    static void ReadTunerVariables();                            // the volume ranges and the fade time
    static std::uint32_t GetVolumeFromSliderValue(std::uint32_t sliderValue, eAudioType type);
    static void SetDefaultMusicListPreferences();                // every song's enable byte = its exemplar default
    // Posts the station's song list: mode 0 is the region station's playlist, 1 the mayor station's.
    static void UpdateExclusionList(std::uint32_t mode);
    static std::uint32_t FadeOutTimeMs();                        // kFadeOutTimeMs (tuning property 0x4a7cb3bf)

private:
    void UpdateSoundSys();                                       // 0x002c51fc
    void InitDialog();
    void InsertMusicList(int mode, int firstRow);
    bool HandleControlMessage(std::uint32_t id, cIGZWin* window, std::uint32_t value);
    bool SelectedSong(int& mode, int& index);

    std::uint32_t refCount_ = 0;
    gz::cIGZWinGen* dialog_ = nullptr;
    cIGZWin* grid_ = nullptr;
    bool onDemand_ = false;                                      // +0xc: Preview / Stop took the radio over
    std::uint8_t saved_[0x67] = {};                             // the audio preferences the dialog opened with
};

}  // namespace sc4
