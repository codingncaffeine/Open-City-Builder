// cSC4MySim — see MySim.h for the recovered layout, the seam inventory and the quirks kept. The original functions
// are at 0x1006bbbe2..0x1007aae18; each definition below names its address.
#include "sc4/MySim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rz/cRZMessage2Standard.h"
#include "sc4/Lot.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {
namespace {

IMySimHost* g_host = nullptr;
IMySimHost* Host() { return g_host; }

// Floor of a coordinate to a standard city cell: the original rounds x/16 through the float add-0.5 trick
// (0x4b400000 = 1.5 * 2^23), i.e. round-to-nearest on the scaled position.
int CellFromPosition(float value) {
    return static_cast<int>(std::nearbyint(static_cast<double>(value) * 0.0625));
}

// 0x1007a5ce4 SetTableString(cISCLua*, char const*, cIGZString*). The table is on top of the stack; the value is
// pushed as a tagged userdata that owns one reference (the AddRef the original performs).
void SetTableString(cISCLua* lua, const char* key, cIGZString* value) {
    lua->PushString(key);                                            // 0x108
    if (value) value->AddRef();                                      // the Lua userdata owns it
    lua->PushUserData(value, Host() ? Host()->LuaUserDataTag(true) : 1);   // 0x228
    lua->SetTable(-3);                                               // 0x170
}

// 0x1007a5be9 SetTableBuilding(cISCLua*, char const*, cISC4BuildingOccupant*).
void SetTableBuilding(cISCLua* lua, const char* key, cISC4BuildingOccupant* building) {
    lua->PushString(key);                                            // 0x108
    if (!building) {
        lua->PushUserData(nullptr, 0);                               // 0x228 (nil)
    } else {
        auto* subject = new cSC4AdviceSubject();
        subject->AddRef();
        subject->SetObject(static_cast<cIGZUnknown*>(building));     // 0x38
        lua->PushUserData(subject, Host() ? Host()->LuaUserDataTag(false) : 0);   // 0x228
    }
    lua->SetTable(-3);                                               // 0x170
}

}  // namespace

IMySimHost* MySimHost() { return g_host; }
void SetMySimHost(IMySimHost* host) { g_host = host; }

void MySimSetTableString(cISCLua* lua, const char* key, cIGZString* value) { SetTableString(lua, key, value); }
void MySimSetTableBuilding(cISCLua* lua, const char* key, cISC4BuildingOccupant* value) {
    SetTableBuilding(lua, key, value);
}

// 0x1007a61d0 SetTableOccupantSubject(cISCLua*, char const*, cISC4Occupant*): replaces the "destination_subject"
// entry of the table on top of the stack. The entry is read with RawGet, then pushed as a tagged userdata and RawSet.
// The original's Lua 4 lua_touserdata (0xd8) answered the tagged pointer itself; the port's Lua 5 binding boxes a tagged
// pointer, so the stored subject is read with ToUserTag (the pointer in the box).
void MySimSetTableOccupantSubject(cISCLua* lua, const char* key, cISC4Occupant* occupant) {
    if (!lua) return;
    const int top = lua->GetTop();
    lua->PushString(key);                                            // 0x108
    lua->RawGet(-2);                                                 // 0x138
    auto* existing = static_cast<cISC4AdviceSubject*>(
        static_cast<cIGZUnknown*>(lua->ToUserTag(-1)));               // 0xd8 (see above)
    if (existing) {
        existing->AddRef();
        lua->SetTop(top);                                            // drop the key and the table value
        existing->SetObject(static_cast<cIGZUnknown*>(occupant));     // 0x38
        existing->Release();
        return;
    }
    lua->SetTop(top);
    if (!Host()) return;
    cISC4AdviceSubject* created = Host()->CreateLuaSubject(occupant, false);
    if (!created) return;
    created->SetObject(static_cast<cIGZUnknown*>(occupant));          // 0x38
    lua->PushString(key);
    lua->PushUserData(created, Host()->LuaUserDataTag(false));         // 0x228 (the stack owns the reference)
    lua->RawSet(-3);                                                 // 0x178
}

// 0x1007a60fc GetCongestion(cISC4TrafficSimulator*, int, int): 0 when the traffic simulator reports no volume.
int MySimGetCongestion(cISC4TrafficSimulator* traffic, int x, int z) {
    (void)traffic;
    return Host() ? Host()->TrafficCongestion(x, z) : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// cSC4MySimDispatch::cLocation (0x1006ac5fc..0x1006ac7da)
// ---------------------------------------------------------------------------------------------------------------
bool MySimLocation::SetByOccupant(cISC4Occupant* newOccupant) {
    occupant = nullptr;
    x = z = -1000.0f;
    cellX = cellZ = lotCellX = lotCellZ = -1;
    hasOccupant = true;
    commuteLocation = false;
    data24 = data28 = 0;
    if (newOccupant) {
        occupant = newOccupant;                                      // AddRef
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        x = position[0];
        z = position[2];
        cellX = CellFromPosition(x);
        cellZ = CellFromPosition(z);
    }
    return true;
}

bool MySimLocation::operator==(const MySimLocation& other) const {
    return hasOccupant == other.hasOccupant && occupant.get() == other.occupant.get() &&
           commuteLocation == other.commuteLocation && cellX == other.cellX && cellZ == other.cellZ &&
           lotCellX == other.lotCellX && lotCellZ == other.lotCellZ;
}

bool MySimLocation::IsValid(IMySimHost& host) const {
    if (!hasOccupant) return host.AnyTravelConnection(lotCellX, lotCellZ);
    if (cellX < 0 || cellZ < 0) return false;
    return cellX < host.CityCellsX() && cellZ < host.CityCellsZ();
}

void MySimLocation::SetCellPosition(std::int32_t cx, std::int32_t cz) {
    cellX = cx;
    cellZ = cz;
    x = static_cast<float>(cx << 4);
    z = static_cast<float>(cz << 4);
}

// ---------------------------------------------------------------------------------------------------------------
// cSC4MySim: construction / lifetime (0x10079ebcc, 0x10079f23c, 0x10079f7b0, 0x10079f7ba, 0x10079f7e8..)
// ---------------------------------------------------------------------------------------------------------------
cSC4MySim::cSC4MySim() {
    if (IMySimHost* host = Host()) dispatch_ = host->CreateDispatch(this);   // +0x328 (0x1006acb3c)
    soundTimer_.Start();                                                     // +0x418
}

cSC4MySim::~cSC4MySim() {
    delete groupDistances_;   // the original only frees it in Shutdown; the port also frees it here (see MySim.h)
    groupDistances_ = nullptr;
}

bool cSC4MySim::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4MySim) {
        // the primary interface: the view cSC4MySimDispatch keeps as its owner (DoMessage / GetWealth / IsMale)
        *out = static_cast<IMySimDispatchOwner*>(this);
    } else if (iid == kGZIID_cISC4MySimLink) {
        *out = static_cast<cISC4MySim*>(this);
    } else {
        return cSC4Advisor::QueryInterface(iid, out);
    }
    AddRef();   // 0x08
    return true;
}

std::uint32_t cSC4MySim::AddRef() { return cSC4Advisor::AddRef(); }        // 0x1004431e8
std::uint32_t cSC4MySim::Release() { return cSC4Advisor::Release(); }      // 0x10044321c
std::uint32_t cSC4MySim::GetGZCLSID() const { return kGZCLSID_cSC4MySim; } // 0x1007aade4
cISC4Advisor* cSC4MySim::AsAdvisor() { return static_cast<cISC4Advisor*>(this); }   // 0x1007aadf0 (this + 0x10)

// ---------------------------------------------------------------------------------------------------------------
// Strings (the three lazily created texts of Init and the generation name of GetPersonalName)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySim::BuildStrings() {
    IMySimHost* host = Host();
    if (!emptyText_) {
        emptyText_ = new cRZString();                                        // +0x300
    }
    if (!noJobText_) {
        noJobText_ = new cRZString();                                        // +0x308
        if (host) host->LoadString(0xea231e96u, 0xaa665d3eu, *noJobText_);
    }
    if (!retiredText_) {
        retiredText_ = new cRZString();                                      // +0x310
        if (host) host->LoadString(0xea231e96u, 0x0a665d4cu, *retiredText_);
    }
}

cRZString& cSC4MySim::EmptyString() {
    static cRZString empty;
    return emptyText_ ? *emptyText_ : empty;
}

cRZString& cSC4MySim::NoJobString() {
    static cRZString empty;
    return noJobText_ ? *noJobText_ : empty;
}

cRZString& cSC4MySim::RetiredString() {
    static cRZString empty;
    return retiredText_ ? *retiredText_ : empty;
}

// 0x1007a261a GetPersonalName(): generation 1 returns the base personal name; from generation 2 on the sim gets a
// "<personal name> <suffix>" name built from the resource (generations below 0xb) or "<personal name> <generation>".
cIGZString& cSC4MySim::GetPersonalName() {
    if (generation_ < 2) return cSC4Advisor::GetPersonalName();
    if (!generationName_) {
        generationName_ = new cRZString();                             // +0x268
        bool named = false;
        if (generation_ < 0xb) {
            cSLString suffix(0xea231e96u, static_cast<std::uint32_t>(generation_) + 0x6a56248eu, true);
            if (suffix.Strlen() != 0) {
                generationName_->Copy(cSC4Advisor::GetPersonalName());
                generationName_->AppendChars(" ", 1);
                generationName_->AppendString(suffix);
                named = true;
            }
        }
        if (!named) {
            generationName_->Sprintf("%s %d", cSC4Advisor::GetPersonalName().ToChar(),
                                     static_cast<int>(generation_));
        }
    }
    return *generationName_;
}

// ---------------------------------------------------------------------------------------------------------------
// Init / Shutdown / InitFromTuning (0x10079f8ea, 0x1007a1fe8, 0x1007a191c)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySim::Init(int advisorType) {
    IMySimHost* host = Host();
    balloonsTimer_.Restart();                                             // +0x3f8
    if (!cSC4Advisor::Init(advisorType)) return false;
    if (host) lua_ = host->Lua();                                         // spCity 0x288 -> 0x50
    BuildStrings();
    if (state_ != 0) {
        const bool hadDispatch = dispatch_ != nullptr;
        if (!dispatch_ && host) {
            dispatch_ = host->CreateDispatch(this);                       // 0x1006acb3c
            if (dispatch_) dispatch_->Init();                             // 0x1006ad11a
        }
        if (residence_ && host) {
            lot_ = host->LotByBuilding(residence_);                       // spLotManager 0x70
            if (lot_) {
                int x = 0, z = 0;
                lot_->GetLocation(x, z);                                  // lot vslot 0xa8
                residenceCellX_ = x;
                residenceCellZ_ = z;
            }
        }
        if (!job_ && (jobCell_[0] != 0 || jobCell_[1] != 0)) {
            cISC4Occupant* occupant = host ? host->OccupantAtCell(jobCell_[0], jobCell_[1]) : nullptr;
            cISC4BuildingOccupant* building = nullptr;
            if (occupant) {
                occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&building));
                occupant->Release();
            }
            neighborJob_ = building;                                     // borrowed from the occupant manager
            commuteCheck_ = true;                                        // +0x240
        } else {
            neighborJob_ = nullptr;
        }
        if (!hadDispatch && dispatch_) {
            MySimLocation location;
            if (residence_) location.SetByOccupant(residence_->AsOccupant());
            dispatch_->SetDefaultDestination(location);                   // 0x1006b00de
            if (neighborJob_) ShowLocation();
        }
        UpdateVariables();
        ActivateLuaContext();                                             // vslot 0x2d8
    }
    init_balloons();
    InitFromTuning();
    return true;
}

// 0x1007a191c InitFromTuning(): reads the "tuning_constants" script table; the balloon durations go to the balloon
// window proc and the trip constants to the dispatch.
bool cSC4MySim::InitFromTuning() {
    cISCLua* lua = lua_.get();
    if (!lua) return false;
    // cTablePropertyReader is the IScriptLua flavour; the sim's context is the cISCLua itself, so the tuning table is
    // read with the same stack sequence here (GetGlobal "tuning_constants", RawGet the field, ToNumber, Pop).
    const int savedTop = lua->GetTop();
    lua->GetGlobal("tuning_constants");                                  // 0x128
    const bool tableValid = lua->IsTable(-1);                            // 0x240
    auto number = [&](const char* key, std::uint32_t fallback) {
        if (!tableValid) return fallback;
        lua->PushString(key);                                            // 0x108
        lua->RawGet(-2);                                                 // 0x138
        const int type = lua->Type(-1);                                  // 0x60
        std::uint32_t value = fallback;
        if (type == 3 || type == 4) {                                    // number or string
            value = static_cast<std::uint32_t>(static_cast<std::int64_t>(lua->ToNumber(-1)));   // 0xb8
        }
        lua->Pop(1);                                                     // 0x58
        return value;
    };
    if (balloonWinProc_) {
        balloonWinProc_->SetFadeDuration(static_cast<int>(number("MYSIM_BALLOON_FADE_DURATION", 300)));       // 0x50
        balloonWinProc_->SetShownDuration(static_cast<int>(number("MYSIM_BALLOON_SHOWN_DURATION", 0x5dc))); // 0x48
        balloonWinProc_->SetHiddenDuration(static_cast<int>(number("MYSIM_BALLOON_HIDDEN_DURATION", 300))); // 0x40
    }
    if (dispatch_) {
        dispatch_->SetMinTimeAtLocation(static_cast<int>(number("MYSIM_TRIP_MIN_TIME_AT_LOCATION", 1)));         // +0x30
        dispatch_->SetMinTimeAtSwitch(static_cast<int>(number("MYSIM_TRIP_MIN_TIME_AT_TRANSIT_SWITCH", 1)));    // +0x34
        dispatch_->SetMinTimeIdling(static_cast<int>(number("MYSIM_TRIP_MIN_TIME_IDLE", 1)));                    // +0x2c
        dispatch_->SetDestinationProximityMin(static_cast<int>(number("MYSIM_TRIP_DESTINATION_PROXIMITY_MIN", 10)));   // +0x28
        dispatch_->SetDestinationApproachDistance(static_cast<int>(number("MYSIM_TRIP_APPROACH_DISTANCE", 10)));      // +0x24
    }
    localAreaSize_ = static_cast<int>(number("MYSIM_TRIP_LOCAL_AREA_SIZE", 10));       // +0x3ac
    defaultCarModel_ = static_cast<int>(number("MYSIM_TRIP_DEFAULT_CAR_MODEL", 0));    // +0x3a8
    lua->SetTop(savedTop);                                               // the reader's destructor
    return true;
}

// 0x1007a1fe8 Shutdown(): unregisters the messages, drops the dispatch/balloon window and every object the sim owns,
// then shuts the advisor base down. The member destructors release whatever is left.
bool cSC4MySim::Shutdown() {
    IMySimHost* host = Host();
    if (balloonsInitialized_) {                                        // +0x2e4
        if (host) {
            host->RemoveFromGameDataRegistry();                        // registry vslot 0x30(0)
            host->UnregisterInputControlTarget(static_cast<cIGZMessageTarget2*>(this));   // ATM 0x30(this, 0xff)
        }
        if (dispatch_) dispatch_->Shutdown();
        dispatch_ = nullptr;
        if (balloonWinProc_) balloonWinProc_->Shutdown();
        balloonWinProc_ = nullptr;
        if (host) {
            host->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimNoop);
            host->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimInput);
            host->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimInput2);
            host->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimToolMessage);
            host->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimChanged);
            host->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimHideAll);
        }
        balloonsInitialized_ = false;
    }
    emptyText_ = nullptr;
    noJobText_ = nullptr;
    retiredText_ = nullptr;
    residence_ = nullptr;
    lot_ = nullptr;
    job_ = nullptr;
    neighborJob_ = nullptr;
    jobCell_[0] = jobCell_[1] = jobCell_[2] = jobCell_[3] = 0;
    if (callout_) {
        HideResidence();                                               // vslot 0x1e0
        callout_ = nullptr;
    }
    if (unknown1a0_) {
        // 0x1a0 is an unidentified object; the original calls its slot 0x68 with 0 before releasing it.
        unknown1a0_ = nullptr;
    }
    homeBuildingText_ = nullptr;
    jobBuildingText_ = nullptr;
    jobCityText_ = nullptr;
    generationName_ = nullptr;
    RemoveHomeSound();
    if (luaTableRef_ != 0) {
        if (lua_) lua_->Unref(luaTableRef_);                           // 0x1f0
        luaTableRef_ = 0;
    }
    delete groupDistances_;
    groupDistances_ = nullptr;
    unknown278_ = nullptr;
    unknown280_ = nullptr;
    emptyText_ = nullptr;
    retiredText_ = nullptr;
    noJobText_ = nullptr;
    lua_ = nullptr;
    ghost_ = nullptr;
    if (host) host->SetCurrentViewInputControl(nullptr);               // mspViewInputControl = 0
    state_ = 0;
    if (pylon_) {
        pylon_->Shutdown();
        pylon_ = nullptr;
        pylonLocked_ = false;
    }
    dispatchToolActive_ = false;
    pylonLocked_ = false;
    return cSC4Advisor::Shutdown();
}

// 0x1007a2502 RemoveHomeSound()
void cSC4MySim::RemoveHomeSound() {
    if (!homeSound_) return;
    if (homeSound_->IsPlaying() && Host()) Host()->DestroyHomeSound(homeSound_.get());   // spPropManager 0xe8
    homeSound_ = nullptr;
}

// 0x1007a25ae CancelViewInput(): drops the global view input control when it is the view's current one.
void cSC4MySim::CancelViewInput() {
    IMySimHost* host = Host();
    if (!host) return;
    if (host->ViewCurrentInputControl() != nullptr) {
        cIGZUnknown* current = host->CurrentViewInputControl();
        if (!current) return;
        if (host->ViewCurrentInputControl() == current) host->ViewSetInputControl(nullptr);   // spView3D 0xd0(0)
        host->SetCurrentViewInputControl(nullptr);
    }
}


// ---------------------------------------------------------------------------------------------------------------
// Identity / state accessors (0x1007a7e06..0x1007a849e)
// ---------------------------------------------------------------------------------------------------------------
std::uint8_t cSC4MySim::GetSlot() const { return slot_; }                              // 0x1007a7f64
cISC4BuildingOccupant* cSC4MySim::GetResidence() const { return residence_.get(); }    // 0x1007a7e06
cISC4BuildingOccupant* cSC4MySim::GetJob() const { return job_.get(); }                // 0x1007a7e1e
cGZPersistResourceKey cSC4MySim::GetVehicleKey() const { return vehicleKey_; }         // 0x1007a7ef2
std::uint8_t cSC4MySim::GetAge() const { return age_; }                                // 0x1007a7f48
std::uint8_t cSC4MySim::GetSign() const { return zodiac_; }                            // 0x1007a7f30
int cSC4MySim::GetWealth() const { return wealth_; }                                   // 0x1007a7f7c
bool cSC4MySim::IsMale() const { return male_ != 0; }                                  // 0x1007a845c
bool cSC4MySim::IsActive() const { return state_ != 0; }                               // 0x1007a8472
const cRZString& cSC4MySim::GetBio() const { return bio_; }                            // 0x1007a8490
std::uint32_t cSC4MySim::GetBitmapID() const { return bitmapId_; }                     // 0x1007a7f92
const cRZString& cSC4MySim::GetBitmapFile() const { return bitmapFile_; }              // 0x1007a7faa
std::uint32_t cSC4MySim::GetModelButtonBitmapID(bool pedestrian) const {               // 0x1006bbbe2
    return pedestrian ? pedestrianBitmapId_ : carBitmapId_;
}

// 0x1007a81f8 GetHomeName(): the "home_building" script text, or the empty text before the script ran.
const cRZString& cSC4MySim::GetHomeName() const {
    if (homeBuildingText_) return *homeBuildingText_;
    return const_cast<cSC4MySim*>(this)->EmptyString();
}

// 0x1007a8240 GetJobName(): retired -> "retired", no job -> "no job", otherwise the "job_building" text.
const cRZString& cSC4MySim::GetJobName() const {
    if (retired_) return const_cast<cSC4MySim*>(this)->RetiredString();
    if (!job_ && !neighborJob_) return const_cast<cSC4MySim*>(this)->NoJobString();
    if (jobBuildingText_) return *jobBuildingText_;
    return const_cast<cSC4MySim*>(this)->EmptyString();
}

// 0x1007a82ea GetJobTitle(): same as GetJobName but the separate job-title text (+0x258).
const cRZString& cSC4MySim::GetJobTitle() const {
    if (retired_) return const_cast<cSC4MySim*>(this)->RetiredString();
    if (!job_ && !neighborJob_) return const_cast<cSC4MySim*>(this)->NoJobString();
    if (jobTitleText_) return *jobTitleText_;
    return const_cast<cSC4MySim*>(this)->EmptyString();
}

// 0x1007a8394 GetEQAsText(): "%d" of the education quotient truncated through int.
bool cSC4MySim::GetEQAsText(cRZString& out) {
    out.Sprintf("%d", static_cast<int>(eq_));
    return true;
}

// 0x1007a83e4 GetHQAsText(): nothing while the sim is not in the city; otherwise one of the three texts by quotient.
bool cSC4MySim::GetHQAsText(cRZString& out) {
    if (state_ == 0) return false;
    std::uint32_t group = kHQTextGroupLow;
    std::uint32_t instance = kHQTextLow;
    if (hq_ >= 50.0f) {
        if (hq_ >= 100.0f) {
            group = kHQTextGroupLow;
            instance = kHQTextHigh;
        } else {
            group = kHQTextGroupMid;
            instance = kHQTextMid;
        }
    }
    if (Host()) Host()->LoadString(group, instance, out);
    return true;
}

// 0x1007a7fc6 GetBitmapBuffer(): the sim's own bitmap or, when it has no bitmap id, the bitmap file.
bool cSC4MySim::GetBitmapBuffer(cIGZBuffer** out) const {
    if (!out) return false;
    *out = nullptr;
    if (!Host()) return false;
    if (bitmapId_ == 0) {
        return Host()->LoadMySimBitmap(0, &bitmapFile_, out);
    }
    return Host()->LoadMySimBitmap(bitmapId_, nullptr, out);
}

// 0x1007a7e36 GetJobTitle(cISC4BuildingOccupant*, cIGZString&): the job title resource for the building's purpose.
void cSC4MySim::GetJobTitle(cISC4BuildingOccupant* building, cIGZString& out) {
    if (!building) return;
    const std::uint32_t purpose = building->GetBuildingProfile().purpose;   // building vslot 0x58 -> first int
    std::uint32_t group = kJobTitleGroup;
    std::uint32_t instance = 0;
    if (wealth_ == 3) {
        instance = kJobTitleWealth3Kind5To8;
        if (purpose - 5u >= 4u) instance = kJobTitleWealth3Other - static_cast<std::uint32_t>(purpose == 2);
    } else if (wealth_ == 2) {
        instance = kJobTitleWealth2Kind5To8;
        if (purpose - 5u >= 4u) instance = kJobTitleWealth2Other - static_cast<std::uint32_t>(purpose == 2);
    } else if (wealth_ == 1) {
        if (purpose - 5u >= 4u) {
            instance = kJobTitleWealth1Other - static_cast<std::uint32_t>(purpose == 2);
        } else {
            instance = (purpose == 5 ? 1u : 0u) | kJobTitleWealth1Kind5To8;   // quirk: OR, not +
        }
    } else {
        return;
    }
    if (Host()) Host()->LoadString(group, instance, out);
}

// 0x1007a8f4e CommandWouldTrigger(): inactive sims never trigger; otherwise the Lua context is refreshed first.
bool cSC4MySim::CommandWouldTrigger(std::uint32_t commandId) {
    if (state_ == 0) return false;
    ActivateLuaContext();                                                // vslot 0x2d8
    return cSC4Advisor::CommandWouldTrigger(commandId);
}

// ---------------------------------------------------------------------------------------------------------------
// Serialization (0x1007a27ce Write, 0x1007a2b1c Read) — record version 0x11
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySim::Write(cIGZOStream& stream) const {
    stream.SetUint16(kMySimStreamVersion);
    const std::uint32_t state = static_cast<std::uint32_t>(state_);
    stream.SetUint8(static_cast<std::uint8_t>(state & 0xff));
    if (state != 0) {
        IMySimHost* host = Host();
        stream.SetUint8(slot_);
        if (host) host->WriteOccupant(stream, residence_.get());
        stream.SetSint32(residenceCellX_);
        stream.SetSint32(residenceCellZ_);
        if (host) host->WriteOccupant(stream, job_.get());
        for (int cell : jobCell_) stream.SetSint32(cell);
        stream.SetUint32(static_cast<std::uint32_t>(unemployedDays_));
        stream << vehicleKey_;
        stream.SetUint8(static_cast<std::uint8_t>(wealth_));
        stream.SetUint16(generation_);
        stream.SetUint8(zodiac_);
        stream << (male_ != 0);
        stream.SetGZStr(bio_);
        stream.SetUint8(age_);
        stream.SetUint8(birthMonth_);
        stream.SetUint32(bitmapId_);
        stream.SetGZStr(bitmapFile_);
        stream.SetUint32(static_cast<std::uint32_t>(trackHandle_));
        stream << retired_;
        stream.SetUint32(static_cast<std::uint32_t>(lastLocalDisaster_));
        stream.SetFloat32(posX_);
        stream.SetFloat32(posY_);
        stream.SetFloat32(posZ_);
        stream.SetUint8(eqHistoryIndex_);
        stream.SetFloat32(deltaEq_);
        for (float value : eqHistory_) stream.SetFloat32(value);
        stream.SetUint8(hqHistoryIndex_);
        stream.SetFloat32(deltaHq_);
        for (float value : hqHistory_) stream.SetFloat32(value);
        stream << carModelKey_;
        stream << pedestrianModelKey_;
        stream.SetUint32(carBitmapId_);
        stream.SetUint32(pedestrianBitmapId_);
        if (host) host->WriteDispatch(stream, dispatch_.get());
        stream << pylonTimer_;
        if (!pylon_) {
            stream << false;
        } else {
            const bool dead = pylon_->IsDead();
            stream << !dead;
            if (!dead && host) host->WritePylon(stream, pylon_.get());
        }
        stream << pylonLocked_;
        stream.SetUint32(static_cast<std::uint32_t>(lastPlayerTripDay_));
    }
    return cSC4Advisor::Write(stream);
}

bool cSC4MySim::Read(cIGZIStream& stream) {
    IMySimHost* host = Host();
    std::uint16_t version = 0;
    std::uint8_t state = 0;
    stream.GetUint16(version);
    stream.GetUint8(state);
    state_ = static_cast<std::int32_t>(state);
    if (state == 0) return cSC4Advisor::Read(stream);
    stream.GetUint8(slot_);
    residence_ = nullptr;
    if (host) {
        cISC4BuildingOccupant* building = nullptr;
        host->ReadOccupant(stream, &building);
        residence_ = building;
        if (building) building->Release();
    }
    stream.GetSint32(residenceCellX_);
    stream.GetSint32(residenceCellZ_);
    job_ = nullptr;
    if (host) {
        cISC4BuildingOccupant* building = nullptr;
        host->ReadOccupant(stream, &building);
        job_ = building;
        if (building) building->Release();
    }
    for (int& cell : jobCell_) stream.GetSint32(cell);
    std::uint32_t value = 0;
    stream.GetUint32(value);
    unemployedDays_ = static_cast<std::int32_t>(value);
    stream >> vehicleKey_;
    stream.GetUint8(state);
    wealth_ = static_cast<std::int32_t>(state);
    stream.GetUint16(generation_);
    stream.GetUint8(zodiac_);
    bool male = false;
    stream >> male;
    male_ = male ? 1 : 0;
    stream.GetGZStr(bio_);
    stream.GetUint8(age_);
    stream.GetUint8(birthMonth_);
    stream.GetUint32(bitmapId_);
    stream.GetGZStr(bitmapFile_);
    stream.GetUint32(value);
    trackHandle_ = static_cast<std::int32_t>(value);
    stream >> retired_;
    stream.GetUint32(value);
    lastLocalDisaster_ = static_cast<std::int32_t>(value);
    stream.GetFloat32(posX_);
    stream.GetFloat32(posY_);
    stream.GetFloat32(posZ_);
    stream.GetUint8(eqHistoryIndex_);
    stream.GetFloat32(deltaEq_);
    for (float& entry : eqHistory_) stream.GetFloat32(entry);
    stream.GetUint8(hqHistoryIndex_);
    stream.GetFloat32(deltaHq_);
    for (float& entry : hqHistory_) stream.GetFloat32(entry);
    if (version < 0xb) {
        dispatch_ = nullptr;
    } else {
        if (version < 0xf) {
            std::uint32_t skipped = 0;
            for (int i = 0; i < 5; ++i) stream.GetUint32(skipped);
        } else {
            stream >> carModelKey_;
            stream >> pedestrianModelKey_;
        }
        if (version > 0x10) {
            stream.GetUint32(carBitmapId_);
            stream.GetUint32(pedestrianBitmapId_);
        }
        if (version < 0xc) {
            dispatch_ = nullptr;
        } else if (version == 0xc) {
            if (dispatch_ && host) {
                IMySimDispatch* read = nullptr;
                host->ReadDispatch(stream, &read);
                dispatch_ = read;
                if (read) read->Release();
            }
        } else {
            if (dispatch_) {
                dispatch_->Shutdown();
                dispatch_ = nullptr;
            }
            if (host) {
                IMySimDispatch* read = nullptr;
                host->ReadDispatch(stream, &read);
                dispatch_ = read;
                if (read) read->Release();
            }
        }
        stream >> pylonTimer_;
        bool hasPylon = false;
        stream >> hasPylon;
        if (hasPylon) {
            pylon_ = nullptr;
            if (host) {
                IMySimPylon* read = nullptr;
                host->ReadPylon(stream, &read);
                pylon_ = read;
                if (read) read->Release();
            }
        }
    }
    if (version > 0xd) stream >> pylonLocked_;
    if (version > 0xf) {
        stream.GetUint32(value);
        lastPlayerTripDay_ = static_cast<std::int32_t>(value);
    }
    return cSC4Advisor::Read(stream);
}

// ---------------------------------------------------------------------------------------------------------------
// Balloons (0x1007a10f0 init_balloons, 0x1007a2fc2..0x1007a3308 the queues and getters, 0x1007a3bd6 simulate)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySim::init_balloons() {
    IMySimHost* host = Host();
    balloons_.clear();
    balloonQueue_.clear();
    eventQueue_.clear();
    eventBalloons_.clear();

    std::map<int, int> adviceToBalloon;
    const std::vector<cSC4Advisor::cAdviceInfo>& infos = AdviceInfos();
    for (std::size_t i = 0; i < infos.size(); ++i) {
        cISC4MySimBalloonAdvice* balloon = nullptr;
        if (!infos[i].advice) continue;
        if (!infos[i].advice->QueryInterface(kGZIID_cISC4MySimBalloonAdvice, reinterpret_cast<void**>(&balloon)) ||
            !balloon) {
            continue;
        }
        adviceToBalloon[static_cast<int>(i)] = static_cast<int>(balloons_.size());
        balloon->SetStatusFlags(7, false);                                    // 0x68: clear shown/blocked/text
        MySimBalloonInfo info;
        info.advice = balloon;                                                // the vector takes the reference
        info.nextDay = DayNumber();                                           // +0x94
        info.startTime = 0;
        balloons_.push_back(info);
        balloon->Release();                                                   // QueryInterface's reference
    }

    // Rebuild the event -> balloon index map from the base class's event -> advice map; events whose advice is not a
    // balloon advice are erased from the base map (kept quirk).
    std::vector<int> eraseEvents;
    for (const std::pair<const int, int>& entry : EventAdvices()) {
        std::map<int, int>::const_iterator found = adviceToBalloon.find(entry.second);
        if (found != adviceToBalloon.end()) {
            eventBalloons_[entry.first] = found->second;
        } else {
            eraseEvents.push_back(entry.first);
        }
    }
    for (int eventId : eraseEvents) EraseEventAdvice(eventId);

    if (!balloonsInitialized_) {
        if (!dispatch_ && host) dispatch_ = host->CreateDispatch(this);
        if (host) {
            balloonWinProc_ = host->CreateBalloonWinProc(this);
            if (balloonWinProc_) balloonWinProc_->Init();
            host->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimNoop);
            host->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimInput);
            host->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimInput2);
            host->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimToolMessage);
            host->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimChanged);
            host->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageMySimHideAll);
            host->RegisterInputControlTarget(static_cast<cIGZMessageTarget2*>(this));
            balloonsInitialized_ = true;
            host->AddToGameDataRegistry(*this);
        }
    }
}

// 0x1007a2fc2 empty_all_balloon_queues(): pop every event balloon, then every advice balloon (max 1000 each).
void cSC4MySim::empty_all_balloon_queues() {
    if (pop_balloon_queue(1, 0xffff)) {
        for (int i = -1000; i != 0; ++i) {
            if (!pop_balloon_queue(1, 0xffff)) break;
        }
    }
    if (pop_balloon_queue(2, 0xffff)) {
        for (int i = -1000; i != 0; ++i) {
            if (!pop_balloon_queue(2, 0xffff)) return;
        }
    }
}

// 0x1007a3038 pop_balloon_queue(queueType, statusMask): shows the front balloon when the advice answers the mask.
bool cSC4MySim::pop_balloon_queue(int queueType, int statusMask) {
    std::list<int>& queue = (queueType == 1) ? eventQueue_ : balloonQueue_;
    if (queue.empty()) return false;
    const int index = queue.front();
    if (index < 0 || static_cast<std::size_t>(index) >= balloons_.size()) return false;
    MySimBalloonInfo& info = balloons_[static_cast<std::size_t>(index)];
    if (!info.advice) return false;
    if (!info.advice->CheckStatusFlags(statusMask)) return false;            // 0x70
    cISC4Advice* advice = info.advice->AsISC4Advice();                       // 0x18
    info.nextDay = DayNumber() + advice->GetFrequencyInDays();               // 0x80
    info.advice->SetStatusFlags(7, false);                                   // 0x68
    queue.pop_front();
    return true;
}

// 0x1007a3118 push_balloon_queue(queueType, index): queues the balloon at the front, resetting its day counters.
void cSC4MySim::push_balloon_queue(int queueType, int index) {
    if (index < 0 || !balloonsEnabled_) return;
    if (static_cast<std::size_t>(index) >= balloons_.size()) return;
    MySimBalloonInfo& info = balloons_[static_cast<std::size_t>(index)];
    info.startTime = balloonStartTime_;                                      // +0x318
    info.nextDay = 0;
    if (info.advice) info.advice->SetStatusFlags(1, true);                   // 0x68: shown
    std::list<int>& queue = (queueType == 1) ? eventQueue_ : balloonQueue_;
    queue.push_front(index);
}

// 0x1007a31b6 get_next_balloon(queueType): the first queued balloon that is not blocked and whose trigger fires;
// balloons that fail are popped with mask 0xffff and the walk continues.
cISC4MySimBalloonAdvice* cSC4MySim::get_next_balloon(int queueType) {
    pop_balloon_queue(queueType, 4);
    std::list<int>& queue = (queueType == 1) ? eventQueue_ : balloonQueue_;
    for (;;) {
        if (queue.empty()) return nullptr;
        const int index = queue.front();
        if (index >= 0 && static_cast<std::size_t>(index) < balloons_.size()) {
            cISC4MySimBalloonAdvice* balloon = balloons_[static_cast<std::size_t>(index)].advice.get();
            if (balloon && !balloon->CheckStatusFlags(4) && balloon->GetRawTrigger()) return balloon;
        }
        pop_balloon_queue(queueType, 0xffff);
    }
}

// 0x1007a3260 / 0x1007a3286 (2 = advice balloons, 1 = event balloons).
cISC4MySimBalloonAdvice* cSC4MySim::GetNextBalloonAdvice() { return get_next_balloon(2); }
cISC4MySimBalloonAdvice* cSC4MySim::GetNextBalloonEventAdvice() { return get_next_balloon(1); }

// 0x1007a32ac / 0x1007a3308: the balloon at the front of the queue (null when the queue is empty).
cISC4MySimBalloonAdvice* cSC4MySim::GetCurrentBalloonAdvice() const {
    if (balloonQueue_.empty()) return nullptr;
    const int index = balloonQueue_.front();
    if (index < 0 || static_cast<std::size_t>(index) >= balloons_.size()) return nullptr;
    return balloons_[static_cast<std::size_t>(index)].advice.get();
}

cISC4MySimBalloonAdvice* cSC4MySim::GetCurrentBalloonEventAdvice() const {
    if (eventQueue_.empty()) return nullptr;
    const int index = eventQueue_.front();
    if (index < 0 || static_cast<std::size_t>(index) >= balloons_.size()) return nullptr;
    return balloons_[static_cast<std::size_t>(index)].advice.get();
}

// 0x1007a3bd6 simulate_balloons(force): walks a fraction (1/7 + 1, or all when forced) of the balloons and queues the
// ones whose trigger fires; balloons that do not fire yet get their next day moved by their frequency.
void cSC4MySim::simulate_balloons(bool force) {
    if (!balloonsEnabled_) return;
    const int count = static_cast<int>(balloons_.size());
    if (count == 0) return;
    ActivateLuaContext();                                                    // vslot 0x2d8
    int remaining = force ? count : static_cast<int>(static_cast<float>(count) * 0.14285715f + 1.0f);
    int index = balloonIndex_;
    while (remaining-- > 0) {
        if (count <= index) {
            balloonIndex_ = 0;
            index = 0;
        }
        MySimBalloonInfo& info = balloons_[static_cast<std::size_t>(index)];
        const int pushedIndex = balloonIndex_;
        if (info.advice) {
            cISC4Advice* advice = info.advice->AsISC4Advice();               // 0x18
            const bool blocked = info.advice->CheckStatusFlags(1);           // 0x70
            if (!blocked) {
                if (!advice->CheckFlags(kAdviceFlagEvent)) {                 // 0x78(0x40)
                    if (info.nextDay <= DayNumber() || force) {
                        if (!info.advice->GetRawTrigger()) {                 // 0x60
                            info.nextDay = DayNumber() + advice->GetFrequencyInDays();   // 0x80
                        } else {
                            push_balloon_queue(2, pushedIndex);
                        }
                    }
                }
            }
        }
        index = ++balloonIndex_;
    }
}

// 0x1007a3d3a PickIdleOffset(): runs the "mysim_get_idle_animation_offset" script function and applies the result to
// the walk model.
int cSC4MySim::PickIdleOffset() {
    cISCLua* lua = lua_.get();
    if (!lua) return -1;
    const int top = lua->GetTop();                                           // 0x30
    ActivateLuaContext();
    lua->GetGlobal("game");                                                  // 0x128
    int offset = -1;
    if (lua->Type(-1) == 5 /* table */) {                                    // 0x60
        lua->PushString("mysim_get_idle_animation_offset");                  // 0x108
        lua->GetTable(-2);                                                   // 0x130
        if (lua->Type(-1) == 6 /* function */) {
            lua->GetRef(luaTableRef_);                                       // 0x158
            if (lua->CallProtected(1, 1) == 0) {                             // 0x198
                offset = static_cast<int>(lua->ToNumber(-1));                // 0xb8
            }
        }
    }
    if (dispatch_) dispatch_->SetModelOffset(2, offset, false);              // 0x1006b000a
    lua->SetTop(top);                                                        // 0x38
    return offset;
}

// 0x1007a5690 is_pylon_location(): the location is the pylon's own occupant or its linked occupant.
bool cSC4MySim::is_pylon_location(const MySimLocation& location) {
    if (!pylon_ || !pylonLocked_) return false;
    cISC4Occupant* occupant = pylon_->AsOccupant();                          // 0x18
    if (location.occupant.get() == occupant) return true;
    return location.occupant.get() == pylon_->GetLinkedOccupant();           // 0x90
}

// ---------------------------------------------------------------------------------------------------------------
// The script variables (0x10079fcb2 UpdateVariables, 0x1007a3e66 UpdateTripVariables, 0x1007a5800
// UpdateQuickVariables, 0x1007a5d5e UpdateLocalBuildings, 0x1007a6160 UpdateJobTitle)
// ---------------------------------------------------------------------------------------------------------------

// PARTIAL: the original refreshes ~40 variables from the power, water, fire, police, crime, pollution, residential,
// building development and traffic simulators (home_has_power, local_crime, ...), which are not recovered under
// src/ yet. The port keeps the UpdateQuickVariables() call and the Lua guard; the per-simulator values are missing.
// see the ledger PARTIAL entry for 0x10079fcb2.
void cSC4MySim::UpdateVariables() {
    UpdateQuickVariables();
    if (!lua_ || luaTableRef_ < 1) return;
    // The remaining fields need the simulators listed above; nothing is written after the guard.
}

// PARTIAL: the original also writes the transit-switch / travel-type block (trip_at_transit_switch, trip_idle,
// trip_on_bus/foot/subway/car/train/elevated train/monorail, last_trip_type) and the traffic simulator's local-area
// congestion sweep (0x1007a41xx..0x1007a5690). Only the first block (trip_in_transit, trip_at_location and the
// destination subject) is ported; the rest is missing. see the ledger PARTIAL entry for 0x1007a3e66.
void cSC4MySim::UpdateTripVariables(int fromState, int toState, int legFlags, int travelFlags, bool force) {
    (void)legFlags;
    (void)travelFlags;
    cISCLua* lua = lua_.get();
    if (!lua) return;
    if (luaTableRef_ < 1) return;
    if (!IsActive()) return;
    lua->GetRef(luaTableRef_);                                               // 0x158
    if (fromState != toState || force) {
        lua->PushString("trip_in_transit");                                  // 0x108
        lua->PushNumber(fromState != toState ? 1.0 : 0.0);                   // 0xf8
        lua->SetTable(-3);                                                   // 0x170
        lua->PushString("trip_at_location");
        lua->PushNumber(fromState == 3 ? 1.0 : 0.0);
        lua->SetTable(-3);
        if (fromState == 2) {
            if (dispatch_) {
                const MySimLocation& destination = dispatch_->GetCurrentDestination();
                MySimSetTableOccupantSubject(lua, "destination_subject", destination.occupant.get());
            }
        } else if (fromState == 3) {
            if (dispatch_) {
                const MySimLocation& location = dispatch_->GetCurrentLocation();
                if (!(location == dispatch_->GetCurrentCommuteDestination()) &&
                    !(location == dispatch_->GetCurrentDefaultDestination())) {
                    MySimSetTableOccupantSubject(lua, "destination_subject", location.occupant.get());
                } else {
                    MySimSetTableOccupantSubject(lua, "destination_subject", nullptr);
                }
            }
        }
    }
    lua->Pop(1);                                                             // 0x58
}

// 0x1007a5800 UpdateQuickVariables()
void cSC4MySim::UpdateQuickVariables() {
    cISCLua* lua = lua_.get();
    if (!lua) return;
    if (luaTableRef_ == 0) CreateLuaTable();
    lua = lua_.get();
    if (!lua) return;
    lua->GetRef(luaTableRef_);                                               // 0x158
    auto setNumber = [lua](const char* key, double value) {
        lua->PushString(key);                                                // 0x108
        lua->PushNumber(value);                                              // 0xf8
        lua->SetTable(-3);                                                   // 0x170
    };
    setNumber("state", static_cast<double>(state_));
    setNumber("age", static_cast<double>(age_));
    setNumber("wealth", static_cast<double>(wealth_));
    setNumber("generation", static_cast<double>(generation_));
    setNumber("retired", static_cast<double>(static_cast<std::int8_t>(retired_ ? 1 : 0)));
    setNumber("zodiac", static_cast<double>(zodiac_));
    setNumber("is_male", static_cast<double>(static_cast<std::int8_t>(male_ ? 1 : 0)));
    setNumber("last_local_disaster", static_cast<double>(lastLocalDisaster_));
    SetTableBuilding(lua, "home_location", residence_.get());
    SetTableBuilding(lua, "job_location", job_.get());
    cRZAutoRefCount<cRZString> name(new cRZString(GetPersonalName()));
    SetTableString(lua, "name", name.get());
    lua->Pop(1);                                                             // 0x58
}

// 0x1007a5d5e UpdateLocalBuildings(): per occupant group of the buildings within 20 cells of the residence, the
// distance to the closest such building (the map at +0x2e8 that DistanceToClosest reads).
void cSC4MySim::UpdateLocalBuildings() {
    IMySimHost* host = Host();
    if (!host || !residence_) return;
    if (!groupDistances_) groupDistances_ = new std::map<std::uint32_t, std::uint32_t>();
    else groupDistances_->clear();

    std::vector<cISC4BuildingOccupant*> buildings;
    host->BuildingsNearResidence(residenceCellX_, residenceCellZ_, 0x14, buildings);
    for (cISC4BuildingOccupant* building : buildings) {
        if (!building) continue;
        cISC4Occupant* occupant = building->AsOccupant();
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        const float dx = posX_ - position[0];
        const float dz = posZ_ - position[2];
        const std::uint32_t distance = static_cast<std::uint32_t>(
            static_cast<long>(std::sqrt(dx * dx + dz * dz)));
        std::vector<std::uint32_t> groups;
        host->BuildingOccupantGroups(building->GetBuildingType(), groups);
        for (std::uint32_t group : groups) {
            std::uint32_t& value = (*groupDistances_)[group];
            if (value == 0 || distance < value) value = distance;
        }
    }
}

// 0x1007a6160 UpdateJobTitle(): refreshes the cached job title text (+0x258), if one exists. Nothing in the recovered
// code ever creates that string, so this normally does nothing (kept).
void cSC4MySim::UpdateJobTitle() {
    if (!jobTitleText_) return;
    if (!neighborJob_) {
        if (job_) GetJobTitle(job_.get(), *jobTitleText_);
    } else if (wealth_ >= 1 && wealth_ <= 3) {
        static const std::uint32_t kTitles[3] = {0x8a538a68u, 0x8a538a69u, 0x8a538a70u};
        if (Host()) Host()->LoadString(kJobTitleGroup, kTitles[static_cast<std::size_t>(wealth_ - 1)], *jobTitleText_);
    }
}


// ---------------------------------------------------------------------------------------------------------------
// Moving in and out (0x1007a6fa6 MoveIn, 0x1007a7112 MoveOut, 0x1007a718c..0x1007a78fc)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySim::MoveIn(std::uint8_t slot, cRZString& name, bool male, std::uint8_t zodiac, cRZString& bio,
                       std::uint32_t bitmapId, cRZString& bitmapFile) {
    slot_ = slot;
    cSC4Advisor::GetPersonalName().Copy(name);                           // +0x60
    male_ = male ? 1 : 0;
    zodiac_ = zodiac;
    bitmapId_ = bitmapId;
    if (bitmapId == 0) bitmapFile_.Copy(bitmapFile);
    bio_.Copy(bio);
    generation_ = 1;
    birthMonth_ = static_cast<std::uint8_t>(Month() % 6);                // +0x90
    CancelViewInput();
    if (IMySimHost* host = Host()) {
        cIGZUnknown* tool = host->CreateMoveInTool(this);                // cSC4MySimInputControlMoveIn
        if (tool) {
            host->ViewPushInputControl(tool);                            // spView3D 0xc8(tool, 2)
            if (host->CurrentViewInputControl() != tool) host->SetCurrentViewInputControl(tool);
        }
    }
    lastPlayerTripDay_ = 0;                                              // +0x410
    ShowLocation();                                                      // vslot 0x1e8
}

// 0x1007a7112 MoveOut(): drops the residence (and its callout/sound) and the lot, and stops the balloon tracking.
void cSC4MySim::MoveOut() {
    if (residence_) {
        residence_ = nullptr;
        if (callout_) HideResidence();                                   // callout->0x18 window 0x230
        RemoveHomeSound();
    }
    lot_ = nullptr;
    balloonsEnabled_ = false;
}

// 0x1007a718c FindNewResidence(): walks the city's buildings in a random order, starting at a random node, and moves
// into the first one TestForResidence accepts. Always answers false.
bool cSC4MySim::FindNewResidence() {
    IMySimHost* host = Host();
    cISC4BuildingOccupant* old = residence_.get();
    if (old) old->AddRef();
    if (host) {
        std::vector<cISC4BuildingOccupant*> candidates;
        host->ResidenceCandidates(candidates);                            // cMySimBuildingFilter(true)
        const std::size_t count = candidates.size();
        if (count != 0) {
            std::size_t index = random_.RandomUint32Uniform(static_cast<std::uint32_t>(count));
            for (std::size_t n = 0; n < count; ++n) {
                cISC4BuildingOccupant* building = candidates[index];
                bool keepLooking = true;
                if (building && TestForResidence(building, false, false)) {
                    if (ChangeResidence(building)) keepLooking = false;
                }
                if (!keepLooking) break;
                if (++index == count) index = 0;
            }
        }
    }
    if (!residence_) {
        cSC4Advisor::PushEvent(kEventMySimNoResidence, nullptr);          // 0xb0
    } else if (residence_.get() == old) {
        MoveOut();
        cSC4Advisor::PushEvent(kEventMySimNoResidence, nullptr);
    }
    if (old) old->Release();
    return false;
}

// 0x1007a73f4 TestForResidence(): no when the building is already the residence, when the wealth differs (checkWealth)
// or when another sim lives there; with checkFilter, the building filter must also accept it.
bool cSC4MySim::TestForResidence(cISC4BuildingOccupant* building, bool checkFilter, bool checkWealth) {
    if (!building) return false;
    IMySimHost* host = Host();
    if (residence_.get() == building) return false;
    cSC4Lot* lot = host ? host->LotByBuilding(building) : nullptr;
    if (lot && checkWealth) {
        if (static_cast<int>(lot->GetOccupantWealth()) != wealth_) return false;   // lot vslot 0x148
    }
    const bool free = host ? (host->MySimAtBuilding(building) == nullptr) : true;
    if (!free) return false;
    if (!checkFilter) return true;
    return host ? host->IsResidenceCandidate(building) : true;
}

// 0x1007a7556 ChangeResidence(): moves out, sets the new residence and rolls back when an advice would trigger.
bool cSC4MySim::ChangeResidence(cISC4BuildingOccupant* building) {
    cISC4BuildingOccupant* old = residence_.get();
    if (old) old->AddRef();
    bool changed = false;
    if (TestForResidence(building, false, false)) {
        MoveOut();
        SetResidence(building);
        if (CommandWouldTrigger(kScriptMySimChangeHouse)) {              // 0xb8
            MoveOut();
            SetResidence(old);
        }
        if (calloutShown_) HideResidence();                              // vslot 0x1d8
        trackHandle_ = dispatch_ ? dispatch_->TrackHandle() : 0;         // dispatch +0x4c
        if (residence_.get() != old) {
            if (lot_) wealth_ = static_cast<int>(lot_->GetOccupantWealth());
            state_ = 1;
            changed = true;
        }
    }
    if (old) old->Release();
    return changed;
}

// 0x1007a768a ChangeJob(): makes the building the commute destination.
bool cSC4MySim::ChangeJob(cISC4BuildingOccupant* building) {
    if (!building || !dispatch_) return false;
    MySimLocation location;
    location.SetByOccupant(building->AsOccupant());
    location.SetByCommute();
    return dispatch_->SetCurrentCommuteDestination(location, true);      // 0x1006b26fe
}

// 0x1007a7722 DeleteFromGame()
bool cSC4MySim::DeleteFromGame() {
    IMySimHost* host = Host();
    if (dispatch_) dispatch_->Shutdown();
    if (pylon_) {
        pylon_->Shutdown();
        pylon_ = nullptr;
        pylonLocked_ = false;
    }
    jobCell_[0] = jobCell_[1] = jobCell_[2] = jobCell_[3] = 0;
    MoveOut();
    state_ = 0;
    HideLocation();                                                      // vslot 0x1f0
    ResetAdvisorState();
    SendMySimsChangedMessage();
    (void)host;
    return true;
}

// 0x1007a77b8 ResetAdvisorState(): shuts the advisor base down and re-inits it on the same row, then rebuilds the
// balloons; the personal name is restored afterwards.
void cSC4MySim::ResetAdvisorState() {
    cRZString saved(cSC4Advisor::GetPersonalName());
    const int row = TableRow();                                          // +0x28
    cSC4Advisor::Shutdown();
    cSC4Advisor::Init(row);
    init_balloons();
    cSC4Advisor::GetPersonalName().Copy(saved);
}

// 0x1007a7918 SetResidence(): sets the residence, its lot/cells/position and (on the first move) starts the sim.
void cSC4MySim::SetResidence(cISC4BuildingOccupant* building) {
    IMySimHost* host = Host();
    if (residence_.get() != building) residence_ = building;
    if (!building) return;
    cISC4Occupant* occupant = building->AsOccupant();                     // 0x18
    occupant->GetPosition(&posX_);                                        // +0x168..0x170
    lot_ = host ? host->LotByBuilding(building) : nullptr;                // spLotManager 0x70
    if (!lot_) return;
    int cellX = 0, cellZ = 0;
    lot_->GetLocation(cellX, cellZ);                                     // lot vslot 0xa8
    residenceCellX_ = cellX;
    residenceCellZ_ = cellZ;
    if (state_ == 0) {
        if (dispatch_) dispatch_->Init();                                // 0x1006ad11a
        SetCar(nullptr);                                                 // 0x328
        SetPedestrian(nullptr);                                          // 0x330
    }
    MySimLocation location;
    location.SetByOccupant(occupant);
    if (dispatch_) dispatch_->SetDefaultDestination(location);           // 0x1006b00de
    if (state_ == 0) {
        ResetAdvisorState();
        state_ = 2;
        int age = host ? host->ResidentialAgeAtCell(residenceCellX_, residenceCellZ_) : 0;
        if (age < 0x13) age = 0x12;
        age_ = static_cast<std::uint8_t>(age);
        state_ = 1;
        wealth_ = static_cast<int>(lot_->GetOccupantWealth());
        if (host) {
            eq_ = host->ResidentialEducationQuotient(residenceCellX_, residenceCellZ_, wealth_);
            hq_ = host->ResidentialHealthQuotient(residenceCellX_, residenceCellZ_, wealth_);
        }
        for (int i = 0; i < 6; ++i) {
            eqHistory_[i] = eq_;
            hqHistory_[i] = hq_;
        }
        SendMySimsChangedMessage();
        FindNewJob();
        DispatchCommute();
        if (dispatch_) dispatch_->SetFollowCamera(true);                 // dispatch +0x20 = 1
    } else if (residence_) {
        balloonsEnabled_ = dispatch_ && dispatch_->CheckFlags(0x100);    // 0x1006afa4a
    }
    UpdateVariables();
}

// 0x1007a7bec DispatchCommute(): (re)issues the commute for the current state.
bool cSC4MySim::DispatchCommute() {
    if (!residence_) return false;
    const int state = state_;
    if (state == 1) {
        if (dispatch_) dispatch_->TeleportToCommuteLocation();           // 0x1006b2860
        return true;
    }
    if (state == 2) {
        if (!dispatch_) return true;
        MySimLocation location = dispatch_->GetCurrentCommuteDestination();
        if (!location.hasOccupant || !location.IsValid(*Host())) {
            MySimLocation fresh;
            fresh.SetByCommute();
            location = fresh;
        }
        const int hour = Host() ? Host()->HourOfDay() : 0;               // sp24HourClock 0x50
        if (hour < 8) {
            location.data24 = 5;
        } else {
            location.data24 = (0x11 - hour) * 0x3c;
            if (0x11 - hour < 0) location.data24 = 5;
        }
        dispatch_->AddDestinationLocation(location, 4);                  // 0x1006afdd8
        return true;
    }
    if (state == 3 && dispatch_) {
        dispatch_->AddDestinationLocation(dispatch_->GetCurrentDefaultDestination(), 4);
    }
    return true;
}

// 0x1007a56ee FailCommute(): quits the job and clears the commute destination.
void cSC4MySim::FailCommute() {
    QuitJob();
    cSC4Advisor::PushEvent(kEventMySimCommuteFailed, nullptr);           // 0xb0
    MySimLocation location;
    location.SetByCommute();
    if (dispatch_) dispatch_->SetCurrentCommuteDestination(location, true);
}

// 0x1007a5786 FindNewJob(): clears the commute destination (a random job) and asks the dispatch to find it.
bool cSC4MySim::FindNewJob() {
    if (!dispatch_) return false;
    MySimLocation location;
    location.SetByCommute();
    return dispatch_->SetCurrentCommuteDestination(location, true);
}

// 0x1007a64f2 CheckCommute(): asks the path finder whether the current job is still reachable; a failed route fails
// the commute and caches the travel time otherwise.
void cSC4MySim::CheckCommute() {
    IMySimHost* host = Host();
    if (!host) return;
    if ((!job_ && !neighborJob_) || !residence_) return;
    SC4CellRect home{};
    residence_->AsOccupant()->GetBoundingCityCells(home);                // occupant 0x58
    int homeRect[4] = {home.x0, home.z0, home.x1, home.z1};
    int jobRect[4] = {0, 0, 0, 0};
    if (!job_) {
        jobRect[0] = jobRect[2] = jobCell_[2];
        jobRect[1] = jobRect[3] = jobCell_[3];
    } else {
        SC4CellRect job{};
        job_->AsOccupant()->GetBoundingCityCells(job);
        jobRect[0] = job.x0;
        jobRect[1] = job.z0;
        jobRect[2] = job.x1;
        jobRect[3] = job.z1;
    }
    int minutes = 0;
    if (!host->FindCommutePath(homeRect, jobRect, minutes)) {            // spCity 0x228 path finder
        FailCommute();
        return;
    }
    lastCommuteTime_ = minutes;                                          // +0x234
}

// ---------------------------------------------------------------------------------------------------------------
// Simulation ticks (0x1007a6344 simulate_day, 0x1007a6676 simulate_day_events, 0x1007a6e26 simulate_month)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySim::simulate_day() {
    if (state_ == 0) return;
    IMySimHost* host = Host();
    if (!host) return;
    if (host->GamePaused()) return;                                      // the window manager's 0x150 flag
    if (!job_ && !neighborJob_ && !retired_) {
        if (state_ == 2 && !FindNewJob()) ++unemployedDays_;             // +0x1a8
    }
    if (commuteCheck_) {                                                 // +0x240
        CheckCommute();
        commuteCheck_ = false;
    }
    ActivateLuaContext();                                                // 0x2d8
    simulate_balloons(false);
    cSC4Advisor::simulate_day();
    if (lastTripDay_ < DayNumber()) {                                    // +0x3d8 / +0x94
        lastTripDay_ = DayNumber();
        UpdateTripVariables(-1, -1, -1, -1, false);
    }
    if (pylon_) {
        if (pylonTimer_.IsTimerRunning()) {
            if (!pylon_->IsDead()) {
                if (pylonTimer_.GetElapsedTimeU32() < 6) return;
                pylon_->Shutdown();                                      // 0x28
                pylon_ = nullptr;
                pylonLocked_ = false;
            }
            pylonTimer_.Stop();
        }
    }
}

// 0x1007a6676 simulate_day_events(): for every queued event (but keyed off the last queued one, kept quirk), pushes
// the event balloons of the event's advice and marks them shown.
void cSC4MySim::simulate_day_events() {
    const EventList& events = Events();
    const int count = static_cast<int>(events.size());
    for (int n = 0; n < count; ++n) {
        if (events.empty()) break;
        const std::pair<int, cRZAutoRefCount<cISC4AdviceSubject>>& last = events.back();
        const int eventId = last.first;
        cISC4AdviceSubject* eventSubject = last.second.get();
        std::map<int, int>::iterator it = eventBalloons_.lower_bound(eventId);
        const std::map<int, int>::iterator end = eventBalloons_.upper_bound(eventId);
        for (; it != end; ++it) {
            const int index = it->second;
            if (index < 0 || static_cast<std::size_t>(index) >= balloons_.size()) continue;
            cISC4MySimBalloonAdvice* balloon = balloons_[static_cast<std::size_t>(index)].advice.get();
            if (!balloon) continue;
            cISC4Advice* advice = balloon->AsISC4Advice();               // 0x18
            const bool shown = balloon->CheckStatusFlags(1);             // 0x70
            const bool trigger = balloon->GetRawTrigger();               // 0x60
            if (!shown) {
                if (trigger) {
                    advice->SetEventSubject(eventSubject);               // 0xc0
                    advice->SetFlags(kAdviceFlagTextDirty, true);        // 0x70(0x10)
                    push_balloon_queue(1, index);
                }
            } else if (trigger) {
                advice->SetEventSubject(eventSubject);
                advice->SetFlags(kAdviceFlagTextDirty, true);
            }
        }
    }
    cSC4Advisor::simulate_day_events();
}

// 0x1007a6e26 simulate_month(): ages the sim on its birth month, rechecks the commute, refreshes the script variables
// and rotates the eq/hq history.
void cSC4MySim::simulate_month() {
    if (state_ == 0) return;
    IMySimHost* host = Host();
    if (!host) return;
    if (host->GamePaused()) return;
    if (Month() % 6 == birthMonth_) ++age_;                              // +0x90 / +0x1e9
    if (job_ || neighborJob_) CheckCommute();
    UpdateVariables();
    const std::uint8_t eqIndex = eqHistoryIndex_;
    deltaEq_ = eq_ - eqHistory_[eqIndex];
    eqHistoryIndex_ = static_cast<std::uint8_t>((eqIndex + 1) % 6);
    eqHistory_[eqIndex] = eq_;
    const std::uint8_t hqIndex = hqHistoryIndex_;
    deltaHq_ = hq_ - hqHistory_[hqIndex];
    hqHistoryIndex_ = static_cast<std::uint8_t>((hqIndex + 1) % 6);
    hqHistory_[hqIndex] = hq_;
    ActivateLuaContext();                                                // 0x2d8
    cSC4Advisor::simulate_month();
}

// ---------------------------------------------------------------------------------------------------------------
// Sounds (0x1007a8fc2 RequestToPlaySound, 0x1007a9042 PlayHomeSound, 0x1007a912a PlayNightSound)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySim::RequestToPlaySound() {
    const std::int32_t elapsed = static_cast<std::int32_t>(soundTimer_.GetElapsedTimeU32());   // +0x418
    if (state_ == 0 || static_cast<std::uint32_t>(elapsed - lastSoundTime_) < 5) return false;
    lastSoundTime_ = elapsed;                                            // +0x414
    return true;
}

void cSC4MySim::PlayHomeSound(std::uint32_t soundId) {
    if (!RequestToPlaySound()) return;
    RemoveHomeSound();
    IMySimHost* host = Host();
    if (!host) return;
    homeSound_ = host->CreateHomeSound(soundId);                         // spPropManager 0x80
    if (!homeSound_) return;
    homeSound_->SetPosition(&posX_);                                     // 0x18 -> 0x48
    host->PlayCitySound(homeSound_.get(), &posX_);                       // spCity 0x110 -> 0xa0
}

void cSC4MySim::PlayNightSound() {
    if (state_ != 2) return;
    PlayHomeSound(male_ ? kSoundNightMale : kSoundNightFemale);
}

// ---------------------------------------------------------------------------------------------------------------
// Work / home transitions and script commands (0x1007a9182..0x1007a96a0)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySim::LeaveForWork() {
    if (state_ >= 6) return;
    if ((0x26u >> (static_cast<unsigned>(state_) & 0x1f) & 1u) == 0) return;   // states 1, 2 and 5
    if (!DispatchCommute()) {
        PlayHomeSound(male_ ? kSoundNoCommuteMale : kSoundNoCommuteFemale);
        return;
    }
    state_ = 4;
    trackHandle_ = dispatch_ ? dispatch_->TrackHandle() : 0;             // dispatch +0x4c
}

void cSC4MySim::ReturnHome() {
    if (static_cast<unsigned>(state_ - 3) >= 2u) return;                 // states 3 and 4
    if (!DispatchCommute()) return;
    state_ = 5;
    trackHandle_ = dispatch_ ? dispatch_->TrackHandle() : 0;
}

// 0x1007a928e AnnounceMoveIn(): posts the "the sim moved in" message, refreshes the script variables and pushes the
// "the sim took over its residence" event (the tail call Ghidra lost at 0x1007a9363 is PushEvent(0xca3e87a8, 0) with
// the id loaded into esi at 0x1007a934f and edx zeroed at 0x1007a9354).
void cSC4MySim::AnnounceMoveIn() {
    if (!RequestToPlaySound()) return;
    IMySimHost* host = Host();
    if (!host) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMessageMySimMovedIn);                              // 0x28
    message->SetIGZUnknown(residence_ ? static_cast<cIGZUnknown*>(residence_->AsOccupant()) : nullptr);   // 0x108
    message->SetData1(male_ ? 1 : 0);                                    // 0x58
    message->SetData2(static_cast<std::uint64_t>(static_cast<std::uint32_t>(wealth_)));   // 0x80
    if (cIGZMessageServer2* bus = host->Messages()) bus->MessageSend(message);
    message->Release();
    UpdateVariables();
    cSC4Advisor::PushEvent(kEventMySimMovedIn, nullptr);                 // 0xb0
}

// 0x1007a8ebe QuitJob()
void cSC4MySim::QuitJob() {
    if (state_ == 0) return;
    if (!job_) {
        if (neighborJob_) {
            neighborJob_ = nullptr;
            jobCell_[2] = 0;                                             // 0x198
        }
    } else {
        ReturnHome();                                                    // vslot 0x210
        job_ = nullptr;
    }
    if (state_ != 5) state_ = 2;
    UpdateVariables();
}

// 0x1007a93a0 DoScriptCommand()
void cSC4MySim::DoScriptCommand(int command) {
    if (state_ == 0) return;
    const std::uint32_t value = static_cast<std::uint32_t>(command);
    switch (value) {
        case kScriptMySimChangeHouse:                                    // 0x6a47e79b
            FindNewResidence();                                          // vslot 0x110
            break;
        case kScriptMySimSpawn:                                          // 0x6a495f7f
            Spawn();
            break;
        case kScriptMySimIncreaseWealth:                                 // 0x6a4a5e9e
            IncreaseWealth();
            break;
        case kScriptMySimQuitJob:                                        // 0x6a5e6860
            QuitJob();
            break;
        case kScriptMySimRetire:                                         // 0xaa4934b4
            QuitJob();
            retired_ = true;                                             // +0x238
            trackHandle_ = dispatch_ ? dispatch_->TrackHandle() : 0;
            break;
        case kScriptMySimDie:                                            // 0xea4d269c
            Die();
            break;
        case kScriptMySimLeaveCity:                                      // 0x0a57de1f
            LeaveCity();
            break;
        default:
            break;
    }
}

// 0x1007a9480 Spawn(): starts the next generation of the sim (a new age, the job quit, the ghost effect).
void cSC4MySim::Spawn() {
    if (state_ == 0) return;
    retired_ = false;
    ++generation_;
    generationName_ = nullptr;                                           // the cached name is rebuilt on demand
    age_ = static_cast<std::uint8_t>(Host() ? Host()->ResidentialAgeForWealth(wealth_) : 0);
    QuitJob();
    StartGhostEffect();
    SendMySimsChangedMessage();
}

// 0x1007a94fe IncreaseWealth()
void cSC4MySim::IncreaseWealth() {
    if (state_ == 0 || wealth_ >= 3) return;
    ++wealth_;
    QuitJob();
    FindNewResidence();                                                  // vslot 0x110
}

// 0x1007a953e Die()
void cSC4MySim::Die() {
    if (state_ == 0) return;
    cRZString text("my_sim_died");
    if (Host()) Host()->ReportSimEvent(text, &posX_);                    // spCity 0x178 -> 0x48
    StartGhostEffect();
    LeaveCity();
}

// 0x1007a9600 LeaveCity()
void cSC4MySim::LeaveCity() {
    if (state_ == 0) return;
    MoveOut();
    HideLocation();                                                      // vslot 0x1f0
    state_ = 0;
    if (dispatch_) dispatch_->Shutdown();
    jobCell_[0] = jobCell_[1] = jobCell_[2] = jobCell_[3] = 0;
    if (pylon_) {
        pylon_->Shutdown();
        pylon_ = nullptr;
        pylonLocked_ = false;
    }
    SendMySimsChangedMessage();
}

// 0x1007a787b SendMySimsChangedMessage(): posts 0xaa25827c through the message server.
void cSC4MySim::SendMySimsChangedMessage() {
    IMySimHost* host = Host();
    if (!host) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMessageMySimsChanged);
    if (cIGZMessageServer2* bus = host->Messages()) bus->MessageSend(message);
    message->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// City messages about buildings, fires, disasters (0x1007a96b2..0x1007a9e84)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySim::DoBuildingAdded(cISC4BuildingOccupant* building) {
    if (state_ == 0 || !residence_ || !building) return;
    cISC4Occupant* occupant = building->AsOccupant();                     // 0x18
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);                                     // 0x40
    const float dx = posX_ - position[0];
    const float dz = posZ_ - position[2];
    const std::uint32_t distance = static_cast<std::uint32_t>(static_cast<long>(std::sqrt(dx * dx + dz * dz)));
    auto* subject = new cSC4AdviceSubject();
    subject->AddRef();
    subject->SetObject(static_cast<cIGZUnknown*>(building));              // 0x38
    subject->SetData1(distance);                                         // 0x50
    cSC4Advisor::PushEvent(kEventMySimBuildingAdded, subject);
    subject->Release();
}

void cSC4MySim::DoBuildingChanged(cISC4BuildingOccupant* building) {
    if (state_ == 0) return;
    if (residence_.get() == building || job_.get() == building) UpdateVariables();
}

void cSC4MySim::DoBuildingRemoved(cISC4BuildingOccupant* building, std::uint32_t data) {
    if (state_ == 0) return;
    if (residence_ && residence_.get() == building) {
        if (data == 0) {
            cSC4Advisor::PushEvent(0xaa48f5c1u, nullptr);                 // "residence removed"
            FindNewResidence();                                          // vslot 0x110
        } else {
            lastLocalDisaster_ = static_cast<std::int32_t>(data);
            UpdateVariables();
            cSC4Advisor::PushEvent(0xaa4d2d64u, nullptr);                 // "residence destroyed"
            MoveOut();
        }
        return;
    }
    if (job_ && job_.get() == building) {
        if (data == 0) {
            cSC4Advisor::PushEvent(0xca48f879u, nullptr);                 // "job removed"
        } else {
            lastLocalDisaster_ = static_cast<std::int32_t>(data);
            UpdateVariables();
            cSC4Advisor::PushEvent(0xaa4d2d65u, nullptr);                 // "job destroyed"
        }
        QuitJob();
    }
}

// 0x1007a9934 StartGhostEffect(): the "MySimGhost" effect, positioned at the sim with an identity matrix and switched
// off (the effect manager plays it when the sim dies).
void cSC4MySim::StartGhostEffect() {
    ghost_ = nullptr;
    IMySimHost* host = Host();
    if (!host) return;
    ghost_ = host->CreateGhostEffect();                                  // spEffectsManager 0x30
    if (!ghost_) return;
    float matrix[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 1.0f, 0.0f,
                        posX_, posY_, posZ_, 1.0f};
    ghost_->SetTransform(matrix);                                        // 0x38
    ghost_->SetActive(false);                                            // 0x18
}

// 0x1007a9a34 / 0x1007a9b1a / 0x1007a9c00: a "distance to the event" advice subject then PushEvent.
void cSC4MySim::DoFireAdded(float x, float z) {
    if (state_ == 0) return;
    const float dx = x - posX_;
    const float dz = z - posZ_;
    const std::uint32_t distance = static_cast<std::uint32_t>(static_cast<long>(std::sqrt(dx * dx + dz * dz)));
    auto* subject = new cSC4AdviceSubject();
    subject->AddRef();
    subject->SetData1(distance);
    cSC4Advisor::PushEvent(kEventMySimFire, subject);
    subject->Release();
}

void cSC4MySim::DoPipeBurst(float x, float z) {
    if (state_ == 0) return;
    const float dx = x - posX_;
    const float dz = z - posZ_;
    const std::uint32_t distance = static_cast<std::uint32_t>(static_cast<long>(std::sqrt(dx * dx + dz * dz)));
    auto* subject = new cSC4AdviceSubject();
    subject->AddRef();
    subject->SetData1(distance);
    cSC4Advisor::PushEvent(kEventMySimPipeBurst, subject);
    subject->Release();
}

void cSC4MySim::DoTornado(float x, float z) {
    if (state_ == 0) return;
    const float dx = x - posX_;
    const float dz = z - posZ_;
    const std::uint32_t distance = static_cast<std::uint32_t>(static_cast<long>(std::sqrt(dx * dx + dz * dz)));
    auto* subject = new cSC4AdviceSubject();
    subject->AddRef();
    subject->SetData1(distance);
    cSC4Advisor::PushEvent(kEventMySimTornado, subject);
    subject->Release();
}

// 0x1007a9ce6 DoDisaster(): only the two disasters the sim reacts to (0xca380b06 and 0xc4c90997).
void cSC4MySim::DoDisaster(std::uint32_t disaster, int x, int z) {
    if (state_ == 0) return;
    if (disaster != 0xca380b06u && disaster != 0xc4c90997u) return;
    float fx = 0.0f, fz = 0.0f;
    if (Host()) Host()->StandardCellToPosition(x, z, fx, fz);             // spCity vslot 0x308
    const float dx = fx - posX_;
    const float dz = fz - posZ_;
    const std::uint32_t distance = static_cast<std::uint32_t>(static_cast<long>(std::sqrt(dx * dx + dz * dz)));
    auto* subject = new cSC4AdviceSubject();
    subject->AddRef();
    subject->SetLocation(static_cast<float>(x), static_cast<float>(z));  // 0x18
    subject->SetData1(distance);
    cSC4Advisor::PushEvent(disaster == 0xc4c90997u ? kEventMySimDisasterB : kEventMySimDisasterA, subject);
    subject->Release();
}

// 0x1007a9e64 DoNeighborConnectionRemoved(): the neighbour job may be gone; recheck the commute on the next day.
void cSC4MySim::DoNeighborConnectionRemoved(int cellX, int cellZ) {
    (void)cellX;
    (void)cellZ;
    if (state_ != 0 && neighborJob_) commuteCheck_ = true;               // +0x240
}

// ---------------------------------------------------------------------------------------------------------------
// Distances / grades (0x1007a6830, 0x1007a6896, 0x1007a69ee, 0x1007a6c06)
// ---------------------------------------------------------------------------------------------------------------
int cSC4MySim::DistanceToClosest(std::uint32_t occupantGroup) {
    if (state_ == 0 || !groupDistances_) return -1;
    std::map<std::uint32_t, std::uint32_t>::iterator it = groupDistances_->lower_bound(occupantGroup);
    if (it == groupDistances_->end() || it->first > occupantGroup) return -1;
    return static_cast<int>(it->second);
}

std::uint32_t cSC4MySim::DistanceToClosestSpecial(std::uint32_t buildingType) {
    IMySimHost* host = Host();
    if (!host) return 0xffffffffu;
    std::vector<cISC4Occupant*> occupants;
    host->CityOccupants(occupants);                                      // spCity 0x1a8 -> 0x160
    std::uint32_t best = 0xffffffffu;
    for (cISC4Occupant* occupant : occupants) {
        if (!occupant) continue;
        cISC4BuildingOccupant* building = nullptr;
        if (!occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&building)) || !building) {
            continue;
        }
        if (building->GetBuildingType() == buildingType) {
            float position[3] = {0.0f, 0.0f, 0.0f};
            occupant->GetPosition(position);
            const float dx = posX_ - position[0];
            const float dz = posZ_ - position[2];
            const std::uint32_t distance = static_cast<std::uint32_t>(
                static_cast<long>(std::sqrt(dx * dx + dz * dz)));
            if (distance < best) best = distance;
        }
        building->Release();
    }
    return best;
}

int cSC4MySim::LocalSchoolGrade(std::uint32_t buildingType) {
    int grade = 0;                                                       // kSC4PercentageZero
    if (Host()) Host()->GetLocalSchoolGrade(buildingType, posX_, posZ_, grade);
    return grade;
}

int cSC4MySim::LocalHospitalGrade(std::uint32_t buildingType) {
    int grade = 0;
    if (Host()) Host()->GetLocalHospitalGrade(buildingType, posX_, posZ_, grade);
    return grade;
}


// ---------------------------------------------------------------------------------------------------------------
// DoMessage (0x1007a3364)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySim::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    IMySimHost* host = Host();
    const std::uint32_t type = message->GetType();
    if (type == kMessageMySimSimulate) {                                 // 0x8ad610ee (balloon tick)
        if (state_ == 0) return true;
        if (!host || !host->SimulatorTickEnabled()) return true;         // spSimulator 0x78/0x80/0x88
        if (balloonsTimer_.GetElapsedTimeU32() < 3) return true;
        simulate_balloons(true);
        balloonsTimer_.Restart();
        return true;
    }
    if (type == kMessageMySimInput || type == kMessageMySimInput2) {     // cancel this sim's view input
        const std::uint32_t id = static_cast<std::uint32_t>(standard->GetData1());
        if (GetID() == static_cast<int>(id) || id == 0) CancelViewInput();
        return true;
    }
    if (type == kMessageMySimNoop) {                                     // consumed, ignored
        (void)standard->GetData1();
        return true;
    }
    if (type == kMessageMySimToolMessage) {                              // 0xbb04126: open the dispatch tool
        if (!CanUDriveIt()) return true;
        if (!host) return true;
        cIGZUnknown* tool = host->CreateDispatchToolControl(this);
        if (!tool) return true;
        host->ViewPushInputControl(tool);                                // spView3D 0xc8(tool, 2)
        return true;
    }
    if (type == kMessageMySimChanged) {                                  // 0x4bc42c48
        if (standard->GetIGZUnknown() != static_cast<cIGZUnknown*>(static_cast<cISC4MySim*>(this))) return true;
        UpdateVariables();
        simulate_balloons(true);
        return true;
    }
    if (type == kMessageMySimHideAll) {                                  // 0x26d31ec1
        HideResidence();
        HideLocation();
        SetPylonVisibility(false);
        return true;
    }
    if (type != kMessageMySimDispatchNotify) return cSC4Advisor::DoMessage(message);   // 0x2ba06b99
    if (!dispatch_) return true;

    const std::uint32_t event = static_cast<std::uint32_t>(standard->GetData1());
    auto postChanged = [host]() {
        if (host) host->PostMessage2(kMessageMySimChanged, 0, 0, 0, 0, nullptr);
    };
    if (event == kDispatchNotifyTripEnd) {                               // 0x4bb2d020
        UpdateTripVariables(-1, -1, static_cast<int>(standard->GetData3()),
                            static_cast<int>(standard->GetData2()), false);
        return true;
    }
    if (event == kDispatchNotifyTripLeg) {                               // 0x4bb2cfc0
        UpdateTripVariables(static_cast<int>(standard->GetData3()),
                            static_cast<int>(standard->GetData2()), -1, -1, false);
        return true;
    }
    if (event == kDispatchNotifyNeighbor) {                              // 0x4bb71a7e: the trip ended
        MySimLocation location = dispatch_->GetCurrentDestination();
        if (!(location == dispatch_->GetCurrentDefaultDestination())) {
            const bool atCommute = location == dispatch_->GetCurrentCommuteDestination();
            state_ = static_cast<std::int32_t>((atCommute ? 0 : 1) + 4 + (atCommute ? 0 : 1));
        } else if (state_ != 1) {
            state_ = 5;
        }
        postChanged();
        return true;
    }
    if (event == 0x2bac398fu) {                                          // the pylon was clicked
        if (!pylon_) return true;
        pylonTimer_.Restart();
        if (!pylonLocked_) return true;
        if (!is_pylon_location(dispatch_->GetCurrentDestination())) return true;
        postChanged();
        pylon_->SetKind(1);                                              // 0x58
        return true;
    }
    if (event == kDispatchNotifyArrived) {                               // 0xeba06bf4: reached the commute destination
        const MySimLocation& commute = dispatch_->GetCurrentCommuteDestination();
        cISC4Occupant* occupant = commute.occupant.get();
        if (occupant) occupant->AddRef();
        if (!occupant) {
            const int cellX = commute.lotCellX;
            const int cellZ = commute.lotCellZ;
            cISC4Occupant* found = host ? host->OccupantAtCell(cellX, cellZ) : nullptr;
            cISC4BuildingOccupant* foundBuilding = nullptr;
            if (found) {
                found->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&foundBuilding));
                found->Release();
            }
            neighborJob_ = foundBuilding;
            if (foundBuilding) foundBuilding->Release();
            if (!neighborJob_) return true;
            jobCell_[0] = cellX;
            jobCell_[1] = cellZ;
            jobCell_[2] = commute.cellX;
            jobCell_[3] = commute.cellZ;
            job_ = nullptr;
        } else {
            cSC4Lot* lot = host ? host->LotAtCell(commute.cellX, commute.cellZ) : nullptr;
            cISC4BuildingOccupant* building = nullptr;
            if (lot) {
                cIGZUnknown* object = lot->GetBuilding();                // lot vslot 0x180
                if (object) {
                    object->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&building));
                }
            }
            if (building) {
                job_ = building;
                jobCell_[2] = commute.cellX;
                jobCell_[3] = commute.cellZ;
                neighborJob_ = nullptr;
                building->Release();
            }
            occupant->Release();
        }
        unemployedDays_ = 0;
        UpdateVariables();
        cSC4Advisor::PushEvent(kEventMySimMoved, nullptr);               // 0xb0
        if (!occupant) job_ = nullptr;
        return true;
    }
    if (event == kDispatchNotifyPylon) {                                 // 0xeba06c12: arrived near the pylon
        if (pylon_) {
            pylonTimer_.Restart();
            if (pylonLocked_ && is_pylon_location(dispatch_->GetCurrentDestination())) pylon_->SetKind(1);
        }
        const MySimLocation& destination = dispatch_->GetCurrentDestination();
        if (destination.commuteLocation) {
            FailCommute();
            return true;
        }
        if (!dispatch_->CheckFlags(0x80)) return true;
        if (state_ != 1) return true;
        state_ = 2;
        AnnounceMoveIn();
        return true;
    }
    if (event == kDispatchNotifyIdleOffset) {                            // 0xeba06bd0
        PickIdleOffset();
        return true;
    }
    if (event == kDispatchNotifyLocation) {                              // 0xeba06bd4
        const MySimLocation& location = dispatch_->GetCurrentLocation();
        if (location == dispatch_->GetCurrentDefaultDestination()) {
            if (state_ == 1) {
                state_ = 2;
                AnnounceMoveIn();
            }
            state_ = 2;
        } else if (location == dispatch_->GetCurrentCommuteDestination()) {
            state_ = 3;
        }
        if (!pylon_) return true;
        pylonTimer_.Restart();
        if (!pylonLocked_) return true;
        if (!is_pylon_location(dispatch_->GetCurrentDestination())) return true;
        postChanged();
        pylon_->SetKind(4);
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Window / camera paths (0x1007a84a8..0x1007a8eac)
// ---------------------------------------------------------------------------------------------------------------
// 0x1007a84a8 Find(): looks at the sim (or its residence/job) through the camera; a neighbour job shows a dialog.
void cSC4MySim::Find() {
    IMySimHost* host = Host();
    if (!host) return;
    float position[3] = {0.0f, 0.0f, 0.0f};
    if (state_ == 2 && residence_) {
        residence_->AsOccupant()->GetPosition(position);
    } else if (state_ == 3) {
        if (job_) {
            job_->AsOccupant()->GetPosition(position);
        } else if (neighborJob_) {
            cRZString title, body;
            host->LoadString(0xea231e96u, 0x2a7877e7u, title);
            host->LoadString(0xea231e96u, 0x0a7877f2u, body);
            cRZString name(GetPersonalName());
            cRZString titleText, bodyText;
            titleText.Sprintf(title.ToChar(), name.ToChar());
            bodyText.Sprintf(body.ToChar(), name.ToChar(), jobCityText_ ? jobCityText_->ToChar() : "");
            host->NotificationDialog(titleText, bodyText);
            if (dispatch_) dispatch_->SetFollowCamera(true);
            return;
        }
    } else if (state_ != 3 && dispatch_) {
        const MySimLocation& location = dispatch_->GetCurrentLocation();
        if (location.occupant) location.occupant->GetPosition(position);
    }
    host->CameraLookAt(position, true);
    if (dispatch_) dispatch_->SetFollowCamera(true);
}

// 0x1007a8876 ShowResidence(): creates the callout box over the residence and refreshes it.
void cSC4MySim::ShowResidence() {
    IMySimHost* host = Host();
    if (state_ == 0 || !residence_ || !host) return;
    if (!callout_) {
        callout_ = host->CreateCallout();                                // cSC4WinCalloutBox
        if (!callout_) return;
        callout_->SetStyle(0xa85f1a83u);                                 // 0x58
        callout_->SetOptions(0);                                         // 0x60
        if (host->CalloutImageAvailable()) callout_->SetImage({kCalloutImageType, kCalloutImageGroup, kCalloutImageInstance});
        callout_->AddToView();                                           // spView3DWin 0x70
    }
    UpdateResidence();                                                   // vslot 0x200
    cRZString title;
    host->LoadString(0xea231e96u, 0xea4018d3u, title);
    cRZString text;
    text.Sprintf(title.ToChar(), GetPersonalName().ToChar());
    callout_->SetText(text);                                             // 0x40
    callout_->Show();                                                    // window 0x228
    callout_->Activate();                                                // window 0xc0
    calloutShown_ = true;
}

// 0x1007a8af8 HideResidence()
void cSC4MySim::HideResidence() {
    if (callout_) callout_->Hide();                                      // window 0x230
    calloutShown_ = false;
}

// 0x1007a8b64 UpdateResidence(): moves the callout over the residence's bounding box.
void cSC4MySim::UpdateResidence() {
    IMySimHost* host = Host();
    if (state_ == 0 || !residence_ || !callout_ || !host) return;
    float minXyz[3] = {0.0f, 0.0f, 0.0f};
    float maxXyz[3] = {0.0f, 0.0f, 0.0f};
    cISC4Occupant* occupant = residence_->AsOccupant();
    occupant->GetBoundingBox(minXyz, maxXyz);                            // occupant 0x50
    host->ProjectPointToScreen(minXyz);
    host->ProjectPointToScreen(maxXyz);
    float screen[2];
    screen[0] = static_cast<float>(static_cast<int>((minXyz[0] + maxXyz[0]) * 0.5f));
    float y = (minXyz[1] <= maxXyz[1]) ? minXyz[1] : maxXyz[1];
    screen[1] = static_cast<float>(static_cast<int>(y - 5.0f));
    callout_->SetScreenPosition(screen);                                 // 0x78
}

// 0x1007a8c6c ShowLocation(): starts the dispatch view tracking of the sim.
void cSC4MySim::ShowLocation() {
    if (state_ == 0) return;
    IMySimHost* host = Host();
    if (!host) return;
    cRZString text;
    if (bitmapId_ == 0) {
        host->CityName(text);                                            // spSC4 vslot 0x1d8
        text.AppendString(bitmapFile_);
    }
    if (dispatch_) dispatch_->StartTracking(static_cast<int>(bitmapId_), text);   // 0x1006b0266
    balloonsEnabled_ = true;
}

// 0x1007a8d5c HideLocation()
void cSC4MySim::HideLocation() {
    if (dispatch_) dispatch_->StopTracking();                            // 0x1006b01d2
    empty_all_balloon_queues();
    balloonsEnabled_ = false;
}

// 0x1007a8dba SetPylonVisibility()
void cSC4MySim::SetPylonVisibility(bool visible) {
    if (pylon_) pylon_->SetVisibility(visible);
}

// 0x1007a8e36 GetCurrentScreenLocationXY(): the centre-top of the dispatch icon rect, 0x19 pixels down.
bool cSC4MySim::GetCurrentScreenLocationXY(int* x, int* y) {
    IMySimHost* host = Host();
    if (!host) return false;
    const int track = get_track_handle();
    if (track == -1) return false;
    int rect[4] = {0, 0, 0, 0};
    if (!host->DispatchIconRect(track, rect)) return false;              // spDispatchManager 0xb0
    if (x) *x = (rect[2] - rect[0]) / 2 + rect[0];
    if (y) *y = rect[1] + 0x19;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Dispatch tool (0x1007a9e9e..0x1007aa4da) and the model setters (0x1007aa5f0..0x1007aa9c6)
// ---------------------------------------------------------------------------------------------------------------
void* cSC4MySim::DispatchToolBegin() {
    IMySimHost* host = Host();
    if (pylon_ || !host) {
        dispatchToolActive_ = true;
        host->SetCurrentViewInputControl(host->ViewCurrentInputControl());
        return pylon_.get();
    }
    pylon_ = host->CreatePylon();                                        // cSC4PylonOccupant
    if (!pylon_) return nullptr;
    pylon_->Init();                                                      // 0x20
    pylon_->SetKind(0);                                                  // 0x58
    pylon_->SetFlags(4);                                                 // 0x48
    pylon_->SetState(1);                                                 // 0x30
    float position[3] = {0.0f, 0.0f, 0.0f};
    if (cISC4Occupant* pylonOccupant = pylon_->AsOccupant()) pylonOccupant->GetPosition(position);
    cIGZUnknown* self = nullptr;
    QueryInterface(kGZIID_cIGZUnknown, reinterpret_cast<void**>(&self));                           // QueryInterface(1) in the binary
    pylon_->SetLinkedAgent(self);                                        // 0x98
    if (self) self->Release();
    cRZString name;
    host->CityName(name);
    name.AppendString(bitmapFile_);
    pylon_->SetBitmap(static_cast<int>(bitmapId_), name);                // 0x40
    pylonLocked_ = false;
    dispatchToolActive_ = true;
    host->SetCurrentViewInputControl(host->ViewCurrentInputControl());    // mspViewInputControl = spView3D->0xc0
    return pylon_.get();
}

bool cSC4MySim::DispatchToolEnd() {
    if (dispatchToolActive_ && !pylonLocked_ && dispatch_ && pylon_) {
        if (dispatch_->GetState() == 2) {                                // 0x1006b0244
            const MySimLocation& destination = dispatch_->GetCurrentDestination();
            if (!destination.hasOccupant || !is_pylon_location(destination)) {
                pylon_->Shutdown();
                pylon_ = nullptr;
                pylonLocked_ = false;
            }
        } else {
            pylon_->Shutdown();
            pylon_ = nullptr;
            pylonLocked_ = false;
        }
    }
    dispatchToolActive_ = false;
    return true;
}

bool cSC4MySim::DispatchToolLockPylon(bool lock) {
    IMySimHost* host = Host();
    if (dispatch_ && lock && pylon_ && host) {
        cISC4Occupant* occupant = pylon_->AsOccupant();
        if (occupant) occupant->AddRef();
        MySimLocation location;
        location.SetByOccupant(occupant);
        cISC4BuildingOccupant* building = nullptr;
        if (cSC4Lot* lot = host->LotAtCell(location.cellX, location.cellZ)) {
            cIGZUnknown* object = lot->GetBuilding();
            if (object) object->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&building));
        }
        if (building) {
            pylon_->SetLinkedObject(building->AsOccupant());             // 0x88
            location.SetByOccupant(building->AsOccupant());
        }
        const MySimLocation& destination = dispatch_->GetCurrentDestination();
        bool go = true;
        if (location.hasOccupant == destination.hasOccupant) {
            if (location.cellX == destination.cellX && location.cellZ == destination.cellZ) {
                go = location.occupant.get() != destination.occupant.get();
            }
        }
        if (go) {
            if (dispatch_->GoTo(location)) {                             // 0x1006afb8c
                pylon_->SetKind(3);                                      // 0x58
                if (host) host->PostMessage2(kMessageMySimChanged, 0, 0, 0, 0, nullptr);
                state_ = 6;
                lastPlayerTripDay_ = DayNumber();                        // +0x410
            } else {
                pylon_->SetKind(1);
            }
        }
        if (building) building->Release();
        if (occupant) occupant->Release();
    }
    pylonLocked_ = lock;
    if (!lock && pylon_) pylon_->SetLinkedObject(nullptr);               // 0x88
    return true;
}

bool cSC4MySim::DispatchToolIsPylonLocked() { return pylonLocked_; }
IMySimPylon* cSC4MySim::DispatchToolGetPylon() { return pylon_.get(); }

void cSC4MySim::DispatchToolSetFollowCamera(bool follow) {
    if (dispatch_) dispatch_->SetFollowCamera(follow);                   // dispatch +0x20
}

bool cSC4MySim::DispatchToolCanDispatch(float x, float z) {
    IMySimHost* host = Host();
    if (!host) return false;
    return host->TerrainHeight(x, z) > host->TerrainSeaLevel();          // spTerrain 0x60 > 0xe8; the height is read first
}

bool cSC4MySim::GetLocation(float* xyz) const {
    if (!xyz || !dispatch_) return false;
    const MySimLocation& location = dispatch_->GetCurrentLocation();
    if (location.cellX < 0 || location.cellZ < 0) return false;
    xyz[0] = location.x;
    xyz[1] = 0.0f;
    xyz[2] = location.z;
    if (Host()) xyz[1] = Host()->TerrainHeight(location.x, location.z);  // spTerrain 0x60
    return true;
}

bool cSC4MySim::SetCar(cISCResExemplar* exemplar) {
    IMySimHost* host = Host();
    cGZPersistResourceKey key = kDefaultCarExemplarKey;
    cIGZUnknown* loaded = nullptr;
    if (!exemplar && host) {
        host->LoadExemplar(key, kMySimExemplarIid, &loaded);
        exemplar = static_cast<cISCResExemplar*>(loaded);
    }
    carBitmapId_ = 0;                                                    // +0x320
    carModelKey_ = cGZPersistResourceKey{};                              // +0x3c0
    if (exemplar) {
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();    // 0x38
        cGZPersistResourceKey modelKey;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyModelResourceKey, modelKey)) {
            carModelKey_ = modelKey;
        }
        std::int32_t bitmap = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyBitmapId, bitmap)) carBitmapId_ = static_cast<std::uint32_t>(bitmap);
    }
    if (dispatch_) dispatch_->SetModelBaseResKey(0, carModelKey_, 0);    // 0x1006aff0e
    if (loaded) loaded->Release();
    return true;
}

bool cSC4MySim::SetPedestrian(cISCResExemplar* exemplar) {
    IMySimHost* host = Host();
    cGZPersistResourceKey key = kDefaultPedExemplarKey;
    cIGZUnknown* loaded = nullptr;
    if (!exemplar && host) {
        host->LoadExemplar(key, kMySimExemplarIid, &loaded);
        exemplar = static_cast<cISCResExemplar*>(loaded);
    }
    pedestrianBitmapId_ = 0;                                             // +0x31c
    pedestrianModelKey_ = cGZPersistResourceKey{};                       // +0x3cc
    if (exemplar) {
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        cGZPersistResourceKey modelKey;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyModelResourceKey, modelKey)) {
            pedestrianModelKey_ = modelKey;
        }
        std::int32_t bitmap = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyBitmapId, bitmap)) {
            pedestrianBitmapId_ = static_cast<std::uint32_t>(bitmap);
        }
    }
    const cGZPersistResourceKey walkKey = GetRandomWalkModelKey();      // 0x1007aa8d0
    if (pedestrianModelKey_.instance == 0) pedestrianModelKey_ = walkKey;
    if (dispatch_) {
        dispatch_->SetModelBaseResKey(1, pedestrianModelKey_, 0);
        dispatch_->SetModelBaseResKey(2, walkKey, 0);
    }
    if (loaded) loaded->Release();
    return true;
}

// 0x1007aa8d0 GetRandomWalkModelKey(): asks the city for a random pedestrian model.
cGZPersistResourceKey cSC4MySim::GetRandomWalkModelKey() {
    cGZPersistResourceKey key;
    if (Host()) Host()->PickRandomWalkModelKey(key);
    return key;
}

// 0x1007aa9da / 0x1007aaab4 / 0x1007aab9e: the three tool wrappers. Each one creates its control, pushes it on the
// view (spView3D vslot 0xc8(control, 2)) and makes it the global mspViewInputControl.
bool cSC4MySim::CreateDispatchTool() {
    IMySimHost* host = Host();
    if (!host) return false;
    cIGZUnknown* tool = host->CreateDispatchToolControl(this);
    if (!tool) return false;
    tool->AddRef();
    host->ViewPushInputControl(tool);
    if (host->CurrentViewInputControl() != tool) host->SetCurrentViewInputControl(tool);
    tool->Release();
    return true;
}

bool cSC4MySim::CreateChangeHouseTool() {
    IMySimHost* host = Host();
    if (!host) return false;
    cIGZUnknown* tool = host->CreateChangeHouseTool(this);
    if (!tool) return false;
    tool->AddRef();
    host->ViewPushInputControl(tool);
    if (host->CurrentViewInputControl() != tool) host->SetCurrentViewInputControl(tool);
    tool->Release();
    return true;
}

bool cSC4MySim::CreateChangeJobTool() {
    IMySimHost* host = Host();
    if (!host) return false;
    cIGZUnknown* tool = host->CreateChangeJobTool(this);
    if (!tool) return false;
    tool->AddRef();
    host->ViewPushInputControl(tool);
    if (host->CurrentViewInputControl() != tool) host->SetCurrentViewInputControl(tool);
    tool->Release();
    return true;
}

// 0x1007aab7c get_track_handle(): the dispatch view's track handle, or the cached one without a dispatch.
int cSC4MySim::get_track_handle() {
    if (dispatch_) return dispatch_->TrackHandle();
    return trackHandle_;
}

// 0x1007aac66 / 0x1007aaca2 / 0x1007aad14
bool cSC4MySim::CanUDriveIt() {
    if (state_ == 0) return false;
    return dispatch_ && dispatch_->CanUDriveIt();
}

bool cSC4MySim::EnterUDriveIt() {
    if (!CanUDriveIt()) return false;
    return dispatch_ && dispatch_->EnterUDriveIt();
}

bool cSC4MySim::ExitUDriveIt() {
    if (state_ == 0) return false;
    return dispatch_ && dispatch_->ExitUDriveIt();
}

// ---------------------------------------------------------------------------------------------------------------
// Lua context (0x10079ea2a CreateLuaTable, 0x10079e9c6 ActivateLuaContext)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySim::CreateLuaTable() {
    cISCLua* lua = lua_.get();
    if (!lua) return;
    lua->NewTable();                                                     // 0x160
    luaTableRef_ = lua->Ref(1);                                          // 0x190
    UpdateTripVariables(-1, -1, -1, -1, true);
}

bool cSC4MySim::ActivateLuaContext() {
    cISCLua* lua = lua_.get();
    if (!lua) return false;
    if (luaTableRef_ == 0) CreateLuaTable();
    if (luaTableRef_ < 1) return false;
    lua->GetRef(luaTableRef_);                                           // 0x158
    lua->SetGlobal("mysim");                                             // 0x168
    return true;
}

}  // namespace sc4
