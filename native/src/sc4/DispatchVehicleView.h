// cSC4DispatchVehicleView — the on-screen indicators over tracked occupants: the dispatch manager's fire / police vehicles
// (an icon with the call number), the MySim (an icon with the sim's picture), the city situations' indicators (the CSI
// button with its picture, the mission target arrow) and the player-drive arrow (a spinning 3-D model). An indicator whose
// occupant is on screen is drawn as a billboard over it; otherwise (and always for the clustered, situation and labelled
// kinds) it is drawn as a 2-D arrow at the view's edge pointing at the occupant, nudged off the UI windows and off the
// arrows already drawn this frame. Recovered from the Mac i386 build (symbols): 0x0019ae08..0x0019e6c0 plus the TrackInfo
// copy / destructor (0x006a5f1c / 0x006a4f7a) and UseAlternateModel (0x00801bfc).
//
// It is a cSC4ViewObject3D (+0x00, the renderer calls Update from its view-object list 5, priority 1000), a cRZUnknown
// (+0x04) and a cIGZMessageTarget2 (+0x0c; QueryInterface answers 0x452294aa with it). Original layout:
//   +0x10 u8  initialised (Init / Shutdown)
//   +0x14 / +0x1c / +0x24 cSC4PackedRenderState  the second 2-D pass / the faint first pass / an unused third state
//   +0x2c..+0x44 seven cS3DTextureBinding*        the icons {0x7ab50e44, 0x1abe787d, instance}: +0x2c fire 0x144161a1,
//                                                 +0x30 police 0x144161a2, +0x34 MySim 0x144161a3, +0x38 0x4bb0ecf3 (loaded,
//                                                 never given to an indicator), +0x3c CSI 0x2bb12d1f, +0x40 target
//                                                 0x4bb0ecf3, +0x44 tracker type 2 0x144161a2
//   +0x48 std::multimap<u8 tracker type, TrackInfo> (header +0x4c, leftmost +0x54; TrackInfo at node +0x14)
//   +0x60 std::set<TrackerType>                   the hidden tracker types (ShowAllIndicators)
//   +0x78 u32[7] maximum per tracker type          (Init sets 0xffffffff; 0 = unlimited for TrackOccupant, none drawn)
//   +0x94 u32[7] tracked count per tracker type
//   +0xb0 u32 active indicator handle
//   +0xb4 std::list<tRectInfo>                     the window clip list BuildWindowClipList fills
//
// TrackInfo (0x108 bytes; offsets relative to the record):
//   +0x00 handle  +0x04 tracker type  +0x08 cISC4Occupant* (counted)  +0x0c call number / picture texture id
//   +0x10 call number  +0x14 picture id (0 = load +0x18 as a bitmap file)  +0x18 cRZString bitmap path
//   +0x24 cS3DTextureBinding* (the view's)  +0x28 sVF_V3F_T2F[4] call number / picture quad  +0x78 sVF_V3F_T2F[4] icon quad
//   +0xc8 / +0xcc width / height  +0xd0 last screen rect (l, t, r, b)  +0xe0 last screen centre  +0xec button state
//   +0xf0 alpha  +0xf4 flags  +0xf8 cIS3DModelInstanceAnimated* (counted)  +0xfc flash time  +0x100 flash period
//   +0x104 cS3DModelProcessor* (owned)
// flags: 0x1 not an automaton (a building: clustered around it), 0x2 has a call number / picture, 0x4 selectable kind,
//   0x8 fades in / out, 0x10 fading out (removed when transparent), 0x20 visible, 0x40 3-D model, 0x80 locked to its last
//   rect, 0x100 flashing, 0x200 coloured by the player vehicle's damage, 0x400 alternate model (never read), 0x800 the
//   occupant's top-priority indicator (only those are drawn).
//
// Quirks kept (addresses are the original functions):
//  * TrackOccupant refuses a type whose maximum is reached, but a maximum of 0 means "no limit" there while Update draws
//    none of that type (0x0019e1ea / 0x0019cc8e).
//  * the handle counter is one file-static shared by every view (0x0019e1ea).
//  * Shutdown does not reset the per-type counts and does not clear the automata flag TrackOccupant set (0x0019c486).
//  * RemoveTrackInfo releases the texture without clearing the field (the record is erased right after, 0x0019cace).
//  * UseAlternateModel looks the multimap up by the *low byte of the handle* (it is keyed by tracker type) and ignores its
//    flag argument; the 0x400 it sets is never read (0x00801bfc).
//  * the 3-D arrow turns by a fixed 7 degrees every drawn frame, not by the elapsed time (0x0019c098).
//  * the 2-D arrow quad's vertices keep their untransformed z (only x / y are written back, 0x0019cc8e).
//  * Update's elapsed time comes from a function-local static timer that is restarted only when the view level is 1, so
//    time spent in other view levels or while snapshotting counts on the next drawn frame (0x0019cc8e).
//
// PARTIAL / seams (docs/SEAMS.md): the SL::sp* services, the font (RZ::WCM()), the texture upload (nsTexUtil) and the 3-D
// view window tree are IDispatchVehicleViewHost; one method per original call, slot / address in the comment.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/DispatchManager.h"
#include "sc4/PackedRenderState.h"
#include "sc4/PylonOccupant.h"   // IPylonFont, IPylonTextureBinding: the same font object and binding shape
#include "sc4/Render3D.h"

class cIGZBuffer;
class SC4DrawContext;
namespace s3d { class cIS3DModelInstanceAnimated; class cS3DModelProcessor; }

namespace sc4 {

class cISC4LightingManager;

// the message Init registers for: every indicator's call number / picture is rebuilt (DoMessage)
constexpr std::uint32_t kDispatchViewMessageRebuild = 0x8acaa95f;
// the occupant types TrackOccupant tells apart
constexpr std::uint32_t kDispatchViewClusteredOccupantType = 0x278128a0;   // flag 0x1: indicators circle the occupant
constexpr std::uint32_t kDispatchViewAutomatonTypeA = 0x496e5783;
constexpr std::uint32_t kDispatchViewAutomatonTypeB = 0xc7cc75f1;
constexpr std::uint32_t kDispatchViewTrackedAutomataFlag = 0x200000;       // set on a tracked automaton, cleared on removal
// the player-drive arrow model {0x5ad0e817, 0xbadb57f1, 0x8bb90000}
constexpr std::uint32_t kDispatchViewArrowModelKey[3] = {0x5ad0e817, 0xbadb57f1, 0x8bb90000};
// the icon textures {0x7ab50e44, 0x1abe787d, instance}, in the order of the view's +0x2c..+0x44 slots
constexpr std::uint32_t kDispatchViewIconInstance[7] = {0x144161a1, 0x144161a2, 0x144161a3, 0x4bb0ecf3,
                                                        0x2bb12d1f, 0x4bb0ecf3, 0x144161a2};
// kCallNumberYOffset (viewport units above the occupant, by zoom), kTrackerClipPriority (by tracker type)
constexpr float kDispatchViewCallNumberYOffset[6] = {20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 60.0f};
constexpr std::uint8_t kDispatchViewTrackerClipPriority[7] = {1, 1, 1, 1, 2, 2, 3};
constexpr int kDispatchViewTrackerTypes = 7;

// The tracker types (cSC4DispatchVehicleView::TrackerType) as their callers use them.
enum DispatchViewTrackerType : int {
    kTrackerFire = 0,             // the dispatch manager's fire vehicles
    kTrackerPolice = 1,           // the dispatch manager's police vehicles
    kTrackerDispatchOther = 2,    // the dispatch manager's third kind (its kTrackerTypeForDispatchType table)
    kTrackerMySim = 3,            // the MySim: an icon with the sim's picture
    kTrackerSituation = 4,        // the city situation indicator (CSI) button: the only selectable kind
    kTrackerSituationTarget = 5,  // the mission target arrow
    kTrackerPlayerDrive = 6,      // the spinning 3-D arrow over the player's vehicle
};

// sVF_V3F_T2F (format 2, 0x14 bytes)
struct DispatchViewVertex {
    float x = 0, y = 0, z = 0;
    float u = 0, v = 0;
};
static_assert(sizeof(DispatchViewVertex) == 0x14, "sVF_V3F_T2F stride");

// tRectInfo: a screen rect plus the tracker clip priority it blocks (0xff = every priority).
struct DispatchViewRectInfo {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;   // +0x00..+0x0c
    std::uint8_t priority = 0xff;                            // +0x10
};
// SC4Rect<long>
struct DispatchViewRect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

// A child window of the 3-D view as sEnumChildWindows reads it.
struct DispatchViewChildWindow {
    std::uint32_t id = 0;          // the EnumChildren id argument
    bool visible = false;          // cIGZWin vslot 0x11c IsVisible
    cRZRect area;                  // cIGZWin vslot 0xc4 GetAreaAbsolute
    bool fading = false;           // cIGZWin vslot 0x1c4 GetParam(0xe6e998fe) answers a value
};

// cISC4Automaton as the view reaches it (cISC4Occupant QueryInterface 0xa9b40f05). One reference; Release drops it.
class IDispatchViewAutomaton {
public:
    virtual ~IDispatchViewAutomaton() = default;
    virtual void Release() = 0;
    virtual void SetAutomataFlag(std::uint32_t flag, bool on) = 0;    // i386 vslot 0x78
    virtual bool CheckAutomataFlag(std::uint32_t flag) = 0;           // i386 vslot 0x7c
    // i386 vslot 0xe0 GetState / vslot 0x10c GetAutomataGroup: false when either is null. `stateValue` is the state's u32
    // at +0x60 (the player-drive damage).
    virtual bool StateAndGroup(std::uint32_t& stateValue) = 0;
    // the group's player-drive parameters (group +0x74, else cSC4OccupantGroupTemplate::GetPlayerDriveParams of +0xc): the
    // float at +0x5c (the damage limit). False when there are none.
    virtual bool PlayerDriveParam5c(float& value) = 0;
};

class IDispatchVehicleViewHost {
public:
    virtual ~IDispatchVehicleViewHost() = default;
    // SL::spMS2.
    virtual cIGZMessageServer2* Messages() = 0;
    // SL::spRenderer: presence, and vslot 0x28 GetDrawContext (Init calls it and drops the result).
    virtual bool RendererPresent() = 0;
    virtual void RendererGetDrawContext() = 0;
    // cIS3DTextureBindingFactory (service 0xfac0e219) vslot 0x10 with {0x7ab50e44, 0x1abe787d, instance}: one reference
    // (null when the texture is missing).
    virtual IPylonTextureBinding* CreateTextureBinding(std::uint32_t instance) = 0;
    // nsTexUtil::ReleaseTexture(SL::spGDriver, id).
    virtual void ReleaseTexture(std::uint32_t texture) = 0;
    // nsTexUtil::SetupTexture(buffer, SL::spGDriver, &id, true, 0, 0, 0); the original skips it without SL::spGDriver.
    virtual bool UploadTexture(cIGZBuffer* buffer, std::uint32_t& texture) = 0;
    // RZ::GS() vslot 0x10: a new buffer (one reference); false without a graphics system.
    virtual bool CreateBuffer(cIGZBuffer** out) = 0;
    // RZ::WCM() vslot 0x14(0x68963c4c): the call number font (borrowed; the view takes a reference while drawing).
    virtual IPylonFont* CallNumberFont() = 0;
    // RZ::GZCOM() vslot 0xc(0xa2ffb5d3, 0x02ffbb60) then vslot 0x10(path, &buffer, 8): a bitmap file (one reference).
    virtual bool LoadBitmapFromFile(const cIGZString& path, cIGZBuffer** out) = 0;
    // SL::spRM vslot 0xc({0x856ddbac, 0x46a006b0, id}, 0x06f1568e) then the resource's vslot 0x10: the picture bitmap
    // (one reference).
    virtual bool LoadPictureBitmap(std::uint32_t id, cIGZBuffer** out) = 0;
    // SC4ViewUtilities (SL::spViewUtilities): GetZoomAndRotation, ViewportUnitsToWorldUnits, +0x18 pitch, +0x54 / +0x5c
    // view direction x / z, ProjectPointToViewportCoordinates2D (in place).
    virtual void GetZoomAndRotation(int& zoom, int& rotation) = 0;
    virtual float ViewportUnitsToWorldUnits(float units) = 0;
    virtual float ViewPitchAngle() = 0;
    virtual void ViewDirection(float& x, float& z) = 0;
    virtual void ProjectPointToViewportCoordinates2D(float* point) = 0;
    // Update's function-local static cRZTimer (milliseconds): Stop then GetElapsedTime, and Restart.
    virtual float StopTickTimer() = 0;
    virtual void RestartTickTimer() = 0;
    // The draw context's render properties (+0x38) bool 28 kRenderSnapshot: Update draws nothing while it is set.
    virtual bool RenderSnapshot(SC4DrawContext& context) = 0;
    // SL::spView3D vslot 0x58 GetCurrentViewLevel (Update draws at level 1 only).
    virtual std::uint32_t CurrentViewLevel() = 0;
    // SL::spSimulator vslot 0x44 IsEmergencyPaused (false without a simulator): situation indicators are not drawn.
    virtual bool SimulatorEmergencyPaused() = 0;
    // SL::spView3DWin: false without it, else its GetAreaAbsolute (vslot 0xc4), and EnumChildren(0x22ba0121, vslot 0x80).
    virtual bool View3DWinArea(cRZRect& out) = 0;
    virtual void View3DChildWindows(std::vector<DispatchViewChildWindow>& out) = 0;
    // SL::spTerrain vslot 0x2c LocationIsInBounds(x, z), 0x30 GetAltitude(x, z), 0x74 GetSeaLevel.
    virtual bool TerrainLocationInBounds(float x, float z) = 0;
    virtual float TerrainAltitude(float x, float z) = 0;
    virtual float TerrainSeaLevel() = 0;
    // The draw context's renderer (+0x34) vslot 0x64 Set2DCameraView / 0x60 Set3DCameraView.
    virtual void Set2DCameraView(SC4DrawContext& context) = 0;
    virtual void Set3DCameraView(SC4DrawContext& context) = 0;
    // cISC4Occupant QueryInterface(0xa9b40f05): null when the occupant is not an automaton.
    virtual IDispatchViewAutomaton* QueryAutomaton(cISC4Occupant* occupant) = 0;
    // GetAnimatedModelFromResourceKey (0x00139f80): one reference in *out.
    virtual bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey& key, s3d::cIS3DModelInstanceAnimated** out) = 0;
    // SL::spLightingManager (borrowed).
    virtual cISC4LightingManager* LightingManager() = 0;
};
IDispatchVehicleViewHost* DispatchVehicleViewHost();
void SetDispatchVehicleViewHost(IDispatchVehicleViewHost* host);

class cSC4DispatchVehicleView final : public IDispatchVehicleView, public cISC4ViewObject3D, public cIGZMessageTarget2 {
public:
    struct TrackInfo {
        TrackInfo() = default;                                  // 0x006a6234
        TrackInfo(const TrackInfo& other);                      // 0x006a5f1c (adds a model reference)
        TrackInfo& operator=(const TrackInfo&) = delete;
        ~TrackInfo();                                           // 0x006a4f7a (drops the model reference)

        std::uint32_t handle = 0;                               // +0x00
        int type = 0;                                           // +0x04
        cISC4Occupant* occupant = nullptr;                      // +0x08 (counted)
        std::uint32_t texture = 0;                              // +0x0c
        std::uint32_t callNumber = 0;                           // +0x10
        std::uint32_t pictureId = 0;                            // +0x14
        cRZString label;                                        // +0x18 the bitmap file path
        IPylonTextureBinding* binding = nullptr;                // +0x24 (the view's)
        DispatchViewVertex numberQuad[4];                       // +0x28
        DispatchViewVertex iconQuad[4];                         // +0x78
        float width = 0.0f;                                     // +0xc8
        float height = 0.0f;                                    // +0xcc
        DispatchViewRect lastRect;                              // +0xd0
        cS3DVector3 lastCenter;                                 // +0xe0
        std::uint32_t buttonState = 0;                          // +0xec
        float alpha = 0.0f;                                     // +0xf0
        std::uint32_t flags = 0;                                // +0xf4
        s3d::cIS3DModelInstanceAnimated* model = nullptr;       // +0xf8 (counted)
        float flashTime = 0.0f;                                 // +0xfc
        float flashPeriod = 0.0f;                               // +0x100
        s3d::cS3DModelProcessor* processor = nullptr;           // +0x104 (owned by the map's record)
    };
    using TrackMap = std::multimap<std::uint8_t, TrackInfo>;

    cSC4DispatchVehicleView();                                  // 0x0019b53e
    ~cSC4DispatchVehicleView();                                 // 0x0019b324 (0x0019b2a8 deleting)

    // cIGZUnknown
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x0019e68e
    std::uint32_t AddRef() override;                               // 0x006a5eec
    std::uint32_t Release() override;                              // 0x006a5f0e

    // IDispatchVehicleView (the dispatch manager's seam)
    bool Init() override;                                          // 0x0019af2a
    bool Shutdown() override;                                      // 0x0019c486
    void HandleViewChanged() override;                             // 0x0019c634
    int TrackOccupant(cISC4Occupant* occupant, int trackerType, std::uint32_t value, const cRZString* label) override;   // 0x0019e1ea
    void UntrackOccupant(int trackId, bool force) override { Untrack(static_cast<std::uint32_t>(trackId), force); }

    // cISC4ViewObject3D
    void DrawViewObject(SC4DrawContext& context) override { Update(&context); }
    // cIGZMessageTarget2
    bool DoMessage(cIGZMessage2* message) override;                // 0x0019c048

    // the rest of the class
    bool Update(SC4DrawContext* context);                          // 0x0019cc8e
    bool Untrack(std::uint32_t handle, bool force);                // UntrackOccupant(unsigned long, bool) 0x0019cbd8
    void SetIndicatorMax(int trackerType, std::uint32_t max);      // 0x0019ae08
    void ShowAllIndicators(int trackerType, bool show);            // 0x0019ca5e
    void FlashIndicator(std::uint32_t handle, bool on, float period);   // 0x0019c788
    void LockIndicator(std::uint32_t handle, bool lock);           // 0x0019c7fc
    void SetActiveIndicator(std::uint32_t handle, std::uint32_t buttonState);   // 0x0019c848
    std::uint32_t FindIndicatorHandle(std::int32_t x, std::int32_t y);   // 0x0019c66e
    const DispatchViewRect* GetLastScreenIconRect(std::uint32_t handle);  // 0x0019c712
    cISC4Occupant* GetActiveOccupant();                            // 0x0019c758
    bool UseAlternateModel(std::uint32_t handle, bool alternate);  // 0x00801bfc
    void SortIndicatorPriority(cISC4Occupant* occupant);           // 0x0019c8fc
    bool IsSelectableIndicator(const TrackInfo& info) const;       // 0x0019ae1a
    void BuildCallNumberDisplay(TrackInfo& info);                  // 0x0019b622
    void BuildWindowClipList();                                    // 0x0019b4ba
    // 0x0019ae6a (static): false when `rect` lies inside `bounds` and overlaps no clip-list entry of its priority; else
    // true with the blocking rect (the bounds, priority 0xff, when `rect` leaves them) in `hit`.
    static bool TestOverlap(const DispatchViewRectInfo& rect, const std::list<DispatchViewRectInfo>& list,
                            const DispatchViewRect& bounds, DispatchViewRectInfo& hit);

    // recovered state, for the tests and the app
    bool IsInitialized() const { return initialized_; }                              // +0x10
    const TrackMap& Tracks() const { return tracks_; }                               // +0x48
    const std::set<int>& HiddenTypes() const { return hidden_; }                     // +0x60
    std::uint32_t IndicatorMax(int type) const { return max_[type]; }                // +0x78
    std::uint32_t TrackedCount(int type) const { return count_[type]; }              // +0x94
    std::uint32_t ActiveHandle() const { return active_; }                           // +0xb0
    const std::list<DispatchViewRectInfo>& WindowClipList() const { return clipList_; }   // +0xb4
    IPylonTextureBinding* IconBinding(int slot) const { return bindings_[slot]; }    // +0x2c + 4 * slot
    const TrackInfo* FindTrackInfo(std::uint32_t handle) const;
    const cSC4PackedRenderState& RenderState(int index) const { return states_[index]; }   // +0x14 / +0x1c / +0x24

private:
    TrackMap::iterator FindTrackInfoByHandle(std::uint32_t handle);   // 0x0019c6e4
    void RemoveTrackInfo(TrackMap::iterator it, TrackInfo& info);      // 0x0019cace
    void Update3DModelIndicator(SC4DrawContext* context, TrackInfo& info, float elapsed);   // 0x0019c098
    static bool EnumChildWindow(const cRZRect& parentArea, const DispatchViewChildWindow& child,
                                std::list<DispatchViewRectInfo>& clipList);   // sEnumChildWindows 0x0019b398

    std::uint32_t refs_ = 0;                                    // +0x04 cRZUnknown
    bool initialized_ = false;                                  // +0x10
    cSC4PackedRenderState states_[3] = {{0, 0}, {0, 0}, {0, 0}};   // +0x14 / +0x1c / +0x24 (the ctor zeroes them)
    IPylonTextureBinding* bindings_[7] = {};                    // +0x2c..+0x44 (owned)
    TrackMap tracks_;                                           // +0x48
    std::set<int> hidden_;                                      // +0x60
    std::uint32_t max_[kDispatchViewTrackerTypes] = {};         // +0x78
    std::uint32_t count_[kDispatchViewTrackerTypes] = {};       // +0x94
    std::uint32_t active_ = 0;                                  // +0xb0
    std::list<DispatchViewRectInfo> clipList_;                  // +0xb4
};

}  // namespace sc4
