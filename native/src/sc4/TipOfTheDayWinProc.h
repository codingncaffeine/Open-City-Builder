// cSC4TipOfTheDayWinProc — the tip of the day dialog (Mac i386 0x0025ec50..0x0025f6de; spec tip-of-the-day.md).
//
// The tips are the exemplars whose exemplar type (property 0x10) is 0x29; each one's text is the string its property
// 0x6a3e9d02 refers to. The city view opens the dialog once per run after a city loads, when tips exist (the shipped game
// data has none: only plugins add them). The dialog (UI script {0, 0x96a006b0, 0xca3bdd94}, window 0x4a3be152) shows one
// tip at a time in an HTML control laid over the placeholder 0xaa3be17e; Next (0x2a3ea604) and Previous (0x4a3ea5fd)
// wrap around; the close buttons (0x42b7c353, 0x42b7c356, 0xaa3be170) store the check box 0xaa3be181 and the last tip's
// instance in the preferences (+0x49c, +0x4a0) and close it.
//
// Quirks kept: the dialog starts at the tip after the last one shown, so with no match it starts at the second tip;
// EntriesPresent scans once per run; the view does not read the check box.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "gz/WinProc.h"
#include "gz/cGZPersistResourceKey.h"
#include "rz/cRZString.h"

class cIGZWin;
class cIGZWinHTML;
namespace gz { class cIGZWinGen; }

namespace sc4 {

constexpr std::uint32_t kTipScript = 0xca3bdd94, kTipWindowId = 0x4a3be152, kTipPlaceholder = 0xaa3be17e,
                        kTipCheckBox = 0xaa3be181, kTipNext = 0x2a3ea604, kTipPrevious = 0x4a3ea5fd;
constexpr std::uint32_t kTipExemplarType = 0x29, kTipTextProperty = 0x6a3e9d02;
constexpr std::size_t kTipPrefShowTips = 0x49c, kTipPrefLastTip = 0x4a0;

class ITipOfTheDayHost {
public:
    virtual ~ITipOfTheDayHost() = default;
    virtual cIGZWin* DialogParent() = 0;   // the window the app keeps at 0x00b43cc0 (the game window)
    // SC4CreateWindowFromScript(key, parent, id, iid, out): one counted reference in *out.
    virtual bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                        std::uint32_t iid, void** out) = 0;
    virtual cIGZWinHTML* CreateHtmlControl() = 0;   // GZCOM 0xaa12e5f5 / 0x4a11fd4a, one reference
    // Every exemplar's key in the resource manager's order (resource manager slot 18 with type 0x6534284a).
    virtual void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) = 0;
    virtual bool ExemplarType(const cGZPersistResourceKey& key, std::uint32_t& type) = 0;   // property 0x10
    virtual bool TipText(const cGZPersistResourceKey& key, cRZString& out) = 0;             // property 0x6a3e9d02
    virtual std::uint8_t* Preferences() = 0;   // SL::spPreferences, null without
};
ITipOfTheDayHost* TipOfTheDayHost();
void SetTipOfTheDayHost(ITipOfTheDayHost* host);

class cSC4TipOfTheDayWinProc final : public gz::cIGZWinProc {
public:
    cSC4TipOfTheDayWinProc() = default;                           // @ 0x0025f4ac
    virtual ~cSC4TipOfTheDayWinProc();                            // @ 0x0025f64e

    bool QueryInterface(std::uint32_t iid, void** out) override;  // @ 0x0025ec50
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // @ 0x0025f324

    static bool HasBeenShownAlready();                            // @ 0x0025ecc0
    static bool EntriesPresent();                                 // @ 0x0025f382
    // Creates the dialog under the dialog parent (none when one is up); the proc goes to *out when out is not null.  @ 0x0025f500
    static bool DoModelessWin(cSC4TipOfTheDayWinProc** out);
    static void ResetForTest();                                   // the run's statics back to their start

    void InitDialog();                                            // @ 0x0025f164
    void EndDialog();                                             // @ 0x0025eccc
    void GoToNextTip(bool forward);                               // the step @ 0x0025f006
    std::size_t TipCount() const { return tips_.size(); }
    std::uint32_t TipIndex() const { return index_; }
    cIGZWin* Window() const;

private:
    bool DoCommand(cGZMessage& message);                          // @ 0x0025f08a
    void GoToTip(const cGZPersistResourceKey& key);               // @ 0x0025eea4
    void LoadAllExemplarData();                                   // @ 0x0025f0f6 (+ callback 0x0025f3c4)

    std::uint32_t refCount_ = 0;
    std::vector<cGZPersistResourceKey> tips_;   // +0x08
    std::uint32_t index_ = 0;                   // +0x14
    gz::cIGZWinGen* window_ = nullptr;          // +0x18
    cIGZWinHTML* html_ = nullptr;               // +0x1c
};

}  // namespace sc4
