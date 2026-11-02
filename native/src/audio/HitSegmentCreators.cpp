#include "audio/HitSegmentCreators.h"

#include <cstdio>
#include <cstdlib>

#include "audio/AudioServices.h"
#include "audio/HitMan.h"
#include "audio/HitManServices.h"
#include "audio/interfaces/cIGZSndDataSourceHolder.h"
#include "audio/interfaces/cIGZSndRegisterSet.h"
#include "audio/interfaces/cIHitTrackPlayer.h"
#include "gz/cIGZPropertySet.h"

namespace {
bool QueryCreatorInterface(std::uint32_t iid, void** out, cIGZSoundCreator* creator, cIGZSndDefRegistrar* registrar,
                           cIGZSndTrackDefParserRegistrar* parser, cIGZUnknown* self) {
    void* result = nullptr;
    switch (iid) {
        case kGZIID_cIGZUnknown:
        case kGZIID_cIGZSndDefRegistrar: result = registrar; break;
        case kGZIID_cIGZSoundCreator: result = creator; break;
        case kGZIID_cIGZSndTrackDefParserRegistrar: result = parser; break;
        default: return false;
    }
    *out = result;
    self->AddRef();
    return true;
}
}  // namespace

// ============================================================================================
// cHitSegmentTrackCreator
// ============================================================================================

cHitSegmentTrackCreator::cHitSegmentTrackCreator() { persist_ = Audio::PersistService(); }

bool cHitSegmentTrackCreator::QueryInterface(std::uint32_t iid, void** out) {
    return QueryCreatorInterface(iid, out, static_cast<cIGZSoundCreator*>(this),
                                 static_cast<cIGZSndDefRegistrar*>(this),
                                 static_cast<cIGZSndTrackDefParserRegistrar*>(this),
                                 static_cast<cIGZSoundCreator*>(this));
}

std::uint32_t cHitSegmentTrackCreator::Release() {
    if (refCount_ == 1) {
        refCount_ = 0;
        delete this;
        return 0;
    }
    return static_cast<std::uint32_t>(--refCount_);
}

bool cHitSegmentTrackCreator::RegisterSoundDef(std::uint32_t group, const cIGZString&, std::uint32_t, const cIGZVariant*) {
    groups_.push_back(group);
    return true;
}

bool cHitSegmentTrackCreator::RegisterTrackDefParser(cIGZSndDefParser* parser) {
    void* raw = nullptr;
    parser->QueryInterface(kGZIID_cIGZSndDefParserBase, &raw);
    parser_ = static_cast<cIGZUnknown*>(raw);
    if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    return true;
}

bool cHitSegmentTrackCreator::CreateSound(std::uint32_t id, std::uint32_t, cIGZVariant*, std::uint32_t iid, void** out) {
    if (!persist_) return false;
    cIGZPropertySet* definition = nullptr;
    for (std::uint32_t group : groups_) {
        void* raw = nullptr;
        if (persist_->GetResource({kTrackResourceType, group, id}, kGZIID_cIGZPropertySet, &raw, 0, 0) && raw) {
            definition = static_cast<cIGZPropertySet*>(raw);
            break;
        }
        if (persist_->GetResource({kAlternateTrackResourceType, group, id}, kGZIID_cIGZPropertySet, &raw, 0, 0) && raw) {
            definition = static_cast<cIGZPropertySet*>(raw);
            break;
        }
    }
    if (!definition) {
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: track creator: no definition %08x in %zu groups\n", id, groups_.size());
        return false;
    }

    bool result = false;
    cHitMan* hitMan = RZ::HitManInstance();
    void* rawPlayer = nullptr;
    const bool made = hitMan && hitMan->CreateTrackPlayer(kGZIID_cIHitTrackPlayer, &rawPlayer) && rawPlayer;
    if (!made && std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: track creator: no player for %08x (hit manager %p)\n", id, static_cast<void*>(hitMan));
    if (made) {
        auto* player = static_cast<cIHitTrackPlayer*>(rawPlayer);
        player->SetTrackDef(definition);
        player->Reset();
        result = player->QueryInterface(iid, out);
        player->Release();
    }
    definition->Release();
    return result;
}

// ============================================================================================
// cHitSegmentDataSourceChannelCreator
// ============================================================================================

cHitSegmentDataSourceChannelCreator::cHitSegmentDataSourceChannelCreator() {
    persist_ = Audio::PersistService();
    soundManager_ = Audio::SoundManagerService();
}

bool cHitSegmentDataSourceChannelCreator::QueryInterface(std::uint32_t iid, void** out) {
    return QueryCreatorInterface(iid, out, static_cast<cIGZSoundCreator*>(this),
                                 static_cast<cIGZSndDefRegistrar*>(this),
                                 static_cast<cIGZSndTrackDefParserRegistrar*>(this),
                                 static_cast<cIGZSoundCreator*>(this));
}

std::uint32_t cHitSegmentDataSourceChannelCreator::Release() {
    if (refCount_ == 1) {
        refCount_ = 0;
        delete this;
        return 0;
    }
    return static_cast<std::uint32_t>(--refCount_);
}

bool cHitSegmentDataSourceChannelCreator::RegisterSoundDef(std::uint32_t group, const cIGZString&, std::uint32_t, const cIGZVariant*) {
    groups_.push_back(group);
    return true;
}

bool cHitSegmentDataSourceChannelCreator::RegisterTrackDefParser(cIGZSndDefParser* parser) {
    void* raw = nullptr;
    parser->QueryInterface(kGZIID_cIGZSndDefParserBase, &raw);
    parser_ = static_cast<cIGZUnknown*>(raw);
    if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    return true;
}

bool cHitSegmentDataSourceChannelCreator::CreateSound(std::uint32_t id, std::uint32_t flags, cIGZVariant* variant,
                                                      std::uint32_t iid, void** out) {
    if (!soundManager_) return false;
    void* rawSource = nullptr;
    if (!soundManager_->GetSndDataSource(id, flags, variant, kGZIID_cIGZSndDataSource, &rawSource) || !rawSource) return false;
    auto* source = static_cast<cIGZSndDataSource*>(rawSource);

    bool result = false;
    Hit::cIHitManServices* services = Hit::GetServices();
    void* rawChannel = nullptr;
    if (services && services->CreateChannel(&rawChannel) && rawChannel) {
        auto* snd = static_cast<cIGZSnd*>(rawChannel);
        void* rawHolder = nullptr;
        if (snd->QueryInterface(0x09d002d2, &rawHolder) && rawHolder) {
            auto* holder = static_cast<cIGZSndDataSourceHolder*>(rawHolder);
            holder->SetDataSource(source, 2);
            void* rawRegisters = nullptr;
            if (holder->QueryInterface(kGZIID_cIGZSndRegisterSet, &rawRegisters) && rawRegisters) {
                auto* registers = static_cast<cIGZSndRegisterSet*>(rawRegisters);
                registers->SetRegisterValue(static_cast<std::int32_t>(kChannelDefaultSemRegister),
                                            static_cast<std::int32_t>(kChannelDefaultSemValue), false);
                registers->Release();
            }
            result = holder->QueryInterface(iid, out);
            holder->Release();
        }
        snd->Release();
    }
    source->Release();
    return result;
}
