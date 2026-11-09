// cSC4AdvisorUIAdvisorWinProc — the advisor window (the mayor panel's "Advisors" button). 34 functions in the Mac i386
// build, 0x001fd314..0x002008ae (decompile scratch/s50/ghidra/advwin.c, notes scratch/s50/advwin-notes.md); not
// recovered in the 64-bit sources. The advisor system creates one (IAdvisorSystemHost::CreateAdvisorWinProc), calls
// Init, and later ShowWindow / HideWindow / IsVisible / Shutdown.
//
// Init builds the window script {0, 0x96a006b0, 0xcbc905cd} (window 0x6a15c767, as cIGZWinGen) inside the city view and
// aligns the windows it added. The script holds three layouts, all hidden at start:
//   0x6a15c767  all seven advisors: one animated head per advisor (image windows cImageArray[i]), a button per head;
//   0xaa15ef06  one advisor, small: head 0xea15efc7, advice list 0x100100;
//   0x2a1d96b1  one advisor, large: head 0xaa1d97df, advice list 0x100101.
// Clicking a head opens that advisor's page (SetIndividualAdvisorWindow): the caption "Name - PersonalName" goes on the
// page and on 0x2a15f5e8 / 0xea1d980d, the advisor's current advices fill the list (GetMessageList, in reverse map
// order), and updateBodyTextCtrl writes one list entry per advice, with the header color and the icon set picked by its
// mood (0 alarm with the blinking header, 1 bad, 5 good, anything else neutral; greyed when no longer current).
// Pressing a head's button shifts that head by the press state (image proc SetOffset).
//
// Messages while the window is attached: 0x8a09fa64 advice added and 0xca09fa6c removed (only for the advisor shown),
// 0x8a09fa65 changed (enables 0xaa1d969d / 0x6a1d96c3 for the shown advisor; updateBodyTextCtrl then disables them again
// = original order), 0x8a09fa67 advisor removed, 0x426841a1 a list's mood toggle (data1 advisor id, data2 mood -> that
// head's SetMood). Each one refreshes every head's mood (SetAllMoods) and the list.
//
// Moods per head: get_advisor_mood = the lowest mood of the advisor's current advices, starting from 7 and stopping
// at 0; an advisor without advices is 3 (neutral).
#pragma once
#include <cstdint>
#include <list>
#include <map>

#include "gz/WinGen.h"
#include "gz/WinProc.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorUIImageWinProc.h"
#include "sc4/WinAdviceList.h"

struct cGZPersistResourceKey;
struct cGZMessage;
class cIGZWin;

namespace sc4 {

class cISC4Advice;

constexpr std::uint32_t kGZIID_cSC4AdvisorUIAdvisorWinProc = 0x22e85d8e;
constexpr int kAdvisorWindowAdvisorCount = 7;
// kAdvisorIndexToIDTable: the advisor id of each head (index 0..6).
constexpr int kAdvisorWindowAdvisorIds[kAdvisorWindowAdvisorCount] = {0x19, 0x15, 0x16, 0x17, 0x18, 0x1a, 0x1b};
// cImageArray: the head image window of each index in the all-advisors layout.
constexpr std::uint32_t kAdvisorWindowImageIds[kAdvisorWindowAdvisorCount] = {
    0x0a15c7a1, 0x6a15c7b5, 0x4a15c7c6, 0x0a15c7d8, 0x6a15c7ea, 0xea15c7fa, 0x8a15c80c};
// The head buttons of the all-advisors layout, same index order.
constexpr std::uint32_t kAdvisorWindowHeadButtons[kAdvisorWindowAdvisorCount] = {
    0xaa15c795, 0x6a15c7aa, 0x6a15c7be, 0xca15c7cf, 0xaa15c7e2, 0x2a15c7f1, 0x8a15c802};

constexpr std::uint32_t kAdvisorWindowScriptGroup = 0x96a006b0;
constexpr std::uint32_t kAdvisorWindowScriptInstance = 0xcbc905cd;
constexpr std::uint32_t kAdvisorWindowAll = 0x6a15c767;          // also the script window Init asks for
constexpr std::uint32_t kAdvisorWindowSmall = 0xaa15ef06;
constexpr std::uint32_t kAdvisorWindowLarge = 0x2a1d96b1;
constexpr std::uint32_t kAdvisorWindowSmallImage = 0xea15efc7;
constexpr std::uint32_t kAdvisorWindowLargeImage = 0xaa1d97df;
constexpr std::uint32_t kAdvisorWindowSmallList = 0x100100;
constexpr std::uint32_t kAdvisorWindowLargeList = 0x100101;
constexpr std::uint32_t kAdvisorWindowCaption1 = 0x2a15f5e8;
constexpr std::uint32_t kAdvisorWindowCaption2 = 0xea1d980d;
constexpr std::uint32_t kAdvisorWindowButtonA = 0xaa1d969d;    // enabled on "advice changed", disabled by each rebuild
constexpr std::uint32_t kAdvisorWindowButtonB = 0x6a1d96c3;
constexpr std::uint32_t kAdvisorWindowListImage = 0x14416264;   // the advice list's background (and every entry's)
constexpr std::uint32_t kAdvisorWindowImageFlag = 0x8000000;    // set on every head image window
// Buttons (command 0x287259f6 ids).
constexpr std::uint32_t kAdvisorWindowClose[] = {0x6a19d8f2, 0xaa19dc53, 0x0a1d96cc};
constexpr std::uint32_t kAdvisorWindowToLarge[] = {0x2a84b45a, 0xca19dd28};
constexpr std::uint32_t kAdvisorWindowToSmall[] = {0x2a84b45b, 0xaa1d9734};
constexpr std::uint32_t kAdvisorWindowToAll[] = {0x2a1d973e, 0x8a15efe6, 0x6a1d97d5, 0x0a15efbc};
// Every control whose notifications go to the script window (kWinProcInit), in the original's order.
constexpr std::uint32_t kAdvisorWindowNotifiers[] = {
    0xca19dd28, 0x2a84b45a, 0x8a15efe6, 0x0a15efbc, 0xaa1d9734, 0x2a84b45b, 0x2a1d973e, 0x6a1d97d5,
    0x4a15efde, 0xca1d974a, 0xaa15c795, 0x6a15c7aa, 0x6a15c7be, 0xca15c7cf, 0xaa15c7e2, 0x2a15c7f1,
    0x8a15c802, 0xaa1d969d, 0x6a1d96c3, 0x6a19d8f2, 0xaa19dc53, 0x0a1d96cc};

// Messages.
constexpr std::uint32_t kAdvisorWindowAdviceAdded = 0x8a09fa64;
constexpr std::uint32_t kAdvisorWindowAdviceChanged = 0x8a09fa65;
constexpr std::uint32_t kAdvisorWindowAdvisorRemoved = 0x8a09fa67;
constexpr std::uint32_t kAdvisorWindowAdviceRemoved = 0xca09fa6c;
constexpr std::uint32_t kAdvisorWindowMoodToggled = 0x426841a1;   // kAdviceListMsgAdviceToggled
constexpr std::uint32_t kAdvisorWindowButtonPressed = 0x287259f8; // kBtnCmdPressedChanged

// Everything the window reaches outside itself (one method per original global / engine call).
class IAdvisorUIAdvisorHost {
public:
    virtual ~IAdvisorUIAdvisorHost() = default;
    // SL::spView3DWin (borrowed; null before the city view exists).
    virtual cIGZWin* View3DWindow() = 0;
    // SL::spCity != null.
    virtual bool CityPresent() = 0;
    // SL::spCity vslot 0x144 (the advisor system) -> GetAdvisor(id); borrowed, null without a city.
    virtual cISC4Advisor* Advisor(int advisorId) = 0;
    // SC4CreateWindowFromScript(key, parent, id, iid, out): one reference in *out.
    virtual bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                        std::uint32_t iid, void** out) = 0;
    // SL::spMS2.
    virtual cIGZMessageServer2* Messages() = 0;
    // nsSC4TutorialSystem::FilterUICommandMessage (its answer is not used).
    virtual void FilterUICommandMessage(cGZMessage& message) = 0;
};
IAdvisorUIAdvisorHost* AdvisorUIAdvisorHost();
void SetAdvisorUIAdvisorHost(IAdvisorUIAdvisorHost* host);

class cSC4AdvisorUIAdvisorWinProc : public gz::cIGZWinProc, public cIGZMessageTarget2 {
public:
    cSC4AdvisorUIAdvisorWinProc();             // @ 0x0020069c: reference count 0, ids -1
    virtual ~cSC4AdvisorUIAdvisorWinProc();    // @ 0x00200758

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x001fd428
    std::uint32_t AddRef() override;                               // @ 0x001fd46a
    std::uint32_t Release() override;                              // @ 0x001fd47c
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // @ 0x002000ba
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x001ff0b8

    void Init();                                                   // @ 0x001ff692
    void Shutdown();                                               // @ 0x001fd886
    void ShowWindow();                                             // @ 0x001fdbe4
    void HideWindow();                                             // @ 0x001fdaee
    bool IsVisible() const { return visible_; }                    // @ 0x001fd664

    void SetAllAdvisorsWindow();                                   // @ 0x001ff590
    void SetIndividualAdvisorWindow(int advisorId);                // @ 0x001ffd78
    void SetSmallIndividualAdvisorWindow();                        // @ 0x001ffacc
    void SetLargeIndividualAdvisorWindow();                        // @ 0x001ffc22
    void updateBodyTextCtrl();                                     // @ 0x001fdc9a
    void RemoveMessage(cISC4Advice* advice);                       // @ 0x001fef02
    void AddMessage(cISC4Advice* advice);                          // @ 0x001fef32
    int get_advisor_mood(cISC4Advisor* advisor);                   // @ 0x001fef90
    void SetAllMoods();                                            // @ 0x001ff036
    void GetMessageList(cISC4Advisor* advisor);                    // @ 0x001ff9e2

    // Harness views.
    gz::cIGZWinGen* Window() const { return window_.get(); }
    int AdvisorId() const { return advisorId_; }
    int AdvisorIndex() const { return advisorIndex_; }
    int Layout() const { return layout_; }
    bool Large() const { return large_; }
    bool NotificationsRegistered() const { return notificationsRegistered_; }
    cIGZWinAdviceList* AdviceList() const { return adviceList_.get(); }
    const std::list<cRZAutoRefCount<cISC4Advice>>& Messages() const { return messages_; }
    cSC4AdvisorUIImageWinProc* Head(int index) const { return heads_[index].get(); }

private:
    void RegisterMessageNotification(bool on);                     // @ 0x001fd314
    void SetupAllAdvisorWinProcs();                                // @ 0x001ff3f0
    void SetupSingleAdvisorWinProc(int advisorId, std::uint32_t imageWindowId);   // @ 0x001ff840
    void RemoveAllAdvisorWinProcs();                               // @ 0x001fd80e
    void RemoveSingleAdvisorWinProc(int advisorId);                // @ 0x001fda22
    void EnableAllAdvisorWinProcs(bool enable);                    // @ 0x001fd7d0
    void EnableSingleAdvisorWinProc(int advisorId, bool enable);   // @ 0x001fda98
    void AttachHead(int index, cIGZWin* imageWindow);              // the shared body of the two Setup functions
    cIGZWin* Parent() const;                                       // window_->AsIGZWin()->GetParentWin()
    cIGZWin* Child(std::uint32_t id) const;                        // Parent()->GetChildWindowFromIDRecursive(id)
    void FindAdviceList(cIGZWin* page, std::uint32_t listId);

    std::uint32_t refCount_ = 0;                                   // +0x08
    cRZAutoRefCount<cISC4Advisor> advisor_;                        // +0x0c the advisor shown
    int advisorId_ = -1;                                           // +0x10
    int advisorIndex_ = -1;                                        // +0x14 (< 7: one advisor shown)
    cRZAutoRefCount<gz::cIGZWinGen> window_;                       // +0x18 the script window
    cRZAutoRefCount<gz::cIGZWinGen> headWindows_[kAdvisorWindowAdvisorCount];   // +0x1c
    cRZAutoRefCount<cSC4AdvisorUIImageWinProc> heads_[kAdvisorWindowAdvisorCount];   // +0x38
    bool notificationsRegistered_ = false;                         // +0x54
    bool large_ = false;                                           // +0x55 the large single-advisor layout was chosen
    bool visible_ = false;                                         // +0x56
    int layout_ = 0;                                               // +0x58 0 all, 1 small, 2 large
    cRZAutoRefCount<cIGZWinAdviceList> adviceList_;                // +0x5c
    std::list<cRZAutoRefCount<cISC4Advice>> messages_;             // +0x60
    AdviceMap advices_;                                            // +0x68 copy of the advisor's current advices
};

}  // namespace sc4
