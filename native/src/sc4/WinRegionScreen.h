// cSC4WinRegionScreen (43 functions + 8 compiler thunks/aliases, 0x100235b78..0x10023c54c): the region view screen — the
// window that shows the whole region, its cities, the city-name labels and the three info panels, and that launches a
// city. This is the window the game creates right after the splash screen; the city screen replaces it.
//
// Vtable groups (the original object has three vtable pointers): +0 the cGZWin/cIGZWin view, +0x150 the
// cIGZWinMessageFilter view (iid 0xc6ae7085, DoMessage(cIGZWin*, cGZMessage&)), +0x158 the cIGZMessageTarget2 view
// (iid 0x452294aa, DoMessage(cIGZMessage2*)). QueryInterface answers 1 / 0x22ba0121 with the cGZWin view and
// 0xc6ae7085 with the filter view; the message-target view is *not* query-able and 0xe98b2f57 is rejected (quirk kept
// from 0x100239f72).
//
// Children: +0x160 cSC4WinRegionView (the map with the city labels and signposts), +0x168 cSC4WinRegionClouds (cloud
// shadows), +0x170 a plain backdrop window, 0x178/0x180/0x188 the three info panels, 0x190/0x198/0x1a0 the three panel
// buttons, +0x288 cSC4WinRegionScroll (the right-button / edge-scroll window).
//
// Layout: +0x290 / +0x294 the camera target in region-map units, +0x298..+0x2a7 the scroll bounds (minX, minY, maxX,
// maxY), +0x2b0 / +0x2b4 the per-city half size in pixels (default 0x96 = 150), +0x2b8 signposts active, +0x2b9 / +0x2ba two
// tunable bytes copied to cSC4Region::sBaseTerrainTypeHeight and DAT_100b248e8, +0x2bc the current region index, +0x2c0
// signpost mode (1 = road signs), +0x2c8 / +0x2cc two label colours, +0x2d0 the paint timer, +0x2e8 the double-click
// timer, +0x300 a countdown that turns the ignore-cursor flag off, +0x304 / +0x308 the smooth-scroll time and distance,
// +0x30c / +0x310 the smoothed target, +0x314 smoothing, +0x315 panels visible. The 12 snapshot buffers at +0x1e8 are
// two sets (large +0x1e8, small +0x218) of one image + one mask per city size code 0..2.
//
// Quirks kept (asserted in scripts/tests/sc4_winregionscreen_test.cpp):
// * QueryInterface refuses iid 0xe98b2f57 which the base cGZWin accepts (0x100239f72).
// * GZPaint's y scroll position is negated: (int)(12582912.0f - y) - h/2 - 12582912 = -round(y) - h/2
//   (0x1002375b6..0x1002375e1), while x uses (int)(x + 12582912.0f) - w/2 - 12582912 (0x100237583).
// * ComputeBaseLimits truncates the vertical origin to an int before centring it (0x10023715a).
// * DoMessage(cGZMessage&) handles the scroll start message by setting view +0x1a8 and calling view vslot 0x2e0, and
//   leaves the +0x1a8 byte alone for the stop message (0x10023a07d / 0x10023a001).
// * The region city list's GetCity slot (0x58) answers a pointer to the list's cRZAutoRefCount slot, not a city
//   (every caller dereferences it once: 0x10023792d, 0x10023c580, 0x10023b58c).
// Init posts 0x2a3ad653 with Data1 = 1 (the region-music request; mayor mode posts Data1 = 0): see
// cSC4AudioEventHandler::HandleRadioMessages and kRegionScreenMsgRegionMusic below.
// The port keeps the host services as pointers and the notification helper behind a seam where the original has the
// 0x18-byte object inline at +0x1b8, so sizeof(cSC4WinRegionScreen) differs; every offset in the comments is the
// original's.
// Compile without FMA contraction: the results follow the original single-precision operation order.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gz/CommandServer.h"
#include "gz/Win.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "rz/cRZTimer.h"
#include "s3d/Camera.h"
#include "sc4/CameraControl.h"
#include "sc4/Region.h"
#include "sc4/RegionalTerrainMap.h"
#include "sc4/SCProperty.h"
#include "sc4/WinRegionClouds.h"
#include "sc4/WinRegionScroll.h"

namespace sc4 {

class cSC4WinRegionScreen;

// ---------------------------------------------------------------------------------------------------------------
// Identifiers (all from the binary's literal pools)
// ---------------------------------------------------------------------------------------------------------------
constexpr std::uint32_t kRegionScreenWindowId = 0xea659793;      // SetID in Init (vslot 0x200)
constexpr std::uint32_t kRegionScreenRootChildId = 0x09ebe9ee;   // the script-built root child
constexpr std::uint32_t kRegionScreenPopulationChildId = 0xc9e41918;   // the population text window
constexpr std::uint32_t kRegionScreenPanelIds[3] = {0x09ebee45, 0xea54df28, 0x09ebee60};
constexpr std::uint32_t kRegionScreenPanelButtonIds[3] = {0x09ebf2bd, 0x09ebf2c8, 0x09ebf2c3};
constexpr std::uint32_t kRegionScreenMapModeButtonIds[3] = {0xaba290e1, 0xcba290ec, 0xaba290f6};
constexpr std::uint32_t kRegionScreenGridCheckBoxId = 0xea5a96e6;
constexpr std::uint32_t kRegionScreenSignpostCheckBoxId = 0xca5cfee2;
constexpr std::uint32_t kRegionScreenSnapshotGroup = 0x856ddbac;
constexpr std::uint32_t kRegionScreenSnapshotType = 0x6a1eed2c;
constexpr std::uint32_t kRegionScreenSnapshotInstances[6][2] = {
    {0x6a231946, 0x6a231947}, {0xea23195d, 0xea23195e}, {0x0a2312d9, 0x0a2312d8},
    {0x6a6ca89e, 0x6a6ca89f}, {0x6a6ca6df, 0x6a6ca6de}, {0x0a6cab89, 0x0a6cab88}};
// How many cells a city of each size code covers (1, 4, 16, read at 0x100a1b814).
constexpr int kRegionScreenCityCellCounts[3] = {1, 4, 16};

// Messages (the 17 types the notification helper registers, 0x100a1b6a0).
constexpr std::uint32_t kRegionScreenMsgTypes[17] = {
    0xc53d10aa, 0x231bbf91, 0x2b96b3ea, 0x6a935e3c, 0x6a9757c2, 0x6a935cd8, 0x2a948275, 0x2a94826e, 0x6a935ce0,
    0x6a935cdd, 0x2a948272, 0x2a94826b, 0x6a935ce2, 0x6aa9fe51, 0x0bb3c277, 0x0bb2747d, 0x6a935cf1};
constexpr std::uint32_t kRegionScreenMsgLeavingApp = 0xabb5bb44;   // posted before Init
constexpr std::uint32_t kRegionScreenMsgEnteredApp = 0xcbb5bb45;   // posted after Init
// The region-music request Init posts to SL::spMS2 with Data1 = 1 (0x100237d54). cSC4AudioEventHandler::
// HandleRadioMessages turns it into cIGZRadio::SetStation("krgn", fade), i.e. GameData/Radio/Stations/Region/Music;
// mayor mode posts Data1 = 0 ("kmay").
constexpr std::uint32_t kRegionScreenMsgRegionMusic = 0x2a3ad653;
constexpr std::uint32_t kRegionScreenMsgLeavingRegion = 0x8bb5bb46;
constexpr std::uint32_t kRegionScreenMsgLeftRegion = 0x8bb5bb4b;
constexpr std::uint32_t kRegionScreenScrollStart = 0x8a4bac53;     // cSC4WinRegionScroll messages
constexpr std::uint32_t kRegionScreenScrollStop = 0x8a4bac5b;
constexpr std::uint32_t kRegionScreenScrollDelta = 0xaaa1cdf2;
constexpr std::uint32_t kMsgRegionCityViewChanged = 0x2b96b3ea;
constexpr std::uint32_t kRegionScreenCameraGridMessage = 0xab96b05f;
constexpr std::uint32_t kMsgRegionCloudsPreference = 0x231bbf91;
constexpr std::uint32_t kRegionScreenCommandMessage = 0x4b99446a;

// Commands (ProcessCommand / Init / Shutdown).
constexpr std::uint32_t kCmdRegionQuitGame = 0x6a935e3c;
constexpr std::uint32_t kCmdRegionBitmapLoad = 0x6a9757c2;
constexpr std::uint32_t kCmdRegionScrollLeft = 0x6a935cd8;
constexpr std::uint32_t kCmdRegionScrollLeftStop = 0x2a948275;
constexpr std::uint32_t kCmdRegionScrollRight = 0x6a935ce0;
constexpr std::uint32_t kCmdRegionScrollRightStop = 0x2a94826e;
constexpr std::uint32_t kCmdRegionScrollUp = 0x6a935cdd;
constexpr std::uint32_t kCmdRegionScrollUpStop = 0x2a948272;
constexpr std::uint32_t kCmdRegionScrollDown = 0x6a935ce2;
constexpr std::uint32_t kCmdRegionScrollDownStop = 0x2a94826b;
constexpr std::uint32_t kCmdRegionSetExpandedToolTips = 0x6aa9fe51;
constexpr std::uint32_t kCmdRegionLoadRegion = 0x0bb3c277;
constexpr std::uint32_t kCmdRegionLoadCity = 0x0bb2747d;
constexpr std::uint32_t kCmdRegionCancel = 0x6a935cf1;
// The 14 commands the screen registers (ids above, in the same order).
constexpr std::uint32_t kRegionScreenCommands[14] = {
    kCmdRegionQuitGame, kCmdRegionBitmapLoad, kCmdRegionScrollLeft, kCmdRegionScrollLeftStop, kCmdRegionScrollRightStop,
    kCmdRegionScrollRight, kCmdRegionScrollUp, kCmdRegionScrollUpStop, kCmdRegionScrollDownStop, kCmdRegionScrollDown,
    kCmdRegionSetExpandedToolTips, kCmdRegionLoadRegion, kCmdRegionLoadCity, kCmdRegionCancel};
constexpr const char* kRegionScreenCommandNames[14] = {
    "QuitGame", "RegionBitmapLoad", "ScrollLeft", "ScrollLeftStop", "ScrollRightStop", "ScrollRight", "ScrollUp",
    "ScrollUpStop", "ScrollDownStop", "ScrollDown", "SetExpandedToolTips", "LoadRegion", "LoadCity", "Cancel"};

// The preference blob offsets the screen reads / writes (SL::spPreferences, 0x1734 bytes).
constexpr std::size_t kPrefsRegionName = 0xebc;     // 0x40 bytes
constexpr std::size_t kPrefsViewX = 0xefc;          // int, region-map units * 256
constexpr std::size_t kPrefsViewY = 0xf00;          // int
constexpr std::size_t kPrefsShowGrid = 0xf04;       // u8
constexpr std::size_t kPrefsSignpostMode = 0xf05;   // u8
constexpr std::size_t kPrefsMapMode = 0xf06;        // u8
constexpr std::size_t kPrefsExpandedToolTips = 0xf0a;   // u8
constexpr std::size_t kPrefsCloudsVisible = 0xf11;  // u8 (also read by cSC4WinRegionScroll's edge scroll)

// Tuning property ids (ReadTunableValues, 0x10023904e).
constexpr std::uint32_t kTuningScrollDeadZone = 0xca383ca4;
constexpr std::uint32_t kTuningScrollMaxDistance = 0xca383ca3;
constexpr std::uint32_t kTuningScrollSpeedDivisor = 0xca383ca7;
constexpr std::uint32_t kTuningCityHalfSizeX = 0xca383ca5;
constexpr std::uint32_t kTuningCityHalfSizeY = 0xca383ca6;
constexpr std::uint32_t kTuningWaterErosionClicks = 0xca383ca8;
constexpr std::uint32_t kTuningWaterErosionFlowSteps = 0xca383ca9;
constexpr std::uint32_t kTuningWaterErosionSmoothing = 0xca383caa;
constexpr std::uint32_t kTuningLabelColor1 = 0xca383cab;
constexpr std::uint32_t kTuningLabelColor2 = 0xca383cac;
constexpr std::uint32_t kTuningBaseTerrainTypeHeight = 0xca383cad;
constexpr std::uint32_t kTuningRegionHeightByte = 0xca383cae;
constexpr std::uint32_t kTuningSmoothDistance = 0xca383caf;
constexpr std::uint32_t kTuningSmoothSeconds = 0xca383cb0;
constexpr std::uint32_t kTuningCloudOpacity = 0xca383cb1;

// ---------------------------------------------------------------------------------------------------------------
// cSC4RegionViewCity (not recovered — the export lists only its constructor and destructor): the per-city record the
// screen keeps in its vector at +0x1d0 (0xf0 bytes each). Only the fields the recovered code touches are named; the
// three span vectors belong to SC4CreateSpanList (thresholds 0, 8 and 0x10) and their element type is a placeholder.
// ---------------------------------------------------------------------------------------------------------------
struct RegionViewCitySpan {
    std::int32_t left = 0, right = 0, y = 0;   // span layout not decoded; never read by the recovered code
};
constexpr std::size_t kRegionScreenCitySize = 0xf0;

struct RegionViewCity {
    int left = 0;                                  // +0x00  x * 64 + 1 + (1 << size)   (PutCitiesInView)
    int top = 0;                                   // +0x04
    int x = 0;                                     // +0x08  city grid x (also the region cell x)
    int z = 0;                                     // +0x0c  city grid z
    float mapX = 0;                                // +0x10  position on the region map
    float mapY = 0;                                // +0x14
    std::uint8_t size = 0;                         // +0x18  size code; a city is (1 << size) cells wide
    std::uint8_t pad19_[7] = {};                   // +0x19..+0x1f
    cRZAutoRefCount<IWinBuffer> snapshot;          // +0x20  the city thumbnail (one reference held)
    cRZAutoRefCount<IWinBuffer> mask;              // +0x28  its mask / alpha
    IWinBuffer* snapshotSmall = nullptr;           // +0x30  the loaded-image pair (owned, released explicitly)
    IWinBuffer* maskSmall = nullptr;               // +0x38
    IWinBuffer* shadowMask = nullptr;              // +0x40
    std::uint8_t pad48_[0x28] = {};                // +0x48..+0x6f
    std::vector<RegionViewCitySpan> spans0;        // +0x70
    std::vector<RegionViewCitySpan> spans10;       // +0x88
    std::vector<RegionViewCitySpan> spans8;        // +0xa0
    int centerX = 0;                               // +0xb8  label anchor in image coordinates
    int centerY = 0;                               // +0xbc
    std::uint8_t padC0_[0x30] = {};                // +0xc0..+0xef
};
static_assert(sizeof(RegionViewCity) == kRegionScreenCitySize, "cSC4RegionViewCity is 0xf0 bytes");

// ---------------------------------------------------------------------------------------------------------------
// cISC4RegionalCity (not recovered) as this screen uses it.
// ---------------------------------------------------------------------------------------------------------------
class IRegionalCity : public cIGZUnknown {
public:
    ~IRegionalCity() = default;
    virtual int GetPopulation() = 0;                       // vslot 0x48, summed by UpdatePopulation
    virtual bool IsOccupied() = 0;                         // vslot 0x158, picks the signpost type
    virtual void GetSize(int& cellsX, int& cellsZ) = 0;    // vslot 0x38, the launched-city size filter
    virtual void GetName(cRZString& out) = 0;              // vslot 0x108
};
// The region's city list holds every city in a cRZAutoRefCount slot whose address the GetCity slot answers.
using RegionCityRef = cRZAutoRefCount<IRegionalCity>;

// ---------------------------------------------------------------------------------------------------------------
// cSC4WinRegionView (34 functions, not recovered): the map window at +0x160.
// ---------------------------------------------------------------------------------------------------------------
class IRegionView {
public:
    virtual ~IRegionView() = default;
    virtual void AddCity(const RegionViewCity& city) = 0;                       // @ 0x10075afc6 region view AddCity
    virtual void UpdateCity(RegionViewCity* city) = 0;                          // @ 0x10075d5e6
    virtual void SetScrollPos(int x, int y) = 0;                                // @ 0x10075d9dc
    virtual RegionViewCity* GetCityAtPoint(int x, int y) = 0;                   // @ 0x10075db8a
    virtual RegionViewCity* GetSelectedCity() = 0;                              // @ 0x10075ddb2
    virtual void ClearSelection() = 0;                                          // @ 0x10075dce8
    virtual void SetSignpostMode(bool mode, bool second) = 0;                   // @ 0x10075dd0a
    virtual void EnableLabels(bool labels, bool second) = 0;                    // @ 0x10075dd1e
    virtual void EnableGrid(bool on) = 0;                                       // @ 0x10075dd36
    virtual void EnableTutorialArrow(bool on) = 0;                              // @ 0x10075dd48
    virtual void SetMapMode(int mode) = 0;                                      // @ 0x10075dd56
    virtual void SelectCity(int x, int y) = 0;                                  // @ 0x10075dd66
    virtual void GetRegionPlayfieldRect(RegionViewCity* city, cRZRect& out) = 0;   // @ 0x10075ddc0
    virtual void SetSignpostType(std::uint32_t type, std::uint32_t sub) = 0;    // @ 0x10075de92
    // +0x1a8 (byte) = on; when on, vslot 0x2e0 is called too (0x10023a07d).
    virtual void SetScrollIndicator(bool on) = 0;
    virtual void SetRegionIndex(int index) = 0;                                 // view +0x158
    virtual void SetLabelColours(std::uint32_t first, std::uint32_t second) = 0;   // view +0x180 / +0x184
    virtual cIGZWin* AsWindow() = 0;                                            // the cGZWin view (for child handling)
};

// ---------------------------------------------------------------------------------------------------------------
// cRZAutoMsg2Notification (the framework helper constructed inline at +0x1b8, 0x18 bytes; not recovered).
// ---------------------------------------------------------------------------------------------------------------
class INotificationHelper {
public:
    virtual ~INotificationHelper() = default;
    virtual void AddMessageTypes(const std::uint32_t* types, int count) = 0;    // constructor's AddMessageTypes
    virtual void AddAllNotifications(cIGZMessageTarget2* target) = 0;           // @ 0x100041aae
    virtual void RemoveAllNotifications(cIGZMessageTarget2* target) = 0;        // @ 0x100041b6e
};

// ---------------------------------------------------------------------------------------------------------------
// cSC43DRender at +0x270 and the two services at +0x250 / +0x258 (all not recovered).
// ---------------------------------------------------------------------------------------------------------------
class IRegionRenderer {
public:
    virtual ~IRegionRenderer() = default;
    virtual bool Init(int viewportWidth, int viewportHeight) = 0;   // vslot 0x18
    virtual void Shutdown() = 0;                                    // vslot 0x20
    // vslot 0x168: GZPaint asks for two floats and ignores the answer, but the call is part of the frame.
    virtual void GetScreenOffsets(float& x, float& y) = 0;
};
// The service at +0x250 (GZCOM 0xa9c73857 / iid 0x86ad10ed); its vslot 0x48 runs once per paint.
class IRegionFrameService {
public:
    virtual ~IRegionFrameService() = default;
    virtual bool Init() = 0;        // vslot 0x18
    virtual void Shutdown() = 0;    // vslot 0x20
    virtual void EndFrame() = 0;    // vslot 0x48
};
// cSC4EffectsManager at +0x258 (GZCOM 0x49822f75 / iid 0xc97cf5cd).
class IEffectsManager {
public:
    virtual ~IEffectsManager() = default;
    virtual bool Init() = 0;        // vslot 0x18
    virtual void Shutdown() = 0;    // vslot 0x20
};

// The camera basis UpdateRendererPosition multiplies by: SC4ViewUtilities +0x68 (right), +0x74 (up) and +0x158
// (world units per viewport unit). The recovered SC4ViewUtilities keeps those members private, so they come from the
// host instead of reading the object directly.
struct RegionCameraBasis {
    float right[3] = {0, 0, 0};       // +0x68 / +0x6c / +0x70
    float up[3] = {0, 0, 0};          // +0x74 / +0x78 / +0x7c
    float worldPerUnit = 0;           // +0x158
};

// ---------------------------------------------------------------------------------------------------------------
// Everything the screen reaches outside itself.
// ---------------------------------------------------------------------------------------------------------------
class IRegionScreenHost {
public:
    virtual ~IRegionScreenHost() = default;

    // --- framework services
    virtual cIGZMessageServer2* MessageServer2() = 0;                       // SL::spMS2
    virtual cIGZCommandServer* CommandServer() = 0;                         // SL::spCommandServer
    virtual std::uint8_t* Preferences() = 0;                                // SL::spPreferences (0x1734 bytes)
    virtual void SavePreferences() = 0;                                     // cISC4App vslot 0x88
    virtual int DoubleClickTimeMs() = 0;                                    // RZ::WCM() vslot 0x140
    virtual void PlayUiSound(std::uint32_t soundId) = 0;                    // SL::spSS vslot 0x50 (1, id, 0, 0, 0)
    virtual cIGZUnknown* GraphicSystemObject() = 0;                         // SL::spGS (AddRef'd by the caller)
    virtual void SetTerrainMap(cSC4RegionalTerrainMap* map) = 0;            // SL::spTerrainMap
    // SL::spViewUtilities / spLightingManager / spRenderer (renderer vslots 0x40 / 0x48).
    virtual void SetViewUtilities(IRegionRenderer* renderer) = 0;
    virtual void ClearEffectsManagerGlobal() = 0;                           // SL::spEffectsManager = 0
    virtual void ClearViewGlobals() = 0;                                    // spLightingManager / spViewUtilities / spRenderer
    virtual void ClearTerrainMapGlobal() = 0;                               // SL::spTerrainMap = 0
    virtual void FlushResources() = 0;                                      // SL::spRM vslot 0x158
    // Objects created by InitApplicationServices (their classes need GZCOM or are not recovered).
    // The three services below are borrowed: the screen Shutdowns them but does not destroy them (the original holds
    // COM references; the host keeps the lifetime in the port).
    virtual IRegionRenderer* CreateRenderer() = 0;                          // new cSC43DRender
    virtual IRegionFrameService* CreateFrameService() = 0;                  // GZCOM 0xa9c73857 / iid 0x86ad10ed
    virtual IEffectsManager* CreateEffectsManager() = 0;                    // GZCOM 0x49822f75 / iid 0xc97cf5cd
    virtual cIGZWinKeyAccelerator* CreateKeyAccelerator() = 0;              // GZCOM 0x42e967be / iid 0xe2c1b3c4
    virtual INotificationHelper* CreateNotificationHelper() = 0;            // the +0x1b8 helper (borrowed)
    virtual cIGZMessage2Standard* CreateStandardMessage() = 0;              // new cRZMessage2Standard with one reference
    virtual void PostEngineMessage(std::uint32_t type) = 0;                 // SL::spMS2 vslot 0x20 (message, false)

    // --- region data (cISC4Region and its city list)
    virtual int RegionCount() = 0;                                          // region vslot 0x30
    virtual int CurrentRegionIndex() = 0;                                   // cISC4Nation vslot 0x38 (0x100237d16)
    // cISC4Nation vslot 0x30 (0x100237cfd, no arguments, return ignored) and vslot 0x50 (0x100237d0a, which writes
    // the previous region index and answers the validity flag). The nation object stays behind the host; the screen
    // only needs the pair to decide whether Init restores the saved view position (0x100237d29).
    virtual void UpdateRegionViewState() = 0;
    virtual bool PreviousRegionIndex(int& previousIndex) = 0;
    virtual void SelectRegion(int index) = 0;                               // region vslot 0x48
    virtual const char* RegionNameAt(int index) = 0;                        // region vslot 0x58 -> 0x18 -> 0x38
    virtual const char* RegionName() = 0;                                   // city list vslot 0x18 -> vslot 0x38
    virtual bool ReloadRegion() = 0;                                        // cISC4App vslot 0x60 (0)
    virtual void DeleteRegion(int index) { (void)index; }                   // cISC4Nation vslot 0x30 (i386)
    virtual bool RegionCellBounds(int& left, int& top, int& right, int& bottom) = 0;   // city list vslot 0xc0
    virtual int CitySnapshotSet() = 0;                                      // city list vslot 0x90
    // region vslot 0xb8 (GetCityLocations): the city places of the active region (x, z, log2 of the edge length).
    virtual void CityLocations(std::vector<cISC4Region::cLocation>& out) { (void)out; }
    // SC4ReadCityRegionViewThumbnails (0x1001d2cf3) for the city at (x, z): the picture the city's save file carries for the region
    // map (records {0x8a2482b9, 0x4a2482bb, 0} and its mask at instance 2). Each comes back with one reference, or null.
    virtual bool LoadCityThumbnails(int x, int z, IWinBuffer** image, IWinBuffer** mask) { (void)x; (void)z; (void)image; (void)mask; return false; }
    virtual RegionCityRef* FindCity(int x, int z) = 0;                      // city list vslot 0x58 (borrowed)
    virtual RegionCityRef* FindOrCreateCity(const RegionViewCity& city, bool create) = 0;
    virtual bool LaunchRegionCityByName(const cRZString& name, RegionCityRef* city) = 0;   // cISC4App vslot 0x68
    virtual void ClearRegionViewSlot() = 0;                                 // cISC4App vslot 0x138 -> +0x200
    virtual std::uint32_t LaunchCityFromRegion(RegionCityRef* city) = 0;    // cISC4App vslot 0x30
    virtual void QuitToRegionSelect(bool flag) = 0;                         // cISC4App vslot 0x58

    // --- the region view window (cSC4WinRegionView, recovered in src/sc4/WinRegionView.*; the screen reaches it
    // through the IRegionView seam so the test can fake the whole view).
    virtual IRegionView* CreateRegionView() = 0;                            // new cSC4WinRegionView (one owner)
    // Init's 12 snapshot buffers (0x100238088..0x10023829f): cSLBuffer(type 0x856ddbac, group 0x6a1eed2c,
    // instance, colour 9, always-reload false).GetBuffer(), and the fallback the original uses when an image is
    // missing: SL::spGS vslot 0x28 makes a blank display-format buffer. Both return one reference.
    virtual IWinBuffer* LoadRegionScreenSnapshot(std::uint32_t instance) = 0;
    virtual IWinBuffer* CreateBlankDisplayBuffer() = 0;

    // --- tuning
    // SL::spRM lookup of {0x6534284a, 0x690f693f, 0xaa383bfe} with iid 0xa52160f5; returns the property holder with one
    // reference the caller releases.
    virtual cISCPropertyHolder* CreateTuningProperties() = 0;
    virtual bool CloudOpacityBytes(cISCPropertyHolder* holder, std::uint8_t& first, std::uint8_t& second) = 0;
    // SC4ViewUtilities +0x68 / +0x74 / +0x158 (the recovered class keeps them private).
    virtual bool CameraBasis(RegionCameraBasis& basis) = 0;
    // The three globals ReadTunableValues writes: kNumWaterErosionIterationsPerPlayerClick, ...
    virtual void SetWaterErosionTunables(std::uint32_t clicks, std::uint32_t flowSteps, std::uint32_t smoothing) = 0;
    // cSC4Region::sBaseTerrainTypeHeight and DAT_100b248e8.
    virtual void SetRegionHeightGlobals(std::uint32_t baseTerrainTypeHeight, std::uint32_t other) = 0;

    // --- the panel buttons' vslot 0xf0 (child window, true)
    virtual void ActivatePanelButton(cIGZWin* button, cIGZWin* rootChild) = 0;
    // The options menu's game options button (0x0a5510a9): the modal "Game Options" dialog (cSC4GamePreferencesWinProc).
    virtual void RunGameOptionsDialog() {}
    // The options menu's audio options button (0xa98f4f88): the modal "Audio Options" dialog.
    virtual void RunAudioOptionsDialog() {}
    // The options menu's graphic options button (0x098f4f6c): the modal "Graphic Options" dialog.
    virtual void RunGraphicOptionsDialog() {}
    // RZ::LM()->vslot 0x128(0)->vslot 0x90 (value, out): the localised number text.
    virtual void FormatNumber(int value, cRZString& out) = 0;
};
IRegionScreenHost* RegionScreenHost();
void SetRegionScreenHost(IRegionScreenHost* host);

// ---------------------------------------------------------------------------------------------------------------
// The screen.
// ---------------------------------------------------------------------------------------------------------------
class cSC4WinRegionScreen : public cGZWin, public cIGZWinMessageFilter, public cIGZMessageTarget2 {
public:
    explicit cSC4WinRegionScreen(cIGZWinMgr* mgr);              // @ 0x100235b78 (+0x100235ed2 base alias)
    ~cSC4WinRegionScreen() override;                            // @ 0x100235edc (+0x100236360 alias, +0x10023636a deleting)

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x100239f72 (+0x100239fa8 / 0x100239fba thunks)
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }   // @ 0x100239f16 (+0x100239f20 / 0x100239f32 thunks)
    std::uint32_t Release() override { return cGZWin::Release(); } // @ 0x100239f44 (+0x100239f4e / 0x100239f60 thunks)

    bool DoMessage(cGZMessage& message) override;                  // @ 0x100239fcc
    bool Init() override;                                          // @ 0x100237bf2 (PARTIAL)
    bool Shutdown() override;                                      // @ 0x100239aa0
    bool GZPaint() override;                                       // @ 0x100237382

    bool GZOnMouseEnter(cIGZWin* from) override;                   // @ 0x100237748
    bool GZOnMouseExit(cIGZWin* to) override;                      // @ 0x10023b78e
    bool GZOnMouseMove(int x, int y, std::uint32_t modifiers) override;   // @ 0x100237794
    bool GZOnMouseDownL(int x, int y, std::uint32_t modifiers) override;  // @ 0x1002377b2
    bool GZOnMouseDownR(int x, int y, std::uint32_t modifiers) override;  // @ 0x100237bda
    bool GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;   // @ 0x10023b7ac
    bool GZOnKeyUp(std::uint32_t key, std::uint32_t modifiers) override;     // @ 0x10023b7b4
    bool GZOnCommand(std::uint32_t command, std::uint64_t param) override;   // @ 0x10023b92a (PARTIAL)

    // --- cIGZMessageTarget2 (the view at +0x158)
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x10023a0dc (+0x10023a648 thunk)

    // --- cIGZWinMessageFilter (the view at +0x150)
    bool DoMessage(cIGZWin* window, cGZMessage& message) override; // @ 0x10023a65c (+0x10023a664 thunk)

    // --- the screen's own virtuals (vtable slots after the cGZWin ones)
    void PutCitiesInView();                                        // @ 0x100236398 (PARTIAL)
    void SetCitySnapshotsToDefault(RegionViewCity& city);          // @ 0x100236b66
    void RecalculateImagesFromCitySnapshots(RegionViewCity& city); // @ 0x100236c36 (PARTIAL)
    void ComputeBaseLimits();                                      // @ 0x100237106
    void UpdateRendererPosition();                                 // @ 0x100237632
    void RestoreSignposts();                                       // @ 0x1002379ae
    void ShowPanel(int panel);                                     // @ 0x100237a58
    void PostRegionScreenMessage(std::uint32_t type);              // @ 0x1002388d2
    void InitApplicationServices();                                // @ 0x100238972 (PARTIAL)
    void ReadTunableValues();                                      // @ 0x10023904e
    void LoadGUI();                                                // @ 0x10023927a (PARTIAL)
    void ShutdownApplicationServices();                            // @ 0x100239dac
    bool ProcessCommand(std::uint32_t command, cIGZCommandParameterSet* input,
                        cIGZCommandParameterSet* output);          // @ 0x10023a1fc
    void MakeWholeRegion();                                        // @ 0x10023a66c (PARTIAL)
    RegionCityRef* GetOrCreateRegionalCity(const RegionViewCity& city, bool create);   // @ 0x10023ae9c (PARTIAL)
    bool LoadRegion(const cRZString& name);                        // @ 0x10023b130
    bool LaunchCity(const cRZString& name, std::uint32_t size, cRZString& out, bool empty);   // @ 0x10023b366
    bool LaunchRegionalCity(RegionCityRef* city);                  // @ 0x10023b63c
    static void SetUserPreferencesToCurrentRegion();               // @ 0x10023b7bc (a static member in the original)
    void SetUserPreferencesToCurrentPosition();                    // @ 0x10023b846
    void UpdatePopulation();                                       // @ 0x10023c54c

    // --- test access
    IRegionView* View() const { return view_; }
    cIGZWin* Backdrop() const { return backdrop_; }
    cSC4WinRegionClouds* Clouds() const { return clouds_; }
    cSC4WinRegionScroll* Scroll() const { return scroll_; }
    cSC4CameraControl* CameraControl() const { return cameraControl_; }
    s3d::cS3DCamera* Camera() const { return camera_; }
    cSC4RegionalTerrainMap* TerrainMap() const { return terrainMap_; }
    IRegionRenderer* Renderer() const { return renderer_; }
    IRegionFrameService* FrameService() const { return frameService_; }
    IEffectsManager* Effects() const { return effects_; }
    cIGZWinKeyAccelerator* Accelerator() const { return accelerator_; }
    INotificationHelper* Notifications() const { return notifications_; }
    std::vector<RegionViewCity>& Cities() { return cities_; }
    const std::vector<RegionViewCity>& Cities() const { return cities_; }
    float TargetX() const { return targetX_; }
    float TargetY() const { return targetY_; }
    void SetTarget(float x, float y) { targetX_ = x; targetY_ = y; }
    bool Smoothing() const { return smoothing_; }
    int BoundMinX() const { return boundMinX_; }
    int BoundMinY() const { return boundMinY_; }
    int BoundMaxX() const { return boundMaxX_; }
    int BoundMaxY() const { return boundMaxY_; }
    void SetBounds(int minX, int minY, int maxX, int maxY);
    bool SignpostActive() const { return signpostActive_; }
    int SignpostMode() const { return signpostMode_; }
    void SetSignpostMode(int mode) { signpostMode_ = mode; }
    void SetCityHalfSizes(int x, int y) { cityHalfSizeX_ = x; cityHalfSizeY_ = y; }
    int CityHalfSizeX() const { return cityHalfSizeX_; }
    int CityHalfSizeY() const { return cityHalfSizeY_; }
    int LayoutCountdown() const { return hideCountdown_; }
    bool PanelsVisible() const { return panelsVisible_; }
    // Puts the smooth-scroll state where a mouse-wheel / command scroll would leave it.
    void BeginSmoothing(float targetX, float targetY) {
        smoothTargetX_ = targetX;
        smoothTargetY_ = targetY;
        smoothing_ = true;
    }
    // Init's snapshot-buffer setup (the original fills the 12 slots from the graphic system); the accessor takes the
    // reference the caller holds.
    void SetSnapshotBuffer(int slot, IWinBuffer* buffer);
    IWinBuffer* SnapshotBuffer(int slot) const { return snapshots_[slot]; }
    void SetMemberWindow(int which, cIGZWin* window);   // 0..2 panels, 3..5 panel buttons, 6 backdrop, 7 unused 0x280
    void SetClouds(cSC4WinRegionClouds* clouds);
    void SetScroll(cSC4WinRegionScroll* scroll);
    void SetView(IRegionView* view);   // the screen takes ownership

private:
    // The two secondary vtables sit at +0x150 / +0x158, so every member starts at +0x160.
    IRegionView* view_ = nullptr;                          // +0x160
    cSC4WinRegionClouds* clouds_ = nullptr;                // +0x168
    cIGZWin* backdrop_ = nullptr;                          // +0x170
    std::array<cIGZWin*, 3> panels_{};                     // +0x178 / +0x180 / +0x188
    std::array<cIGZWin*, 3> panelButtons_{};               // +0x190 / +0x198 / +0x1a0
    cIGZUnknown* unknown1a8_ = nullptr;                    // +0x1a8
    cIGZWinKeyAccelerator* accelerator_ = nullptr;         // +0x1b0
    INotificationHelper* notifications_ = nullptr;         // +0x1b8 (the original object is 0x18 bytes inline)
    std::vector<RegionViewCity> cities_;                   // +0x1d0
    std::array<IWinBuffer*, 12> snapshots_{};              // +0x1e8: large [0..5], small [6..11]
    cIGZUnknown* graphicSystem_ = nullptr;                 // +0x248
    IRegionFrameService* frameService_ = nullptr;          // +0x250
    IEffectsManager* effects_ = nullptr;                   // +0x258
    s3d::cS3DCamera* camera_ = nullptr;                    // +0x260
    cSC4CameraControl* cameraControl_ = nullptr;           // +0x268
    IRegionRenderer* renderer_ = nullptr;                  // +0x270
    cSC4RegionalTerrainMap* terrainMap_ = nullptr;         // +0x278
    cIGZWin* unused280_ = nullptr;                         // +0x280
    cSC4WinRegionScroll* scroll_ = nullptr;                // +0x288

    float targetX_ = 0;                                    // +0x290
    float targetY_ = 0;                                    // +0x294
    int boundMinX_ = 0;                                    // +0x298
    int boundMinY_ = 0;                                    // +0x29c
    int boundMaxX_ = 0;                                    // +0x2a0
    int boundMaxY_ = 0;                                    // +0x2a4
    int cityHalfSizeX_ = 0x96;                             // +0x2b0
    int cityHalfSizeY_ = 0x96;                             // +0x2b4
    bool signpostActive_ = false;                          // +0x2b8
    std::uint8_t baseTerrainTypeHeight_ = 0x80;            // +0x2b9
    std::uint8_t regionHeightByte_ = 0x40;                 // +0x2ba
    int currentRegionIndex_ = 0;                           // +0x2bc
    int signpostMode_ = 1;                                 // +0x2c0
    std::uint32_t labelColor1_ = 0x00808080;               // +0x2c8
    std::uint32_t labelColor2_ = 0xffff0000;               // +0x2cc
    cRZTimer paintTimer_{cRZTimer::kMilliseconds};         // +0x2d0
    cRZTimer doubleClickTimer_{cRZTimer::kMilliseconds};   // +0x2e8
    int hideCountdown_ = 0;                                // +0x300
    float smoothSeconds_ = 5.0f;                           // +0x304
    float smoothDistance_ = 1200.0f;                       // +0x308
    float smoothTargetX_ = 0;                              // +0x30c
    float smoothTargetY_ = 0;                              // +0x310
    bool smoothing_ = false;                               // +0x314
    bool panelsVisible_ = false;                           // +0x315
    [[maybe_unused]] std::uint8_t pad316_[2] = {};
};

}  // namespace sc4
