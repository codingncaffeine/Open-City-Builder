// cSC4AudioAmbienceManager — the director of the city's ambient sound: it owns the set of
// cSC4AudioAmbienceLayer objects the current city uses (traffic hum, water, wind, crowds ...), feeds
// them the camera's attention rectangle as the camera and the time of day change, and tracks the
// occupants that carry their own sounds (buildings with a hum, railway crossings, automata, alarms
// that fire at a given hour).
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (60 symbols: 46 functions + 14 non-virtual thunks;
// 0x1003dee26..0x1003ec6a8, primary vtable 0x100aabc90, 0x1a0-byte object, ctor @ 0x1003e9050).
//
// COM identity (four bases, one per vtable; QueryInterface answers 1, 0x6a12f51c and 0x287fb697 with
// the *primary* pointer for all three — the original does not adjust the pointer):
//   +0x00 cIGZSystemService            (primary; service id 0xaa12f911, priority 0)
//   +0x08 cIGZMessageTarget2           (DoMessage; registered on the message server)
//   +0x10 cISC4AudioAmbienceManager    (iid 0x6a12f51c: Init / Shutdown)
//   +0x18 cIGZSndEventHandler          (DoEvent; registered on the sound system)
//
// Object layout as in the binary (member offsets are the original's; the four bases put the first
// member at +0x20 exactly like the original):
//   +0x20 refcount (starts at 0, Release destroys below 2), +0x24 service id, +0x28 running flag,
//   +0x29 a second flag the constructor sets and nothing reads,
//   +0x30 cISC4AudioScapeManager, +0x38 cIGZSndSys, +0x40 cISC4OccupantManager,
//   +0x48 "all occupants" filter used by the tick walk (cSC4AudioAmbienceManagerOccupantFilter),
//   +0x50 std::list<cISC4AudioAmbienceLayer*>, +0x68 zoom level, +0x6c listener *target*,
//   +0x78 listener *eye*, +0x88 std::list<tOccupantAlarm>, +0xa0 ambience-active flag,
//   +0xa8 zoom distances (five floats, tuning 0xa36f410), +0xc0 layer instance ids (tuning 0x2a3ff8cd),
//   +0xd8 attention cell rect (current), +0xe8 the previous one, +0xf8 attention rect in metres,
//   +0x108 city rect in metres (x0, z0, x1, z1), +0x118 city-rect valid, +0x11c update stage (0..3),
//   +0x120 RandomIterator over the attention cell rect, +0x160 work timer (microseconds),
//   +0x178 tick timer (milliseconds), +0x190 industrial-population filter, +0x198 that population.
//
// What reaches outside the class goes through IAmbienceManagerHost (the framework, its services, the
// manager's tuning exemplar and the 24-hour clock) plus the seams that already exist for the audio
// layer (SC4Audio::GetServices(), ViewUtilities(), cSC4AudioSoundManager::GenerateNewInstanceId).
// The two neighbours without an interface of their own are narrowed here: IAmbienceLot (cISC4Lot) and
// IAmbienceAutomaton (cISC4Automaton, iid 0xa9b40f05).
//
// Quirks kept (addresses are the originals):
//  * QueryInterface answers 0x6a12f51c / 0x287fb697 with the primary pointer instead of the interface
//    subobject (0x1003eb33a).
//  * The constructor stores service id 0xaa12f911 and starts with the "ambience active" flag set, so
//    OnTick does work before any Init (0x1003e9106 / 0x1003e90a2).
//  * Init adds itself to the framework *before* the service lookups, and returns 0 without removing
//    itself again when the scape manager or the sound system is missing (0x1003e9527).
//  * Init releases and recreates the occupant filter (+0x190) every time it runs; Shutdown drops it but
//    not the sound system / occupant manager / tick filter, which only the destructor releases
//    (0x1003e994f..0x1003e9983 vs 0x1003e9301..0x1003e93c5).
//  * The destructor clears the layer list without Shutdown/Release on the layers and clears the alarm
//    list without releasing the occupants it holds: a manager destroyed without Shutdown leaks both
//    (0x1003e9373..0x1003e9383). Kept (asserted indirectly: the message server is not notified).
//  * _CreateAmbienceLayersFromExemplarIds builds the "Failed to init layer (%08x)" message and drops it
//    (0x1003eb45c); _DoStageUpdate builds nothing at all when the 1 ms work budget is already spent.
//  * The layer interface is handed the listener *target* as its "eye" and the listener *eye* as its
//    "target" (_DoStageUpdate 0x1003eb1b6 passes this+0x6c then this+0x78).
//  * _CreateAmbienceLayersFromExemplarIds reads the player's audio options twice (0x1003eb41b and
//    0x1003eb43e) and turns a disabled sound option into score master volume 0.
//  * When the exemplar has no zoom-distance property (0xa36f410) _ReadTuningValues writes five 200.0f
//    values through the vector's storage without resizing it (0x1003ea08b): undefined when the vector
//    is empty, so this port sizes the vector to five first (the only deviation in that path).
//  * HandleInsertOccupant / HandleMoveOccupant / HandleRemoveOccupant test the "marked" property
//    0x29244db through an *uninitialised* stack byte (0x1003eb97d): the port initialises it to false.
//  * _AddAlarmOccupant only accepts occupants with IsFlagSet(5) and without IsFlagSet(0x10)
//    (0x1003ebedd / 0x1003ebef2). The alarm list stores a repeating flag at +0x41 that no code ever
//    sets (the only writer zeroes it), so the repeating branch of _UpdateAlarms is dead code; it is
//    nevertheless recovered below.
//  * _RemoveAlarmOccupant erases the first node of the occupant whatever sound id it is asked for
//    (0x1003ec2f2).
//  * The instance id an occupant's sound gets is (int)occupant + soundId, i.e. the occupant pointer
//    truncated to 32 bits (0x1003ebee7); the position events of HandleMoveOccupant and
//    _MoveOccupantSndInstance use (int)occupant without the sound id (0x1003ebbe7 / 0x1003ec1ff).
//  * _PlayOccupantSound posts 0xea451903 / 0xca45190f for "play" and only 0x6a451908 for "stop"
//    (0x1003ec3f3).
//  * HandleOccupant requires the scape manager (+0x30) but only *forwards* to the layers when the
//    occupant has flag 4 set, answering true either way (0x1003ec4f5..0x1003ec536).
//  * DoMessage answers false only for the shutdown message (0x1003e9f66); every other registered type
//    — including 0xe8bda2e7, which _HandleOccupantMessages ignores — answers true.
//  * _HandleFocusChange sets the update stage to 4 (never handled) when the game loses focus and pauses
//    every layer (0x1003e9fb4).
//  * The DoEvent score "start" path sets the player's min/max distance from the two volume payload
//    words and "start mode" sets head-relative before playing (0x1003e9d06 / 0x1003e9bdf), because the
//    layer's events carry the volume in data1/data2.
//
// Deliberate deviations, all in paths where the original would dereference null: a missing listener
// (SFX::Listener() has no null check at 0x1003ea30d), a missing city / occupant manager in
// _HandleInitMessage, a null property holder in the handle-* messages, and a zoom level beyond the
// zoom-distance table (0x1003ea350).
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "audio/interfaces/cIFreshPlayer.h"
#include "audio/interfaces/cIGZSndEventStd.h"
#include "audio/interfaces/cIGZSndSys.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSystemService.h"
#include "rz/cRZTimer.h"
#include "sc4/MiscTypes.h"
#include "sc4/OccupantManager.h"
#include "sc4/QuadGrid.h"
#include "sc4/RandomIterator.h"
#include "sc4audio/AmbienceLayer.h"
#include "sc4audio/SC4AudioHost.h"

namespace SC4Audio {

// The message types the manager registers and handles belong to other recovered headers: city init /
// shutdown and the listener-moved message to Listener.h, the three occupant messages and the two lot
// messages to sc4/Occupant.h and sc4/Lot.h, focus change and tuning reload to SoundManager.h. The two
// no other header owns (the automaton callback and the railway crossing) are declared by the
// implementation in AudioAmbienceManager.cpp.

// The manager's game-side interface (iid 0x6a12f51c; vtable at object+0x10).
inline constexpr std::uint32_t kGZIID_cISC4AudioAmbienceManager = 0x6a12f51c;
class cISC4AudioAmbienceManager : public cIGZUnknown {
public:
    virtual bool Init() = 0;
    virtual bool Shutdown() = 0;
};

// Class ids / interface ids the manager uses (see gz/GZCOM.h and sc4/OccupantFilters3.h).
inline constexpr std::uint32_t kCLSID_cSC4AudioAmbienceManagerFilter = 0x4a401ba7;
inline constexpr std::uint32_t kIID_cSC4AmbienceOccupantFilterAlt = 0x4a40074c;
inline constexpr std::uint32_t kGZIID_cISC4AutomatonView = 0xa9b40f05;
inline constexpr std::uint32_t kGZIID_cISC4LotView = 0xe818fced;
inline constexpr std::uint32_t kGZIID_cISC4BuildingOccupantView = 0x87dda3a9;
inline constexpr std::uint32_t kGZIID_cISC4PropOccupantView = 0xe9793a65;

// The manager's tuning exemplar (type 0x6534284a, group 0xca25875d, instance 0x6a359c5c, read by
// _ReadTuningValues @ 0x1003ea032) and its two properties.
inline constexpr std::uint32_t kAmbienceManagerTuningType = 0x6534284a;
inline constexpr std::uint32_t kAmbienceManagerTuningGroup = 0xca25875d;
inline constexpr std::uint32_t kAmbienceManagerTuningInstance = 0x6a359c5c;
inline constexpr std::uint32_t kPropZoomDistances = 0xa36f410;    // vector<float>, five zoom levels
inline constexpr std::uint32_t kPropLayerIds = 0x2a3ff8cd;        // vector<uint32> of layer exemplar ids

// Occupant properties the manager reads (exemplar property ids).
inline constexpr std::uint32_t kPropMarked = 0x29244db;           // present -> the manager ignores the occupant
inline constexpr std::uint32_t kPropSoundId = 0x6a36afac;         // main looping sound of the occupant
inline constexpr std::uint32_t kPropSoundId2 = 0xc9b93a56;        // second sound, posted with flags 0
inline constexpr std::uint32_t kPropAlarmTimes = 0x6a36afab;      // vector<float> hours
inline constexpr std::uint32_t kPropAlarmSound = 0xa36afa2;       // alarm sound, one shot
inline constexpr std::uint32_t kPropAlarmSoundRepeat = 0x2a53be13;// alarm sound, repeating
inline constexpr std::uint32_t kPropSoundStateA = 0xa4c13cb;      // sounds swapped by the lot condition
inline constexpr std::uint32_t kPropSoundStateB = 0xca19d7ca;
inline constexpr std::uint32_t kPropSoundVacant = 0x4a4c132e;     // sound of a condition-1 / state-0 lot
inline constexpr std::uint32_t kPropRailwayCrossingSound = 0xaa87817a;

// Occupant flags the manager tests: 5 (bits 0 and 2) admits an occupant's sound, 0x10 refuses it, and
// 4 is the visible flag HandleOccupant forwards on.
inline constexpr std::uint32_t kOccupantFlagSoundEligible = 5;
inline constexpr std::uint32_t kOccupantFlagNoAlarm = 0x10;
inline constexpr std::uint32_t kOccupantFlagVisible = 4;

// The score events DoEvent handles are the ones cSC4AudioAmbienceLayer posts (kEventScore* and
// kEventSetMasterVolume, declared by AmbienceLayer.h / SoundManager.h).

// cISC4Lot as the manager uses it: the lot manager's slot 0x70 answers with one of these, the lot's
// own slot 0x118 answers the state and slot 0x188 hands out its occupants (one reference each).
class IAmbienceLot : public cIGZUnknown {
public:
    virtual std::int32_t State() = 0;                                        // vslot 0x118
    virtual void GetOccupants(std::list<sc4::cISC4Occupant*>& out) = 0;      // vslot 0x188
};

// cISC4Automaton (iid 0xa9b40f05) as the manager uses it: it registers itself as a callback target,
// reads the automaton's position, its occupant and (for the stop signal) one state.
class IAmbienceAutomaton : public cIGZUnknown {
public:
    virtual sc4::cISC4Occupant* AsOccupant() = 0;          // vslot 0x30
    virtual void GetPosition(float* xyz) = 0;              // vslot 0x38
    virtual void AddCallback(cIGZMessageTarget2* target) = 0;     // vslot 0x138
    virtual void RemoveCallback(cIGZMessageTarget2* target) = 0;  // vslot 0x140
    virtual void* FindState(std::uint32_t state) = 0;      // vslot 0x1c8
};

// Everything the original reached through RZ::spFW, RZGetFramework(), SL:: globals and GZCOM().
class IAmbienceManagerHost {
public:
    virtual ~IAmbienceManagerHost() = default;
    // RZGetFramework() / RZ::spFW: false when there is no framework at all.
    virtual bool HasFramework() = 0;
    virtual void AddSystemService(cIGZSystemService* service) = 0;        // framework vslot 0x18
    virtual void RemoveSystemService(cIGZSystemService* service) = 0;     // framework vslot 0x20
    virtual void AddToTick(cIGZSystemService* service) = 0;               // framework vslot 0x48
    virtual void RemoveFromTick(cIGZSystemService* service) = 0;          // framework vslot 0x50
    // RZGetFramework()->GetSystemService(id, iid, &out) (vslot 0x28); the answers are borrowed and the
    // manager takes its own reference where it keeps them.
    virtual cIGZUnknown* AudioScapeManager() = 0;      // service 0x49e50ba1, iid 0x29e39db4
    virtual cIGZSndSys* SoundSystem() = 0;             // service 0x23cecba0, iid 0x106077d2
    virtual cIGZMessageServer2* MessageServer() = 0;   // service 0x04fa845b, iid 0x652294c7
    // SL::spOccupantManager (borrowed; the manager adds a reference).
    virtual sc4::cISC4OccupantManager* OccupantManager() = 0;
    // SL::spLotManager vslot 0x70(occupant): the lot the occupant belongs to, borrowed.
    virtual IAmbienceLot* LotOfOccupant(sc4::cISC4Occupant* occupant) = 0;
    // SL::spCity vslots 0x2e8 (cells in x) and 0x2f0 (cells in z).
    virtual void CityCellExtent(int& cellsX, int& cellsZ) = 0;
    // GZCOM()->CreateInstance(classId, iid, &out) (vslot 0x18); *out holds one reference.
    virtual bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) = 0;
    // SL::spRM->GetResource({0x6534284a, 0xca25875d, 0x6a359c5c}, iid 0xa52160f5) and the two
    // cSCPropertyHelp::GetPropertyValue reads on the exemplar it answers.
    virtual bool HasTuning() = 0;
    virtual bool GetTuningFloats(std::uint32_t propertyId, std::vector<float>& out) = 0;
    virtual bool GetTuningUints(std::uint32_t propertyId, std::vector<std::uint32_t>& out) = 0;
    // SL::sp24HourClock vslots 0x50 (hour) and 0x68 (minute); false when there is no clock (the
    // original dereferences the clock without a check in _AddAlarmOccupant).
    virtual bool ClockTime(int& hour, int& minute) = 0;
    // SFX::SoundManager()->GenerateNewInstanceId() (cSC4AudioSoundManager's process-wide counter).
    virtual std::uint32_t GenerateNewInstanceId() = 0;
    virtual void LogError(const char* /*message*/) {}

    // The three places the original takes another class's own interface straight from an object: an occupant's (or a
    // railway crossing message's) automaton by QueryInterface 0xa9b40f05 (one reference), the automaton a callback
    // message carries in Void1 (the pointer itself, borrowed), and a lot message's lot by QueryInterface 0xe818fced
    // (one reference). The defaults do exactly that, for objects that carry IAmbienceAutomaton / IAmbienceLot; the
    // integration answers with views of the game's own automaton and lot classes.
    virtual IAmbienceAutomaton* AutomatonOfObject(cIGZUnknown* object);
    virtual IAmbienceAutomaton* AutomatonOfCallbackPayload(void* payload) { return static_cast<IAmbienceAutomaton*>(payload); }
    virtual IAmbienceLot* LotOfObject(cIGZUnknown* object);
};
IAmbienceManagerHost* AmbienceManagerHost();
void SetAmbienceManagerHost(IAmbienceManagerHost* host);

// One alarm of the alarm list at +0x88: a sound that plays once (or in the, unreachable, repeating
// mode) when the 24-hour clock passes one of `times` (payload at +0x10 of the list node, 0x38 bytes).
struct tOccupantAlarm {
    sc4::cISC4Occupant* occupant = nullptr;   // +0x00 one reference, released by _RemoveAlarmOccupant
    std::uint32_t soundId = 0;                // +0x08
    float hour = 0.0f;                        // +0x0c the clock reading when the alarm was added
    std::int32_t count = 0;                   // +0x10 number of valid entries in `times`
    std::vector<float> times;                 // +0x18 hours at which the sound starts
    bool active = false;                      // +0x30 a sound instance is playing
    bool repeating = false;                   // +0x31 repeats between consecutive times
};

class cSC4AudioAmbienceManager final : public cIGZSystemService,
                                       public cIGZMessageTarget2,
                                       public cISC4AudioAmbienceManager,
                                       public cIGZSndEventHandler {
public:
    cSC4AudioAmbienceManager();                                // @ 0x1003e9050
    static cSC4AudioAmbienceManager* Create() { return new cSC4AudioAmbienceManager(); }   // @ 0x1003dee26

    // --- cIGZUnknown, shared by the four bases (iids 1, 0x6a12f51c, 0x287fb697) ---
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1003eb33a
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount_); }   // @ 0x1003eb286
    std::uint32_t Release() override;                              // @ 0x1003eb2be

    // --- cIGZSystemService (also the cISC4AudioAmbienceManager Init/Shutdown) ---
    bool Init() override;                                          // @ 0x1003e94ec
    bool Shutdown() override;                                      // @ 0x1003e97e4
    bool OnTick(std::uint32_t elapsedMs) override;                 // @ 0x1003eaa8e
    bool OnIdle(std::uint32_t elapsedMs) override { (void)elapsedMs; return true; }   // @ 0x1003eb210
    std::uint32_t GetServiceID() const override { return serviceId_; }                 // @ 0x1003ec678
    void SetServiceID(std::uint32_t id) override { serviceId_ = id; }                  // @ 0x1003ec682
    std::int32_t GetServicePriority() const override { return 0; }                     // @ 0x1003ec68c
    bool IsServiceRunning() const override { return running_; }                        // @ 0x1003ec694
    void SetServiceRunning(bool running) override { running_ = running; }              // @ 0x1003ec69e

    // --- cIGZSndEventHandler / cIGZMessageTarget2 ---
    bool DoEvent(cIGZSndEventStd* event) override;                 // @ 0x1003e9a3a
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1003e9e4a

    std::uint32_t GetIndustrialPopulation() const { return industrialPopulation_; }    // @ 0x1003e9a2e

    // --- occupant tracking (called from the message handlers and the two iteration callbacks) ---
    void HandleInsertOccupant(sc4::cISC4Occupant* occupant);        // @ 0x1003eb95e
    void HandleMoveOccupant(sc4::cISC4Occupant* occupant);          // @ 0x1003ebb40
    void HandleRemoveOccupant(sc4::cISC4Occupant* occupant);        // @ 0x1003ebc36
    void HandleInitOccupant(sc4::cISC4Occupant* occupant);          // @ 0x1003ec048
    bool HandleOccupant(sc4::cISC4Occupant* occupant);              // @ 0x1003ec4dc

    // --- inspection (tests) ---
    std::size_t LayerCount() const { return layers_.size(); }
    const std::list<cSC4AudioAmbienceLayer*>& Layers() const { return layers_; }
    std::size_t AlarmCount() const { return alarms_.size(); }
    const std::list<tOccupantAlarm>& Alarms() const { return alarms_; }
    std::uint32_t Stage() const { return stage_; }
    const sc4::SC4RectT<float>& AttentionRect() const { return attentionRect_; }
    float AttentionX0() const { return attentionRect_.x0; }
    float AttentionZ0() const { return attentionRect_.z0; }
    float AttentionX1() const { return attentionRect_.x1; }
    float AttentionZ1() const { return attentionRect_.z1; }
    const std::int32_t* CurrentCells() const { return currentCells_; }
    const std::int32_t* PreviousCells() const { return previousCells_; }
    std::int32_t ZoomLevel() const { return zoomLevel_; }
    const float* ListenerTarget() const { return listenerTarget_; }
    const float* ListenerEye() const { return listenerEye_; }
    bool IsActive() const { return active_; }
    const std::vector<float>& ZoomDistances() const { return zoomDistances_; }
    const std::vector<std::uint32_t>& LayerIds() const { return layerIds_; }
    sc4::cISC4OccupantFilter* IndustrialFilter() const { return industrialFilter_; }
    sc4::cISC4OccupantFilter* TickFilter() const { return tickFilter_; }
    cIGZUnknown* ScapeManager() const { return scapeManager_; }
    cIGZSndSys* SoundSystem() const { return soundSystem_; }
    sc4::cISC4OccupantManager* OccupantManager() const { return occupantManager_; }

private:
    ~cSC4AudioAmbienceManager();                                   // @ 0x1003e92b6

    void RemoveAllLayers();                                        // @ 0x1003e99ba
    void HandleSetMasterVolume(cIGZSndEventStd* event);            // @ 0x1003e9dd8
    bool HandleFocusChange(cIGZMessage2Standard* message);         // @ 0x1003e9f96
    void ReadTuningValues();                                       // @ 0x1003e9ffc
    bool HandleInitMessage();                                      // @ 0x1003ea11a
    bool HandleShutdownMessage();                                  // @ 0x1003ea282
    void UpdateAttentionRect();                                    // @ 0x1003ea2f4
    bool HandleOccupantMessages(cIGZMessage2Standard* message);    // @ 0x1003ea4cc
    bool HandleAutomatonMessage(cIGZMessage2Standard* message);    // @ 0x1003ea70c
    bool HandleRailwayCrossingMessage(cIGZMessage2Standard* m);    // @ 0x1003ea900
    void UpdateAlarms();                                           // @ 0x1003eabd6
    bool DoStageUpdate(std::uint32_t elapsedMs);                   // @ 0x1003eaf44
    void CreateAmbienceLayersFromExemplarIds();                    // @ 0x1003eb38e
    void IterateOccupantsThatNeedToBeTracked();                    // @ 0x1003eb54c
    void UpdateLotOccupantsForConditionChange(IAmbienceLot* lot, int condition, int state);   // @ 0x1003eb696
    bool AddOccupantSndInstance(sc4::cISC4Occupant* occupant, std::uint32_t soundId, bool flag);  // @ 0x1003ebd6a
    bool AddAlarmOccupant(sc4::cISC4Occupant* occupant, std::uint32_t soundId, const std::vector<float>& times,
                          bool repeating);                         // @ 0x1003ebeb8
    bool MoveOccupantSndInstance(sc4::cISC4Occupant* occupant);    // @ 0x1003ec1aa
    bool RemoveOccupantSndInstance(sc4::cISC4Occupant* occupant, std::uint32_t soundId);   // @ 0x1003ec228
    bool RemoveAlarmOccupant(sc4::cISC4Occupant* occupant, std::uint32_t soundId);         // @ 0x1003ec2d2
    void PlayOccupantSound(sc4::cISC4Occupant* occupant, bool play);                       // @ 0x1003ec332

    static bool IterateOccupantsOnTickCallback(sc4::cISC4Occupant* occupant, void* context);  // @ 0x1003eb218
    static bool IterateOccupantsOnInitCallback(sc4::cISC4Occupant* occupant, void* context);  // @ 0x1003ec544

    // +0x20..+0x2f
    std::int32_t refCount_ = 0;                 // +0x20
    std::uint32_t serviceId_ = 0xaa12f911;      // +0x24
    bool running_ = false;                      // +0x28
    [[maybe_unused]] bool unused29_ = true;     // +0x29 (written by the constructor, never read)
    // +0x30..+0x4f
    cIGZUnknown* scapeManager_ = nullptr;       // +0x30
    cIGZSndSys* soundSystem_ = nullptr;         // +0x38
    sc4::cISC4OccupantManager* occupantManager_ = nullptr;   // +0x40
    sc4::cISC4OccupantFilter* tickFilter_ = nullptr;         // +0x48
    // +0x50..+0x67
    std::list<cSC4AudioAmbienceLayer*> layers_;     // +0x50 (the binary's list holds the concrete class)
    // +0x68..+0x87 (a 4-byte hole at +0x84)
    std::int32_t zoomLevel_ = 0;                // +0x68
    float listenerTarget_[3] = {0, 0, 0};       // +0x6c
    float listenerEye_[3] = {0, 0, 0};          // +0x78
    [[maybe_unused]] std::uint32_t unused84_ = 0;   // +0x84
    // +0x88..+0x9f
    std::list<tOccupantAlarm> alarms_;          // +0x88
    // +0xa0..+0xa7
    bool active_ = true;                        // +0xa0
    // +0xa8..+0xd7
    std::vector<float> zoomDistances_;          // +0xa8
    std::vector<std::uint32_t> layerIds_;       // +0xc0
    // +0xd8..+0x107
    std::int32_t currentCells_[4] = {0, 0, 0, 0};    // +0xd8 x0, z0, x1, z1
    std::int32_t previousCells_[4] = {0, 0, 0, 0};   // +0xe8
    sc4::SC4RectT<float> attentionRect_;        // +0xf8
    // +0x108..+0x11f
    float cityRect_[4] = {0, 0, 0, 0};          // +0x108 x0, z0, x1, z1 (metres)
    bool cityRectValid_ = false;                // +0x118
    // +0x11c..+0x15b
    std::uint32_t stage_ = 0;                   // +0x11c
    RandomIterator iterator_;                   // +0x120
    // +0x160..+0x18f
    cRZTimer workTimer_{cRZTimer::kMicroseconds};   // +0x160 (the 1 ms work budget)
    cRZTimer tickTimer_{cRZTimer::kMilliseconds};   // +0x178 (the tick length)
    // +0x190..+0x19f
    sc4::cISC4OccupantFilter* industrialFilter_ = nullptr;   // +0x190
    std::uint32_t industrialPopulation_ = 0;                 // +0x198
};

}  // namespace SC4Audio
