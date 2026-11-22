#include "s3d/ModelInstance.h"

namespace s3d {

namespace {
IModelInstanceHost* g_host = nullptr;
}
IModelInstanceHost* ModelInstanceHost() { return g_host; }
void SetModelInstanceHost(IModelInstanceHost* host) { g_host = host; }

// ------------------------------------------------------------------------------------------ cS3DModelInstanceBasic

cS3DModelInstanceBasic::~cS3DModelInstanceBasic() {   // @ 0x1000d92dc
    if (owner_) {
        owner_->Release();
        owner_ = nullptr;
    }
}

bool cS3DModelInstanceBasic::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x1000d95e2
    if (iid != kGZIID_cIS3DModelInstance && iid != 1) return false;
    *out = static_cast<cIS3DModelInstance*>(this);
    AddRef();
    return true;
}

std::uint32_t cS3DModelInstanceBasic::Release() {   // @ 0x1000d958a
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

void cS3DModelInstanceBasic::SetOwner(cIGZUnknown* owner) {   // @ 0x1000d9340
    if (owner_ != owner) {
        if (owner_) owner_->Release();
        if (owner) owner->AddRef();
    }
    owner_ = owner;
}

void cS3DModelInstanceBasic::GetTransform(cS3DTransform& out) {   // @ 0x1000d9426
    if (out.flags == 0) return;   // already the identity
    out.rotation[0][0] = out.rotation[1][1] = out.rotation[2][2] = 1.0f;
    out.rotation[0][1] = out.rotation[0][2] = out.rotation[1][0] = out.rotation[1][2] = out.rotation[2][0] = out.rotation[2][1] = 0.0f;
    out.translation[0] = out.translation[1] = out.translation[2] = 0.0f;
    out.scale = 1.0f;
    out.flags = 0;
    ++out.changeCounter;
}

// ------------------------------------------------------------------------------------------------ cS3DModelInstance

cS3DModelInstance::cS3DModelInstance() = default;   // @ 0x1000d8966 (the field defaults carry the original's initial values)

cS3DModelInstance::~cS3DModelInstance() {   // @ 0x1000d8b10
    if (resModel_) {
        cIS3DResModel* model = resModel_;
        resModel_ = nullptr;
        model->Release();
    }
    if (owner_) {
        cIGZUnknown* owner = owner_;
        owner_ = nullptr;
        owner->Release();
    }
}

bool cS3DModelInstance::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x1000d919e
    if (iid == kGZIID_cIS3DModelInstance) {
        *out = static_cast<cIS3DModelInstance*>(this);
    } else if (iid == kGZIID_cIS3DMeshAndMaterial || iid == 1) {
        *out = static_cast<cS3DMeshAndMaterial*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

void cS3DModelInstance::SetOwner(cIGZUnknown* owner) {   // @ 0x1000d8d82
    cIGZUnknown* previous = owner_;
    if (previous == owner) return;
    if (owner) owner->AddRef();
    owner_ = owner;
    if (previous) previous->Release();
}

void cS3DModelInstance::GetBounds(cS3DBoundingBox& out) {   // @ 0x1000d8e6c
    if (boundsDirty_ && resModel_) {
        resModel_->ComputeBounds(bounds_);
        boundsDirty_ = false;
    }
    out = bounds_;
}

void cS3DModelInstance::RecalculateBounds() {   // @ 0x1000d8ee0
    if (resModel_) {
        resModel_->ComputeBounds(bounds_);
        boundsDirty_ = false;
    }
}

void cS3DModelInstance::GetWorldBounds(cS3DBoundingBox& out) {   // @ 0x1000d8f20
    GetBounds(out);
    if (IModelInstanceHost* host = ModelInstanceHost()) host->TransformBoundingBox(transform_, out);
}

void cS3DModelInstance::GetTransform(cS3DTransform& out) {   // @ 0x1000d8fb4
    ++out.changeCounter;   // the caller's counter moves, ours is not copied
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) out.rotation[row][column] = transform_.rotation[row][column];
    for (int i = 0; i < 3; ++i) out.translation[i] = transform_.translation[i];
    out.scale = transform_.scale;
    out.flags = transform_.flags;
}

void cS3DModelInstance::SetTransform(const cS3DTransform& transform) {   // @ 0x1000d9028
    ++transform_.changeCounter;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) transform_.rotation[row][column] = transform.rotation[row][column];
    for (int i = 0; i < 3; ++i) transform_.translation[i] = transform.translation[i];
    transform_.scale = transform.scale;
    transform_.flags = transform.flags;
}

bool cS3DModelInstance::Pick(cSC4PickContext* context) {   // @ 0x1000d909e (i386 0x0009f834: returns ModelPick's answer)
    IModelInstanceHost* host = ModelInstanceHost();
    return host && host->ModelPick(context, this, this);
}

int cS3DModelInstance::Draw(SC4DrawContext* context, bool flag) {   // @ 0x1000d90e4
    if (IModelInstanceHost* host = ModelInstanceHost()) host->RenderModelInstance(context, this, this, &attributes_, flag);
    return 1;
}

void cS3DModelInstance::GetSortValue(std::uint32_t& out) const {   // @ 0x1000d8d16
    out = 0;
    if (materials_.empty()) return;
    // The first material's texture binding (record +0x18) points at a record whose first word is the texture id.
    const void* binding = nullptr;
    std::memcpy(&binding, materials_.front().data() + 0x18, sizeof binding);
    if (!binding) return;
    const void* record = *static_cast<const void* const*>(binding);
    if (record) out = *static_cast<const std::uint32_t*>(record);
}

void cS3DModelInstance::SetModelResource(cIS3DResModel* resource) {   // @ 0x1000d91ea
    cIS3DResModel* previous = resModel_;
    if (previous != resource) {
        if (resource) resource->AddRef();
        resModel_ = resource;
        if (previous) previous->Release();
    }
    if (materials_.empty()) return;
    std::uint8_t* material = materials_.front().data();
    if (material[9] == 8 && material[10] == 1) {
        properties_ = static_cast<std::uint16_t>((properties_ & 0xfff9) | 4);
        material[4] &= 0x7f;
        attributes_.flags |= 0xe;
        material[0x24] = 0;
    } else if (material[4] & 0x10) {
        properties_ = static_cast<std::uint16_t>((properties_ & 0xfff9) | 4);
    }
}

}  // namespace s3d
