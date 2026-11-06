#include "s3d/ModelInstanceAnimatedMesh.h"

#include "s3d/S3DMath.h"

namespace s3d {

bool cS3DModelInstanceAnimated::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIS3DModelInstanceAnimated) {
        *out = static_cast<cIS3DModelInstanceAnimated*>(this);
        AddRef();
        return true;
    }
    return cS3DModelInstance::QueryInterface(iid, out);
}

int cS3DModelInstanceAnimated::SetPosition(const cS3DVector3& position) {
    cS3DTransform t;
    GetTransform(t);
    nS3D::SetTranslation(t, position);
    SetTransform(t);
    return 1;
}

int cS3DModelInstanceAnimated::SetOrientation(const cS3DVector3& angles) {
    cS3DTransform t;
    GetTransform(t);
    nS3D::SetOrientationEuler(t, angles);
    SetTransform(t);
    return 0;
}

int cS3DModelInstanceAnimated::SetOrientation(const cS3DVector3& target, const cS3DVector3& up) {
    cS3DTransform t;
    GetTransform(t);
    const cS3DVector3 forward{t.translation[0] - target.x, t.translation[1] - target.y, t.translation[2] - target.z};
    nS3D::SetOrientationForwardUp(t, forward, up);
    t.changeCounter = std::uint8_t(t.changeCounter + 1);
    SetTransform(t);
    return 2;
}

void cS3DModelInstanceAnimated::ModelPointToWorldPoint(cS3DVector3& point) {
    cS3DTransform t;
    GetTransform(t);
    nS3D::ModelPointToWorldPoint(t, point);
}

void cS3DModelInstanceAnimated::ModelPointToWorldPoint(float* point) {
    cS3DVector3 p{point[0], point[1], point[2]};
    ModelPointToWorldPoint(p);
    point[0] = p.x;
    point[1] = p.y;
    point[2] = p.z;
}

void cS3DModelInstanceAnimated::ModelVectorToWorldVector(cS3DVector3& vector) {
    cS3DTransform t;
    GetTransform(t);
    nS3D::ModelVectorToWorldVector(t, vector);
}

void cS3DModelInstanceAnimated::ModelVectorToWorldVector(float* vector) {
    cS3DVector3 v{vector[0], vector[1], vector[2]};
    ModelVectorToWorldVector(v);
    vector[0] = v.x;
    vector[1] = v.y;
    vector[2] = v.z;
}

// The draw attributes are the static instance's (+0x7c flags, +0x7d highlight, +0x8c opacity).
void cS3DModelInstanceAnimated::PushAttributes() {
    SC4DrawAttributes attributes = GetDrawAttributes();
    attributes.flags = (attributes.flags & ~0xff07u) | (state_.flags & 7u) | (std::uint32_t(state_.highlight) << 8);
    attributes.shade[3] = state_.opacity;
    SetDrawAttributes(attributes);
}

int cS3DModelInstanceAnimated::SetOpacity(bool translucent, float opacity) {
    const SC4DrawAttributes current = GetDrawAttributes();
    state_.flags = std::uint8_t(current.flags & 0xff);
    const int result = state_.SetOpacity(translucent, opacity);
    PushAttributes();
    return result;
}

int cS3DModelInstanceAnimated::SetHighlight(int value) {
    const SC4DrawAttributes current = GetDrawAttributes();
    state_.flags = std::uint8_t(current.flags & 0xff);
    state_.highlight = std::uint8_t((current.flags >> 8) & 0xff);
    const int result = state_.SetHighlight(value);
    if (result) PushAttributes();
    return result;
}

int cS3DModelInstanceAnimated::SetSelfIlluminated(bool on) {
    const SC4DrawAttributes current = GetDrawAttributes();
    state_.flags = std::uint8_t(current.flags & 0xff);
    const int result = state_.SetSelfIlluminated(on);
    if (result) PushAttributes();
    return result;
}

int cS3DModelInstanceAnimated::ModelFrameCount() {
    cIS3DResModel* resource = GetResModel();
    return resource ? resource->FrameCount() : 1;
}

int cS3DModelInstanceAnimated::ModelDefaultRate() {
    cIS3DResModel* resource = GetResModel();
    return resource ? resource->FrameRate() : 0;
}

bool cS3DModelInstanceAnimated::LoadModelFrame(int frame) {
    cIS3DResModel* resource = GetResModel();
    return resource && resource->SelectFrame(this, frame);
}

}  // namespace s3d
