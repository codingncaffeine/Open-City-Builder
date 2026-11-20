#include "audio/PCMPlayer.h"

bool CPCMPlayer::Open(std::uint16_t channels, std::uint16_t bitsPerSample, std::uint32_t sampleRate,
                      std::uint8_t* data, std::int32_t size) {
    Close();
    AudioPlatform::IBackend* backend = AudioPlatform::GetBackend();
    voice_.reset(backend ? backend->CreatePcmVoice(data, size, channels, sampleRate, bitsPerSample) : nullptr);
    delete[] data;
    if (voice_) {
        UpdateVolumeAndBalance();
        UpdateFrequency();
        return true;
    }
    return false;
}

void CPCMPlayer::Close() {
    if (voice_) {
        Stop();
        voice_.reset();
        originalFrequency_ = 0;
    }
}

void CPCMPlayer::Stop() {
    if (voice_ && playing_) {
        playing_ = false;
        voice_->Stop();
    }
}

void CPCMPlayer::Play(bool loop) {
    if (!voice_) return;
    Stop();
    UpdateVolumeAndBalance();
    loop_ = loop;
    playing_ = true;
    voice_->Play(loop_ ? -1 : 0);
}

void CPCMPlayer::SetVolume(float volume) {
    if (volume_ != volume || volume != volume) {
        volume_ = volume;
        UpdateVolumeAndBalance();
    }
}

void CPCMPlayer::SetVolumes(float left, float right) {
    if (leftVolume_ != left || rightVolume_ != right || left != left || right != right) {
        leftVolume_ = left;
        rightVolume_ = right;
        UpdateVolumeAndBalance();
    }
}

void CPCMPlayer::SetBalance(float balance) {
    if (balance_ != balance || balance != balance) {
        balance_ = balance;
        UpdateVolumeAndBalance();
    }
}

void CPCMPlayer::SetFrequency(std::uint32_t frequency) {
    if (frequency_ != frequency) {
        frequency_ = frequency;
        UpdateFrequency();
    }
}

// 3D sounds additionally pan according to the left/right volume ratio; 2D sounds only get volume.
void CPCMPlayer::UpdateVolumeAndBalance() {
    if (!voice_) return;
    voice_->SetVolume(volume_);
    if (is3D_) voice_->SetPan((rightVolume_ * 2.0f) / (leftVolume_ + rightVolume_) - 1.0f);
}

bool CQTRemotePlayer::Open(const char* path) {
    AudioPlatform::IBackend* backend = AudioPlatform::GetBackend();
    voice_.reset(backend ? backend->CreateFileVoice(path) : nullptr);
    return voice_ != nullptr;
}

void CQTRemotePlayer::Play(bool loop) {
    if (!voice_) return;
    voice_->Play(loop ? -1 : 0);
}

void CQTRemotePlayer::Stop() {
    if (voice_) voice_->Stop();
}
