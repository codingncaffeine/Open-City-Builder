// cSC4AudioEventHandler / cSC4AudioEventHandlerDisasterFire — see sc4audio/AudioEventHandler.h for the recovered
// layout, the message registry and the region-music path.
//
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1: cSC4AudioEventHandler 0x1003ed1a2..0x1003efe08 (23 named
// functions plus their non-virtual thunks) and cSC4AudioEventHandlerDisasterFire 0x1003ec79c..0x1003ed0b8 (14 named
// functions plus thunks), both from SC4AudioEventHandler.cpp (module initialiser @ 0x1003efe16).
#include "sc4audio/AudioEventHandler.h"

#include <cstdio>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "audio/interfaces/cIGZSndPropertyCache.h"
#include "audio/interfaces/cIGZSndRegisterSet.h"
#include "gz/RZServices.h"
#include "gz/StreamOps.h"   // cS3DVector3
#include "gz/cGZInputMessages.h"
#include "gz/cIGZFileSystem.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZPoint.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ISC4City.h"
#include "sc4/Occupant.h"
#include "sc4audio/AudioScapeManager.h"
#include "sc4audio/SC4AudioHost.h"
#include "sc4audio/SoundManager.h"

using namespace sc4audio;

namespace {

// The messages Init registers with SL::spMS2 (registration order at 0x1003ed4xx).
constexpr std::uint32_t kRegisteredMessages[] = {
    kMessageCityInit,          kMessageCityShutdown,   kMessageTuningReload,   kMessagePostMayorMusic,
    kMessagePostRegionMusic,   kMessageListenerProperty, kMessageRadioStation, kMessageRadioStationOptions,
    kMessageStopPlayback,      kMessagePlaySong,       kMessageRadioOnOffVolume, kMessageRadioOnDemand,
    kMessageFocusChange,
};
// The automata / construction / dispatch messages the city-init handler adds and the city-shutdown handler removes
// (0x1003eebc3 / 0x1003eeaad). 0xe94671ed is registered but DoMessage never dispatches it (quirk kept).
constexpr std::uint32_t kCityMessages[] = {
    0xe94671eb, 0xe94671ed, kMessageDispatchFire,     kMessageAutomataInstance, kMessageAutomataEffect,
    kMessageAutomataStop,      kMessageDispatchPylonPolice, kMessageDispatchPylonFire, kMessageAutomataCreate,
    kMessageConstructionNetwork, kMessageConstructionUpdate,
};
// The sound ids whose start events carry flags = 1 (cSC4AudioSoundManager's alias of kSoundInstanceEventStart).
constexpr std::uint32_t kSoundInstanceEventStartAlias = 1;
constexpr std::uint32_t kSoundIdDisabledClick = 0xaa62c9d5;   // sc4/WinCatalogView.h kCatalogSoundDisabled
constexpr std::uint32_t kPylonOccupantIid = 0xc9b6d7d6;      // cISC4PylonOccupant
constexpr std::uint32_t kSndPropertyGlobalVolume = 0x7f;     // global sound property DoEvent 0xbaab7a4 writes
constexpr std::uint32_t kDispatchTypeSoundMask = 0x63;       // dispatch types 0, 1, 5 and 6 make a sound

// torch-tool state (function-local statics @ 0x100b8a250..0x100b8a264; process-wide in the original).
std::int32_t g_torchLastX = 0;
std::int32_t g_torchLastY = 0;
std::int32_t g_torchLastTime = 0;
std::uint8_t g_torchMousePressed = 0;
std::uint8_t g_torchFlared = 0;
float g_torchLastVelocity = 0.0f;

std::uint32_t ReadU32(const std::uint8_t* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}
void WriteU32(std::uint8_t* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// The original's cvttss2si: NaN and out-of-range give INT32_MIN.
std::int32_t FloatToInt32(float value) {
    if (!(value > -2147483648.0f && value < 2147483648.0f)) return INT32_MIN;
    return static_cast<std::int32_t>(value);
}

// Every soundscape instance key in this class is the low 32 bits of the game object the sound belongs to.
std::uint32_t InstanceKey(const void* object) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(object));
}

using cS3DVector3 = ::cS3DVector3;

// ---------------------------------------------------------------------------------------------
// EnumFunc_UpdateMinMaxDistances (free function @ 0x1003efc6d), used by DoEvent 0xaa36d9b6 through
// cIGZSndSys::EnumerateTargets: re-write register 0x20 of every target that has a register set, which makes the
// sound recompute its minimum / maximum distance.
bool EnumFunc_UpdateMinMaxDistances(cIGZSndUpdateTarget* target, std::uint32_t, void*) {
    if (!target) return true;
    cIGZSndRegisterSet* registers = nullptr;
    if (target->QueryInterface(kGZIID_cIGZSndRegisterSet, reinterpret_cast<void**>(&registers)) && registers) {
        const std::int32_t value = registers->GetRegisterValue(0x20);
        registers->SetRegisterValue(0x20, value, false);
        registers->Release();
    }
    return true;
}

// The built-in host: the values the binary uses for everything cSC4AudioPreferencesWinProc owns, and "not
// available" for the neighbours that need the game.
class DefaultAudioEventHandlerHost final : public IAudioEventHandlerHost {
public:
    std::uint8_t* PreferencesBlob() override { return SL::spPreferences; }
};

DefaultAudioEventHandlerHost g_defaultHost;
IAudioEventHandlerHost* g_host = &g_defaultHost;

}  // namespace

namespace sc4audio {

// Out of line so the interface's vtable / typeinfo live in this translation unit.
IAudioEventHandlerHost::~IAudioEventHandlerHost() = default;
std::uint8_t* IAudioEventHandlerHost::PreferencesBlob() { return SL::spPreferences; }

IAudioEventHandlerHost* AudioEventHandlerHost() { return g_host; }
void SetAudioEventHandlerHost(IAudioEventHandlerHost* host) { g_host = host ? host : &g_defaultHost; }

}  // namespace sc4audio

// =============================================================================================
// cSC4AudioEventHandler
// =============================================================================================

// @ 0x1003dedb2: new 0x80, constructor.
cSC4AudioEventHandler* cSC4AudioEventHandler::Create() { return new cSC4AudioEventHandler(); }

// @ 0x1003ed1a2. The two cRZSysServPtr members look the services up in the framework here, before the other
// members are built.
cSC4AudioEventHandler::cSC4AudioEventHandler() = default;

// @ 0x1003ed2e0. Releases the soundscape manager, then the two services (member destructors). The original clears
// the modal-window list without releasing the windows it holds (quirk kept: std::list::clear only frees nodes).
cSC4AudioEventHandler::~cSC4AudioEventHandler() {
    modalWindows_.clear();
    if (scapeManager_) {
        scapeManager_->Release();
        scapeManager_ = nullptr;
    }
}

// @ 0x1003efda6. iids 1 / 0xbb523bb2 answer the primary view, 0x452294aa the +0x10 cIGZMessageTarget2 view and
// 0x89e51472 the +0x18 cIGZInitShutdown view; the +0x08 cIGZMessageTarget view is never answered (quirk).
bool cSC4AudioEventHandler::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cSC4AudioEventHandler) {
        *out = static_cast<cIGZSndEventHandler*>(this);
    } else if (iid == sc4::kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cIGZInitShutdown) {
        *out = static_cast<cIGZInitShutdown*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// @ 0x1003efd2a. At a count of 1 (or 0) the deleting destructor runs without decrementing first (quirk kept).
std::uint32_t cSC4AudioEventHandler::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// @ 0x1003ed3e8.
bool cSC4AudioEventHandler::Init() {
    if (initialized_) return true;
    IAudioEventHandlerHost* host = AudioEventHandlerHost();

    // A second sound-system lookup; the answer is used here but never stored back into +0x28 (quirk kept).
    cRZSysServPtr<cIGZSndSys, kGZSID_cGZSndSys, kGZIID_cIGZSndSys> soundSystem;
    cIGZSndSys* soundSystemPointer = soundSystem;
    if (!soundSystemPointer) return false;
    soundSystemPointer->RegisterEventHandler(this);

    cRZSysServPtr<cISC4AudioScapeManager, kGZSID_cISC4AudioScapeManager, kGZIID_cISC4AudioScapeManager> scapeManager;
    cISC4AudioScapeManager* scapePointer = scapeManager;
    if (!scapePointer) return false;
    if (scapeManager_ != scapePointer) {
        scapePointer->AddRef();
        cISC4AudioScapeManager* previous = scapeManager_;
        scapeManager_ = scapePointer;
        if (previous) previous->Release();
    }

    cRZSysServPtr<cIGZFileSystem, kGZSID_cGZFileSystem, kGZIID_cIGZFileSystem> fileSystem;
    if (cIGZFileSystem* fileSystemPointer = fileSystem) {
        cRZString radioDirectory;
        radioDirectory.str() = std::string(fileSystemPointer->DataDirectory()) + kRadioBaseDirectory;
        if (radio_) {
            radio_->SetRadioDirectory(radioDirectory);
            radio_->TurnOn(false);
            radio_->SetVolume(0);
        }
    }

    if (SL::spMS) {
        SL::spMS->AddRef();
        cIGZMessageTarget* target = static_cast<cIGZMessageTarget*>(this);
        SL::spMS->AddNotification(target, kMessageModalWindowOpened);
        SL::spMS->AddNotification(target, kMessageModalWindowClosed);
        SL::spMS->Release();
    }

    {
        cRZSysServPtr<cIGZMessageServer2, kGZSID_MessageServer2, kGZIID_cIGZMessageServer2> messageServer;
        if (cIGZMessageServer2* server = messageServer) {
            cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
            for (std::uint32_t type : kRegisteredMessages) server->AddNotification(target, type);
        }
    }

    host->PreferencesStaticInit();
    std::uint8_t* preferences = host->PreferencesBlob();
    // The original dereferences the blob without a check (the app object has to be up before audio is).
    if (preferences[kPrefsAudioResetFlag] == 0) {
        preferences[kPrefsAudioFlags] &= static_cast<std::uint8_t>(~1u);
    } else {
        preferences[kPrefsAudioResetFlag] = 0;
        WriteU32(preferences, kPrefsMusicVolume,
                 host->VolumeFromSliderValue(kMusicSliderDefault, IAudioEventHandlerHost::kAudioTypeMusic));
        WriteU32(preferences, kPrefsEffectsVolume,
                 host->VolumeFromSliderValue(kEffectsSliderDefault, IAudioEventHandlerHost::kAudioTypeEffects));
        preferences[kPrefsAudioFlags] |= 1;
        host->SetDefaultMusicListPreferences();
    }
    host->UpdateExclusionList(IAudioEventHandlerHost::kAudioModeMayor);
    host->UpdateExclusionList(IAudioEventHandlerHost::kAudioModeRegion);

    if (radio_) {
        radio_->SetOnDemandState(true);
        radio_->TurnOn(ReadU32(preferences, kPrefsMusicEnabled) != 0);
        radio_->SetVolume(static_cast<std::int32_t>(ReadU32(preferences, kPrefsMusicVolume)));
    }
    initialized_ = true;
    return true;
}

// @ 0x1003ed8ce.
bool cSC4AudioEventHandler::Shutdown() {
    if (!initialized_) return true;

    cRZSysServPtr<cIGZSndSys, kGZSID_cGZSndSys, kGZIID_cIGZSndSys> soundSystem;
    cIGZSndSys* soundSystemPointer = soundSystem;
    if (!soundSystemPointer) return false;
    soundSystemPointer->UnregisterEventHandler(this);

    if (scapeManager_) {
        cISC4AudioScapeManager* previous = scapeManager_;
        scapeManager_ = nullptr;
        previous->Release();
    }
    if (radio_) radio_->TurnOn(false);
    radio_ = static_cast<cIGZRadio*>(nullptr);

    {
        cRZSysServPtr<cIGZMessageServer2, kGZSID_MessageServer2, kGZIID_cIGZMessageServer2> messageServer;
        if (cIGZMessageServer2* server = messageServer) {
            cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
            static constexpr std::uint32_t kRemovalOrder[] = {
                kMessageFocusChange,     kMessageRadioStation,     kMessageRadioStationOptions,
                kMessageStopPlayback,    kMessagePlaySong,         kMessageRadioOnOffVolume,
                kMessageRadioOnDemand,   kMessageListenerProperty, kMessageCityInit,
                kMessageCityShutdown,    kMessageTuningReload,     kMessagePostMayorMusic,
                kMessagePostRegionMusic,
            };
            for (std::uint32_t type : kRemovalOrder) server->RemoveNotification(target, type);
        }
    }

    if (SL::spMS) {
        SL::spMS->AddRef();
        cIGZMessageTarget* target = static_cast<cIGZMessageTarget*>(this);
        SL::spMS->RemoveNotification(target, kMessageModalWindowOpened);
        SL::spMS->RemoveNotification(target, kMessageModalWindowClosed);
        SL::spMS->Release();
    }
    initialized_ = false;
    return true;
}

cIGZWin* cSC4AudioEventHandler::lastModalWindow() const {
    return modalWindows_.empty() ? nullptr : modalWindows_.back();
}

// @ 0x1003edb82 (the cIGZMessageTarget view). Types 0x62ba7b42 and 0x62ba7b43 differ only in bit 0.
bool cSC4AudioEventHandler::DoMessage(cGZMessage& message) {
    if ((message.type & 0xfffffffeu) == kMessageModalWindowOpened) HandleModalMessages(message);
    return true;
}

// @ 0x1003edb9c. Keeps the list of modal windows and, while at least one is open, subscribes the +0x10 view to
// the mouse messages so that clicks outside the modal window play the "disabled" sound.
bool cSC4AudioEventHandler::HandleModalMessages(cGZMessage& message) {
    cIGZMessageServer* messageServer = SL::spMS;
    if (messageServer) messageServer->AddRef();

    if (message.type == kMessageModalWindowOpened) {
        if (modalWindows_.empty()) {
            if (cIGZMessageServer2* mouseServer = AudioEventHandlerHost()->MouseMessageServer())
                mouseServer->AddNotification(static_cast<cIGZMessageTarget2*>(this), gz::kMouseMessageType);
        }
        auto* window = reinterpret_cast<cIGZWin*>(message.param2);   // +0x10
        if (window) {
            window->AddRef();
            modalWindows_.push_back(window);
        }
    } else if (message.type == kMessageModalWindowClosed) {
        if (!modalWindows_.empty()) {
            cIGZWin* window = modalWindows_.back();
            modalWindows_.pop_back();
            if (window) window->Release();
        }
        if (modalWindows_.empty()) {
            if (cIGZMessageServer2* mouseServer = AudioEventHandlerHost()->MouseMessageServer())
                mouseServer->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), gz::kMouseMessageType);
        }
    }

    if (messageServer) messageServer->Release();
    return true;
}

// @ 0x1003ee2f6. A mouse-down outside the front modal window plays the disabled click through the SL::spSS cache
// (the original sends PostEventIds(1, 0xaa62c9d5, 0, 0, 0): the alias of the start-sound event).
bool cSC4AudioEventHandler::HandleMouseMessagesForModalWindow(cIGZMessage2* message) {
    cIGZWin* window = lastModalWindow();
    if (window) window->AddRef();

    gz::cIGZMouseMessage* mouse = nullptr;
    bool result = true;
    if (message && message->QueryInterface(kGZIID_cIGZMouseMessageAudio, reinterpret_cast<void**>(&mouse)) && mouse) {
        const std::uint32_t event = mouse->EventType();
        if (event == kGZMsgMouseDownL || event == kGZMsgMouseDownR || event == 9) {
            // The original dereferences the back window without a check (empty list -> crash); guard instead.
            cRZRect area;
            if (window) {
                window->GetArea(area);
                const cRZPoint position = mouse->Position();
                const bool outside = position.x < area.left || position.y < area.top || area.right < position.x ||
                                     area.bottom < position.y;
                if (outside) {
                    auto* soundSystem = static_cast<cIGZSndSys*>(SL::spSS);
                    if (!soundSystem) {
                        result = false;
                    } else {
                        soundSystem->AddRef();
                        soundSystem->PostEventIds(kSoundInstanceEventStartAlias, kSoundIdDisabledClick, 0, 0, 0);
                        soundSystem->Release();
                    }
                }
            }
        }
        mouse->Release();
    }
    if (window) window->Release();
    return result;
}

// @ 0x1003ee462. The region / mayor music request: 0x2a3ad653 with Data1 = 1 selects "krgn", Data1 = 0 "kmay".
bool cSC4AudioEventHandler::HandleRadioMessages(cIGZMessage2* message) {
    if (!message) return false;
    cIGZMessage2Standard* standard = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) || !standard)
        return false;
    bool result = false;
    if (standard->GetType() == kMessageRadioStation) {
        const std::int32_t mode = static_cast<std::int32_t>(standard->GetData1());
        std::uint32_t fade = AudioEventHandlerHost()->FadeOutTimeMs();
        if (standard->GetHasData2()) fade = static_cast<std::uint32_t>(standard->GetData2());
        result = true;   // the type matched, even when the mode selects no station
        if (mode == 1) {
            _SetRadioStation(kRegionStationCallSign, fade, true);
        } else if (mode == 0) {
            _SetRadioStation(kMayorStationCallSign, fade, true);
        }
    }
    standard->Release();
    return result;
}

// @ 0x1003ef5ae. The call that starts the region music:
//   radio->SetStation("krgn", &FadeOptions{targetVolume = 0, durationMs = fade})   (cIGZRadio vslot 0x70)
// after quitting the on-demand state, unless "krgn" is already the current station.
void cSC4AudioEventHandler::_SetRadioStation(const char* callSign, std::uint32_t fadeMs, bool fade) {
    if (!radio_) return;
    std::uint32_t current = 0;   // the original leaves this uninitialised when GetCurrentStation fails
    radio_->GetCurrentStation(current);
    radio_->SetOnDemandState(false);
    cIGZRadioStation* currentStation = radio_->GetStation(current);
    if (currentStation && currentStation == radio_->GetStation(callSign)) return;
    if (fade) {
        cIGZRadio::FadeOptions options;
        options.targetVolume = 0;
        options.durationMs = static_cast<std::int32_t>(fadeMs);
        radio_->SetStation(callSign, &options);
    } else {
        radio_->SetStation(callSign, nullptr);
    }
}

// @ 0x1003ee568. Construction sounds: one message per crew / network / occupancy change.
bool cSC4AudioEventHandler::HandleConstructionMessages(cIGZMessage2* message) {
    if (!message || !scapeManager_) return false;
    cIGZMessage2Standard* standard = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) || !standard)
        return false;

    IAudioEventHandlerHost* host = AudioEventHandlerHost();
    const std::uint32_t type = message->GetType();
    bool result = false;

    if (type == kMessageConstructionUpdate) {
        // A crew keeps moving: re-position its looping sound.
        cIGZUnknown* payload = standard->GetIGZUnknown();
        if (payload) {
            const std::uint32_t instanceId = static_cast<std::uint32_t>(standard->GetData1());
            const std::uint64_t flags = standard->GetData3();
            float position[3] = {0.0f, 0.0f, 0.0f};
            if (host->NetworkOccupantPosition(payload, position)) {
                if (scapeManager_) {
                    const std::uint32_t layer =
                        (flags & 0x2) != 0 ? kLayerConstructionNetworkB : kLayerConstructionNetworkA;
                    cS3DVector3 where{position[0], position[1], position[2]};
                    scapeManager_->UpdateInstance(layer, instanceId, where, 0.0f);
                }
                result = true;
            } else {
                result = true;   // not a network occupant: nothing to move
            }
        }
    } else if (type == kMessageConstructionNetwork) {
        // Data2 (a pointer) tells an insert from a remove.
        const std::uint32_t instanceId = static_cast<std::uint32_t>(standard->GetData1());
        const std::uint64_t flags = standard->GetData3();
        const std::uint32_t layer = (flags & 0x2) != 0 ? kLayerConstructionNetworkB : kLayerConstructionNetworkA;
        bool hasPosition = false;
        float position[3] = {0.0f, 0.0f, 0.0f};
        // Data2 is only the "there is a position" flag; the occupant itself is the message's unknown payload.
        if (standard->GetHasData2() && standard->GetVoid2() != nullptr)
            hasPosition = host->NetworkOccupantPosition(standard->GetIGZUnknown(), position);
        if (hasPosition) {
            if (scapeManager_) {
                cS3DVector3 where{position[0], position[1], position[2]};
                scapeManager_->InsertInstance(layer, instanceId, where, 0.0f);
            }
        } else if (scapeManager_) {
            scapeManager_->RemoveInstance(layer, instanceId);
        }
        result = true;
    } else if (type == kMessageConstructionOccupantInsert) {
        cIGZUnknown* payload = standard->GetIGZUnknown();
        if (!payload) {
            result = false;
        } else {
            sc4::cISC4Occupant* occupant = nullptr;
            if (!payload->QueryInterface(kGZIID_cISC4OccupantAudio, reinterpret_cast<void**>(&occupant)) || !occupant) {
                result = false;
            } else {
                float position[3] = {0.0f, 0.0f, 0.0f};
                occupant->GetPosition(position);
                if (scapeManager_) {
                    cS3DVector3 where{position[0], position[1], position[2]};
                    scapeManager_->InsertInstance(kLayerConstructionCrew, InstanceKey(occupant), where, 0.0f);
                }
                occupant->Release();
                result = true;
            }
        }
    } else if (type == kMessageConstructionOccupantToggle) {
        cIGZUnknown* payload = standard->GetIGZUnknown();
        if (!payload) {
            result = false;
        } else {
            sc4::cISC4Occupant* occupant = nullptr;
            if (!payload->QueryInterface(kGZIID_cISC4OccupantAudio, reinterpret_cast<void**>(&occupant)) || !occupant) {
                result = false;
            } else {
                result = true;
                if (!scapeManager_) {
                    // unreachable (checked at entry); the original re-checks and reports failure (quirk kept)
                    result = false;
                } else if (standard->GetHasData1()) {
                    const std::uint64_t add = standard->GetData1();
                    if (add == 0) {
                        scapeManager_->RemoveInstance(kLayerConstructionOccupancy, InstanceKey(occupant));
                    } else {
                        float position[3] = {0.0f, 0.0f, 0.0f};
                        occupant->GetPosition(position);
                        cS3DVector3 where{position[0], position[1], position[2]};
                        scapeManager_->InsertInstance(kLayerConstructionOccupancy, InstanceKey(occupant), where, 0.0f);
                    }
                }
                occupant->Release();
            }
        }
    } else {
        result = true;
    }

    standard->Release();
    return result;
}

// @ 0x1003eea44. At city init: subscribe the automata messages, remember the viewport, and ask for the mayor
// music when the city is established. At city shutdown: clear the soundscape layers and unsubscribe.
bool cSC4AudioEventHandler::HandleCityInitShutdownMessages(cIGZMessage2* message) {
    if (!message) return false;
    cIGZMessage2Standard* standard = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) || !standard)
        return false;

    bool result = false;
    if (soundSystem_) {
        const std::uint32_t type = message->GetType();
        if (type == kMessageCityInit) {
            if (SL::spMS2) {
                cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
                for (std::uint32_t messageType : kCityMessages) SL::spMS2->AddNotification(target, messageType);
            }
            if (scapeManager_) {
                result = true;
                if (SC4Audio::cIViewport* view = SC4Audio::ViewUtilities()) {
                    viewportWidth_ = view->ViewportWidth();
                    viewportHeight_ = view->ViewportHeight();
                }
                if (AudioEventHandlerHost()->CityEstablished() && SL::spMS2) {
                    auto* request = new cRZMessage2Standard();
                    request->AddRef();
                    request->SetType(kMessageRadioStation);
                    request->SetData1(0);   // mayor music
                    SL::spMS2->MessagePost(request, false);
                    request->Release();
                }
            }
        } else if (type == kMessageCityShutdown) {
            if (scapeManager_) scapeManager_->ClearAllLayers(0, false);
            if (SL::spMS2) {
                cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
                for (std::uint32_t messageType : kCityMessages) SL::spMS2->RemoveNotification(target, messageType);
            }
            result = true;
        } else {
            result = true;
        }
    }
    standard->Release();
    return result;
}

// @ 0x1003eedda. Automata sounds: one-shot positional effects (0x8a385dd8), instance add/remove (0xa9e3b6e3),
// stop (0xaa396960) and create (0xea3ada33).
bool cSC4AudioEventHandler::HandleAutomataMessages(cIGZMessage2* message) {
    if (!message || !scapeManager_) return false;
    cIGZMessage2Standard* standard = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) || !standard)
        return false;
    if (!soundSystem_) {
        standard->Release();
        return false;
    }

    const std::uint32_t type = message->GetType();
    bool result = false;

    if (type == kMessageAutomataEffect) {
        if (standard->GetData1() == kSoundIdTorchOneShot) {
            // Retrigger throttle: a fresh 1000..3999 ms delay per shot.
            if (timer_.IsTimerRunning() && timer_.GetElapsedTimeU32() < retriggerDelayMs_) {
                standard->Release();
                return true;
            }
            timer_.Restart();
            // 32-bit multiply wraps on purpose, then the signed division the binary performs.
            const std::int32_t product =
                static_cast<std::int32_t>(static_cast<std::uint32_t>(std::rand()) * 3000u);
            retriggerDelayMs_ = static_cast<std::uint32_t>(product / 0x7fff + 1000);
        }
        cISC4AutomatonAudio* automaton = nullptr;
        cIGZUnknown* payload = standard->GetIGZUnknown();
        const bool hasAutomaton =
            payload && payload->QueryInterface(kGZIID_cISC4AutomatonAudio, reinterpret_cast<void**>(&automaton)) &&
            automaton;
        if (hasAutomaton) {
            float position[3] = {0.0f, 0.0f, 0.0f};
            automaton->GetPosition(position);
            soundSystem_->PostEventIds(kSoundInstanceEventStart, static_cast<std::uint32_t>(standard->GetData1()),
                                       static_cast<std::uint32_t>(standard->GetData2()), 1, 0);
            soundSystem_->PostEventIds(kSoundInstanceEventPosition, static_cast<std::uint32_t>(standard->GetData2()),
                                       static_cast<std::uint32_t>(FloatToInt32(position[0])),
                                       static_cast<std::uint32_t>(FloatToInt32(position[1])),
                                       static_cast<std::uint32_t>(FloatToInt32(position[2])));
            automaton->Release();
        }
        result = hasAutomaton;
    } else if (type == kMessageAutomataInstance) {
        if (standard->GetIGZUnknown() && standard->GetHasData1() && standard->GetHasData2()) {
            const std::int32_t direction = static_cast<std::int32_t>(standard->GetData1());
            if (direction == 0) {
                // remove from both the "enter" and the "leave" layer
                const std::uint32_t key = InstanceKey(standard->GetIGZUnknown());
                cISC4AudioScapeManager* scape = scapeManager_;
                scape->RemoveInstance(kLayerAutomataEnter, key);
                scape->RemoveInstance(kLayerAutomataLeave, key);
                result = true;
            } else {
                const std::uint32_t layer = direction > 0 ? kLayerAutomataEnter : kLayerAutomataLeave;
                sc4::cISC4Occupant* occupant = nullptr;
                if (standard->GetIGZUnknown()->QueryInterface(kGZIID_cISC4OccupantAudio,
                                                              reinterpret_cast<void**>(&occupant)) &&
                    occupant) {
                    float position[3] = {0.0f, 0.0f, 0.0f};
                    occupant->GetPosition(position);
                    const std::uint32_t key = InstanceKey(standard->GetIGZUnknown());
                    if (scapeManager_) {
                        cS3DVector3 where{position[0], position[1], position[2]};
                        scapeManager_->InsertInstance(layer, key, where, kAutomataInstanceRadius);
                    }
                    occupant->Release();
                    result = true;
                }
            }
        }
    } else if (type == kMessageAutomataStop) {
        soundSystem_->PostEventIds(kSoundInstanceEventStop, static_cast<std::uint32_t>(standard->GetData2()), 0, 0, 0);
        result = true;
    } else if (type == kMessageAutomataCreate) {
        cIGZUnknown* payload = standard->GetIGZUnknown();
        float position[3] = {0.0f, 0.0f, 0.0f};
        if (payload) {
            sc4::cISC4Occupant* occupant = nullptr;
            if (payload->QueryInterface(kGZIID_cISC4OccupantAudio, reinterpret_cast<void**>(&occupant)) && occupant) {
                occupant->GetPosition(position);
                occupant->Release();
            }
        }
        const std::int32_t selector =
            standard->GetHasData1() ? static_cast<std::int32_t>(standard->GetData1()) : -1;
        std::uint32_t layer = kLayerAutomataDefault;
        if (standard->GetHasData2()) {
            switch (static_cast<std::int32_t>(standard->GetData2())) {
                case 3: layer = selector != 0 ? kLayerAutomata3b : kLayerAutomata3; break;
                case 2: layer = selector != 0 ? kLayerAutomata2b : kLayerAutomata2; break;
                case 1: layer = selector != 0 ? kLayerAutomata1b : kLayerAutomata1; break;
                default: break;   // keep the default layer
            }
        }
        if (scapeManager_) {
            cS3DVector3 where{position[0], position[1], position[2]};
            // SFX::SoundManager() is also evaluated in the original; GenerateNewInstanceId is static.
            const std::uint32_t instanceId = cSC4AudioSoundManager::GenerateNewInstanceId();
            scapeManager_->InsertInstance(layer, instanceId, where, kAutomataInstanceRadius);
        }
        result = true;
    } else {
        result = true;
    }

    standard->Release();
    return result;
}

// @ 0x1003ef31e. Dispatch sounds: a start event carrying the dispatch sound id and the dispatch object's key.
bool cSC4AudioEventHandler::HandleDispatchManagerMessages(cIGZMessage2* message) {
    if (!message) return false;
    cIGZMessage2Standard* standard = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) || !standard)
        return false;

    bool result = false;
    if (soundSystem_ && standard->GetHasData1()) {
        const std::uint32_t dispatchType = static_cast<std::uint32_t>(standard->GetData1());
        cIGZUnknown* payload = standard->GetIGZUnknown();
        const std::uint32_t type = message->GetType();
        if (type == kMessageDispatchFire || type == kMessageDispatchPolice) {
            result = true;
            if (dispatchType < 7) {
                const std::uint32_t soundId =
                    type == kMessageDispatchFire ? kDispatchSoundFire : kDispatchSoundPolice;
                if (((1u << dispatchType) & kDispatchTypeSoundMask) != 0 && payload) {
                    void* event = nullptr;
                    if (soundSystem_->EventCreateWithData(kSoundInstanceEventStart, kGZCLSID_cGZSndEventStd, &event,
                                                          soundId, InstanceKey(payload), 0, 0) &&
                        event) {
                        auto* soundEvent = static_cast<cIGZSndEventStd*>(event);
                        soundSystem_->PostEvent(soundEvent);
                        soundEvent->Release();
                    }
                }
            }
        } else if (type == kMessageDispatchPylonFire || type == kMessageDispatchPylonPolice) {
            if (payload) {
                payload->AddRef();
                void* pylon = nullptr;
                result = payload->QueryInterface(kPylonOccupantIid, &pylon);
                if (pylon) static_cast<cIGZUnknown*>(pylon)->Release();
                payload->Release();
            }
        } else {
            result = true;
        }
    }
    standard->Release();
    return result;
}

// @ 0x1003ef652. The sound-event receiver. Only the torch tool and two bookkeeping events are handled; every
// handled event answers false (the original returns 1 only when the sound system is missing).
bool cSC4AudioEventHandler::DoEvent(cIGZSndEventStd* event) {
    if (!soundSystem_) return true;
    if (!event) return false;
    const std::uint32_t id = event->GetEventId();
    switch (id) {
        case kEventListenerTuningChanged: {
            if (auto* soundSystem = static_cast<cIGZSndSys*>(RZ::SS()))
                soundSystem->EnumerateTargets(&EnumFunc_UpdateMinMaxDistances, 0, nullptr);
            return false;
        }
        case kEventSoundRegisterValue: {
            cIGZMessage2Standard* standard = event->AsMessage2Standard();
            if (standard) {
                const std::uint32_t value = static_cast<std::uint32_t>(standard->GetData1());
                if (auto* soundSystem = static_cast<cIGZSndSys*>(RZ::SS())) {
                    cIGZSndPropertyCache* cache = nullptr;
                    if (soundSystem->GetSndPropertyCacheByIid(&cache) && cache) {
                        cache->SetGlobalPropertyUint32(kSndPropertyGlobalVolume, value);
                        cache->Release();
                    }
                }
            }
            return false;
        }
        case kEventTorchStop:
        case kEventTorchMouseDown:
        case kEventTorchMouseUp:
        case kEventTorchStopLoop:
        case kEventTorchMove:
        case kEventTorchFlare:
            HandleTorchControlMessages(event);
            return false;
        default:
            return false;
    }
}

// @ 0x1003ef790. The burn-tool (torch) sound: three looping sounds on instance keys 0x600..0x603 whose volumes
// follow the cursor speed; the flare toggles two extra layers. The last position / time / velocity are
// process-wide statics in the original.
bool cSC4AudioEventHandler::HandleTorchControlMessages(cIGZSndEventStd* event) {
    if (!scapeManager_) return false;
    cIGZUnknown* soundSystemRaw = RZ::SS();
    if (!soundSystemRaw) return false;
    soundSystemRaw->AddRef();

    if (event) {
        cIGZMessage2Standard* standard = event->AsMessage2Standard();
        if (standard) {
            const std::int32_t x = static_cast<std::int32_t>(standard->GetData1());
            const std::int32_t y = static_cast<std::int32_t>(standard->GetData2());
            const std::int32_t time = static_cast<std::int32_t>(standard->GetData3());
            const std::int32_t delta = time - g_torchLastTime;
            float speed = 0.0f;
            if (delta != 0) {
                const float dx = static_cast<float>(x) - static_cast<float>(g_torchLastX);
                const float dy = static_cast<float>(y) - static_cast<float>(g_torchLastY);
                speed = std::sqrt(dx * dx + dy * dy) / (static_cast<float>(delta) / 100.0f);
            }
            const float velocity = std::min(60.0f, speed) + g_torchLastVelocity;
            const float scaled = velocity * 0.5f;
            const std::int32_t volume = FloatToInt32(scaled * 424.0f / 60.0f);

            cIGZSndSys* soundSystem = static_cast<cIGZSndSys*>(soundSystemRaw);
            const std::uint32_t id = event->GetEventId();
            if (id == kEventTorchStop) {
                soundSystem->PostEventIds(kSoundInstanceEventStop, 0x601, 0, 0, 0);
                soundSystem->PostEventIds(kSoundInstanceEventStop, 0x602, 0, 0, 0);
                soundSystem->PostEventIds(kSoundInstanceEventStop, 0x603, 0, 0, 0);
            } else if (id == kEventTorchMouseDown) {
                g_torchMousePressed = 1;
            } else if (id == kEventTorchFlare) {
                const std::uint64_t flare = standard->GetData1();
                if (flare == 0) {
                    if ((g_torchFlared & 1) != 0) {
                        g_torchFlared = 0;
                        soundSystem->PostEventIds(kSoundInstanceEventStop, 0x602, 0, 0, 0);
                        soundSystem->PostEventIds(kSoundInstanceEventStop, 0x603, 0, 0, 0);
                        soundSystem->PostEventIds(kSoundInstanceEventStart, kSoundIdTorchStop, 0x601, 0, 0);
                        soundSystem->PostEventIds(kSoundInstanceEventVolume, 0x601, 0x400, 0, 0);
                    }
                } else if ((g_torchFlared & 1) == 0) {
                    g_torchFlared = 1;
                    soundSystem->PostEventIds(kSoundInstanceEventStart, kSoundIdTorchLoopA, 0x600, 0, 0);
                    soundSystem->PostEventIds(kSoundInstanceEventVolume, 0x600, 0x400, 0, 0);
                    soundSystem->PostEventIds(kSoundInstanceEventStart, kSoundIdTorchLoopB, 0x602, 0, 0);
                    soundSystem->PostEventIds(kSoundInstanceEventVolume, 0x602,
                                              static_cast<std::uint32_t>(volume + 600), 0, 0);
                    soundSystem->PostEventIds(kSoundInstanceEventStart, kSoundIdTorchResponse, 0x603, 0, 0);
                    soundSystem->PostEventIds(kSoundInstanceEventVolume, 0x603,
                                              static_cast<std::uint32_t>(0x400 - volume), 0, 0);
                    soundSystem->PostEventIds(kSoundInstanceEventStop, 0x601, 0, 0, 0);
                }
            } else if (id == kEventTorchMouseUp) {
                g_torchMousePressed = 0;
            } else if (id == kEventTorchStopLoop) {
                soundSystem->PostEventIds(kSoundInstanceEventStart, kSoundIdTorchStop, 0x601, 0, 0);
                soundSystem->PostEventIds(kSoundInstanceEventVolume, 0x601, 0x400, 0, 0);
            } else if (id == kEventTorchMove && g_torchMousePressed == 1) {
                soundSystem->PostEventIds(kSoundInstanceEventVolume, 0x602,
                                          static_cast<std::uint32_t>(volume + 600), 0, 0);
                soundSystem->PostEventIds(kSoundInstanceEventVolume, 0x603,
                                          static_cast<std::uint32_t>(volume + 600), 0, 0);
            }

            g_torchLastX = x;
            g_torchLastY = y;
            g_torchLastTime = time;
            g_torchLastVelocity = scaled;
        }
    }

    soundSystemRaw->Release();
    return false;
}

// @ 0x1003eddf0 (the cIGZMessageTarget2 view).
bool cSC4AudioEventHandler::DoMessage(cIGZMessage2* message) {
    if (!message) return false;
    const std::uint32_t type = message->GetType();

    // Payload accessors live on the cIGZMessage2Standard view of the same object.
    cIGZMessage2Standard* standard = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard))) standard = nullptr;

    // SC4_AUDIODEBUG: the radio's messages as they arrive (station, playlist options, stop, play, on / off + volume, on demand).
    if (standard && std::getenv("SC4_AUDIODEBUG") &&
        (type == kMessageRadioStation || type == kMessageRadioStationOptions || type == kMessageStopPlayback || type == kMessagePlaySong ||
         type == kMessageRadioOnOffVolume || type == kMessageRadioOnDemand))
        std::fprintf(stderr, "audio handler: radio message %08x data %llu %llu %llu\n", type, static_cast<unsigned long long>(standard->GetData1()),
                     static_cast<unsigned long long>(standard->GetData2()), static_cast<unsigned long long>(standard->GetData3()));

    bool result = true;
    switch (type) {
        case kMessageRadioStation:
            HandleRadioMessages(message);
            break;
        case kMessageRadioStationOptions: {
            // Data1 selects the station, Data2 the music exclusion list, Data3 the playlist mode (quirk: the values
            // are read from the standard view, which the original skips).
            if (standard) {
                const std::int32_t mode = static_cast<std::int32_t>(standard->GetData1());
                const char* callSign = nullptr;
                if (mode == 1) {
                    callSign = kRegionStationCallSign;
                } else if (mode == 0) {
                    callSign = kMayorStationCallSign;
                }
                std::int32_t playlistMode = 2;
                if (standard->GetHasData3()) playlistMode = standard->GetData3() == 0 ? 2 : 1;
                if (radio_) {
                    if (cIGZRadioStation* station = radio_->GetStation(callSign)) {
                        station->SetModifierList(static_cast<std::list<cRZString>*>(standard->GetVoid2()));
                        station->SetPlaylistMode(playlistMode);
                    }
                }
            }
            break;
        }
        case kMessageStopPlayback:
            if (radio_) radio_->StopPlayback();
            break;
        case kMessagePlaySong:
            if (standard) {
                auto* song = static_cast<cRZString*>(standard->GetVoid1());
                if (song) {
                    if (radio_) radio_->PlaySong(*song);
                    song->Release();
                }
            }
            break;
        case kMessageRadioOnOffVolume:
            if (radio_) {
                if (standard && standard->GetHasData2()) {
                    radio_->SetVolume(static_cast<std::int32_t>(standard->GetData2()));
                }
                if (standard && standard->GetHasData1()) {
                    const std::uint64_t requested = standard->GetData1();
                    const bool on = radio_->IsOn();
                    if (requested == 1) {
                        if (on) break;
                    } else if (!on) {
                        break;
                    }
                    radio_->TurnOn(requested == 1);
                    if (requested == 1) radio_->PlayNextSong(false);
                }
            }
            break;
        case kMessageRadioOnDemand:
            if (radio_ && standard) radio_->SetOnDemandState(standard->GetData1() != 0);
            break;
        case kMessageFocusChange: {
            const std::uint8_t* preferences = AudioEventHandlerHost()->PreferencesBlob();
            if (preferences && ReadU32(preferences, kPrefsMusicEnabled) != 0 && standard && radio_)
                radio_->TurnOn(standard->GetData1() != 0);
            break;
        }
        case kMessagePostMayorMusic:
        case kMessagePostRegionMusic:
            if (SL::spMS2) {
                auto* request = new cRZMessage2Standard();
                request->AddRef();
                request->SetType(kMessageRadioStation);
                request->SetData1(type == kMessagePostRegionMusic ? 1 : 0);
                SL::spMS2->MessagePost(request, false);
                request->Release();
            }
            break;
        case kMessageListenerProperty:
            if (standard) {
                const std::uint32_t value = static_cast<std::uint32_t>(standard->GetData1());
                if (auto* soundSystem = static_cast<cIGZSndSys*>(RZ::SS()))
                    soundSystem->DoEventIds(kEventSoundRegisterValue, value, 0, 0, 0);
            }
            break;
        case gz::kMouseMessageType:
            result = HandleMouseMessagesForModalWindow(message);
            break;
        case kMessageCityInit:
        case kMessageCityShutdown:
            HandleCityInitShutdownMessages(message);
            break;
        case kMessageConstructionNetwork:
        case kMessageConstructionUpdate:
        case kMessageConstructionOccupantInsert:
        case kMessageConstructionOccupantToggle:
            HandleConstructionMessages(message);
            break;
        case kMessageDispatchFire:
        case kMessageDispatchPolice:
        case kMessageDispatchPylonFire:
        case kMessageDispatchPylonPolice:
            HandleDispatchManagerMessages(message);
            break;
        case kMessageAutomataEffect:
        case kMessageAutomataInstance:
        case kMessageAutomataStop:
        case kMessageAutomataCreate:
        case kMessageAutomataOther:
            HandleAutomataMessages(message);
            break;
        default:
            break;   // 0xc53d10aa and every unregistered type are consumed without an action
    }

    if (standard) standard->Release();
    return result;
}

// =============================================================================================
// cSC4AudioEventHandlerDisasterFire
// =============================================================================================

cSC4AudioEventHandlerDisasterFire::cSC4AudioEventHandlerDisasterFire() = default;

// @ 0x1003ec83a. Releases the soundscape manager, then the cSC4OccupantFilter base.
cSC4AudioEventHandlerDisasterFire::~cSC4AudioEventHandlerDisasterFire() {
    if (scapeManager_) {
        scapeManager_->Release();
        scapeManager_ = nullptr;
    }
}

// @ 0x1003dedea: returns the +8 (cIGZInitShutdown) view.
cIGZInitShutdown* cSC4AudioEventHandlerDisasterFire::Create() {
    return static_cast<cIGZInitShutdown*>(new cSC4AudioEventHandlerDisasterFire());
}

// @ 0x1003ed02c. Quirk kept: iid 1 and 0x89e51472 answer the +8 view.
bool cSC4AudioEventHandlerDisasterFire::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZInitShutdown) {
        *out = static_cast<cIGZInitShutdown*>(this);
    } else if (iid == kGZIID_cSC4AudioEventHandler) {
        *out = static_cast<cIGZSndEventHandler*>(this);
    } else if (iid == sc4::kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// @ 0x1003ecf90.
std::uint32_t cSC4AudioEventHandlerDisasterFire::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// @ 0x1003ec91e. Subscribes the occupant and city messages, holds the soundscape manager, creates the "Fires"
// layer and registers its three distance-banded sounds.
bool cSC4AudioEventHandlerDisasterFire::Init() {
    if (initialized_) return true;

    if (!SL::spMS2) return false;
    cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
    SL::spMS2->AddNotification(target, 0x99ef1142);   // occupant inserted
    SL::spMS2->AddNotification(target, 0x99ef1143);   // occupant removed
    SL::spMS2->AddNotification(target, kMessageCityInit);

    cRZSysServPtr<cISC4AudioScapeManager, kGZSID_cISC4AudioScapeManager, kGZIID_cISC4AudioScapeManager> scapeManager;
    cISC4AudioScapeManager* scapePointer = scapeManager;
    if (scapeManager_ != scapePointer) {
        if (scapePointer) scapePointer->AddRef();
        cISC4AudioScapeManager* previous = scapeManager_;
        scapeManager_ = scapePointer;
        if (previous) previous->Release();
    }
    if (!scapeManager_) return false;

    const cRZString name(kDisasterFireLayerName);
    if (!scapeManager_->CreateLayer(name, kLayerDisasterFire, 0, 50.0f)) return false;
    scapeManager_->RegisterLayerSound(kLayerDisasterFire, kDisasterFireSoundSmall, true, 0, 1, 9);
    scapeManager_->RegisterLayerSound(kLayerDisasterFire, kDisasterFireSoundMedium, true, 0, 10, 0x31);
    scapeManager_->RegisterLayerSound(kLayerDisasterFire, kDisasterFireSoundLarge, true, 0, 0x32, 0x7fffffff);
    initialized_ = true;
    return true;
}

// @ 0x1003ecb84. Always reports success.
bool cSC4AudioEventHandlerDisasterFire::Shutdown() {
    if (initialized_) {
        if (SL::spMS2) {
            cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
            SL::spMS2->RemoveNotification(target, kMessageCityInit);
            SL::spMS2->RemoveNotification(target, 0x99ef1142);
            SL::spMS2->RemoveNotification(target, 0x99ef1143);
        }
        if (scapeManager_) {
            cISC4AudioScapeManager* previous = scapeManager_;
            scapeManager_ = nullptr;
            previous->Release();
        }
        initialized_ = false;
    }
    return true;
}

// @ 0x1003ece84. A fire occupant is one that answers the cISC4FireOccupant interface (0x69d25c60).
bool cSC4AudioEventHandlerDisasterFire::IsOccupantIncluded(sc4::cISC4Occupant* occupant) {
    if (!occupant) return false;
    void* fire = nullptr;
    const bool included = occupant->QueryInterface(kGZIID_cISC4FireOccupantAudio, &fire);
    if (fire) static_cast<cIGZUnknown*>(fire)->Release();
    return included;
}

// @ 0x1003ecc60.
bool cSC4AudioEventHandlerDisasterFire::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    cIGZMessage2Standard* standard = nullptr;
    if (message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) && standard) {
        cIGZUnknown* payload = standard->GetIGZUnknown();
        if (payload) payload->AddRef();
        const std::uint32_t type = message->GetType();
        if (type == 0x99ef1142 || type == 0x99ef1143) {
            if (payload) {
                sc4::cISC4Occupant* occupant = nullptr;
                if (payload->QueryInterface(kGZIID_cISC4OccupantAudio, reinterpret_cast<void**>(&occupant)) &&
                    occupant && IsOccupantIncluded(occupant)) {
                    if (type == 0x99ef1142) {
                        IterateFireOccupantCallback(occupant);
                    } else if (scapeManager_) {
                        scapeManager_->RemoveInstance(kLayerDisasterFire, InstanceKey(occupant));
                    }
                }
                if (occupant) occupant->Release();
            }
        } else if (type == kMessageCityInit) {
            AudioEventHandlerHost()->IterateOccupants(&IterateFireOccupantsOnInitCallback, this,
                                                      static_cast<sc4::cISC4OccupantFilter*>(this));
        }
        if (payload) payload->Release();
        standard->Release();
    }
    return true;
}

// @ 0x1003ecef0.
void cSC4AudioEventHandlerDisasterFire::IterateFireOccupantCallback(sc4::cISC4Occupant* occupant) {
    if (!occupant) return;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);
    if (scapeManager_) {
        cS3DVector3 where{position[0], position[1], position[2]};
        scapeManager_->InsertInstance(kLayerDisasterFire, InstanceKey(occupant), where, 0.0f);
    }
}

// @ 0x1003ece2a (free function in the original): one reference around the callback so the handler cannot be
// destroyed while the occupant manager walks the city.
bool cSC4AudioEventHandlerDisasterFire::IterateFireOccupantsOnInitCallback(sc4::cISC4Occupant* occupant,
                                                                           void* context) {
    if (!occupant || !context) return false;
    auto* self = static_cast<cSC4AudioEventHandlerDisasterFire*>(context);
    self->AddRef();
    self->IterateFireOccupantCallback(occupant);
    self->Release();
    return true;
}
