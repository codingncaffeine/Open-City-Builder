// cSC4FirePlaneDispatchSource / cSC4PoliceHelicopterDispatchSource: identity, Init/Shutdown (notifications, the dispatch
// manager's registry, the held city, the stations released), the station list from the occupant messages (unit count, all
// units home, the call-number search that reuses a freed run), CanDispatch, a whole dispatch through the real dispatch manager
// (the nearest station with a unit home, the aircraft group handed to the automata manager, the unit's bit cleared and set again
// when the dispatch is revoked), DispatchFromStation's quirks, the record (version, station count, next call number).
#include <cstdio>
#include <cstring>
#include <list>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/AircraftDispatchSource.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DispatchManager.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

constexpr std::uint32_t kBuilding = kDispatchSourceBuildingOccupantType;

// ---- byte streams ---------------------------------------------------------------------------------------------------------
struct OStream final : cIGZOStream {
    std::vector<unsigned char> bytes;
    template <class T> void Put(T v) { const auto* p = reinterpret_cast<const unsigned char*>(&v); bytes.insert(bytes.end(), p, p + sizeof(T)); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    void Flush() override {}
    bool SetSint8(std::int8_t v) override { Put(v); return true; }
    bool SetUint8(std::uint8_t v) override { Put(v); return true; }
    bool SetSint16(std::int16_t v) override { Put(v); return true; }
    bool SetUint16(std::uint16_t v) override { Put(v); return true; }
    bool SetSint32(std::int32_t v) override { Put(v); return true; }
    bool SetUint32(std::uint32_t v) override { Put(v); return true; }
    bool SetSint64(std::int64_t v) override { Put(v); return true; }
    bool SetUint64(std::uint64_t v) override { Put(v); return true; }
    bool SetFloat32(float v) override { Put(v); return true; }
    bool SetFloat64(double v) override { Put(v); return true; }
    bool SetRZCharStr(const char*) override { return false; }
    bool SetGZStr(const cIGZString&) override { return false; }
    bool SetGZSerializable(const cIGZSerializable&) override { return false; }
    bool SetVoid(const void*, std::uint32_t) override { return false; }
    bool GetError() const override { return false; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};
struct IStream final : cIGZIStream {
    std::vector<unsigned char> bytes;
    std::size_t at = 0;
    bool error = false;
    template <class T> T Get() {
        T v{};
        if (at + sizeof(T) > bytes.size()) { error = true; return v; }
        std::memcpy(&v, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    bool Skip(std::uint32_t) override { return false; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 0; }
    bool GetSint8(std::int8_t& v) override { v = Get<std::int8_t>(); return !error; }
    bool GetUint8(std::uint8_t& v) override { v = Get<std::uint8_t>(); return !error; }
    bool GetSint16(std::int16_t& v) override { v = Get<std::int16_t>(); return !error; }
    bool GetUint16(std::uint16_t& v) override { v = Get<std::uint16_t>(); return !error; }
    bool GetSint32(std::int32_t& v) override { v = Get<std::int32_t>(); return !error; }
    bool GetUint32(std::uint32_t& v) override { v = Get<std::uint32_t>(); return !error; }
    bool GetSint64(std::int64_t& v) override { v = Get<std::int64_t>(); return !error; }
    bool GetUint64(std::uint64_t& v) override { v = Get<std::uint64_t>(); return !error; }
    bool GetFloat32(float& v) override { v = Get<float>(); return !error; }
    bool GetFloat64(double& v) override { v = Get<double>(); return !error; }
    bool GetRZCharStr(char*, std::uint32_t) override { return false; }
    bool GetGZStr(cIGZString&) override { return false; }
    bool GetGZSerializable(cIGZSerializable&) override { return false; }
    bool GetVoid(void*, std::uint32_t) override { return false; }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

// ---- a building (its properties in a real property holder) ----------------------------------------------------------------
struct Building final : cISC4Occupant {
    int refs = 1;
    std::uint32_t type = kBuilding;
    float position[3] = {0, 0, 0};
    cSCPropertyHolder holder;
    Building(float x, float z) { position[0] = x; position[2] = z; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4Occupant) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return &holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof(position)); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof(position)); }
    void GetBoundingBox(float*, float*) override {}
    void GetBoundingCityCells(SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }

    void SetUint(std::uint32_t id, std::uint32_t value) { cRZVariant v(value); holder.AddProperty(id, &v, false); }
    std::uint32_t Uint(std::uint32_t id) { std::uint32_t v = 0; cSCPropertyHelp::GetPropertyValue(&holder, id, v); return v; }
};

// ---- the message server, the city, the dispatch manager's neighbours ------------------------------------------------------
struct FakeServer final : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    std::vector<std::uint32_t> posted;
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* m, bool) override { posted.push_back(m->GetType()); return true; }
    bool AddNotification(cIGZMessageTarget2* t, std::uint32_t type) override { added.emplace_back(t, type); return true; }
    bool RemoveNotification(cIGZMessageTarget2* t, std::uint32_t type) override { removed.emplace_back(t, type); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};
struct FakeCity final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
};
struct Automaton final : IDispatchAutomaton {
    Building body{0, 0};
    int refs = 0;
    std::uint16_t call = 0;
    tDispatchState state = kDispatchStateIdle;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    cISC4Occupant* Occupant() override { return &body; }
    bool Remove(bool) override { return true; }
    std::uint16_t GetCallNumber() const override { return call; }
    void SetCallNumber(std::uint16_t c) override { call = c; }
    void SetDispatchState(tDispatchState s) override { state = s; }
    tDispatchState GetDispatchState() const override { return state; }
    void SetDispatchInfoID(std::uint32_t) override {}
    void ReturnToSourceOccupant() override {}
    void AddCallback(cIGZMessageTarget2*) override {}
};
struct Pylon final : IPylonOccupant {
    Building body{0, 0};
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    cISC4Occupant* AsOccupant() override { return &body; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetUIState(std::uint32_t) override {}
    void SetDispatchType(tDispatchType) override {}
    tDispatchType GetDispatchType() const override { return kDispatchTypeNone; }
    void SetDispatchState(tDispatchState) override {}
    void SetCallNumber(std::uint16_t) override {}
    std::uint16_t GetCallNumber() const override { return 0; }
    void SetSourceOccupant(cISC4Occupant*) override {}
    cISC4Occupant* GetSourceOccupant() const override { return nullptr; }
};
// The aircraft manager as the dispatch manager sees it: records what it was asked to send.
struct AircraftManager final : IAutomataDispatchManager {
    Automaton aircraft;
    std::uint32_t group = 0, flags = 0;
    tDispatchLocation from, to;
    int sends = 0;
    IDispatchAutomaton* Dispatch(std::uint32_t g, const tDispatchLocation& f, const tDispatchLocation& t, std::uint32_t fl, std::uint8_t,
                                 const cGZPersistResourceKey&, std::uint8_t) override {
        group = g; from = f; to = t; flags = fl; ++sends;
        return &aircraft;
    }
    bool Dispatch(IDispatchAutomaton*, const tDispatchLocation&, const tDispatchLocation&, std::uint32_t, std::uint8_t) override { return true; }
};
struct DispatchHostFake final : IDispatchHost {
    FakeServer* server = nullptr;
    AircraftManager aircraft;
    std::vector<Pylon*> pylons;   // the manager keeps (and leaks) the call boxes it makes without the tool
    std::vector<std::uint32_t> groupsAsked;
    ~DispatchHostFake() override { for (Pylon* p : pylons) delete p; }
    cIGZMessageServer2* MessageServer() override { return server; }
    IDispatchCity* City() override { return nullptr; }
    IDispatchVehicleView* CreateDispatchVehicleView() override { return nullptr; }
    IPylonOccupant* CreatePylonOccupant() override { pylons.push_back(new Pylon()); pylons.back()->AddRef(); return pylons.back(); }
    IAutomataDispatchManager* GetAutomataManagerForGroup(std::uint32_t group) override {
        groupsAsked.push_back(group);
        return group == 0x4301 || group == 0x4303 ? &aircraft : nullptr;
    }
    void AddDispatchViewLayer(IDispatchVehicleView*) override {}
    void RemoveDispatchViewLayer(IDispatchVehicleView*) override {}
    bool WriteSerializable(cIGZOStream&, const cIGZUnknown*) override { return false; }
    bool ReadSerializable(cIGZIStream&, std::uint32_t, void** out) override { *out = nullptr; return false; }
};
struct SourceHost final : IAircraftDispatchSourceHost {
    FakeCity city;
    FakeServer* server = nullptr;
    cISC4DispatchManager* manager = nullptr;
    int listReleases = 0;
    cIGZUnknown* City() override { return &city; }
    cIGZMessageServer2* MessageServer() override { return server; }
    cISC4DispatchManager* DispatchManager() override { return manager; }
    void ReleaseOccupantList(std::list<cISC4Occupant*>& occupants) override {
        ++listReleases;
        for (cISC4Occupant* o : occupants) o->Release();
        occupants.clear();
    }
};

void Deliver(cSC4AircraftDispatchSource& source, std::uint32_t type, cISC4Occupant* occupant, std::uint32_t data1 = 0, std::uint32_t data2 = 0) {
    cRZMessage2Standard message;
    message.SetType(type);
    if (occupant) {
        message.SetVoid1(occupant);   // the occupant messages carry the occupant; Void1 and Data1 share one slot
    } else {
        message.SetData1(data1);
        message.SetData2(data2);
    }
    source.DoMessage(&message);
}

}  // namespace

int main() {
    FakeServer server;
    DispatchHostFake dispatchHost;
    dispatchHost.server = &server;
    SetDispatchHost(&dispatchHost);
    auto* manager = new cSC4DispatchManager();
    manager->AddRef();
    manager->Init();
    SourceHost host;
    host.server = &server;
    host.manager = manager;
    SetAircraftDispatchSourceHost(&host);

    // ---- identity ----
    auto* helicopter = new cSC4PoliceHelicopterDispatchSource();
    helicopter->AddRef();
    auto* plane = new cSC4FirePlaneDispatchSource();
    plane->AddRef();
    CHECK(helicopter->CanHandleDispatchType(6) && !helicopter->CanHandleDispatchType(5) && !helicopter->CanHandleDispatchType(0));
    CHECK(plane->CanHandleDispatchType(5) && !plane->CanHandleDispatchType(6) && !plane->CanHandleDispatchType(1));
    CHECK(helicopter->GetGZCLSID() == 0x0c0b06e8 && plane->GetGZCLSID() == 0x4c0ae114);
    CHECK(helicopter->NextCallNumber() == 1 && !helicopter->IsInitialized());
    {
        void* out = nullptr;
        CHECK(helicopter->QueryInterface(0xc9c00435, &out) && out == static_cast<IDispatchSource*>(helicopter));
        helicopter->Release();
        CHECK(helicopter->QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(helicopter));
        helicopter->Release();
        CHECK(helicopter->QueryInterface(0xe4fda3d4, &out) && out == static_cast<cIGZSerializable*>(helicopter));
        helicopter->Release();
        CHECK(!helicopter->QueryInterface(1, &out));   // not even cIGZUnknown's own id
    }

    // ---- Init: the notifications, the held city and message server, the dispatch manager's registry ----
    CHECK(manager->GetDispatchSource(6) == nullptr);
    CHECK(helicopter->Init());
    CHECK(helicopter->IsInitialized());
    std::vector<std::uint32_t> heard;
    for (const auto& n : server.added)
        if (n.first == static_cast<cIGZMessageTarget2*>(helicopter)) heard.push_back(n.second);
    CHECK((heard == std::vector<std::uint32_t>{0x99ef1142, 0x99ef1143, 0xe94671eb, 0x894671f6, 0xe94671ec, 0xe94671ed}));
    CHECK(host.city.refs == 1);
    CHECK(manager->GetDispatchSource(6) == static_cast<IDispatchSource*>(helicopter));
    CHECK(helicopter->Init() && host.city.refs == 1);   // a second Init does nothing
    plane->Init();
    CHECK(manager->GetDispatchSource(5) == static_cast<IDispatchSource*>(plane) && manager->GetDispatchSource(6) == static_cast<IDispatchSource*>(helicopter));

    // ---- no stations: CanDispatch is false ----
    bool flag = true;
    CHECK(!helicopter->CanDispatch(0, 0, &flag));
    CHECK(manager->Dispatch(kDispatchTypePoliceHelicopter, 100, 100) == 0);

    // ---- stations from the occupant messages ----
    const std::uint32_t count = kPoliceHelicopterDispatchTraits.countProperty, mask = kPoliceHelicopterDispatchTraits.maskProperty,
                        base = kPoliceHelicopterDispatchTraits.baseProperty;
    Building near(100, 100), far(900, 900), third(500, 500), plain(50, 50), lot(60, 60), barred(70, 70);
    near.SetUint(count, 2);
    far.SetUint(count, 1);
    plain.SetUint(0x1234, 7);          // a building without units
    lot.type = 0x12345678;             // not a building occupant
    lot.SetUint(count, 3);
    barred.SetUint(count, 1);
    barred.SetUint(kDispatchSourceNoAutomataProperty, 1);   // "no automata here"
    Deliver(*helicopter, 0x99ef1142, &near);
    Deliver(*helicopter, 0x99ef1142, &far);
    Deliver(*helicopter, 0x99ef1142, &plain);
    Deliver(*helicopter, 0x99ef1142, &lot);
    Deliver(*helicopter, 0x99ef1142, &barred);
    CHECK(helicopter->Stations().size() == 2);
    CHECK(near.refs == 2 && far.refs == 2 && plain.refs == 1 && lot.refs == 1 && barred.refs == 1);
    CHECK(near.Uint(mask) == 3 && near.Uint(base) == 1);    // two units, both home, call numbers 1..2
    CHECK(far.Uint(mask) == 1 && far.Uint(base) == 3);      // the next free run
    CHECK(helicopter->NextCallNumber() == 4);
    CHECK(plane->Stations().empty());                       // the fire plane source reads its own unit property
    std::list<cISC4Occupant*> listed;
    CHECK(helicopter->GetStations(listed) && listed.size() == 2 && near.refs == 3);
    for (cISC4Occupant* o : listed) o->Release();
    CHECK(helicopter->GetMaxPossibleDispatchCount() == 3);

    // a freed run of call numbers is reused by the next station that fits it
    Deliver(*helicopter, 0x99ef1143, &near);
    CHECK(helicopter->Stations().size() == 1 && near.refs == 1);
    third.SetUint(count, 1);
    Deliver(*helicopter, 0x99ef1142, &third);
    CHECK(third.Uint(base) == 1 && helicopter->NextCallNumber() == 4);
    Deliver(*helicopter, 0x99ef1142, &near);                // two units: 1 is taken, 2 is free but 3 is not -> after 3
    CHECK(near.Uint(base) == 4 && helicopter->NextCallNumber() == 6);

    // ---- CanDispatch with units: true, the flag says whether every unit is out ----
    CHECK(helicopter->CanDispatch(0, 0, &flag) && !flag);

    // ---- a dispatch through the real dispatch manager ----
    const std::uint32_t id = manager->Dispatch(kDispatchTypePoliceHelicopter, 120, 130);
    CHECK(id != 0);
    CHECK(dispatchHost.aircraft.sends == 1 && dispatchHost.aircraft.group == 0x4301);   // the police helicopter group
    CHECK(dispatchHost.aircraft.from.occupant == &near);    // the nearest station with a unit home
    CHECK(dispatchHost.aircraft.to.type == 1 && dispatchHost.aircraft.to.x == 120.0f && dispatchHost.aircraft.to.z == 130.0f);
    CHECK(dispatchHost.aircraft.flags == 0x11882);
    DispatchInfo info;
    CHECK(manager->GetDispatchInfo(id, info) && info.station == &near && info.callNumber == 4);
    CHECK(near.Uint(mask) == 2);                            // unit 4 is out, unit 5 home
    CHECK(dispatchHost.aircraft.aircraft.call == 4);
    CHECK(manager->GetDispatchCount(kDispatchTypePoliceHelicopter) == 1);
    // the dispatch messages for other types do not reach this source's handlers
    Deliver(*helicopter, 0x894671f6, nullptr, 5, id);
    CHECK(near.Uint(mask) == 2);
    // revoking it sends the unit home
    CHECK(manager->RevokeDispatchByID(id));
    CHECK(near.Uint(mask) == 3);
    CHECK(manager->GetDispatchCount(kDispatchTypePoliceHelicopter) == 0);

    // the fire plane: its own units and group
    Building airfield(300, 300);
    airfield.SetUint(kFirePlaneDispatchTraits.countProperty, 1);
    Deliver(*plane, 0x99ef1142, &airfield);
    CHECK(plane->Stations().size() == 1 && airfield.Uint(kFirePlaneDispatchTraits.maskProperty) == 1 &&
          airfield.Uint(kFirePlaneDispatchTraits.baseProperty) == 1);
    const std::uint32_t fireId = manager->Dispatch(kDispatchTypeFirePlane, 310, 320);
    CHECK(fireId != 0 && dispatchHost.aircraft.group == 0x4303 && dispatchHost.aircraft.from.occupant == &airfield);
    CHECK(airfield.Uint(kFirePlaneDispatchTraits.maskProperty) == 0);
    // every unit out: CanDispatch still answers true, with the flag set
    CHECK(plane->CanDispatch(0, 0, &flag) && flag);
    CHECK(manager->RevokeDispatchByID(fireId) && airfield.Uint(kFirePlaneDispatchTraits.maskProperty) == 1);

    // ---- DispatchFromStation / ReturnDispatchToStation ----
    std::uint32_t call = 9;                                 // past the station's last unit (base 4, two units: index 5 > 2)
    CHECK(!helicopter->DispatchFromStation(&near, &call));
    call = 5;                                               // that unit
    CHECK(helicopter->DispatchFromStation(&near, &call) && call == 5 && near.Uint(mask) == 1);
    call = 0;
    CHECK(helicopter->DispatchFromStation(&near, &call) && call == 4 && near.Uint(mask) == 0);
    call = 0;                                               // none home: still true, the call number past the last unit
    CHECK(helicopter->DispatchFromStation(&near, &call) && call == 6 && near.Uint(mask) == 0);
    CHECK(helicopter->ReturnDispatchToStation(&near, 5) && near.Uint(mask) == 2);
    CHECK(!helicopter->ReturnDispatchToStation(&near, 5));  // already home
    CHECK(helicopter->ReturnDispatchToStation(&near, 4) && near.Uint(mask) == 3);
    CHECK(!helicopter->ReturnDispatchToStation(&near, 4));  // every unit home
    std::uint32_t nearest = 77;
    CHECK(helicopter->DispatchFromNearestStation(880, 880, &nearest) == &far && nearest == 3);   // the far one is nearer there
    std::uint32_t group = 0, flags = 0;
    cGZPersistResourceKey key{1, 2, 3};
    const float at[3] = {0, 0, 0};
    CHECK(helicopter->GetDispatchAutomataProperties(&near, group, key, flags, at) && group == 0x4301 && flags == 0x11882 &&
          key.type == 0 && key.group == 0 && key.instance == 0);

    // ---- the record: version 1, the station count, the next call number; Read checks the class's versions ----
    {
        OStream out;
        CHECK(helicopter->Write(out));
        CHECK(out.bytes.size() == 2 + 4 + 4);               // a plain stream carries no occupant references
        IStream in;
        in.bytes = out.bytes;
        std::uint16_t version = 0;
        std::uint32_t stations = 0, next = 0;
        std::memcpy(&version, in.bytes.data(), 2);
        std::memcpy(&stations, in.bytes.data() + 2, 4);
        std::memcpy(&next, in.bytes.data() + 6, 4);
        CHECK(version == 1 && stations == 3 && next == 6);
        auto* back = new cSC4PoliceHelicopterDispatchSource();
        back->AddRef();
        CHECK(back->Read(in) && back->NextCallNumber() == 6);
        in.at = 0;
        in.bytes[0] = 0;                                    // the helicopter accepts version 0 ...
        CHECK(back->Read(in));
        in.at = 0;
        in.bytes[0] = 2;                                    // ... but not 2
        CHECK(!back->Read(in));
        auto* planeBack = new cSC4FirePlaneDispatchSource();
        planeBack->AddRef();
        in.at = 0;
        in.bytes[0] = 0;                                    // the fire plane accepts exactly 1
        CHECK(!planeBack->Read(in));
        in.at = 0;
        in.bytes[0] = 1;
        CHECK(planeBack->Read(in) && planeBack->NextCallNumber() == 6);
        back->Release();
        planeBack->Release();
    }

    // ---- Shutdown: out of the registry, the stations released, the notifications removed, the city let go ----
    const int releases = host.listReleases;
    CHECK(helicopter->Shutdown());
    CHECK(!helicopter->IsInitialized());
    CHECK(manager->GetDispatchSource(6) == nullptr && manager->GetDispatchSource(5) == static_cast<IDispatchSource*>(plane));
    CHECK(host.listReleases == releases + 1 && helicopter->Stations().empty());
    CHECK(near.refs == 1 && far.refs == 1 && third.refs == 1);
    int removedCount = 0;
    for (const auto& n : server.removed) removedCount += n.first == static_cast<cIGZMessageTarget2*>(helicopter) ? 1 : 0;
    CHECK(removedCount == 6);
    CHECK(host.city.refs == 1);                             // the plane still holds it
    CHECK(plane->Shutdown());
    CHECK(manager->GetDispatchSource(5) == nullptr && host.city.refs == 0 && airfield.refs == 1);
    plane->Release();
    helicopter->Release();

    manager->Shutdown();
    manager->Release();
    SetAircraftDispatchSourceHost(nullptr);
    SetDispatchHost(nullptr);
    if (failures == 0) std::printf("sc4_aircraftdispatchsource: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
