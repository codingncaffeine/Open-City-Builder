// cSC4VehicleManager — see VehicleManager.h for the layout, the messages, the seams and the quirks. Every function here is
// a hand port of the exported decompile; the part that still depends on an unrecovered neighbor (the funds sweep) is listed
// under PARTIAL in notes/ledger_pending/cPedestrian-vehicle.txt.
#include "sc4/VehicleManager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

// The float->int "round to nearest" idiom of the original (0x1000da8b6): the magic-number add extracts the rounded
// mantissa, the bias subtract recovers the integer part (no -0.5 bias, unlike the base's floor idiom).
int RoundToInt(float value) {
    const float biased = value + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof bits);
    return static_cast<int>(bits) - 0x4b400000;
}

}  // namespace

// ---- construction / destruction ----------------------------------------------------------------------------------------
// 0x1000d9c40: the traffic manager constructor, then the four vtables (set by C++), the null marked-cell view, the three
// exemplar/group defaults, the default path/network types (1/1), the ten populate layers (1,2,3,5,4,7,6,8,9,10) and the
// crime-layer multiplier (0.2f).
cSC4VehicleManager::cSC4VehicleManager(IVehicleManagerHost* host)
    : cSC4TrafficAutomataManager(host), vehicleHost_(host) {
    SetExemplarGroup(kVehicleExemplarGroup);
    SetDefaultGroup(kVehicleDefaultGroup);
    SetFallbackGroup(kVehicleFallbackGroup);
    SetDefaultTypes(1, 1);
    for (std::uint32_t layer : {1u, 2u, 3u, 5u, 4u, 7u, 6u, 8u, 9u, 10u}) AddLayer(layer);
    SetCrimeLayerMultiplier(0.2f);
    markedCellView_ = nullptr;
}

// 0x1000e2498: the traffic manager destructor.
cSC4VehicleManager::~cSC4VehicleManager() = default;

// ---- COM -------------------------------------------------------------------------------------------------------------------
// 0x1000dac68: 0xa98f9d3b answers this object; everything else goes to the base's four-view QueryInterface.
bool cSC4VehicleManager::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZCLSID_cSC4VehicleManager) {
        *out = static_cast<cSC4VehicleManager*>(this);
        AddRef();
        return true;
    }
    return cSC4AutomataManager::QueryInterface(iid, out);
}

// ---- lifecycle -------------------------------------------------------------------------------------------------------------
// 0x1000d9f76: the base Init, then register the seven vehicle messages; then re-read the tuning and the render properties.
bool cSC4VehicleManager::Init() {
    if (Initialized()) return true;
    if (!cSC4TrafficAutomataManager::Init()) return false;
    if (IVehicleManagerHost* host = VehicleHostOrGlobal()) {
        if (cIGZMessageServer2* server = host->MessageServer()) {
            for (std::uint32_t message :
                 {kVehicleMessageHeadlights, kVehicleMessageCameraTarget1, kVehicleMessageCameraTarget2,
                  kVehicleMessageHeadlightsOff, kVehicleMessageHeadlightsOn, kVehicleMessageHourPassed,
                  kVehicleMessageFundsChanged})
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
        }
    }
    HandleTuningParametersChanged();
    HandleRenderPropertiesChanged();
    return true;
}

// 0x1000da046: free the marked-cell view (never created by the recovered code), unregister the messages, then the base
// Shutdown.
bool cSC4VehicleManager::Shutdown() {
    if (Initialized()) {
        if (markedCellView_) {
            markedCellView_->Release();
            markedCellView_ = nullptr;
        }
        if (IVehicleManagerHost* host = VehicleHostOrGlobal()) {
            if (cIGZMessageServer2* server = host->MessageServer()) {
                for (std::uint32_t message :
                     {kVehicleMessageHeadlights, kVehicleMessageCameraTarget1, kVehicleMessageCameraTarget2,
                      kVehicleMessageHeadlightsOff, kVehicleMessageHeadlightsOn, kVehicleMessageHourPassed,
                      kVehicleMessageFundsChanged})
                    server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), message);
            }
        }
    }
    return cSC4TrafficAutomataManager::Shutdown();
}

// 0x1000da126: the legacy message view forwards to the base.
bool cSC4VehicleManager::DoMessage(cGZMessage& message) { return cSC4AutomataManager::DoMessage(message); }

// 0x1000da13e: the camera target clears the follow when the target left the drive state; the headlights messages sweep
// the fleet; the funds message runs the funds sweep; the hour message turns the lights on/off for day/night; the tuning
// message re-reads the tuning and notifies every kind manager. The base then runs.
bool cSC4VehicleManager::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    IVehicleManagerHost* host = VehicleHostOrGlobal();
    const std::uint32_t type = message->GetType();
    if (type == kVehicleMessageCameraTarget1 || type == kVehicleMessageCameraTarget2) {
        // 0x1000da166: state 0x10 is the player-drive / driving state.
        if (CameraFollowTarget() && !CameraChanging() && CameraFollowTarget()->FindState(0x10) == nullptr)
            SetCameraFollow(nullptr);
        return true;
    }
    if (type == kVehicleMessageHeadlights) {
        void* raw = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) && raw) {
            auto* payload = static_cast<cIGZMessage2Standard*>(raw);
            const std::uint64_t data1 = payload->GetData1();
            payload->GetData2();   // read and dropped
            payload->Release();
            if (data1 == kVehicleTuningChangedToken) {
                ReadTuningParameters();
                if (host) host->NotifyKindManagersTuningChanged();
            }
        }
        return true;
    }
    if (type == kVehicleMessageHeadlightsOff) {
        if (host)
            for (IAutomataAutomaton* automaton : Automata()) host->ShowVehicleHeadlights(automaton, false, true);
        return true;
    }
    if (type == kVehicleMessageHeadlightsOn) {
        if (host)
            for (IAutomataAutomaton* automaton : Automata()) host->ShowVehicleHeadlights(automaton, true, true);
        return true;
    }
    if (type == kVehicleMessageFundsChanged) {
        HandleFundsChanged();
        return true;
    }
    if (type == kVehicleMessageHourPassed) {
        void* raw = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) && raw) {
            auto* payload = static_cast<cIGZMessage2Standard*>(raw);
            const std::uint64_t data2 = payload->GetData2();
            payload->Release();
            const int timeOfDay = host ? host->TimeOfDay() : 0;
            if (data2 == 1 && timeOfDay == 1) {
                if (host)
                    for (IAutomataAutomaton* automaton : Automata()) host->ShowVehicleHeadlights(automaton, false, false);
            } else if (data2 == 0 && timeOfDay == 2) {
                if (host)
                    for (IAutomataAutomaton* automaton : Automata()) host->ShowVehicleHeadlights(automaton, true, false);
            }
        }
        return true;
    }
    return cSC4AutomataManager::DoMessage(message);
}

// 0x1000da428 PARTIAL: the building-occupant sweep through the city's two occupant lists and the script system's
// name-based group lookup is unrecovered; the port delegates to the host seam.
void cSC4VehicleManager::HandleFundsChanged() {
    if (IVehicleManagerHost* host = VehicleHostOrGlobal()) host->HandleFundsChangedSweep();
}

// 0x1000da884: the max count is the tuning params' first field (300 by default); the min zoom is the byte at 0x100b219d4.
bool cSC4VehicleManager::HandleTuningParametersChanged() {
    SetMaxCount(static_cast<int>(AutomataTuning().params.f000));
    SetMinZoom(static_cast<int>(AutomataTuning().f219d4));
    return true;
}

// 0x1000da8b6: re-derive the max count from the render properties density factor, re-read the min zoom, then re-apply the
// view zoom with a -1 rotation.
bool cSC4VehicleManager::HandleRenderPropertiesChanged() {
    if (IVehicleManagerHost* host = VehicleHostOrGlobal()) {
        if (host->HasRenderProperties()) {
            const float value = host->AutomataDensityFactor() * static_cast<float>(AutomataTuning().params.f000);
            SetMaxCount(RoundToInt(value));
            SetMinZoom(host->AutomataMinZoom() - 1);
            int zoom = 0, rotation = 0;
            if (IAutomataViewUtilities* view = host->ViewUtilities()) view->GetZoomAndRotation(zoom, rotation);
            HandleViewZoomChanged(zoom, 0xffffffff);
        }
    }
    return true;
}

// 0x1000da95c: layers 1 and 7 use the tuning table's first curve (0x100b218b0); everything else is the base curve.
cSC4ResponseCurve& cSC4VehicleManager::GetEdgeDensityCurve(PopulateLayer layer) {
    if (layer != PopulateLayer::kLayer1 && layer != PopulateLayer::kLayer7)
        return cSC4TrafficAutomataManager::GetEdgeDensityCurve(layer);
    return AutomataTuning().f218b0;
}

// 0x1000da97e: layers 1 and 7 use the tuning table's first float (0x100b218d0); everything else is the base value.
float cSC4VehicleManager::GetMaxAutosPerSecond(PopulateLayer layer) {
    if (layer != PopulateLayer::kLayer1 && layer != PopulateLayer::kLayer7)
        return cSC4TrafficAutomataManager::GetMaxAutosPerSecond(layer);
    return AutomataTuning().f218d0;
}

// 0x1000da9a0 (i386 0x000a039a): a finished building asks for its response automata. The building's property 0xe9aee3da lists
// automata groups and 0xe9aee3db their counts (1 where the list is shorter or missing); each group is queued as a forced response
// (DispatchResponseAutomata: spawned on a road next to the building, path type 5, flags 2) while the manager is below its
// automaton limit. A building without such a list that is residential (profile purpose 1) gets, with the tuning probability of
// property 0xc9c18eae, one automaton of group 0x4206 that drives in from a road 8 to 10 cells away. Then the base handling.
bool cSC4VehicleManager::HandleNewBuilding(IAutomataBuildingOccupant* building) {
    IAutomataOccupant* occupant = building ? building->AsOccupant() : nullptr;
    if (occupant) {
        ITrafficAutomataManagerHost* host = TrafficHostOrGlobal();
        cISC4Occupant* cityOccupant = host ? host->AsCityOccupant(occupant) : nullptr;
        cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        bool queued = false;
        std::vector<std::uint32_t> groups;
        if (holder && cSCPropertyHelp::GetPropertyValue(holder, kResponseGroupsProperty, groups)) {
            std::vector<std::uint8_t> counts;
            cSCPropertyHelp::GetPropertyValue(holder, kResponseCountsProperty, counts);
            for (std::size_t i = 0; i < groups.size() && static_cast<std::uint32_t>(GetAutomataCount()) < MaxCount(); ++i) {
                DispatchResponseAutomata(groups[i], cityOccupant, i < counts.size() ? counts[i] : 1, true, 5, 2);
                queued = true;
            }
        }
        static const bool trace = std::getenv("SC4_DISPATCHDEBUG") != nullptr;
        if (trace)
            std::fprintf(stderr, "response: new building, purpose %u, %zu response groups, automata %d of %u\n", building->GetProfilePurpose(),
                         groups.size(), GetAutomataCount(), static_cast<unsigned>(MaxCount()));
        if (!queued && building->GetProfilePurpose() == 1 && static_cast<std::uint32_t>(GetAutomataCount()) < MaxCount() &&
            static_cast<float>(Random().RandomDoubleUniform()) <= AutomataTuning().f21a00)
            DispatchResponseAutomata(kNewResidenceResponseGroup, cityOccupant, 1, false, 8, 2);
    }
    return cSC4TrafficAutomataManager::HandleNewBuilding(building);
}

// 0x1000dab6e: a null automaton is a new cSC4Vehicle (when the count limit allows it), then the base adds it.
bool cSC4VehicleManager::AddAutomaton(IAutomataAutomaton* automaton) {
    if (!automaton) {
        if (MaxCount() <= static_cast<std::uint32_t>(Automata().size())) return false;
        IVehicleManagerHost* host = VehicleHostOrGlobal();
        if (!host) return false;
        automaton = host->CreateVehicleAutomaton();
        if (!automaton) return false;
    }
    return cSC4AutomataManager::AddAutomaton(automaton);
}

// 0x1000dac3a: the traffic manager tick, then the camera-follow update.
void cSC4VehicleManager::DoAnimationTick(std::uint32_t data, float deltaMilliseconds) {
    cSC4TrafficAutomataManager::DoAnimationTick(data, deltaMilliseconds);
    UpdateCameraFollow(deltaMilliseconds);
}

// ---- host -----------------------------------------------------------------------------------------------------------------
IVehicleManagerHost* cSC4VehicleManager::VehicleHostOrGlobal() const {
    if (vehicleHost_) return vehicleHost_;
    return dynamic_cast<IVehicleManagerHost*>(TrafficHostOrGlobal());
}

}  // namespace sc4
