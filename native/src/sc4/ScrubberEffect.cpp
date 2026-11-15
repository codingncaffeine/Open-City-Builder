#include "sc4/ScrubberEffect.h"

#include <algorithm>
#include <cmath>

#include "s3d/S3DMath.h"

namespace sc4 {

namespace {

IScrubberHost* g_host = nullptr;

// The binary's two float -> int idioms: (v - 0.5 + 12582912) read as an integer is the floor, (v + 0.5 + 12582912) the ceiling.
std::int32_t Floor(float value) { return static_cast<std::int32_t>(std::floor(value)); }
std::int32_t Ceil(float value) { return static_cast<std::int32_t>(std::ceil(value)); }

constexpr float kCellsPerUnit = 0.0625f;   // 0x00a70700
constexpr float kCellWidth = 16.0f;        // 0x00a70778 (and its negative 0x00a70b00)

}  // namespace

IScrubberHost* ScrubberHost() { return g_host; }
void SetScrubberHost(IScrubberHost* host) { g_host = host; }

bool ScrubberFilterIncludes(std::uint32_t flags, float minSize, float maxSize, const ScrubberOccupant& occupant) {
    if (occupant.signpost || occupant.textDecal) return false;
    if ((flags & kScrubberLeaveNetworks) != 0 && occupant.network) return false;
    if ((flags & kScrubberLeaveFlora) != 0 && occupant.type == kScrubberOccupantTypeFlora) return false;
    if (0.0f < maxSize || 0.0f < minSize) {
        const float size[3] = {occupant.boxMax[0] - occupant.boxMin[0], occupant.boxMax[1] - occupant.boxMin[1],
                               occupant.boxMax[2] - occupant.boxMin[2]};
        if (0.0f < maxSize && maxSize <= std::max(size[0], std::max(size[1], size[2]))) return false;
        if (0.0f < minSize && std::min(size[0], std::min(size[1], size[2])) <= minSize) return false;
    }
    return true;
}

cSC4ScrubberEffect::cSC4ScrubberEffect(cSC4ScrubberDescription* description) : description_(description) {
    if (description_) {
        description_->AddRef();
        messageData_ = description_->u44;
    }
}

cSC4ScrubberEffect::~cSC4ScrubberEffect() {
    if (description_) description_->Release();
}

// Nothing starts without a simulator.
void cSC4ScrubberEffect::Start(int) {
    if (started_ || !g_host || !g_host->HasSimulator()) return;
    started_ = true;
    applied_ = 0;
    blobInMap_ = false;
    const std::uint32_t flags = description_->u18;
    if (flags & kScrubberPause) g_host->SimulatorPause(true);
    if (flags & kScrubberHiddenPause) g_host->SimulatorHiddenPause(true);
    if (flags & kScrubberClockHiddenPause) g_host->ClockHiddenPause(true);
    elapsed_ = 0.0f;
}

void cSC4ScrubberEffect::Stop(int) {
    if (!started_ || !g_host || !g_host->HasSimulator()) return;
    started_ = false;
    const std::uint32_t flags = description_->u18;
    if (flags & kScrubberPause) g_host->SimulatorPause(false);
    if (flags & kScrubberHiddenPause) g_host->SimulatorHiddenPause(false);
    if (flags & kScrubberClockHiddenPause) g_host->ClockHiddenPause(false);
    if (blobInMap_) RemoveLastBlob();
}

bool cSC4ScrubberEffect::SetVoidParams(int type, void** values, int) {
    if (type != kScrubberParamMessageData) return false;
    messageData_ = reinterpret_cast<std::uintptr_t>(*values);
    return true;
}

// The effect's place in the world; entering another cell arms the next application, and the blob's cells follow.
void cSC4ScrubberEffect::SetTransforms(const cS3DTransform& parent, const cS3DTransform& local) {
    cS3DTransform world = local;
    world.changeCounter = 0;
    nS3D::MultiplyTransform(world, parent);
    position_ = ComposeTranslation(parent, local);
    const std::int32_t cellX = Floor(position_.x * kCellsPerUnit), cellZ = Floor(position_.z * kCellsPerUnit);
    if (cellX == cellX_ && cellZ == cellZ_) return;
    cellX_ = cellX;
    cellZ_ = cellZ;
    cellChanged_ = true;
    if (description_->u48 == 0) return;
    const cS3DVector3 corners[2] = {{-kCellWidth * description_->v50.x, 0.0f, -kCellWidth * description_->v50.y},
                                    {kCellWidth * description_->v50.x, 0.0f, kCellWidth * description_->v50.y}};
    cS3DVector3 placed[2];
    nS3D::TransformPoints(world, corners, placed, 2);
    blob_[0] = Floor(std::min(placed[0].x, placed[1].x) * kCellsPerUnit);
    blob_[2] = Floor(std::max(placed[0].x, placed[1].x) * kCellsPerUnit);
    blob_[1] = Floor(std::min(placed[0].z, placed[1].z) * kCellsPerUnit);
    blob_[3] = Floor(std::max(placed[0].z, placed[1].z) * kCellsPerUnit);
}

void cSC4ScrubberEffect::ApplyEffect(float elapsed) {
    if (description_->u18 & kScrubberOnce) {
        DoScrubberEffect();
        started_ = false;
        return;
    }
    elapsed_ += elapsed;
    if (0.0f < description_->f5c && description_->f5c < elapsed_) {
        Stop(0);
        return;
    }
    if (!cellChanged_) return;
    DoScrubberEffect();
    ++applied_;
    cellChanged_ = false;
}

void cSC4ScrubberEffect::DoScrubberEffect() {
    if (!g_host) return;
    if (description_->u34 != 0) DoBurn();
    if (description_->u38 != 0) DoToxic();
    if (description_->u24 != 0) DoDemolish();
    if (description_->u3c != 0) DoExtinguish();
    if (description_->u40 != 0) g_host->PostScrubMessage(description_->u40, messageData_, cellX_, cellZ_);
    if (description_->u48 == 0) return;
    if (applied_ != 0) RemoveLastBlob();
    AddBlobToMap();
}

void cSC4ScrubberEffect::DoBurn() { g_host->Burn(cellX_, cellZ_, description_->u34); }
void cSC4ScrubberEffect::DoToxic() { g_host->SpillToxic(cellX_, cellZ_); }
void cSC4ScrubberEffect::DoExtinguish() { g_host->Extinguish(cellX_, cellZ_, static_cast<std::int32_t>(description_->u3c)); }

// The square of cells around the scrubber, cut to the city; a square wholly outside the city becomes the cell (0, 0)
// (the original's own fallback).
void cSC4ScrubberEffect::DoDemolish() {
    const std::int32_t radius = static_cast<std::int32_t>(description_->u24);
    std::int32_t rect[4] = {cellX_ - radius, cellZ_ - radius, cellX_ + radius, cellZ_ + radius};
    const std::int32_t lastX = g_host->CityCellCountX() - 1, lastZ = g_host->CityCellCountZ() - 1;
    if (lastX < rect[0] || rect[2] < 0 || lastZ < rect[1] || rect[3] < 0) {
        rect[0] = rect[1] = rect[2] = rect[3] = 0;
    } else {
        if (rect[0] < 0) rect[0] = 0;
        if (rect[1] < 0) rect[1] = 0;
        if (lastX < rect[2]) rect[2] = lastX;
        if (lastZ < rect[3]) rect[3] = lastZ;
    }
    g_host->DemolishRegion(rect, description_->u28, (description_->u18 & kScrubberDemolishOption) != 0, description_->u18, description_->f2c,
                           description_->f30);
}

void cSC4ScrubberEffect::AddBlobToMap() {
    g_host->AddRectToEffectMap(description_->u48, blob_, description_->f4c, static_cast<std::uint32_t>(Ceil(description_->f58)));
    lastBlobValue_ = description_->f4c;
    std::copy(blob_, blob_ + 4, lastBlob_);
    blobInMap_ = true;
}

void cSC4ScrubberEffect::RemoveLastBlob() {
    g_host->AddRectToEffectMap(description_->u48, lastBlob_, -lastBlobValue_, static_cast<std::uint32_t>(Ceil(description_->f58)));
    blobInMap_ = false;
}

}  // namespace sc4
