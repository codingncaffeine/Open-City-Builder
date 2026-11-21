// cSC4ProgressScreen (Mac i386 0x0025d520..0x0025ea48, 0x9c bytes): the picture the city view paints straight onto the screen
// while a city loads, before the view itself can draw. A tiled background, the city's region-view thumbnail (or the default
// snapshot of its terrain type and size) blended in through its mask, and three centred text lines under it: the title the
// view sets ("Loading <city>"), then one of the shuffled loading tips, changed at most once a second as the view reports
// progress. An obliterated city gets a white screen with one red line instead.
//
// Layout of the original: +0xc screen buffer, +0x10 / +0x14 thumbnail and its mask, +0x18 line font (0x0a9c7973), +0x1c title
// font (0x4a9c7970), +0x20 the tips, +0x2c three caption strings, +0x50 text colour, +0x54 / +0x58 two blues (set, not read),
// +0x5c background colour, +0x60 the screen area, +0x70 the text area (top + 12 .. top + 100, the bottom refitted to the fonts),
// +0x80 the background tile, +0x84 the tip timer (milliseconds), +0x98 initialised, +0x99 obliterating.
//
// Seams: IProgressScreenHost holds what the original reaches through SL::spMS2, RZ::WCM(), SL::spGS / spGS2 / spGDriver,
// cSLBuffer, SC4LoadString / cSLString, SL::spCity, SL::spSC4's region and SC4ReadCityRegionViewThumbnails.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/Buffer.h"
#include "gz/Font.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZRefCount.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"

namespace sc4 {

constexpr std::uint32_t kMsgCityInitBegins = 0x26d31ec0;           // the city's init-begins message (the thumbnail is read then)
constexpr std::uint32_t kProgressLineFontId = 0x0a9c7973, kProgressTitleFontId = 0x4a9c7970;
constexpr std::uint32_t kProgressImageType = 0x856ddbac, kProgressImageGroup = 0x6a1eed2c;
constexpr std::uint32_t kProgressBackgroundInstance = 0x4a2805ff;
constexpr std::uint32_t kProgressTipGroup = 0x4a5cb171, kProgressFirstTipInstance = 0x0a6deb35;
constexpr std::uint32_t kProgressTextGroup = 0x6a231eaa, kProgressObliteratingInstance = 0xca81ff90;
constexpr float kProgressTipPeriodMs = 1000.0f;                    // FLOAT_00a706f8
// kDefaultSnapInstances (0x00848d20): {image, mask} per base terrain type (2) x city size (3).
constexpr std::uint32_t kDefaultSnapInstances[6][2] = {
    {0x6a231946, 0x6a231947}, {0xea23195d, 0xea23195e}, {0x0a2312d9, 0x0a2312d8},
    {0x6a6ca89e, 0x6a6ca89f}, {0x6a6ca6df, 0x6a6ca6de}, {0x0a6cab89, 0x0a6cab88},
};

class IProgressScreenHost {
public:
    virtual ~IProgressScreenHost() = default;
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;      // SL::spMS2
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) = 0;
    virtual cIGZFont* Font(std::uint32_t id) = 0;                                          // RZ::WCM() vslot 0x14 (borrowed)
    virtual cIGZBuffer* CreateScreenBuffer() = 0;                                          // SL::spGS vslot 0x20 (one reference)
    virtual cIGZBuffer* LoadImage(std::uint32_t group, std::uint32_t instance) = 0;        // cSLBuffer(0x856ddbac, ...) (one reference)
    virtual bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;   // SC4LoadString / cSLString
    // SL::spCity GetCitySaveFilePath + SC4ReadCityRegionViewThumbnails(path, &image, &mask): one reference each.
    virtual bool ReadCityThumbnails(cIGZBuffer** image, cIGZBuffer** mask) = 0;
    // SL::spSC4 GetRegion -> GetBaseTerrainType (vslot 0x90), and SL::spCity vslot 0x2b8 (the city's size type).
    virtual int BaseTerrainType() = 0;
    virtual int CitySizeType() = 0;
    virtual void BeginScreenDraw() = 0;                                                     // SL::spGS2 vslot 0x60
    virtual void ShowBuffer(cIGZBuffer* buffer, const cRZRect& source, const cRZRect& target) = 0;   // SL::spGS2 vslot 0x68
    // SL::spGDriver ClearColor (vslot 0x60), vslot 0x78(1) and Clear(0x4000) (vslot 0x58): the frame cleared to a colour.
    virtual void ClearScreen(float r, float g, float b, float a) = 0;
    virtual void EndScreenDraw() = 0;                                                       // SL::spGDriver vslot 0x158
};

class cSC4ProgressScreen final : public cIGZMessageTarget2, public cRZRefCount {
public:
    cSC4ProgressScreen(IProgressScreenHost* host, const cRZRect& area, bool obliterating);   // @ 0x0025e62a
    ~cSC4ProgressScreen() override;                                                          // @ 0x0025e84a

    bool QueryInterface(std::uint32_t iid, void** out) override;                             // @ 0x0025d53c
    std::uint32_t AddRef() override { cRZRefCount::AddRef(); return RefCount(); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(cRZRefCount::Release()); }
    bool DoMessage(cIGZMessage2* message) override;                                          // @ 0x0025e01a

    void Init();                                                                             // @ 0x0025e2b4
    void Shutdown();                                                                         // @ 0x0025e1f8
    void SetCaption(std::uint32_t index, const cIGZString& text);                           // @ 0x0025df1c
    void Repaint();                                                                          // @ 0x0025d7a4
    void RepaintForObliterationProgress();                                                   // @ 0x0025d574

    // what the screen shows (no original counterpart)
    bool Initialized() const { return initialized_; }
    const cRZString& Caption(int index) const { return captions_[index]; }
    std::size_t TipsLeft() const { return tips_.size(); }
    bool HasThumbnail() const { return thumbnail_ && thumbnailMask_; }
    cIGZBuffer* ScreenBuffer() const { return screen_; }

private:
    static void Assign(cIGZBuffer*& slot, cIGZBuffer* value);   // cRZAutoRefCount<cIGZBuffer>::operator=
    static void Assign(cIGZFont*& slot, cIGZFont* value);

    IProgressScreenHost* host_;
    cIGZBuffer* screen_ = nullptr;          // +0xc
    cIGZBuffer* thumbnail_ = nullptr;       // +0x10
    cIGZBuffer* thumbnailMask_ = nullptr;   // +0x14
    cIGZFont* lineFont_ = nullptr;          // +0x18
    cIGZFont* titleFont_ = nullptr;         // +0x1c
    std::vector<cRZString> tips_;           // +0x20
    cRZString captions_[3];                 // +0x2c
    std::uint32_t textColor_ = 0;           // +0x50
    std::uint32_t blue1_ = 0, blue2_ = 0;   // +0x54 / +0x58
    std::uint32_t backgroundColor_ = 0;     // +0x5c
    cRZRect area_;                          // +0x60
    cRZRect textArea_;                      // +0x70
    cIGZBuffer* background_ = nullptr;      // +0x80
    cRZTimer tipTimer_{cRZTimer::kMilliseconds};   // +0x84
    bool initialized_ = false;              // +0x98
    bool obliterating_;                     // +0x99
};

}  // namespace sc4
