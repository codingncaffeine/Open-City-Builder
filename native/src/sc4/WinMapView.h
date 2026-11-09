// cSC4WinMapView — the full-screen city map window (60 symbols in the binary, 42 of them functions plus 18
// compiler thunks, 0x1003494a6..0x100352b98). It is the data-view map the player opens from the main screen: a
// cGZWinGen frame whose parent window owns the map's child windows (the three legend panels, the RCI/desirability/
// traffic buttons and the 3D view). The window keeps the current data view (land value, pollution, traffic, ...),
// draws that view's grid into a map buffer (Update / DisplayStandard*Grid), paints the legend of the view
// (DisplayLegend), collects the occupants a view highlights (UpdateHighlights) and routes the buttons the game's
// own UI script places in the parent (DoMessage(cGZMessage&)).
//
// Two view-manager vtables sit in front of the window: at +0 the cISC4ViewManager view (iid 0x67c5bede: SetBaseView /
// PushView / PopView and the Init / Shutdown / DoMessage / IsVisible / ShowWindow / HideWindow overrides) and at +8 the
// cIGZMessageTarget2 view (iid 0x452294aa, the message server's tick / animation receiver). The cGZWinGen base starts at
// +0x10, so its cGZWin lives at +0x10 and its cIGZWinGen view at +0x160. The port derives from cGZWinGen first (so the
// window is `this`) and implements both extra interfaces as secondary bases; QueryInterface answers both ids.
//
// Original layout of the part after the cGZWinGen base (all offsets are object offsets; the base ends at +0x1d8):
//   +0x1d8 bool started                  the Init guard; Init leaves it set when the message server is missing
//   +0x1e0 cIGZMessageServer2* messages  held with a reference
//   +0x1e8 SC4MarkedCellView* markedCells (a 0x78-byte object; the port uses IWinMapViewMarkedCells)
//   +0x1f0 byte useMarkedCells (1), +0x1f1 byte markedCellsRegistered, +0x1f2 byte viewActive
//   +0x1f4 int dataView (0 = none, 0x4c = the traffic route view), +0x1f8 int layerStyle (3 initially)
//   +0x1fc uint32 paletteA[256], +0x5fc uint32 paletteB[256]  (the two colour tables the map layers fill)
//   +0x9fc byte clampGridToCity, +0x9fd byte averageGridByLots, +0xa00 uint32 lotTypeMask
//   +0xa04 uint32 paletteSteps (0x4a0b47ec), +0xa08 int64 u16GridScale,
//   +0xa10 int64 s16GridScale
//   +0xa30 std::set<cHighlightInfo> newHighlights (the per-tick collection, cleared by MergeNewHighlights),
//   +0xa18 std::set<cHighlightInfo> activeHighlights (the merged set the effects live in)
//   +0xa48 int highlightView (the UpdateHighlights switch), +0xa50 cRZString effectName
//   +0xa70 uint32 unknown (only ever cleared), +0xa80 int desirabilityView (99), +0xa84 int trafficView (0x4c),
//   +0xa88 int viewView (0), +0xa8c bool trafficRoutes, +0xa8d uint8 networkTypes[8], +0xa95 uint8 networkTypeCount,
//   +0xa96 bool showTrafficLegend, +0xa98 cSC4SimGrid<sint16>* occupancyGrid
//   +0xaa0 std::vector<uint32> watchedMessages, +0xab8 std::list<uint32> viewStack (+0xac8 its size)
//   +0xad0 uint32 cached data-view key, +0xad8 cIGZUnknown* viewExemplar, +0xae0 uint32 baseView
//   +0xae4 bool shown, +0xae5 bool dirty, +0xae8 uint32 lastUpdateMs, +0xaec bool keepGridWhileUpdating
//
// Quirks kept (the ones the test can reach are asserted in scripts/tests/sc4_winmapview_test.cpp):
//  * Init sets started even when it fails (no message server), so a second Init does nothing (0x100349a4a).
//  * DoMessage(cIGZMessage2*) compares the message type as a *signed* int, so the high ids (0x86ad10ee, 0xa6b79602)
//    take the animation branch and only 0x26d31ec1/0x26d31ec2 take the activate pair (0x100349e0e).
//  * Update() only runs when the elapsed time is at least the interval *and* something is dirty; the traffic route
//    view (0x4c) uses interval 0xfa and sets the flag first (0x100349e5c..0x100349e9c). The traffic branch itself
//    needs SetDataView, which is partial.
//  * The moved message (0xa6b79602) marks the view dirty only when the second word of the two payload rectangles
//    differs (0x100349f0e), and the command filter's veto makes DoMessage(cGZMessage&) answer true without passing
//    the message on to the window (0x10034d81e).
//  * ShowWindow points all 82 controls of its file-static kButtonIDList at this window as their notification target,
//    not just the map buttons (0x100352814..0x100352844).
//  * DisplayLegend tests the label index with `<=` against the label count (0x10034d2aa), i.e. one past the end;
//    the port substitutes instance 0 there instead of reading past the vector.
//  * The button exclusions compare `firstId - selected + i` (0x10034cde2), so a selected id outside the range
//    leaves every button switched off.
//
// PARTIAL (see the comments at their definitions): Update (the map image generation, 0x10034a848), SetDataView (the
// exemplar-driven view configuration, 0x100351858), UpdateHighlights (its nine per-view simulator queries are the
// IWinMapViewHost::CollectHighlights seam, 0x100349f5c), DisplayStandardUint8Grid / DisplayStandardSint8Grid (the
// city-size rebuild, per-lot averaging and bilinear resample, 0x10034e730 / 0x10034f10a) and the three small grid
// converters (0x10034faca / 0x10034fc14 / 0x10034ff26), whose byte grid is handed to the partial Uint8 display.
#pragma once
#include <cstdint>
#include <list>
#include <set>
#include <vector>

#include "gz/Buffer.h"
#include "gz/Win.h"
#include "gz/WinFont.h"
#include "gz/WinGen.h"
#include "rz/cRZString.h"
#include "sc4/Occupant.h"
#include "sc4/cSC4SimGrid.h"

class cIGZFont;
class cIGZMessage2;
class cIGZMessageServer2;
struct cRZRect;

namespace sc4 {

class cISC4GrowthDeveloper;   // sc4/GrowthDeveloper.h

// ---------------------------------------------------------------------------------------------------------------
// Identifiers (literal pools of the recovered functions).
// ---------------------------------------------------------------------------------------------------------------
constexpr std::uint32_t kGZIID_cISC4ViewManager = 0x67c5bede;      // QueryInterface, 0x1003498f8
constexpr std::uint32_t kGZIID_cIGZMessageTarget2_ = 0x452294aa;   // the +8 view, 0x1003498ea
constexpr std::uint32_t kIID_cViewColorSink = 0xca318385;          // child 0x4203's colour-table view, InitWindow

// The parent window's child ids the view pokes (all literals in the recovered functions).
constexpr std::uint32_t kWinMapIdMainFrame = 0x4203;               // the 3D view child (InitWindow)
constexpr std::uint32_t kWinMapIdLegendBackground = 0x8a909e00;    // the legend panel's first background
constexpr std::uint32_t kWinMapIdLegendText = 0x8a909e00;          // first label (same base as the backgrounds)
constexpr std::uint32_t kWinMapIdLegendBackground1 = 0x8a909e10;   // first colour swatch
constexpr std::uint32_t kWinMapIdLegendTitle = 0x8a909d6a;
constexpr std::uint32_t kWinMapIdLargePanel = 0xaa32bcd4;
constexpr std::uint32_t kWinMapIdDesirablePanel = 0xaa32bcd5;
constexpr std::uint32_t kWinMapIdTrafficLegend = 0x8a2871c4;
constexpr std::uint32_t kWinMapIdMap1 = 0x8a2871b1;
constexpr std::uint32_t kWinMapIdMap2 = 0x8a2871b2;
constexpr std::uint32_t kWinMapIdLegendPanel = 0x8a2871c3;
constexpr std::uint32_t kWinMapIdButtonView = 0x8a2871d6;
constexpr std::uint32_t kWinMapIdButtonSmall = 0x8a2871c5;
constexpr std::uint32_t kWinMapIdRefresh = 0x8a2871d5;
constexpr std::uint32_t kWinMapIdViewSmall1 = 0x4a32ca92;
constexpr std::uint32_t kWinMapIdViewSmall2 = 0x0a32cac3;
constexpr std::uint32_t kWinMapIdViewSmall3 = 0xa32cacd4;
constexpr std::uint32_t kWinMapIdViewContext1 = 0xea8b80a6;
constexpr std::uint32_t kWinMapIdViewContext2 = 0xea8b80a7;
constexpr std::uint32_t kWinMapIdHide1 = 0xea2871d9;
constexpr std::uint32_t kWinMapIdHide2 = 0xea2871e9;
constexpr std::uint32_t kWinMapIdRefresh2 = 0xca2871f0;            // ca2871f0 / ca2871f1 both refresh
constexpr std::uint32_t kWinMapIdRefresh3 = 0xca2871f1;
constexpr std::uint32_t kWinMapIdShowTraffic = 0xcb72f247;         // only ShowTrafficWindow touches it
constexpr std::uint32_t kWinMapIdSmallButton1 = 0x8100, kWinMapIdSmallButton2 = 0x8101;

// The child ids of every control in the parent window that forwards its commands to the view (ShowWindow's
// file-static kButtonIDList, 82 entries read at 0x100a1d8b0).
extern const std::uint32_t kWinMapButtonIds[82];

// Messages.
constexpr std::uint32_t kWinMapMsgAnimationTick = 0x86ad10ee;      // cSC4MessageAnimationTick
constexpr std::uint32_t kWinMapMsgViewMoved = 0xa6b79602;          // window move/resize notification
constexpr std::uint32_t kWinMapMsgViewActivate = 0x26d31ec1;       // added by Init, removed by Shutdown
constexpr std::uint32_t kWinMapMsgViewDeactivate = 0x26d31ec2;
constexpr std::uint32_t kWinMapMsgTrafficRoutes = 0x287259f6;      // DoMessage(cGZMessage&) command family
constexpr std::uint32_t kWinMapMsgPollutionAir = 0x2a09b21a;       // view 1
constexpr std::uint32_t kWinMapMsgDesirability = 0x87cc75b3;       // view 2
constexpr std::uint32_t kWinMapMsgCrime = 0x88ea4dff;              // view 4
constexpr std::uint32_t kWinMapMsgPower = 0x6a07c2ee;              // view 7
constexpr std::uint32_t kWinMapMsgPollutionAirChange = 0xfa6dd0bb; // view 8, 0x18
constexpr std::uint32_t kWinMapMsgPollutionWaterChange = 0x5a6dd1aa;
constexpr std::uint32_t kWinMapMsgPollutionGarbageChange = 0xda6dd2cb;
constexpr std::uint32_t kWinMapMsgPollutionRadiationChange = 0xda6dd35d;
constexpr std::uint32_t kWinMapMsgTraffic = 0x69247dc7;            // view 9 / 0x4c
constexpr std::uint32_t kWinMapMsgPlumbing = 0x6946dc34;           // view 10
constexpr std::uint32_t kWinMapMsgLandValue = 0x09b6d58e;          // view 0x1d

// The traffic view (the one the route arrows and the pothole decals belong to) and the legend views the per-view
// buttons store.
constexpr std::uint32_t kWinMapViewTraffic = 0x4c;
constexpr std::uint32_t kWinMapViewNone = 0;
constexpr std::uint32_t kWinMapViewLegendTraffic = 0x4a0b6803;     // the legend that shows the traffic panel

// Effects: the empty name means "no highlight effect" (ActivateHighlight returns at once).
constexpr std::uint32_t kWinMapEffectFontStyle = 0xe9c86b5f;        // RZ::WCM()->SystemFont style for the legend

// ---------------------------------------------------------------------------------------------------------------
// The data the view asks the rest of the game for. Every method documents the original call it replaces; the
// default host answers "nothing", tests install their own.
// ---------------------------------------------------------------------------------------------------------------
struct WinMapHighlight {
    cISC4Occupant* occupant = nullptr;   // borrowed; AddNewHighlight takes the view's own reference
    float scale = 0.0f;                  // the highlight radius the simulator reported
    float x = 0.0f, y = 0.0f, z = 0.0f;  // centre; only used when occupant is null (the crime hot spot)
};
// SC4Point<int>: the marked cell (x, z).
struct WinMapCell {
    std::int32_t x = 0, z = 0;
};

class IWinMapViewEffect : public cIGZUnknown {
public:
    // The identity matrix plus the position the original builds before calling vslot 0x40, then vslot 0x18.
    virtual void Place(const float position[3], float size) = 0;   // cISC4VisualEffect vslot 0x40
    virtual void Activate(bool on) = 0;                            // cISC4VisualEffect vslot 0x18
};

// SC4MarkedCellView (0x10077b62c.., 17 symbols, not recovered): the renderer's list of highlighted cells.
class IWinMapViewMarkedCells {
public:
    virtual ~IWinMapViewMarkedCells() = default;
    virtual void Init() = 0;                                                    // 0x10077b7b8
    virtual void Shutdown() = 0;                                                // 0x10077b924
    virtual void SetShouldIgnoreDepthBuffer(bool on) = 0;                       // 0x10077c2b4
    virtual void SetShouldUseWaterAltitude(bool on) = 0;                        // 0x10077c2be
    virtual void SetShouldUseNetworkAltitude(bool on) = 0;                      // 0x10077c2c8
    virtual void ClearMarkedCells() = 0;                                        // 0x10077c496
    virtual void AddMarkedCells(const std::vector<WinMapCell>& cells) = 0;      // 0x1003537f0
    virtual void SetCellColors(const std::vector<std::uint32_t>& colors) = 0;   // 0x10077c40e
    virtual void SetCellSize(float x, float z) = 0;                             // 0x10077c42e
};

// The colour table of the child window 0x4203 (iid 0xca318385) InitWindow fills.
class IWinMapViewColorSink {
public:
    virtual ~IWinMapViewColorSink() = default;
    virtual void SetBaseColor(std::uint32_t color, std::uint32_t flags) = 0;   // vslot 0x40
    virtual void ResetColors() = 0;                                            // vslot 0x30
    virtual void SetColor(int index, std::uint32_t rgb) = 0;                   // vslot 0x38
    // The rest of cISC4WinMiniMap that SetDataView / Update reach (i386 vslots 0x10, 0x24, 0xc -> window 0xc0 / 0x170).
    virtual void SetOverlay(cIGZBuffer* overlay) { (void)overlay; }           // SetOverlay
    virtual void Refresh() {}                                                  // Update
    virtual void Invalidate() {}                                               // AsIGZWin()->InvalidateSelf
    virtual bool MapSize(int& width, int& height) { (void)width; (void)height; return false; }   // AsIGZWin()->GetArea
};

class IWinMapViewHost {
public:
    virtual ~IWinMapViewHost() = default;
    // cRZTime::GetMillisecondSystemTime (0x1007b0f84).
    virtual std::uint32_t NowMilliseconds() = 0;
    // SL::spMS2 (the +0x1e0 member).
    virtual cIGZMessageServer2* MessageServer2() = 0;
    // SL::ATM() vslots 0x28 / 0x30 (start / stop the animation messages) and 0x40 (the flags a view wants).
    virtual void AddAnimationTarget(cIGZMessageTarget2* target) = 0;
    virtual void RemoveAnimationTarget(cIGZMessageTarget2* target) = 0;
    virtual void SetAnimationNotifications(cIGZMessageTarget2* target, std::uint32_t flags) = 0;
    // UpdateHighlights' simulator switch: the occupants (and the one crime hot spot) a data view highlights. The
    // original queries spResidentialSim 0x178/0x188, spFireProtectionSim 0xb0, spPoliceSim 0xa8, spCrimeSim 0x40,
    // spPollutionSim 0x170, the zone manager's 0x42 occupant scan, spPowerSim 0xc8, spPlumbingSim 0xe0,
    // spTrafficSim 0xe0 and then spOccupantManager 0x180.
    virtual void CollectHighlights(std::uint32_t highlightView, std::vector<WinMapHighlight>& out) = 0;
    // new SC4MarkedCellView (0x10077b62c) + the two SetShould* calls.
    virtual IWinMapViewMarkedCells* CreateMarkedCells() = 0;
    // SL::spRenderer vslot 0x100 (AddMarkedCellView, flags 5 / 1000) and 0x108 (RemoveMarkedCellView).
    virtual bool AddMarkedCells(IWinMapViewMarkedCells* cells) = 0;
    virtual bool RemoveMarkedCells(IWinMapViewMarkedCells* cells) = 0;
    // SL::spEffectsManager vslot 0x30 (create the effect named by effectName_); a counted reference.
    virtual bool CreateEffect(const cRZString& name, IWinMapViewEffect** out) = 0;
    // DisplayLegend's exemplar read: RM 0x18 with {type 0x6534284a, group 0x690f693f, instance view + 0x4a0b6800},
    // then AsISCPropertyHolder (vslot 0x30) and properties 0x4a0b47f2 (colours), 0x4a0b47f3 (label string ids),
    // 0x4a0b47f4 (title string id).
    virtual bool LoadLegendData(std::uint32_t view, std::vector<std::uint32_t>& colors,
                                std::vector<std::uint32_t>& labelStrings, std::uint32_t& titleString) = 0;
    // cSLString(group 0x6a231eaa, instance, true): the view 0 title is instance 0x2a5e854f.
    virtual void LoadString(std::uint32_t group, std::uint32_t instance, cRZString& out) = 0;
    // RZ::WCM()->SystemFont(0xe9c86b5f) as cIGZFont.
    virtual cIGZFont* LegendFont() = 0;
    // The child 0x4203 queried for iid 0xca318385 (InitWindow / SetDataView).
    virtual IWinMapViewColorSink* ViewColorSink(cIGZWin* parent) = 0;
    // AddNewHighlight's position source: spLotManager vslot 0x70 (the occupant's lot) and lot vslot 0x78
    // (GetPosition); false means the occupant's own GetPosition (cISC4Occupant vslot 0x40) is used.
    virtual bool OccupantLotPosition(cISC4Occupant* occupant, float position[3]) = 0;
    // The traffic simulator's "is this cell a road" query (spTrafficSim vslot 0x80) and the city's cell counts
    // (spCity 0x2e8 / 0x2f0).
    virtual bool IsTransportationCell(int x, int z) = 0;
    virtual int CitySizeXInCells() = 0;
    virtual int CitySizeZInCells() = 0;
    // SL::spGF vslot 0x50 with the pothole decal resource key (0x96a006b0 / ...): a counted reference.
    virtual cIGZBuffer* CreatePotholeDecal() = 0;
    // The renderer's map rotation (SL::spRenderer vslot 0x80 -> object +0x114), negated for the rotated blit.
    virtual int MapRotation() = 0;
    // SL::spCommandServer vslot 0x98 (refresh command 0x6a935cf1).
    virtual void SendCommand(std::uint32_t command, std::uint32_t arg1, std::uint32_t arg2) = 0;
    // nsSC4TutorialSystem::FilterUICommandMessage (0x10006008e): false vetoes the command and DoMessage answers
    // true without forwarding it.
    virtual bool FilterUICommand(const cGZMessage& message) = 0;

    // ---- SetDataView / Update (i386 0x0027fc98 / 0x0027db2e). Defaults answer "nothing".
    // The data view's exemplar {0x6534284a, 0x690f693f, key} (RM vslot 0xc, iid 0xa52160f5), read into `out`.
    virtual bool LoadDataViewExemplar(std::uint32_t key, struct WinMapViewExemplar& out) { (void)key; (void)out; return false; }
    // nSC4UI::InterpolateRampToPalette: `count` entries from the (position, colour) stops.
    virtual void InterpolateRamp(std::uint32_t* out, int count, const std::uint32_t* stops, int stopCount) {
        (void)out; (void)count; (void)stops; (void)stopCount;
    }
    // GZCOM()->CreateInstance(0xc470d325, 0x20732180): a counted, uninitialised buffer.
    virtual cIGZBuffer* CreateMapBuffer() { return nullptr; }
    // The simulator grid a data view draws (Update's switch): the borrowed grid of `source` (WinMapGridSource), with its
    // tract widths. `argument` is the pollution kind (0 air, 1 water, 2 garbage, 3 radiation).
    virtual bool SimulatorGrid(int source, int argument, struct WinMapGrid& out) { (void)source; (void)argument; (void)out; return false; }
    // BlockMapByLot's walk: spLotManager vslot 0x3c (every lot), lot 0x38 GetBoundingRect and 0x7c GetZoneType.
    virtual void CityLots(std::vector<struct WinMapLot>& out) { (void)out; }
};

// One data view's exemplar (the properties SetDataView reads; `has*` = the property exists).
struct WinMapViewExemplar {
    std::vector<std::uint32_t> occupantGroups;   // 0x4a0b47e0
    bool hasOccupantGroups = false;
    bool hasInvertGroups = false, invertGroups = false;          // 0x4a0b47e1
    bool hasHidePlain = false, hidePlain = false;                // 0x4a0b47e2
    bool hasUnderground = false; std::uint32_t underground = 0;  // 0x4a0b47e3
    std::vector<std::uint32_t> ramp;             // 0x4a0b47e4 (stops)
    bool hasRamp = false;
    bool hasDataView = false; std::uint32_t dataView = 0;        // 0x4a0b47e5
    bool hasZoneDisplay = false; bool zoneDisplay = false;       // 0x4a0b47e6
    bool hasClampToCity = false; bool clampToCity = false;       // 0x4a0b47e7
    bool hasLotTypeMask = false; std::uint32_t lotTypeMask = 0;  // 0x4a0b47e8
    bool hasHighlightView = false; std::uint32_t highlightView = 0;   // 0x4a0b47e9
    bool hasEffectName = false; cRZString effectName;            // 0x4a0b47ea
    bool hasCustomView = false; std::uint32_t customView = 0;    // 0x4a0b47eb (GZCOM class id)
    bool hasSteps = false; std::uint32_t steps = 0;              // 0x4a0b47ec
    bool hasLayerStyle = false; std::uint32_t layerStyle = 0;    // 0x4a0b47ed
    bool hasCircleColor = false; std::uint32_t circleColor = 0;  // 0x4a0b47ee
    bool hasLotShadeColor = false; std::uint32_t lotShadeColor = 0;   // 0x4a0b47ef
    bool hasDevelopmentView = false; std::uint8_t developmentView = 0;   // 0x4a0b47f0
    bool hasNetworkList = false; std::uint32_t networkList = 0;  // 0x4a0b47f1
    bool hasShowZones = false; bool showZones = false;           // 0x4a0b47f5
    std::vector<std::uint32_t> networkTypes;     // 0x6b7c5cdd
    bool hasNetworkTypes = false;
};

// Update's grid sources (the simulator and its vslot in the original).
enum WinMapGridSource : int {
    kGridAuraMayorRating = 1,      // spAuraSim 0x30, sint8
    kGridLandValue,                // spLandValueSim 0x20, uint8
    kGridPolice,                   // spPoliceSim 0x14, uint8
    kGridCrime,                    // spCrimeSim 0x14, uint8
    kGridFireProtection,           // spFireProtectionSim 0x14, uint8
    kGridFlammability,             // spFlammabilitySim 0x14, uint8
    kGridPower,                    // spPowerSim 0x90, uint8
    kGridPollution,                // spPollutionSim 0x14(kind), sint16
    kGridTraffic,                  // spTrafficSim 0x28, uint8
    kGridPlumbing,                 // spPlumbingSim 0x1c, uint8
    kGridAura,                     // spAuraSim 0x38, sint16
    kGridResidentialAge,           // spResidentialSim 0xa4, uint8
};
struct WinMapGrid {
    const std::vector<std::vector<std::uint8_t>>* u8 = nullptr;
    const std::vector<std::vector<std::int8_t>>* s8 = nullptr;
    const std::vector<std::vector<std::int16_t>>* s16 = nullptr;
    float cellSizeX = 0.0f, cellSizeZ = 0.0f;   // the grid's vslots 0x38 / 0x3c (tract widths)
};
struct WinMapLot {
    std::int32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;   // GetBoundingRect, inclusive cells
    std::uint8_t zoneType = 0;                      // GetZoneType
};
IWinMapViewHost* WinMapViewHost();
void SetWinMapViewHost(IWinMapViewHost* host);

// ---------------------------------------------------------------------------------------------------------------
// cISC4ViewManager: the interface view (iid 0x67c5bede) at +0 of the original. Only the three view-stack methods
// are declared here; Init / Shutdown / the DoMessage pair / IsVisible / ShowWindow / HideWindow are the window
// class's own overrides in the original's primary vtable.
// ---------------------------------------------------------------------------------------------------------------
class cISC4ViewManager : public cIGZUnknown {
public:
    virtual void SetBaseView(std::uint32_t view) = 0;      // slot 3 0x18, 0x100351846
    virtual void PushView(std::uint32_t view) = 0;         // slot 4 0x20, 0x100351790
    virtual void PopView(std::uint32_t view) = 0;          // slot 5 0x28, 0x1003517de
};

// The occupant entry of the two highlight sets (the set is ordered by the occupant pointer).
struct WinMapHighlightInfo {
    cISC4Occupant* occupant = nullptr;              // +0x00 (one reference while in the set)
    IWinMapViewEffect* effect = nullptr;            // +0x08 (one reference)
    float scale = 0.0f;                             // +0x10
    float x = 0.0f, y = 0.0f, z = 0.0f;             // +0x14 / +0x18 / +0x1c

    WinMapHighlightInfo() = default;
    WinMapHighlightInfo(cISC4Occupant* occ, IWinMapViewEffect* fx, float s, const float position[3]);
    WinMapHighlightInfo(const WinMapHighlightInfo& other);
    WinMapHighlightInfo& operator=(const WinMapHighlightInfo& other);
    ~WinMapHighlightInfo();
};
struct WinMapHighlightInfoLess {
    bool operator()(const WinMapHighlightInfo& a, const WinMapHighlightInfo& b) const { return a.occupant < b.occupant; }
};

class cSC4WinMapView final : public gz::cGZWinGen,
                            public cIGZMessageTarget2,
                            public cISC4ViewManager {
public:
    explicit cSC4WinMapView(cIGZWinMgr* mgr);          // 0x1003494a6 (the 0x10034971c redirect)
    ~cSC4WinMapView() override;                        // 0x100349726 (0x100349874 / 0x10034988c)

    // ---- cIGZUnknown (the original forwards everything else to cGZWinGen)
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x1003498c8
    std::uint32_t AddRef() override { return gz::cGZWinGen::AddRef(); }        // 0x10034992a
    std::uint32_t Release() override { return gz::cGZWinGen::Release(); }      // 0x100349962

    // ---- cGZWin / cIGZWinGen
    bool DoMessage(cGZMessage& message) override;                  // 0x10034d7ee
    bool DoMessage(cIGZMessage2* message) override;                // 0x100349df6
    bool Init() override;                                          // 0x1003499a4
    bool Shutdown() override;                                      // 0x100349b3a
    bool IsVisible() const override;                               // 0x10035261c
    void ShowWindow() override;                                    // 0x1003527b0
    void HideWindow() override;                                    // 0x100352a6e

    // ---- cISC4ViewManager
    void SetBaseView(std::uint32_t view) override;                 // 0x100351846
    void PushView(std::uint32_t view) override;                    // 0x100351790
    void PopView(std::uint32_t view) override;                     // 0x1003517de

    // ---- internals (the original's non-virtual members)
    bool UpdateHighlights();                                       // 0x100349f5c
    void Update();                                                 // 0x10034a848 (PARTIAL)
    void OnViewModeButtonSelected(std::uint32_t id, int view);     // 0x10034cd56
    void DisplayLegend(std::uint32_t view);                        // 0x10034ce8c
    void ShowLargeWindow();                                        // 0x10034d6aa
    void OnDesirabilityModeButtonSelected(std::uint32_t id, int view);   // 0x10034d75e
    void OnTrafficModeButtonSelected(std::uint32_t id, int view);  // 0x10034d7a6
    void ShowDesirableWindow();                                    // 0x10034e070
    void ShowTrafficWindow();                                      // 0x10034e12c
    void ShowSmallWindow();                                        // 0x10034e1f2
    void ResetDataView();                                          // 0x10034e316
    void HookUpdateMessages();                                     // 0x10034e426
    void DisplayStandardUint8Grid(const std::vector<std::vector<std::uint8_t>>& grid, cIGZBuffer* buffer,
                                  float cellSizeX, float cellSizeZ);   // 0x10034e730 (PARTIAL)
    void DisplayStandardSint8Grid(const std::vector<std::vector<std::int8_t>>& grid, cIGZBuffer* buffer,
                                  float cellSizeX, float cellSizeZ);   // 0x10034f10a (PARTIAL)
    void DisplayStandardFloat32GridAsUint8(const std::vector<std::vector<float>>& grid, cIGZBuffer* buffer,
                                           float cellSizeX, float cellSizeZ);   // 0x10034faca
    void DisplayStandardSint16GridScaled(const std::vector<std::vector<std::int16_t>>& grid, cIGZBuffer* buffer,
                                         float cellSizeX, float cellSizeZ);   // 0x10034fc14
    void DrawTransportationPotholeDecals(cIGZBuffer* buffer);      // 0x10034fd86
    void DisplayStandardUint16GridScaled(const std::vector<std::vector<std::uint16_t>>& grid, cIGZBuffer* buffer,
                                         float cellSizeX, float cellSizeZ);   // 0x10034ff26
    void UpdateOccupancyMap(const cISC4GrowthDeveloper* developer);   // 0x1003503fa
    void AddNewHighlight(cISC4Occupant* occupant, float scale);    // 0x1003507c4
    bool MergeNewHighlights();                                     // 0x100350a1c
    void ActivateHighlight(WinMapHighlightInfo& info);             // 0x100350fa4
    bool SetDataView(std::uint32_t view);                          // 0x100351858 (PARTIAL)
    void InitWindow();                                             // 0x10035266e

    // ---- inspection helpers (not in the original)
    std::uint32_t dataView() const { return dataView_; }
    std::uint32_t baseView() const { return baseView_; }
    const std::list<std::uint32_t>& viewStack() const { return viewStack_; }
    bool isDirty() const { return dirty_; }
    bool viewActive() const { return viewActive_; }
    cIGZMessageServer2* messageServer() const { return messages_; }
    IWinMapViewMarkedCells* markedCells() const { return markedCells_; }
    bool markedCellsRegistered() const { return markedCellsRegistered_; }
    const std::vector<std::uint32_t>& watchedMessages() const { return watchedMessages_; }
    const std::set<WinMapHighlightInfo, WinMapHighlightInfoLess>& activeHighlights() const { return activeHighlights_; }
    const std::set<WinMapHighlightInfo, WinMapHighlightInfoLess>& newHighlights() const { return newHighlights_; }
    std::uint32_t highlightView() const { return highlightView_; }
    std::uint32_t desirabilityView() const { return desirabilityView_; }
    std::uint32_t trafficView() const { return trafficView_; }
    std::uint32_t viewView() const { return viewView_; }
    bool showTrafficLegend() const { return showTrafficLegend_; }
    const cRZString& effectName() const { return effectName_; }
    const std::uint32_t* paletteA() const { return paletteA_; }
    cSC4SimGrid<std::int16_t>* occupancyGrid() const { return occupancyGrid_; }
    const std::uint32_t* paletteB() const { return paletteB_; }

private:
    cIGZMessageTarget2* messageTarget() { return static_cast<cIGZMessageTarget2*>(this); }
    cIGZWin* parentWin() const;
    void setWindowVisible(std::uint32_t id, bool visible);
    bool windowVisible(std::uint32_t id) const;
    cIGZWin* childWindow(std::uint32_t id) const;
    void removeWatchedNotifications();
    void fillPaletteFromGridEdges();

    bool started_ = false;                     // +0x1d8
    cIGZMessageServer2* messages_ = nullptr;   // +0x1e0
    IWinMapViewMarkedCells* markedCells_ = nullptr;   // +0x1e8
    bool useMarkedCells_ = true;               // +0x1f0
    bool markedCellsRegistered_ = false;       // +0x1f1
    bool viewActive_ = false;                  // +0x1f2
    std::uint32_t dataView_ = 0;               // +0x1f4
    std::uint32_t layerStyle_ = 3;             // +0x1f8
    std::uint32_t paletteA_[256] = {};         // +0x1fc
    std::uint32_t paletteB_[256] = {};         // +0x5fc
    bool clampGridToCity_ = false;             // +0x9fc
    bool averageGridByLots_ = false;           // +0x9fd
    std::uint32_t lotTypeMask_ = 1;            // +0xa00
    std::uint32_t paletteSteps_ = 0x100;       // +0xa04 (i386 0x96c: 0x4a0b47ec; the scales below are 0xffff00 / 0x7fff00 over it)
    std::int64_t u16GridScale_ = 0x100;        // +0xa08
    std::int64_t s16GridScale_ = 0x100;        // +0xa10
    std::set<WinMapHighlightInfo, WinMapHighlightInfoLess> newHighlights_;      // +0xa18
    std::set<WinMapHighlightInfo, WinMapHighlightInfoLess> activeHighlights_;   // +0xa30
    std::uint32_t highlightView_ = 0;          // +0xa48
    cRZString effectName_;                     // +0xa50
    std::uint32_t unknownA70_ = 0;             // +0xa70 (only cleared by the constructor)
    std::uint32_t lotShadeColor_ = 0;          // i386 0x9c0 (0x4a0b47ef): Update shades lots in this colour
    std::uint32_t circleColor_ = 0;            // i386 0x9c4 (0x4a0b47ee): Update circles the highlights in this colour
    std::uint32_t desirabilityView_ = 99;      // +0xa80
    std::uint32_t trafficView_ = kWinMapViewTraffic;   // +0xa84
    std::uint32_t viewView_ = 0;               // +0xa88
    bool trafficRoutes_ = false;               // +0xa8c
    std::uint8_t networkTypes_[8] = {};        // +0xa8d
    std::uint8_t networkTypeCount_ = 0;        // +0xa95
    bool showTrafficLegend_ = false;           // +0xa96
    cSC4SimGrid<std::int16_t>* occupancyGrid_ = nullptr;   // +0xa98
    std::vector<std::uint32_t> watchedMessages_;   // +0xaa0
    std::list<std::uint32_t> viewStack_;       // +0xab8 (+0xac8 size)
    std::uint32_t cachedViewKey_ = 0;          // +0xad0
    cIGZUnknown* viewExemplar_ = nullptr;      // +0xad8
    std::uint32_t baseView_ = 0;               // +0xae0
    bool shown_ = false;                       // +0xae4
    bool dirty_ = false;                       // +0xae5
    std::uint32_t lastUpdateMs_ = 0;           // +0xae8
    bool keepGridWhileUpdating_ = false;       // +0xaec
};

// ---------------------------------------------------------------------------------------------------------------
// Free functions of the same module (recovered here because the window needs them).
// ---------------------------------------------------------------------------------------------------------------
// 0x10034cde2: switch the first `count` buttons starting at `firstId` on for `selectedId`, off for the rest.
void SC4DoButtonExclusion(cIGZWin* parent, std::uint32_t firstId, std::uint32_t count, std::uint32_t selectedId);
// 0x10034e278: enable/disable the ten buttons 0x8003..0x800c of the parent (flag 2 = enabled).
void EnableChildWinSequence(cIGZWin* parent, std::uint32_t enabled, std::uint32_t unused, bool unusedBool);
// 0x10035008a: alpha-blend `source` onto `destination` rotated by the low two bits of `rotation` (the renderer's
// map rotation, negated by the callers), centred on (x, y).
void SC4MapViewRotatedAlphaBlt(cIGZBuffer* destination, cIGZBuffer* source, int x, int y, int rotation);

}  // namespace sc4
