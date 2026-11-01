// cSC4ConstructionOccupant — the occupant that shows a building going up: the scaffold around it, the four corner
// posts, the planks and girders on top, the tower crane, the ground/roof props, the construction crew automata, the
// dust cloud and the welding sparks. 52 hand-written functions at 0x100177328..0x10017df2c (plus the compiler's
// adjustor thunks for the six base views); the plain/sized operators at 0x100177318 / 0x1001775f2 and the GZCOM
// factory at 0x1001772db are recorded with the allocators / GZCOM tables, and 0x100177f2e..0x10017e5xx are the
// std::vector / std::list helpers of the nested ConstructionPropEntry / CrewData types.
//
// RTTI: `typeinfo for cSC4ConstructionOccupant` (0x100a79f40) has one base, `cISC4ConstructionOccupant`
// (0x100b00a60, its own vtable at 0x100b00a88). The class id is 0xa97f909e, the interface iid 0x496e7636 and the
// occupant type 0x096e763c.
//
// Original layout (0x270+ bytes; the GZCOM creator allocates sizeof(cSC4ConstructionOccupant)):
//   +0x00 cISC4ConstructionOccupant vptr (0x100a798a0: +0x18 AsOccupant, +0x20 SetLot, +0x28 Init, +0x30 Shutdown,
//         +0x38 Write, +0x40 Read, +0x48 GetGZCLSID, +0x50 GetPosition, +0x58 SetPosition, +0x60 GetBoundingBox,
//         +0x68 GetOwner, +0x70 SetOwner, +0x78 GetBounds, +0x80 GetWorldBounds, +0x88 GetTransform,
//         +0x90 SetTransform, +0x98 GetSortValue, +0xa0 Draw, +0xa8 DoMessage, +0xb0/+0xb8 the two destructors)
//   +0x08 cIGZMessageTarget2 vptr (DoMessage at +0x18)
//   +0x10 cSC4Occupant (0x60 bytes; its occupant type sits at +0x58, i.e. the base's +0x48)
//   +0x70 cS3DModelInstanceBasic (0x38 bytes; properties 5 instead of the default 2)
//   +0xa8 cISC4Lot* lot                 +0xb0 cISC4BuildingOccupant* building   (one reference each)
//   +0xb8 cS3DTransform transform       +0xf0 cS3DBoundingBox building bounds    +0x108 cS3DBoundingBox roof bounds
//   +0x120 bool has crane               +0x121 bool model needs a rebuild
//   +0x128 void* sides[4] (0x50 each)   +0x148 void* planks (0x140)   +0x150 void* girders (0x50)
//   +0x158 void* posts (0x500)          +0x160 cSC4PackedRenderState  +0x168 bool rotation changed
//   +0x170 cS3DModelInstance* crane     +0x178 float crane base xyz   +0x188 void* crane base plate (0x100)
//   +0x190 std::vector<std::vector<cS3DTextureBinding*>> textures (6 rows x 5 zoom levels)
//   +0x1a8 u8 crane model index
//   +0x1b0 std::list<cISC4PropOccupant*> ground props (count +0x1c0)
//   +0x1c8 std::list<cISC4PropOccupant*> roof props   (count +0x1d8)
//   +0x1e0 std::vector<cSC4PropOccupantVisualEffect*> sparks
//   +0x1f8 cSC4PropOccupantVisualEffect* dust cloud
//   +0x200 u32 stage (0 scaffold, 1 segment, 2 interior, 3 scaffold removal, 4 complete)
//   +0x204 u32 day the stage timing started   +0x208 u32 (saved, never read)
//   +0x20c u32 day the next stage is due      +0x210 u32 segment count      +0x214 u32 segments done
//   +0x218 float segments * segment height    +0x21c float building base Y
//   +0x220 u32 first-scaffold duration        +0x224 u32 segment duration
//   +0x228 u32 scaffold removal duration      +0x22c u32 interior duration
//   +0x230 u32 animation time (ms / 1000)     +0x234 u32 crane state (0 rotate, 1 pause, 2 wait)
//   +0x238 float crane yaw min                +0x23c float crane yaw max
//   +0x240 bool (saved, never read)           +0x244 u32 crane rotation time   +0x248 u32 crane rotation duration
//   +0x24c float crane yaw target             +0x250 float crane yaw start
//   +0x258 std::list<CrewData> crew (next / prev / count at +0x258 / +0x260 / +0x268)
//
// Geometry buffers (the vertex strides the draw calls imply):
//   sides / planks / girders use the draw context's format 2 = 0x14-byte vertices (x, y, z, u, v); the port's
//   ScaffoldVertex is that layout. The sides and girders never get u/v written in the original (the buffers are
//   operator new[] memory), the planks get u = x * scaffold texture scale and v = z * scale.
//   posts use format 1 = 0x10-byte vertices (x, y, z, colour); the port's PostVertex is that layout.
//   The post buffer is 4 corner posts x 5 quads x 4 vertices = 80 vertices; RebuildModel writes the four vertical
//   faces of each 1 x 1 x height box and one quad per corner post on top.
//
// The neighbours the original reaches through SL::sp* globals / the framework / the building development simulator
// are seams in IConstructionOccupantHost (one method per original call, the global and vtable slot in the comment).
// The lot, the props, the visual effects, the automata, the occupant manager, the renderer and the view filter are
// reached through the narrow interfaces below because the port's concrete classes for them do not keep the original
// COM views (cSC4Lot is a plain class, cSC4PropOccupant has no cISC4PropOccupant view, ...).
//
// Quirks kept (each asserted in scripts/tests/sc4_constructionoccupant_test.cpp, address in the comment):
//  * SetPosition does nothing (a 6-byte body: `ret`, 0x10017cbfe).
//  * GetPosition returns the centre of the bounding box, Y included (0x10017cb4e); GetBoundingBox copies the box.
//  * Init creates texture bindings for rows 0..4 only (0x1001779fd compares the row counter with 5), so the girders
//    (row 5, drawn at 0x10017c7a1) never get a binding; the original would dereference a null row. The port draws
//    the girders with the same texture only when the row exists and otherwise skips them (0x10017c7a1).
//  * StaticInit seeds the process-wide random generator from the wall clock (0x100177a46) and reads all tuning
//    values from exemplar 0x6534284a / 0x2989fd57 / 0x8989dbe3; a missing exemplar leaves the defaults
//    (segment height 3, post width 1, ...).
//  * DoDailySim removes the roof props with a `for` over the list that erases nothing (the list is cleared after
//    the walk, 0x10017909a), and the same stage-0/2 case both bumps the segment counter.
//  * SetLot with a lot that has no building detaches and releases the lot and returns without adding the occupant
//    to the renderer (0x10017b0xx).
//  * CreateCrane starts the crane at yaw `-yawMin * 0.5 + yawMax + yawMin` (0x100179e50), which is neither the
//    yaw min nor the yaw max SetLot sets up.
//  * The construction prop entries are built from the prop profile's size[0] and size[2] only; the seven inverted
//    tuning properties divide one by the property value at load time.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/StreamOps.h"
#include "rz/cRZRandom.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PackedRenderState.h"
#include "sc4/AutomataManager.h"
#include "sc4/PropManager.h"
#include "sc4/SCPropertyHelp.h"
#include "s3d/ModelInstance.h"

class SC4DrawContext;
class cIGZMessage2;
class cIGZMessageServer2;

namespace sc4 {

class IConstructionLot;
class IConstructionAutomaton;

constexpr std::uint32_t kGZCLSID_cSC4ConstructionOccupant = 0xa97f909e;
constexpr std::uint32_t kGZIID_cISC4ConstructionOccupant = 0x496e7636;
constexpr std::uint32_t kOccupantTypeConstruction = 0x096e763c;
constexpr std::uint32_t kOccupantGroupConstruction = 0x5007;        // added by Init
constexpr std::uint32_t kConstructionRecordVersion = 5;             // u16 on the stream, < 4 is rejected
constexpr std::uint32_t kConstructionLegacyTransformVersion = 5;    // < 5 uses S3DTransformLegacyLoad
constexpr std::uint32_t kOccupantFlagInRenderer = 2;                // cSC4Occupant flags byte, bit 1

// Message types DoMessage dispatches.
constexpr std::uint32_t kMessageAnimationTick = 0x86ad10ee;        // data2 = elapsed ms
constexpr std::uint32_t kMessageViewZoomRotationDeferred = 0xa6b79602;   // void1 / void2: two int[2] (zoom, rotation)
constexpr std::uint32_t kMessageViewFilterChange = 0x6cdb65b;       // void2: cSC4ViewFilterInformation*
constexpr std::uint32_t kMessageCityPostInit = 0x26d31ec1;
constexpr std::uint32_t kMessageDayTick = 0x66956814;
constexpr std::uint32_t kMessageConstructionStage = 0x49e7b1e2;    // sent on every segment / at the start
constexpr std::uint32_t kMessageConstructionComplete = 0xc9e7b1e8; // sent on start and on shutdown / completion
constexpr std::uint32_t kMessageBuildingCompletionChanged = 0x2977ae2f;
constexpr std::uint32_t kAnimationTickChannel = 0x80;

// Exemplars and resources.
constexpr std::uint32_t kConstructionPropertyType = 0x6534284a;     // the tuning exemplar
constexpr std::uint32_t kConstructionPropertyGroup = 0x2989fd57;
constexpr std::uint32_t kConstructionPropertyInstance = 0x8989dbe3;
constexpr std::uint32_t kConstructionPropertyIID = 0xa52160f5;      // seek id of the resource manager call
constexpr std::uint32_t kResourceTypeS3DModel = 0x9ad0e7e3;
constexpr std::uint32_t kCraneModelType = 0x5ad0e817;
constexpr std::uint32_t kCraneModelGroup = 0xbadb57f1;
constexpr std::uint32_t kTextureBindingType = 0x7ab50e44;
constexpr std::uint32_t kTextureBindingGroup = 0x1abe787d;
constexpr std::uint32_t kTextureBindingServiceId = 0x1ac0e11a;      // cIS3DTextureBindingFactory
constexpr std::uint32_t kTextureBindingServiceIID = 0xfac0e219;
constexpr std::uint32_t kScaffoldTextureRows = 6;                   // side x 4, planks, girders
constexpr std::uint32_t kScaffoldTextureFrames = 5;                 // zoom levels

// The tuning exemplar's properties (StaticInit, 0x100177a46).
constexpr std::uint32_t kPropertySegmentHeight = 0x8989ec22;
constexpr std::uint32_t kPropertyPostWidth = 0x8989ec23;
constexpr std::uint32_t kPropertyScaffoldGap = 0x8989ec24;
constexpr std::uint32_t kPropertyPlankWidth = 0x8989ec25;
constexpr std::uint32_t kPropertyPlankTextureScale = 0x8989ec26;    // stored inverted
constexpr std::uint32_t kPropertyScaffoldTextureScale = 0x8989ec27; // stored inverted
constexpr std::uint32_t kPropertyGirderTextureScale = 0x8989ec28;   // stored inverted
constexpr std::uint32_t kPropertyScaffoldGrowthRate = 0x8989ec29;   // stored inverted
constexpr std::uint32_t kPropertyScaffoldRemovalRate = 0x8989ec2a;  // stored inverted
constexpr std::uint32_t kPropertyCranePostWidth = 0x8989ec2b;
constexpr std::uint32_t kPropertyCranePostToBuildingGap = 0x8989ec2c;
constexpr std::uint32_t kPropertyCraneHeightAboveScaffold = 0x8989ec2d;
constexpr std::uint32_t kPropertyCraneTimeOfFullRotation = 0x8989ec2e;
constexpr std::uint32_t kPropertyConstructionCraneType = 0xaa83558f; // 1 outer, 2 inner, 3 outer then inner

// The tuning defaults (the statics' initial values in the binary).
constexpr float kDefaultSegmentHeight = 3.0f;
constexpr float kDefaultPostWidth = 1.0f;
constexpr float kDefaultScaffoldGap = 0.5f;
constexpr float kDefaultPlankWidth = 0.5f;
constexpr float kDefaultPlankTextureScale = 1.0f;
constexpr float kDefaultScaffoldTextureScale = 1.0f;
constexpr float kDefaultGirderTextureScale = 1.0f;
constexpr float kDefaultScaffoldGrowthRate = 1.0f;
constexpr float kDefaultScaffoldRemovalRate = 1.0f;
constexpr float kDefaultCranePostWidth = 0.5f;
constexpr float kDefaultCranePostToBuildingGap = 0.25f;
constexpr float kDefaultCraneHeightAboveScaffold = 2.0f;
constexpr float kDefaultCraneTimeOfFullRotation = 10.0f;

// The statics StaticInit fills (the original keeps them as class statics in .data / .bss).
extern float sfSegmentHeight, sfPostWidth, sfScaffoldGap, sfPlankWidth, sfScaffoldGrowthRate,
    sfScaffoldRemovalRate, sfCranePostWidth, sfCranePostToBuildingGap, sfCraneHeightAboveScaffold,
    sfCraneTimeOfFullRotation;
extern float sfPlankTextureScale, sfScaffoldTextureScale, sfGirderTextureScale;
extern float sfCraneLength[3], sfCraneFrontArmLength[3], sfCraneBackArmLength[3];
extern bool sbStaticInitialized;
extern std::uint32_t snTotalPropCount, snUniversalPropCount;

// One entry of the construction prop table (0xc bytes): the prop type and its footprint along x / z.
struct ConstructionPropEntry {
    std::uint32_t type = 0;   // +0
    float sizeX = 0;          // +4
    float sizeZ = 0;          // +8
};
static_assert(sizeof(ConstructionPropEntry) == 0xc, "ConstructionPropEntry stride");
extern std::vector<ConstructionPropEntry> sPropList;

// A crew member (std::list node payload, 0x10 bytes with the two flags): the automaton and the flag that says
// whether it walks on the roof (SetScaffoldHeight repositions only the roof crew, 0x10017a710).
struct CrewData {
    bool first = false;               // +0x10 in the node (saved, never read)
    bool onRoof = false;              // +0x11
    IConstructionAutomaton* automaton = nullptr;   // +0x18 (one reference)
};

// A scaffold vertex with the draw context's format 2 (0x14 bytes).
struct ScaffoldVertex {
    float x = 0, y = 0, z = 0;
    float u = 0, v = 0;
};
// A post vertex with the draw context's format 1 (0x10 bytes): colour is 0xAABBGGRR.
struct PostVertex {
    float x = 0, y = 0, z = 0;
    std::uint32_t colour = 0;
};
static_assert(sizeof(ScaffoldVertex) == 0x14, "ScaffoldVertex stride");
static_assert(sizeof(PostVertex) == 0x10, "PostVertex stride");

// ---- seams (one method per original call; the SL:: global / vtable slot is in the comment) -----------------------

// A texture binding (cS3DTextureBinding, 0x1000d9bd4): the original keeps one reference per binding and reads the
// texture id it draws with as *(int*)(**(void***)binding) (0x10017c5ed).
class IConstructionTexture : public cIGZUnknown {
public:
    virtual std::int32_t TextureId() const = 0;
};

// The tool that owns the lot the occupant is built on: the calls the class makes on cISC4Lot.
// (cISC4Lot vslot 0x70 GetLocation -> cell rect, 0x110 SetState(3, 0, 0), 0x180 GetBuilding.)
class IConstructionLot : public cIGZUnknown {
public:
    virtual void GetCellRect(std::int32_t rect[4]) = 0;                        // vslot 0x70
    virtual void SetState(std::uint32_t state, std::uint32_t wealth, std::uint8_t vacateReason) = 0;   // 0x110
    virtual cISC4BuildingOccupant* GetBuilding() = 0;                          // vslot 0x180 (borrowed)
};

// A prop occupant (cISC4PropOccupant, iid 0xe9793a65): the class creates props through the prop manager and only
// uses these two calls on the prop view (the rest go through the occupant view).
class IConstructionProp : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;      // [3] 0x18
    virtual void SetOrientation(std::int32_t orientation) = 0;   // [10] 0x50
};

// A prop visual effect (cSC4PropOccupantVisualEffect, iid 0x09820792): new + Init + the calls below.
class IConstructionEffect : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                   // 0x58
    virtual void SetPosition(const float* xyz) = 0;            // 0x80
    virtual void SetEffectType(const char* name) = 0;          // 0x28
    virtual bool StartEffect(bool instant) = 0;                // 0x38
    virtual bool StopEffect(bool instant) = 0;                 // 0x40
    virtual cISC4Occupant* AsOccupant() = 0;                   // 0x18
};

// A construction automaton (cISC4Automaton, iid 0xa9b40f05): the crew's walking actors.
class IConstructionAutomaton : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                         // 0x18 (the crew's occupant view)
    virtual void SetAutomataFlag(std::uint32_t flag, bool on) = 0;   // 0xf0
    virtual void SetOrientation(const float* direction) = 0;         // 0x70
    std::uint32_t AddRef() override = 0;                             // 0x08
};

// The renderer's model-list calls (cISC43DRender vslots 0xd8 / 0xe0 / 0xe8).
class IConstructionRenderer {
public:
    virtual ~IConstructionRenderer() = default;
    virtual bool IsModelInserted(s3d::cIS3DModelInstance* model) = 0;
    virtual bool InsertModel(s3d::cIS3DModelInstance* model) = 0;
    virtual bool RemoveModel(s3d::cIS3DModelInstance* model) = 0;
};

// Everything outside the object. The real build hands the recovered classes / globals straight to these methods
// (the adapter lives in the application layer); the test installs fakes.
class IConstructionOccupantHost {
public:
    virtual ~IConstructionOccupantHost() = default;

    // SL::spMS2 and SL::ATM() (vslots 0x28 / 0x30, channel 0x80).
    virtual cIGZMessageServer2* MessageServer() = 0;
    virtual void AddTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    virtual void RemoveTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    // The process-wide random generator (::sRandom).
    virtual cRZRandom* Random() = 0;
    // SL::spRM vslot 0x18({0x6534284a, 0x2989fd57, 0x8989dbe3}, 0xa52160f5) then the resource's vslot 0x30:
    // the tuning exemplar's property holder (borrowed).
    virtual cISCPropertyHolder* ConstructionSettings() = 0;
    // SL::spRM vslot 0x18(key, iid) + cIS3DResModel vslot 0x20(0, &instance); one reference.
    virtual s3d::cIS3DModelInstance* CreateModelInstance(const cGZPersistResourceKey& key, std::uint32_t iid) = 0;
    // RZGetFramework()->GetService(0x1ac0e11a, 0xfac0e219) vslot 0x20(key, &binding); one reference.
    virtual IConstructionTexture* CreateTextureBinding(const cGZPersistResourceKey& key) = 0;
    // SL::spPropManager vslot 0x70: the prop profile (only size[0] / size[2] are read).
    virtual const PropProfile* PropProfile(std::uint32_t type) = 0;
    // SL::spPropManager vslot 0x78: one reference.
    virtual bool CreateProp(std::uint32_t type, IConstructionProp** out) = 0;
    virtual void AddLotProp(IConstructionLot* lot, cISC4Occupant* occupant) = 0;      // vslot 0xd0
    virtual void RemoveProp(cISC4Occupant* occupant) = 0;                            // vslot 0xe8
    // The automata manager's RemoveAutomaton (vslot 0x40) used by Shutdown.
    virtual void RemoveAutomaton(IConstructionAutomaton* automaton) = 0;
    // new cSC4PropOccupantVisualEffect + AddRef + Init; one reference.
    virtual IConstructionEffect* CreatePropEffect() = 0;
    // The building development simulator: vslot 0x120 -> 0x148(lot, &out, true) creates the lot's base texture
    // occupant (one reference); 0x120 -> 0x198(lot) returns the lot's building occupant (borrowed).
    virtual bool CreateLotBaseTextureOccupant(IConstructionLot* lot, cISC4LotBaseTextureOccupant** out) = 0;
    virtual cISC4BuildingOccupant* LotBuilding(IConstructionLot* lot) = 0;
    // The automata manager behind SL::spCity vslot 0x160 and its CreateAutomaton (vslot 0x78).
    virtual IConstructionAutomaton* CreateAutomaton(std::uint32_t group, const float position[3],
                                                    std::uint32_t flags, const cGZPersistResourceKey& key,
                                                    std::uint8_t argument) = 0;
    // SL::spOccupantManager vslots 0xa0 / 0xa8 / 0xc0.
    virtual bool InsertOccupant(cISC4Occupant* occupant, std::uint32_t data) = 0;
    virtual bool RemoveOccupant(cISC4Occupant* occupant, bool shutdown, std::uint32_t data) = 0;
    virtual bool MoveOccupant(cISC4Occupant* occupant, bool post) = 0;
    // SL::spTerrain vslots 0x60 (height), 0x38 / 0x40 (cell counts), 0x110 (cell has water).
    virtual float TerrainAltitude(float x, float z) = 0;
    virtual int TerrainCellCountX() = 0;
    virtual int TerrainCellCountZ() = 0;
    virtual bool TerrainCellHasWater(int x, int z) = 0;
    // SL::spCity vslot 0x300: the world position of a standard city cell's origin.
    virtual void CityCellToPosition(int cellX, int cellZ, float& x, float& z) = 0;
    // SL::spSimulator vslot 0x40 (GetSimDateNumber).
    virtual std::uint32_t SimulatorDay() = 0;
    // SL::spView3D vslot 0x90: the current occupant filter (borrowed, null when there is no view).
    virtual cISC4OccupantFilter* ViewFilter() = 0;
    // SL::spViewUtilities: ProjectPointToViewCoordinates (0x10017383c) / TransformBoundingBox (0x100176ae6).
    virtual void ProjectToViewCoordinates(float point[3]) = 0;
    virtual void TransformBoundingBox(const cS3DTransform& transform, cS3DBoundingBox& box) = 0;
    // The render properties' detail level (SL::spRenderProperties +0x30 object, +0x230 int).
    virtual int RenderDetailLevel() = 0;
    // SL::spLightingManager vslot 0xb8: the light colour for a direction, 3 bytes.
    virtual void LightColourForDirection(const float direction[3], std::uint8_t rgb[3]) = 0;
    // The renderer's model list.
    virtual IConstructionRenderer* Renderer() = 0;
    // Read: the lot object a city file holds (the port's serializable lot adapter) as the lot interface, one reference.
    virtual IConstructionLot* AdoptLot(cIGZUnknown* object) = 0;
};
IConstructionOccupantHost* ConstructionOccupantHost();
void SetConstructionOccupantHost(IConstructionOccupantHost* host);

// cISC4ConstructionOccupant (iid 0x496e7636).
class cISC4ConstructionOccupant : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                                       // [3]  0x18
    virtual void SetLot(IConstructionLot* lot) = 0;                                // [4]  0x20
    virtual bool Init() = 0;                                                       // [5]  0x28
    virtual bool Shutdown() = 0;                                                   // [6]  0x30
    virtual bool Write(cIGZOStream& stream) const = 0;                             // [7]  0x38
    virtual bool Read(cIGZIStream& stream) = 0;                                    // [8]  0x40
    virtual std::uint32_t GetGZCLSID() const = 0;                                  // [9]  0x48
    virtual void GetPosition(float* xyz) = 0;                                      // [10] 0x50
    virtual void SetPosition(const float* xyz) = 0;                                // [11] 0x58
    virtual void GetBoundingBox(float* minXyz, float* maxXyz) = 0;                 // [12] 0x60
    virtual cIGZUnknown* GetOwner() = 0;                                           // [13] 0x68
    virtual void SetOwner(cIGZUnknown* owner) = 0;                                 // [14] 0x70
    virtual void GetBounds(cS3DBoundingBox& out) = 0;                              // [15] 0x78
    virtual void GetWorldBounds(cS3DBoundingBox& out) = 0;                         // [16] 0x80
    virtual void GetTransform(cS3DTransform& out) = 0;                             // [17] 0x88
    virtual void SetTransform(const cS3DTransform& transform) = 0;                 // [18] 0x90
    virtual void GetSortValue(std::uint32_t& out) const = 0;                       // [19] 0x98
    virtual int Draw(SC4DrawContext* context, bool flag) = 0;                      // [20] 0xa0
    virtual bool DoMessage(cIGZMessage2* message) = 0;                             // [21] 0xa8
};

// cSC4ConstructionOccupant — the six original views (cISC4ConstructionOccupant at +0, cIGZMessageTarget2 at +8,
// cSC4Occupant at +0x10, cIGZSerializable / cSCExemplarPropertyHolder inside it, cS3DModelInstanceBasic at +0x70)
// collapse into this base list; the port's layout differs, the original offsets are in the members' comments.
class cSC4ConstructionOccupant final : public cISC4ConstructionOccupant,
                                       public cIGZMessageTarget2,
                                       public cSC4Occupant,
                                       public s3d::cS3DModelInstanceBasic {
public:
    cSC4ConstructionOccupant();
    ~cSC4ConstructionOccupant() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }
    std::uint32_t Release() override { return cSC4Occupant::Release(); }

    // cISC4ConstructionOccupant + cISC4Occupant + cIGZSerializable + cS3DModelInstanceBasic
    cISC4Occupant* AsOccupant() override { return static_cast<cSC4Occupant*>(this); }
    void SetLot(IConstructionLot* lot) override;
    bool Init() override;
    bool Shutdown() override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4ConstructionOccupant; }
    void GetPosition(float* xyz) override;
    void SetPosition(const float*) override {}                     // quirk: a no-op (0x10017cbfe)
    void GetBoundingBox(float* minXyz, float* maxXyz) override;
    cIGZUnknown* GetOwner() override;
    void SetOwner(cIGZUnknown*) override {}                        // quirk: a no-op (0x10017dd24)
    void GetBounds(cS3DBoundingBox& out) override;
    void GetWorldBounds(cS3DBoundingBox& out) override;
    void GetTransform(cS3DTransform& out) override;
    void SetTransform(const cS3DTransform& transform) override;
    void GetSortValue(std::uint32_t& out) const override;
    int Draw(SC4DrawContext* context, bool flag) override;
    bool DoMessage(cIGZMessage2* message) override;

    // The rest of the class (the original's own non-interface entry points).
    static void StaticInit();                                       // 0x100177a46
    void SendConstructionMessage(std::uint32_t message, bool flag); // 0x1001783d6
    void DoDailySim();                                              // 0x10017909a
    void DoAnimationTick(std::uint32_t elapsed);                    // 0x100179308
    void RebuildModel();                                            // 0x1001794e4
    void CreateCrane(float x, float z, int modelIndex);             // 0x100179e50
    void AddToRenderer();                                           // 0x10017a218
    void RepositionSparks();                                        // 0x10017a342
    void SetBuildingCompletion(const SC4Percentage& completion);    // 0x10017a63c
    int SetScaffoldHeight(float height);                            // 0x10017a710
    void AnimateCrane(std::uint32_t elapsed);                       // 0x10017aa36
    void SetCraneYaw(float yaw);                                    // 0x10017abe0
    void DivideLot(cS3DBoundingBox* boxes);                         // 0x10017b102  (8 rectangles)
    int AttemptAddCrane(cS3DBoundingBox* boxes);                    // 0x10017b33c
    void AddGroundProps(cS3DBoundingBox* boxes);                    // 0x10017b3c2
    void CreateConstructionCrew(cS3DBoundingBox* boxes, cS3DBoundingBox& roof);   // 0x10017b708
    void AddRoofProps(cS3DBoundingBox& roof);                       // 0x10017b964
    void CreateDustClouds();                                        // 0x10017bb84
    void CreateSparks();                                            // 0x10017bcee
    void CreateLotTexture();                                        // 0x10017be96
    void DoRotationChange(int arg);                                 // 0x10017c93e
    int AttemptAddOuterCrane(cS3DBoundingBox* boxes);               // 0x10017cc7e
    int AttemptAddInnerCrane();                                     // 0x10017d204
    void RemoveRectangle(cS3DBoundingBox& box, const cS3DBoundingBox& removed);   // 0x10017d4e4
    bool CreateAndPlaceProp(cS3DBoundingBox& box, std::uint32_t index, IConstructionProp** out, bool ground);
    void CreateAutomata(cS3DBoundingBox& box, bool roof, std::uint32_t automataType);   // 0x10017d9f6

    // Accessors for the test and the seam tests.
    std::uint32_t Stage() const { return stage_; }
    std::uint32_t SegmentCount() const { return segmentCount_; }
    std::uint32_t CurrentSegment() const { return currentSegment_; }
    float ScaffoldHeight() const { return scaffoldHeight_; }
    float BuildingBaseY() const { return buildingBaseY_; }
    bool HasCrane() const { return crane_ != nullptr; }
    std::uint8_t CraneType() const { return craneType_; }
    float CraneYawMin() const { return craneYawMin_; }
    float CraneYawMax() const { return craneYawMax_; }
    IConstructionLot* Lot() const { return lot_; }
    cISC4BuildingOccupant* Building() const { return building_; }
    const cS3DBoundingBox& Bounds() const { return bounds_; }
    const cS3DBoundingBox& RoofBounds() const { return roofBounds_; }
    const cS3DTransform& Transform() const { return transform_; }
    const std::vector<ScaffoldVertex>& SideVertices(std::size_t side) const { return sides_[side]; }
    const std::vector<ScaffoldVertex>& Planks() const { return planks_; }
    const std::vector<ScaffoldVertex>& Girders() const { return girders_; }
    const std::vector<PostVertex>& Posts() const { return posts_; }
    const std::vector<PostVertex>& CranePosts() const { return cranePosts_; }
    const std::list<IConstructionProp*>& GroundProps() const { return groundProps_; }
    const std::list<IConstructionProp*>& RoofProps() const { return roofProps_; }
    const std::vector<IConstructionEffect*>& Sparks() const { return sparks_; }
    IConstructionEffect* Dust() const { return dust_; }
    std::size_t CrewCount() const { return crews_.size(); }
    const std::list<CrewData>& Crews() const { return crews_; }
    std::uint32_t GroundPropDuration() const { return firstStageDuration_; }
    std::uint32_t SegmentDuration() const { return segmentDuration_; }
    std::uint32_t RemovalDuration() const { return scaffoldRemovalDuration_; }
    std::uint32_t InteriorDuration() const { return interiorDuration_; }
    bool RotationChanged() const { return rotationChanged_; }

private:
    void CreateCranePosts(float x, float z);
    std::uint32_t craneRandomBelow(std::uint32_t range) const;
    void AddPropToList(std::list<IConstructionProp*>& list, IConstructionProp* prop);
    void ClearPropList(std::list<IConstructionProp*>& list);
    void ReleaseEffects();

    IConstructionLot* lot_ = nullptr;                       // +0xa8
    cISC4BuildingOccupant* building_ = nullptr;             // +0xb0
    cS3DTransform transform_;                               // +0xb8
    cS3DBoundingBox bounds_{{1, 1, 1}, {0, 0, 0}};          // +0xf0
    cS3DBoundingBox roofBounds_{{1, 1, 1}, {0, 0, 0}};      // +0x108
    bool hasCrane_ = false;                                 // +0x120
    bool needsRebuild_ = false;                             // +0x121
    std::vector<ScaffoldVertex> sides_[4];                  // +0x128
    std::vector<ScaffoldVertex> planks_;                    // +0x148
    std::vector<ScaffoldVertex> girders_;                   // +0x150
    std::vector<PostVertex> posts_;                         // +0x158
    cSC4PackedRenderState renderState_;                     // +0x160
    bool rotationChanged_ = false;                          // +0x168
    s3d::cIS3DModelInstance* crane_ = nullptr;              // +0x170
    float cranePosition_[3] = {0, 0, 0};                    // +0x178
    std::vector<PostVertex> cranePosts_;                    // +0x188
    std::vector<std::vector<IConstructionTexture*>> textures_;   // +0x190
    std::uint8_t craneType_ = 0;                            // +0x1a8
    std::list<IConstructionProp*> groundProps_;             // +0x1b0
    std::list<IConstructionProp*> roofProps_;               // +0x1c8
    std::vector<IConstructionEffect*> sparks_;              // +0x1e0
    IConstructionEffect* dust_ = nullptr;                   // +0x1f8
    std::uint32_t stage_ = 0;                               // +0x200
    std::uint32_t stageStartDay_ = 0;                       // +0x204
    std::uint32_t saved208_ = 0;                            // +0x208
    std::uint32_t nextStageDay_ = 0;                        // +0x20c
    std::uint32_t segmentCount_ = 0;                        // +0x210
    std::uint32_t currentSegment_ = 0;                      // +0x214
    float segmentsHeight_ = 0;                              // +0x218
    float buildingBaseY_ = 0;                               // +0x21c
    std::uint32_t firstStageDuration_ = 0;                  // +0x220
    std::uint32_t segmentDuration_ = 0;                     // +0x224
    std::uint32_t scaffoldRemovalDuration_ = 0;             // +0x228
    std::uint32_t interiorDuration_ = 0;                    // +0x22c
    std::uint32_t animTime_ = 0;                            // +0x230
    std::uint32_t craneState_ = 0;                          // +0x234
    float craneYawMin_ = 0;                                 // +0x238
    float craneYawMax_ = 0;                                 // +0x23c
    bool saved240_ = false;                                 // +0x240
    std::uint32_t craneRotTime_ = 0;                        // +0x244
    std::uint32_t craneRotDuration_ = 0;                    // +0x248
    float craneYawTarget_ = 0;                              // +0x24c
    float craneYawStart_ = 0;                               // +0x250
    std::list<CrewData> crews_;                             // +0x258
    float scaffoldHeight_ = 0;                              // the last SetScaffoldHeight value (the original stores
                                                            // it in +0x118, inside the roof bounds)
};

}  // namespace sc4
