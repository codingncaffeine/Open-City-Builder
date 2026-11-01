// Dialog window procedures and the script window factory (cSC4DialogWinProcBase 14, cSC4CreditsWinProc 11 functions,
// 0x1003cd4bc..0x1003d86a8; SC4CreateWindowFromScript 0x100039a9f/0x100039b38/0x100039bf7).
//  * SC4CreateWindowFromScript asks the UI script service (id 0x5a356e15, iid 0xfa3562fa) to build a window from the
//    UI script resource {0, 0x96a006b0, instance}; the overload with an interface id answers that interface.
//  * cSC4DialogWinProcBase: a modal dialog. Display() builds the window (resource instance +0x20, window id +0x24),
//    attaches itself as the window procedure, restores the saved window position, runs the dialog modally and saves the
//    new position. The attach/detach messages call GZOnAttach/GZOnDetach; command messages from sender 0x287259f6 go to
//    GZOnButtonActivate (0xca5b9966 = OK, 0x8a5b9959 = Cancel); when the window loses focus without the "keep focus"
//    flag 0x1000 the control with the default-focus id (+0x28) takes it back. OK/Cancel end the dialog (code 0xf, 1/0).
//  * cSC4CreditsWinProc: the credits dialog (script instance 0xca551016, window id 0x0a592004). The HTML text view it
//    fills (cGZWinHTML) is set up through ICreditsView.
#pragma once
#include <cstdint>
#include <functional>

#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinProc.h"
#include "gz/cGZPersistResourceKey.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/WinLocationSaver.h"

namespace sc4 {

constexpr std::uint32_t kServiceUIScript = 0x5a356e15, kIIDUIScript = 0xfa3562fa;
constexpr std::uint32_t kUIScriptGroup = 0x96a006b0;

// The UI script service (slot 3 builds a window, slot 4 builds it and answers an interface).
class cIGZUIScriptService : public cIGZUnknown {
public:
    virtual cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) = 0;
    virtual bool CreateWindowFromScriptAs(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                          std::uint32_t iid, void** out) = 0;
};
cIGZWin* SC4CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id);
bool SC4CreateWindowFromScript(std::uint32_t instance, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out);
bool SC4CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out);

// Things the dialogs need around them.
struct DialogServices {
    std::function<cSLWinLocationSaver*()> locationSaver;       // SL::spWinLocationSaver
    std::function<void(bool)> busyCursor;                      // cRZAutoCursor(0xf) around attaching the procedure
    std::function<void(const cIGZString& in, cIGZString& out)> detokenize;   // the string detokenizer (%token% -> text)
};
DialogServices& GetDialogServices();

class cSC4DialogWinProcBase : public gz::cIGZWinProc {
public:
    cSC4DialogWinProcBase(std::uint32_t resourceInstance, std::uint32_t windowId, std::uint32_t defaultFocusId = 0)
        : resourceInstance_(resourceInstance), windowId_(windowId), defaultFocusId_(defaultFocusId) {}
    virtual ~cSC4DialogWinProcBase() = default;   // Release deletes the derived dialogs through this class
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;

    virtual std::uint32_t Display(cIGZWin* parent);                   // @ 0x1003d6a5a: the dialog's result
    virtual void GZOnAttach() {}
    virtual void GZOnDetach() {}
    virtual bool GZOnButtonActivate(std::uint32_t id);
    virtual void GZOnCancel();
    virtual void GZOnOK();

    cIGZWin* Window() const { return window_; }
    void EndDialog(std::uint32_t result);                            // @ 0x1003d6b7e: the result Display returns
    gz::cIGZWinBtn* GetBtnControl(std::uint32_t id);                     // @ 0x1003d6e3e
    void TranslateTokensInCaption(std::uint32_t id);                 // @ 0x1003d6cc6 (caption text with %tokens% resolved)

protected:
    std::uint32_t refCount_ = 0;           // +0xc (cRZRefCount at +8)
    cIGZWin* window_ = nullptr;            // +0x18
    std::uint32_t resourceInstance_;       // +0x20
    std::uint32_t windowId_;               // +0x24
    std::uint32_t defaultFocusId_;         // +0x28
};

class ICreditsView {
public:
    virtual ~ICreditsView() = default;
    // fills the HTML child (id 0x0a5d0e13) of the credits window with the credits text
    virtual bool Build(cIGZWin* creditsWindow, cIGZUnknown*& htmlView) = 0;
};
void SetCreditsView(ICreditsView* view);

class cSC4CreditsWinProc final : public gz::cIGZWinProc {
public:
    enum eCreditsWindowType { kStandard = 0 };
    cSC4CreditsWinProc() = default;
    ~cSC4CreditsWinProc();
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;

    bool ShowDialog(eCreditsWindowType type);         // @ 0x1003cd610
    bool DoModalWin();                                // @ 0x1003cd546
    void InitDialog();                                // @ 0x1003cd6a8
    void EndDialog();                                 // @ 0x1003cda8c

private:
    std::uint32_t refCount_ = 0;                      // +8
    gz::cIGZWinGen* dialog_ = nullptr;                    // +0x10
    cIGZUnknown* htmlView_ = nullptr;                 // +0x18
};

}  // namespace sc4
