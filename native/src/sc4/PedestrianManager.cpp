// cSC4PedestrianManager — see PedestrianManager.h for the layout, the messages, the seams and the quirks. Every function
// here is a hand port of the exported decompile; the building property arrays are the only PARTIAL.
#include "sc4/PedestrianManager.h"

#include <cmath>
#include <cstring>

namespace sc4 {

namespace {

int RoundToInt(float value) {
    const float biased = value + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof bits);
    return static_cast<int>(bits) - 0x4b400000;
}

}  // namespace

// ---- construction / destruction ----------------------------------------------------------------------------------------
// 0x1000f0cc0: the traffic manager constructor, then the three exemplar/group defaults, the default path/network types
// (2/0), the three populate layers (1,7,8) and the crime-layer multiplier (1.0f).
cSC4PedestrianManager::cSC4PedestrianManager(IPedestrianManagerHost* host)
    : cSC4TrafficAutomataManager(host), pedestrianHost_(host) {
    SetExemplarGroup(kPedestrianExemplarGroup);
    SetDefaultGroup(kPedestrianDefaultGroup);
    SetFallbackGroup(kPedestrianFallbackGroup);
    SetDefaultTypes(2, 0);
    for (std::uint32_t layer : {1u, 7u, 8u}) AddLayer(layer);
    SetCrimeLayerMultiplier(1.0f);
}

cSC4PedestrianManager::~cSC4PedestrianManager() = default;

// ---- COM -------------------------------------------------------------------------------------------------------------------
// 0x1000f158c: 0xc98f9d17 answers this object; everything else goes to the base's four-view QueryInterface.
bool cSC4PedestrianManager::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZCLSID_cSC4PedestrianManager) {
        *out = static_cast<cSC4PedestrianManager*>(this);
        AddRef();
        return true;
    }
    return cSC4AutomataManager::QueryInterface(iid, out);
}

// ---- lifecycle -------------------------------------------------------------------------------------------------------------
// 0x1000f0e46: the base Init, register the crowd-reaction message, then re-read the tuning and render properties.
bool cSC4PedestrianManager::Init() {
    if (Initialized()) return true;
    if (!cSC4TrafficAutomataManager::Init()) return false;
    if (IPedestrianManagerHost* host = PedestrianHostOrGlobal()) {
        if (cIGZMessageServer2* server = host->MessageServer())
            server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kPedestrianMessageCrowdReaction);
    }
    HandleTuningParametersChanged();
    HandleRenderPropertiesChanged();
    return true;
}

// 0x1000f0e9c: unregister the crowd-reaction message, then the base Shutdown.
bool cSC4PedestrianManager::Shutdown() {
    if (Initialized()) {
        if (IPedestrianManagerHost* host = PedestrianHostOrGlobal()) {
            if (cIGZMessageServer2* server = host->MessageServer())
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kPedestrianMessageCrowdReaction);
        }
    }
    return cSC4TrafficAutomataManager::Shutdown();
}

// 0x1000f0eda: the cheat-code message is consumed (returning true without the base); the crowd-reaction message fans the
// event out; everything else goes to the base.
bool cSC4PedestrianManager::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (type == kMessageCheatCode) return true;
    if (type != kPedestrianMessageCrowdReaction) return cSC4AutomataManager::DoMessage(message);

    cISC4Occupant* occupant = nullptr;
    void* raw = nullptr;
    if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) && raw) {
        auto* payload = static_cast<cIGZMessage2Standard*>(raw);
        occupant = static_cast<cISC4Occupant*>(payload->GetVoid1());
        const std::uint32_t event = static_cast<std::uint32_t>(payload->GetData1());
        const std::uint32_t radius = static_cast<std::uint32_t>(payload->GetData2());
        payload->Release();
        HandleCrowdReactionEvent(occupant, event, radius);
    }
    return true;
}

// 0x1000f0fd2: for every pedestrian within `radius` of the occupant that does not carry flag 0x4000, run the crowd
// reaction; then post message 0xa9e3b6e3 with the occupant, the event and the count of reacting pedestrians.
bool cSC4PedestrianManager::HandleCrowdReactionEvent(cISC4Occupant* occupant, std::uint32_t event, std::uint32_t radius) {
    IPedestrianManagerHost* host = PedestrianHostOrGlobal();
    cS3DVector3 position{0.0f, 0.0f, 0.0f};
    if (occupant) occupant->GetPosition(&position.x);
    std::uint32_t count = 0;
    for (IAutomataAutomaton* automaton : Automata()) {
        float pedestrianPosition[3] = {0.0f, 0.0f, 0.0f};
        automaton->GetPosition(pedestrianPosition);
        const float dx = pedestrianPosition[0] - position.x;
        const float dy = pedestrianPosition[1] - position.y;
        const float dz = pedestrianPosition[2] - position.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < static_cast<float>(radius)) {
            if (!automaton->CheckAutomataFlag(0x4000)) {
                if (host && host->PedestrianCrowdReaction(automaton, position, event)) ++count;
            }
        }
    }
    if (host) host->PostCrowdReactionMessage(occupant, event, count);
    return true;
}

// 0x1000f1434: the max count is the u32 at 0x100b21894; the min zoom is the byte at 0x100b219d5.
bool cSC4PedestrianManager::HandleTuningParametersChanged() {
    SetMaxCount(static_cast<int>(AutomataTuning().f21894));
    SetMinZoom(static_cast<int>(AutomataTuning().f219d5));
    return true;
}

// 0x1000f1466: re-derive the max count from the render properties density factor, re-read the min zoom, then re-apply the
// view zoom with a -1 rotation.
bool cSC4PedestrianManager::HandleRenderPropertiesChanged() {
    if (IPedestrianManagerHost* host = PedestrianHostOrGlobal()) {
        if (host->HasRenderProperties()) {
            const float value = host->AutomataDensityFactor() * static_cast<float>(AutomataTuning().f21894);
            SetMaxCount(RoundToInt(value));
            SetMinZoom(host->AutomataMinZoom() - 1);
            int zoom = 0, rotation = 0;
            if (IAutomataViewUtilities* view = host->ViewUtilities()) view->GetZoomAndRotation(zoom, rotation);
            HandleViewZoomChanged(zoom, 0xffffffff);
        }
    }
    return true;
}

// 0x1000f150c: layers 1 and 7 use the tuning params' sixth curve (0x100b21980, +0x100); everything else is the base curve.
cSC4ResponseCurve& cSC4PedestrianManager::GetEdgeDensityCurve(PopulateLayer layer) {
    if (layer != PopulateLayer::kLayer1 && layer != PopulateLayer::kLayer7)
        return cSC4TrafficAutomataManager::GetEdgeDensityCurve(layer);
    return AutomataTuning().params.curve100;
}

// 0x1000f152e: layers 1 and 7 use the tuning table's float at 0x100b219a0; everything else is the base value.
float cSC4PedestrianManager::GetMaxAutosPerSecond(PopulateLayer layer) {
    if (layer != PopulateLayer::kLayer1 && layer != PopulateLayer::kLayer7)
        return cSC4TrafficAutomataManager::GetMaxAutosPerSecond(layer);
    return AutomataTuning().f219a0;
}

// i386 0x000b179a: more walkers where attractors pull: when the automata are loaded, commotion / 50 + 1; otherwise 1.
float cSC4PedestrianManager::GetCommotionMultiplier(ITrafficNetworkMapOccupant* occupant, PopulateLayer layer) {
    (void)layer;
    if (LoadedFlag() == 0) return 1.0f;
    ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
    if (!host) return 1.0f;
    return host->AttractionCommotion(occupant) / 50.0f + 1.0f;
}

// 0x1000f121c PARTIAL: the building property arrays (0xe9aee3dc / 0xe9aee3dd) that name the response groups and flags are
// unrecovered; only the base traffic handling runs.
bool cSC4PedestrianManager::HandleNewBuilding(IAutomataBuildingOccupant* building) {
    return cSC4TrafficAutomataManager::HandleNewBuilding(building);
}

// 0x1000f1368: a null automaton is a new cSC4Pedestrian (when the count limit allows it), then the base adds it.
bool cSC4PedestrianManager::AddAutomaton(IAutomataAutomaton* automaton) {
    if (!automaton) {
        if (MaxCount() <= static_cast<std::uint32_t>(Automata().size())) return false;
        IPedestrianManagerHost* host = PedestrianHostOrGlobal();
        if (!host) return false;
        automaton = host->CreatePedestrianAutomaton();
        if (!automaton) return false;
    }
    return cSC4AutomataManager::AddAutomaton(automaton);
}

// ---- host -----------------------------------------------------------------------------------------------------------------
IPedestrianManagerHost* cSC4PedestrianManager::PedestrianHostOrGlobal() const {
    if (pedestrianHost_) return pedestrianHost_;
    return dynamic_cast<IPedestrianManagerHost*>(TrafficHostOrGlobal());
}

}  // namespace sc4
