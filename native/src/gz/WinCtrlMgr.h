// cGZWinCtrlMgr — the window control manager (0x1001f2560..0x1001f5c5a, service 0xc2c2eb0f, iid 0x22c2eb1f for the
// control view at +0x28). It owns the shared look of the stock controls (system fonts and bitmaps by id, default
// colours, default texts, font-name mapping, the double click time, sound settings) and builds the controls:
// Create* make a configured control, CreateCtrl asks the registered creators first and falls back to
// CreateDefaultCtrl, which knows the stock class ids.
//
// Layout (0x130 bytes): cRZSystemService (0x28), +0x28 cIGZWinCtrlMgr view, +0x30 initialised, +0x38 graphics
// system, +0x40 sound manager, +0x48 colour map, +0x70 font map, +0x98 bitmap map, +0xc0 text map, +0xe8 font
// mapping, +0x110 creator list, +0x128 double click time (0x2ee ms).
//
// Seams: the graphic system (WinGraphics()), the sound manager (IWinSoundSettings), the COM class factory
// (IWinComSource, tried before the built-in classes), and the window manager the built-in controls are created on.
#pragma once
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "gz/Win.h"
#include "gz/WinBMP.h"
#include "gz/WinBtn.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinLineInput.h"
#include "gz/WinListBox.h"
#include "gz/WinOptGrp.h"
#include "gz/WinScrollbar.h"
#include "gz/WinSlider.h"
#include "gz/WinSpinner.h"
#include "gz/WinText.h"
#include "gz/cIGZString.h"
#include "rz/cRZSystemService.h"

namespace gz {

constexpr std::uint32_t kGZSID_cGZWinCtrlMgr = 0xc2c2eb0f;
constexpr std::uint32_t kGZIID_cIGZWinCtrlMgr = 0x22c2eb1f;
constexpr std::uint32_t kGZIID_cIGZCtrlCreator = 0xa2ed067f;

// RZ::SndMgr (service 0x1af85bfb): the master volume and the ids of the stock interface sounds.
class IWinSoundSettings {
public:
    virtual ~IWinSoundSettings() = default;
    virtual void SetVolume(int volume) = 0;                     // vslot 0x20
    virtual int GetVolume() = 0;                                // vslot 0x28
    virtual std::uint32_t SoundId(int which) = 0;               // vslot 0x30
    virtual void SetSoundId(int which, std::uint32_t id) = 0;   // vslot 0x38
};
IWinSoundSettings* WinSoundSettings();
void SetWinSoundSettings(IWinSoundSettings* sound);

// GZCOM()->GetCOMObject(classId, iid, &out) (vslot 0x18): a game-registered replacement for a stock control.
class IWinComSource {
public:
    virtual ~IWinComSource() = default;
    virtual bool GetCOMObject(std::uint32_t classId, std::uint32_t iid, void** out) = 0;
};
IWinComSource* WinComSource();
void SetWinComSource(IWinComSource* source);

// A registered control creator (iid 0xa2ed067f): slot 3 is not used by the manager.
class cIGZCtrlCreator : public cIGZUnknown {
public:
    virtual void Slot3() {}
    virtual bool Initialize() = 0;                                                         // [4]
    virtual void Shutdown() = 0;                                                           // [5]
    virtual bool CreateCtrl(std::uint32_t classId, std::uint32_t iid, void** out) = 0;    // [6]
};

class cIGZWinCtrlMgr : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                               // [3]
    virtual bool Shutdown() = 0;                                                           // [4]
    virtual IWinFont* SystemFont(int id) = 0;                                              // [5]
    virtual void SetSystemFont(int id, IWinFont* font) = 0;                                // [6]
    virtual cIGZBuffer* SystemBMP(int id) = 0;                                             // [7]
    virtual void SetSystemBMP(int id, cIGZBuffer* image) = 0;                              // [8]
    virtual cIGZWinBtn* CreateButton(std::uint32_t style, std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [9]
    virtual cIGZWinBtn* CreateButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [10]
    virtual cIGZWinBtn* CreateToggleButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [11]
    virtual cIGZWinBtn* CreateRadioCheck(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [12]
    virtual cIGZWinText* CreateLabel(std::uint32_t id, const cIGZString& caption) = 0;     // [13]
    virtual cIGZWinOptGrp* CreateOptGrp(std::uint32_t id, const cIGZString& caption, const cRZRect* area) = 0;   // [14]
    virtual cIGZWinBMP* CreateWinBMP(std::uint32_t id, cIGZBuffer* image, bool fitToImage) = 0;   // [15]
    virtual cIGZWinSlider* CreateSlider(std::uint32_t id, int direction, int low, int high, int value) = 0;   // [16]
    virtual cIGZWinScrollbar* CreateScrollbar(std::uint32_t id, int direction, int low, int high, int value) = 0;   // [17]
    virtual cIGZWinSpinner* CreateSpinner(std::uint32_t id, int type, int low, int high, int step, int value) = 0;   // [18]
    virtual cIGZWinLineInput* CreateLineInput(std::uint32_t id, bool editable) = 0;         // [19]
    virtual cIGZWinBtn* CreateFixedButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [20]
    virtual cIGZWinBtn* CreateFixedToggleButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [21]
    virtual cIGZWinBtn* CreateCheckBox(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [22]
    virtual cIGZWinBtn* CreateRadioButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) = 0;   // [23]
    virtual bool RegisterCtrlCreator(cIGZCtrlCreator* creator) = 0;                        // [24]
    virtual bool RemoveCtrlCreator(cIGZCtrlCreator* creator) = 0;                          // [25]
    virtual bool CreateCtrl(std::uint32_t classId, std::uint32_t iid, void** out) = 0;     // [26]
    virtual void SetSystemSndVol(int volume) = 0;                                          // [27]
    virtual int GetSystemSndVol() = 0;                                                     // [28]
    virtual std::uint32_t SystemSndID(int which) = 0;                                      // [29]
    virtual void SetSystemSndID(int which, std::uint32_t id) = 0;                          // [30]
    virtual std::uint32_t DefaultColor(int id) = 0;                                        // [31]
    virtual void SetDefaultColor(int id, std::uint32_t native) = 0;                        // [32]
    virtual void SetDefaultColor(int id, std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;   // [33]
    virtual void DefaultColor(int id, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) = 0;   // [34]
    virtual cIGZString* DefaultText(int id) = 0;                                           // [35]
    virtual void SetDefaultText(int id, const cIGZString* text) = 0;                       // [36]
    virtual cIGZString* GetFontMapping(int id) = 0;                                        // [37]
    virtual int GetFontMappingID(const cIGZString* name) = 0;                              // [38]
    virtual void SetFontMapping(int id, const cIGZString* name) = 0;                       // [39]
    virtual std::uint32_t GetDoubleClickTimeMilliseconds() const = 0;                      // [40]
    virtual void SetDoubleClickTimeMilliseconds(std::uint32_t ms) = 0;                     // [41]
    virtual bool CreateDefaultCtrl(std::uint32_t classId, std::uint32_t iid, void** out) = 0;   // [42]
};

class cGZWinCtrlMgr : public cRZSystemService, public cIGZWinCtrlMgr, public IWinCtrlFactory {
public:
    explicit cGZWinCtrlMgr(cIGZWinMgr* windowManager = nullptr) : cRZSystemService(kGZSID_cGZWinCtrlMgr, 0), winMgr_(windowManager) {}
    ~cGZWinCtrlMgr() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cRZSystemService::AddRef(); }
    std::uint32_t Release() override { return cRZSystemService::Release(); }
    bool Init() override;
    bool Shutdown() override;

    // The window manager the built-in controls are created on (the binary's window constructors find it themselves).
    void SetWindowManager(cIGZWinMgr* manager) { winMgr_ = manager; }
    // Classes this file does not build itself (not recovered yet): a constructor per class id, tried by CreateDefaultCtrl.
    using Constructor = std::function<cIGZUnknown*()>;   // returns the primary object with one reference
    void SetFallbackConstructor(std::uint32_t classId, Constructor constructor) { fallbacks_[classId] = std::move(constructor); }

    IWinFont* SystemFont(int id) override;
    void SetSystemFont(int id, IWinFont* font) override;
    cIGZBuffer* SystemBMP(int id) override;
    void SetSystemBMP(int id, cIGZBuffer* image) override;
    cIGZWinBtn* CreateButton(std::uint32_t style, std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override;
    cIGZWinBtn* CreateButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override { return CreateButton(2, id, caption, image, area, key); }
    cIGZWinBtn* CreateToggleButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override { return CreateButton(3, id, caption, image, area, key); }
    cIGZWinBtn* CreateRadioCheck(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override { return CreateButton(4, id, caption, image, area, key); }
    cIGZWinText* CreateLabel(std::uint32_t id, const cIGZString& caption) override;
    cIGZWinOptGrp* CreateOptGrp(std::uint32_t id, const cIGZString& caption, const cRZRect* area) override;
    cIGZWinBMP* CreateWinBMP(std::uint32_t id, cIGZBuffer* image, bool fitToImage) override;
    cIGZWinSlider* CreateSlider(std::uint32_t id, int direction, int low, int high, int value) override;
    cIGZWinScrollbar* CreateScrollbar(std::uint32_t id, int direction, int low, int high, int value) override;
    cIGZWinSpinner* CreateSpinner(std::uint32_t id, int type, int low, int high, int step, int value) override;
    cIGZWinLineInput* CreateLineInput(std::uint32_t id, bool editable) override;
    cIGZWinBtn* CreateFixedButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override { return CreateButton(2, id, caption, image, area, key); }
    cIGZWinBtn* CreateFixedToggleButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override { return CreateButton(3, id, caption, image, area, key); }
    cIGZWinBtn* CreateCheckBox(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override { return CreateButton(4, id, caption, image, area, key); }
    cIGZWinBtn* CreateRadioButton(std::uint32_t id, const cIGZString& caption, cIGZBuffer* image, const cRZRect* area, std::uint32_t key) override { return CreateButton(4, id, caption, image, area, key); }
    bool RegisterCtrlCreator(cIGZCtrlCreator* creator) override;
    bool RemoveCtrlCreator(cIGZCtrlCreator* creator) override;
    bool CreateCtrl(std::uint32_t classId, std::uint32_t iid, void** out) override;
    void SetSystemSndVol(int volume) override { if (IWinSoundSettings* s = WinSoundSettings()) s->SetVolume(volume); }
    int GetSystemSndVol() override { IWinSoundSettings* s = WinSoundSettings(); return s ? s->GetVolume() : 0; }
    std::uint32_t SystemSndID(int which) override { IWinSoundSettings* s = WinSoundSettings(); return s ? s->SoundId(which) : 0; }
    void SetSystemSndID(int which, std::uint32_t id) override { if (IWinSoundSettings* s = WinSoundSettings()) s->SetSoundId(which, id); }
    std::uint32_t DefaultColor(int id) override;
    void SetDefaultColor(int id, std::uint32_t native) override;
    void SetDefaultColor(int id, std::uint8_t r, std::uint8_t g, std::uint8_t b) override;
    void DefaultColor(int id, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override;
    cIGZString* DefaultText(int id) override;
    void SetDefaultText(int id, const cIGZString* text) override { SetString(texts_, id, text); }
    cIGZString* GetFontMapping(int id) override;
    int GetFontMappingID(const cIGZString* name) override;
    void SetFontMapping(int id, const cIGZString* name) override { SetString(fontMapping_, id, name); }
    // The style id whose font mapping equals `name` without regard to case (0 when none): how the font-style script
    // vocabulary, which holds the same names, resolves them (cGZScriptVocab compares without case).
    std::size_t FontMappingCount() const { return fontMapping_.size(); }
    int FontMappingIDIgnoringCase(const cIGZString& name) const {
        for (const auto& entry : fontMapping_)
            if (entry.second && name.IsEqualToStringPointer(entry.second, false)) return entry.first;
        return 0;
    }
    std::uint32_t GetDoubleClickTimeMilliseconds() const override { return doubleClickMs_; }
    void SetDoubleClickTimeMilliseconds(std::uint32_t ms) override { doubleClickMs_ = ms; }
    std::uint32_t DoubleClickMilliseconds() override { return doubleClickMs_; }   // IWinCtrlFactory
    bool CreateDefaultCtrl(std::uint32_t classId, std::uint32_t iid, void** out) override;

private:
    using StringMap = std::unordered_map<int, cIGZString*>;
    static void SetString(StringMap& map, int id, const cIGZString* text);
    static void ClearStrings(StringMap& map);
    // Builds a stock control: the COM source first, else the built-in class. Returns the interface with one reference.
    template <class T> void* Make(std::uint32_t classId, std::uint32_t iid);
    bool QueryAndRelease(void* object, std::uint32_t iid, void** out);
    bool CreateButtonCtrl(std::uint32_t classId, std::uint32_t iid, void** out);   // 0x68963e48 / 0xa8963e4b
    bool CreateOptGrpCtrl(std::uint32_t iid, void** out);                          // 0xc8963e3c
    bool CreateListBoxCtrl(std::uint32_t iid, void** out);                         // 0x28963e36

    bool initialized_ = false;                                 // +0x30
    std::unordered_map<int, std::uint32_t> colors_;            // +0x48 (r << 16 | g << 8 | b)
    std::unordered_map<int, IWinFont*> fonts_;                 // +0x70
    std::unordered_map<int, cIGZBuffer*> bitmaps_;             // +0x98
    StringMap texts_;                                          // +0xc0
    StringMap fontMapping_;                                    // +0xe8
    std::list<cIGZCtrlCreator*> creators_;                     // +0x110
    std::uint32_t doubleClickMs_ = 0x2ee;                      // +0x128
    cIGZWinMgr* winMgr_;
    std::map<std::uint32_t, Constructor> fallbacks_;
};

}  // namespace gz
