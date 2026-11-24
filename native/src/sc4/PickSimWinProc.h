// cSC4PickSimWinProc (i386 0x00281d1c..0x00283db8, SC4Advisors): the MySim panel's "add a sim" dialog (script 0x96a006b0 /
// 0x0a243d80, window 0x6a243d9e). It lists the sim templates of the MySim pool 21 to a page (head images 0x12340000..14, head
// buttons mHeadButton), six page buttons per group (0x12345000..05, the current page's twin 0x12345100..05), sorting by name,
// sign or sex (control held: descending), and the picked sim's portrait (0x12340015), name (text 0x8a243f1f), sex caption
// (0xea243f57) and sign (combo 0xaa243f5e). "Move in" (0xca243e0c, enabled while the name has a visible character) hands the
// sim to the MySim agent simulator, which starts the move-in tool. Init runs the dialog modally with the game paused.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "gz/WinProc.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/MySimPool.h"
#include "sc4/MySimWinProc.h"

class cIGZWin;
class cIGZWinMgr;
class cIGZMessageServer2;
struct cGZPersistResourceKey;
namespace gz { class cIGZWinGen; }

namespace sc4 {

// What the dialog asks the game for.
class IPickSimHost {
public:
    virtual ~IPickSimHost() = default;
    virtual cIGZWinMgr* WindowManager() = 0;                                                 // RZ::WM()
    // SC4CreateWindowFromScript(key, null parent, id, iid 0x5386d516, out): the dialog's cIGZWinGen, one reference
    virtual gz::cIGZWinGen* CreateDialog(const cGZPersistResourceKey& key, std::uint32_t id) = 0;
    virtual cIGZMessageServer2* MessageServer() = 0;                                         // SL::spMS2
    virtual void MySimDirectory(cIGZString& out) = 0;                                        // SL::spSC4 vslot 0xec
    virtual void RestoreWindowLocation(cIGZWin* window) = 0;                                 // SL::spWinLocationSaver 0x24 (window, 1)
    virtual void SaveWindowLocation(cIGZWin* window) = 0;                                    // 0x14
    // SL::spCity vslot 0x11c (the MySim agent simulator) -> vslot 0x18 MoveIn
    virtual void MoveIn(cRZString& name, bool male, std::uint8_t zodiac, cRZString& bio, std::uint32_t bitmapId,
                        cRZString& bitmapFile, std::uint8_t slot) = 0;
};
IPickSimHost* PickSimHost();
void SetPickSimHost(IPickSimHost* host);

class cSC4PickSimWinProc final : public IPickSimWinProc, public gz::cIGZWinProc, public cIGZMessageTarget2 {
public:
    explicit cSC4PickSimWinProc(std::uint32_t slot);   // 0x002829d8: the MySim slot the sim moves into
    virtual ~cSC4PickSimWinProc();                     // 0x00282922

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x00281d1c
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override;

    bool Init() override;                                            // 0x00283db8 (runs the modal loop)
    bool Shutdown() override;                                        // 0x00282b5e
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // 0x002838c8
    bool DoMessage(cIGZMessage2* message) override;                  // 0x002828ec

    static constexpr std::uint32_t kTickMessage = 0xcc327b60;
    static const std::uint32_t kHeadButtons[21];                    // mHeadButton (i386 0x00849280)

    // test views
    std::uint32_t Count() const { return count_; }
    std::uint32_t Top() const { return top_; }
    std::uint32_t Picked() const { return picked_; }
    const std::vector<std::uint32_t>& Order() const { return order_; }
    const std::vector<tMySimTemplate>& Templates() const { return templates_; }
    void SetTemplatesForTest(const std::vector<tMySimTemplate>& templates);
    void SortByName(bool ascending);                                 // 0x00282ed2
    void SortBySign(bool ascending);                                 // 0x00282ff0
    void SortByGender(bool femaleFirst);                             // 0x002830ae

private:
    void EndDialog();                                                // 0x00281d7e
    bool ValidateText();                                             // 0x00281dfc
    void SetScrollButtons();                                         // 0x00281f1c
    void TickSelf();                                                 // 0x00282868
    void SetupPickSims(std::uint32_t top);                           // 0x00282c3c
    void MoveIn();                                                   // 0x0028313e
    void PickHead(std::uint32_t head);                               // 0x002833fa
    cIGZWin* Window() const;                                         // the dialog (+0x10 AsIGZWin)
    cIGZWin* Find(std::uint32_t id) const;                           // GetParentWin -> GetChildWindowFromIDRecursive
    void ShowImage(std::uint32_t bmpId, const tMySimTemplate& sim);  // the portrait of a template into a cIGZWinBMP

    std::uint32_t refs_ = 0;
    gz::cIGZWinGen* dialog_ = nullptr;   // +0x10
    std::vector<std::uint32_t> order_;   // +0x14 template index per row
    std::uint32_t picked_ = 0;           // +0x18
    bool male_ = true;                   // +0x1c
    std::uint32_t bitmapId_ = 0;         // +0x20
    bool modal_ = false;                 // +0x24
    cRZString bitmapFile_;               // +0x28
    std::uint32_t top_ = 0;              // +0x34 first row shown
    std::uint32_t count_ = 0;            // +0x38
    std::uint32_t pageGroup_ = 0;        // +0x3c first page of the six page buttons
    std::uint32_t page_ = 0;             // +0x40
    std::unique_ptr<cSC4MySimPool> pool_;   // +0x44
    std::vector<tMySimTemplate> templates_; // +0x48
    std::uint32_t slot_ = 0;             // +0x54
    cIGZMessage2* tick_ = nullptr;       // +0x58
};

}  // namespace sc4
