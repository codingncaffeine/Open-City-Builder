// nSC4UI — the shared UI helper functions of the game (16 functions, 0x100309e70..0x10030bd27). It is a class of
// static helpers only (no instance): the window-set bookkeeping AddGUI runs around every script window, the caption
// driven window alignment, the cursor hit test, the palette ramp interpolation, the "#D:xxxxxxxx#" property token
// replacement and the three script dialogs (notification, confirmation, scripted modal) with their catalog builders.
//
// Recovered pieces:
//  * GetWindowChildrenSet / GetWindowChildrenNotInSet: the direct children (iid 0x22ba0121) as a std::set, and the
//    set difference with another set. Both answer the number of entries left in the destination set (the original
//    returns the libc++ tree's size field at +0x10, not a bool).
//  * AlignWindowSetToWindow: for every window in the set, align it through AlignWindowToWindow(id, 0xaaaa, 0, parent).
//  * AlignWindowToWindow(id, childId, alignId, parent): finds both windows recursively from `parent` (RZ::WM()'s main
//    window when null) and, with alignId 0, reads the child's caption. A caption "-\?tmb\?lcr" (three chars, '-' first,
//    then a letter of "tmb" and one of "lcr") puts the child at the top/middle/bottom and left/centre/right of the main
//    window by offsetting the parent window; any other caption is parsed as the hex id of the window to align the
//    child onto.
//  * AlignWindowToWindow(window, alignTo, reference): offsets `window` by reference's origin minus alignTo's origin
//    (both origins come from WindowToScreenCoordinates(0, 0)).
//  * ClearAcceptFocusFlagRecursive: clears flag 0x8000 on the window and (through EnumChildren) every descendant.
//  * GetChildWindowForCursorPos(Recursive): the first visible child that does not have 0x20000...  see below — the
//    hit test is IsPointInWindowScreenCoordinates, then descends; the Recursive flavour repeats until no child is hit.
//  * InterpolateRampToPalette: fills a palette from a (position, colour) stop ramp with 8-bit-per-channel linear
//    interpolation (the ARGB channels are blended pairwise, exactly as the original's masked arithmetic does).
//  * PropertyTokenReplacementFunction: the detokenizer callback for "#D:<hex property id>#" (number) and
//    "#M:<hex property id>#" (money); the token must be exactly 10 characters.
//  * CreateCatalogItemFromExemplar / CreateCatalogItemList / CreateCatalogView: build cSC4CatalogItem records from the
//    resource manager's exemplars and collect them into a cSC4WinCatalogView.
//  * GenericNotificationDialog / GenericConfirmationDialog / DoScriptedModalDialog: build the dialog window from its
//    UI script resource, centre it, install a cSC4GenericDialogWinProc (from WinProcs.h), run it modally, save/restore
//    the window location and restore the keyboard focus.
//
// Everything the original reaches through an engine global goes through INSC4UIHost: SL::spSC4 (the popup-dialogs
// preference), SL::spRM/SC4LoadExemplarString, RZ::LM() (number formatting), RZ::WM(), SC4CreateWindowFromScript,
// SL::spWinLocationSaver and cSC4AutoGamePause. The default host is installed lazily and calls those recovered
// accessors; tests install their own.
#pragma once
#include <cstdint>
#include <map>
#include <set>

#include "gz/WinInterfaces.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZString.h"
#include "rz/cRZString.h"
#include "sc4/WinLocationSaver.h"

namespace sc4 {
class cISCPropertyHolder;
class cISCResExemplar;
class cSCStringDetokenizer;
class cSC4CatalogItem;
class cSC4WinCatalogView;
class cISC4WinCatalogView;
class cISC4CatalogItem;
}  // namespace sc4

namespace nSC4UI {

constexpr std::uint32_t kChildEnumIID = 0x22ba0121;   // cIGZWin (the iid every EnumChildren call passes)
constexpr std::uint32_t kNoFocusFlag = 0x8000;        // cleared by ClearAcceptFocusFlagRecursive
constexpr std::uint32_t kAlignChildId = 0xaaaa;       // the anchor child AlignWindowSetToWindow looks for

// The engine services nSC4UI reaches through globals.
class INSC4UIHost {
public:
    virtual ~INSC4UIHost() = default;
    // SC4GetPopupModalDialogsEnabled @ 0x100038e6b (SL::spSC4 vslot 0xb8); false without an app.
    virtual bool PopupModalDialogsEnabled() = 0;
    // SC4LoadExemplarString @ 0x100121132 -> SC4LoadString @ 0x100121184 (the detokenizer may be null).
    virtual bool LoadExemplarString(cIGZString& out, sc4::cISCPropertyHolder* holder, std::uint32_t property,
                                    sc4::cSCStringDetokenizer* detokenizer) = 0;
    // RZ::LM()->GetLanguageUtility(0) vslot 0x90 (MakeNumberString); a no-op when the service is absent.
    virtual void MakeNumberString(std::int64_t value, cIGZString& out) = 0;
    // RZ::WM().
    virtual cIGZWinMgr* WindowManager() = 0;
    // SC4CreateWindowFromScript @ 0x100039a9f / 0x100039bf7.
    virtual cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) = 0;
    virtual bool CreateWindowFromScriptAs(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                          std::uint32_t iid, void** out) = 0;
    // SL::spWinLocationSaver (RestoreWindowLocation / SaveWindowLocation).
    virtual cSLWinLocationSaver* WindowLocationSaver() = 0;
    // cSC4AutoGamePause(false, false, true, true) around the modal dialog.
    virtual void BeginGamePause() = 0;
    virtual void EndGamePause() = 0;
};
INSC4UIHost* Host();
void SetHost(INSC4UIHost* host);

// --- window sets / alignment / focus -------------------------------------------------------------------------
// Both return the size of the destination set (the original answers the libc++ tree size at +0x10).
std::uint32_t GetWindowChildrenSet(cIGZWin* window, std::set<cIGZWin*>& children);              // @ 0x10030ae2d
std::uint32_t GetWindowChildrenNotInSet(cIGZWin* window, const std::set<cIGZWin*>& excluded,
                                        std::set<cIGZWin*>& out);                              // @ 0x10030ae83
bool AlignWindowSetToWindow(const std::set<cIGZWin*>& windows, cIGZWin* parent);                // @ 0x10030ad12
bool AlignWindowToWindow(std::uint32_t windowId, std::uint32_t childId, std::uint32_t alignId,
                         cIGZWin* parent);                                                     // @ 0x10030aaaa
bool AlignWindowToWindow(cIGZWin* window, cIGZWin* alignTo, cIGZWin* reference);               // @ 0x10030ac8a
void ClearAcceptFocusFlagRecursive(cIGZWin* window);                                            // @ 0x10030b220
cIGZWin* GetChildWindowForCursorPos(cIGZWin* window, int x, int y);                             // @ 0x10030a98e
cIGZWin* GetChildWindowForCursorPosRecursive(cIGZWin* window, int x, int y);                    // @ 0x10030aa41

// --- misc helpers -------------------------------------------------------------------------------------------
// `out[0..count)` filled from the (position, colour) stops in `ramp` (pairs: position, colour).
void InterpolateRampToPalette(std::uint32_t* out, int count, const std::uint32_t* ramp, int rampCount);   // @ 0x10030a7f0
// cSCStringDetokenizer unknown-token callback; `context` is the cISCPropertyHolder.
bool PropertyTokenReplacementFunction(const cIGZString& token, cIGZString& out, void* context);  // @ 0x10030a156

// --- catalogs -----------------------------------------------------------------------------------------------
sc4::cSC4CatalogItem* CreateCatalogItemFromExemplar(sc4::cISCResExemplar* exemplar, cIGZWin* window,
                                                    std::uint32_t itemType,
                                                    sc4::cSCStringDetokenizer* detokenizer);     // @ 0x100309e70
// itemType (param_3) is the exemplar group the items come from (the Mac i386 build at 0x244284 filters on type 0x6534284a and it).
void CreateCatalogItemList(std::multimap<int, sc4::cISC4CatalogItem*>& items, sc4::cISC4WinCatalogView* view,
                           std::uint32_t itemType, std::uint32_t id, std::uint32_t typeFilter,
                           std::uint32_t propertyFilter);                                       // @ 0x10030a37b
sc4::cSC4WinCatalogView* CreateCatalogView(std::uint32_t itemType, std::uint32_t id, std::uint32_t typeFilter,
                                           std::uint32_t propertyFilter);                       // @ 0x10030a6f4

// --- dialogs ------------------------------------------------------------------------------------------------
void GenericNotificationDialog(const cIGZString& title, const cIGZString& text);                // @ 0x10030b2da
bool GenericConfirmationDialog(const cIGZString& title, const cIGZString& text,
                               const cIGZString* okText, const cIGZString* cancelText);         // @ 0x10030b655
std::uint32_t DoScriptedModalDialog(std::uint32_t scriptInstance, std::uint32_t windowId);      // @ 0x10030ba79

}  // namespace nSC4UI
