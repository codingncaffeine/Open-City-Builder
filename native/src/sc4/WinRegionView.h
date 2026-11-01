// cSC4WinRegionView (32 functions + 6 compiler thunks, 0x10075afc6..0x10075e6ac): the region map window — the scrolling
// tile canvas that draws the region bitmap, the city thumbnails with their masks, the city-name labels, the selected-city
// grid and the signpost windows that pop up over a city.
//
// The object has two vtable groups: +0 the cGZWin / cIGZWin view (Init, Shutdown, SetArea, GZPaint, is_point_transparent,
// QueryInterface / AddRef / Release) and +0x150 the cSC4TiledScrollingCanvasSource view (RenderTile, called by the tile
// canvas at vslot 0x18 for every dirty tile). QueryInterface forwards to cGZWin::QueryInterface (0x10075b18c), so the
// source view is *not* query-able — quirk kept (0x10075b18c..0x10075b1a0).
//
// Layout (the original's offsets; the second vtable pointer occupies +0x150..+0x157):
//   +0x158 region index                  the screen writes it (the constructor leaves it alone, 0x10023a8xx)
//   +0x160 cIGZBuffer* backdrop          the region bitmap, tiled over the canvas (Init)
//   +0x168 city*       selected city     ClearSelection / SelectCity / GetSelectedCity
//   +0x170 int scrollX, +0x174 int scrollY
//   +0x178 uint signpost type, +0x17c uint signpost sub type   (SetSignpostType)
//   +0x180 uint label colour, +0x184 uint second label colour  (the screen writes both, 0x10023a8xx)
//   +0x188 std::vector<cSC4RegionViewCity*> cities
//   +0x1a0 cSC4TiledScrollingCanvas* canvas
//   +0x1a8 bool scroll indicator (posted by cSC4WinRegionScroll; read by the original's paint code)
//   +0x1a9..+0x1ae the six option bytes (signpost mode, forced signposts, grid, labels, always labels, tutorial arrow)
//   +0x1b0 int map mode
//   +0x1b8 std::vector<cRZAutoRefCount<cIGZBuffer>> the two signpost icon bitmaps (airport, seaport)
//   object size 0x1d0
//
// Quirks kept (asserted in scripts/tests/sc4_winregionview_test.cpp):
// * The constructor writes the six option bytes as two stores: +0x1a9..+0x1ac = 0x00010001 and +0x1ad..+0x1ae = 0x0100,
//   i.e. signpost mode on, forced signposts off, grid on, labels off, always labels off, tutorial arrow on
//   (0x10075aff2..0x10075b00a).
// * InvalidateRegion invalidates in *canvas* (world) coordinates: it subtracts the scroll offsets and adds them back, so
//   the rectangle is floor(mapX), floor(mapY) - image height regardless of the scroll position (0x10075b6c5..0x10075b72a).
//   GetRegionPlayfieldRect is the same world rectangle; GetRegionScreenRect is the scrolled window rectangle.
// * GetRegionScreenRect / GetRegionPlayfieldRect subtract the image *height* from y, not its top, so a city's rectangle
//   sits above its map position (0x10075dae2, 0x10075ddc0).
// * SetScrollPos places a signpost at (rect.right + rect.left)/2 - width/2, (rect.bottom + rect.top)/2 - height: the
//   vertical centring subtracts the whole height instead of half of it (0x10075d9dc).
// * The city hit test is a lower_bound over the packed span list and accepts the point when the found index is odd, i.e.
//   the previous marker was a span start (0x10075db8a).
// * The constructor never initialises +0x158; the screen sets it in LoadGUI. The port zeroes it so a hand-built view is
//   deterministic (the constructor's store list, 0x10075afc6).
//
// RenderTile is PARTIAL: only the backdrop blit / black fill is recovered (0x10075bb66..0x10075bbf0); the per-city
// compositing, the antialiased mask spans, the label windows and the selected-city grid of the remaining 5.4 KB are not.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <vector>

#include "gz/Buffer.h"
#include "gz/StreamOps.h"
#include "gz/Win.h"
#include "gz/cGZPersistResourceKey.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "sc4/SCProperty.h"

namespace sc4 {

class cSC4WinRegionView;

// ---------------------------------------------------------------------------------------------------------------
// Identifiers (literal pools of the recovered functions).
// ---------------------------------------------------------------------------------------------------------------
constexpr std::uint32_t kRegionViewWindowId = 0x2ba6bb97;          // SetID in Init (vslot 0x200), 0x10075b211
constexpr std::uint32_t kRegionViewBackdropGroup = 0x6a1eed2c;     // Init's region bitmap
constexpr std::uint32_t kRegionViewBackdropInstance = 0x4a2805ff;
constexpr std::uint32_t kRegionViewIconGroup = 0x46a006b0;         // both icon bitmaps
constexpr std::uint32_t kRegionViewIconInstance[2] = {0xebabb1b0, 0xebabb1b1};   // gMapIconTypes, 0x100aebc60
constexpr const char* kRegionViewIconName[2] = {"region_airport", "region_seaport"};
constexpr std::uint32_t kRegionViewIconEffectType[2] = {0, 1};     // (&DAT_100aebc64)[i * 4]
constexpr std::uint32_t kRegionViewAuraBarId = 0x4a553000;         // the signpost's value bar
constexpr std::uint32_t kRegionViewBarImageGroup = 0x46a006b0;     // cSLBuffer(0, 0x46a006b0, 0x14416327, 9, 0)
constexpr std::uint32_t kRegionViewBarImageInstance = 0x14416327;
// The signpost scripts are UI scripts: type 0, group 0x96a006b0 (the first port had the group in the type field, so no
// signpost window was ever built).
constexpr std::uint32_t kRegionViewSignpostKeyGroup = 0x96a006b0;
constexpr std::uint32_t kRegionViewSignpostKeyInstance = 0xca539343;
constexpr std::uint32_t kRegionViewSignpostWindowId = 0x0a551c53;
constexpr std::uint32_t kRegionViewSignpostTuningType = 0x6534284a;
constexpr std::uint32_t kRegionViewSignpostTuningGroup = 0xe7e2c2db;
constexpr std::uint32_t kRegionViewSignpostTuningInstance = 0xc8e8f6c5;
constexpr std::uint32_t kRegionViewSignpostTuningIid = 0xa52160f5;
constexpr std::uint32_t kRegionViewSignpostBudgetProperty = 0x6a9c7718;   // default 10000
constexpr std::uint32_t kRegionViewTutorialItemHidesName = 0x10;          // TutorialItem +0x10 byte

// The signpost child window ids (GetChildWindowFromIDRecursive, 0x10075dee0).
constexpr std::uint32_t kSignpostCityNameId = 0x4a552000;
constexpr std::uint32_t kSignpostMayorNameId = 0x4a552001;
constexpr std::uint32_t kSignpostBudgetId = 0x4a552002;
constexpr std::uint32_t kSignpostPopulationId = 0x4a552003;
constexpr std::uint32_t kSignpostCommercialId = 0x4a552004;
constexpr std::uint32_t kSignpostIndustrialId = 0x4a552005;
constexpr std::uint32_t kSignpostBudgetNegativeId = 0x4a552006;
constexpr std::uint32_t kSignpostEmptyBudgetId = 0x4a560003;
constexpr std::uint32_t kSignpostDifficultyLowId = 0x6c06f4a0;
constexpr std::uint32_t kSignpostDifficultyMediumId = 0xac06f4c4;
constexpr std::uint32_t kSignpostDifficultyHighId = 0xcc06f4cf;

// ---------------------------------------------------------------------------------------------------------------
// cISC4RegionalCity (not recovered) as this view uses it; the slot numbers are the original vtable offsets.
// (WinRegionScreen.h has its own narrower IRegionalCity for the screen; this interface is the view's wider view of the
// same original class, hence the distinct name.)
// ---------------------------------------------------------------------------------------------------------------
struct RegionalCityPointOfInterest {
    float x = 0, y = 0, z = 0;                      // 12-byte cISC4RegionalCity::PointOfInterest
};
static_assert(sizeof(RegionalCityPointOfInterest) == 12);

class IRegionCityView : public cIGZUnknown {
public:
    virtual void GetPosition(int& x, int& z) const = 0;                            // 0x28
    virtual int GetPopulation() const = 0;                                        // 0x48
    virtual int GetCommercialJobs() const = 0;                                    // 0x50
    virtual int GetIndustrialJobs() const = 0;                                    // 0x58
    virtual int GetMayorRating() const = 0;                                       // 0x68 (sign-extended byte)
    virtual int GetDifficultyLevel() const = 0;                                   // 0x70
    virtual void GetCityName(cIGZString& out) const = 0;                          // 0x108
    virtual void GetMayorName(cIGZString& out) const = 0;                         // 0x118
    virtual bool GetEstablished() const = 0;                                      // 0x158
    virtual double GetBudget() const = 0;                                         // 0x190
    virtual int GetTutorialGUID() const = 0;                                      // 0x208
    virtual bool IsTutorial() const = 0;                                          // 0x218
    virtual void GetPointsOfInterest(int type, std::vector<RegionalCityPointOfInterest>& out) = 0;   // 0x240
};

// cISC4VisualEffect (src/sc4/Disaster.h) as the city's signpost icon effects use it.
class IRegionViewEffect : public cIGZUnknown {
public:
    virtual void Start(int type) = 0;                                             // 0x18
    virtual void Stop(int type) = 0;                                              // 0x20
    virtual void SetEffectTransform(const cS3DTransform& transform) = 0;          // 0x38
};

// ---------------------------------------------------------------------------------------------------------------
// cSC4RegionViewCity (not recovered: the export lists only its destructor and copy constructor). The 0xf0-byte record
// the screen keeps per city; the view only reads it. The four span vectors are SC4CreateSpanList output:
//   +0x58 spans with threshold 0 and alpha bytes, rebuilt by RenderTile (transparent-run work list)
//   +0x70 threshold 0, no alpha bytes  -> the hit-test list read by GetCityAtPoint
//   +0x88 threshold 0x10, alpha bytes  -> the grid list read by RenderTile
//   +0xa0 threshold 8, alpha bytes     -> label / second-image list (not read by the recovered view code)
// A span stream is a flat std::vector<uint32_t>: a start marker (x | y << 16), the end marker (xEnd | y << 16) and,
// when the list carries alpha, one 0..255 word per pixel of the run.
// ---------------------------------------------------------------------------------------------------------------
struct RegionViewCityIcon {
    std::int32_t x = 0, y = 0;        // +0x00 / +0x04 screen position
    cIGZBuffer* image = nullptr;      // +0x08 the icon bitmap (borrowed from the view)
};
static_assert(sizeof(RegionViewCityIcon) == 16, "cSC4RegionViewCity::cIcon is 16 bytes");

constexpr std::size_t kRegionViewCitySize = 0xf0;

struct cSC4RegionViewCity {
    std::int32_t left = 0;                          // +0x00
    std::int32_t top = 0;                           // +0x04
    std::int32_t x = 0;                             // +0x08 city grid x (region cell)
    std::int32_t z = 0;                             // +0x0c city grid z
    float mapX = 0;                                 // +0x10 position on the region map
    float mapY = 0;                                 // +0x14
    std::uint8_t size = 0;                          // +0x18 size code, (1 << size) cells wide
    std::uint8_t pad19_[7] = {};                    // +0x19..+0x1f
    cIGZBuffer* image = nullptr;                    // +0x20 the city thumbnail (borrowed)
    cIGZBuffer* imageAlt = nullptr;                 // +0x28 second thumbnail / alpha source
    cIGZBuffer* imageSmall = nullptr;               // +0x30 the small thumbnail (map mode 1)
    cIGZBuffer* imageSmallAlt = nullptr;            // +0x38
    cIGZBuffer* mask = nullptr;                     // +0x40 the mask / shadow (RenderTile locks it and rewrites pixels)
    cIGZWin* signpost = nullptr;                    // +0x48 the signpost window (owned by the city, released here)
    std::uint8_t dirty = 0;                         // +0x50
    std::uint8_t pad51_[7] = {};                    // +0x51..+0x57
    std::vector<std::uint32_t> spans;               // +0x58 RenderTile's transparent-run work list
    std::vector<std::uint32_t> hitSpans;            // +0x70 packed hit-test spans
    std::vector<std::uint32_t> gridSpans;           // +0x88 packed grid spans
    std::vector<std::uint32_t> labelSpans;          // +0xa0 packed label/alpha spans
    std::int32_t centerX = 0;                       // +0xb8 label anchor
    std::int32_t centerY = 0;                       // +0xbc
    std::list<cRZAutoRefCount<IRegionViewEffect>> effects;   // +0xc0 the city's signpost effects
    std::vector<RegionViewCityIcon> icons;          // +0xd8 airport / seaport icons
};
static_assert(sizeof(cSC4RegionViewCity) == kRegionViewCitySize, "cSC4RegionViewCity is 0xf0 bytes");

// ---------------------------------------------------------------------------------------------------------------
// The tile canvas. cSC4TiledScrollingCanvas (0x10075a5a2..0x10075afc2, not recovered) is a grid of 0x100x0x100 tiles,
// 16 bytes each at +0x18 ({cIGZBuffer* image, u8 dirty}), cols / rows at +0x48 / +0x4c and the source at +0x58.
// ---------------------------------------------------------------------------------------------------------------
class ITiledScrollingCanvasSource : public cIGZUnknown {
public:
    virtual void RenderTile(cIGZBuffer* buffer, int x, int y) = 0;                // vslot 0x18 (canvas::Render 0x10075acbc)
};

class ITiledScrollingCanvas : public cIGZUnknown {
public:
    virtual bool Init(int width, int height, int tileWidth, int tileHeight,
                      ITiledScrollingCanvasSource* source) = 0;                   // @ 0x10075a6be
    virtual bool Shutdown() = 0;                                                  // @ 0x10075a874
    virtual void ScrollTo(int x, int y) = 0;                                      // @ 0x10075a8fa
    virtual void InvalidateRect(const cRZRect& rect) = 0;                         // @ 0x10075ab9e
    virtual void InvalidateAllTiles() = 0;    // the +8 dirty-byte loop over +0x18..+0x20, inlined at 0x10075bb1a
};

// ---------------------------------------------------------------------------------------------------------------
// Everything the view reaches outside itself. All of it mirrors one original call (slot / address in the comment).
// ---------------------------------------------------------------------------------------------------------------
class IRegionViewHost {
public:
    virtual ~IRegionViewHost() = default;

    // --- renderer / canvas
    virtual ITiledScrollingCanvas* CreateTiledScrollingCanvas() = 0;    // new cSC4TiledScrollingCanvas (0x60 bytes)
    virtual void AddRenderLayer(ITiledScrollingCanvas* canvas) = 0;     // SL::spRenderer vslot 0x100(canvas, 0, 1000)
    virtual void RemoveRenderLayer(ITiledScrollingCanvas* canvas) = 0;  // SL::spRenderer vslot 0x108(canvas)
    virtual void InvalidateRenderer() = 0;                              // SL::spRenderer vslot 0xa0 (no arguments)

    // --- cSC4TutorialRegistry (SL::spSC4 vslot 0x178; not recovered)
    virtual int NextActiveTutorialCity() = 0;      // GetNextActive(region) -> *item, -1 when the answer was null
    virtual IRegionCityView* TutorialCity(int item) = 0;            // GetRegionalCity(item), borrowed
    virtual bool TutorialItemHidesMayorName(int guid) = 0;        // GetItem(guid) != null && item[0x10] != 0

    // --- region city list (SL::spSC4 vslot 0x110 -> vslot 0x58 -> vslot 0x58; not recovered)
    virtual IRegionCityView* FindRegionalCity(int regionIndex, int x, int z) = 0;   // borrowed

    // --- window scripts (SC4CreateWindowFromScript @ 0x100039a9f; needs the UI script service)
    virtual cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) = 0;

    // --- SL::spEffectsManager vslot 0x30(name, &effect) (not recovered): one counted reference
    virtual bool CreateRegionEffect(const char* name, IRegionViewEffect** out) = 0;

    // --- the signpost tuning holder (SL::spRM vslot 0x18: type 0x6534284a, group 0xe7e2c2db, instance 0xc8e8f6c5, iid
    //     0xa52160f5). Borrowed; cSCPropertyHelp::GetPropertyValue reads 0x6a9c7718.
    virtual cISCPropertyHolder* SignpostTuning() = 0;

    // --- nSC4UI::GetChildWindowForCursorPos @ 0x10030a98e (cIGZWin::EnumChildren over iid 0x22ba0121).
    virtual cIGZWin* ChildWindowForCursorPos(cIGZWin* window, int x, int y) = 0;
};
IRegionViewHost* RegionViewHost();
void SetRegionViewHost(IRegionViewHost* host);

// ---------------------------------------------------------------------------------------------------------------
// The view.
// ---------------------------------------------------------------------------------------------------------------
class cSC4WinRegionView final : public cGZWin, public ITiledScrollingCanvasSource {
public:
    explicit cSC4WinRegionView(cIGZWinMgr* mgr);      // @ 0x10075afc6
    ~cSC4WinRegionView() override;                    // @ 0x10075b066 (+0x10075b154 / 0x10075b15e aliases)

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x10075b18c (+0x10075b196 thunk)
    std::uint32_t AddRef() override;                               // @ 0x10075b1a8 (+0x10075b1b2 thunk)
    std::uint32_t Release() override;                              // @ 0x10075b1c4 (+0x10075b1ce thunk)

    bool Init() override;                                          // @ 0x10075b1e0
    bool Shutdown() override;                                      // @ 0x10075b4f0
    void SetArea(int l, int t, int r, int b) override;             // @ 0x10075b652
    bool GZPaint() override;                                       // @ 0x10075bb5e
    bool is_point_transparent(int x, int y) const override;        // @ 0x10075e6ac

    void RenderTile(cIGZBuffer* buffer, int x, int y) override;    // @ 0x10075bb66 (+0x10075d57a thunk) PARTIAL

    void ClearCities();                                            // @ 0x10075b5c6
    void InvalidateRegion(cSC4RegionViewCity& city);               // @ 0x10075b6b0
    void UpdateSignpost(cSC4RegionViewCity& city);                 // @ 0x10075b7a6
    void InvalidateAllRegions();                                   // @ 0x10075bb02
    void filter_tutorial_mayor_name(IRegionCityView* city, cIGZString* name) const;   // @ 0x10075d4fc
    void AddCity(cSC4RegionViewCity* city);                        // @ 0x10075d58c
    void UpdateCity(cSC4RegionViewCity* city);                     // @ 0x10075d5e6
    void SetScrollPos(int x, int y);                               // @ 0x10075d9dc
    void GetRegionScreenRect(const cSC4RegionViewCity& city, cRZRect& out) const;   // @ 0x10075dae2
    cSC4RegionViewCity* GetCityAtPoint(int x, int y);              // @ 0x10075db8a
    void ClearSelection();                                         // @ 0x10075dce8
    void SetSignpostMode(bool mode, bool forced);                  // @ 0x10075dd0a
    void EnableLabels(bool labels, bool always);                   // @ 0x10075dd1e
    void EnableGrid(bool on);                                      // @ 0x10075dd36
    void EnableTutorialArrow(bool on);                             // @ 0x10075dd48
    void SetMapMode(int mode);                                     // @ 0x10075dd56
    void SelectCity(int x, int y);                                 // @ 0x10075dd66
    cSC4RegionViewCity* GetSelectedCity() const;                   // @ 0x10075ddb2
    void GetRegionPlayfieldRect(const cSC4RegionViewCity& city, cRZRect& out) const;   // @ 0x10075ddc0
    void SetSignpostType(std::uint32_t type, std::uint32_t sub);   // @ 0x10075de92
    // The original takes cRZAutoRefCount<cISC4RegionalCity>*; the port takes the borrowed city pointer.
    void InitSignpost(cSC4RegionViewCity* city, IRegionCityView* regionalCity);   // @ 0x10075dee0

    // --- test / hook-up access
    cIGZBuffer* Backdrop() const { return backdrop_; }
    ITiledScrollingCanvas* Canvas() const { return canvas_; }
    int RegionIndex() const { return regionIndex_; }
    void SetRegionIndex(int index) { regionIndex_ = index; }
    void SetLabelColours(std::uint32_t first, std::uint32_t second) { labelColour1_ = first; labelColour2_ = second; }
    std::uint32_t LabelColour1() const { return labelColour1_; }
    std::uint32_t LabelColour2() const { return labelColour2_; }
    void SetMapModeForTest(int mode) { mapMode_ = mode; }
    void SetScrollIndicator(bool on) { scrollIndicator_ = on; }
    int ScrollX() const { return scrollX_; }
    int ScrollY() const { return scrollY_; }
    bool SignpostMode() const { return signpostMode_; }
    bool ForcedSignposts() const { return forcedSignposts_; }
    bool Grid() const { return grid_; }
    bool Labels() const { return labels_; }
    bool AlwaysLabels() const { return alwaysLabels_; }
    bool TutorialArrow() const { return tutorialArrow_; }
    int MapMode() const { return mapMode_; }
    std::vector<cSC4RegionViewCity*>& Cities() { return cities_; }
    const std::vector<cSC4RegionViewCity*>& Cities() const { return cities_; }
    std::vector<cRZAutoRefCount<cIGZBuffer>>& IconBuffers() { return iconBuffers_; }
    void SetBackdrop(cIGZBuffer* buffer);
    void SetCanvas(ITiledScrollingCanvas* canvas);
    // Puts a signpost pointer on a city without touching its reference count (the screen allocates it).
    static void SetCitySignpost(cSC4RegionViewCity& city, cIGZWin* signpost) { city.signpost = signpost; }

private:
    std::uint32_t AddRefImpl();       // both vtables forward here
    std::uint32_t ReleaseImpl();
    void ClearCityEffectList(cSC4RegionViewCity& city);
    void UpdateSignpostChildText(cIGZWin* signpost, std::uint32_t id, const cIGZString& text);
    void UpdateSignpostChildFlag(cIGZWin* signpost, std::uint32_t id, int flag, bool on);
    void PositionSignpost(cSC4RegionViewCity& city);

    int regionIndex_ = 0;                       // +0x158 (see the header comment)
    cIGZBuffer* backdrop_ = nullptr;            // +0x160
    cSC4RegionViewCity* selected_ = nullptr;    // +0x168
    int scrollX_ = 0;                           // +0x170
    int scrollY_ = 0;                           // +0x174
    std::uint32_t signpostType_ = 0;            // +0x178
    std::uint32_t signpostSubType_ = 0;         // +0x17c
    std::uint32_t labelColour1_ = 0;            // +0x180
    std::uint32_t labelColour2_ = 0;            // +0x184
    std::vector<cSC4RegionViewCity*> cities_;   // +0x188
    ITiledScrollingCanvas* canvas_ = nullptr;   // +0x1a0
    bool scrollIndicator_ = false;              // +0x1a8
    bool signpostMode_ = false;                 // +0x1a9
    bool forcedSignposts_ = false;              // +0x1aa
    bool grid_ = false;                         // +0x1ab
    bool labels_ = false;                       // +0x1ac
    bool alwaysLabels_ = false;                 // +0x1ad
    bool tutorialArrow_ = false;                // +0x1ae
    std::uint8_t pad1af_ = 0;                   // +0x1af
    int mapMode_ = 0;                           // +0x1b0
    std::vector<cRZAutoRefCount<cIGZBuffer>> iconBuffers_;   // +0x1b8

    // RenderTile's per-city work (the original keeps it in the city record: +0x2c the work buffer, +0x34 its "prepared"
    // flag, +0x38 the runs): the picture with the mask's alpha, the grid tint and the grown semi-transparent edge, and
    // its runs. Rebuilt when the picture, the mask, the grid switch, the grid colour or the map mode change.
    struct PreparedCity {
        const cIGZBuffer* image = nullptr;
        const cIGZBuffer* mask = nullptr;
        bool grid = false;
        std::uint32_t gridColour = 0;
        int mapMode = 0;
        int width = 0, height = 0;
        std::vector<std::uint32_t> pixels;   // 0xAARRGGBB, colours premultiplied as the original treats them
        struct Run { int y, x0, x1; bool opaque; };
        std::vector<Run> runs;
    };
    std::map<const cSC4RegionViewCity*, PreparedCity> prepared_;
    const PreparedCity& Prepare(const cSC4RegionViewCity& city);
};

}  // namespace sc4
