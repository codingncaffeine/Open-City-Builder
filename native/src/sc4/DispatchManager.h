// cSC4DispatchManager — the building-dispatch manager: every fire/police/... building that can send a
// vehicle out (a "dispatch source") registers here, the manager owns the dispatches (a dispatch ID per
// call), hands the call number to the automata manager, drives the pylon (call-box) occupants the
// dispatch tool places and tracks the automaton in the on-screen dispatch view.
//
// 37 functions at 0x1002157bc..0x100217abc (complete object dtor 0x1002157bc, base dtor 0x100215848,
// deleting dtor 0x100215852; the 10 non-virtual thunks 0x10021588e/0x10021589c/0x1002158ee/0x100215928/
// 0x1002159ac/0x1002159ba/0x100215ec8/0x100216058/0x100216344/0x100217ab0 are compiler-generated).
// Class id 0x4990bd93, interfaces 0x694028ec (primary), 0x452294aa (cIGZMessageTarget2) and 0xe4fda3d4
// (cIGZSerializable); one refcount shared by the three views.
//
// Original layout (0xa0 bytes): +0x18 initialized, +0x1c refcount, +0x20 "loaded from a save" flag,
// +0x28 cIGZMessageServer2*, +0x30 city*, +0x38 std::list<DispatchInfo> (sentinel prev at +0x38, next at
// +0x40, info at node+0x10), +0x48 dispatch count, +0x50 next dispatch id, +0x58 cSC4DispatchVehicleView*,
// +0x60 active pylon*, +0x68 std::vector<pylon*> (begin/end/cap at +0x68/+0x70/+0x78), +0x80 current
// dispatch tool type (8 = no tool), +0x88 std::vector<cRZAutoRefCount<cISC4DispatchSource>>.
//
// DispatchInfo (0x38 bytes): +0x00 tDispatchType, +0x04 id, +0x08 call number, +0x0c dispatch-view track
// id, +0x10 bool "activated" (dispatch state 5), +0x14/+0x18/+0x1c x/y/z, +0x20 source occupant (the
// station being served), +0x28 automaton, +0x30 pylon occupant.
//
// Messages: registered in Init and handled by DoMessage (0x100216066): 0xa6b79602 camera moved (view
// changed), 0x99ef1143 occupant removed, 0xaa1b2002 (name not recovered: every dispatched automaton is
// sent back to its source occupant), 0x09d92fc3 automaton removed / arrived (HandleAutomataEventMessage,
// 0x10021628e). It posts 0xe94671eb dispatch began, 0xe94671ec activated, 0xe94671ed deactivated and
// 0x894671f6 dispatch ended; a dispatch source (cSC4FirePlaneDispatchSource::DoMessage) consumes them.
//
// Quirks kept on purpose (see the .cpp for the address of each):
//  * InitDispatch, when no pylon is active and no call box in the available list matches the call number
//    (including when the dispatch type is not the one the tool created call boxes for), creates a pylon
//    occupant, sets the dispatch's pylon to null and still reports success (the created pylon is leaked).
//  * RevokeAllDispatches ignores its dispatch-type argument and revokes every dispatch (it always takes
//    the first list entry).
//  * GetDispatchIDArray checks its limit only after writing the id, so a limit of 0 never stops it.
//  * Read does not clear the list/vector/pylon it is reading into and does not release the old active
//    pylon; Write serialises the next-dispatch-id counter between the list and the active pylon.
//  * The dispatch-view track id (+0x0c) is not part of the serialized DispatchInfo record.
//  * Shutdown does not end the live dispatches and the destructor only clears the list nodes (it does not
//    release the objects a dispatch holds).
//  * CanDispatch drops the "already dispatched" out flag the source fills in.
//  * EndDispatch leaves the dispatch's pylon/automaton/station attached when no dispatch source answers
//    for its type.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"   // kGZIID_cIGZMessageTarget2

class cRZString;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4DispatchManager = 0x4990bd93;
constexpr std::uint32_t kGZIID_cISC4DispatchManager = 0x694028ec;
constexpr std::uint32_t kDispatchRecordVersion = 2;

// The dispatch interface of the other neighbours (ids used by the serialized DispatchInfo record).
constexpr std::uint32_t kDispatchAutomatonIid = 0xa9b40f05;
constexpr std::uint32_t kDispatchPylonIid = 0xc9b6d7d6;

// 0 = police, 1 = fire (cSC4PoliceSimulator / cSC4FireProtectionSimulator::CanHandleDispatchType),
// 5 = fire plane (cSC4FirePlaneDispatchSource +0x20), 6 = police helicopter
// (cSC4PoliceHelicopterDispatchSource +0x20). 2..4 are the remaining sources; 8 = no dispatch tool.
enum tDispatchType : std::uint32_t {
    kDispatchTypePolice = 0,
    kDispatchTypeFire = 1,
    kDispatchType2 = 2,
    kDispatchType3 = 3,
    kDispatchType4 = 4,
    kDispatchTypeFirePlane = 5,
    kDispatchTypePoliceHelicopter = 6,
    kDispatchTypeNone = 8,
};

// Values recovered from use: 0 idle (ReturnPylonToStation), 3 dispatched (BeginDispatch), 5 arrived /
// working (UpdateDispatchState tests it); automata also report other values through the removal message.
enum tDispatchState : std::uint32_t {
    kDispatchStateIdle = 0,
    kDispatchStateDispatched = 3,
    kDispatchStateActive = 5,
};

// cISC4AutomataManager::tDispatchLocation (20 bytes): +0x00 kind (0 occupant, 1 world position,
// 2 cell, 3 rect), +0x08 x / occupant / cell, +0x0c y, +0x10 z.
struct tDispatchLocation {
    std::uint32_t type = 0;      // +0x00
    std::uint32_t reserved = 0;  // +0x04
    cISC4Occupant* occupant = nullptr;   // kind 0: the occupant (shares +0x08 with x in the original)
    float x = 0.0f;              // +0x08
    float y = 0.0f;              // +0x0c
    float z = 0.0f;              // +0x10
};

class IPylonOccupant;
class IDispatchAutomaton;

// cISC4DispatchManager::DispatchInfo (0x38 bytes), see the header comment for the field offsets.
struct DispatchInfo {
    tDispatchType type = static_cast<tDispatchType>(0);   // +0x00
    std::uint32_t id = 0;                                // +0x04
    std::uint32_t callNumber = 0;                        // +0x08
    std::int32_t trackId = 0;                            // +0x0c
    bool active = false;                                 // +0x10
    float x = 0.0f, y = 0.0f, z = 0.0f;                  // +0x14, +0x18, +0x1c
    cISC4Occupant* station = nullptr;                    // +0x20 (counted)
    IDispatchAutomaton* automaton = nullptr;             // +0x28 (counted)
    IPylonOccupant* pylon = nullptr;                     // +0x30 (counted)
};

// ----------------------------------------------------------------------------------------------------------------------------
// Neighbour seams. Every method mirrors one original virtual call; the slot/address is in the comment. The real game adapts
// the recovered class to the interface (docs/SEAMS.md); tests supply a fake.
// ----------------------------------------------------------------------------------------------------------------------------

// cISC4PylonOccupant (iid 0xc9b6d7d6, vtable 0x100a87598; implementation cSC4PylonOccupant 0x10022def8). Only the slots this
// manager calls are declared.
class IPylonOccupant : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                              // [3] 0x18
    virtual bool Init() = 0;                                              // [4] 0x20
    virtual bool Shutdown() = 0;                                          // [5] 0x28
    virtual void SetUIState(std::uint32_t state) = 0;                     // [6] 0x30
    virtual void SetDispatchType(tDispatchType type) = 0;                 // [9] 0x48
    virtual tDispatchType GetDispatchType() const = 0;                    // [10] 0x50
    virtual void SetDispatchState(tDispatchState state) = 0;              // [11] 0x58
    virtual void SetCallNumber(std::uint16_t callNumber) = 0;             // [13] 0x68
    virtual std::uint16_t GetCallNumber() const = 0;                      // [14] 0x70
    virtual void SetSourceOccupant(cISC4Occupant* occupant) = 0;          // [17] 0x88
    virtual cISC4Occupant* GetSourceOccupant() const = 0;                 // [18] 0x90
    // The two slots only the dispatch tool reads (defaults for the manager's tests).
    virtual std::uint32_t GetUIState() const { return 1; }                // [7] 0x38
    virtual tDispatchState GetDispatchState() const { return kDispatchStateIdle; }   // [12] 0x60
};

// cISC4Automaton (iid 0xa9b40f05; vtable of cSC4Automaton 0x100a73b30) as the dispatch manager uses it. Only the slots called
// here are declared, in vtable order.
class IDispatchAutomaton : public cIGZUnknown {
public:
    virtual cISC4Occupant* Occupant() = 0;                                // [6] 0x30 AsOccupant()
    virtual bool Remove(bool force) = 0;                                  // [13] 0x68
    virtual std::uint16_t GetCallNumber() const = 0;                      // [23] 0xb8
    virtual void SetCallNumber(std::uint16_t callNumber) = 0;             // [24] 0xc0
    virtual void SetDispatchState(tDispatchState state) = 0;              // [25] 0xc8
    virtual tDispatchState GetDispatchState() const = 0;                  // [26] 0xd0
    virtual void SetDispatchInfoID(std::uint32_t id) = 0;                 // [28] 0xe0
    virtual void ReturnToSourceOccupant() = 0;                            // [29] 0xe8
    virtual void AddCallback(cIGZMessageTarget2* target) = 0;             // [39] 0x138
};

// cISC4DispatchSource (vtable of cSC4FirePlaneDispatchSource 0x100ae9e40; implementations 0x10072xxxx). Only the slots called
// here are declared, in vtable order.
class IDispatchSource : public cIGZUnknown {
public:
    virtual bool CanHandleDispatchType(std::uint32_t type) const = 0;     // [3] 0x18
    virtual bool CanDispatch(float x, float z, bool* alreadyDispatched) = 0;   // [4] 0x20
    virtual bool GetStations(std::list<cISC4Occupant*>& out) = 0;         // [6] 0x30 (copies one reference per station)
    virtual bool GetDispatchesAtStation(cISC4Occupant* station, std::uint32_t* callMask, std::uint32_t* baseCall,
                                        std::uint32_t* count) = 0;      // [7] 0x38
    virtual cISC4Occupant* DispatchFromNearestStation(float x, float z, std::uint32_t* callNumber) = 0;   // [8] 0x40
    virtual bool DispatchFromStation(cISC4Occupant* station, std::uint32_t* callNumber) = 0;              // [9] 0x48
    virtual void RevokeDispatchFromStation(cISC4Occupant* station, std::uint32_t callNumber) = 0;         // [10] 0x50
    virtual bool GetDispatchAutomataProperties(cISC4Occupant* station, std::uint32_t& group,
                                               cGZPersistResourceKey& key, std::uint32_t& flags,
                                               const float position[3]) = 0;   // [11] 0x58
};

// cSC4DispatchVehicleView (0x100-byte object created at 0x10022ec1c; Init 0x10023066c, Shutdown 0x100230a1c,
// HandleViewChanged 0x100232cc8, TrackOccupant 0x100233ae2, UntrackOccupant 0x1002342e0). The renderer layer install is a
// host service (spRenderer 0x100/0x108).
class IDispatchVehicleView : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                              // 0x10023066c
    virtual bool Shutdown() = 0;                                          // 0x100230a1c
    virtual void HandleViewChanged() = 0;                                 // 0x100232cc8
    virtual int TrackOccupant(cISC4Occupant* occupant, int trackerType, std::uint32_t callNumber,
                              const cRZString* label) = 0;                // 0x100233ae2
    virtual void UntrackOccupant(int trackId, bool force) = 0;            // 0x1002342e0
};

// cISC4AutomataManager (concrete cSC4AutomataManager, vtable 0x100a73830). The two dispatch entry points are slots 0x88/0x90;
// the implementations in the binary (0x100100d86 / 0x100100d8e) are stubs and the real work lives in the per-kind managers
// (cSC4AircraftManager::Dispatch 0x10050155c, ...).
class IAutomataDispatchManager {
public:
    virtual ~IAutomataDispatchManager() = default;
    // 0x88: (group, from, to, flags, 0, resource key, 0xff) -> new automaton or null. The original passes a zeroed first
    // location and the clicked position as the second one.
    virtual IDispatchAutomaton* Dispatch(std::uint32_t group, const tDispatchLocation& from, const tDispatchLocation& to,
                                        std::uint32_t flags, std::uint8_t type, const cGZPersistResourceKey& key,
                                        std::uint8_t argument) = 0;
    // 0x90: re-dispatch an existing automaton, (automaton, from, to, flags, 2).
    virtual bool Dispatch(IDispatchAutomaton* automaton, const tDispatchLocation& from, const tDispatchLocation& to,
                          std::uint32_t flags, std::uint8_t type) = 0;
};

// The city services the manager reads through SL::spCity (cISC4City). The original stores the city pointer at +0x30 and adds
// one reference to it in Init.
class IDispatchCity : public cIGZUnknown {
public:
    // spCity vslot 0x300: cell -> world X/Z of the cell corner.
    virtual void CellToWorld(std::int32_t cellX, std::int32_t cellZ, float& x, float& z) = 0;
    // spCity vslot 0x2a0 (city view) -> vslot 0x60: terrain height at a world X/Z.
    virtual float CellHeight(float x, float z) = 0;
    // spCity vslot 0x110 (occupant manager) -> vslot 0x180: drop the references GetStations added.
    virtual void ReleaseStationList(std::list<cISC4Occupant*>& stations) = 0;
};

// Services and factories the manager reaches through SL::sp* globals and operator new.
class IDispatchHost {
public:
    virtual ~IDispatchHost() = default;
    // SL::spMS2 (borrowed; the manager calls AddRef/Release itself).
    virtual cIGZMessageServer2* MessageServer() = 0;
    // SL::spCity (borrowed; the manager calls AddRef/Release itself), null when there is no city.
    virtual IDispatchCity* City() = 0;
    // cSC4DispatchVehicleView: operator new(0x100) + ctor + AddRef (counted).
    virtual IDispatchVehicleView* CreateDispatchVehicleView() = 0;
    // cSC4PylonOccupant: operator new(0x228) + ctor + AddRef (counted).
    virtual IPylonOccupant* CreatePylonOccupant() = 0;
    // cSC4AutomataManager::GetAutomataManagerForGroup @ 0x100104c7e (borrowed).
    virtual IAutomataDispatchManager* GetAutomataManagerForGroup(std::uint32_t group) = 0;
    // SL::spRenderer vslot 0x100(view, layer 5, priority 1000).
    virtual void AddDispatchViewLayer(IDispatchVehicleView* view) = 0;
    // SL::spRenderer vslot 0x108(view).
    virtual void RemoveDispatchViewLayer(IDispatchVehicleView* view) = 0;
    // SC4WriteGZSerializable(stream, object) @ 0x1000435e3.
    virtual bool WriteSerializable(cIGZOStream& stream, const cIGZUnknown* object) = 0;
    // SC4ReadGZSerializable(stream, iid, &out, false) @ 0x100043267; *out receives a counted reference (or null).
    virtual bool ReadSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) = 0;
    // Port seam: the automaton an automaton event (0x09d92fc3, Void1) names, as this manager holds it (the original compares the
    // object itself). The default takes the object as the view (the tests' fakes implement the view directly).
    virtual IDispatchAutomaton* AutomatonFromMessage(void* object) { return static_cast<IDispatchAutomaton*>(object); }
};
IDispatchHost* DispatchHost();
void SetDispatchHost(IDispatchHost* host);

// cISC4DispatchManager (iid 0x694028ec, slots 3..22 of the primary vtable 0x100a86df0).
class cISC4DispatchManager : public cIGZUnknown {
public:
    virtual bool Init() = 0;                                                              // [3] 0x18 @0x1002159c8
    virtual bool Shutdown() = 0;                                                          // [4] 0x20 @0x100215bca
    virtual bool CanDispatch(tDispatchType type, float x, float z) = 0;                   // [5] 0x28 @0x10021720c
    virtual std::uint32_t Dispatch(tDispatchType type, float x, float z) = 0;              // [6] 0x30 @0x10021724e
    virtual bool RevokeDispatchByID(std::uint32_t id) = 0;                                 // [7] 0x38 @0x10021695a
    virtual bool RevokeDispatchByPylon(IPylonOccupant* pylon) = 0;                          // [8] 0x40 @0x10021660c
    virtual bool RevokeAllDispatches(tDispatchType type) = 0;                              // [9] 0x48 @0x100216b58
    virtual bool GetDispatchInfo(std::uint32_t id, DispatchInfo& out) = 0;                 // [10] 0x50 @0x100216b84
    virtual int GetDispatchCount(tDispatchType type) = 0;                                  // [11] 0x58 @0x100216be0
    virtual void GetDispatchIDArray(tDispatchType type, std::uint32_t* ids, std::uint32_t& count) = 0;   // [12] 0x60 @0x100216c06
    virtual void RegisterDispatchSource(IDispatchSource* source) = 0;                      // [13] 0x68 @0x100216454
    virtual void UnregisterDispatchSource(IDispatchSource* source) = 0;                    // [14] 0x70 @0x1002164f0
    virtual IDispatchSource* GetDispatchSource(std::uint32_t type) const = 0;              // [15] 0x78 @0x1002165ba
    virtual bool BeginDispatchTool(tDispatchType type) = 0;                                // [16] 0x80 @0x100216dd0
    virtual bool EndDispatchTool(tDispatchType type) = 0;                                  // [17] 0x88 @0x1002171dc
    virtual IPylonOccupant* GetActivePylon() const = 0;                                    // [18] 0x90 @0x100217a90
    virtual void SetActivePylon(IPylonOccupant* pylon) = 0;                                // [19] 0x98 @0x100216c38
    virtual IDispatchAutomaton* GetAutomatonFromPylon(IPylonOccupant* pylon) const = 0;    // [20] 0xa0 @0x100216da8
    virtual int GetAvailablePylonCount(tDispatchType type) const = 0;                      // [21] 0xa8 @0x100216c96
    virtual IDispatchVehicleView* GetDispatchVehicleView() const = 0;                      // [22] 0xb0 @0x100217a9a
};

class cSC4DispatchManager : public cISC4DispatchManager, public cIGZMessageTarget2, public cIGZSerializable {
public:
    cSC4DispatchManager() = default;                          // ctor is inlined into GZCOM_CreatecSC4DispatchManager 0x100215710
    virtual ~cSC4DispatchManager();                           // @0x1002157bc (base 0x100215848, deleting 0x100215852)

    // cIGZUnknown, one refcount for the three views @0x100215880/0x1002158aa/0x100215962
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Init() override;                                     // @0x1002159c8
    bool Shutdown() override;                                 // @0x100215bca
    bool CanDispatch(tDispatchType type, float x, float z) override;                 // @0x10021720c
    std::uint32_t Dispatch(tDispatchType type, float x, float z) override;           // @0x10021724e
    bool RevokeDispatchByID(std::uint32_t id) override;                              // @0x10021695a
    bool RevokeDispatchByPylon(IPylonOccupant* pylon) override;                      // @0x10021660c
    bool RevokeAllDispatches(tDispatchType type) override;                          // @0x100216b58
    bool GetDispatchInfo(std::uint32_t id, DispatchInfo& out) override;             // @0x100216b84
    int GetDispatchCount(tDispatchType type) override;                              // @0x100216be0
    void GetDispatchIDArray(tDispatchType type, std::uint32_t* ids, std::uint32_t& count) override;   // @0x100216c06
    void RegisterDispatchSource(IDispatchSource* source) override;                  // @0x100216454
    void UnregisterDispatchSource(IDispatchSource* source) override;                // @0x1002164f0
    IDispatchSource* GetDispatchSource(std::uint32_t type) const override;          // @0x1002165ba
    bool BeginDispatchTool(tDispatchType type) override;                            // @0x100216dd0
    bool EndDispatchTool(tDispatchType type) override;                              // @0x1002171dc
    IPylonOccupant* GetActivePylon() const override;                                // @0x100217a90
    void SetActivePylon(IPylonOccupant* pylon) override;                            // @0x100216c38
    IDispatchAutomaton* GetAutomatonFromPylon(IPylonOccupant* pylon) const override;   // @0x100216da8
    int GetAvailablePylonCount(tDispatchType type) const override;                  // @0x100216c96
    IDispatchVehicleView* GetDispatchVehicleView() const override;                  // @0x100217a9a

    bool Write(cIGZOStream& stream) const override;           // @0x100215de8 (thunk 0x100215ec8)
    bool Read(cIGZIStream& stream) override;                  // @0x100215ed6 (thunk 0x100216058)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4DispatchManager; }   // @0x100217aa4 (thunk 0x100217ab0)
    bool DoMessage(cIGZMessage2* message) override;           // @0x100216066 (thunk 0x100216344)

    std::size_t DispatchCount() const { return dispatchCount_; }
    tDispatchType ToolType() const { return toolType_; }
    std::size_t AvailablePylonCount() const { return availablePylons_.size(); }
    // Every call box the manager holds (the free ones, then the ones of the dispatches): the native pick walks them.
    std::vector<IPylonOccupant*> AllPylons() const {
        std::vector<IPylonOccupant*> out(availablePylons_.begin(), availablePylons_.end());
        for (const DispatchInfo& info : dispatches_) if (info.pylon) out.push_back(info.pylon);
        return out;
    }

private:
    using DispatchList = std::list<DispatchInfo>;
    using PylonList = std::vector<IPylonOccupant*>;

    bool HandleAutomataEventMessage(cIGZMessage2Standard* message);          // @0x10021628e
    bool UpdateDispatchState(DispatchInfo& info, tDispatchState state);      // @0x100216352
    void ClearAvailablePylonList();                                          // @0x100215b80
    void ReturnPylonToStation(IDispatchSource* source, IPylonOccupant* pylon);   // @0x10021667c
    bool EndDispatch(DispatchInfo& info);                                    // @0x1002169c2
    bool InitDispatch(DispatchInfo& info);                                   // @0x100217430
    bool BeginDispatch(DispatchInfo& info);                                  // @0x100217684

    DispatchList::iterator FindByID(std::uint32_t id);
    DispatchList::iterator FindByPylon(IPylonOccupant* pylon);
    void EraseDispatch(DispatchList::iterator it);
    void WriteDispatchInfo(cIGZOStream& stream, const DispatchInfo& info) const;
    void ReadDispatchInfo(cIGZIStream& stream, DispatchInfo& info);
    void PostDispatchMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2);
    void AddPylonToAvailableList(IPylonOccupant* pylon);

    bool initialized_ = false;                                  // +0x18
    std::uint32_t refCount_ = 0;                                // +0x1c
    bool restored_ = false;                                     // +0x20
    cIGZMessageServer2* messageServer_ = nullptr;               // +0x28
    IDispatchCity* city_ = nullptr;                             // +0x30
    DispatchList dispatches_;                                   // +0x38
    std::size_t dispatchCount_ = 0;                             // +0x48
    std::uint32_t nextDispatchID_ = 0;                          // +0x50
    IDispatchVehicleView* vehicleView_ = nullptr;               // +0x58
    IPylonOccupant* activePylon_ = nullptr;                     // +0x60
    PylonList availablePylons_;                                 // +0x68
    tDispatchType toolType_ = kDispatchTypeNone;                // +0x80
    std::vector<cRZAutoRefCount<IDispatchSource>> sources_;     // +0x88
};

}  // namespace sc4
