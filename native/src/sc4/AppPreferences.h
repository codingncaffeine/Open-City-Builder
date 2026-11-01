// The application preferences blob (SimCity 4.cfg): 0x1734 bytes written verbatim as one record of a DBPF file
// (type 0xa9dd6e06, group 0x49dd6e08, instance 1, version 0x15). Three sub-structures live in it, each with a
// constructor that fills in the defaults (SC4AudioPreferences @ 0x10001384a, SC4AppPreferences @ 0x1000138c8,
// SC4TutorialPreferences2 @ 0x10001393c); the surrounding fields are set by cSC4App's constructor (0x1000039e2).
// The layout is the on-disk format, so the blob is kept as bytes with named offsets and typed accessors.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace sc4 {

constexpr std::uint32_t kPreferencesRecordType = 0xa9dd6e06;
constexpr std::uint32_t kPreferencesRecordGroup = 0x49dd6e08;
constexpr std::uint32_t kPreferencesRecordInstance = 1;
constexpr std::uint16_t kPreferencesRecordVersion = 0x15;
constexpr std::size_t kPreferencesSize = 0x1734;

namespace prefs {
// sub-structure bases / sizes
constexpr std::size_t kAudio = 0x004, kAudioSize = 0x67;                       // SC4AudioPreferences
constexpr std::size_t kApp = 0x4a4, kAppSize = 0x9ac;                          // SC4AppPreferences
constexpr std::size_t kTutorial = 0xf34, kTutorialSize = 0x800;                // SC4TutorialPreferences2
// fields set from the command line
constexpr std::size_t kAudioEnabled = 0x004;        // u8  (-audio / -sound)
constexpr std::size_t kMusicEnabled = 0x010;        // u32 (-radio / -music)
constexpr std::size_t kAudioEnabledMirror = 0x069;  // u8  (-audio / -sound)
constexpr std::size_t kSimulatorParameter = 0x470;  // float, default 50.0 (-sim)
constexpr std::size_t kSwitchXDD = 0x498;           // u8  (-XDD)
constexpr std::size_t kPauseOnFocusLoss = 0x4a4;    // u8  (-g p)
constexpr std::size_t kLanguageName = 0x4ac;        // 0x40 bytes (-l): the language to use, cleared by SavePreferences
constexpr std::size_t kLanguageId = 0x4a8;          // u32: the language in use (SavePreferences stores 1)
constexpr std::size_t kImeEnabled = 0x4ef;          // u8  (-IME)
constexpr std::size_t kFullScreen = 0xf17;          // u8  (-f / -w)
// fields addressed by cSC4App
constexpr std::size_t kAppResetFlag = 0x4a8;       // u32: the language id; SavePreferences stores 1
constexpr std::size_t kAppResetBlock = 0x4ac;      // 0x40 bytes zeroed by SavePreferences
constexpr std::size_t kAppResetByte = 0x4ef;       // u8: set to 1 by SavePreferences
constexpr std::size_t kVideoAutoConfigure = 0xf0c; // u8
constexpr std::size_t kVideoHardwareDriver = 0xf13;// u8: cleared when the hardware driver fails to initialise
// fields the "establish city" dialog reads and writes (cSC4View3DWin::DoEstablishCityDialog)
constexpr std::size_t kLastCityName = 0x06c;      // 0x200 bytes: the last city name entered
constexpr std::size_t kMayorName = 0x26c;         // 0x200 bytes: the mayor name, offered again for the next city
constexpr std::size_t kDifficultyLevel = 0x46c;   // u32: the last difficulty chosen (0 easy .. 2 hard)
constexpr std::size_t kVideoWidth = 0xf1c;         // u32 (default 1024)
constexpr std::size_t kVideoHeight = 0xf20;        // u32 (default 768)
constexpr std::size_t kVideoColorDepth = 0xf24;    // u32
}  // namespace prefs

class SC4Preferences {
public:
    SC4Preferences() { Reset(); }
    void Reset();                                          // cSC4App's constructor sequence

    static void ResetAudio(std::uint8_t* base);            // SC4AudioPreferences::SC4AudioPreferences
    static void ResetApp(std::uint8_t* base);              // SC4AppPreferences::SC4AppPreferences
    static void ResetTutorial(std::uint8_t* base);         // SC4TutorialPreferences2::SC4TutorialPreferences2

    std::uint8_t* data() { return bytes_.data(); }
    const std::uint8_t* data() const { return bytes_.data(); }
    static constexpr std::size_t size() { return kPreferencesSize; }

    template <class T> T Get(std::size_t offset) const { T value; std::memcpy(&value, bytes_.data() + offset, sizeof value); return value; }
    template <class T> void Set(std::size_t offset, T value) { std::memcpy(bytes_.data() + offset, &value, sizeof value); }

private:
    std::array<std::uint8_t, kPreferencesSize> bytes_{};
};

}  // namespace sc4
