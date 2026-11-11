#include "sc4/FloraOccupant.h"

#include "gz/StreamOps.h"

namespace sc4 {

namespace {
IFloraHost* g_floraHost = nullptr;
}
IFloraHost* FloraHost() { return g_floraHost; }
void SetFloraHost(IFloraHost* host) { g_floraHost = host; }

cSC4FloraOccupant::cSC4FloraOccupant() {
    type_ = kOccupantTypeFlora;
    flags_ |= kOccupantFlagFlora;
}

cSC4FloraOccupant::~cSC4FloraOccupant() {
    if (data_) data_->Release();
}

bool cSC4FloraOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4FloraOccupant || iid == 0xc83dd057) {   // 0xc83dd057: the interface id the original asks for
        *out = static_cast<cISC4FloraOccupant*>(this);
        AddRef();
        return true;
    }
    return cSC4Occupant::QueryInterface(iid, out);
}

bool cSC4FloraOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(3);
    if (cSC4Occupant::Write(stream)) {
        stream.SetUint32(floraType_);
        stream << cS3DVector3{position_[0], position_[1], position_[2]};
        stream.SetUint32(birthDate_);
        stream.SetUint32(lastSeedingDate_);
        stream.SetSint8(state_);
        stream.SetUint8(orientation_);
        stream.SetUint32(lotTag_);
    }
    return !stream.GetError();
}

bool cSC4FloraOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 3) return false;
    if (cSC4Occupant::Read(stream)) {
        stream.GetUint32(floraType_);
        cS3DVector3 p;
        stream >> p;
        position_[0] = p.x; position_[1] = p.y; position_[2] = p.z;
        stream.GetUint32(birthDate_);
        stream.GetUint32(lastSeedingDate_);
        std::int8_t state = 0;
        stream.GetSint8(state);
        state_ = state;
        stream.GetUint8(orientation_);
        stream.GetUint32(lotTag_);
    }
    return !stream.GetError();
}

void cSC4FloraOccupant::GetPosition(float* xyz) { for (int i = 0; i < 3; ++i) xyz[i] = position_[i]; }
void cSC4FloraOccupant::SetPosition(const float* xyz) { for (int i = 0; i < 3; ++i) position_[i] = xyz[i]; }

void cSC4FloraOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {
    if (!data_ && g_floraHost) SetStaticFloraData(g_floraHost->StaticFloraData(floraType_));
    const float half = data_ ? data_->HalfExtent() : 1.0f;
    const float height = data_ ? data_->Height() : 1.0f;
    minXyz[0] = position_[0] - half;
    minXyz[1] = position_[1];
    minXyz[2] = position_[2] - half;
    maxXyz[0] = position_[0] + half;
    maxXyz[1] = height + position_[1];
    maxXyz[2] = half + position_[2];
}

bool cSC4FloraOccupant::GetLotTag(std::uint32_t& tag) const {
    if (lotTag_ == 0xffffffffu) return false;
    tag = lotTag_;
    return true;
}

void cSC4FloraOccupant::SetStaticFloraData(cSC4StaticFloraData* data) {
    if (data_ == data) return;
    if (data) data->AddRef();
    if (data_) data_->Release();
    data_ = data;
    SetDefaultExemplar(data ? data->exemplar : nullptr);
}

}  // namespace sc4
