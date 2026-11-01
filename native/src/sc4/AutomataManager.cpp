// cSC4AutomataManager — see AutomataManager.h for the layout, the seams and the quirks. Every function here follows the
// decompilation at the address in its comment; the places where a value had to be inferred are marked "inferred" and are
// also listed in notes/ledger_pending/cSC4AutomataManager.txt.
#include "sc4/AutomataManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gz/cIGZString.h"
#include "rz/cRZCmdLine.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

IAutomataManagerHost* g_host = nullptr;

// ---- class statics (the original keeps these as data symbols, not members) ------------------------------------------------
int snStaticInstanceCount = 0;                       // 0x100b21b00
IAutomataAutomaton* spCamFollow = nullptr;           // 0x100a...
float sCamVelocity[3] = {0.0f, 0.0f, 0.0f};          // 0x100b21850
float sCamFollowLastPosition[3] = {0.0f, 0.0f, 0.0f};   // 0x100b2185c
int sCamFollowLastRotation = 0;                      // 0x100b21870
bool sbCameraChanging = false;                       // 0x100b21874
bool sbSimSpeedPaused = false;                       // 0x100b21b10
float sfSimSpeedModifier = 1.0f;                     // 0x100b21b0c
IAutomataPathView* spRoadPathView = nullptr;         // file static (__ZL14spRoadPathView)
IAutomataPlayerDriveControl* spPlayerDriveView = nullptr;
AutomataTuningGlobals sTuning;
constexpr std::uint16_t kWriteVersion = 6;
constexpr std::uint16_t kMaxReadVersion = 7;
constexpr float kViewRectMargin = 64.0f;
constexpr std::uint32_t kAutomatonExemplarGroupProperty = 0x10;

const std::uint32_t kRegisteredMessages[] = {
    kMsgCameraMoved,        kMsgCameraTargetDone,   kMsgViewFilterChanged, kMsgPostCityInit,         kMsgCityShutdown,
    kMsgOccupantInserted,   kMsgOccupantRemoved,    kMsgBuildingCompleted, kMsgAutomataEnabled,      kMsgControllerRemoved,
    kMsgRenderPropertiesChanged, kMsgScrollStart,   kMsgScrollEnd,         kMsgOccupantGroupAdded};

// The rounding idiom of the original (see AutomataGenerator.h): n = round(v - 0.5); n + 1 when v == (float)(n + 1).
int FloorIdiom(float v) {
    // AutomataRoundMagic of (v - 0.5): the biased float's bit pattern minus the 12582912.0f exponent (see AutomataGenerator.h).
    const float biased = v - 0.5f + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof bits);
    const int n = static_cast<int>(bits) - 0x4b400000;
    const int n1 = n + 1;
    const float fn1 = static_cast<float>(n1);
    if (v != fn1 || std::isnan(v) || std::isnan(fn1)) return n;
    return n1;
}

std::string GroupNotFoundText(std::uint32_t group) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer,
                  "Couldn't find automata manager for group 0x%4x!  (No automata exemplar instances exist for that automata "
                  "group?)",
                  group);
    return buffer;
}

// Reads a "byte list" property into the bit mask the exemplar data keeps: bit (value - 1); 0xffffffff when the property is
// missing or holds a zero byte (the original's two loops, 0x1001037a8).
bool ReadByteMask(const cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t& out, bool zeroBased) {
    std::vector<std::uint8_t> values;
    if (!cSCPropertyHelp::GetPropertyValue(holder, id, values)) {
        out = 0xffffffffu;
        return false;
    }
    out = 0;
    for (std::uint8_t value : values) {
        if (value == 0) {
            if (!zeroBased) {
                out = 0xffffffffu;
                return false;
            }
            continue;
        }
        out |= 1u << ((zeroBased ? value : static_cast<unsigned>(value - 1)) & 31);
    }
    return true;
}

void ReadCurve(const cISCPropertyHolder* holder, std::uint32_t id, cSC4ResponseCurve& curve) {
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, id, values) && !values.empty())
        curve.SetResponseCurveData(values.data(), values.size());
}

void ReadFloats(const cISCPropertyHolder* holder, std::uint32_t id, float* out, std::size_t count) {
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, id, values))
        for (std::size_t i = 0; i < count && i < values.size(); ++i) out[i] = values[i];
}

void ReadUints(const cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t* out, std::size_t count) {
    std::vector<std::uint32_t> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, id, values))
        for (std::size_t i = 0; i < count && i < values.size(); ++i) out[i] = values[i];
}

void ReadBytes(const cISCPropertyHolder* holder, std::uint32_t id, std::uint8_t* out, std::size_t count) {
    std::vector<std::uint8_t> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, id, values))
        for (std::size_t i = 0; i < count && i < values.size(); ++i) out[i] = values[i];
}

void ReadTuningParametersImpl() {
    if (!g_host) return;
    cISCPropertyHolder* holder = nullptr;
    if (!g_host->GetTuningProperties(&holder) || !holder) return;
    auto f = [&](std::uint32_t id, float& value) { cSCPropertyHelp::GetPropertyValue(holder, id, value); };
    auto u = [&](std::uint32_t id, std::uint32_t& value) { cSCPropertyHelp::GetPropertyValue(holder, id, value); };
    auto b = [&](std::uint32_t id, std::uint8_t& value) { cSCPropertyHelp::GetPropertyValue(holder, id, value); };
    auto curve = [&](std::uint32_t id, cSC4ResponseCurve& value) { ReadCurve(holder, id, value); };

    // 0x100b218b0 .. 0x100b219c4: the curves and the values that are not part of cSC4AutomataTuningParams.
    curve(0x4998c89c, sTuning.f218b0);
    f(0x4998c89e, sTuning.f218d0);
    curve(0x4998c901, sTuning.f21980);
    f(0x4998c903, sTuning.f219a0);
    curve(0x4a511873, sTuning.f218d8);
    f(0x4998c89e, sTuning.f218f8);
    curve(0xa51188a, sTuning.f21900);
    f(0xea511de0, sTuning.f21920);
    curve(0x2a511893, sTuning.f21928);
    f(0xca511de6, sTuning.f21948);
    curve(0xca51189a, sTuning.f21950);
    f(0xca511df2, sTuning.f21970);
    // 0x4998c89f reads into the first field of the tuning params (the original passes &sTuningParams; inferred).
    u(0x4998c89f, reinterpret_cast<std::uint32_t&>(sTuning.params.f000));
    f(0x4998c90b, sTuning.f218a8);
    ReadUints(holder, 0x4998c900, &sTuning.f21884, 2);
    ReadBytes(holder, 0x4998c902, &sTuning.f2188c, 2);
    u(0x4998c904, sTuning.f21894);
    ReadUints(holder, 0x4998c905, &sTuning.f21898, 2);
    ReadBytes(holder, 0x4998c909, &sTuning.f218a0, 2);
    f(0x2a511cbc, sTuning.f219b8);
    // 0x4998c912..0x4998c916
    f(0x4998c912, sTuning.f219a4);
    f(0x4998c913, sTuning.f219a8);
    f(0x4998c914, sTuning.f219ac);
    f(0x4998c915, sTuning.f219b0);
    f(0x4998c916, sTuning.f219b4);
    ReadUints(holder, 0x4998c90a, &sTuning.f219c0, 2);
    u(0x4998c91d, sTuning.f219c8);
    ReadByteMask(holder, 0xa9af1e54, reinterpret_cast<std::uint32_t&>(sTuning.f219cc), false);
    ReadByteMask(holder, 0xa9af1e55, reinterpret_cast<std::uint32_t&>(sTuning.f219d0), false);
    b(0x4998c918, sTuning.f219d4);
    b(0x4998c919, sTuning.f219d5);
    if (sTuning.f219d4) --sTuning.f219d4;
    if (sTuning.f219d5) --sTuning.f219d5;
    // The per-layer clock curves: 1, 2, 5, 4, 3, 7, 8, 11 in the original's call order.
    AutomataTuningGlobals& t = sTuning;
    auto readClock = [&](std::uint32_t id, PopulateLayer layer) {
        cSC4ResponseCurve& c = t.clockCurves[static_cast<std::uint32_t>(layer)];
        ReadCurve(holder, id, c);
    };
    readClock(0x4998c90c, PopulateLayer::kLayer1);
    readClock(0x4998c90d, PopulateLayer::kLayer2);
    readClock(0x4998c90e, PopulateLayer::kLayer5);
    readClock(0x4998c90f, PopulateLayer::kLayer4);
    readClock(0x4998c911, PopulateLayer::kLayer3);
    readClock(0x4998c910, PopulateLayer::kLayer7);
    readClock(0x6a09f631, PopulateLayer::kLayer8);
    readClock(0xec06e7cf, PopulateLayer::kLayer11);
    cSCPropertyHelp::GetPropertyValue(holder, 0x4998c91c, sTuning.f21aa8);
    ReadByteMask(holder, 0x4998c89d, reinterpret_cast<std::uint32_t&>(sTuning.f21aac), true);
    f(0x89aec88b, sTuning.f219f0);
    f(0x89aec88c, reinterpret_cast<float&>(sTuning.f219f4));
    b(0x89aec88d, sTuning.f219f5);
    f(0x89aec88e, sTuning.f219f8);
    f(0xc9ee640d, reinterpret_cast<float&>(sTuning.f219fc));
    cSCPropertyHelp::GetPropertyValue(holder, 0x49be9565, sTuning.f219fd);
    f(0xc9c18eae, sTuning.f21a00);
    ReadFloats(holder, 0xa9d6cc17, &sTuning.f21a04, 2);
    ReadFloats(holder, 0xa9d6cc18, &sTuning.f21a0c, 3);
    f(0xc9ee640c, sTuning.f21a18);
    {
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(holder, 0xc9da95d4, values)) {
            sTuning.f21a1c = values.size() > 0 ? values[0] : -90.0f;
            sTuning.f21a20 = values.size() > 1 ? values[1] : 90.0f;
        }
    }
    {
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x29dd40c1, values)) {
            sTuning.f21a24 = values.size() > 0 ? values[0] : -1.0f;
            sTuning.f21a28 = values.size() > 1 ? values[1] : 1.0f;
        }
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x29dd40c2, values)) {
            sTuning.f21a2c = values.size() > 0 ? values[0] : -1.0f;
            sTuning.f21a30 = values.size() > 1 ? values[1] : 1.0f;
        }
    }
    sTuning.f21a34 = 0.0f;
    f(0xc9ee640a, sTuning.f21a34);
    sTuning.f21a34 *= 0.017453292f;
    f(0xc9ee640b, sTuning.f21a38);
    sTuning.f21a3c = 0.0f;
    f(0x6a09f62f, sTuning.f21a3c);
    ReadBytes(holder, 0x6a09f630, &sTuning.f21a40, 2);
    ReadBytes(holder, 0xea441c97, &sTuning.f21a42, 2);
    ReadBytes(holder, 0x8a441c9c, &sTuning.f21a44, 2);
    sTuning.f21a50 = 0x14;
    u(0xa53fd32, sTuning.f21a50);
    ReadUints(holder, 0x6a440d40, &sTuning.f21a48, 2);
    {
        std::uint32_t count = 100;
        u(0xca5545ea, count);
        sTuning.f219bc = static_cast<float>(count) * 0.0002777778f;
    }
    ReadFloats(holder, 0x6b42922c, &sTuning.f21a54, 4);
    u(0x6bac0b3e, sTuning.f21a64);
    sTuning.f219e0 = sTuning.f219d8.empty() ? 0.0f : sTuning.f219d8[0];
    {
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x4bae8fe0, values)) sTuning.f219d8 = values;
    }
    f(0x4bae8fe1, sTuning.f21a68);
    f(0x4bae8fe2, sTuning.f21a6c);
    f(0xec0700c0, sTuning.f21974);
    f(0x6c0700c4, sTuning.f21978);
    // The spawn/recall parameters, in the original's network-type order.
    auto spawn = [&](std::uint32_t id, std::int32_t type) {
        AutomataTuningGlobals::SpawnRecallParams& p = t.spawnRecall[type];
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(holder, id, values))
            for (std::size_t i = 0; i < 4 && i < values.size(); ++i)
                (&p.f0)[i] = values[i];
        ReadCurve(holder, id + 0x100, p.curve);
    };
    spawn(0xcba5895b, 3);
    spawn(0xcba5895c, 0);
    spawn(0xcba5895d, 6);
    spawn(0xcba5895e, 2);
    spawn(0xcba5895f, 1);
    {
        std::uint32_t count = 0x32;
        u(0x8c3e8185, count);
        sTuning.f21a7c = count ? 1.0f / static_cast<float>(count) : 0.0f;
    }
    f(0xebfb4a0a, sTuning.f21a70);
    f(0xcc101142, sTuning.f21a74);
    f(0x6c200139, sTuning.f21a78);
    holder->Release();
}

}  // namespace

const float kAutomataScrollGranularitySquared[8] = {216783.359375f, 60712.95703125f, 13271.0390625f,
                                                    3136.0f,       1600.0f,        0.0f,
                                                    0.0f,          0.0f};
const float kAutomataCameraNoseFraction[5] = {0.083333f, 0.083333f, 0.083333f, 0.1f, 0.125f};

IAutomataManagerHost* AutomataManagerHost() { return g_host; }
void SetAutomataManagerHost(IAutomataManagerHost* host) { g_host = host; }
AutomataTuningGlobals& AutomataTuning() { return sTuning; }

cSC4AutomataManager::cSC4AutomataManager() = default;

cSC4AutomataManager::~cSC4AutomataManager() {
    // The original's destructor destroys the containers; the exemplar references it owns are dropped here too.
    for (tExemplarData& data : exemplarData_)
        if (data.exemplar) data.exemplar->Release();
    exemplarData_.clear();
}

// ---- COM -------------------------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    void* view = nullptr;
    if (iid == kGZIID_cISC4AutomataManager) {
        view = static_cast<cISC4AutomataManager*>(this);
    } else if (iid == kGZIID_cIGZSerializableView) {
        view = static_cast<cIGZSerializable*>(this);
    } else if (iid == 1) {
        view = static_cast<cIGZMessageTarget*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2View) {
        view = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    *out = view;
    AddRef();
    return true;
}

std::uint32_t cSC4AutomataManager::Release() {
    if (refCount_ > 1) return --refCount_;
    Shutdown();
    delete this;
    return 0;
}

// ---- lifecycle -------------------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::StaticInit() {
    if (snStaticInstanceCount == 0) {
        if (g_host && g_host->CheatManagerAvailable() && g_host->DebugFunctionalityEnabled()) {
            g_host->RegisterCheat(kCheatDrawAutomataPaths, "DrawPaths");
            g_host->RegisterCheat(kCheatHideAutomataPaths, "HidePaths");
            g_host->RegisterCheat(kCheatPlayerDrive, "udriveit");
            g_host->RegisterCheat(kCheatNoCsi, "NoCSI");
            g_host->RegisterCheat(kCheatGhostModels, "Casper");
        }
        ReadTuningParametersImpl();
    }
    ++snStaticInstanceCount;
    return true;
}

bool cSC4AutomataManager::StaticShutdown() {
    --snStaticInstanceCount;
    if (snStaticInstanceCount == 0) {
        SetCameraFollow(nullptr);
        if (g_host) g_host->DebugViewShutdown();
        if (spRoadPathView) {
            if (g_host) g_host->RemoveRendererLayer(spRoadPathView);
            spRoadPathView->Shutdown();
            spRoadPathView->Release();
            spRoadPathView = nullptr;
        }
        if (g_host && g_host->CheatManagerAvailable()) {
            g_host->UnregisterCheat(kCheatDrawAutomataPaths);
            g_host->UnregisterCheat(kCheatHideAutomataPaths);
            g_host->UnregisterCheat(kCheatPlayerDrive);
            g_host->UnregisterCheat(kCheatNoCsi);
            g_host->UnregisterCheat(kCheatGhostModels);
        }
    }
    return true;
}

bool cSC4AutomataManager::Init() {
    if (initialized_) return true;
    initialized_ = true;
    StaticInit();
    if (g_host) {
        if (cIGZMessageServer2* server = g_host->MessageServer())
            for (std::uint32_t message : kRegisteredMessages)
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
        g_host->AddTickNotification(static_cast<cIGZMessageTarget2*>(this), kAutomataTickChannel);
        if (g_host->CheatManagerAvailable()) g_host->AddCheatListener(static_cast<cIGZMessageTarget*>(this));
        if (!networkManager_) {
            networkManager_ = g_host->NetworkManager();
            if (networkManager_) networkManager_->AddRef();
        }
        if (!scriptGroups_) {
            scriptGroups_ = g_host->ScriptGroups();
            if (scriptGroups_) scriptGroups_->AddRef();
        }
        if (!cityClock_) {
            cityClock_ = g_host->CityClock();
            if (cityClock_) cityClock_->AddRef();
        }
    }
    SeedTrackedBuildingsMap();
    ScanExemplars();
    return true;
}

bool cSC4AutomataManager::Shutdown() {
    if (!initialized_) return true;
    DestroyAutomata();
    if (cityClock_) {
        cityClock_->Release();
        cityClock_ = nullptr;
    }
    if (scriptGroups_) {
        scriptGroups_->Release();
        scriptGroups_ = nullptr;
    }
    if (networkManager_) {
        networkManager_->Release();
        networkManager_ = nullptr;
    }
    if (g_host) {
        if (g_host->CheatManagerAvailable()) g_host->RemoveCheatListener(static_cast<cIGZMessageTarget*>(this));
        g_host->RemoveTickNotification(static_cast<cIGZMessageTarget2*>(this), kAutomataTickChannel);
        if (cIGZMessageServer2* server = g_host->MessageServer())
            for (std::uint32_t message : kRegisteredMessages)
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), message);
    }
    exemplarDataMap_.clear();
    for (tExemplarData& data : exemplarData_) {
        if (data.exemplar) {
            data.exemplar->Release();
            data.exemplar = nullptr;
        }
    }
    StaticShutdown();
    initialized_ = false;
    return true;
}

void cSC4AutomataManager::ReadTuningParameters() { ReadTuningParametersImpl(); }

void cSC4AutomataManager::ReadTuningClock(cISCPropertyHolder* holder, std::uint32_t property, PopulateLayer layer) {
    if (!holder) return;
    cSC4ResponseCurve& curve = sTuning.clockCurves[static_cast<std::uint32_t>(layer)];
    ReadCurve(holder, property, curve);
}

void cSC4AutomataManager::ReadSpawnRecallParameters(cISCPropertyHolder* holder, std::uint32_t property,
                                                    std::int32_t networkType) {
    if (!holder) return;
    AutomataTuningGlobals::SpawnRecallParams& params = sTuning.spawnRecall[networkType];
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, property, values))
        for (std::size_t i = 0; i < 4 && i < values.size(); ++i) (&params.f0)[i] = values[i];
    ReadCurve(holder, property + 0x100, params.curve);
}

// ---- views / activation ----------------------------------------------------------------------------------------------------
void cSC4AutomataManager::Activate(bool active) {
    active_ = active;
    if (g_host) g_host->ActiveInputControl();   // the original looks the current input control up and drops it
    for (IAutomataAutomaton* automaton : automata_)
        if (automaton) automaton->SetVisible(active_);
}

void cSC4AutomataManager::HandleViewChanged() {
    for (int i = 0; i < 4; ++i) previousViewRect_[i] = viewRect_[i];
    if (g_host) {
        if (IAutomataViewUtilities* view = g_host->ViewUtilities()) {
            view->ViewRect(viewRect_);
            viewRect_[0] -= kViewRectMargin;
            viewRect_[1] -= kViewRectMargin;
            viewRect_[2] += kViewRectMargin;
            viewRect_[3] += kViewRectMargin;
        }
    }
    UpdateZoomAndRotation();
}

bool cSC4AutomataManager::HandleViewZoomChanged(int oldZoom, int newZoom) {
    if (oldZoom != newZoom) UpdateZoomAndRotation();
    return true;
}

bool cSC4AutomataManager::HandleViewFilterChanged(const IAutomataViewFilterInformation* oldFilter,
                                                 const IAutomataViewFilterInformation* newFilter) {
    (void)oldFilter;   // the original only uses the second filter
    int zoom = 0, rotation = 0;
    if (g_host)
        if (IAutomataViewUtilities* view = g_host->ViewUtilities()) view->GetZoomAndRotation(zoom, rotation);
    active_ = zoom >= minZoom_;
    for (IAutomataAutomaton* automaton : automata_) {
        if (!automaton) continue;
        bool visible = active_;
        if (newFilter && !newFilter->Includes(automaton->GetAutomataGroup())) visible = false;
        automaton->SetVisible(visible);
    }
    return true;
}

void cSC4AutomataManager::UpdateZoomAndRotation() {
    int zoom = 0, rotation = 0;
    if (g_host)
        if (IAutomataViewUtilities* view = g_host->ViewUtilities()) view->GetZoomAndRotation(zoom, rotation);
    if (zoom < minZoom_) {
        bool driven = spPlayerDriveView && spPlayerDriveView->HasDrivenAutomaton() &&
                      CanCreateGroup(spPlayerDriveView->DrivenGroup());
        if (driven) {
            if (!active_) Activate(true);
        } else if (active_) {
            Activate(false);
        }
    } else if (!active_) {
        Activate(true);
    }
    if (!active_) return;
    for (IAutomataAutomaton* automaton : automata_)
        if (automaton) automaton->SetZoomAndRotation(zoom, rotation);
}

// ---- automata registry -----------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::AddAutomaton(IAutomataAutomaton* automaton) {
    if (!automaton) return true;
    automaton->AddRef();
    automaton->Init();
    automata_.push_front(automaton);
    for (auto& entry : trackedGroups_) {
        IAutomataOccupant* occupant = automaton->Occupant();
        if (!occupant || !occupant->IsOccupantGroup(entry.first)) continue;
        if (entry.second.insert(automaton).second) automaton->AddRef();
    }
    AddAutomatonInternal(automaton);
    bool visible = active_;
    IAutomataPlayerDriveControl* control = g_host ? g_host->ActiveInputControl() : nullptr;
    if (control) {
        if (!control->HandlesGroup(automaton->GetAutomataGroup())) visible = false;
        control->Release();
    }
    automaton->SetVisible(visible);
    return true;
}

bool cSC4AutomataManager::AddAutomatonInternal(IAutomataAutomaton* automaton) {
    if (!automaton) return false;
    IAutomataOccupant* occupant = automaton->Occupant();
    if (!occupant) return false;
    if ((occupant->GetFlags() & 4) == 0) return false;
    if (automaton->IsPlacedInOccupantManager()) return false;
    if (!automaton->PlaceInOccupantManager(true)) return false;
    if (!automaton->IsPlacedInRenderer()) automaton->PlaceInRenderer(true);
    return true;
}

bool cSC4AutomataManager::RemoveAutomatonInternal(IAutomataAutomaton* automaton) {
    if (!automaton) return true;
    if (automaton->IsPlacedInRenderer()) automaton->PlaceInRenderer(false);
    if (automaton->IsPlacedInOccupantManager()) automaton->PlaceInOccupantManager(false);
    return true;
}

bool cSC4AutomataManager::RemoveAutomaton(IAutomataAutomaton* automaton, bool force) {
    if (!automaton) return true;
    if (force) {
        automaton->SendCallbackMessage(3, 0);
        automaton->SetMarkedForRemove(true);
        if (g_host) g_host->DebugAutomatonRemoved(automaton);
        for (auto& entry : trackedGroups_) {
            IAutomataOccupant* occupant = automaton->Occupant();
            if (!occupant || !occupant->IsOccupantGroup(entry.first)) continue;
            auto it = entry.second.find(automaton);
            if (it == entry.second.end()) continue;
            entry.second.erase(it);
            automaton->Release();
        }
        for (auto it = automata_.begin(); it != automata_.end(); ++it) {
            if (*it != automaton) continue;
            automata_.erase(it);
            break;
        }
        if (spCamFollow == automaton) SetCameraFollow(nullptr);
        RemoveAutomatonInternal(automaton);
        automaton->Shutdown();
        automaton->Release();
    } else {
        automaton->SendCallbackMessage(2, 0);
        automaton->SetMarkedForRemove(true);
        automaton->SetOpacity(0.0f, sTuning.f219b4);
        automaton->StopEffectsAndClearModel();
    }
    return true;
}

void cSC4AutomataManager::DestroyAutomata() {
    for (auto& entry : trackedGroups_) ReleaseAutomatonSet(entry.second);
    trackedGroups_.clear();
    for (auto it = automata_.begin(); it != automata_.end();) {
        IAutomataAutomaton* automaton = *it;
        ++it;
        RemoveAutomatonInternal(automaton);
        if (automaton == spCamFollow) SetCameraFollow(nullptr);
        automaton->Shutdown();
        automaton->Release();
    }
    automata_.clear();
}

void cSC4AutomataManager::SetMaxCount(int count) {
    const std::uint32_t wanted = count < 0 ? 0u : static_cast<std::uint32_t>(count);
    const std::uint32_t limit = wanted < 5000u ? wanted : 5000u;
    if (limit == maxCount_) return;
    for (auto it = automata_.begin(); it != automata_.end() && limit < automata_.size();) {
        IAutomataAutomaton* automaton = *it;
        ++it;
        if (automaton->CheckAutomataFlag(0x200000)) continue;
        if (automaton->GetPriority() == 0xff) continue;
        RemoveAutomatonInternal(automaton);
    }
    maxCount_ = limit;
    maxCountLimit_ = static_cast<std::uint32_t>(static_cast<double>(limit) * 1.25);
}

bool cSC4AutomataManager::AdjustCountForAutomaton(std::uint8_t priority) {
    if (priority != 0 && automata_.size() < maxCountLimit_) {
        for (IAutomataAutomaton* automaton : automata_) {
            if (automaton->GetPriority() >= priority) continue;
            automaton->Remove(false);
            return true;
        }
    }
    return false;
}

bool cSC4AutomataManager::IsAutomatonOnscreen(IAutomataAutomaton* automaton) {
    if (!automaton) return false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    automaton->GetPosition(position);
    if (!g_host) return false;
    IAutomataViewUtilities* view = g_host->ViewUtilities();
    return view ? view->IsPointInViewFrustum(position) : false;
}

IAutomataAutomaton* cSC4AutomataManager::GetCollidingAutomaton(IAutomataAutomaton* automaton, float margin) {
    if (!automaton) return nullptr;
    const float radius = automaton->GetRadius();
    float position[3] = {0.0f, 0.0f, 0.0f};
    automaton->GetPosition(position);
    IAutomataAutomaton* best = nullptr;
    float bestDistance = 0.0f;
    for (IAutomataAutomaton* other : automata_) {
        if (!other || other == automaton) continue;
        if (!automaton->ShouldCollide(other)) continue;
        const float otherRadius = other->GetRadius();
        float otherPosition[3] = {0.0f, 0.0f, 0.0f};
        other->GetPosition(otherPosition);
        const float dx = otherPosition[0] - position[0];
        const float dy = otherPosition[1] - position[1];
        const float dz = otherPosition[2] - position[2];
        const float distance =
            std::sqrt(dx * dx + dy * dy + dz * dz) - (otherRadius + radius + margin);
        if (distance <= bestDistance) {
            best = other;
            bestDistance = distance;
        }
    }
    return best;
}

// ---- creation --------------------------------------------------------------------------------------------------------------
IAutomataAutomaton* cSC4AutomataManager::CreateAutomaton(std::uint32_t group, const cS3DVector3& position,
                                                         std::uint32_t flags, const cGZPersistResourceKey& key,
                                                         cIGZMessageTarget2* listener, std::uint8_t argument,
                                                         const cS3DVector3* velocity) {
    IAutomataAutomaton* automaton = CreateAutomatonInternal(group, flags, key, nullptr, argument);
    if (!automaton) return nullptr;
    automaton->AddCallback(listener);
    automaton->SetPosition(&position.x);
    if (velocity) automaton->SetOrientation(&velocity->x);
    AddAutomaton(automaton);
    automaton->Release();   // quirk: the creation reference is dropped and the borrowed pointer is returned
    return automaton;
}

IAutomataAutomaton* cSC4AutomataManager::CreateAutomatonInternal(std::uint32_t group, std::uint32_t flags,
                                                                const cGZPersistResourceKey& key,
                                                                const tExemplarData* data, std::uint8_t priority) {
    std::uint8_t spawn = 0;
    if (priority == 0xff) {
        spawn = 0xff;
    } else {
        IAutomataGroupBudget* budget = g_host ? g_host->GroupBudget() : nullptr;
        bool usedBudget = false;
        if (budget && budget->HandlesGroup(group) && budget->CurrentCount() < budget->MaxCount()) {
            spawn = 0xfe;
            usedBudget = true;
        }
        if (!usedBudget) {
            spawn = priority;
            if (priority == 0 && !active_) return nullptr;
        }
    }
    if (maxCount_ < automata_.size() && AdjustCountForAutomaton(spawn) && spawn != 0xff &&
        static_cast<std::int8_t>(flags) >= 0 && !AdjustCountForAutomaton(spawn))
        return nullptr;

    const tExemplarData* exemplar = data;
    if (!exemplar) {
        exemplar = key.instance == 0 ? ChooseRandomExemplar(group, PopulateLayer::kLayerNone, -1, -1)
                                     : FindExemplarFromModelKey(key);
    }
    const std::uint32_t groupId = exemplar ? exemplar->defaultGroup : group;
    IAutomataGroupTemplate* groupTemplate = scriptGroups_ ? scriptGroups_->GetAutomataGroup(groupId) : nullptr;
    bool haveTemplate = false;
    std::uint32_t classId = fallbackGroup_;
    if (!groupTemplate) {
        groupTemplate = scriptGroups_ ? scriptGroups_->GetAutomataGroup(defaultGroup_) : nullptr;
    }
    if (groupTemplate) {
        classId = groupTemplate->ClassId();
        haveTemplate = true;
        if (classId == 0) {
            haveTemplate = false;
            classId = fallbackGroup_;
        }
    }
    IAutomataAutomaton* automaton = g_host ? g_host->CreateAutomatonByClassId(classId) : nullptr;
    if (!automaton) return nullptr;
    automaton->SetPriority(spawn);
    automaton->SetOpacity(static_cast<float>(flags), 1.0f);
    auto applyExemplar = [&]() {
        if (!exemplar) return;
        automaton->SetExemplar(exemplar->exemplar);
        cRZVariant variant;
        variant.RefUint32(exemplar->groupIds, exemplar->groupCount);
        automaton->SetPropertyVariant(kPropertyOccupantGroups, variant, false);
        if (exemplar->flag50) automaton->SetAutomataFlag(0x100000, true);
    };
    // The model key comes from the caller when it is non-zero, from the exemplar otherwise.
    if (key.instance != 0) automaton->AddModelResourceKey(key);
    else if (exemplar) automaton->AddModelResourceKey(exemplar->modelKey);
    applyExemplar();
    if (haveTemplate) {
        cRZVariant& variant = groupTemplate->GroupIdVariant();
        automaton->SetPropertyVariant(kPropertyOccupantGroups, variant, false);
    }
    automaton->SetAutomataGroup(haveTemplate ? groupTemplate : nullptr);
    automaton->SetOpacity(0.0f, 0.0f);
    automaton->SetOpacity(1.0f, sTuning.f219b4);
    return automaton;
}


bool cSC4AutomataManager::GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& origin, float minDistance,
                                                 float maxDistance, const cS3DVector3& center, cS3DVector3& direction,
                                                 std::uint32_t flags) {
    (void)origin;   // the second location argument is not read by the original
    if (!CanCreateGroup(group)) return false;
    if (!g_host) return false;
    const int cellCount = g_host->CityCellCount();
    const float cellSize = g_host->CellSize();
    float span = cellSize * static_cast<float>(cellCount);
    if (maxDistance <= span) span = maxDistance;
    const float base = maxDistance < minDistance ? minDistance : span;
    const double random = random_.RandomDoubleUniform();
    const float distance = static_cast<float>((static_cast<double>(base) * random - static_cast<double>(minDistance)) +
                                              static_cast<double>(minDistance));
    double draw = random_.RandomDoubleUniform();
    direction.x = static_cast<float>(draw + draw - 1.0);
    float height = 0.0f;
    if ((flags & 0x40000) != 0) height = static_cast<float>(random_.RandomDoubleUniform());
    direction.y = height;
    draw = random_.RandomDoubleUniform();
    const float third = static_cast<float>(draw + draw - 1.0);
    const float length = 1.0f / std::sqrt(third * third + direction.y * direction.y + direction.x * direction.x);
    direction.x *= length;
    direction.y *= length;
    direction.z = length * third;
    direction.x = direction.x * distance + center.x;
    direction.y = direction.y * distance + center.y;
    direction.z = direction.z * distance + center.z;
    if ((flags & 0x40000) == 0) direction.y = g_host->TerrainHeight(direction.x, direction.z);
    return true;
}

IAutomataKindManager* cSC4AutomataManager::GetAutomataManagerForGroup(std::uint32_t group) {
    if (!g_host) return nullptr;
    IAutomataKindManager* last = nullptr;
    for (int index = 0; index < 4; ++index) {
        IAutomataKindManager* manager = g_host->KindManager(index);
        last = manager;
        if (manager && manager->HandlesGroup(group)) return manager;
    }
    g_host->Report(GroupNotFoundText(group));
    return last;
}

IAutomataAutomaton* cSC4AutomataManager::StaticCreateAutomaton(std::uint32_t group, const cS3DVector3& position,
                                                               std::uint32_t flags, const cGZPersistResourceKey& key,
                                                               cIGZMessageTarget2* listener, std::uint8_t argument) {
    IAutomataKindManager* manager = GetAutomataManagerForGroup(group);
    if (!manager) return nullptr;
    return manager->CreateAutomaton(group, position, flags, key, listener, argument, nullptr);
}

// ---- dispatch location translation -------------------------------------------------------------------------------------------
bool cSC4AutomataManager::TranslateDispatchLocation(const tAutomataLocation& location, tCellRect& rect) {
    switch (location.kind) {
        case kLocationOccupant: {
            if (!location.occupant) return false;
            if (location.occupant->GetType() == kBuildingOccupantType) {
                if (!g_host) return false;
                if (!g_host->OccupantCells(location.occupant, rect)) return false;
                return true;
            }
            float position[3] = {0.0f, 0.0f, 0.0f};
            location.occupant->GetPosition(position);
            int cellX = 0, cellZ = 0;
            if (!g_host || !g_host->WorldToCell(position[0], position[2], cellX, cellZ)) return false;
            rect.x0 = cellX;
            rect.x1 = cellX;
            rect.z0 = cellZ;   // quirk: z1 is left untouched in this path
            return true;
        }
        case kLocationWorldPosition:
            rect.x0 = rect.x1 = FloorIdiom(location.x * 0.0625f);
            rect.z0 = rect.z1 = FloorIdiom(location.z * 0.0625f);
            return true;
        case kLocationCell:
            rect.x0 = rect.x1 = static_cast<std::int32_t>(location.x);
            rect.z0 = rect.z1 = static_cast<std::int32_t>(location.y);
            return true;
        case kLocationCellRect:
            rect.x0 = static_cast<std::int32_t>(location.x);
            rect.z0 = static_cast<std::int32_t>(location.y);
            rect.x1 = static_cast<std::int32_t>(location.z);
            rect.z1 = static_cast<std::int32_t>(location.w);
            return true;
        default:
            return false;
    }
}

bool cSC4AutomataManager::TranslateDispatchLocation(const tAutomataLocation& location, cS3DVector3& position) {
    if (!g_host) return false;
    switch (location.kind) {
        case kLocationOccupant:
            if (!location.occupant) return false;
            location.occupant->GetPosition(&position.x);
            return true;
        case kLocationWorldPosition:
            position.x = location.x;
            position.y = location.y;
            position.z = location.z;
            return true;
        case kLocationCell: {
            float x = 0.0f, z = 0.0f;
            if (!g_host->CellToWorld(static_cast<int>(location.x), static_cast<int>(location.y), x, z)) return false;
            position.x = x;
            position.z = z;
            break;
        }
        case kLocationCellRect: {
            float x0 = 0.0f, z0 = 0.0f, x1 = 0.0f, z1 = 0.0f;
            if (!g_host->CellToWorld(static_cast<int>(location.x), static_cast<int>(location.y), x0, z0)) return false;
            if (!g_host->CellToWorld(static_cast<int>(location.z), static_cast<int>(location.w), x1, z1)) return false;
            position.x = (x1 - x0) * 0.5f + x0;
            position.z = (z1 - z0) * 0.5f + z0;
            break;
        }
        default:
            return false;
    }
    if (IAutomataNetworkManager* network = networkManager_ ? networkManager_ : g_host->NetworkManager())
        position.y = network->Height(position.x, position.z);
    return true;
}

// ---- chasing ---------------------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::ChaseAutomaton(IAutomataAutomaton* automaton, IAutomataAutomaton* target) {
    if (!automaton || !target || automaton == target) return false;
    if (automaton->FindState(0x10)) return false;
    if (automaton->CheckAutomataFlag(0x40)) return false;
    if (automaton->CheckAutomataFlag(1)) return false;
    if (automaton->FindState(0x34)) return false;
    if (!automaton->PushState(0x34, nullptr, -1.0f, false, 0, 0, nullptr)) return false;
    if (!automaton->GetState()) return false;
    if (automaton->StateTarget() != target) automaton->SetStateTarget(target);
    void* view = nullptr;
    if (automaton->QueryInterface(kGZIID_cIGZMessageTarget2View, &view) && view) {
        if (IAutomataAutomaton* chased = automaton->StateTarget()) chased->AddCallback(nullptr);
        if (g_host) g_host->ReleaseObject(static_cast<cIGZUnknown*>(view));
    }
    return true;
}

bool cSC4AutomataManager::ChaseAutomaton(std::uint32_t group, float radius, IAutomataAutomaton* target, bool all) {
    if (!target) return false;
    if (!CanCreateGroup(group)) return false;
    float center[3] = {0.0f, 0.0f, 0.0f};
    target->GetPosition(center);
    const float radiusSquared = radius * radius;
    std::unordered_set<IAutomataAutomaton*> candidates;
    if (!GetInstancesOfGroup(group, candidates)) {
        ReleaseAutomatonSet(candidates);
        return false;
    }
    bool result = false;
    if (all) {
        for (IAutomataAutomaton* automaton : candidates) {
            if (!automaton) continue;
            float position[3] = {0.0f, 0.0f, 0.0f};
            automaton->GetPosition(position);
            if (SC4GeometryUtilities::LengthOfLineSquared(position, center) < radiusSquared) {
                result = true;
                ChaseAutomaton(automaton, target);
            }
        }
    } else {
        IAutomataAutomaton* best = nullptr;
        float bestDistance = radiusSquared;
        for (IAutomataAutomaton* automaton : candidates) {
            if (!automaton) continue;
            float position[3] = {0.0f, 0.0f, 0.0f};
            automaton->GetPosition(position);
            const float distance = SC4GeometryUtilities::LengthOfLineSquared(position, center);
            if (distance < bestDistance) {
                best = automaton;
                bestDistance = distance;
            }
        }
        if (best) result = ChaseAutomaton(best, target);
    }
    ReleaseAutomatonSet(candidates);
    return result;
}

// ---- per tick --------------------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::CalculateSimSpeedModifier() {
    sfSimSpeedModifier = 1.0f;
    sbSimSpeedPaused = false;
    IAutomataSimulator* simulator = g_host ? g_host->Simulator() : nullptr;
    if (!simulator) return false;
    if (g_host->VehicleManagerPaused()) {
        sfSimSpeedModifier = sTuning.f21a60;
    } else if (simulator->IsPaused()) {
        sfSimSpeedModifier = sTuning.f21a54;
    } else if (!simulator->SpeedPanelVisible()) {
        sfSimSpeedModifier = sTuning.f21a54;
    } else if (simulator->IsFastForward()) {
        sfSimSpeedModifier = 0.0f;
        sbSimSpeedPaused = true;
    } else {
        const int index = simulator->SpeedIndex();
        const float* table = &sTuning.f21a54;
        if (index == 1) table = &sTuning.f21a58;
        else if (index == 2) table = &sTuning.f21a5c;
        sfSimSpeedModifier = *table;
    }
    return sbSimSpeedPaused;
}

float cSC4AutomataManager::GetClockMultiplier(PopulateLayer layer) {
    if (!sTuning.f21aa8) return 1.0f;
    if (!g_host) return 1.0f;
    IAutomataClock* clock = cityClock_ ? cityClock_ : g_host->CityClock();
    if (!clock) return 1.0f;
    const int hour = clock->Hour();
    const int minute = clock->Minute();
    std::uint32_t key = static_cast<std::uint32_t>(layer);
    if (key < 11 && ((0x640u >> (key & 31)) & 1)) key = static_cast<std::uint32_t>(PopulateLayer::kLayer4);
    auto it = sTuning.clockCurves.find(key);
    if (it == sTuning.clockCurves.end()) return 1.0f;
    const float input = static_cast<float>(minute) / 60.0f + static_cast<float>(hour);
    return it->second.GetOutputValue(input);
}

void cSC4AutomataManager::DoAnimationTick(std::uint32_t data, float deltaMilliseconds) {
    CalculateSimSpeedModifier();
    if (automata_.empty() || paused_) return;
    if (g_host) g_host->ActiveInputControl();
    for (auto it = automata_.begin(); it != automata_.end();) {
        IAutomataAutomaton* automaton = *it;
        ++it;
        if (!automaton) continue;
        IAutomataOccupant* occupant = automaton->Occupant();
        const bool flagged = occupant && (occupant->GetFlags() & 4) != 0;
        if (!flagged) {
            if (automaton->GetMarkedForRemove()) {
                RemoveAutomaton(automaton, true);
                continue;
            }
            if (sbSimSpeedPaused) continue;
            if (!automaton->CheckAutomataFlag(0x210040)) continue;
        }
        if (!automaton->CheckAutomataFlag(0x4000000)) {
            automaton->SetAutomataFlag(0x4000000, true);
            if (!automaton->PostInit()) RemoveAutomaton(automaton, false);
        }
        if (occupant && occupant->IsInitialized() && !automaton->DoAnimationTick(data, deltaMilliseconds))
            RemoveAutomaton(automaton, false);
        if (!automaton->GetMarkedForRemove() || automaton->GetOpacity() >= 0.1f) {
            if (!automaton->CheckAutomataFlag(0x10000) && !IsAutomatonOnscreen(automaton))
                RemoveAutomaton(automaton, false);
        } else {
            RemoveAutomaton(automaton, true);
        }
    }
}

// ---- camera follow ---------------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::IsCameraFollowUpdating() { return sbCameraChanging; }
int cSC4AutomataManager::StaticInstanceCount() { return snStaticInstanceCount; }
IAutomataAutomaton* cSC4AutomataManager::CameraFollowTarget() { return spCamFollow; }
bool cSC4AutomataManager::CameraChanging() { return sbCameraChanging; }
bool cSC4AutomataManager::SimSpeedPaused() { return sbSimSpeedPaused; }
float cSC4AutomataManager::SimSpeedModifier() { return sfSimSpeedModifier; }
IAutomataPathView* cSC4AutomataManager::PathView() { return spRoadPathView; }

void cSC4AutomataManager::ResetCameraFollowState() {
    if (spCamFollow) {
        spCamFollow->Release();
        spCamFollow = nullptr;
    }
    sCamVelocity[0] = sCamVelocity[1] = sCamVelocity[2] = 0.0f;
    sCamFollowLastPosition[0] = sCamFollowLastPosition[1] = sCamFollowLastPosition[2] = 0.0f;
    sCamFollowLastRotation = 0;
    sbCameraChanging = false;
}

void cSC4AutomataManager::SetCameraFollow(IAutomataAutomaton* automaton) {
    if (spCamFollow == automaton) return;
    if (spCamFollow) spCamFollow->Release();
    spCamFollow = automaton;
    if (!automaton) return;
    automaton->AddRef();
    if (!g_host) return;
    IAutomataCameraControl* camera = g_host->CameraControl();
    if (!camera) return;
    sbCameraChanging = true;
    if (g_host->CameraFollowFast() && camera->VirtualZoom() < 3) camera->SetZoom(4.0f);
    float position[3] = {0.0f, 0.0f, 0.0f};
    automaton->GetPosition(position);
    camera->SetViewTargetPosition(cS3DVector3{position[0], position[1], position[2]});
    sCamVelocity[0] = sCamVelocity[1] = sCamVelocity[2] = 0.0f;
    for (int i = 0; i < 3; ++i) sCamFollowLastPosition[i] = position[i];
    sCamFollowLastRotation = 0;
    UpdateCameraFollow(100.0f);
    sbCameraChanging = false;
}

void cSC4AutomataManager::UpdateCameraFollow(float deltaMilliseconds) {
    if (!spCamFollow || !g_host) return;
    if (spCamFollow->CheckAutomataFlag(0x8000000)) return;
    IAutomataCameraControl* camera = g_host->CameraControl();
    if (!camera) return;
    IAutomataViewUtilities* view = g_host->ViewUtilities();
    if (!view) return;
    sbCameraChanging = true;

    float position[3] = {0.0f, 0.0f, 0.0f};
    spCamFollow->GetPosition(position);
    float heading[3] = {0.0f, 0.0f, 0.0f};
    spCamFollow->GetOrientation(heading);
    float ahead[3] = {position[0] + heading[0], position[1] + heading[1], position[2] + heading[2]};

    float groundPosition[3] = {position[0], position[1], position[2]};
    float height = 0.0f;
    if (g_host->TerrainShadowEnabled()) {
        groundPosition[1] = g_host->TerrainHeight(position[0], position[2]);
        height = g_host->TerrainShadowHeight(position[0], position[2]);
    } else {
        groundPosition[1] = g_host->TerrainShadowHeight(position[0], position[2]);
        height = groundPosition[1];
    }
    if (height > position[1]) position[1] = height;

    float viewPosition[3] = {position[0], position[1], position[2]};
    float viewAhead[3] = {ahead[0], ahead[1], ahead[2]};
    float viewGround[3] = {groundPosition[0], groundPosition[1], groundPosition[2]};
    view->ProjectPointToViewCoordinates(viewPosition);
    view->ProjectPointToViewCoordinates(viewAhead);
    view->ProjectPointToViewCoordinates(viewGround);

    float dirX = viewPosition[0] - viewAhead[0];
    float dirY = viewPosition[1] - viewAhead[1];
    const float length = 1.0f / std::sqrt(dirY * dirY + dirX * dirX);
    dirX *= length;
    dirY *= length;
    bool left = dirX < -0.25f, right = dirX > 0.25f;
    bool up = dirY < -0.25f, down = dirY > 0.25f;
    if (dirX <= 0.25f && dirX >= -0.25f) {
        right = (sCamFollowLastRotation & 8) != 0;
        left = (sCamFollowLastRotation & 2) != 0;
    }
    if (dirY <= 0.25f && dirY >= -0.25f) {
        up = (sCamFollowLastRotation & 1) != 0;
        down = (sCamFollowLastRotation & 4) != 0;
    }

    float seconds = deltaMilliseconds / 1000.0f;
    if (seconds == 0.0f) seconds = 1.0f;   // the original divides by it (a zero delta would be a division by zero)
    const float scale = 1.0f / seconds;
    sCamFollowLastRotation = 0;

    int viewportWidth = 0, viewportHeight = 0;
    camera->GetViewportSize(viewportWidth, viewportHeight);
    int zoom = camera->VirtualZoom();
    if (zoom < 0) zoom = 0;
    if (zoom > 4) zoom = 4;
    const float nose = kAutomataCameraNoseFraction[zoom];
    const int noseX = static_cast<int>(static_cast<float>(viewportWidth) * nose + 12582912.0f) - 0x4b400000;
    view->ProjectPointFromViewCoordinatesToViewportCoordinates(viewPosition);
    view->ProjectPointFromViewCoordinatesToViewportCoordinates(viewGround);

    if (right) {
        viewPosition[0] -= static_cast<float>(noseX);
        sCamFollowLastRotation |= 8;
    } else if (left) {
        viewPosition[0] += static_cast<float>(noseX);
        sCamFollowLastRotation |= 2;
    }
    const int noseY = static_cast<int>(static_cast<float>(viewportHeight) * nose + 12582912.0f) - 0x4b400000;
    if (up) {
        viewPosition[1] -= static_cast<float>(noseY);
        sCamFollowLastRotation |= 1;
    } else if (down) {
        viewPosition[1] += static_cast<float>(noseY);
        sCamFollowLastRotation |= 4;
    }

    const float deltaX = (position[0] - sCamFollowLastPosition[0]) * scale;
    const float deltaY = (position[1] - sCamFollowLastPosition[1]) * scale;
    const float deltaZ = (position[2] - sCamFollowLastPosition[2]) * scale;
    view->UnProjectPointFromViewportCoordinatesToWorldCoordinates(viewPosition);

    float blend = seconds * 2.5f;
    if (blend > 1.0f) blend = 1.0f;
    if (blend < 0.0f) blend = 0.0f;
    float target[3] = {0.0f, 0.0f, 0.0f};
    camera->TargetVelocity(target);
    const float inverse = 1.0f - blend;
    sCamVelocity[0] = sCamVelocity[0] * inverse + (deltaX + (viewPosition[0] - target[0]) * 0.825f) * blend;
    sCamVelocity[1] = sCamVelocity[1] * inverse + (deltaY + (viewPosition[1] - target[1]) * 0.825f) * blend;
    sCamVelocity[2] = sCamVelocity[2] * inverse + (deltaZ + (viewPosition[2] - target[2]) * 0.825f) * blend;
    for (int i = 0; i < 3; ++i) sCamFollowLastPosition[i] = position[i];
    const cS3DVector3 newTarget{target[0] + sCamVelocity[0] * seconds, target[1] + sCamVelocity[1] * seconds,
                                target[2] + sCamVelocity[2] * seconds};
    camera->SetViewTargetPosition(newTarget);
    camera->SetViewTargetVelocity(cS3DVector3{sCamVelocity[0], sCamVelocity[1], sCamVelocity[2]});
    sbCameraChanging = false;
}

// ---- automata groups / building counts --------------------------------------------------------------------------------------
bool cSC4AutomataManager::CanCreateGroup(std::uint32_t group) {
    return exemplarDataMap_.find(group) != exemplarDataMap_.end();
}

bool cSC4AutomataManager::TrackAutomataGroupInstances(std::uint32_t group, bool track) {
    if (!CanCreateGroup(group)) return true;
    if (track) {
        if (trackedGroups_.find(group) != trackedGroups_.end()) return false;
        auto& set = trackedGroups_[group];
        for (IAutomataAutomaton* automaton : automata_) {
            if (!automaton) continue;
            IAutomataOccupant* occupant = automaton->Occupant();
            if (!occupant || !occupant->IsOccupantGroup(group)) continue;
            if (set.insert(automaton).second) automaton->AddRef();
        }
        return true;
    }
    auto it = trackedGroups_.find(group);
    if (it == trackedGroups_.end()) return false;
    ReleaseAutomatonSet(it->second);
    trackedGroups_.erase(it);
    return true;
}

bool cSC4AutomataManager::GetInstancesOfGroup(std::uint32_t group, std::unordered_set<IAutomataAutomaton*>& out) {
    auto it = trackedGroups_.find(group);
    if (it != trackedGroups_.end()) {
        for (IAutomataAutomaton* automaton : it->second) {
            if (!automaton || automaton->CheckAutomataFlag(1)) continue;
            IAutomataOccupant* occupant = automaton->Occupant();
            if (!occupant || !occupant->IsOccupantGroup(group)) continue;
            if (out.insert(automaton).second) automaton->AddRef();
        }
    } else {
        for (IAutomataAutomaton* automaton : automata_) {
            if (!automaton || automaton->CheckAutomataFlag(1)) continue;
            IAutomataOccupant* occupant = automaton->Occupant();
            if (!occupant || !occupant->IsOccupantGroup(group)) continue;
            if (out.insert(automaton).second) automaton->AddRef();
        }
    }
    return !out.empty();
}

void cSC4AutomataManager::ReleaseAutomatonSet(std::unordered_set<IAutomataAutomaton*>& set) {
    for (IAutomataAutomaton* automaton : set)
        if (automaton) automaton->Release();
    set.clear();
}

void cSC4AutomataManager::HandleOccupantGroupChanged(IAutomataAutomaton* automaton) {
    if (!automaton) return;
    IAutomataOccupant* occupant = automaton->Occupant();
    if (!occupant) return;
    for (auto& entry : trackedGroups_) {
        if (!occupant->IsOccupantGroup(entry.first)) continue;
        if (entry.second.insert(automaton).second) automaton->AddRef();
    }
}

void cSC4AutomataManager::SeedTrackedBuildingsMap() {
    if (!scriptGroups_) return;
    for (std::uint32_t group : scriptGroups_->GetTrackBuildingGroups())
        if (trackedBuildings_.find(group) == trackedBuildings_.end()) trackedBuildings_[group] = 0;
}

std::uint32_t cSC4AutomataManager::GetSourceBuildingCount(std::uint32_t group) {
    if ((group | 4u) == 0x1406u) {
        if (g_host && g_host->LandfillDeveloperPresent())
            return static_cast<std::uint32_t>(g_host->LandfillSourceBuildingCount());
        return 0;
    }
    auto it = trackedBuildings_.find(group);
    return it == trackedBuildings_.end() ? 0 : it->second;
}

std::uint32_t cSC4AutomataManager::GetSourceBuildingCountForAutomataGroup(std::uint32_t group) {
    if (!scriptGroups_) return 0;
    IAutomataGroupTemplate* tmpl = scriptGroups_->GetAutomataGroup(group);
    while (tmpl && tmpl->SourceBuilding() == 0) tmpl = tmpl->Parent();
    return GetSourceBuildingCount(tmpl ? tmpl->SourceBuilding() : 0);
}

// ---- occupant / building handlers --------------------------------------------------------------------------------------------
bool cSC4AutomataManager::HandleNewBuilding(IAutomataBuildingOccupant* building) {
    if (!building) return true;
    IAutomataOccupant* occupant = building->AsOccupant();
    if (!occupant) return true;
    std::set<std::uint32_t> groups;
    if (occupant->GetOccupantGroups(groups)) {
        for (std::uint32_t group : groups) {
            auto it = trackedBuildings_.find(group);
            if (it != trackedBuildings_.end()) ++it->second;
        }
    }
    return true;
}

bool cSC4AutomataManager::HandleInsertOccupant(IAutomataOccupant* occupant) {
    if (!occupant || occupant->GetType() != kBuildingOccupantType) return true;
    void* raw = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return true;
    auto* building = static_cast<IAutomataBuildingOccupant*>(raw);
    if (building->GetCompletionPercent() >= 100.0f) HandleNewBuilding(building);
    if (g_host) g_host->ReleaseObject(static_cast<cIGZUnknown*>(raw));
    return true;
}

bool cSC4AutomataManager::HandleRemoveOccupant(IAutomataOccupant* occupant) {
    if (!occupant) return true;
    if (occupant->GetType() == kNetworkOccupantType) {
        void* raw = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4NetworkOccupant, &raw) && raw) {
            HandleRemoveNetworkOccupant(static_cast<IAutomataNetworkOccupant*>(raw));
            if (g_host) g_host->ReleaseObject(static_cast<cIGZUnknown*>(raw));
        }
    }
    for (auto& entry : trackedBuildings_)
        if (entry.second != 0 && occupant->IsOccupantGroup(entry.first)) --entry.second;
    return true;
}

bool cSC4AutomataManager::HandleRemoveController(void* controller) {
    for (IAutomataAutomaton* automaton : automata_)
        if (automaton) automaton->HandleRemoveController(controller);
    return true;
}

void cSC4AutomataManager::HandlePostCityInit() {
    if (postCityInitPending_) {
        for (auto it = automata_.begin(); it != automata_.end();) {
            IAutomataAutomaton* automaton = *it;
            ++it;
            if (!automaton->CheckAutomataFlag(0x8000)) {
                automaton->Init();
                continue;
            }
            automata_.erase(std::find(automata_.begin(), automata_.end(), automaton));
            RemoveAutomatonInternal(automaton);
            automaton->Shutdown();
            automaton->Release();
        }
        if (recountBuildings_) {
            std::vector<std::uint32_t> groups;
            groups.reserve(trackedBuildings_.size());
            for (auto& entry : trackedBuildings_) {
                groups.push_back(entry.first);
                entry.second = 0;
            }
            cIGZUnknown* filter = g_host ? g_host->CreateOccupantGroupFilter(groups) : nullptr;
            std::list<IAutomataOccupant*> occupants;
            if (g_host && g_host->EnumerateOccupants(filter, occupants)) {
                for (IAutomataOccupant* occupant : occupants) {
                    if (!occupant) continue;
                    std::set<std::uint32_t> occupantGroups;
                    if (!occupant->GetOccupantGroups(occupantGroups)) continue;
                    for (std::uint32_t group : occupantGroups) {
                        auto found = trackedBuildings_.find(group);
                        if (found != trackedBuildings_.end()) ++found->second;
                    }
                }
                g_host->ReleaseOccupantList(occupants);
            }
            if (filter) filter->Release();
        }
    }
    UpdateZoomAndRotation();
}

// ---- player drive ----------------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::IsDrivingMode() { return g_host && g_host->CurrentInputControlIsPlayerDrive(); }

void cSC4AutomataManager::EnterPlayerDriveView(IAutomataAutomaton* automaton, cIGZMessageTarget2* callback) {
    if (spPlayerDriveView) return;
    if (!g_host) return;
    IAutomataPlayerDriveControl* control = g_host->CreatePlayerDriveControl();
    if (!control) return;
    control->AddRef();
    g_host->SetPlayerDriveControl(control);
    spPlayerDriveView = control;
    g_host->AddViewInputControl(control, 2);
    if (!automaton) {
        control->EnterChoosingMode();
    } else {
        automaton->SetPriority(0xff);
        g_host->SetVehicleManagerSpeed(sTuning.f21a78);
        control->EnterDrivingMode(automaton, true);   // the second argument is not recovered (inferred true)
    }
    if (callback) control->AddCallbackTarget(callback);
}

void cSC4AutomataManager::ExitPlayerDriveView() {
    if (g_host) {
        // The original passes the address of sTuningParams here, i.e. its first four bytes read as a float (quirk kept).
        float speed = 0.0f;
        std::memcpy(&speed, &sTuning.params.f000, sizeof speed);
        g_host->SetVehicleManagerSpeed(speed);
        g_host->SetPlayerDriveControl(nullptr);
    }
    IAutomataPlayerDriveControl* control = spPlayerDriveView;
    spPlayerDriveView = nullptr;
    if (control) control->Release();
}

// ---- notifications / messages ----------------------------------------------------------------------------------------------
void cSC4AutomataManager::PostNotificationMessage(std::uint32_t type, IAutomataAutomaton* automaton, std::uint32_t data1,
                                                  std::uint32_t data2) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetVoid1(automaton);
    message->SetData2(data1);
    message->SetData3(data2);
    if (g_host) g_host->PostMessage(message, false);
    message->Release();
}

bool cSC4AutomataManager::DoMessage(cGZMessage& message) {
    if (message.type != kMessageCheatCode) return false;
    const std::uint64_t cheat = message.param1;
    auto* text = reinterpret_cast<cIGZString*>(static_cast<std::uintptr_t>(message.param2));
    const char* chars = text ? text->Data() : "";
    if (cheat == kCheatPlayerDrive) {
        cRZCmdLine command(chars);
        if (command.argc() > 1) {
            const cRZString& argument = command[1];
            if (argument.Strlen() == 5 && std::strcmp(argument.Data(), "reset") == 0) {
                if (g_host) {
                    if (IAutomataGroupBudget* budget = g_host->GroupBudget()) budget->ClearTrackedAutomata();
                }
                return true;
            }
        }
        EnterPlayerDriveView(nullptr, nullptr);
        return true;
    }
    if (cheat == kCheatGhostModels) {
        cRZCmdLine command(chars);
        bool enabled = true;
        if (command.argc() > 1) {
            const cRZString& argument = command[1];
            if (argument.Strlen() == 3 && std::strcmp(argument.Data(), "off") == 0) enabled = false;
        }
        if (g_host) g_host->SetGhostModels(enabled);
        return true;
    }
    if (cheat == kCheatHideAutomataPaths) {
        if (spRoadPathView) {
            if (g_host) g_host->RemoveRendererLayer(spRoadPathView);
            spRoadPathView->Shutdown();
            spRoadPathView->Release();
            spRoadPathView = nullptr;
        }
        return true;
    }
    if (cheat == kCheatDrawAutomataPaths) {
        cRZCmdLine command(chars);
        if (!spRoadPathView && g_host) {
            spRoadPathView = g_host->CreatePathView();
            if (spRoadPathView) {
                spRoadPathView->AddRef();
                spRoadPathView->Init();
                spRoadPathView->SetShouldIgnoreDepthBuffer(true);
                g_host->AddRendererLayer(spRoadPathView);
            }
        }
        int filter = -1;
        if (spRoadPathView && command.argc() > 1) filter = std::atoi(command[1].Data());
        if (spRoadPathView) spRoadPathView->SetPathFilter(filter);
        return true;
    }
    if (cheat == kCheatNoCsi) {
        if (g_host) {
            if (IAutomataDispatchView* view = g_host->DispatchView()) view->ShowAllIndicators(4, false);
        }
        return true;
    }
    return false;
}

bool cSC4AutomataManager::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    // Scroll start / end only toggle the flag.
    if (type == kMsgScrollStart) {
        scrollInProgress_ = true;
        return true;
    }
    if (type == kMsgScrollEnd) {
        scrollInProgress_ = false;
        return true;
    }
    if (type == kMsgCameraMoved) {
        HandleViewChanged();
        return true;
    }
    if (type == kMsgCameraTargetDone) {
        if (scrollInProgress_) return true;
        float viewPosition[3] = {0.0f, 0.0f, 0.0f};
        int zoom = 0, rotation = 0;
        if (g_host) {
            if (IAutomataViewUtilities* view = g_host->ViewUtilities()) {
                view->ViewPosition(viewPosition);
                view->GetZoomAndRotation(zoom, rotation);
            }
        }
        const float moved = SC4GeometryUtilities::LengthOfLineSquared(lastViewPosition_, viewPosition);
        const float granularity = zoom >= 0 && zoom < 8 ? kAutomataScrollGranularitySquared[zoom] : 0.0f;
        if (moved <= granularity) return true;
        for (int i = 0; i < 3; ++i) lastViewPosition_[i] = viewPosition[i];
        HandleViewChanged();
        return true;
    }
    if (type == kMsgAutomataEnabled) {
        void* standard = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) && standard) {
            auto* payload = static_cast<cIGZMessage2Standard*>(standard);
            loadedFlag_ = payload->GetData1() != 0 ? 1u : 0u;
            payload->Release();
        }
        return true;
    }
    if (type == kMsgPostCityInit) {
        HandlePostCityInit();
        return true;
    }
    if (type == kMsgCityShutdown) {
        DestroyAutomata();
        return true;
    }
    void* standardRaw = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &standardRaw) || !standardRaw) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(standardRaw);
    switch (type) {
        case kMsgTick:
            DoAnimationTick(static_cast<std::uint32_t>(standard->GetData2()), static_cast<float>(standard->GetData3()));
            break;
        case kMsgOccupantInserted:
        case kMsgOccupantRemoved: {
            auto* occupant = static_cast<IAutomataOccupant*>(standard->GetVoid1());
            if (occupant) {
                cISCPropertyHolder* holder = occupant->AsPropertyHolder();
                if (!holder || !holder->HasProperty(kPropertyNoAutomata)) {
                    if (type == kMsgOccupantInserted) HandleInsertOccupant(occupant);
                    else HandleRemoveOccupant(occupant);
                }
            }
            break;
        }
        case kMsgBuildingCompleted: {
            auto* building = static_cast<IAutomataBuildingOccupant*>(standard->GetVoid1());
            if (building && building->GetCompletionPercent() >= 100.0f) HandleNewBuilding(building);
            break;
        }
        case kMsgControllerRemoved:
            HandleRemoveController(standard->GetVoid1());
            break;
        case kMsgOccupantGroupAdded: {
            void* raw = standard->GetVoid1();
            if (raw) {
                void* automatonRaw = nullptr;
                auto* unknown = static_cast<cIGZUnknown*>(raw);
                if (unknown->QueryInterface(kGZIID_cISC4Automaton, &automatonRaw) && automatonRaw) {
                    HandleOccupantGroupChanged(static_cast<IAutomataAutomaton*>(automatonRaw));
                    static_cast<cIGZUnknown*>(automatonRaw)->Release();
                }
            }
            break;
        }
        case kMsgViewFilterChanged:
            HandleViewFilterChanged(static_cast<const IAutomataViewFilterInformation*>(standard->GetVoid2()),
                                    static_cast<const IAutomataViewFilterInformation*>(standard->GetVoid1()));
            break;
        case kMsgQueuePointsInvalid:
            if (standard->GetIGZUnknown() && standard->GetData1() != 0)
                HandleInsertNetworkOccupant(static_cast<IAutomataNetworkOccupant*>(standard->GetIGZUnknown()));
            break;
        case kMsgRenderPropertiesChanged:
            HandleRenderPropertiesChanged();
            break;
        default:
            break;
    }
    standard->Release();
    return true;
}

// ---- exemplars -------------------------------------------------------------------------------------------------------------
void cSC4AutomataManager::ClearExemplars() {
    for (tExemplarData& data : exemplarData_)
        if (data.exemplar) data.exemplar->Release();
    exemplarData_.clear();
    // The original leaves the map with stale indices here (ScanExemplars only truncates the vector); the port clears it so a
    // second scan cannot index past the vector.
    exemplarDataMap_.clear();
}

void cSC4AutomataManager::AddExemplarToGroupMap(const tExemplarData& data, std::size_t index) {
    for (std::uint8_t i = 0; i < data.groupCount; ++i) exemplarDataMap_[data.groupIds[i]].push_back(static_cast<std::uint32_t>(index));
}

void cSC4AutomataManager::ScanExemplars() {
    ClearExemplars();
    if (!g_host) return;
    std::vector<cGZPersistResourceKey> keys;
    if (!g_host->EnumerateResourceKeys(kAutomataTuningResourceType, keys)) return;
    for (const cGZPersistResourceKey& key : keys) EnumAutomatonExemplar(key, this);
}

void cSC4AutomataManager::EnumAutomatonExemplar(const cGZPersistResourceKey& key, void* context) {
    if (!context || !g_host) return;
    auto* self = static_cast<cSC4AutomataManager*>(context);
    cISCResExemplar* exemplar = nullptr;
    if (!g_host->GetExemplar(key, &exemplar) || !exemplar) return;
    cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
    std::uint32_t group = 0;
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, kAutomatonExemplarGroupProperty, group) &&
        group == self->exemplarGroup_) {
        tExemplarData data;
        data.exemplar = exemplar;
        exemplar->AddRef();
        self->exemplarData_.push_back(data);
        const std::size_t index = self->exemplarData_.size() - 1;
        self->InitExemplarData(self->exemplarData_.back(), exemplar);
        if (self->exemplarData_.back().groupCount != 0) self->AddExemplarToGroupMap(self->exemplarData_.back(), index);
    }
    exemplar->Release();
}

void cSC4AutomataManager::InitExemplarData(tExemplarData& data, cISCResExemplar* exemplar) {
    if (!exemplar) return;
    cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
    data.flag50 = false;
    data.crimeType = 0;
    data.modelKey = cGZPersistResourceKey{};
    data.name = cRZString();
    data.groupCount = 0;
    for (std::uint32_t& group : data.groupIds) group = 0;
    data.defaultGroup = defaultGroup_;
    if (holder) cSCPropertyHelp::GetPropertyValue(holder, 0x20, data.name);
    std::vector<std::uint32_t> groups;
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropertyOccupantGroups, groups)) {
        std::size_t count = 0;
        for (; count < 10 && count < groups.size(); ++count) data.groupIds[count] = groups[count];
        data.groupCount = static_cast<std::uint8_t>(count);
        // Add the ancestors of every listed group (the original walks each template's parent chain).
        for (std::size_t i = 0; i < groups.size(); ++i) {
            IAutomataGroupTemplate* tmpl = scriptGroups_ ? scriptGroups_->GetOccupantGroup(groups[i]) : nullptr;
            if (!tmpl) continue;
            for (IAutomataGroupTemplate* parent = tmpl->Parent(); parent; parent = parent->Parent()) {
                if (data.groupCount >= 10) break;
                const std::uint32_t id = parent->GroupId();
                bool present = false;
                for (std::uint8_t j = 0; j < data.groupCount; ++j)
                    if (data.groupIds[j] == id) present = true;
                if (!present) data.groupIds[data.groupCount++] = id;
            }
        }
        IAutomataGroupTemplate* top = nullptr;
        bool haveDefault = holder && cSCPropertyHelp::GetPropertyValue(holder, 0x2c23a3a1, data.defaultGroup);
        if (haveDefault) {
            top = scriptGroups_ ? scriptGroups_->GetOccupantGroup(data.defaultGroup) : nullptr;
        } else {
            for (std::uint8_t i = 0; i < data.groupCount; ++i) {
                IAutomataGroupTemplate* candidate = scriptGroups_ ? scriptGroups_->GetOccupantGroup(data.groupIds[i]) : nullptr;
                if (!candidate) continue;
                bool isAncestor = false;
                for (std::uint8_t j = 0; j < data.groupCount; ++j) {
                    if (j == i) continue;
                    IAutomataGroupTemplate* other = scriptGroups_ ? scriptGroups_->GetOccupantGroup(data.groupIds[j]) : nullptr;
                    if (other && other->IsOccupantGroup(candidate->GroupId())) {
                        isAncestor = true;
                        break;
                    }
                }
                if (isAncestor) continue;
                data.defaultGroup = candidate->GroupId();
                top = candidate;
            }
        }
        // The first source building on the template's parent chain is the one whose count is tracked.
        for (IAutomataGroupTemplate* tmpl = top; tmpl; tmpl = tmpl->Parent()) {
            const std::uint32_t source = tmpl->SourceBuilding();
            if (source == 0) continue;
            if (trackedBuildings_.find(source) == trackedBuildings_.end()) trackedBuildings_[source] = 0;
            break;
        }
    }
    if (holder) {
        ReadByteMask(holder, 0x696e88db, data.networkMask0, false);
        ReadByteMask(holder, 0x296e88e9, data.networkMask1, false);
        ReadByteMask(holder, 0x496e88f1, data.networkMask2, false);
        ReadByteMask(holder, 0xc973c9aa, data.layerMask, true);
        std::vector<std::uint32_t> key;
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x9c80416, key) && key.size() == 3) {
            data.modelKey.type = key[0];
            data.modelKey.group = key[1];
            data.modelKey.instance = key[2];
        }
        cSCPropertyHelp::GetPropertyValue(holder, 0xaa5a9633, data.flag50);
        cSCPropertyHelp::GetPropertyValue(holder, 0x2a62d9f3, data.crimeType);
    }
}

tExemplarData* cSC4AutomataManager::FindExemplarFromModelKey(const cGZPersistResourceKey& key) {
    for (tExemplarData& data : exemplarData_) {
        if (data.modelKey.type == key.type && data.modelKey.group == key.group && data.modelKey.instance == key.instance)
            return &data;
    }
    return nullptr;
}

tExemplarData* cSC4AutomataManager::ChooseRandomExemplar(std::uint32_t group, PopulateLayer layer, int cellX, int cellZ) {
    if (group == 0) group = defaultGroup_;
    auto found = exemplarDataMap_.find(group);
    if (found == exemplarDataMap_.end()) return nullptr;
    std::vector<std::uint32_t> candidates = found->second;
    if (cellX >= -1 && cellZ >= -1 && g_host) {
        std::uint32_t masks[3] = {0, 0, 0};
        if (g_host->LotCellMasks(cellX, cellZ, masks) && masks[0] && masks[1] && masks[2]) {
            std::vector<std::uint32_t> filtered;
            for (std::uint32_t index : candidates) {
                if (index >= exemplarData_.size()) continue;
                const tExemplarData& data = exemplarData_[index];
                if ((data.networkMask2 & masks[2]) && (data.networkMask0 & masks[0]) &&
                    (data.networkMask1 & masks[1]))
                    filtered.push_back(index);
            }
            candidates.swap(filtered);
        }
    }
    if (layer != PopulateLayer::kLayerNone) {
        const std::uint32_t bit = 1u << (static_cast<std::uint32_t>(layer) & 31);
        std::vector<std::uint32_t> filtered;
        for (std::uint32_t index : candidates) {
            if (index >= exemplarData_.size()) continue;
            if (exemplarData_[index].layerMask & bit) filtered.push_back(index);
        }
        candidates.swap(filtered);
    }
    if (candidates.empty()) return nullptr;
    const std::uint32_t pick = random_.RandomUint32Uniform(static_cast<std::uint32_t>(candidates.size()));
    const std::uint32_t index = candidates[pick];
    if (index >= exemplarData_.size()) return nullptr;
    return &exemplarData_[index];
}

tExemplarData* cSC4AutomataManager::ChooseRandomExemplarByFilter(std::uint32_t group, std::uint32_t layerMask,
                                                                std::uint32_t mask0, std::uint32_t mask1,
                                                                std::uint32_t mask2) {
    if (group == 0) group = defaultGroup_;
    auto found = exemplarDataMap_.find(group);
    if (found == exemplarDataMap_.end()) return nullptr;
    std::vector<std::uint32_t> candidates;
    for (std::uint32_t index : found->second) {
        if (index >= exemplarData_.size()) continue;
        const tExemplarData& data = exemplarData_[index];
        if ((data.layerMask & layerMask) && (data.networkMask2 & mask2) && (data.networkMask0 & mask0) &&
            (data.networkMask1 & mask1))
            candidates.push_back(index);
    }
    if (candidates.empty()) return nullptr;
    const std::uint32_t pick = random_.RandomUint32Uniform(static_cast<std::uint32_t>(candidates.size()));
    return &exemplarData_[candidates[pick]];
}

tExemplarData* cSC4AutomataManager::ChooseExemplarByCrimeType(std::uint32_t group, std::uint32_t crimeType,
                                                             PopulateLayer layer) {
    if (group == 0) group = defaultGroup_;
    auto found = exemplarDataMap_.find(group);
    if (found == exemplarDataMap_.end()) return nullptr;
    const std::uint32_t bit = 1u << (static_cast<std::uint32_t>(layer) & 31);
    std::vector<std::uint32_t> candidates;
    for (std::uint32_t index : found->second) {
        if (index >= exemplarData_.size()) continue;
        const tExemplarData& data = exemplarData_[index];
        if ((data.layerMask & bit) && (crimeType == 0 || data.crimeType == crimeType)) candidates.push_back(index);
    }
    if (candidates.empty()) return nullptr;
    const std::uint32_t pick = random_.RandomUint32Uniform(static_cast<std::uint32_t>(candidates.size()));
    return &exemplarData_[candidates[pick]];
}

// ---- persistence -----------------------------------------------------------------------------------------------------------
bool cSC4AutomataManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(kWriteVersion);
    stream.SetSint32(static_cast<std::int32_t>(maxCount_));
    stream << (active_ ? true : false);
    stream << (loadedFlag_ != 0);
    stream.SetUint32(static_cast<std::uint32_t>(automata_.size()));
    for (IAutomataAutomaton* automaton : automata_)
        SC4WriteGZSerializable(stream, static_cast<const cIGZUnknown*>(automaton));
    stream.SetUint32(static_cast<std::uint32_t>(trackedBuildings_.size()));
    for (const auto& entry : trackedBuildings_) {
        stream.SetUint32(entry.first);
        stream.SetUint32(entry.second);
    }
    return true;
}

bool cSC4AutomataManager::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return ReadInternal(stream, version);
}

bool cSC4AutomataManager::ReadInternal(cIGZIStream& stream, std::uint16_t version) {
    if (version >= kMaxReadVersion) return false;
    postCityInitPending_ = true;
    std::int32_t maxCount = 0;
    stream.GetSint32(maxCount);
    maxCount_ = static_cast<std::uint32_t>(maxCount);
    bool active = false, loaded = false;
    stream >> active;
    stream >> loaded;
    active_ = active;
    loadedFlag_ = loaded ? 1u : 0u;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    automata_.clear();
    std::uint32_t droppedOnLoad = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kGZIID_cISC4Automaton, &raw, false);
        if (!raw) continue;
        // PORT GAP: the object answers 0xa9b40f05 as the recovered cSC4Automaton, which does not implement this manager's
        // IAutomataAutomaton (the two halves were ported apart). Kept in the list, every call through it hit the wrong
        // vtable (leaving a city crashed in DestroyAutomata). The saved automata are dropped until an adapter exists:
        // they are the cars and pedestrians on screen, and the simulation spawns new ones.
        static_cast<cIGZUnknown*>(raw)->Release();
        ++droppedOnLoad;
    }
    if (droppedOnLoad != 0) std::fprintf(stderr, "automata: %u saved automata dropped (no IAutomataAutomaton adapter yet)\n", droppedOnLoad);
    if (version < 5) {
        recountBuildings_ = true;
    } else {
        std::uint32_t mapSize = 0;
        stream.GetUint32(mapSize);
        trackedBuildings_.clear();
        for (std::uint32_t i = 0; i < mapSize; ++i) {
            std::uint32_t group = 0, value = 0;
            stream.GetUint32(group);
            stream.GetSint32(reinterpret_cast<std::int32_t&>(value));
            trackedBuildings_[group] = value;
        }
        recountBuildings_ = version < 6;
    }
    return true;
}

}  // namespace sc4
