#include "audio/HitTrackPlayer.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "audio/HitClassIds.h"
#include "gz/cGZAny.h"
#include "gz/cIGZPersistResource.h"

namespace {

using nsHitReg::GetPropertyIdFromLegacyRegisterId;

constexpr std::uint32_t kIidTrackPlayer = 0xbb1be367;
constexpr std::uint32_t kIidSnd = 0x206077d2;
constexpr std::uint32_t kIidSnd3D = 0x306077d2;
constexpr std::uint32_t kIidUpdateTarget = 0x5b34f0af;
constexpr std::uint32_t kIidRegisterSet = 0xfd584467;
constexpr std::uint32_t kIidViewTarget = 0xaa11a64f;
constexpr std::uint32_t kIidMultipleSources = 0xdd583e2d;
constexpr std::uint32_t kIidCompletion = 0xe9d00a9a;
constexpr std::uint32_t kIidHitResource = 0x456b8f1d;
constexpr std::uint32_t kIidHitListIface = 0xdb1acfb2;

// Property ids used by the player itself (names from their behaviour in PlayPause/HandleTimer).
constexpr std::uint32_t kRegFadeActive = 0xff0ad592;     // a fade is running
constexpr std::uint32_t kRegFadeTarget = 0xff47f685;     // register id being faded
constexpr std::uint32_t kRegFadeEnd = 0xff2d16f2;        // value the fade ends at
constexpr std::uint32_t kRegFadeRate = 0xffd1c82f;       // change per second
constexpr std::uint32_t kRegFadeInMs = 0xffff4e79;       // fade-in time (PlayPause)
constexpr std::uint32_t kRegFadeOutMs = 0xff595ffd;      // fade-out time (Stop)
constexpr std::uint32_t kRegVolumeScale = 0xff53dbf3;
constexpr std::uint32_t kRegArg0 = 0xff141c48, kRegArg1 = 0xff9250b3, kRegArg2 = 0xff9ec945, kRegArg3 = 0xff1885be;
constexpr std::uint32_t kRegArgMode = 0xff4dcd0d;        // how Play's arguments map onto registers
constexpr std::uint32_t kRegGameObject = 0xffb4335c;
constexpr std::uint32_t kRegPan = 0xff3ec880, kRegVolume = 0xff90aac8;
constexpr std::uint32_t kRegPosX = 0xffcc4d2d, kRegPosY = 0xff4a01d6, kRegPosZ = 0xff469820;
constexpr std::uint32_t kRegPatchId = 0xffaec905;        // sound id of the note
constexpr std::uint32_t kRegRecursionGuard = 0xff6f9546;
constexpr std::uint32_t kRegSemId = 0xff891eaa;
constexpr std::uint32_t kRegEntryPoint = 0xff5f15e1;
constexpr std::uint32_t kRegHitListId = 0xff3c2160;
constexpr std::uint32_t kRegStartDelay = 0xff0246fe;
constexpr std::uint32_t kRegPlayingFlag = 0xff245400;
constexpr std::uint32_t kRegControlGroup = 0xffb0333d;
constexpr std::uint32_t kRegDependentSound = 0xff59fdd8;
constexpr std::uint32_t kRegDuckPriority = 0xffae9ddb;
constexpr std::uint32_t kRegLoopFlag = 0xfff7b8df;
constexpr std::uint32_t kRegPitchMultiplier = 0xff3071b9;
constexpr std::uint32_t kRegTimeMultiplier = 0xff86fe79;
constexpr std::uint32_t kRegParentLink = 0xff4c97be;
constexpr std::uint32_t kRegFrequency = 0xff099db2;
constexpr std::uint32_t kRegSemitones = 0xff8dcfd8, kRegCents = 0xff69510b;  // pitch offsets (semitone-like units)
constexpr double kSemitoneRatio = 1.0594630943592953;  // 0x3ff0f38f92d97963

std::int32_t g_numSoundObjects = 0;

std::uint32_t TranslateLegacy(std::int32_t id) {
    const auto uid = static_cast<std::uint32_t>(id);
    if (uid < 0x100) {
        const std::uint32_t mapped = GetPropertyIdFromLegacyRegisterId(uid);
        if (mapped != 0) return mapped;
    }
    return uid;
}

// EnumFunc_KillTrack @ 0x10047ac30 / EnumFunc_StopTrack @ 0x10047ad1b: for every update target that
// is a track player whose track definition has instance `id`: kill/stop it and stop enumerating.
bool EnumKillStop(cIGZSndUpdateTarget* target, std::uint32_t id, bool kill) {
    void* p = nullptr;
    if (!target->QueryInterface(kIidTrackPlayer, &p) || p == nullptr) return true;
    auto* player = static_cast<cIHitTrackPlayer*>(p);
    bool matched = false;
    void* res = nullptr;
    if (player->GetTrackDef(kIidHitResource, &res) && res != nullptr) {
        auto* resource = static_cast<cIHitPersistResource*>(res);
        if (resource->GetInstance() == id) {
            cIGZSnd* snd = player->AsIGZSnd();
            if (kill) snd->Kill(); else snd->Stop();
            matched = true;
        }
    }
    player->Release();
    return !matched;
}
bool EnumFuncKillTrack(cIGZSndUpdateTarget* t, std::uint32_t id, void*) { return EnumKillStop(t, id, true); }
bool EnumFuncStopTrack(cIGZSndUpdateTarget* t, std::uint32_t id, void*) { return EnumKillStop(t, id, false); }

}  // namespace

// ---- construction -------------------------------------------------------------------------------

cHitTrackPlayer::cHitTrackPlayer() {
    layers_.resize(10);
    layers_[0] = new cGZPropertySet();  // the working register layer (created lazily by the binary)
    ++g_numSoundObjects;
}

cHitTrackPlayer::~cHitTrackPlayer() {
    --g_numSoundObjects;
    if (completion_) completion_ = nullptr;
}

std::int32_t cHitTrackPlayer::NumSoundObjects() { return g_numSoundObjects; }

cIHitMan* cHitTrackPlayer::HitMan() const { return RZ::HitManInstance(); }

void cHitTrackPlayer::ReportError(const char* message) {
    if (cIHitMan* hm = HitMan()) hm->HandleTrackFlowError(const_cast<cHitTrackPlayer*>(this), message);
}

bool cHitTrackPlayer::QueryInterface(std::uint32_t iid, void** out) {
    switch (iid) {
        case kGZIID_cIGZUnknown:
        case kIidTrackPlayer: *out = static_cast<cIHitTrackPlayer*>(this); break;
        case kIidRegisterSet: *out = static_cast<cIGZSndRegisterSet*>(this); break;
        case kIidSnd3D: *out = static_cast<cIGZSnd3D*>(this); break;
        case kIidSnd: *out = static_cast<cIGZSnd*>(this); break;
        case kIidViewTarget: *out = static_cast<cIGZSndViewUpdateTarget*>(this); break;
        case kIidUpdateTarget: *out = static_cast<cIGZSndUpdateTarget*>(this); break;
        case kIidMultipleSources: *out = static_cast<cIGZSndMultipleSources*>(this); break;
        case kIidCompletion: *out = static_cast<cIGZSndCompletionNotification*>(this); break;
        default: return false;
    }
    AddRef();
    return true;
}

// Destroys the object when the count is below 2 (deleting destructor, vtable slot 21).
std::uint32_t cHitTrackPlayer::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return static_cast<std::uint32_t>(--refCount_);
}

bool cHitTrackPlayer::Shutdown() {  // @ 0x10047b982
    if (channelSnd_) channelSnd_->Shutdown();
    channelRegs_ = nullptr;
    channelSnd_ = nullptr;
    return true;
}

// ---- registers ---------------------------------------------------------------------------------------

std::int32_t cHitTrackPlayer::GetRegisterValue(std::int32_t id) const {  // @ 0x10047fd08
    const std::uint32_t key = TranslateLegacy(id);
    AnyUint32 any;
    for (const auto& layer : layers_) {
        if (layer && layer->Value(key, &any)) return static_cast<std::int32_t>(any.get());
    }
    return 0;
}

float cHitTrackPlayer::GetRegisterFloat(std::uint32_t id) const {  // @ 0x10047fdb4 (no legacy translation)
    AnyFloat32 any;
    for (const auto& layer : layers_) {
        if (layer && layer->Value(id, &any)) return any.get();
    }
    return 0.0f;
}

// Only the working layer and the track definition are consulted.
bool cHitTrackPlayer::HasRegisterValue(std::int32_t id) const {  // @ 0x10047fe56
    const std::uint32_t key = TranslateLegacy(id);
    if (layers_[0] && layers_[0]->HasValue(key)) return true;
    return layers_[1] && layers_[1]->HasValue(key);
}

// Writes the working layer, copies the value to dependent sounds that do not define the register
// themselves, then (unless suppressed) reacts to the change.
bool cHitTrackPlayer::SetRegisterValue(std::int32_t id, std::int32_t value, bool suppress) {  // @ 0x10047feb8
    const std::uint32_t key = TranslateLegacy(id);
    SetFlags(value);
    cRZAutoRefCount<cIGZPropertySet> own = layers_[0];  // keep the layer alive across callbacks
    own->SetValue(key, AnyUint32(static_cast<std::uint32_t>(value)));
    for (auto& child : children_) {
        void* p = nullptr;
        if (!child->QueryInterface(kIidTrackPlayer, &p) || p == nullptr) continue;
        auto* player = static_cast<cIHitTrackPlayer*>(p);
        void* def = nullptr;
        player->GetTrackDef(kGZIID_cIGZPropertySet, &def);
        if (def != nullptr) {
            auto* defSet = static_cast<cIGZPropertySet*>(def);
            if (!defSet->HasValue(key)) player->AsIGZSndRegisterSet()->SetRegisterValue(static_cast<std::int32_t>(key), value, suppress);
            defSet->Release();
        }
        player->Release();
    }
    return suppress ? true : HandleNotifySetPropertyValue(static_cast<std::int32_t>(key));
}

bool cHitTrackPlayer::SetRegisterFloat(std::uint32_t id, float value, bool suppress) {  // @ 0x1004800e4
    flagGreater_ = 0.0f < value;
    flagEqual_ = value == 0.0f;
    flagLess_ = value < 0.0f;
    cRZAutoRefCount<cIGZPropertySet> own = layers_[0];
    own->SetValue(id, AnyFloat32(value));
    const auto asInt = static_cast<std::int32_t>(value);  // dependents receive the truncated value
    for (auto& child : children_) {
        void* p = nullptr;
        if (!child->QueryInterface(kIidTrackPlayer, &p) || p == nullptr) continue;
        auto* player = static_cast<cIHitTrackPlayer*>(p);
        void* def = nullptr;
        player->GetTrackDef(kGZIID_cIGZPropertySet, &def);
        if (def != nullptr) {
            auto* defSet = static_cast<cIGZPropertySet*>(def);
            if (!defSet->HasValue(id)) player->AsIGZSndRegisterSet()->SetRegisterValue(static_cast<std::int32_t>(id), asInt, suppress);
            defSet->Release();
        }
        player->Release();
    }
    return suppress ? true : HandleNotifySetPropertyValue(static_cast<std::int32_t>(id));
}

void cHitTrackPlayer::ClearRegisterValue(std::int32_t id, bool suppress) {  // @ 0x100480302
    const std::uint32_t key = TranslateLegacy(id);
    for (auto& child : children_) {
        void* p = nullptr;
        if (child->QueryInterface(kIidTrackPlayer, &p) && p != nullptr) {
            auto* player = static_cast<cIHitTrackPlayer*>(p);
            player->AsIGZSndRegisterSet()->ClearRegisterValue(static_cast<std::int32_t>(key), false);
            player->Release();
        }
    }
    if (!suppress) HandleNotifySetPropertyValue(static_cast<std::int32_t>(key));
}

void cHitTrackPlayer::SemRefresh() {  // the "(+0xe0 && +0xf8) sem->vtbl[0x28](this+8)" idiom
    if (channelRegs_ && sem_) sem_->UpdateVolPan(this, channelRegs_.get());
}

// HandleNotifySetPropertyValue @ 0x1004803f0: forwards register changes to the channel, refreshes
// volume/pan through the sem, or re-patches. Returns false only when SetTrackDataEntryPoint fails.
bool cHitTrackPlayer::HandleNotifySetPropertyValue(std::int32_t id) {
    const std::int32_t value = GetRegisterValue(id);
    switch (static_cast<std::uint32_t>(id)) {
        case 0xff099db2: UpdatePitch(); return true;        // frequency
        case 0xff2d16f2:                                    // fade end value: cancels "fading out"
            if (value == 0) return true;
            fadingOut_ = false;
            return true;
        case 0xff5f15e1: return SetTrackDataEntryPoint(static_cast<std::uint32_t>(value));  // entry point
        case 0xffaec905: UpdatePatch(); return true;        // sound id
        case 0xffb36659:                                    // pause switch of the channel
            if (!channelRegs_) return true;
            if (GetRegisterValue(0xffb36659) != 0) channelSnd_->Pause(false); else channelSnd_->Unpause();
            return true;
        case 0xffc6e666: {                                  // target sound: register writes go there
            targetRegs_ = nullptr;
            if (value == 0) return true;
            for (auto& child : children_) {
                if (child->GetID() == static_cast<std::uint32_t>(value)) {
                    void* p = nullptr;
                    // The binary asks for iid 0xebb10cdd (a property set interface), which the
                    // sounds never expose, so this reports an error — kept as is.
                    if (child->QueryInterface(kGZIID_cIGZPropertySet, &p) && p != nullptr) {
                        targetRegs_ = static_cast<cIGZSndRegisterSet*>(p);
                        static_cast<cIGZUnknown*>(p)->Release();
                    } else {
                        ReportError("Attempt to set target to object with no register interface");
                    }
                    return true;
                }
            }
            return true;
        }
        // float registers forwarded to the channel
        case 0xff3071b9: case 0xff0150f2: case 0xff734343: case 0xff86fe79: case 0xffd3d39b:
            if (channelRegs_) channelRegs_->SetRegisterFloat(static_cast<std::uint32_t>(id), GetRegisterFloat(static_cast<std::uint32_t>(id)), false);
            return true;
        // integer registers forwarded to the channel
        case 0xff050a94: case 0xff595ffd: case 0xff7e5922: case 0xffa42951: case 0xffa9b525:
        case 0xffbdb196: case 0xfff3edf2: case 0xfff7b8df: case 0xffff4e79:
            if (channelRegs_) channelRegs_->SetRegisterValue(id, value, false);
            return true;
        // registers that change volume/pan/position
        case 0xff3ec880: case 0xff40ff96: case 0xff444c9a: case 0xff469820: case 0xff4a01d6:
        case 0xff4c6660: case 0xff53dbf3: case 0xff90aac8: case 0xffa6a614: case 0xffaab623:
        case 0xffc20061: case 0xffc6b36d: case 0xffcc3b29: case 0xffcc4d2d: case 0xffce9997:
            SemRefresh();
            return true;
        default: return true;
    }
}

// ---- track definition -----------------------------------------------------------------------------------

bool cHitTrackPlayer::GetTrackDef(std::uint32_t iid, void** out) {  // @ 0x10047ed34
    return layers_[1] && layers_[1]->QueryInterface(iid, out);
}

// SetTrackDef @ 0x10047ed56: clears the working layer, installs `trackDef` as layer 1 and follows
// its parent chain (register 0xff4c97be -> instance of a track property set in group 0xfc45caf7)
// into layers 2.., then applies entry point and sem.
bool cHitTrackPlayer::SetTrackDef(cIGZPropertySet* trackDef) {
    if (!layers_[0]) layers_[0] = new cGZPropertySet(); else layers_[0]->ClearAll();
    layers_[1] = trackDef;

    Hit::cIHitManServices* services = Services();
    cRZAutoRefCount<cIGZPropertySet> current = layers_[1];
    std::size_t slot = 2;
    for (int hops = 0; current && slot < layers_.size() && hops < 64; ++hops) {
        AnyUint32 parent;
        const std::uint32_t key = kRegParentLink;
        if (!current->Value(key, &parent) || parent.get() == 0) break;
        void* res = nullptr;
        const cGZPersistResourceKey rk{0x0b8d821a, 0xfc45caf7, parent.get()};
        if (services == nullptr || !services->GetPersistResource(rk, kGZIID_cIGZPropertySet, &res) || res == nullptr) break;
        cRZAutoRefCount<cIGZPropertySet> next(static_cast<cIGZPropertySet*>(res));
        static_cast<cIGZPropertySet*>(res)->Release();
        layers_[slot++] = next;
        current = next;
    }
    if (const std::int32_t entry = GetRegisterValue(static_cast<std::int32_t>(kRegEntryPoint)); entry != 0) {
        SetTrackDataEntryPoint(static_cast<std::uint32_t>(entry));
    }
    if (const std::int32_t semId = GetRegisterValue(static_cast<std::int32_t>(kRegSemId)); semId != 0) {
        if (cIHitMan* hm = HitMan()) sem_ = hm->GetSem(static_cast<std::uint32_t>(semId));
    }
    void* res = nullptr;
    if (trackDef != nullptr && trackDef->QueryInterface(kGZIID_cIHitPersistResource, &res) && res != nullptr) {
        auto* resource = static_cast<cIHitPersistResource*>(res);
        SetID(resource->GetInstance());   // the binary calls SetID(key.instance)
        resource->Release();
    }
    return true;
}

bool cHitTrackPlayer::Reset() { OwnLayer()->ClearAll(); return true; }   // @ 0x10047f1ac

bool cHitTrackPlayer::SetHitList(cIHitList* list) { defaultHitList_ = list; return true; }   // @ 0x10047f040
bool cHitTrackPlayer::GetHitList(std::uint32_t iid, void** out) {  // @ 0x10047f08c
    return hitList_ && hitList_->QueryInterface(iid, out);
}

float cHitTrackPlayer::CompletionTime() {  // @ 0x10047f0aa
    // The binary reads the playback position and size from the channel's data source; those
    // accessors are recovered with the sound data sources. Until then: unknown -> 0.
    return 0.0f;
}

bool cHitTrackPlayer::SetTrackDataEntryPoint(std::uint32_t entryKey) {  // @ 0x100480af2
    cIHitMan* hm = HitMan();
    if (hm == nullptr) return false;
    cIHitTrackDataBlock* block = nullptr;
    std::uint32_t offset = 0;
    if (!hm->GetTrackDataBlockAndOffset(entryKey, &block, offset)) return false;
    // The base is the start of the whole track data block (jump targets are offsets from it); the entry point, the
    // program counter and the loop mark start at the track's own offset in it (Mac i386 0x00356a48: +0x60 / +0x64..0x6c).
    base_ = block->DataStartAddress();
    pc_ = entry_ = loopMark_ = base_ + offset;
    return true;
}

bool cHitTrackPlayer::CurrentPlayPosition(const std::uint8_t** position) {  // @ 0x100480d00
    *position = IsPlaying() ? pc_ : entry_;
    return true;
}

bool cHitTrackPlayer::IsPlaying() const {  // @ 0x10047b9f2
    if (playing_) return true;
    for (const auto& child : children_) {
        if (child->IsPlaying()) return true;
    }
    return false;
}

// ---- notes / channel ------------------------------------------------------------------------------------

bool cHitTrackPlayer::UpdatePatch() {  // @ 0x10048082a
    const std::int32_t soundId = GetRegisterValue(static_cast<std::int32_t>(kRegPatchId));
    const std::int32_t guard = GetRegisterValue(static_cast<std::int32_t>(kRegRecursionGuard));
    if (guard != 0 && soundId == guard) {
        ReportError("Recursive sound id");
        return false;
    }
    if (soundId != patchId_) {
        channelRegs_ = nullptr;
        channelSnd_ = nullptr;
        patchId_ = soundId;
    }
    if (soundId != 0 && !channelRegs_) {
        void* p = nullptr;
        Hit::cIHitManServices* services = Services();
        if (services == nullptr || !services->GetSoundById(static_cast<std::uint32_t>(soundId), kIidRegisterSet, &p) || p == nullptr) {
            ReportError("Sample file not found");
            return false;
        }
        channelRegs_ = static_cast<cIGZSndRegisterSet*>(p);
        static_cast<cIGZUnknown*>(p)->Release();
    }
    if (channelRegs_) {
        void* snd = nullptr;
        channelSnd_ = nullptr;
        if (channelRegs_->QueryInterface(kIidSnd, &snd) && snd != nullptr) {
            channelSnd_ = static_cast<cIGZSnd*>(snd);
            static_cast<cIGZUnknown*>(snd)->Release();
        }
        if (const std::int32_t semId = GetRegisterValue(static_cast<std::int32_t>(kRegSemId)); semId != 0) {
            if (cIHitMan* hm = HitMan()) sem_ = hm->GetSem(static_cast<std::uint32_t>(semId));
            if (sem_) sem_->ConfigureChannel(this, channelRegs_.get());
        }
        channelRegs_->SetRegisterValue(static_cast<std::int32_t>(kRegLoopFlag),
                                       GetRegisterValue(static_cast<std::int32_t>(kRegLoopFlag)), false);
        SemRefresh();
    }
    UpdatePitch();
    return true;
}

bool cHitTrackPlayer::NoteOn() {  // @ 0x100480a3c
    if (!channelRegs_) {
        ReportError("Attempt to play note with no channel");
        return false;
    }
    UpdatePatch();
    const bool ok = channelSnd_ && channelSnd_->PlayWithVolumeAndLoop(0x400, false, 1);
    SemRefresh();
    UpdatePitch();
    return ok;
}

bool cHitTrackPlayer::UpdateVolPan() {  // @ 0x10047f562
    if (!channelRegs_ || !sem_) return false;
    SemRefresh();
    return true;
}

// UpdatePitch @ 0x10047f596: base frequency (register, or the sound's own), scaled by the two
// pitch offset registers (each 2^(1/12)^n), then forwarded to the channel; the pitch and time
// multipliers are re-notified.
bool cHitTrackPlayer::UpdatePitch() {
    if (!channelRegs_) return false;
    cIGZSnd* snd = channelSnd_.get();
    if (snd == nullptr || !snd->IsLoaded()) return false;

    if (!HasRegisterValue(static_cast<std::int32_t>(kRegFrequency))) {
        std::int32_t original = snd->GetOriginalFrequency();
        AnyUint32 any;
        const std::uint32_t key = kRegFrequency;
        if (layers_[1] && layers_[1]->Value(key, &any)) original = static_cast<std::int32_t>(any.get());
        SetRegisterValue(static_cast<std::int32_t>(kRegFrequency), original, false);
    }
    const std::int32_t semi = GetRegisterValue(static_cast<std::int32_t>(kRegSemitones));
    const std::int32_t cents = GetRegisterValue(static_cast<std::int32_t>(kRegCents));
    std::int32_t frequency = GetRegisterValue(static_cast<std::int32_t>(kRegFrequency));
    if (semi != 0 || cents != 0) {
        double high = static_cast<double>(frequency);
        double low = high;
        if (semi != 0) high = std::pow(kSemitoneRatio, static_cast<double>(semi)) * high;
        if (cents != 0) low = low * std::pow(kSemitoneRatio, static_cast<double>(cents));
        if (cIHitMan* hm = HitMan()) {
            frequency = hm->Random(static_cast<std::int32_t>(low), static_cast<std::int32_t>(high));
            SetRegisterValue(static_cast<std::int32_t>(kRegFrequency), frequency, true);
        }
    }
    channelRegs_->SetRegisterValue(static_cast<std::int32_t>(kRegFrequency), frequency, false);
    if (HasRegisterValue(static_cast<std::int32_t>(kRegPitchMultiplier))) HandleNotifySetPropertyValue(static_cast<std::int32_t>(kRegPitchMultiplier));
    if (HasRegisterValue(static_cast<std::int32_t>(kRegTimeMultiplier))) HandleNotifySetPropertyValue(static_cast<std::int32_t>(kRegTimeMultiplier));
    return true;
}

bool cHitTrackPlayer::SetFrequency(std::int32_t frequency) {  // @ 0x100480bc6
    SetRegisterValue(static_cast<std::int32_t>(kRegFrequency), frequency, false);
    return channelRegs_ ? channelRegs_->SetRegisterValue(static_cast<std::int32_t>(kRegFrequency), frequency, false) : true;
}

bool cHitTrackPlayer::Load() {  // @ 0x100480c60
    if (loadCount_++ == 0 && channelSnd_) channelSnd_->Load();
    return true;
}
bool cHitTrackPlayer::Unload() {  // @ 0x100480cb8
    if (--loadCount_ == 0 && channelSnd_) channelSnd_->Unload();
    return true;
}

bool cHitTrackPlayer::CancelNote() {  // @ 0x10047fc92
    if (channelRegs_ && channelSnd_->IsPlaying()) channelSnd_->Kill();
    return true;
}

bool cHitTrackPlayer::GetPosition(float* x, float* y, float* z) const {  // @ 0x100481094
    *x = static_cast<float>(GetRegisterValue(static_cast<std::int32_t>(kRegPosX)));
    *y = static_cast<float>(GetRegisterValue(static_cast<std::int32_t>(kRegPosY)));
    *z = static_cast<float>(GetRegisterValue(static_cast<std::int32_t>(kRegPosZ)));
    return true;
}
bool cHitTrackPlayer::SetPosition(float x, float y, float z) {  // @ 0x10048118c: also turns 3D positioning off
    SetRegisterValue(static_cast<std::int32_t>(kRegPosX), static_cast<std::int32_t>(x), false);
    SetRegisterValue(static_cast<std::int32_t>(kRegPosY), static_cast<std::int32_t>(y), false);
    SetRegisterValue(static_cast<std::int32_t>(kRegPosZ), static_cast<std::int32_t>(z), false);
    SetRegisterValue(0xffcc156f, 0, false);
    return true;
}

// ---- sources -----------------------------------------------------------------------------------------------

// Binary bug kept: the insertion sits inside a branch that is only reached when the id is
// already present, so a source is never actually added (the list stays empty); only the
// UpdateSources() notification happens.
void cHitTrackPlayer::AddSource(std::uint32_t) { UpdateSources(); }
bool cHitTrackPlayer::RemoveSource(std::uint32_t sourceId) {  // @ 0x100480e46
    if (sources_.erase(sourceId) == 0) return false;
    return UpdateSources();
}
void cHitTrackPlayer::RemoveAllSources() { sources_.clear(); UpdateSources(); }  // @ 0x100480ebe
bool cHitTrackPlayer::UpdateSources() {  // @ 0x100480f58
    SemRefresh();
    return true;
}

bool cHitTrackPlayer::SetCompletionNotification(cIGZSndCompletionFunctionObject* callback) {  // @ 0x100480fbc
    completion_ = callback;
    return true;
}
bool cHitTrackPlayer::StopCompletionNotification() { completion_ = nullptr; return true; }  // @ 0x100481034

// ---- playback control -----------------------------------------------------------------------------------------

// StartSndDependent @ 0x10047eb34: fetches sound `soundId`, copies this player's explicitly set
// registers to it (where it has none) and starts it with the four argument registers.
bool cHitTrackPlayer::StartSndDependent(std::uint32_t soundId) {
    Hit::cIHitManServices* services = Services();
    void* p = nullptr;
    if (services == nullptr || !services->GetSoundById(soundId, kIidSnd, &p) || p == nullptr) return false;
    cRZAutoRefCount<cIGZSnd> snd(static_cast<cIGZSnd*>(p));
    static_cast<cIGZUnknown*>(p)->Release();
    children_.push_back(snd);

    cRZAutoRefCount<cIGZPropertySet> own = layers_[0];
    void* regs = nullptr;
    if (snd->QueryInterface(kIidRegisterSet, &regs) && regs != nullptr) {
        auto* childRegs = static_cast<cIGZSndRegisterSet*>(regs);
        std::uint32_t key = 0;
        cIGZAny* value = nullptr;
        if (own->FirstKeyAndValue(&key, &value)) {
            do {
                if (!childRegs->HasRegisterValue(static_cast<std::int32_t>(key))) {
                    // the payload word right after the vptr is copied as an integer
                    std::uint32_t payload = 0;
                    std::memcpy(&payload, reinterpret_cast<const unsigned char*>(value) + 8, sizeof payload);
                    childRegs->SetRegisterValue(static_cast<std::int32_t>(key), static_cast<std::int32_t>(payload), true);
                }
            } while (own->NextKeyAndValue(&key, &value));
        }
        childRegs->Release();
    }
    snd->PlayWithScriptArguments(GetRegisterValue(static_cast<std::int32_t>(kRegArg0)),
                                 GetRegisterValue(static_cast<std::int32_t>(kRegArg1)),
                                 GetRegisterValue(static_cast<std::int32_t>(kRegArg2)),
                                 GetRegisterValue(static_cast<std::int32_t>(kRegArg3)));
    return true;
}

// Play(int,int,int,int) @ 0x10047e8ae. The argument registers are stored without notification;
// kRegArgMode (0..5) decides how the arguments are mirrored into volume/pan/position/object.
bool cHitTrackPlayer::PlayWithScriptArguments(std::int32_t a0, std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    if (IsPlaying()) {
        if (GetRegisterValue(0xff303b1b) == 0) return true;   // "don't restart a playing track"
        Kill();
    }
    SetRegisterValue(static_cast<std::int32_t>(kRegArg0), a0, true);
    SetRegisterValue(static_cast<std::int32_t>(kRegArg1), a1, true);
    SetRegisterValue(static_cast<std::int32_t>(kRegArg2), a2, true);
    SetRegisterValue(static_cast<std::int32_t>(kRegArg3), a3, true);
    switch (GetRegisterValue(static_cast<std::int32_t>(kRegArgMode))) {
        case 1: SetRegisterValue(static_cast<std::int32_t>(kRegVolume), a0, true); SetRegisterValue(static_cast<std::int32_t>(kRegPan), a1, true); break;
        case 2: SetRegisterValue(static_cast<std::int32_t>(kRegGameObject), a0, true); break;
        case 3:
            SetRegisterValue(static_cast<std::int32_t>(kRegGameObject), a0, true);
            SetRegisterValue(static_cast<std::int32_t>(kRegVolume), a1, true);
            SetRegisterValue(static_cast<std::int32_t>(kRegPan), a2, true);
            break;
        case 4:
            SetRegisterValue(static_cast<std::int32_t>(kRegPosX), a0, true);
            SetRegisterValue(static_cast<std::int32_t>(kRegPosY), a1, true);
            SetRegisterValue(static_cast<std::int32_t>(kRegPosZ), a2, true);
            break;
        case 5:
            SetRegisterValue(static_cast<std::int32_t>(kRegGameObject), a0, true);
            SetRegisterValue(static_cast<std::int32_t>(kRegPosX), a1, true);
            SetRegisterValue(static_cast<std::int32_t>(kRegPosY), a2, true);
            SetRegisterValue(static_cast<std::int32_t>(kRegPosZ), a3, true);
            break;
        default: break;
    }
    const bool started = PlayPause();
    if (const std::int32_t dep = GetRegisterValue(static_cast<std::int32_t>(kRegDependentSound)); dep != 0) {
        StartSndDependent(static_cast<std::uint32_t>(dep));
    }
    if (!started) return false;
    return Unpause();  // the binary tail-calls vtable slot 0x258 (Unpause) to arm the timer
}

// PlayPause @ 0x10047f1c4: (re)starts the track: honours a muted control group, resumes when
// merely paused, otherwise sets up the fade-in, entry point, hit list and wait counter.
bool cHitTrackPlayer::PlayPause() {
    if (const std::int32_t groupId = GetRegisterValue(static_cast<std::int32_t>(kRegControlGroup)); groupId != 0) {
        if (cIHitMan* hm = HitMan()) {
            if (cIHitControlGroup* group = hm->ControlGroup(groupId)) {
                if (group->AsIGZSnd()->GetVolume() == 0) return true;  // muted group: nothing to do
            }
        }
    }
    if (IsPlaying() && IsPaused()) {
        paused_ = 1;
        return Unpause();
    }
    fadingOut_ = false;
    if (const std::int32_t fadeIn = GetRegisterValue(static_cast<std::int32_t>(kRegFadeInMs)); fadeIn != 0) {
        if (GetRegisterValue(static_cast<std::int32_t>(kRegFadeActive)) != 0) {
            SetRegisterValue(static_cast<std::int32_t>(kRegVolumeScale),
                             GetRegisterValue(static_cast<std::int32_t>(kRegFadeEnd)), true);
        }
        SetRegisterValue(static_cast<std::int32_t>(kRegFadeTarget), static_cast<std::int32_t>(kRegVolumeScale), true);
        SetRegisterValue(static_cast<std::int32_t>(kRegFadeEnd), GetRegisterValue(static_cast<std::int32_t>(kRegVolumeScale)), true);
        SetRegisterValue(static_cast<std::int32_t>(kRegVolumeScale), 0, true);
        const std::int64_t end = GetRegisterValue(static_cast<std::int32_t>(kRegFadeEnd));
        const std::int64_t ms = GetRegisterValue(static_cast<std::int32_t>(kRegFadeInMs));
        SetRegisterValue(static_cast<std::int32_t>(kRegFadeRate), static_cast<std::int32_t>(ms == 0 ? 0 : (end * 1000) / ms), true);
        if (GetRegisterValue(static_cast<std::int32_t>(kRegFadeRate)) != 0) {
            SetRegisterValue(static_cast<std::int32_t>(kRegFadeActive), 1, false);
        }
    }
    paused_ = 0;
    SetRegisterValue(static_cast<std::int32_t>(kRegPatchId), GetRegisterValue(static_cast<std::int32_t>(kRegPatchId)), false);
    SemRefresh();
    if (!SetRegisterValue(static_cast<std::int32_t>(kRegEntryPoint), GetRegisterValue(static_cast<std::int32_t>(kRegEntryPoint)), false)) {
        return false;
    }
    pc_ = entry_;
    loopMark_ = entry_;
    hitList_ = defaultHitList_;
    if (const std::int32_t listId = GetRegisterValue(static_cast<std::int32_t>(kRegHitListId)); listId != 0) {
        cIHitMan* hm = HitMan();
        void* list = nullptr;
        if (hm == nullptr || !hm->GetHitList(static_cast<std::uint32_t>(listId), kIidHitListIface, &list) || list == nullptr) return false;
        hitList_ = static_cast<cIHitList*>(list);
        static_cast<cIGZUnknown*>(list)->Release();
    }
    wait_ = GetRegisterValue(static_cast<std::int32_t>(kRegStartDelay));
    UpdatePitch();
    playing_ = true;
    AddRef();  // released again when the track ends
    SetRegisterValue(static_cast<std::int32_t>(kRegPlayingFlag), 1, false);
    return true;
}

bool cHitTrackPlayer::Pause(bool pauseChannel) {  // @ 0x10047f81a
    paused_ = 1;
    for (auto& child : children_) child->Pause(pauseChannel);
    if (IsPlaying()) {
        if (pauseChannel && channelRegs_) channelSnd_->Pause(true);
        if (Hit::cIHitManServices* s = Services()) s->UnregisterUpdateTarget(static_cast<cIGZSndUpdateTarget*>(this));
    }
    return true;
}

bool cHitTrackPlayer::Unpause() {  // @ 0x10047f8dc
    paused_ = 0;
    for (auto& child : children_) child->Unpause();
    if (channelRegs_ && channelSnd_->IsPaused()) channelSnd_->Unpause();
    HandleTimerCallback(0);
    if (IsPlaying()) {
        if (Hit::cIHitManServices* s = Services()) s->RegisterUpdateTarget(static_cast<cIGZSndUpdateTarget*>(this), 20);
    }
    return !GetErrorCondition();
}

// Stop @ 0x10047f9a8: stops dependents; if the channel plays and a fade-out time is set (and no fade
// is running) it starts the fade-out, otherwise it kills the track.
bool cHitTrackPlayer::Stop() {
    for (auto& child : children_) child->Stop();
    if (IsPlaying()) {
        if (channelRegs_ && channelSnd_->IsPlaying() &&
            GetRegisterValue(static_cast<std::int32_t>(kRegFadeOutMs)) != 0 &&
            GetRegisterValue(static_cast<std::int32_t>(kRegFadeActive)) == 0) {
            SetRegisterValue(static_cast<std::int32_t>(kRegFadeTarget), static_cast<std::int32_t>(kRegVolumeScale), true);
            SetRegisterValue(static_cast<std::int32_t>(kRegFadeEnd), 0, true);
            const std::uint32_t ms = static_cast<std::uint32_t>(GetRegisterValue(static_cast<std::int32_t>(kRegFadeOutMs)));
            const std::int32_t scale = GetRegisterValue(static_cast<std::int32_t>(kRegVolumeScale));
            SetRegisterValue(static_cast<std::int32_t>(kRegFadeRate),
                             static_cast<std::int32_t>(static_cast<std::uint32_t>(scale * 1000) / ms), true);
            if (GetRegisterValue(static_cast<std::int32_t>(kRegFadeRate)) != 0) {
                SetRegisterValue(static_cast<std::int32_t>(kRegFadeActive), 1, false);
            }
            fadingOut_ = true;
            return true;
        }
        Kill();
    }
    return true;
}

// Kill @ 0x10047fb0e: ends the track immediately and tears down dependents.
bool cHitTrackPlayer::Kill() {
    while (!children_.empty()) {
        children_.front()->Kill();
        children_.pop_front();
    }
    if (channelRegs_) {
        channelSnd_->Stop();
        if (channelRegs_) channelSnd_->Kill();
    }
    SetRegisterValue(static_cast<std::int32_t>(kRegFadeActive), 0, false);
    fadingOut_ = false;
    targetRegs_ = nullptr;
    pc_ = entry_;
    wait_ = 0;
    AddRef();  // keep alive for the rest of the function
    if (Hit::cIHitManServices* s = Services()) s->UnregisterUpdateTarget(static_cast<cIGZSndUpdateTarget*>(this));
    callStack_.clear();
    if (cIHitMan* hm = HitMan()) hm->DuckMapSetSndobPri(static_cast<cIGZSnd*>(this), 0);
    if (completion_) completion_->OnSoundCompleted(static_cast<cIGZSnd*>(this));
    SetRegisterValue(static_cast<std::int32_t>(kRegPlayingFlag), 0, false);
    if (playing_) {
        playing_ = false;
        Release();  // the reference PlayPause took
    }
    Release();      // the hold above (may destroy the object)
    return true;
}

void cHitTrackPlayer::Step() {  // @ 0x10047fccc: single-steps the program
    if (!IsPlaying()) PlayPause();
    Unpause();  // the binary calls vtable slot 0x250 with a zero argument
    DoCommand();
}

void cHitTrackPlayer::Update(std::int32_t elapsedMs) { HandleTimerCallback(elapsedMs); }  // @ 0x10047ba98

// HandleTimerCallback @ 0x10047bac0: the player's tick. Prunes finished dependents, advances a
// running fade, burns down the wait counter and executes instructions until one waits.
void cHitTrackPlayer::HandleTimerCallback(std::int32_t elapsedMs) {
    if (paused_ != 0) return;
    for (auto it = children_.begin(); it != children_.end();) {
        it = (*it)->IsPlaying() ? std::next(it) : children_.erase(it);
    }
    if (playing_ || !children_.empty()) {
        if (GetRegisterValue(static_cast<std::int32_t>(kRegFadeActive)) != 0) {
            const std::int32_t target = GetRegisterValue(static_cast<std::int32_t>(kRegFadeTarget));
            std::int32_t current = GetRegisterValue(target);
            const std::int32_t end = GetRegisterValue(static_cast<std::int32_t>(kRegFadeEnd));
            const std::int32_t rate = GetRegisterValue(static_cast<std::int32_t>(kRegFadeRate));
            if (rate == 0) SetRegisterValue(static_cast<std::int32_t>(kRegFadeActive), 0, false);
            std::int32_t step = -rate;
            if ((current < end) == (0 < rate)) step = rate;
            std::int32_t next = (step * elapsedMs) / 1000 + current;
            if ((step > 0 && end < next) || (step < 0 && next < end)) {
                SetRegisterValue(static_cast<std::int32_t>(kRegFadeActive), 0, false);
                next = end;
            }
            SetRegisterValue(target, next, false);
        }
        if (!fadingOut_ || GetRegisterValue(static_cast<std::int32_t>(kRegFadeActive)) != 0) {
            if (wait_ != 0) {
                const std::int32_t burn = std::min(wait_, elapsedMs);
                wait_ -= burn;
            }
            if (paused_ == 0) {
                for (std::uint32_t n = 0; playing_ && wait_ == 0 && paused_ == 0; ++n) {
                    DoCommand();
                    if (n > 100) ReportError("Too many instructions executed without wait (infinite loop?)");
                }
            }
            if (noteTicks_ != 0 && --noteTicks_ == 0 && channelRegs_) channelSnd_->Stop();
            return;
        }
        fadingOut_ = false;
    }
    Kill();
}

// ---- the interpreter ---------------------------------------------------------------------------------------------

bool cHitTrackPlayer::ReadRegisterOperand(std::uint8_t& out, bool clamp) {
    out = *pc_++;
    if (out > 0x4a) {
        ReportError("Illegal Register Id");
        if (clamp) out = 1;
        return false;
    }
    return true;
}

// DoCommand @ 0x10047bda2: executes one instruction at pc_. Register operands are single bytes
// (0..0x4a, anything else is reported and replaced by register 1); immediates are 32-bit and
// jump targets are offsets from the data block base. Arithmetic writes through SetRegisterValue
// with notification. Waiting instructions rewind pc_ and set the wait counter to 2.
bool cHitTrackPlayer::DoCommand() {
    const std::uint8_t* const opAddr = pc_;
    const std::uint8_t op = *pc_++;
    auto reg = [this]() { std::uint8_t r; ReadRegisterOperand(r); return r; };
    auto val = [this](std::uint8_t r) { return GetRegisterValue(r); };
    auto set = [this](std::uint8_t r, std::int32_t v) { SetRegisterValue(r, v, false); };
    auto imm32 = [this]() { std::int32_t v; std::memcpy(&v, pc_, 4); pc_ += 4; return v; };
    auto jump = [this](std::int32_t off) { pc_ = base_ + off; };
    auto cmpWait = [this](bool satisfied) {  // wait-until instructions: rewind to the opcode
        if (satisfied) return;
        pc_ -= 3;
        wait_ = 2;
    };
    cIHitMan* hm = HitMan();

    switch (op) {
        case 0x01: {  // note on with duration register
            const auto r1 = reg();
            (void)val(r1);
            const auto r2 = reg();
            noteTicks_ = val(r2);
            if (NoteOn()) return true;
            (void)GetRegisterValue(static_cast<std::int32_t>(kRegPatchId));
            ReportError("Could not set patch - file or resource not found");
            return true;
        }
        case 0x02: ++pc_; NoteOn(); break;
        case 0x03: return channelSnd_ ? channelSnd_->Stop() : true;   // note off
        case 0x04: {  // load byte immediate (sign extended)
            const auto r = reg();
            const auto v = static_cast<std::int8_t>(*pc_++);
            if (!SetRegisterValue(r, v, false)) ReportError("Could not set register at kTrackCmdLoadb");
            return true;
        }
        case 0x05: {  // load 32-bit immediate
            const auto r = reg();
            const std::int32_t v = imm32();
            if (!SetRegisterValue(r, v, false)) ReportError("Could not set register at kTrackCmdLoadl");
            return true;
        }
        case 0x06: case 0x4a: {  // set register from register
            const auto d = reg();
            const auto s = reg();
            if (!SetRegisterValue(d, val(s), false)) ReportError("Could not set local property at kTrackCmdSet");
            return true;
        }
        case 0x07: {  // call (offset)
            const std::int32_t off = imm32();
            callStack_.push_back(pc_);
            jump(off);
            break;
        }
        case 0x0a: {  // call by entry point key
            const std::int32_t key = imm32();
            callStack_.push_back(pc_);
            pc_ = hm ? hm->TrackDataEntryPoint(static_cast<std::uint32_t>(key)) : nullptr;
            break;
        }
        case 0x0d: jump(*reinterpret_cast<const std::int32_t*>(pc_)); break;  // jump
        case 0x08:  // return; with an empty stack it behaves like "end"
            if (!callStack_.empty()) {
                pc_ = callStack_.back();
                callStack_.pop_back();
                return true;
            }
            [[fallthrough]];
        case 0x0c:  // end
            if (playing_) {
                playing_ = false;
                Release();
            }
            break;
        case 0x09: wait_ = val(reg()); break;  // wait register
        case 0x0b:  // wait for the note to finish
            if (!channelRegs_) return true;
            if (!channelSnd_->IsPlaying()) return true;
            pc_ -= 1;
            wait_ = 2;
            break;
        case 0x0e: SetFlags(val(reg())); break;  // compare with zero
        case 0x0f: case 0x1d: case 0x1e: case 0x5d: case 0x5e: break;  // no-ops
        case 0x10: { const auto d = reg(); const auto s = reg(); set(d, val(s) + val(d)); break; }  // add
        case 0x11: { const auto d = reg(); const auto s = reg(); set(d, val(d) - val(s)); break; }  // subtract
        case 0x12: {  // divide (a / b)
            const auto d = reg(); const auto s = reg();
            const std::int32_t a = val(d), b = val(s);
            if (b == 0) { ReportError("Divide by zero"); set(d, 0); } else set(d, a / b);
            break;
        }
        case 0x13: { const auto d = reg(); const auto s = reg(); set(d, val(s) * val(d)); break; }  // multiply
        case 0x14: {  // compare two registers
            const auto a = reg(); const auto b = reg();
            const std::int32_t x = val(a), y = val(b);
            flagEqual_ = x == y; flagGreater_ = x != y && y <= x; flagLess_ = x < y;
            break;
        }
        case 0x15: { const auto d = reg(); const auto s = reg(); set(d, val(d) < val(s)); break; }  // less-than
        case 0x16: case 0x28: { const auto d = reg(); const auto s = reg(); set(d, val(s) < val(d)); break; }  // greater-than
        case 0x17: { const auto d = reg(); set(d, val(d) == 0); break; }  // not
        case 0x18: {  // random between two registers
            const auto d = reg(); const auto lo = reg(); const auto hi = reg();
            const std::int32_t lower = val(lo), upper = val(hi);
            set(d, hm ? hm->Random(upper, lower) : 0);
            break;
        }
        case 0x19: { const auto d = reg(); const std::int32_t v = val(d); set(d, v > 0 ? v : -v); break; }  // abs
        case 0x1b: Stop(); return true;
        case 0x1c: { const auto r = reg(); if (val(r) != 0) return true; Stop(); return true; }
        case 0x1f: { const auto d = reg(); const auto s = reg(); set(d, GetRegisterValue(val(s))); break; }  // indirect read
        case 0x20: pc_ = loopMark_; break;   // jump to the saved mark
        case 0x21: loopMark_ = pc_; break;   // save mark
        case 0x22: { const auto a = reg(); const auto b = reg(); if (hm) hm->Callback(val(a), val(b)); break; }
        case 0x23: { const auto r = reg(); if (hitList_) hitList_->AddValue(val(r)); break; }
        case 0x24: { const auto r = reg(); if (hitList_) hitList_->Remove(val(r)); break; }
        case 0x25: if (hitList_) hitList_->RemoveAll(); break;
        case 0x26: { const auto r = reg(); if (hitList_) hitList_->SetCriterion(val(r)); break; }
        case 0x27: {  // choose from hit list
            const auto d = reg();
            if (!hitList_) { ReportError("HitList is NULL"); return true; }
            if (hitList_->NumItems() == 0) { ReportError("Attempt to choose from empty hitlist"); return true; }
            set(d, hitList_->Choose());
            break;
        }
        case 0x29: { const auto d = reg(); const auto s = reg(); set(d, val(d) != 0 ? val(s) == 0 : 1); break; }   // NAND
        case 0x2a: { const auto d = reg(); const auto s = reg(); set(d, val(s) | val(d)); break; }                  // OR
        case 0x2b: { const auto d = reg(); const auto s = reg(); set(d, val(d) == 0 ? val(s) == 0 : 0); break; }  // NOR
        case 0x2c: { const auto d = reg(); const auto s = reg(); set(d, val(s) ^ val(d)); break; }                  // XOR
        case 0x2d: {  // clamp to an upper bound
            const auto d = reg(); const std::int32_t v = imm32();
            if (val(d) <= v) return true;
            set(d, v);
            break;
        }
        case 0x2e: {  // clamp to a lower bound
            const auto d = reg(); const std::int32_t v = imm32();
            if (v <= val(d)) return true;
            set(d, v);
            break;
        }
        case 0x2f: { const auto d = reg(); set(d, val(d) + 1); break; }
        case 0x30: { const auto d = reg(); set(d, val(d) - 1); break; }
        case 0x31: (void)val(reg()); break;
        case 0x32: {  // play dependent track
            const std::int32_t id = val(reg());
            if (id != 0) { StartSndDependent(static_cast<std::uint32_t>(id)); return true; }
            ReportError("Attempt to playtrack 0");
            return true;
        }
        case 0x33: {  // kill track by id (0 = self)
            const std::int32_t id = val(reg());
            if (id == 0) { Kill(); return true; }
            if (Hit::cIHitManServices* s = Services()) s->EnumerateUpdateTargets(&EnumFuncKillTrack, static_cast<std::uint32_t>(id), nullptr);
            break;
        }
        case 0x3e: { const std::int32_t off = imm32(); if (flagEqual_) jump(off); break; }                           // jump if equal
        case 0x3f: { const std::int32_t off = imm32(); if (!flagEqual_) jump(off); break; }                          // if not equal
        case 0x40: { const std::int32_t off = imm32(); if (flagGreater_ && !flagEqual_) jump(off); break; }           // if greater
        case 0x41: { const std::int32_t off = imm32(); if (flagLess_ && !flagEqual_) jump(off); break; }              // if less
        case 0x42: { const std::int32_t off = imm32(); if (flagGreater_ || flagEqual_) jump(off); break; }            // if >=
        case 0x43: { const std::int32_t off = imm32(); if (flagLess_ || flagEqual_) jump(off); break; }              // if <=
        case 0x44: {  // select hit list by id (0 = the default list)
            const std::int32_t id = val(reg());
            if (id == 0) {
                hitList_ = defaultHitList_;
            } else {
                hitList_ = nullptr;
                void* list = nullptr;
                if (hm && hm->GetHitList(static_cast<std::uint32_t>(id), kIidHitListIface, &list) && list) {
                    hitList_ = static_cast<cIHitList*>(list);
                    static_cast<cIGZUnknown*>(list)->Release();
                }
            }
            if (!hitList_) ReportError("Undefined Hit List");
            return true;
        }
        case 0x45: {  // sequence group: wait for the previous owner to fade, then take the group
            const std::int32_t group = val(reg());
            cIGZSnd* other = nullptr;
            const bool have = hm && hm->GetSequenceGroupSnd(static_cast<std::uint32_t>(group), &other);
            if (!have || other == nullptr || other == static_cast<cIGZSnd*>(this) || !other->IsPlaying()) {
                if (hm) hm->SetSequenceGroupSnd(static_cast<std::uint32_t>(group), static_cast<cIGZSnd*>(this));
                return true;
            }
            void* p = nullptr;
            other->QueryInterface(kIidTrackPlayer, &p);
            auto* otherPlayer = static_cast<cIHitTrackPlayer*>(p);
            cIGZSndRegisterSet* regs = otherPlayer->AsIGZSndRegisterSet();
            if (regs->GetRegisterValue(static_cast<std::int32_t>(kRegVolumeScale)) == 0) {
                other->Kill();
                return true;
            }
            if (regs->GetRegisterValue(static_cast<std::int32_t>(kRegFadeActive)) == 0 ||
                regs->GetRegisterValue(static_cast<std::int32_t>(kRegFadeTarget)) != static_cast<std::int32_t>(kRegVolumeScale)) {
                regs->SetRegisterValue(static_cast<std::int32_t>(kRegFadeRate), 50000, false);
                regs->SetRegisterValue(static_cast<std::int32_t>(kRegFadeTarget), static_cast<std::int32_t>(kRegVolumeScale), false);
                regs->SetRegisterValue(static_cast<std::int32_t>(kRegFadeActive), 1, false);
            }
            pc_ = opAddr;
            wait_ = 2;
            break;
        }
        case 0x46: case 0x47: {  // kill the sequence group's other sound (or take the group)
            const std::int32_t group = val(reg());
            if (group == 0) { ReportError("kTrackCmdSeqGroupKill: No group ID"); return true; }
            cIGZSnd* other = nullptr;
            const bool have = hm && hm->GetSequenceGroupSnd(static_cast<std::uint32_t>(group), &other);
            if (!have || other == nullptr || other == static_cast<cIGZSnd*>(this)) {
                if (hm) hm->SetSequenceGroupSnd(static_cast<std::uint32_t>(group), static_cast<cIGZSnd*>(this));
                return true;
            }
            if (!other->IsPlaying()) return true;
            Kill();
            return true;
        }
        case 0x48: {  // read an app data field of the game object
            const auto d = reg();
            std::uint8_t skipped;
            ReadRegisterOperand(skipped, false);  // checked, unused
            const auto s = reg();
            const std::int32_t object = GetRegisterValue(static_cast<std::int32_t>(kRegGameObject));
            if (object == 0) { ReportError("App data request has no game object id\n"); return true; }
            std::int32_t out = 0;
            if (hm && hm->GetSourceDataField(object, val(s), &out)) { SetRegisterValue(d, out, false); return true; }
            ReportError("Illegal source parameter request");
            return true;
        }
        case 0x49: {  // id of the sound that owns a sequence group
            const auto g = reg();
            const auto d = reg();
            const std::int32_t group = val(g);
            if (group == 0) { ReportError("Illegal sequence group ID"); return true; }
            cIGZSnd* owner = nullptr;
            std::int32_t value = 0;
            if (hm && hm->GetSequenceGroupSnd(static_cast<std::uint32_t>(group), &owner) && owner) value = static_cast<std::int32_t>(owner->GetID());
            set(d, value);
            break;
        }
        case 0x4b: {  // set a register of the target sound
            const auto d = reg(); const auto s = reg();
            const std::int32_t v = val(s);
            if (!targetRegs_) return true;
            if (!targetRegs_->SetRegisterValue(d, v, false)) ReportError("Could not set register at kTrackCmdSetLocalTarget");
            return true;
        }
        case 0x4c: {  // set local property from register (message names the target variant)
            const auto d = reg(); const auto s = reg();
            if (!SetRegisterValue(d, val(s), false)) ReportError("Could not set local property at kTrackCmdSetTargetLocal");
            return true;
        }
        case 0x4d: { const auto a = reg(); const auto b = reg(); cmpWait(val(a) == val(b)); break; }  // wait until ==
        case 0x4e: { const auto a = reg(); const auto b = reg(); cmpWait(val(a) != val(b)); break; }  // wait until !=
        case 0x4f: { const auto a = reg(); const auto b = reg(); const auto x = val(a), y = val(b); cmpWait(y < x); break; }   // wait until >
        case 0x50: { const auto a = reg(); const auto b = reg(); const auto x = val(a), y = val(b); cmpWait(x < y); break; }   // wait until <
        case 0x51: { const auto a = reg(); const auto b = reg(); const auto x = val(a), y = val(b); cmpWait(y <= x); break; }  // wait until >=
        case 0x52: { const auto a = reg(); const auto b = reg(); const auto x = val(a), y = val(b); cmpWait(x <= y); break; }  // wait until <=
        case 0x53:
            if (hm) hm->DuckMapSetSndobPri(static_cast<cIGZSnd*>(this), GetRegisterValue(static_cast<std::int32_t>(kRegDuckPriority)));
            break;
        case 0x54: if (hm) hm->DuckMapSetSndobPri(static_cast<cIGZSnd*>(this), 0); break;
        case 0x55: {  // range compare: value against low/high
            const auto a = reg(); const auto b = reg(); const auto c = reg();
            const std::int32_t v = val(a), lo = val(b), hi = val(c);
            flagLess_ = lo < v;      // (named by the binary's own flag slots: +0x132 = lo < v)
            flagGreater_ = v < hi;   // +0x131
            flagEqual_ = hi < v || v < lo;  // +0x130
            break;
        }
        case 0x56: {  // set a global property from two registers
            const auto a = reg(); const auto b = reg();
            const std::int32_t id = val(a), v = val(b);
            Hit::cIHitManServices* s = Services();
            if (s == nullptr) { ReportError("Could not get global property cache at kTrackCmdSetLocalGlobal"); return true; }
            if (!s->SetGlobalProperty(static_cast<std::uint32_t>(id), AnyUint32(static_cast<std::uint32_t>(v)))) {
                ReportError("Could not set global property at kTrackCmdSetLocalGlobal");
            }
            break;
        }
        case 0x57: {  // read a global property into a register
            const auto d = reg();
            const std::int32_t id = imm32();
            AnyUint32 any;
            Hit::cIHitManServices* s = Services();
            if (s == nullptr) ReportError("Could not get global property cache at kTrackCmdSetGlobalLocal");
            else if (!s->GetGlobalProperty(static_cast<std::uint32_t>(id), &any)) ReportError("Could not get global property at kTrackCmdSetGlobalLocal");
            if (!SetRegisterValue(d, static_cast<std::int32_t>(any.get()), false)) ReportError("Could not set local property at kTrackCmdSetGlobalLocal");
            break;
        }
        case 0x58:  // throw: report, then behaves like 0x5b
            ReportError("Track threw an exception (throw instruction)");
            [[fallthrough]];
        case 0x5b: {  // set a register of the channel
            const auto d = reg(); const auto s = reg();
            if (channelRegs_) channelRegs_->SetRegisterValue(d, val(s), false);
            break;
        }
        case 0x59: {  // set an instance property of the game object
            const auto a = reg();
            const std::int32_t value = val(a);
            std::uint8_t skipped;
            ReadRegisterOperand(skipped, false);
            const std::int32_t object = GetRegisterValue(static_cast<std::int32_t>(kRegGameObject));
            if (object == 0) { ReportError("App data request has no game object id\n"); return true; }
            const auto idReg = reg();
            const std::int32_t propId = val(idReg);
            Hit::cIHitManServices* s = Services();
            if (s == nullptr) ReportError("Could not get global property cache");
            else if (!s->SetInstanceProperty(static_cast<std::uint32_t>(object), static_cast<std::uint32_t>(propId),
                                             AnyUint32(static_cast<std::uint32_t>(value)))) {
                ReportError("Could not set global property");
            }
            break;
        }
        case 0x5a: {  // stop track by id
            const std::int32_t id = val(reg());
            if (id == 0) return true;
            if (Hit::cIHitManServices* s = Services()) s->EnumerateUpdateTargets(&EnumFuncStopTrack, static_cast<std::uint32_t>(id), nullptr);
            break;
        }
        case 0x5c:  // play the channel's sound with the four argument registers
            if (channelSnd_) {
                channelSnd_->PlayWithScriptArguments(GetRegisterValue(static_cast<std::int32_t>(kRegArg0)),
                                                     GetRegisterValue(static_cast<std::int32_t>(kRegArg1)),
                                                     GetRegisterValue(static_cast<std::int32_t>(kRegArg2)),
                                                     GetRegisterValue(static_cast<std::int32_t>(kRegArg3)));
            }
            break;
        case 0x5f: {  // read a hit list entry by index (clamped to the last entry)
            const auto d = reg(); const auto i = reg();
            if (!hitList_) { ReportError("HitList is NULL"); return true; }
            const std::int32_t count = hitList_->NumItems();
            if (count == 0) { ReportError("Attempt to index empty hitlist"); return true; }
            std::int32_t index = val(i);
            if (count <= index) index = count - 1;
            std::int32_t out = 0;
            hitList_->GetIntValueByIndex(static_cast<std::uint32_t>(index), &out);
            set(d, out);
            break;
        }
        case 0x60: {  // set register (id immediate) from register (id immediate)
            const std::int32_t d = imm32(), s = imm32();
            if (!SetRegisterValue(d, GetRegisterValue(s), false)) ReportError("Could not set local property at kTrackCmdSetDirect");
            return true;
        }
        case 0x61: {  // indirect: value = register[register[s]]
            const std::int32_t d = imm32(), s = imm32();
            if (!SetRegisterValue(d, GetRegisterValue(GetRegisterValue(s)), false)) ReportError("Could not set local property at kTrackCmdSetIndirect");
            return true;
        }
        default:  // includes 0x1a and the unused 0x34..0x3d
            if (std::getenv("SC4_AUDIODEBUG")) {   // the opcode, where it sits in the track's data and the bytes before it
                std::fprintf(stderr, "hit track %p: opcode %02x at +%td (entry +%td), before:", static_cast<void*>(this), op, opAddr - base_,
                             entry_ - base_);
                for (const std::uint8_t* b = opAddr - base_ >= 12 ? opAddr - 12 : base_; b < opAddr + 4; ++b) std::fprintf(stderr, " %02x", *b);
                std::fprintf(stderr, "\n");
                static const cHitTrackPlayer* dumped = nullptr;   // the program's head, once per player
                if (dumped != this) {
                    dumped = this;
                    std::fprintf(stderr, "hit track %p: sound %08x program:", static_cast<void*>(this), GetID());
                    for (const std::uint8_t* b = base_; b < base_ + 96; ++b) std::fprintf(stderr, " %02x", *b);
                    std::fprintf(stderr, "\n");
                }
            }
            ReportError("Illegal Instruction");
            return true;
    }
    return true;
}

// HandleTrackFlowControlError @ 0x1004be3b9: the default handler installed in cHitMan kills the
// offending track and flags it as errored.
namespace {
void HandleTrackFlowControlError(cIHitTrackPlayer* player, const char*) {
    if (player == nullptr) return;
    player->AsIGZSnd()->Kill();
    player->SetErrorCondition();
}
struct DefaultHandlerInstaller {
    DefaultHandlerInstaller() { RegisterDefaultTrackFlowErrorHandler(&HandleTrackFlowControlError); }
} g_defaultHandlerInstaller;
}  // namespace
