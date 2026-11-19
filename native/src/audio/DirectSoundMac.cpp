#include "audio/DirectSoundMac.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#pragma STDC FP_CONTRACT OFF

GUID16 IID_IDirectSound3DBuffer{}, IID_IDirectSound3DListener{}, IID_IDirectSoundBuffer8{};
float gAmbientVolumeLimit = 1.0f;

namespace {
IAudioQueueApi* g_queueApi = nullptr;
IDirectSound_Mac* s_directSound = nullptr;
IDirectSoundBuffer_Mac* s_soundBuffers[256];     // DirectSound buffer slots
std::uint8_t* sBuffers[256];                       // reusable byte stores, one per slot
std::int32_t sBufferSizes[256];
void* s_audioQueueBank[32];
bool m_PrimaryCreated = false;

// The positional pan table: (left, right) gains at the four quadrant corners, and the angle of each corner.
constexpr float kPanTable[4][2] = {{0.4f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.4f}, {0.3f, 0.3f}};
constexpr float kQuadrantAngle[4] = {0.0f, 1.5707964f, 3.1415927f, 4.712389f};
constexpr int kNextQuadrant[4] = {1, 2, 3, 0};
constexpr std::uint32_t kLpcm = 0x6d63706c;
}  // namespace

IAudioQueueApi* GetAudioQueueApi() { return g_queueApi; }
void SetAudioQueueApi(IAudioQueueApi* api) { g_queueApi = api; }

// ---- GetXZAngle ----

float GetXZAngle(cS3DVector3 v) {
    const float length = std::sqrt(v.z * v.z + v.x * v.x + v.y * v.y);
    const float inverse = 0.0f < length ? 1.0f / length : 0.0f;
    const float nx = v.x * inverse;
    const float nz = inverse * v.z;
    if (nx == 0.0f || nz == 0.0f) return 0.0f;
    int quadrant = 4;
    if (v.z <= 0.0f) quadrant = 1;
    if (0.0f < v.x) quadrant = (0.0f < v.z ? 1 : 0) | 2;
    if (quadrant < 1 || quadrant > 4) return 0.0f;
    const float ratio = nz / nx;
    switch (quadrant) {
        case 1: return std::atan(ratio);
        case 4: return std::atan(ratio) + 6.2831855f;
        default: return std::atan(ratio) + 3.1415927f;
    }
}

// ---- IDirectSound_Mac ----

IDirectSound_Mac::IDirectSound_Mac() {
    if (s_directSound) s_directSound->Release();
    std::memset(s_soundBuffers, 0, sizeof s_soundBuffers);
    std::memset(sBuffers, 0, sizeof sBuffers);
    std::memset(sBufferSizes, 0, sizeof sBufferSizes);
    s_directSound = this;
}

IDirectSound_Mac::~IDirectSound_Mac() {
    for (void*& queue : s_audioQueueBank) {
        if (queue && g_queueApi) g_queueApi->Dispose(queue);
    }
    delete listener_;   // (never freed by the original)
    listener_ = nullptr;
    s_directSound = nullptr;
}

IDirectSound_Mac* IDirectSound_Mac::Instance() { return s_directSound; }

void IDirectSound_Mac::ResetGlobalsForTest() {
    std::memset(s_audioQueueBank, 0, sizeof s_audioQueueBank);
    m_PrimaryCreated = false;
    gAmbientVolumeLimit = 1.0f;
}

IDirectSoundBuffer_Mac* IDirectSound_Mac::MacGetSoundBuffer(int index) { return s_soundBuffers[index]; }

std::int32_t IDirectSound_Mac::QueryInterface(const GUID16&, void** out) {
    *out = nullptr;
    return DSERR_NOINTERFACE;
}

std::int32_t IDirectSound_Mac::CreateSoundBuffer(const _DSBUFFERDESC* desc, IDirectSoundBuffer_Mac** out, void*) {
    int slot = -1;
    for (int i = 0; i < 256; ++i) {
        if (!s_soundBuffers[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return static_cast<std::int32_t>(0x8007000e);   // the original would index slot -1; refuse instead
    auto* buffer = new IDirectSoundBuffer_Mac(slot, static_cast<std::int32_t>(desc->bufferBytes), desc->flags, desc->format);
    buffer->AddRef();
    buffer->SetOwner(this);
    AddRef();
    s_soundBuffers[slot] = buffer;
    *out = buffer;
    return 0;
}

std::int32_t IDirectSound_Mac::GetCaps(_DSCAPS* caps) {
    caps->flags = 0xf7f;
    caps->minSecondarySampleRate = 0x1f4;
    caps->maxSecondarySampleRate = 0xbb80;
    caps->primaryBuffers = 1;
    caps->maxHwMixingAllBuffers = 0x20;
    caps->maxHwMixingStaticBuffers = 0x20;
    caps->maxHwMixingStreamingBuffers = 0x20;
    std::uint32_t free = 0;
    for (void* queue : s_audioQueueBank) free += queue == nullptr ? 1 : 0;
    caps->freeHwMixingAllBuffers = free;
    caps->freeHwMixingStaticBuffers = free;
    caps->freeHwMixingStreamingBuffers = free;
    caps->unlockTransferRateHwBuffers = 2;
    caps->playCpuOverheadSwBuffers = 0;
    std::memset(caps->reserved, 0, sizeof caps->reserved);
    caps->reserved[0] = 0;
    return 0;
}

std::int32_t IDirectSound_Mac::DuplicateSoundBuffer(IDirectSoundBuffer_Mac* source, IDirectSoundBuffer_Mac** out) {
    if (!source || !out || source->kind() != IDirectSoundBuffer_Mac::kStatic) return DSERR_INVALIDPARAM;
    int slot = 0;
    for (; slot < 256; ++slot) {
        if (!s_soundBuffers[slot]) {
            // The original files the caller's previous out-pointer here (not the new buffer): the copy is never registered.
            s_soundBuffers[slot] = *out;
            break;
        }
    }
    auto* copy = new IDirectSoundBuffer_Mac(slot, *source);
    *out = copy;
    copy->AddRef();
    source->AddRef();
    return 0;
}

void* IDirectSound_Mac::MacAudioQueueBankAllocate(const AudioStreamBasicDescription& format, IDirectSoundBuffer_Mac* userData) {
    for (void*& slot : s_audioQueueBank) {
        if (slot) continue;
        void* queue = g_queueApi ? g_queueApi->NewOutput(format, userData) : nullptr;
        if (!queue) continue;
        slot = queue;
        g_queueApi->EnableTimePitchBypass(queue);
        return queue;
    }
    return nullptr;
}

IDirectSound3DListener_Mac* IDirectSound_Mac::MacGet3DListener() {
    if (!listener_) {
        listener_ = new IDirectSound3DListener_Mac();
        listener_->MacSetDirectSound(this);
    }
    return listener_;
}

// ---- IDirectSoundBuffer_Mac ----

void IDirectSoundBuffer_Mac::AllocateBytes(int slot, std::uint32_t bytes) {
    const std::uint32_t rounded = (bytes + 1) & ~1u;
    if (slot < 0) {   // no pooled slot: private store
        data_ = static_cast<std::uint8_t*>(std::calloc(1, rounded ? rounded : 1));
        return;
    }
    if (sBuffers[slot]) {
        if (rounded <= static_cast<std::uint32_t>(sBufferSizes[slot])) {
            data_ = sBuffers[slot];
            return;
        }
        std::free(sBuffers[slot]);
    }
    data_ = static_cast<std::uint8_t*>(std::calloc(1, rounded ? rounded : 1));
    sBuffers[slot] = data_;
    sBufferSizes[slot] = static_cast<std::int32_t>(rounded);
}

IDirectSoundBuffer_Mac::IDirectSoundBuffer_Mac(int slot, std::int32_t bufferBytes, std::uint32_t flags, const tWAVEFORMATEX* format) {
    if (flags & 1) {   // the primary buffer holds no samples
        kind_ = kPrimary;
        m_PrimaryCreated = true;
        return;
    }
    size_ = bufferBytes;
    AllocateBytes(slot, static_cast<std::uint32_t>(bufferBytes));
    format_ = *format;
    MacInitSoundInfo();
    playPosition_ = 0;
    nextFill_ = chunkBytes_;
    kind_ = static_cast<Kind>(((flags >> 1) & 1) ^ 3);
}

IDirectSoundBuffer_Mac::IDirectSoundBuffer_Mac(int slot, const IDirectSoundBuffer_Mac& source) {
    AllocateBytes(slot, static_cast<std::uint32_t>(source.size_));
    std::memcpy(data_, source.data_, static_cast<std::size_t>(source.size_));
    size_ = source.size_;
    kind_ = source.kind_;
    playPosition_ = source.playPosition_;
    nextFill_ = source.nextFill_;
    chunkBytes_ = source.chunkBytes_;
    pendingChunk_ = source.pendingChunk_;
    wrapped_ = source.wrapped_;
    volume_ = source.volume_;
    currentRate_ = source.currentRate_;
    originalRate_ = source.originalRate_;
    pan_ = source.pan_;
    leftMultiplier_ = source.leftMultiplier_;
    rightMultiplier_ = source.rightMultiplier_;
    distanceMultiplier_ = source.distanceMultiplier_;
    asbd_ = source.asbd_;
    format_ = source.format_;
    loop_ = source.loop_;
    stopped_ = false;
    owner_ = source.owner_;
    buffer3D_ = nullptr;
    queue_ = nullptr;
}

IDirectSoundBuffer_Mac::~IDirectSoundBuffer_Mac() {
    if (owner_) owner_->Release();
    for (auto& entry : s_soundBuffers) {
        if (entry == this) {
            entry = nullptr;
            break;
        }
    }
    bool any3D = false;
    for (auto* entry : s_soundBuffers) {
        if (entry && entry->buffer3D_) {
            any3D = true;
            break;
        }
    }
    if (!any3D) gAmbientVolumeLimit = 1.0f;
    if (kind_ == kStatic || kind_ == kStreaming) Stop();
    else if (kind_ == kPrimary) m_PrimaryCreated = false;
}

std::int32_t IDirectSoundBuffer_Mac::QueryInterface(const GUID16& iid, void** out) {
    void* result;
    if (iid == IID_IDirectSound3DBuffer) {
        MacCreate3DBuffer();
        result = buffer3D_;
        gAmbientVolumeLimit = 1.0f;
    } else if (iid == IID_IDirectSound3DListener) {
        result = owner_->MacGet3DListener();
    } else if (iid == IID_IDirectSoundBuffer8) {
        *out = this;
        AddRef();
        return 0;
    } else {
        *out = nullptr;
        return DSERR_NOINTERFACE;
    }
    *out = result;
    return 0;
}

void IDirectSoundBuffer_Mac::MacCreate3DBuffer() {
    if (buffer3D_) return;
    owner_->MacGet3DListener();
    buffer3D_ = new IDirectSound3DBuffer_Mac();
    buffer3D_->MacSetBuffer(this);
}

std::int32_t IDirectSoundBuffer_Mac::GetCaps(_DSBCAPS* caps) {
    if (!caps) return DSERR_INVALIDPARAM;
    caps->unlockTransferRate = 0;
    caps->playCpuOverhead = 0;
    caps->flags = 0;
    caps->size = 0x14;
    caps->bufferBytes = static_cast<std::uint32_t>(size_);
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::SetFormat(const tWAVEFORMATEX* format) {
    format_ = *format;
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::GetFormat(tWAVEFORMATEX* format, std::uint32_t size, std::uint32_t*) {
    if (!format || size != 0x12) return DSERR_INVALIDPARAM;
    *format = format_;
    return 0;
}

void IDirectSoundBuffer_Mac::MacInitSoundInfo() {
    const std::uint32_t rate = format_.samplesPerSec;
    const std::uint32_t channels = format_.channels;
    const std::uint32_t bytesPerSample = format_.bitsPerSample >> 3;
    asbd_.sampleRate = static_cast<double>(rate);
    asbd_.formatID = kLpcm;
    asbd_.formatFlags = 0xc;
    asbd_.bytesPerPacket = bytesPerSample * channels;
    asbd_.framesPerPacket = 1;
    asbd_.bytesPerFrame = bytesPerSample * channels;
    asbd_.channelsPerFrame = channels;
    asbd_.bitsPerChannel = format_.bitsPerSample;
    currentRate_ = rate;
    originalRate_ = rate;
    chunkBytes_ = bytesPerSample ? (((rate / bytesPerSample) * channels) / 1000) * 500 : 0;
}

void IDirectSoundBuffer_Mac::MacAdjustSampleRate() {
    if (currentRate_ == format_.samplesPerSec || currentRate_ == originalRate_) return;
    if (!stopped_ && queue_ && playing_) {
        Stop();
        MacInitSoundInfo();
        Play(0, 0, loop_ ? 1u : 0u);
        return;
    }
    MacInitSoundInfo();
}

std::int32_t IDirectSoundBuffer_Mac::Lock(std::uint32_t offset, std::uint32_t bytes, void** ptr1, std::uint32_t* size1,
                                          void** ptr2, std::uint32_t* size2, std::uint32_t flags) {
    std::uint8_t* base = data_;
    std::uint32_t total = static_cast<std::uint32_t>(size_);
    MacAdjustSampleRate();
    if (!(bytes <= total && offset < total && ptr1 && size1)) return DSERR_INVALIDPARAM;
    if (ptr2) *ptr2 = nullptr;
    if (size2) *size2 = 0;
    if ((flags & 2) == 0) {
        if (flags & 1) offset = static_cast<std::uint32_t>(nextFill_);
        *ptr1 = base + offset;
        const std::uint32_t available = total - offset;
        if (available < bytes) {
            *size1 = available;
            if (ptr2) *ptr2 = base;
            if (size2) *size2 = bytes - available;
            return 0;
        }
        *size1 = bytes;
    } else {
        *ptr1 = base;
        *size1 = total;
    }
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::Unlock(void*, std::uint32_t, void*, std::uint32_t) {
    MacAdjustSampleRate();
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::SetCurrentPosition(std::uint32_t position) {
    MacAdjustSampleRate();
    if (!stopped_ && queue_) {
        playPosition_ = position;
        nextFill_ = static_cast<std::uint64_t>(chunkBytes_) + position;
        if (playing_) MacPlaySndBuffer(loop_);
    } else {
        playPosition_ = position;
        nextFill_ = static_cast<std::uint64_t>(chunkBytes_) + position;
    }
    return 0;
}

void IDirectSoundBuffer_Mac::MacPlaySndBuffer(bool loop) {
    if (!queue_ || !g_queueApi) return;
    const std::uint32_t bytesPerSample = format_.bitsPerSample >> 3;   // format at +0x80..: channels, bits
    chunkBytes_ = bytesPerSample ? (static_cast<std::uint32_t>(format_.channels) * (currentRate_ / bytesPerSample)) / 1000 * 500 : 0;
    if (!g_queueApi->Start(queue_)) return;
    AudioQueueBuffer* audioBuffer = nullptr;
    if (!g_queueApi->AllocateBuffer(queue_, chunkBytes_, &audioBuffer)) return;
    loop_ = loop;
    std::memcpy(audioBuffer->data, data_ + playPosition_, chunkBytes_);
    audioBuffer->size = chunkBytes_;
    if (g_queueApi->Enqueue(queue_, audioBuffer)) playing_ = true;
}

std::int32_t IDirectSoundBuffer_Mac::GetCurrentPosition(std::uint32_t* play, std::uint32_t* write) {
    MacAdjustSampleRate();
    if (play) *play = static_cast<std::uint32_t>(playPosition_);
    if (write) *write = static_cast<std::uint32_t>(nextFill_) + chunkBytes_;
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::Play(std::uint32_t, std::uint32_t, std::uint32_t flags) {
    stopped_ = false;
    if (kind_ != kPrimary) {
        MacAdjustSampleRate();
        if (!queue_) {
            queue_ = IDirectSound_Mac::MacAudioQueueBankAllocate(asbd_, this);
            if (!queue_) return DSERR_NOQUEUE;
        }
        if (stopped_ || !playing_) MacPlaySndBuffer((flags & 1) != 0);
    }
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::Stop() {
    if (!stopped_ && queue_ && playing_) loop_ = false;
    stopped_ = true;
    queue_ = nullptr;   // the queue stays in the bank
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::SetPan(std::int32_t pan) {
    if (pan_ == pan) return 0;
    pan_ = pan;
    if (pan == 0) {
        leftMultiplier_ = 1.0f;
        rightMultiplier_ = 1.0f;
    } else if (pan < 0) {
        leftMultiplier_ = 1.0f;
        rightMultiplier_ = (static_cast<float>(pan) + 10000.0f) / 10000.0f;
        MacApplyVolumeAndPan();
        return 0;
    }
    rightMultiplier_ = 1.0f;
    leftMultiplier_ = (10000.0f - static_cast<float>(pan)) / 10000.0f;
    MacApplyVolumeAndPan();
    return 0;
}

void IDirectSoundBuffer_Mac::MacApplyVolumeAndPan() {
    if (!queue_ || !g_queueApi) return;
    g_queueApi->SetParameter(queue_, 0xd, static_cast<float>(static_cast<double>(pan_) / 10000.0));
    float gain = 0.0f;
    if (volume_ != -10000) {
        if (volume_ < -99) {
            int n = volume_ / 100 - 1;
            gain = 1.0f;
            do {
                gain = static_cast<float>(static_cast<double>(gain) * 0.8912509383736036);
                ++n;
            } while (n < -1);
        } else {
            gain = 1.0f;
        }
    }
    if (!buffer3D_) gain = gain * gAmbientVolumeLimit;
    g_queueApi->SetParameter(queue_, 1, gain);
}

std::int32_t IDirectSoundBuffer_Mac::SetVolume(std::int32_t volume) {
    if (volume_ != volume) {
        volume_ = volume;
        MacApplyVolumeAndPan();
    }
    return 0;
}

std::int32_t IDirectSoundBuffer_Mac::GetStatus(std::uint32_t* status) {
    *status = 0;
    std::uint32_t fallback = 2;
    if (!stopped_ && queue_ && playing_) {
        *status = 1;
        *status = (loop_ ? 1u : 0u) * 4 + 1;
        fallback = (loop_ ? 1u : 0u) * 4 + 3;
    }
    if (!data_) *status = fallback;
    return 0;
}

void IDirectSoundBuffer_Mac::MacSetPanVolumeMultipliers(float left, float right, float distance) {
    leftMultiplier_ = left;
    rightMultiplier_ = right;
    distanceMultiplier_ = distance;
    MacApplyVolumeAndPan();
}

void AudioBufferCallback(IDirectSoundBuffer_Mac* b, void* queue, AudioQueueBuffer* audioBuffer) {
    if (!b) return;
    std::int64_t position = static_cast<std::int64_t>(b->nextFill_);
    if (position != 0 || b->loop_) {
        std::uint32_t chunk = b->pendingChunk_;
        if (chunk == 0) {
            chunk = b->chunkBytes_;
        } else {
            b->chunkBytes_ = chunk;
            b->pendingChunk_ = 0;
        }
        std::size_t count = chunk;
        if (position + static_cast<std::int64_t>(count) < static_cast<std::int64_t>(b->size_)) {
            std::uint64_t next = static_cast<std::uint64_t>(position + static_cast<std::int64_t>(count));
            if (b->wrapped_) {
                b->wrapped_ = false;
                position = 0;
                next = count;
            }
            b->playPosition_ = static_cast<std::uint64_t>(position);
            b->nextFill_ = next;
        } else {
            b->playPosition_ = static_cast<std::uint64_t>(position);
            b->nextFill_ = 0;
            if (b->loop_) b->wrapped_ = true;
            count = static_cast<std::uint32_t>(b->size_ - static_cast<std::int32_t>(position));
        }
        audioBuffer->size = static_cast<std::uint32_t>(count);
        if (!b->stopped_) {
            std::memcpy(audioBuffer->data, b->data_ + b->playPosition_, count);
            if (g_queueApi) g_queueApi->Enqueue(queue, audioBuffer);
            return;
        }
    }
    b->playing_ = false;
}

// ---- IDirectSound3DBuffer_Mac ----

IDirectSound3DBuffer_Mac::IDirectSound3DBuffer_Mac() {
    params_.size = 0x40;
    params_.minDistance = 1.0f;
    params_.maxDistance = 1.0e9f;
}

std::int32_t IDirectSound3DBuffer_Mac::QueryInterface(const GUID16&, void** out) {
    *out = nullptr;
    return DSERR_NOINTERFACE;
}

std::int64_t IDirectSound3DBuffer_Mac::Release() {
    const std::int64_t n = --refCount_;
    if (n == 0) {
        if (buffer_) buffer_->MacSetBuffer3DNull();
        delete this;
        return 0;
    }
    return n;
}

std::int32_t IDirectSound3DBuffer_Mac::GetAllParameters(_DS3DBUFFER* out) {
    if (!out || out->size != 0x40) return DSERR_INVALIDPARAM;
    *out = params_;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetConeAngles(std::uint32_t* inside, std::uint32_t* outside) {
    if (!inside || !outside) return DSERR_INVALIDPARAM;
    *inside = params_.insideConeAngle;
    *outside = params_.outsideConeAngle;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetConeOrientation(_D3DVECTOR* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.coneOrientation;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetConeOutsideVolume(std::int32_t* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.coneOutsideVolume;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetMaxDistance(float* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.maxDistance;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetMinDistance(float* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.minDistance;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetMode(std::uint32_t* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.mode;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetPosition(_D3DVECTOR* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.position;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::GetVelocity(_D3DVECTOR* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.velocity;
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetAllParameters(const _DS3DBUFFER* in, std::uint32_t mode) {
    if (!in || mode >= 2 || in->size != 0x40) return DSERR_INVALIDPARAM;
    params_ = *in;
    dirtyConeVolume_ = dirtyMode_ = true;
    dirtyPosition_ = dirtyVelocity_ = dirtyConeAngles_ = dirtyConeOrientation_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

void IDirectSound3DBuffer_Mac::MacUpdateListener() {
    buffer_->MacGetDirectSound()->MacGet3DListener()->MacCommitDeferredBufferSettings();
}

std::int32_t IDirectSound3DBuffer_Mac::SetConeAngles(std::uint32_t inside, std::uint32_t outside, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.insideConeAngle = inside;
    params_.outsideConeAngle = outside;
    dirtyConeAngles_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetConeOrientation(float x, float y, float z, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.coneOrientation = {x, y, z};
    dirtyConeOrientation_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetConeOutsideVolume(std::int32_t volume, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.coneOutsideVolume = volume;
    dirtyConeVolume_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetMaxDistance(float distance, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.maxDistance = distance;
    dirtyPosition_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetMinDistance(float distance, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.minDistance = distance;
    dirtyPosition_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetMode(std::uint32_t value, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.mode = value;
    dirtyMode_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetPosition(float x, float y, float z, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.position = {x, y, z};
    dirtyPosition_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

std::int32_t IDirectSound3DBuffer_Mac::SetVelocity(float x, float y, float z, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.velocity = {x, y, z};
    dirtyVelocity_ = true;
    if (mode == 0) MacUpdateListener();
    return 0;
}

void IDirectSound3DBuffer_Mac::MacApplyPosition() {
    if (!dirtyPosition_) return;
    float left = 0.0f, right = 0.0f, volume = 0.0f;
    float x = params_.position.x, y = params_.position.y, z = params_.position.z;
    float sideLeft = 0.0f, sideRight = 0.0f;
    if (x != 0.0f || z != 0.0f) {
        if (x == 0.0f) {
            sideLeft = sideRight = z >= 0.0f ? 1.0f : 0.3f;
            left = right = sideLeft;
        }
        if (z != 0.0f) {
            if (sideLeft == 0.0f) {
                IDirectSound3DListener_Mac* listener = buffer_->MacGetDirectSound()->MacGet3DListener();
                vector4 position;
                position.v[0] = x; position.v[1] = y; position.v[2] = z; position.v[3] = 0.0f;
                const vector4 rotated = listener->Matrix() * position;
                const float rx = rotated.v[0], ry = rotated.v[1], rz = rotated.v[2];
                int quadrant;
                if (rx > 0.0f) quadrant = (rz > 0.0f ? 1 : 0) + 2;
                else quadrant = (rz > 0.0f ? 3 : 0) + 1;
                float angle = GetXZAngle({rx, ry, rz}) + 1.5707964f;
                while (static_cast<double>(angle) > 6.283185308) angle = static_cast<float>(static_cast<double>(angle) + -6.283185308);
                const int current = quadrant == 4 ? 0 : quadrant;
                const int next = kNextQuadrant[current];
                const float t = (angle - kQuadrantAngle[current]) / 1.5707964f;
                left = (kPanTable[next][0] - kPanTable[current][0]) * t + kPanTable[current][0];
                left = left < 0.4f ? 0.4f : left;
                left = 1.0f < left ? 1.0f : left;
                right = (kPanTable[next][1] - kPanTable[current][1]) * t + kPanTable[current][1];
                right = right < 0.4f ? 0.4f : right;
                right = 1.0f < right ? 1.0f : right;
                x = params_.position.x;
                z = params_.position.z;
            }
        } else if (x >= 0.0f) {
            left = 0.4f;
            right = 1.0f;
        } else {
            left = 1.0f;
            right = 0.4f;
        }
    } else {
        left = right = 1.0f;
    }
    const float distance = std::sqrt(z * z + y * y + x * x);
    if (distance <= params_.minDistance) {
        volume = 1.0f;
    } else {
        const float clamped = distance <= params_.maxDistance ? distance : params_.maxDistance;
        volume = 1.0f / (clamped / params_.minDistance);
    }
    if (!(0.0f < volume)) volume = 0.0f;
    if (!(volume <= 1.0f)) volume = 1.0f;
    buffer_->MacSetPanVolumeMultipliers(left, right, volume);
    dirtyPosition_ = false;
}

// ---- IDirectSound3DListener_Mac ----

IDirectSound3DListener_Mac::IDirectSound3DListener_Mac() {
    params_.size = 0x40;
    params_.distanceFactor = 1.0f;
}

std::int32_t IDirectSound3DListener_Mac::QueryInterface(const GUID16&, void** out) {
    *out = nullptr;
    return DSERR_NOINTERFACE;
}

std::int32_t IDirectSound3DListener_Mac::Release() {
    const std::int32_t n = --refCount_;
    if (n != 0) return n;
    delete this;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::GetAllParameters(_DS3DLISTENER* out) {
    if (!out || out->size != 0x40) return DSERR_INVALIDPARAM;
    *out = params_;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::GetDistanceFactor(float* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.distanceFactor;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::GetDopplerFactor(float* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.dopplerFactor;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::GetOrientation(_D3DVECTOR* front, _D3DVECTOR* top) {
    if (!front || !top) return DSERR_INVALIDPARAM;
    *front = params_.orientFront;
    *top = params_.orientTop;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::GetPosition(_D3DVECTOR* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.position;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::GetRolloffFactor(float* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.rolloffFactor;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::GetVelocity(_D3DVECTOR* out) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = params_.velocity;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::SetAllParameters(const _DS3DLISTENER* in, std::uint32_t mode) {
    if (!in || mode >= 2 || in->size != 0x40) return DSERR_INVALIDPARAM;
    params_ = *in;
    dirtyVelocity_ = true;
    dirtyDistance_ = dirtyDoppler_ = dirtyMatrix_ = dirtyRolloff_ = true;
    if (mode == 0) CommitDeferredSettings();
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::SetDistanceFactor(float value, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.distanceFactor = value;
    dirtyDistance_ = mode != 0;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::SetDopplerFactor(float value, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.dopplerFactor = value;
    dirtyDoppler_ = mode != 0;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::SetOrientation(float fx, float fy, float fz, float tx, float ty, float tz,
                                                        std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.orientFront = {fx, fy, fz};
    params_.orientTop = {tx, ty, tz};
    dirtyMatrix_ = true;
    if (mode == 0) MacCreateListenerMatrix();
    return 0;
}

void IDirectSound3DListener_Mac::MacCreateListenerMatrix() {
    const float angle = static_cast<float>(static_cast<double>(GetXZAngle({params_.orientFront.x, params_.orientFront.y, params_.orientFront.z})) +
                                           -1.5707963267948966);
    matrix_ = RotateRadMatrix44('y', angle);
    matrix_.m[12] = -params_.position.x;
    matrix_.m[13] = -params_.position.y;
    matrix_.m[14] = -params_.position.z;
    if (owner_) {
        for (int i = 0; i < IDirectSound_Mac::MacGetSoundBufferMaxCount(); ++i) {
            IDirectSoundBuffer_Mac* buffer = IDirectSound_Mac::MacGetSoundBuffer(i);
            if (buffer) {
                if (IDirectSound3DBuffer_Mac* b3d = buffer->MacGet3DBuffer()) b3d->MarkPositionDirty();
            }
        }
    }
    dirtyMatrix_ = false;
}

std::int32_t IDirectSound3DListener_Mac::SetPosition(float x, float y, float z, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.position = {x, y, z};
    dirtyMatrix_ = true;
    if (mode == 0) MacCreateListenerMatrix();
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::SetRolloffFactor(float value, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.rolloffFactor = value;
    dirtyRolloff_ = mode != 0;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::SetVelocity(float x, float y, float z, std::uint32_t mode) {
    if (mode >= 2) return DSERR_INVALIDPARAM;
    params_.velocity = {x, y, z};
    dirtyVelocity_ = mode != 0;
    return 0;
}

std::int32_t IDirectSound3DListener_Mac::CommitDeferredSettings() {
    MacCommitDeferredBufferSettings();
    return 0;
}

void IDirectSound3DListener_Mac::MacCommitDeferredBufferSettings() {
    if (dirtyMatrix_) MacCreateListenerMatrix();
    dirtyVelocity_ = false;
    dirtyDistance_ = false;
    dirtyDoppler_ = false;
    dirtyRolloff_ = false;
    MacProcess3DBuffers();
}

void IDirectSound3DListener_Mac::MacProcess3DBuffers() {
    for (int i = 0; i < IDirectSound_Mac::MacGetSoundBufferMaxCount(); ++i) {
        IDirectSoundBuffer_Mac* buffer = IDirectSound_Mac::MacGetSoundBuffer(i);
        if (!buffer) continue;
        IDirectSound3DBuffer_Mac* b3d = buffer->MacGet3DBuffer();
        if (b3d && b3d->positionDirty()) b3d->MacApplyPosition();
    }
}
