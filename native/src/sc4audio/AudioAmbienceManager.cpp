#include "sc4audio/AudioAmbienceManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "sc4/Lot.h"
#include "sc4/OccupantFilters2.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4audio/Listener.h"
#include "sc4audio/SoundManager.h"

namespace SC4Audio {
namespace {

IAmbienceManagerHost* g_host = nullptr;

// The two message types no other recovered header owns (the others come from Listener.h, sc4/Occupant.h,
// sc4/Lot.h and SoundManager.h).
constexpr std::uint32_t kMessageAutomaton = 0x9d92fc3;         // posted by an automaton's callback
constexpr std::uint32_t kMessageRailwayCrossing = 0x8a878385;  // posted by a railway crossing

// The 12 message types Init registers, in the original's order (0x1003e962b..0x1003e9700). Shutdown
// removes the same set in its own order (0x1003e9876..0x1003e9944).
constexpr std::uint32_t kRegisteredMessages[] = {
    kMessageCityInit,               kMessageCityShutdown,     kMessageListenerMoved,
    sc4::kMessageOccupantInserted,       sc4::kMessageOccupantRemoved,  sc4::kMessageOccupantRemovedFromManager,
    sc4::kMessageLotConditionChanged,    sc4::kMessageLotStateChanged,  kMessageAutomaton,
    kMessageReloadTuning,           kMessageRailwayCrossing,  kMessageFocusChange,
};
constexpr std::uint32_t kRemovedMessages[] = {
    kMessageFocusChange,            kMessageRailwayCrossing,  kMessageCityInit,
    kMessageCityShutdown,           kMessageListenerMoved,    sc4::kMessageOccupantInserted,
    sc4::kMessageOccupantRemoved,        sc4::kMessageOccupantRemovedFromManager,
    sc4::kMessageLotStateChanged,        sc4::kMessageLotConditionChanged,
    kMessageAutomaton,              kMessageReloadTuning,
};

// The properties read on the tuning exemplar (found or not).
constexpr float kDefaultZoomDistance = 200.0f;      // 0x1003ea090 writes 0x428c0000 five times
constexpr int kZoomDistanceCount = 5;

// The lot states whose occupants swap their two sounds (bit set of 0x61, 0x1003eb78d).
constexpr std::uint32_t kLotStateSoundSwapMask = 0x61;
// The fade-out length of the score-stop event (0x1003e9b10).
constexpr std::int32_t kScoreFadeOutMs = 3000;
// The automaton state the "start" signal looks for (0x1003ea869 passes 0x10).
constexpr std::uint32_t kAutomatonRunningState = 0x10;

bool PropertyBool(const sc4::cISCPropertyHolder* holder, std::uint32_t id, bool& out) {
    return holder && sc4::cSCPropertyHelp::GetPropertyValue(holder, id, out);
}
bool PropertyUint(const sc4::cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t& out) {
    return holder && sc4::cSCPropertyHelp::GetPropertyValue(holder, id, out);
}
bool PropertyFloats(const sc4::cISCPropertyHolder* holder, std::uint32_t id, std::vector<float>& out) {
    return holder && sc4::cSCPropertyHelp::GetPropertyValue(holder, id, out);
}

// cvttss2si in the original: a float position becomes a truncated int.
std::int32_t PositionToInt(float value) {
    return static_cast<std::int32_t>(value);
}
// The instance id of an occupant's sound: the occupant pointer truncated to 32 bits (0x1003ebee7 adds
// the sound id to it).
std::uint32_t OccupantInstanceId(sc4::cISC4Occupant* occupant, std::uint32_t soundId) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(occupant)) + soundId;
}
std::uint32_t OccupantInstanceId(sc4::cISC4Occupant* occupant) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(occupant));
}

// The cISC4Automaton view of an occupant (iid 0xa9b40f05); one reference, or null.
IAmbienceAutomaton* QueryAutomaton(cIGZUnknown* object) { return g_host ? g_host->AutomatonOfObject(object) : nullptr; }

}  // namespace

IAmbienceAutomaton* IAmbienceManagerHost::AutomatonOfObject(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4AutomatonView, &raw) || !raw) return nullptr;
    return static_cast<IAmbienceAutomaton*>(raw);
}
IAmbienceLot* IAmbienceManagerHost::LotOfObject(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4LotView, &raw) || !raw) return nullptr;
    return static_cast<IAmbienceLot*>(raw);
}

IAmbienceManagerHost* AmbienceManagerHost() { return g_host; }
void SetAmbienceManagerHost(IAmbienceManagerHost* host) { g_host = host; }

// ---------------------------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------------------------

// cSC4AudioAmbienceManager::cSC4AudioAmbienceManager() @ 0x1003e9050: four vtables, the service id
// 0xaa12f911, refcount 0 and the "active" flag already set (so the framework may tick it before Init).
cSC4AudioAmbienceManager::cSC4AudioAmbienceManager() = default;

// ~cSC4AudioAmbienceManager @ 0x1003e92b6: releases the industrial filter first, then the automatic
// destruction of the members frees the timers, the vectors and the two lists. The layer list's nodes and
// the alarm list's occupants are *not* released (quirk kept: see the header).
cSC4AudioAmbienceManager::~cSC4AudioAmbienceManager() {
    if (industrialFilter_) {
        industrialFilter_->Release();
        industrialFilter_ = nullptr;
    }
    if (tickFilter_) {
        tickFilter_->Release();
        tickFilter_ = nullptr;
    }
    if (occupantManager_) {
        occupantManager_->Release();
        occupantManager_ = nullptr;
    }
    if (soundSystem_) {
        soundSystem_->Release();
        soundSystem_ = nullptr;
    }
    if (scapeManager_) {
        scapeManager_->Release();
        scapeManager_ = nullptr;
    }
}

bool cSC4AudioAmbienceManager::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x1003eb33a
    // Quirk: iid 0x6a12f51c belongs to the object's +0x10 interface, but the original hands out the
    // primary pointer for it (and for the system-service id) as well.
    if (iid == 1 || iid == kGZIID_cISC4AudioAmbienceManager || iid == kGZIID_cIGZSystemService) {
        if (out) *out = static_cast<cIGZSystemService*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4AudioAmbienceManager::Release() {   // @ 0x1003eb2be
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return static_cast<std::uint32_t>(--refCount_);
}

// ---------------------------------------------------------------------------------------------
// framework service
// ---------------------------------------------------------------------------------------------

// Init @ 0x1003e94ec
bool cSC4AudioAmbienceManager::Init() {
    IAmbienceManagerHost* host = g_host;
    if (!host || !host->HasFramework()) return false;
    // The original registers itself before it looks anything up and never takes the registration back
    // when a later step fails.
    host->AddSystemService(static_cast<cIGZSystemService*>(this));

    cIGZUnknown* scape = host->AudioScapeManager();
    if (!scape) return false;
    if (scape != scapeManager_) {
        scape->AddRef();
        cIGZUnknown* previous = scapeManager_;
        scapeManager_ = scape;
        if (previous) previous->Release();
    }

    cIGZSndSys* sound = host->SoundSystem();
    if (!sound) return false;
    if (sound != soundSystem_) {
        sound->AddRef();
        cIGZSndSys* previous = soundSystem_;
        soundSystem_ = sound;
        if (previous) previous->Release();
    }
    soundSystem_->RegisterEventHandler(static_cast<cIGZSndEventHandler*>(this));

    if (cIGZMessageServer2* server = host->MessageServer()) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        for (std::uint32_t message : kRegisteredMessages) server->AddNotification(target, message);
    }

    if (industrialFilter_) {
        industrialFilter_->Release();
        industrialFilter_ = nullptr;
    }
    void* created = nullptr;
    if (!host->CreateInstance(kCLSID_cSC4AudioAmbienceManagerFilter, kIID_cSC4AmbienceOccupantFilterAlt,
                              &created) ||
        !created) {
        return false;
    }
    industrialFilter_ = static_cast<sc4::cISC4OccupantFilter*>(created);

    tickTimer_.Start();
    active_ = true;
    return true;
}

// Shutdown @ 0x1003e97e4: unregisters from the framework, the sound system and the message server,
// drops the layers and the industrial filter — but neither the sound system, the occupant manager nor
// the tick filter reference (the destructor does that).
bool cSC4AudioAmbienceManager::Shutdown() {
    IAmbienceManagerHost* host = g_host;
    if (!host || !host->HasFramework()) return false;
    host->RemoveSystemService(static_cast<cIGZSystemService*>(this));

    if (soundSystem_) soundSystem_->UnregisterEventHandler(static_cast<cIGZSndEventHandler*>(this));

    if (cIGZMessageServer2* server = host->MessageServer()) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        for (std::uint32_t message : kRemovedMessages) server->RemoveNotification(target, message);
    }

    RemoveAllLayers();

    if (scapeManager_) {
        cIGZUnknown* previous = scapeManager_;
        scapeManager_ = nullptr;
        previous->Release();
    }
    if (industrialFilter_) {
        sc4::cISC4OccupantFilter* previous = industrialFilter_;
        industrialFilter_ = nullptr;
        previous->Release();
    }
    return true;
}

// _RemoveAllLayers @ 0x1003e99ba: Shutdown then Release every layer, dropping its list node.
void cSC4AudioAmbienceManager::RemoveAllLayers() {
    for (auto it = layers_.begin(); it != layers_.end();) {
        cSC4AudioAmbienceLayer* layer = *it;
        layer->Shutdown();
        layer->Release();
        it = layers_.erase(it);
    }
}

// ---------------------------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------------------------

// _HandleSetMasterVolume @ 0x1003e9dd8: every layer gets the event's first payload word.
void cSC4AudioAmbienceManager::HandleSetMasterVolume(cIGZSndEventStd* event) {
    if (!event) return;
    cIGZMessage2Standard* message = event->AsMessage2Standard();
    if (!message) return;
    const std::uint32_t volume = static_cast<std::uint32_t>(message->GetData1());
    for (cSC4AudioAmbienceLayer* layer : layers_) layer->SetScoreMasterVolume(volume);
}

// DoEvent @ 0x1003e9a3a: the events cSC4AudioAmbienceLayer posts for its freshness scores carry the
// cIFreshPlayer they belong to as their payload object; the manager applies them to that player.
bool cSC4AudioAmbienceManager::DoEvent(cIGZSndEventStd* event) {
    if (!event) return false;
    if (event->GetEventId() == kEventSetMasterVolume) {
        HandleSetMasterVolume(event);
        return false;       // quirk: the master-volume event is handled and then answered false
    }
    cIGZMessage2Standard* message = event->AsMessage2Standard();
    if (!message) return false;
    cIGZUnknown* raw = message->GetIGZUnknown();
    if (!raw) return false;
    raw->AddRef();
    cIFreshPlayer* player = nullptr;
    if (!raw->QueryInterface(kGZIID_cIFreshPlayer, reinterpret_cast<void**>(&player)) || !player) {
        raw->Release();
        return false;
    }

    bool handled = false;
    switch (event->GetEventId()) {
        case kEventScoreStartMode: {   // 0xea2c0390: head-relative from data1, then play
            handled = true;
            player->SetHeadRelative(message->GetData1() != 0);
            player->Play();
            break;
        }
        case kEventScoreStart: {       // 0xea2d827c: the two volume words are the player's distance range
            handled = true;
            player->SetMinMaxDistance(static_cast<std::uint32_t>(message->GetData1()),
                                      static_cast<std::uint32_t>(message->GetData2()));
            break;
        }
        case kEventScoreParameter: {   // 0x0a414c5c
            handled = true;
            player->SetPlayPercent(static_cast<std::int32_t>(message->GetData1()));
            break;
        }
        case kEventScoreAdd: {         // 0x2a2c0384: load the score the three words describe
            handled = true;
            cIGZMessage2Standard* payload = nullptr;
            if (event->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&payload)) && payload) {
                cGZPersistResourceKey key;
                key.type = static_cast<std::uint32_t>(payload->GetData1());
                key.group = static_cast<std::uint32_t>(payload->GetData2());
                key.instance = static_cast<std::uint32_t>(payload->GetData3());
                if (!player->InitializeFromResourceKey(key) && g_host) {
                    // the original formats this message into a temporary cRZString and frees it again
                    char buffer[160];
                    std::snprintf(buffer, sizeof buffer,
                                  "Error: cSC4AudioAmbienceLayer::DoEvent : Failed to load freshness score "
                                  "(%08x:%08x:%08x)\n",
                                  key.type, key.group, key.instance);
                    g_host->LogError(buffer);
                }
                payload->Release();
            }
            break;
        }
        case kEventScoreStop: {        // 0x2a2c0393: fade out and stop
            handled = true;
            player->FadeVolume(kScoreFadeOutMs, 0, true);
            break;
        }
        case kEventScoreRemove: {      // 0x4a2c0389: stop, then shut the player down
            player->Stop();
            handled = true;
            player->Shutdown();
            break;
        }
        case kEventScoreVolume: {      // 0x4a3da208
            handled = true;
            player->SetVolume(static_cast<std::int32_t>(message->GetData1()));
            break;
        }
        default: break;
    }

    player->Release();
    raw->Release();
    return handled;
}

// ---------------------------------------------------------------------------------------------
// messages
// ---------------------------------------------------------------------------------------------

// _HandleFocusChange @ 0x1003e9f96: losing focus pauses every layer and parks the update stage at 4,
// which OnTick never handles (nothing happens until a focus-gain message resets it to 1).
bool cSC4AudioAmbienceManager::HandleFocusChange(cIGZMessage2Standard* message) {
    const std::uint64_t focus = message->GetData1();
    const bool lost = (focus == 0);
    stage_ = lost ? 4u : 1u;
    for (cSC4AudioAmbienceLayer* layer : layers_) layer->Pause(lost);
    return true;
}

// _ReadTuningValues @ 0x1003e9ffc: the zoom distances and the ambience layer ids of the manager's tuning
// exemplar (type 0x6534284a, group 0xca25875d, instance 0x6a359c5c).
void cSC4AudioAmbienceManager::ReadTuningValues() {
    IAmbienceManagerHost* host = g_host;
    if (!host || !host->HasTuning()) return;
    if (!host->GetTuningFloats(kPropZoomDistances, zoomDistances_)) {
        // The original writes five floats through the (empty) vector's storage here; sizing it first is
        // the one deviation in this path.
        zoomDistances_.assign(kZoomDistanceCount, kDefaultZoomDistance);
    }
    host->GetTuningUints(kPropLayerIds, layerIds_);
}

// _CreateAmbienceLayersFromExemplarIds @ 0x1003eb38e
void cSC4AudioAmbienceManager::CreateAmbienceLayersFromExemplarIds() {
    const int count = static_cast<int>(layerIds_.size());
    for (int i = 1; i <= count; ++i) {
        const std::uint32_t instanceId = layerIds_[static_cast<std::size_t>(i - 1)];
        auto* layer = new cSC4AudioAmbienceLayer(instanceId);
        layer->AddRef();
        if (!layer->Init()) {
            char buffer[160];
            std::snprintf(buffer, sizeof buffer,
                          "Error : cSC4AudioAmbienceManager::_CreateAmbienceLayers : Failed to init layer (%08x)\n",
                          instanceId);
            if (g_host) g_host->LogError(buffer);
        } else {
            const AudioPreferences preferences =
                GetServices().game ? GetServices().game->GetAudioPreferences() : AudioPreferences{};
            layer->SetScoreMasterVolume(preferences.soundEnabled ? preferences.masterVolume : 0u);
            layer->AddRef();
            layers_.push_back(layer);
        }
        layer->Release();
    }
}

// _IterateOccupantsThatNeedToBeTracked @ 0x1003eb54c: walk every building / prop / network occupant once
// and let HandleInitOccupant pick up the ones that carry sounds or alarms.
void cSC4AudioAmbienceManager::IterateOccupantsThatNeedToBeTracked() {
    if (!occupantManager_) return;
    auto* filter = new sc4::cSC4OccupantTypesFilter();
    filter->AddRef();
    filter->AddType(0x278128a0);    // buildings
    filter->AddType(0xa823821e);    // props
    filter->AddType(0xc7cc75f1);    // networks
    occupantManager_->IterateOccupants(&IterateOccupantsOnInitCallback, this, nullptr, nullptr, filter);
    filter->Release();
}

// _HandleInitMessage @ 0x1003ea11a
bool cSC4AudioAmbienceManager::HandleInitMessage() {
    IAmbienceManagerHost* host = g_host;
    ReadTuningValues();
    CreateAmbienceLayersFromExemplarIds();

    sc4::cISC4OccupantManager* manager = host ? host->OccupantManager() : nullptr;
    if (manager != occupantManager_) {
        if (manager) manager->AddRef();
        sc4::cISC4OccupantManager* previous = occupantManager_;
        occupantManager_ = manager;
        if (previous) previous->Release();
    }

    int cellsX = 0, cellsZ = 0;
    if (host) host->CityCellExtent(cellsX, cellsZ);
    if (occupantManager_) {
        occupantManager_->StandardCityCellToPosition(0, 0, cityRect_[0], cityRect_[1]);
        occupantManager_->StandardCityCellToPosition(cellsX - 1, cellsZ - 1, cityRect_[2], cityRect_[3]);
    }
    cityRectValid_ = true;
    UpdateAttentionRect();

    // The original allocates this filter with new; see the deviation note in the header.
    void* created = nullptr;
    if (host && host->CreateInstance(kCLSID_cSC4AudioAmbienceManagerFilter, kIID_cSC4AmbienceOccupantFilterAlt,
                                     &created) &&
        created) {
        auto* filter = static_cast<sc4::cISC4OccupantFilter*>(created);
        if (sc4::cISC4OccupantFilter* previous = tickFilter_) {
            tickFilter_ = filter;
            previous->Release();
        } else {
            tickFilter_ = filter;
        }
    }

    if (!iterator_.IsModular()) iterator_.SetModular(true);
    iterator_.Reset(true);
    stage_ = 0;

    if (host) host->AddToTick(static_cast<cIGZSystemService*>(this));
    IterateOccupantsThatNeedToBeTracked();
    active_ = true;
    return true;
}

// _HandleShutdownMessage @ 0x1003ea282
bool cSC4AudioAmbienceManager::HandleShutdownMessage() {
    active_ = false;
    if (g_host) g_host->RemoveFromTick(static_cast<cIGZSystemService*>(this));
    RemoveAllLayers();
    if (occupantManager_) {
        sc4::cISC4OccupantManager* previous = occupantManager_;
        occupantManager_ = nullptr;
        previous->Release();
    }
    if (tickFilter_) {
        sc4::cISC4OccupantFilter* previous = tickFilter_;
        tickFilter_ = nullptr;
        previous->Release();
    }
    return false;
}

// _UpdateAttentionRect @ 0x1003ea2f4: the camera's attention rectangle is the listener target grown by
// the current zoom distance, clipped to the city, and then reduced to even city cells (the update walks
// 2x2 blocks).
void cSC4AudioAmbienceManager::UpdateAttentionRect() {
    Services& services = GetServices();
    ListenerView view;
    // The original reads SFX::Listener() without a null check; without a camera the rect simply stays.
    if (!services.game || !services.game->GetListenerView(view)) return;

    zoomLevel_ = ViewUtilities() ? ViewUtilities()->GetZoomLevel() : 0;
    // Quirk kept: the listener's target is stored where the layers will be handed their "eye" (+0x6c)
    // and the eye where they get their "target" (+0x78).
    listenerTarget_[0] = view.target[0];
    listenerTarget_[1] = view.target[1];
    listenerTarget_[2] = view.target[2];
    listenerEye_[0] = view.eye[0];
    listenerEye_[1] = view.eye[1];
    listenerEye_[2] = view.eye[2];

    if (zoomLevel_ < 0 || static_cast<std::size_t>(zoomLevel_) >= zoomDistances_.size())
        return;     // deviation: the original indexes the table without a bounds check
    const float distance = zoomDistances_[static_cast<std::size_t>(zoomLevel_)];
    sc4::SC4RectT<float> around;
    around.x0 = view.target[0] - distance;
    around.z0 = view.target[2] - distance;
    around.x1 = distance + view.target[0];
    around.z1 = distance + view.target[2];
    sc4::SC4RectT<float> city;
    city.x0 = cityRect_[0];
    city.z0 = cityRect_[1];
    city.x1 = cityRect_[2];
    city.z1 = cityRect_[3];
    attentionRect_.IntersectRect(around, city);

    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    if (occupantManager_) {
        occupantManager_->PositionToStandardCityCell(attentionRect_.x0, attentionRect_.z0, x0, z0);
        occupantManager_->PositionToStandardCityCell(attentionRect_.x1, attentionRect_.z1, x1, z1);
    }
    int cellsX = 0, cellsZ = 0;
    if (g_host) g_host->CityCellExtent(cellsX, cellsZ);
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > cellsX - 1) x1 = cellsX - 1;
    if (z1 > cellsZ - 1) z1 = cellsZ - 1;
    // The minimum corner is masked with 0x7ffffffe and the maximum with 0xfffffffe (both clear bit 0;
    // the first also clears the sign bit) — kept as it is.
    const std::int32_t evenX0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(x0) & 0x7ffffffeu);
    const std::int32_t evenZ0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(z0) & 0x7ffffffeu);
    const std::int32_t evenX1 = static_cast<std::int32_t>(static_cast<std::uint32_t>(x1) & 0xfffffffeu);
    const std::int32_t evenZ1 = static_cast<std::int32_t>(static_cast<std::uint32_t>(z1) & 0xfffffffeu);

    // Quirk kept: the "have we been here before" flag is a function-local static, so the very first
    // update of the process seeds the previous rectangle instead of shifting it.
    static bool firstTimeThrough = false;
    if (!firstTimeThrough) {
        firstTimeThrough = true;
        previousCells_[0] = evenX0;
        previousCells_[1] = evenZ0;
        previousCells_[2] = evenX1;
        previousCells_[3] = evenZ1;
    } else {
        previousCells_[0] = currentCells_[0];
        previousCells_[1] = currentCells_[1];
        previousCells_[2] = currentCells_[2];
        previousCells_[3] = currentCells_[3];
    }
    currentCells_[0] = evenX0;
    currentCells_[1] = evenZ0;
    currentCells_[2] = evenX1;
    currentCells_[3] = evenZ1;
}

// _HandleOccupantMessages @ 0x1003ea4cc
bool cSC4AudioAmbienceManager::HandleOccupantMessages(cIGZMessage2Standard* message) {
    if (!message) return false;
    cIGZUnknown* source = message->GetIGZUnknown();
    if (source) source->AddRef();
    sc4::cISC4Occupant* occupant = nullptr;
    IAmbienceLot* lot = nullptr;

    const std::uint32_t type = message->GetType();
    if (source && (type == sc4::kMessageOccupantInserted || type == sc4::kMessageOccupantRemovedFromManager ||
                   type == sc4::kMessageOccupantRemoved)) {
        void* raw = nullptr;
        if (source->QueryInterface(sc4::kGZIID_cISC4Occupant, &raw) && raw) {
            occupant = static_cast<sc4::cISC4Occupant*>(raw);
            sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
            if (!holder || !holder->HasProperty(kPropMarked)) {
                if (type == sc4::kMessageOccupantInserted) HandleInsertOccupant(occupant);
                else if (type == sc4::kMessageOccupantRemovedFromManager) HandleRemoveOccupant(occupant);
                else HandleMoveOccupant(occupant);
            }
        }
    } else if (source && type == sc4::kMessageLotConditionChanged) {
        lot = g_host ? g_host->LotOfObject(source) : nullptr;
        if (lot)
            UpdateLotOccupantsForConditionChange(lot, static_cast<int>(message->GetData1()),
                                                static_cast<int>(message->GetData2()));
    }

    if (lot) lot->Release();
    if (occupant) occupant->Release();
    if (source) source->Release();
    return true;    // quirk: every type answered true, including the ones this function ignores
}

// _HandleAutomatonMessage @ 0x1003ea70c: an automaton's callback messages. Data2 is the automaton kind
// (12 / 13), Data3 the signal, Void1 the automaton itself.
bool cSC4AudioAmbienceManager::HandleAutomatonMessage(cIGZMessage2Standard* message) {
    if (!message) return true;
    const std::uint32_t kind = static_cast<std::uint32_t>(message->GetData2());
    if (kind == 12 || kind == 13) {
        const std::uint64_t signal = message->GetData3();
        void* payload = message->GetVoid1();
        if (signal == 0x10) {
            // The original treats the payload as the automaton directly (no QueryInterface) and asks its
            // state 0x10: a moving automaton plays the occupant's sound, a stopped one stops it.
            IAmbienceAutomaton* automaton = payload && g_host ? g_host->AutomatonOfCallbackPayload(payload) : nullptr;
            if (!automaton) return true;
            automaton->AddRef();
            const bool hasState = automaton->FindState(kAutomatonRunningState) != nullptr;
            const bool play = !(hasState || kind == 12);
            PlayOccupantSound(automaton->AsOccupant(), play);
            automaton->Release();
        } else if (signal == 0x1e || signal == 0x14) {
            IAmbienceAutomaton* automaton = payload && g_host ? g_host->AutomatonOfCallbackPayload(payload) : nullptr;
            if (!automaton) return true;
            automaton->AddRef();
            PlayOccupantSound(automaton->AsOccupant(), kind != 12);
            automaton->Release();
        }
        return true;
    }

    if (kind == 9 && soundSystem_) {
        // "the automaton moved": update the sound position of its occupant instead of restarting it.
        void* payload = message->GetVoid1();
        if (!payload) return true;
        IAmbienceAutomaton* automaton = g_host ? g_host->AutomatonOfCallbackPayload(payload) : nullptr;
        if (!automaton) return true;
        float position[3] = {0.0f, 0.0f, 0.0f};
        automaton->GetPosition(position);
        sc4::cISC4Occupant* occupant = automaton->AsOccupant();
        sc4::cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
        std::uint32_t soundId = 0;
        if (!PropertyUint(holder, kPropSoundId, soundId)) return true;
        soundSystem_->PostEventIds(kEventSetSoundPosition, OccupantInstanceId(occupant, soundId),
                                   static_cast<std::uint32_t>(PositionToInt(position[0])),
                                   static_cast<std::uint32_t>(PositionToInt(position[1])),
                                   static_cast<std::uint32_t>(PositionToInt(position[2])));
    }
    return true;
}

// _HandleRailwayCrossingMessage @ 0x1003ea900: a crossing's payload object is an automaton whose occupant
// carries the crossing sound (0xaa87817a); it is started at the automaton's position.
bool cSC4AudioAmbienceManager::HandleRailwayCrossingMessage(cIGZMessage2Standard* message) {
    if (!soundSystem_) return true;
    if (!message || message->GetType() != kMessageRailwayCrossing) return false;
    cIGZUnknown* source = message->GetIGZUnknown();
    if (!source) return true;
    source->AddRef();
    IAmbienceAutomaton* automaton = QueryAutomaton(source);
    if (!automaton) {
        source->Release();
        return true;
    }
    sc4::cISC4Occupant* occupant = automaton->AsOccupant();
    sc4::cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
    std::uint32_t soundId = 0;
    if (PropertyUint(holder, kPropRailwayCrossingSound, soundId)) {
        float position[3] = {0.0f, 0.0f, 0.0f};
        automaton->GetPosition(position);
        // the instance id is the payload object's address, not the occupant's
        const std::uint32_t instance = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(source));
        soundSystem_->PostEventIds(kEventStartSound, soundId, instance, 1, 0);
        soundSystem_->PostEventIds(kEventSetSoundPosition, instance,
                                   static_cast<std::uint32_t>(PositionToInt(position[0])),
                                   static_cast<std::uint32_t>(PositionToInt(position[1])),
                                   static_cast<std::uint32_t>(PositionToInt(position[2])));
    }
    automaton->Release();
    source->Release();
    return true;
}

// DoMessage @ 0x1003e9e4a: dispatch by message type. Only the shutdown message answers false.
bool cSC4AudioAmbienceManager::DoMessage(cIGZMessage2* message) {
    if (!message) return false;
    cIGZMessage2Standard* standard = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) || !standard)
        return false;
    bool result = true;
    switch (standard->GetType()) {
        case kMessageCityInit:
            HandleInitMessage();
            break;
        case kMessageCityShutdown:
            HandleShutdownMessage();
            result = false;
            break;
        case sc4::kMessageOccupantInserted:
        case sc4::kMessageOccupantRemovedFromManager:
        case sc4::kMessageOccupantRemoved:
        case sc4::kMessageLotStateChanged:
        case sc4::kMessageLotConditionChanged:
            HandleOccupantMessages(standard);
            break;
        case kMessageAutomaton:
            HandleAutomatonMessage(standard);
            break;
        case kMessageReloadTuning:
            ReadTuningValues();
            break;
        case kMessageListenerMoved:
            UpdateAttentionRect();
            break;
        case kMessageRailwayCrossing:
            HandleRailwayCrossingMessage(standard);
            break;
        case kMessageFocusChange:
            HandleFocusChange(standard);
            break;
        default:
            break;
    }
    standard->Release();
    return result;
}

// ---------------------------------------------------------------------------------------------
// tick
// ---------------------------------------------------------------------------------------------

// OnTick @ 0x1003eaa8e: a three-stage walk. Stage 0/1 prepares the block iterator over the attention
// rectangle (and begins every layer's update); stage 2 spends up to one millisecond handing cells and
// occupants to the layers; stage 3 ends the layers' update. A focus-change message parks the stage at 4.
bool cSC4AudioAmbienceManager::OnTick(std::uint32_t elapsedMs) {
    (void)elapsedMs;    // the original ignores the framework's tick length and measures with its own timer
    if (!active_) return true;
    const std::uint32_t elapsed = tickTimer_.GetElapsedTimeU32();
    tickTimer_.Restart();
    UpdateAlarms();

    if (stage_ < 2) {
        const std::int32_t minX = currentCells_[0] >> 1;
        const std::int32_t minZ = currentCells_[1] >> 1;
        std::int32_t maxX = currentCells_[2] >> 1;
        std::int32_t maxZ = currentCells_[3] >> 1;
        if (maxX < minX) maxX = minX;
        if (maxZ < minZ) maxZ = minZ;
        iterator_.SetRect(minX, minZ, maxX, maxZ);
        iterator_.Reset(false);
        const SC4RectFloat rect{attentionRect_.x0, attentionRect_.z0, attentionRect_.x1, attentionRect_.z1};
        for (cSC4AudioAmbienceLayer* layer : layers_) layer->BeginUpdate(rect);
        stage_ = 2;
    } else if (stage_ != 2) {
        if (stage_ != 3) return true;
        for (cSC4AudioAmbienceLayer* layer : layers_) layer->EndUpdate();
        stage_ = 1;
        return true;
    }

    if (!DoStageUpdate(elapsed)) return true;
    stage_ = 3;
    for (cSC4AudioAmbienceLayer* layer : layers_) layer->EndUpdate();
    stage_ = 1;
    return true;
}

// _DoStageUpdate @ 0x1003eaf44: one block of four cells at a time until a millisecond has been spent.
bool cSC4AudioAmbienceManager::DoStageUpdate(std::uint32_t elapsedMs) {
    workTimer_.Restart();
    bool finished = false;
    if (workTimer_.GetElapsedTimeU32() < 1000) {
        for (;;) {
            if (iterator_.IsDone()) {
                finished = true;
                break;
            }
            int a = 0, b = 0;
            if (!iterator_.Next(a, b)) {
                finished = true;
                break;
            }
            const std::int32_t x0 = a * 2, z0 = b * 2;
            const std::int32_t x1 = a * 2 + 1, z1 = b * 2 + 1;
            int from[2] = {x0, z0};
            int to[2] = {x1, z1};
            if (occupantManager_ && tickFilter_) {
                occupantManager_->IterateOccupantsByStandardCityCell(&IterateOccupantsOnTickCallback, this, from, to,
                                                                     tickFilter_);
            }
            for (cSC4AudioAmbienceLayer* layer : layers_) layer->HandleCell(x0, z0);
            for (cSC4AudioAmbienceLayer* layer : layers_) layer->HandleCell(x1, z0);
            for (cSC4AudioAmbienceLayer* layer : layers_) layer->HandleCell(x0, z1);
            for (cSC4AudioAmbienceLayer* layer : layers_) layer->HandleCell(x1, z1);
            if (workTimer_.GetElapsedTimeU32() >= 1000) break;
        }
    }
    // Quirk kept: the listeners' "eye" is the manager's listener target and the "target" its eye.
    const cS3DVector3 eye{listenerTarget_[0], listenerTarget_[1], listenerTarget_[2]};
    const cS3DVector3 target{listenerEye_[0], listenerEye_[1], listenerEye_[2]};
    for (cSC4AudioAmbienceLayer* layer : layers_) layer->Update(elapsedMs, eye, target);
    return finished;
}

bool cSC4AudioAmbienceManager::IterateOccupantsOnTickCallback(sc4::cISC4Occupant* occupant, void* context) {
    if (!occupant || !context) return false;
    auto* manager = static_cast<cSC4AudioAmbienceManager*>(context);
    occupant->AddRef();
    const bool result = manager->HandleOccupant(occupant);
    occupant->Release();
    return result;
}

bool cSC4AudioAmbienceManager::IterateOccupantsOnInitCallback(sc4::cISC4Occupant* occupant, void* context) {
    if (!occupant || !context) return false;
    auto* manager = static_cast<cSC4AudioAmbienceManager*>(context);
    occupant->AddRef();
    manager->HandleInitOccupant(occupant);
    occupant->Release();
    return true;
}

// _UpdateAlarms @ 0x1003eabd6: the alarms whose hour the clock has just passed. A one-shot alarm fires
// once (its sound is created and positioned); a repeating one starts its sound at the even entries of
// its time list and stops it at the odd ones.
void cSC4AudioAmbienceManager::UpdateAlarms() {
    IAmbienceManagerHost* host = g_host;
    if (!host) return;
    int hour = 0, minute = 0;
    if (!host->ClockTime(hour, minute)) return;     // no clock: nothing to compare against
    const float now = static_cast<float>(hour) + static_cast<float>(minute) / 60.0f;
    const float nowMinusDay = now + -24.0f;

    for (tOccupantAlarm& alarm : alarms_) {
        const int count = alarm.count;
        for (int i = 0; i < count; ++i) {
            const float time = alarm.times[static_cast<std::size_t>(i)];
            if (!alarm.repeating) {
                // fired when the clock passed `time` since the last update
                if (time <= now && time > alarm.hour) {
                    const std::uint32_t instance = host->GenerateNewInstanceId();
                    float position[3] = {0.0f, 0.0f, 0.0f};
                    if (alarm.occupant) alarm.occupant->GetPosition(position);
                    if (soundSystem_) {
                        soundSystem_->PostEventIds(kEventStartSound, alarm.soundId, instance, 1, 0);
                        soundSystem_->PostEventIds(kEventSetSoundPosition, instance,
                                                   static_cast<std::uint32_t>(PositionToInt(position[0])),
                                                   static_cast<std::uint32_t>(PositionToInt(position[1])),
                                                   static_cast<std::uint32_t>(PositionToInt(position[2])));
                    }
                }
                continue;
            }
            const int next = (i == count - 1) ? 0 : i + 1;
            const float timeNext = alarm.times[static_cast<std::size_t>(next)];
            const bool window = (time <= now && now < timeNext) ||
                                (timeNext < time &&
                                 (now >= time || (nowMinusDay >= time && timeNext > nowMinusDay)));
            if (!window) continue;
            std::uint32_t soundId = 0;
            sc4::cISCPropertyHolder* holder = alarm.occupant ? alarm.occupant->AsPropertyHolder() : nullptr;
            if (!PropertyUint(holder, kPropAlarmSoundRepeat, soundId)) continue;
            if ((i & 1) == 0) {
                if (!alarm.active) {
                    AddOccupantSndInstance(alarm.occupant, soundId, true);
                    alarm.active = true;
                }
            } else if (alarm.active) {
                RemoveOccupantSndInstance(alarm.occupant, soundId);
                alarm.active = false;
            }
        }
        alarm.hour = now;
    }
}

// ---------------------------------------------------------------------------------------------
// occupant handling
// ---------------------------------------------------------------------------------------------

// HandleOccupant @ 0x1003ec4dc: the automaton tick callback. Needs the scape manager, but answers true
// even when the occupant is not visible and nothing is forwarded.
bool cSC4AudioAmbienceManager::HandleOccupant(sc4::cISC4Occupant* occupant) {
    if (!scapeManager_) return false;
    if (occupant && occupant->IsFlagSet(kOccupantFlagVisible)) {
        for (cSC4AudioAmbienceLayer* layer : layers_) layer->HandleOccupant(occupant);
    }
    return true;
}

// HandleInsertOccupant @ 0x1003eb95e
void cSC4AudioAmbienceManager::HandleInsertOccupant(sc4::cISC4Occupant* occupant) {
    if (!occupant) return;
    sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    bool marked = false;    // the original reads an uninitialised byte here; see the header
    const bool hasMarked = PropertyBool(holder, kPropMarked, marked);
    if (marked && hasMarked) return;

    if (industrialFilter_) {
        // Quirk kept: the original passes the *occupant* where the filter expects a property holder (the
        // filter ignores its argument and answers true, so every occupant counts).
        if (industrialFilter_->IsPropertyHolderIncluded(reinterpret_cast<sc4::cISCPropertyHolder*>(occupant)))
            ++industrialPopulation_;
    }

    holder = occupant->AsPropertyHolder();
    if (!holder) return;
    std::uint32_t id = 0;
    if (PropertyUint(holder, kPropSoundId, id)) AddOccupantSndInstance(occupant, id, true);

    std::vector<float> times;
    if (PropertyFloats(holder, kPropAlarmTimes, times)) {
        if (PropertyUint(holder, kPropAlarmSound, id)) AddAlarmOccupant(occupant, id, times, false);
        if (PropertyUint(holder, kPropAlarmSoundRepeat, id)) AddAlarmOccupant(occupant, id, times, true);
    }
    if (PropertyUint(holder, kPropSoundId2, id)) AddOccupantSndInstance(occupant, id, false);

    if (PropertyUint(holder, kPropSoundStateB, id) || PropertyUint(holder, kPropSoundStateA, id) ||
        PropertyUint(holder, kPropSoundVacant, id)) {
        IAmbienceLot* lot = g_host ? g_host->LotOfOccupant(occupant) : nullptr;
        UpdateLotOccupantsForConditionChange(lot, -1, -1);
    }
}

// HandleMoveOccupant @ 0x1003ebb40
void cSC4AudioAmbienceManager::HandleMoveOccupant(sc4::cISC4Occupant* occupant) {
    if (!occupant) return;
    sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    bool marked = false;    // uninitialised in the original, as in HandleInsertOccupant
    const bool hasMarked = PropertyBool(holder, kPropMarked, marked);
    if (marked && hasMarked) return;

    holder = occupant->AsPropertyHolder();
    if (!holder) return;
    if (holder->HasProperty(kPropSoundId) && soundSystem_) {
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        soundSystem_->PostEventIds(kEventSetSoundPosition, OccupantInstanceId(occupant),
                                   static_cast<std::uint32_t>(PositionToInt(position[0])),
                                   static_cast<std::uint32_t>(PositionToInt(position[1])),
                                   static_cast<std::uint32_t>(PositionToInt(position[2])));
    }
    std::uint32_t dummy = 0;
    if (PropertyUint(holder, kPropSoundStateB, dummy)) MoveOccupantSndInstance(occupant);
}

// HandleRemoveOccupant @ 0x1003ebc36
void cSC4AudioAmbienceManager::HandleRemoveOccupant(sc4::cISC4Occupant* occupant) {
    if (!occupant) return;
    sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    bool marked = false;
    const bool hasMarked = PropertyBool(holder, kPropMarked, marked);
    if (marked && hasMarked) return;

    if (industrialFilter_) {
        if (industrialFilter_->IsPropertyHolderIncluded(reinterpret_cast<sc4::cISCPropertyHolder*>(occupant)))
            --industrialPopulation_;
    }

    holder = occupant->AsPropertyHolder();
    if (!holder) return;
    std::uint32_t id = 0;
    if (PropertyUint(holder, kPropSoundStateA, id)) RemoveOccupantSndInstance(occupant, id);
    if (PropertyUint(holder, kPropSoundStateB, id)) RemoveOccupantSndInstance(occupant, id);
    if (PropertyUint(holder, kPropSoundId, id)) RemoveOccupantSndInstance(occupant, id);
    if (PropertyUint(holder, kPropAlarmSound, id)) RemoveAlarmOccupant(occupant, id);
    if (PropertyUint(holder, kPropAlarmSoundRepeat, id)) RemoveAlarmOccupant(occupant, id);
}

// HandleInitOccupant @ 0x1003ec048: the city was just loaded / started, so only the alarms and the
// condition-dependent sounds need setting up (the main sound is started by the first update).
void cSC4AudioAmbienceManager::HandleInitOccupant(sc4::cISC4Occupant* occupant) {
    if (!occupant) return;
    if (industrialFilter_) {
        if (industrialFilter_->IsPropertyHolderIncluded(reinterpret_cast<sc4::cISCPropertyHolder*>(occupant)))
            ++industrialPopulation_;
    }
    sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    if (!holder) return;
    std::uint32_t id = 0;
    std::vector<float> times;
    if (PropertyFloats(holder, kPropAlarmTimes, times)) {
        if (PropertyUint(holder, kPropAlarmSound, id)) AddAlarmOccupant(occupant, id, times, false);
        if (PropertyUint(holder, kPropAlarmSoundRepeat, id)) AddAlarmOccupant(occupant, id, times, true);
    }
    if (PropertyUint(holder, kPropSoundStateB, id) || PropertyUint(holder, kPropSoundStateA, id) ||
        PropertyUint(holder, kPropSoundVacant, id)) {
        IAmbienceLot* lot = g_host ? g_host->LotOfOccupant(occupant) : nullptr;
        UpdateLotOccupantsForConditionChange(lot, -1, -1);
    }
}

// _UpdateLotOccupantsForConditionChange @ 0x1003eb696: when a lot's condition changes, its occupants swap
// between two sounds depending on the lot state (and the newly built / vacated cases).
void cSC4AudioAmbienceManager::UpdateLotOccupantsForConditionChange(IAmbienceLot* lot, int condition, int state) {
    if (!lot) return;
    if (state < 0) state = lot->State();
    const std::uint32_t combined = static_cast<std::uint32_t>(state) | 1u;
    const bool vacant = (condition == 1 && state == 0);

    std::list<sc4::cISC4Occupant*> occupants;
    lot->GetOccupants(occupants);
    for (sc4::cISC4Occupant* occupant : occupants) {
        if (!occupant) continue;
        // The original lets an occupant through when it answers either the building-occupant view or the
        // prop-occupant view; the queried reference is dropped at once.
        cIGZUnknown* view = nullptr;
        bool included = occupant->QueryInterface(kGZIID_cISC4BuildingOccupantView, reinterpret_cast<void**>(&view));
        if (included && view) view->Release();
        if (!included) {
            view = nullptr;
            included = occupant->QueryInterface(kGZIID_cISC4PropOccupantView, reinterpret_cast<void**>(&view));
            if (included && view) view->Release();
        }
        if (!included) continue;

        sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        if (!holder) continue;
        std::uint32_t id = 0;
        if (state >= 0 && state <= 6 &&
            ((kLotStateSoundSwapMask >> (static_cast<std::uint32_t>(state) & 0x1f)) & 1u) != 0) {
            if (PropertyUint(holder, kPropSoundStateA, id)) RemoveOccupantSndInstance(occupant, id);
            if (PropertyUint(holder, kPropSoundStateB, id)) AddOccupantSndInstance(occupant, id, true);
        }
        if (combined == 3u) {
            if (PropertyUint(holder, kPropSoundStateB, id)) RemoveOccupantSndInstance(occupant, id);
            if (PropertyUint(holder, kPropSoundStateA, id)) AddOccupantSndInstance(occupant, id, true);
        }
        if (state == 4 || state == 1) {
            if (PropertyUint(holder, kPropSoundStateA, id)) RemoveOccupantSndInstance(occupant, id);
            if (PropertyUint(holder, kPropSoundStateB, id)) RemoveOccupantSndInstance(occupant, id);
        }
        if (vacant) {
            if (PropertyUint(holder, kPropSoundVacant, id)) AddOccupantSndInstance(occupant, id, true);
        }
    }
    if (occupantManager_) occupantManager_->ReleaseOccupantList(occupants);
    occupants.clear();      // the original clears the list after the manager released its entries
}

// _AddOccupantSndInstance @ 0x1003ebd6a
bool cSC4AudioAmbienceManager::AddOccupantSndInstance(sc4::cISC4Occupant* occupant, std::uint32_t soundId,
                                                     bool flag) {
    if (!occupant || !occupant->IsFlagSet(kOccupantFlagSoundEligible)) return false;
    bool marked = false;
    PropertyBool(occupant->AsPropertyHolder(), kPropMarked, marked);
    if (marked) return false;

    IAmbienceAutomaton* automaton = QueryAutomaton(occupant);
    if (automaton) automaton->AddCallback(static_cast<cIGZMessageTarget2*>(this));

    if (soundSystem_) {
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        const std::uint32_t instance = OccupantInstanceId(occupant, soundId);
        soundSystem_->PostEventIds(kEventStartSound, soundId, instance, flag ? 1u : 0u, 0u);
        soundSystem_->PostEventIds(kEventSetSoundPosition, instance,
                                   static_cast<std::uint32_t>(PositionToInt(position[0])),
                                   static_cast<std::uint32_t>(PositionToInt(position[1])),
                                   static_cast<std::uint32_t>(PositionToInt(position[2])));
    }
    if (automaton) automaton->Release();
    return true;
}

// _AddAlarmOccupant @ 0x1003ebeb8
bool cSC4AudioAmbienceManager::AddAlarmOccupant(sc4::cISC4Occupant* occupant, std::uint32_t soundId,
                                                const std::vector<float>& times, bool repeating) {
    if (!occupant || !occupant->IsFlagSet(kOccupantFlagSoundEligible)) return false;
    if (occupant->IsFlagSet(kOccupantFlagNoAlarm)) return false;
    occupant->AddRef();
    int hour = 0, minute = 0;
    // The original dereferences the clock without a check; here a missing clock means midnight.
    if (g_host) g_host->ClockTime(hour, minute);

    tOccupantAlarm alarm;
    alarm.occupant = occupant;
    alarm.soundId = soundId;
    alarm.hour = static_cast<float>(hour) + static_cast<float>(minute) / 60.0f;
    alarm.count = static_cast<std::int32_t>(times.size());
    alarm.times = times;
    alarm.active = false;
    alarm.repeating = repeating;
    alarms_.push_back(alarm);
    return true;
}

// _MoveOccupantSndInstance @ 0x1003ec1aa
bool cSC4AudioAmbienceManager::MoveOccupantSndInstance(sc4::cISC4Occupant* occupant) {
    if (soundSystem_ && occupant) {
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        soundSystem_->PostEventIds(kEventSetSoundPosition, OccupantInstanceId(occupant),
                                   static_cast<std::uint32_t>(PositionToInt(position[0])),
                                   static_cast<std::uint32_t>(PositionToInt(position[1])),
                                   static_cast<std::uint32_t>(PositionToInt(position[2])));
    }
    return true;
}

// _RemoveOccupantSndInstance @ 0x1003ec228
bool cSC4AudioAmbienceManager::RemoveOccupantSndInstance(sc4::cISC4Occupant* occupant, std::uint32_t soundId) {
    if (!occupant) return false;
    IAmbienceAutomaton* automaton = QueryAutomaton(occupant);
    if (automaton) automaton->RemoveCallback(static_cast<cIGZMessageTarget2*>(this));
    if (soundSystem_)
        soundSystem_->PostEventIds(kEventStopSound, OccupantInstanceId(occupant, soundId), 0u, 0u, 0u);
    if (automaton) automaton->Release();
    return true;
}

// _RemoveAlarmOccupant @ 0x1003ec2d2: drops the *first* alarm of the occupant (whatever sound id is
// asked for), stopping its sound when it was playing.
bool cSC4AudioAmbienceManager::RemoveAlarmOccupant(sc4::cISC4Occupant* occupant, std::uint32_t soundId) {
    (void)soundId;      // quirk: the sound id argument is never compared
    if (!occupant) return false;
    for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
        if (it->occupant != occupant) continue;
        if (it->active) {
            RemoveOccupantSndInstance(occupant, soundId);
            it->active = false;
        }
        it->occupant->Release();
        it->occupant = nullptr;
        alarms_.erase(it);
        return true;
    }
    return false;
}

// _PlayOccupantSound @ 0x1003ec332: start (and position) or stop the occupant's main sound.
void cSC4AudioAmbienceManager::PlayOccupantSound(sc4::cISC4Occupant* occupant, bool play) {
    if (!occupant || !soundSystem_) return;
    std::uint32_t soundId = 0;
    if (!PropertyUint(occupant->AsPropertyHolder(), kPropSoundId, soundId)) return;
    const std::uint32_t instance = OccupantInstanceId(occupant, soundId);
    if (play) {
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        soundSystem_->PostEventIds(kEventStartSound, soundId, instance, 1, 0);
        soundSystem_->PostEventIds(kEventSetSoundPosition, instance,
                                   static_cast<std::uint32_t>(PositionToInt(position[0])),
                                   static_cast<std::uint32_t>(PositionToInt(position[1])),
                                   static_cast<std::uint32_t>(PositionToInt(position[2])));
    } else {
        soundSystem_->PostEventIds(kEventStopSound, instance, 0u, 0u, 0u);
    }
}

}  // namespace SC4Audio
