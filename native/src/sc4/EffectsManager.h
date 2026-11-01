// cSC4EffectsManager — the owner of the visual-effects system: the description library (a cSC4EffectsCollection
// sub-object at +0xd0), the effect-name / (group,type) -> description registries, the pool of live cSC4VisualEffect
// objects with its active / new / free slot lists, the particle-system pools, the screen-flash and lighting-tint
// state, the snowline and the per-frame tick / message plumbing.
//
// Original: 97 symbols at 0x1005dacf0..0x1005e4b4a (81 in the class range; the GZCOM factory pair at
// 0x1005dacf0 / 0x1005dad32 is already recovered in src/gz/GZCOM.cpp, and the rest are libc++ container
// instantiations). Object size 0x10f8 bytes. The class is a six-vtable multiple-inheritance object:
//   +0x000 primary vtable 0x100ad3ba0 (cISC4EffectsManager): QueryInterface, AddRef, Release, Init, Shutdown,
//          HasVisualEffect, CreateVisualEffect(name), CreateVisualEffect(group,type), GetStatsString, GetEffectMaps,
//          GetTallestBuildingAltitude, destructors, FinalRelease, DoMessage, Update, Pick, BeginCollection,
//          EndCollection (the slot map is recorded in scripts/vtable_ghidra.py 0x100ad3ba0)
//   +0x008 vtable 0x100ad3c48: QueryInterface / AddRef / Release / Update / Pick (the "renderable" half; the port
//          has a single vtable)
//   +0x010 vtable 0x100ad3c80: QueryInterface / AddRef / Release / DoMessage (the message-listener half)
//   +0x018 cSC4EffectMaps sub-object (0xb8 bytes) — a second, embedded map object, distinct from +0x1060
//   +0x020 vtable 0x100ad3d30 (dtor / RemoveRef / RefCount)
//   +0x0d0 cSC4EffectsCollection sub-object (0x220 bytes)
// The interface ids (QueryInterface 0x1005dd616): 1 and 0xc97cf5cd -> the manager object, 0x452294aa -> the +0x10
// sub-object (same address in the port), 0xc97cf5ce -> the collection at +0xd0, 0xa5eaa1a -> the effect maps at
// +0x1060 (forwarded to their own QueryInterface).
//
// Field map (original offsets; the port keeps the names/roles, not the byte layout):
//   +0x018 cSC4EffectMaps            +0x0d0 cSC4EffectsCollection
//   +0x2d8 std::string               lowercase scratch shared by HasVisualEffect / CreateVisualEffect
//   +0x2f0 u16 initialised           +0x2f2 u8 suppress occupant effects
//   +0x2f4 u32 reference count       +0x2f8 u8 "killing orphaned effects" guard
//   +0x2fc u32 animation tick count  +0x300 f32 elapsed seconds (sum of the ticks)
//   +0x308 cRZRandomSuper (seed 0x1105, the effect-spawn random used by UpdateVisualEffect)
//   +0xcd8 weather simulator (AddRef'd)   +0xce0 u8 force description update (a resource reload is in progress)
//   +0xce4 u32 maximum effect description version (5)   +0xce8 cRZAutoRefCount<cSC4VisualEffect> pool
//   +0xd00 new-slot list   +0xd18 active-slot list   +0xd30 free-slot list
//   +0xd48 decal pool      +0xd60 new decals   +0xd78 free decals
//   +0xd90 particle pool   +0xda8 new particles +0xdc0 free particles
//   +0xdd8 dynamic-particle pool +0xdf0 new +0xe08 free
//   +0xe20 model-based-particle pool +0xe38 new +0xe50 free
//   +0xe68 geometry-sourced-particle pool +0xe80 new +0xe98 free
//   +0xeb0 effects file parser (nSCRes::cFileParser, iid 0x1cf94583, AddRef'd)   +0xeb8 effects parser
//          (cSC4EffectsParser, AddRef'd)
//   +0xec0 zoom (-1)   +0xec4 LOD level   +0xec8 previous LOD level   +0xecc LOD offset
//   +0xed0 particles drawn / frame   +0xed4 active visual effects   +0xed8 particle systems drawn
//   +0xedc decals drawn   +0xee0 cSC4GlobalEffectParams {rateScale / "density", sizeScale, wind xyz}
//   +0xef8 std::vector<float> LOD distance keys (copied out of the camera parameters by EndCollection)
//   +0xf10 / +0xf18 the four camera-parameter values   +0xf20 LOD scale   +0xf24 world units per viewport unit
//   +0xf28 cRZCallbackTimer (the 1000 ms effects tick)
//   +0xf60 std::unordered_map<cEffectKey,int>  effect bindings (group,type) -> effect index
//   +0xf88 std::unordered_map<u32,int>         message triggers -> message-trigger record index
//   +0xfb0 screen flash: colours / +0xfc8 intensities / +0xfe0 duration (2.0) / +0xfe8 time / +0xfec 1/duration
//   +0xff0 "flash at a point" / +0xff4 point / +0xffc point radius scale
//   +0x1000 / +0x1008 the two packed render states CreateFlashMaterials builds
//   +0x1010 lighting tint: colours / +0x1028 intensities / +0x1040 duration (2.0) / +0x1048 time / +0x104c 1/duration
//   +0x1050 atmospheric effect   +0x1058 water effect   +0x1060 effect maps (AddRef'd)   +0x1068 draw terrain maps
//   +0x106c tallest building altitude   +0x1070 particle draw pairs   +0x1088 culled particles
//   +0x10a0 snowline effect id (-1)   +0x10a4 current snowline altitude   +0x10a8 / +0x10ac the two seasonal
//   altitudes (300 / 600)   +0x10b0 the altitude bias passed to the terrain view (200)
//   +0x10b8 cheat-effect pool   +0x10d0 heap-check cheat flag
//   +0x10d8 renderer (AddRef'd)   +0x10e0 / +0x10e4 half the map extent in x / z
//   +0x10e8 vertex-buffer extension (AddRef'd)   +0x10f0 the thread that called Init
//
// Every reach into an unrecovered neighbour (renderer, message server, cheat manager, game-data registry, effect
// resource / parser, terrain view, lighting manager, view utilities, weather sim, occupant manager) is one
// IEffectsManagerHost method with the original vtable slot or address in a comment.
//
// Quirks kept (each asserted by scripts/tests/sc4_effectsmanager_test.cpp):
//  * Release() never decrements the reference count, it always returns 1 (0x1005dd746); only FinalRelease actually
//    shuts down and deletes the object (0x1005dd78e calls vslot 0x20 = Shutdown, then vslot 0x60 = the deleting
//    destructor). AddRef only bumps the counter.
//  * CreateVisualEffect(name) resolves the name through an unordered_multimap range and picks the entry with the
//    highest description version that is > 0 and <= the manager's maximum version (5); ties keep the first entry the
//    hash range yields - i.e. the choice is not stable between names that share a version.
//  * CreateVisualEffect(name) has a redundant second lookup of the same key on the empty-range path; it can never
//    succeed, so an unknown or empty name always fails (0x1005e0857 / 0x1005e08dd).
//  * CreateVisualEffect(group,type) iterates only the single hash node the binding lookup found (the loop compares
//    against that node's next pointer), and it repeats the failed lookup on the miss path; a binding whose effect
//    description has version 0 or > 5 yields a live effect with slot -1 (no description).
//  * EndCollection keeps the *first* effect index registered for a given (group,type) key: an existing key is not
//    overwritten when a later binding resolves to it (0x1005e11b5).
//  * BeginCollection unregisters the currently registered message-trigger handlers before clearing the trigger map,
//    and EndCollection registers the new ones; the trigger map is index -> message-trigger record, and EndCollection
//    overwrites the index of a duplicate message id.
//  * NewVisualEffect pops the free-slot list from the back and pushes the reused index onto the new list; it returns
//    the pooled object *without* taking another reference.
//  * DeactivateUnusedVisualEffects counts the effects that are still running, not the ones it keeps: an effect that
//    has stopped but is still referenced by somebody else is kept in the active list without being counted.
//  * Recycle clears the effect's name, group, type and slot and releases its components, but the pooled object keeps
//    its reference: the free list is a slot list, not a reference list.
//  * KillOrphanedEffects stops (transition 1) every active effect whose reference count is exactly 1, and it sets the
//    "killing" guard so ActivateNewVisualEffects does nothing while it runs.
//  * GetStatsString's labels do not match the fields it prints: the "particle systems" line adds the geometry /
//    model-based free lists to the particle *new* list and vice versa (0x1005e1318).
//  * UpdateLODScale's LOD fraction is computed with signed integer division of (level - flagA) by (flagB - flagA - 1)
//    and is not clamped; when flagA >= flagB - 1 the first key is used.
//  * StartAncilliary computes the screen-space position with the effect transform's row 0 for x but its column 2 for
//    y (0x1005e1464); the event type bits are 1 = shake, 2 = "has a screen position", 4 = screen flash, 8 = tint.
//  * StartScreenFlash / StartLightingTint always copy the light description's curves (the original's inlined
//    self-assignment guard compares the vector member's address with the description, which can never match), then
//    set duration / inverse duration and clear the elapsed time.
//  * SetSnowlineState(true) allocates the terrain snowline effect on first use and remembers its id; the snowline
//    curve is cos(dayOfYear / 364 * 2pi) between the two seasonal altitudes, mirrored by half a year in the southern
//    hemisphere, and the terrain update only happens when the altitude moves by more than 8 units.
//  * DoSnowCheat with no arguments toggles (enable only when nothing is active); with 1-3 arguments it writes the two
//    seasonal altitudes / the bias and always enables.
//  * Init is a no-op the second time (the u16 flag), and its effect-tick callback ignores the user pointer: the
//    free 0x1005dc8c9 posts a 0x697d01ed message through the message server instead.
//  * The cSC4GlobalEffectParams block the particle effects receive starts at +0xee0 and is only 0x18 bytes long: its
//    maxAlpha field (+0x44) would land on +0xf24, the "world units per viewport unit" value DoZoomChanged writes, so
//    the port copies that value into globalParams_.maxAlpha whenever it recomputes it.
//  * FinalRelease is the only path that destroys the object (Release never does); the destructor releases the
//    renderer / weather / vertex-buffer / resource / parser references but deliberately leaves the effect maps at
//    +0x1060 and the atmospheric / water effects at +0x1050 / +0x1058 alone (Shutdown releases those).
#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZCallbackTimer.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZString.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/EffectDescriptions.h"
#include "sc4/EffectMaps.h"
#include "sc4/EffectsCollection.h"
#include "sc4/ModelBasedParticlesEffect.h"
#include "sc4/PackedRenderState.h"
#include "sc4/ParticlesEffect.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/VisualEffect.h"

class cIGZUnknown;
class cIGZCommandParameterSet;
class SC4DrawContext;   // sc4/DrawContext.h (global, like the renderer's)

namespace sc4 {

class cSC4EffectsManager;
class cISC4GameDataRegistry;   // sc4/GameDataRegistry.h (the game-data registry lives in this namespace)

constexpr std::uint32_t kGZIID_cISC4EffectsManager = 0x452294aa;    // +0x10 sub-object (0x1005dd616)
constexpr std::uint32_t kGZIID_cISC4EffectsManagerAlt = 0xc97cf5cd; // the manager object itself
constexpr std::uint32_t kGZIID_cISC4EffectsCollection = 0xc97cf5ce; // the collection at +0xd0
constexpr std::uint32_t kGZCLSID_cSC4EffectsManager = 0x00000000;   // read from GZCOM.cpp, see that file
// Effect-tick plumbing (0x1005dba0e / 0x1005dc8c9): the callback timer posts this message every 1000 ms.
constexpr std::uint32_t kEffectsTickMessage = 0x697d01ed;
// ParseEffectsFiles posts this message after a successful parse (0x1005e32b4; it is one of the ids Init registers).
constexpr std::uint32_t kEffectsParsedMessage = 0xc9f775bb;
// Message types Init registers on the message server (in the original's order).
constexpr std::uint32_t kEffectsMessageIds[] = {
    0xa6b79620, 0xa6b79602, 0x697d01ed, 0x2acd2cf3, 0x66956814, 0x26d31ec1,
    0xab96b05f, 0x26d31ec2, 0xc9f775bb, 0x99ef1142, 0x99ef1143,
};
// The three cheat codes (0x1005dba0e): effect console, snow, heap check.
constexpr std::uint32_t kCheatEffectCode = 0x4921ff8e;
constexpr std::uint32_t kCheatSnowCode = 0x4921ff8d;
constexpr std::uint32_t kCheatHeapCheckCode = 0x4921ff8f;

// The neighbours the recovered functions reach. Every method mirrors one original call; the comment carries the
// object, vtable slot and/or address. A game-side adapter implements it (SL::sp* in the original); the test installs
// a fake.
class IEffectsManagerHost {
public:
    virtual ~IEffectsManagerHost() = default;

    // ---- Init / Shutdown ---------------------------------------------------------------------------------------
    virtual void SendProgressMessage(int stage, const char* text) = 0;               // SC4SendProgressMessage(stage, str)
    virtual std::uint32_t CurrentThreadId() = 0;                                    // cRZThread::GetCurrentThreadId (+0x10f0)
    virtual float TerrainCellSize() = 0;                                            // SL::spTerrainMap +0x30
    virtual int TerrainCellsX() = 0;                                                // SL::spTerrainMap +0x38
    virtual int TerrainCellsZ() = 0;                                                // SL::spTerrainMap +0x40
    virtual cIGZUnknown* Renderer() = 0;                                            // SL::spRenderer (AddRef'd)
    virtual void AttachRenderable(cIGZUnknown* renderable) = 0;                     // renderer vslot 0x100(renderable,5,0x76c)
    virtual void DetachRenderable(cIGZUnknown* renderable) = 0;                     // renderer vslot 0x108(renderable)
    virtual IParticlesVertexBuffer* VertexBufferExtension() = 0;                    // renderer device QI 0x9cd86f9 (AddRef'd)
    virtual cIGZUnknown* WeatherSim() = 0;                                          // SL::spWeatherSim (AddRef'd)
    virtual cSC4EffectMaps* CreateEffectMaps() = 0;                                 // new cSC4EffectMaps + Init
    virtual cIGZUnknown* CreateEffectsResource() = 0;                               // GZCOM 0x5cf9459a/0x1cf94583, vslot 0x18
    virtual void ShutdownEffectsResource(cIGZUnknown* resource) = 0;                 // resource vslot 0x20(1)
    virtual bool EffectsResourceChanged() = 0;                                      // resource vslot 0x70
    virtual void EffectsResourceReloaded() = 0;                                     // resource vslot 0x90
    virtual cIGZUnknown* CreateEffectsParser() = 0;                                 // GZCOM 0xa956ad14/0x6956babc
    virtual void LoadEffectsFiles() = 0;                                            // cSC4EffectsManager::LoadAllEffects (PARTIAL)
    virtual void RegisterMessageHandler(std::uint32_t message) = 0;                 // SL::spMS2 vslot 0x28(manager, message)
    virtual void UnregisterMessageHandler(std::uint32_t message) = 0;               // SL::spMS2 vslot 0x30(manager, message)
    virtual void AddCheatHandler(cIGZUnknown* listener) = 0;                         // SL::spCCM vslot 0x38(listener, 0)
    virtual void RemoveCheatHandler(cIGZUnknown* listener) = 0;                      // SL::spCCM vslot 0x40(listener, 0)
    virtual void RegisterCheatCode(std::uint32_t id, const char* text) = 0;         // SL::spCCM vslot 0x18 / 0x38
    virtual void UnregisterCheatCode(std::uint32_t id) = 0;                         // SL::spCCM vslot 0x20(id)
    virtual bool CheatsEnabled() = 0;                                               // SL::spSC4 vslot 0xa8
    virtual void AddTickNotification(cIGZUnknown* listener) = 0;                     // SL::ATM vslot 0x28(listener, 0xc0)
    virtual void RemoveTickNotification(cIGZUnknown* listener) = 0;                  // SL::ATM vslot 0x30
    virtual void RegisterGameDataFunctions() = 0;                                   // GameDataRegistry AddNode "effects" + AddValue
    virtual void UnregisterGameDataFunctions() = 0;                                 // the same registry entries removal
    // SL::spMS2 vslot 0x20 (MessagePost): a cRZMessage2Standard of this type, posted with `false`. Both the effects
    // tick callback (0x1005dc8c9) and ParseEffectsFiles (0x1005e32ca) build that message inline in the original.
    virtual void PostMessage(std::uint32_t type) = 0;
    // LoadAllEffects' resource-manager half: the bytes of every packed effects library (type 0xea5118b0), the base
    // game's instance 1 of group 0xea5118b1 first, then the others the resource manager lists.
    virtual void PackedEffectsResources(std::vector<std::vector<std::uint8_t>>& out) { out.clear(); }
    // Update's draw half: the particle view of the renderer's draw context (null = simulate without drawing) and the
    // view-frustum test of a particle system's bounds (SC4ViewUtilities::IsBoundingBoxInViewFrustum).
    virtual IParticlesDrawContext* ParticlesDrawContext(SC4DrawContext* context) { (void)context; return nullptr; }
    virtual bool IsBoundingBoxInView(const cS3DBoundingBox& box) { (void)box; return true; }

    // ---- effect files (0x1005e324a ParseEffectsFiles) ----------------------------------------------------------
    // The file parser at +0xeb0 is the nSCRes::cFileParser GZCOM object (clsid 0x5cf9459a, iid 0x1cf94583). Its two
    // calls are separate host methods so the port never reads that object's vtable directly.
    virtual bool EffectsResourceParseFiles() = 0;                                   // parser vslot 0x38 ParseFiles()
    virtual const char* EffectsResourceErrorString() = 0;                           // parser vslot 0x50 ErrorString()
    virtual void SetView3DErrorReportString(const char* text) = 0;                  // SL::spView3D vslot 0x168 (SetErrorReportString)

    // ---- zoom / render properties ------------------------------------------------------------------------------
    virtual int ViewZoom() = 0;                                                     // SL::spViewUtilities +0x14
    virtual float WorldUnitsPerViewportUnit() = 0;                                  // SL::spViewUtilities +0x158
    virtual float RenderHeightScale() = 0;                                          // SL::spRenderProperties+0x48 -> +0x350

    // ---- screen effects ----------------------------------------------------------------------------------------
    virtual std::uint32_t AddShake(const cSC4ShakeDescription& shake, const cS3DVector2* screenPosition) = 0;  // renderer 0x158
    virtual void RemoveShake(std::uint32_t handle) = 0;                             // renderer vslot 0x160
    virtual void SetLightingTint(float intensity, const cS3DVector3& colour) = 0;   // SL::spLightingManager vslot 0x38
    virtual void ClearLightingTint() = 0;                                           // SL::spLightingManager vslot 0x40

    // ---- snowline ----------------------------------------------------------------------------------------------
    virtual std::int32_t AddSnowlineEffect() = 0;                                   // terrain view vslot 0x40(0x7f94207)
    virtual void RemoveSnowlineEffect(std::int32_t handle) = 0;                     // terrain view vslot 0x78
    virtual void SetSnowlineAltitude(std::int32_t handle, float altitude, float bias) = 0;   // terrain view vslot 0xa8
    virtual int SimulatorDayOfYear() = 0;                                           // SL::spSimulator vslot 0x30 -> +0x70
    virtual int CityHemisphere() = 0;                                               // SL::spCity vslot 0xf0
    virtual bool WeatherEnabled() = 0;                                              // weather sim vslot 0x70
    virtual bool ViewportPositionToRay(int x, int y, float* origin, float* direction) = 0;   // SC4ViewUtilities::ViewportPositionToRay
    virtual float SimulatorSpeed() = 0;                                             // SL::spSimulator vslot 0x100
    virtual void WeatherWind(float& x, float& z) = 0;                               // weather sim vslot 0xb0 (u64 payload)
};

// ---- process-wide instances (the original's SL::spEffectsManager at 0x100b21368 and the host) ---------------------
IEffectsManagerHost* EffectsManagerHost();
void SetEffectsManagerHost(IEffectsManagerHost* host);          // not owning; null disables Init / Shutdown
cSC4EffectsManager* EffectsManager();
void SetEffectsManager(cSC4EffectsManager* manager);            // not owning

// The effect class key the binding registry uses (cSC4EffectsCollection::cEffectKey; the original hashes id ^ value).
struct EffectKeyHash {
    std::size_t operator()(const cSC4EffectsCollection::cEffectKey& key) const noexcept {
        return static_cast<std::size_t>(key.id) ^ static_cast<std::size_t>(key.value);
    }
};
struct EffectKeyEqual {
    bool operator()(const cSC4EffectsCollection::cEffectKey& a,
                    const cSC4EffectsCollection::cEffectKey& b) const noexcept {
        return a.id == b.id && a.value == b.value;
    }
};

class cSC4EffectsManager final : public cIGZUnknown, public IPropVisualEffectHost {
public:
    // cSC4VisualEffect holds an IEffectsManagerLink (void AddRef / Release, StartAncilliary, StopAncilliary), which
    // cannot be cIGZUnknown's uint32_t trio on the same object; the manager therefore hands every effect this adapter.
    class ManagerLink final : public IEffectsManagerLink {
    public:
        explicit ManagerLink(cSC4EffectsManager* owner) : owner_(owner) {}
        void AddRef() override;
        void Release() override;
        void StartAncilliary(cSC4VisualEffect* effect) override;
        void StopAncilliary(cSC4VisualEffect* effect) override;

    private:
        cSC4EffectsManager* owner_;
    };

    cSC4EffectsManager();                                                   // @ 0x1005dad3e
    ~cSC4EffectsManager() override;
    // The host the recovered functions reached through SL::sp* / GZCOM in the original; defaults to the process-wide
    // EffectsManagerHost() and may be replaced before Init.
    void SetHost(IEffectsManagerHost* host) { host_ = host; }
    IEffectsManagerHost* Host() const { return host_; }                                         // @ 0x1005db26c (D2), deleting variant 0x1005db9b2
    cSC4EffectsManager(const cSC4EffectsManager&) = delete;
    cSC4EffectsManager& operator=(const cSC4EffectsManager&) = delete;

    // ---- cIGZUnknown (primary vtable +0x00 / +0x08 / +0x10) ----------------------------------------------------
    bool QueryInterface(std::uint32_t iid, void** out) override;            // @ 0x1005dd616
    std::uint32_t AddRef() override;                                        // @ 0x1005dd6ce
    std::uint32_t Release() override;                                       // @ 0x1005dd746 (always 1)
    void FinalRelease();                                                    // @ 0x1005dd78e (Shutdown + delete)

    // ---- cISC4EffectsManager (primary vtable +0x18 .. +0x90) ---------------------------------------------------
    bool Init();                                                            // @ 0x1005dba0e (PARTIAL)
    bool Shutdown();                                                        // @ 0x1005dcd62 (PARTIAL)
    bool HasVisualEffect(const char* name) const;                           // @ 0x1005e06ce
    bool CreateVisualEffect(const char* name, cSC4VisualEffect** out);      // @ 0x1005e075c (vslot 0x30)
    bool CreateVisualEffect(std::uint32_t group, std::uint32_t type,
                           cSC4VisualEffect** out);                         // @ 0x1005e0c08 (vslot 0x38)
    void GetStatsString(cRZString& out);                                    // @ 0x1005e1318 (vslot 0x40)
    cSC4EffectMaps* GetEffectMaps() const { return effectMaps_; }           // @ 0x1005e341a (vslot 0x48)
    float GetTallestBuildingAltitude() const { return tallestBuildingAltitude_; }   // @ 0x1005e3428 (vslot 0x50)
    bool DoMessage(cIGZMessage2* message);                                  // @ 0x1005dd7b8 (PARTIAL, vslot 0x70)
    bool Update(SC4DrawContext* context);                                   // @ 0x1005df48a (PARTIAL, vslot 0x78)
    bool Pick(const cS3DVector3& origin, const cS3DVector3& direction, SC4DrawContext* context,
              float* distance);                                            // @ 0x1005e06be (always false, vslot 0x80)
    void BeginCollection();                                                 // @ 0x1005e0d32 (vslot 0x88)
    void EndCollection();                                                   // @ 0x1005e0dea (vslot 0x90)

    // ---- the IEffectsManagerLink side (what a started cSC4VisualEffect calls back into, via ManagerLink) --------
    void StartAncilliary(cSC4VisualEffect* effect);                         // @ 0x1005e1464
    void StopAncilliary(cSC4VisualEffect* effect);                          // @ 0x1005e174a

    // ---- IPropVisualEffectHost (the prop occupant's seam; vslots 0x30 / 0x38 of the manager) --------------------
    bool CreateEffectByName(const char* name, cSC4VisualEffect** out) override {
        return CreateVisualEffect(name, out);
    }
    bool CreateEffectByType(std::uint32_t groupId, std::uint32_t typeId, cSC4VisualEffect** out) override {
        return CreateVisualEffect(groupId, typeId, out);
    }

    // ---- effect factory + active-list machinery ----------------------------------------------------------------
    cSC4VisualEffect* NewVisualEffect();                                    // @ 0x1005e099c
    void ActivateNewVisualEffects();                                        // @ 0x1005e0b2e
    void UpdateActiveVisualEffects();                                       // @ 0x1005de2b6
    void DeactivateUnusedVisualEffects();                                   // @ 0x1005e0102
    void KillOrphanedEffects();                                             // @ 0x1005e30f8
    void DoRotationChanged();                                               // @ 0x1005de004
    void DoZoomChanged(int zoom);                                           // @ 0x1005ddf00
    void UpdateVisualEffect(cSC4VisualEffect* effect, bool forceUpdate);    // @ 0x1005e1850 (PARTIAL)
    void UpdateLODScale(const cSC4EffectDescription::cDescriptionRec& record,
                        cSC4VisualEffect::cComponentRec* component);        // @ 0x1005e21f8

    // ---- ancilliary effects (shakes / flashes / lighting tints) -------------------------------------------------
    void StartScreenFlash(const cSC4LightDescription& light, cS3DVector2* screenPosition, float scale);  // @ 0x1005e162e
    void StartLightingTint(const cSC4LightDescription& light);              // @ 0x1005e16de
    void UpdateLightingTint(float elapsed);                                 // @ 0x1005dff0a
    void DrawScreenFlashes(SC4DrawContext* context, float elapsed);         // @ 0x1005dfb64 (PARTIAL)

    // ---- particles ---------------------------------------------------------------------------------------------
    // The four other factories (model-based 0x1005e279c, geometry-sourced 0x1005e2908, dynamic 0x1005e2c12 and
    // decal 0x1005e2d80) are SKIPPED: their classes are not recovered, so they are not declared here.
    cSC4ParticlesEffect* NewParticlesEffect();                              // @ 0x1005e2a74
    cSC4ModelBasedParticlesEffect* NewModelBasedParticlesEffect();          // @ i386 0x00405ed8
    // LoadAllEffects (i386 0x00409e36): BeginCollection, every packed effects library into the collection,
    // EndCollection. The plug-in "main.fx" text path is not ported.
    void LoadAllEffects();

    // ---- effect files and the game-data registry command --------------------------------------------------------
    // The files the parser at +0xeb0 was given (LoadAllEffects adds "main.fx" / "Effects/config.fx"): a successful
    // parse clears the 3D view's error report and posts kEffectsParsedMessage, a failed one reports the parser's
    // error string there instead.
    void ParseEffectsFiles();                                               // @ 0x1005e324a
    // The registry's "create_effect" command (registered by Init's RegisterGameDataFunctions). `handle` is the
    // manager as it was registered with the registry; parameters 0..3 are the effect name and the source position.
    // It is a plain GameDataCommandFn, so it is static and finds the manager through the registry.
    static bool sCreateEffect(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out,
                              cISC4GameDataRegistry* registry);             // @ 0x1005dcb24

    // ---- snowline / ticks / cheats -----------------------------------------------------------------------------
    void SetSnowlineState(bool enabled);                                    // @ 0x1005e31cc
    void UpdateSnowline();                                                  // @ 0x1005de13c
    void DoEffectsTick();                                                   // @ 0x1005de054
    void DoAnimationTick(std::uint32_t elapsedMs);                          // @ 0x1005ddd1c (PARTIAL)
    bool DoSnowCheat(cRZCmdLine& commandLine);                              // @ 0x1005de8d4
    void DoRenderPropertiesChanged();                                       // @ 0x1005de30a (PARTIAL)
    void DoOccupantInserted(cISC4Occupant* occupant);                       // @ 0x1005de68c (PARTIAL)
    void DoOccupantRemoved(cISC4Occupant* occupant);                        // @ 0x1005de762 (PARTIAL)
    void CreateFlashMaterials();                                            // @ 0x1005dc2a2
    void CreateComponentFromDescription(const cSC4EffectDescription::cDescriptionRec& record,
                                        cSC4VisualEffect::cComponentRec* component,
                                        cSC4ModelBasedParticlesEffect** chained);   // @ i386 0x00406246 (PARTIAL)

    // ---- the description library (the +0xd0 sub-object) --------------------------------------------------------
    cSC4EffectsCollection& Collection() { return collection_; }
    const cSC4EffectsCollection& Collection() const { return collection_; }
    const cSC4EffectMaps& EmbeddedMaps() const { return embeddedMaps_; }    // +0x18

    // ---- inspection (the original's fields; used by the test and by other recovered classes) --------------------
    bool Initialised() const { return initialised_ != 0; }
    std::uint32_t RefCount() const { return refCount_; }
    bool EffectsActive() const { return effectsActive_; }
    void SetEffectsActive(bool value) { effectsActive_ = value; }
    bool OccupantEffectsSuppressed() const { return suppressOccupantEffects_; }
    bool ForceDescriptionUpdate() const { return forceUpdate_; }
    std::uint32_t MaxEffectVersion() const { return maxEffectVersion_; }
    void SetMaxEffectVersion(std::uint32_t version) { maxEffectVersion_ = version; }
    const std::vector<cRZAutoRefCount<cSC4VisualEffect>>& VisualEffects() const { return visualEffects_; }
    const std::vector<int>& ActiveVisualEffectSlots() const { return activeVisualEffectSlots_; }
    const std::vector<int>& NewVisualEffectSlots() const { return newVisualEffectSlots_; }
    const std::vector<int>& FreeVisualEffectSlots() const { return freeVisualEffectSlots_; }
    const std::vector<cRZAutoRefCount<cSC4ParticlesEffect>>& ParticleEffects() const { return particlesEffects_; }
    const std::vector<int>& NewParticlesEffectSlots() const { return newParticlesEffectSlots_; }
    const std::vector<int>& FreeParticlesEffectSlots() const { return freeParticlesEffectSlots_; }
    const std::unordered_map<cSC4EffectsCollection::cEffectKey, int, EffectKeyHash, EffectKeyEqual>& EffectBindings() const {
        return effectBindingsByKey_;
    }
    std::uint32_t ActiveVisualEffectCount() const { return activeVisualEffectCount_; }
    std::uint32_t ParticlesDrawn() const { return particlesDrawn_; }
    std::uint32_t ParticleSystemCount() const { return particleSystemCount_; }
    std::uint32_t DecalCount() const { return decalCount_; }
    float LodScale() const { return lodScale_; }
    float WorldUnitsPerViewportUnit() const { return worldUnitsPerViewportUnit_; }
    float LodRangeValue(int index) const { return lodRange_[index]; }
    int LodLevel() const { return lodLevel_; }
    int PreviousLodLevel() const { return previousLodLevel_; }
    int Zoom() const { return zoom_; }
    const cSC4GlobalEffectParams& GlobalParams() const { return globalParams_; }
    const cSC4PackedRenderState& FlashStateLit() const { return flashStateLit_; }
    const cSC4PackedRenderState& FlashStateDark() const { return flashStateDark_; }
    const std::vector<cS3DVector3>& ScreenFlashColours() const { return flashColours_; }
    const std::vector<float>& ScreenFlashIntensities() const { return flashIntensities_; }
    float ScreenFlashTime() const { return flashTime_; }
    float ScreenFlashInverseDuration() const { return flashInverseDuration_; }
    bool ScreenFlashAtPoint() const { return flashAtPoint_; }
    const cS3DVector2& ScreenFlashPoint() const { return flashPoint_; }
    const std::vector<cS3DVector3>& LightingTintColours() const { return tintColours_; }
    const std::vector<float>& LightingTintIntensities() const { return tintIntensities_; }
    float LightingTintTime() const { return tintTime_; }
    std::int32_t SnowlineEffectId() const { return snowlineEffectId_; }
    float SnowlineAltitude() const { return snowlineCurrent_; }
    float SnowlineLow() const { return snowlineLow_; }
    float SnowlineHigh() const { return snowlineHigh_; }
    float SnowlineBias() const { return snowlineBias_; }
    cIGZUnknown* Renderer() const { return renderer_; }
    IParticlesVertexBuffer* VertexBufferExtension() const { return vertexBufferExtension_; }
    std::uint32_t ThreadId() const { return threadId_; }
    std::uint32_t TickCount() const { return tickCount_; }
    float ElapsedSeconds() const { return elapsedSeconds_; }
    cRZRandomSuper& Random() { return random_; }
    bool HeapCheckEnabled() const { return heapCheck_; }
    void SetHeapCheckEnabled(bool value) { heapCheck_ = value; }
    bool DrawTerrainMaps() const { return drawTerrainMaps_; }
    bool KillGuardCleared() const { return !killing_; }
    void SetLodLevel(int level) { lodLevel_ = level; }
    // Debug census of the model-based pool: systems in use, started, live particles, systems with a model.
    void ModelEffectCensus(int& inUse, int& started, int& particles, int& withModel) const {
        inUse = static_cast<int>(newModelEffectSlots_.size());
        started = particles = withModel = 0;
        for (int index : newModelEffectSlots_) {
            const auto* effect = modelEffects_[static_cast<std::size_t>(index)].get();
            started += effect->IsStarted() ? 1 : 0;
            particles += effect->LiveParticleCount();
            withModel += effect->ModelInstance() ? 1 : 0;
        }
    }   // UpdateVisualEffect's LOD filtering; public for the test

private:
    // The shared lowercase scratch (+0x2d8): the original lowercases in place into one std::string member.
    std::string LowercaseName(const char* name) const;
    // Description selection used by both CreateVisualEffect overloads and UpdateVisualEffect: the highest version
    // that is > 0 and <= maxEffectVersion_, -1 when the name / key has no candidate.
    int ResolveDescriptionByName(const std::string& lowercaseName) const;
    int ResolveDescriptionByKey(std::uint32_t group, std::uint32_t type) const;

    IEffectsManagerHost* host_ = nullptr;               // the installed host (SL::sp* in the original)
    ManagerLink managerLink_{this};                      // the IEffectsManagerLink handed to every effect
    cSC4EffectMaps embeddedMaps_;                       // +0x018
    cSC4EffectsCollection collection_;                  // +0x0d0
    mutable std::string scratchName_;                   // +0x2d8
    std::uint16_t initialised_ = 0;                     // +0x2f0
    bool effectsActive_ = false;                        // +0x2f1 (the 0xab96b05f message sets it)
    bool suppressOccupantEffects_ = false;              // +0x2f2
    std::uint32_t refCount_ = 0;                        // +0x2f4
    bool killing_ = false;                              // +0x2f8
    std::uint32_t tickCount_ = 0;                       // +0x2fc
    float elapsedSeconds_ = 0.0f;                       // +0x300
    cRZRandomSuper random_{0x1105};                     // +0x308 (seed 0x1105)
    cIGZUnknown* weatherSim_ = nullptr;                 // +0xcd8 (one reference)
    bool forceUpdate_ = false;                          // +0xce0
    std::uint32_t maxEffectVersion_ = 5;                // +0xce4

    std::vector<cRZAutoRefCount<cSC4VisualEffect>> visualEffects_;   // +0xce8
    std::vector<int> newVisualEffectSlots_;             // +0xd00
    std::vector<int> activeVisualEffectSlots_;          // +0xd18
    std::vector<int> freeVisualEffectSlots_;            // +0xd30

    std::vector<cRZAutoRefCount<cSC4ParticlesEffect>> particlesEffects_;   // +0xd90
    std::vector<int> newParticlesEffectSlots_;          // +0xda8
    std::vector<int> freeParticlesEffectSlots_;         // +0xdc0
    std::vector<cRZAutoRefCount<cSC4ModelBasedParticlesEffect>> modelEffects_;   // +0xe20
    std::vector<int> newModelEffectSlots_;              // +0xe38
    std::vector<int> freeModelEffectSlots_;             // +0xe50

    cIGZUnknown* effectsResource_ = nullptr;            // +0xeb0 the nSCRes::cFileParser (one reference)
    cIGZUnknown* effectsParser_ = nullptr;              // +0xeb8 (one reference)

    int zoom_ = -1;                                     // +0xec0
    int lodLevel_ = -1;                                 // +0xec4 (-1 until the first zoom message: i386 ctor 0x00407e30)
    int previousLodLevel_ = -1;                         // +0xec8
    int lodOffset_ = 0;                                 // +0xecc
    std::uint32_t particlesDrawn_ = 0;                  // +0xed0
    std::uint32_t activeVisualEffectCount_ = 0;         // +0xed4
    std::uint32_t particleSystemCount_ = 0;             // +0xed8
    std::uint32_t decalCount_ = 0;                      // +0xedc
    cSC4GlobalEffectParams globalParams_;               // +0xee0 (maxAlpha aliases +0xf24 in the original)
    std::vector<float> lodKeys_;                        // +0xef8
    float lodRange_[4] = {0, 0, 0, 0};                  // +0xf10 / +0xf18
    float lodScale_ = 0.0f;                             // +0xf20
    float worldUnitsPerViewportUnit_ = 0.0f;            // +0xf24
    cRZCallbackTimer tickTimer_;                        // +0xf28
    std::unordered_map<cSC4EffectsCollection::cEffectKey, int, EffectKeyHash, EffectKeyEqual>
        effectBindingsByKey_;                           // +0xf60
    std::unordered_map<std::uint32_t, int> messageTriggers_;   // +0xf88

    std::vector<cS3DVector3> flashColours_;             // +0xfb0
    std::vector<float> flashIntensities_;               // +0xfc8
    float flashDuration_ = 2.0f;                        // +0xfe0
    float flashTime_ = 0.0f;                            // +0xfe8
    float flashInverseDuration_ = 0.0f;                 // +0xfec
    bool flashAtPoint_ = false;                         // +0xff0
    cS3DVector2 flashPoint_;                            // +0xff4
    float flashPointScale_ = 0.0f;                      // +0xffc
    cSC4PackedRenderState flashStateLit_;               // +0x1000
    cSC4PackedRenderState flashStateDark_;              // +0x1008

    std::vector<cS3DVector3> tintColours_;              // +0x1010
    std::vector<float> tintIntensities_;                // +0x1028
    float tintDuration_ = 2.0f;                         // +0x1040
    float tintTime_ = 0.0f;                             // +0x1048
    float tintInverseDuration_ = 0.0f;                  // +0x104c

    cSC4VisualEffect* atmosphericEffect_ = nullptr;     // +0x1050 (one reference)
    cSC4VisualEffect* waterEffect_ = nullptr;           // +0x1058 (one reference)
    cSC4EffectMaps* effectMaps_ = nullptr;              // +0x1060 (one reference)
    bool drawTerrainMaps_ = false;                      // +0x1068
    float tallestBuildingAltitude_ = 0.0f;              // +0x106c

    std::int32_t snowlineEffectId_ = -1;                // +0x10a0
    float snowlineCurrent_ = 512.0f;                    // +0x10a4 (the constructor starts it at 512)
    float snowlineLow_ = 300.0f;                        // +0x10a8
    float snowlineHigh_ = 600.0f;                       // +0x10ac
    float snowlineBias_ = 200.0f;                       // +0x10b0

    bool heapCheck_ = false;                            // +0x10d0
    cIGZUnknown* renderer_ = nullptr;                   // +0x10d8 (one reference)
    float halfExtentX_ = 0.0f;                          // +0x10e0
    float halfExtentZ_ = 0.0f;                          // +0x10e4
    IParticlesVertexBuffer* vertexBufferExtension_ = nullptr;   // +0x10e8 (one reference)
    std::uint32_t threadId_ = 0;                        // +0x10f0

    static void TickCallback(void* userData);           // 0x1005dc8c9 EffectsTickCallback (ignores userData)
};

}  // namespace sc4

// The free function the original's effect tick callback is (0x1005dc8c9): it posts kEffectsTickMessage through the
// message server. Declared at global scope because it is a plain function pointer target.
void EffectsTickCallback(void* userData);
