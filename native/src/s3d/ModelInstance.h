// The model instances of the S3D engine: cIS3DModelInstance (iid 0x26a68449, the interface every drawable model answers),
// cS3DModelInstanceBasic (21 functions + virtual thunks at 0x1000d9296..0x1000d9612, a base with owner / custom data /
// draw attributes and do-nothing geometry) and cS3DModelInstance (0x1000d8966..0x1000d966c, the real one: a mesh and
// material list (cS3DMeshAndMaterial, iid 0x26a68448, whose reference count it shares) plus a transform, cached bounds
// of its model resource and the draw attributes). Both derive virtually from cIS3DModelInstance, so the interface sits at
// the end of the object (+0xc8 in cS3DModelInstance).
//
// cS3DModelInstance layout: +8.. mesh and material lists (cS3DMeshAndMaterial), +0x38 reference count, +0x40 owner,
// +0x48 model resource, +0x50 transform (change counter, flags, 3x3 rotation, translation, scale), +0x88 bounds
// (min, max), +0xa0 draw attributes, +0xb8 custom data, +0xc2 properties (bit 1 / 2 translucency flags), +0xc6 bounds dirty.
// Properties (a bit set kept as an int): the defaults are 2.
// Seams: ModelPick / RenderModelInstance (SC4ViewUtilities / SC4DrawContext) and TransformBoundingBox.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "gz/StreamOps.h"
#include "s3d/MeshAndMaterial.h"

class cSC4PickContext;
class SC4DrawContext;

namespace s3d {

constexpr std::uint32_t kGZIID_cIS3DModelInstance = 0x26a68449;

struct SC4DrawAttributes {   // 0x14 bytes
    std::uint32_t flags = 0;
    float shade[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

class cIS3DModelInstance;

// The model resource as the instance uses it: slot 4 creates a model instance from it, slot 7 computes its bounding
// box. (cSC4ModelMaker::AddModel 0x100314328 calls slot 7 then slot 4 with a zero flag word.)
// One mesh of a model's first level of detail as the shadow code reads it (port seam: the original walks the model's mesh list through
// vslots 0x18 / 0x28 / 0x38 of the object resource model's vslot 0x30(0)).
struct ShadowMeshData {
    std::vector<cS3DVector3> points;     // the vertex positions
    std::vector<cS3DVector2> uvs;        // the first texture coordinates
    std::uint32_t textureLayers = 0;     // the material's layer count (material + 0xb): a mesh without a texture casts no shadow
    std::uint64_t texture = 0;           // the first layer's texture binding (material + 0x18), opaque to the model maker
};

class cIS3DResModel : public cIGZUnknown {
public:
    virtual void Slot3() {}
    virtual bool CreateModelInstance(std::uint32_t flags, cIS3DModelInstance** out) = 0;   // [4] +0x20
    virtual void Slot5() {}
    virtual void Slot6() {}
    virtual void ComputeBounds(cS3DBoundingBox& out) = 0;   // [7]
    // The shadow code's view of the model: false when the model has no mesh list for level 0.
    virtual bool GetShadowMeshes(std::vector<ShadowMeshData>& out) { out.clear(); return false; }
    // Frame animation (the ANIM chunk; port seam for cIS3DModelInstanceAnimated's frame loads): the frame count, the
    // chunk's rate word (milliseconds per frame) and a frame load into an instance this model created.
    virtual int FrameCount() const { return 1; }
    virtual int FrameRate() const { return 0; }
    virtual bool SelectFrame(cIS3DModelInstance* instance, int frame) { (void)instance; (void)frame; return false; }
};

class cIS3DModelInstance : public cIGZUnknown {
public:
    virtual cIGZUnknown* GetOwner() = 0;                                     // [3]
    virtual void SetOwner(cIGZUnknown* owner) = 0;                           // [4]
    virtual void* GetCustomData() = 0;                                       // [5]
    virtual void SetCustomData(void* data) = 0;                              // [6]
    virtual void GetBounds(cS3DBoundingBox& out) = 0;                        // [7]
    virtual void GetWorldBounds(cS3DBoundingBox& out) = 0;                   // [8]
    virtual void GetTransform(cS3DTransform& out) = 0;                       // [9]
    virtual void SetTransform(const cS3DTransform& transform) = 0;           // [10]
    virtual bool Pick(cSC4PickContext* context) = 0;                         // [11]
    virtual int Draw(SC4DrawContext* context, bool flag) = 0;                // [12]
    virtual int GetProperties() = 0;                                         // [13]
    virtual void SetProperties(int properties) = 0;                          // [14]
    virtual SC4DrawAttributes GetDrawAttributes() const = 0;                 // [15]
    virtual void SetDrawAttributes(const SC4DrawAttributes& attributes) = 0;   // [16]
    virtual void GetSortValue(std::uint32_t& out) const = 0;                 // [17]
    virtual cIS3DResModel* GetResModel() const = 0;                          // [18]
};

class IModelInstanceHost {
public:
    virtual ~IModelInstanceHost() = default;
    virtual void ModelPick(cSC4PickContext* context, cIS3DModelInstance* instance, cS3DMeshAndMaterial* meshes) = 0;
    virtual void RenderModelInstance(SC4DrawContext* context, cIS3DModelInstance* instance, cS3DMeshAndMaterial* meshes,
                                     const SC4DrawAttributes* attributes, bool flag) = 0;
    virtual void TransformBoundingBox(const cS3DTransform& transform, cS3DBoundingBox& box) = 0;
};
IModelInstanceHost* ModelInstanceHost();
void SetModelInstanceHost(IModelInstanceHost* host);

// A base class for instances with no geometry of their own (GetWorldBounds stays pure).
class cS3DModelInstanceBasic : public virtual cIS3DModelInstance {
public:
    cS3DModelInstanceBasic() = default;   // @ 0x1000d9296
    virtual ~cS3DModelInstanceBasic();   // @ 0x1000d92dc

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1000d95e2
    std::uint32_t AddRef() override { return ++refCount_; }        // @ 0x1000d955a
    std::uint32_t Release() override;                              // @ 0x1000d958a
    cIGZUnknown* GetOwner() override { return owner_; }            // @ 0x1000d9324
    void SetOwner(cIGZUnknown* owner) override;                    // @ 0x1000d9340
    void* GetCustomData() override { return customData_; }         // @ 0x1000d93c2
    void SetCustomData(void* data) override { customData_ = data; }   // @ 0x1000d93de
    void GetBounds(cS3DBoundingBox& out) override { GetWorldBounds(out); }   // @ 0x1000d93fa (asks for the world bounds)
    void GetTransform(cS3DTransform& out) override;                // @ 0x1000d9426
    void SetTransform(const cS3DTransform&) override {}            // @ 0x1000d945e
    bool Pick(cSC4PickContext*) override { return false; }         // @ 0x1000d946a
    int Draw(SC4DrawContext*, bool) override { return 1; }         // @ 0x1000d947a
    int GetProperties() override { return properties_; }           // @ 0x1000d9492
    void SetProperties(int properties) override { properties_ = properties; }   // @ 0x1000d94ae
    SC4DrawAttributes GetDrawAttributes() const override { return attributes_; }   // @ 0x1000d94cc
    void SetDrawAttributes(const SC4DrawAttributes& attributes) override { attributes_ = attributes; }   // @ 0x1000d9504
    cIS3DResModel* GetResModel() const override { return nullptr; }   // @ 0x1001634a0

private:
    cIGZUnknown* owner_ = nullptr;           // +8
    void* customData_ = nullptr;             // +0x10
    std::uint16_t refCount_ = 0;             // +0x18
    int properties_ = 2;                     // +0x1c
    SC4DrawAttributes attributes_;           // +0x20
};

class cS3DModelInstance : public cS3DMeshAndMaterial, public virtual cIS3DModelInstance {
public:
    cS3DModelInstance();   // @ 0x1000d8966
    ~cS3DModelInstance() override;   // @ 0x1000d8b10

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1000d919e
    std::uint32_t AddRef() override { return cS3DMeshAndMaterial::AddRef(); }
    std::uint32_t Release() override { return cS3DMeshAndMaterial::Release(); }   // @ 0x1001a2d4c

    cIGZUnknown* GetOwner() override { return owner_; }            // @ 0x1000d8d66
    void SetOwner(cIGZUnknown* owner) override;                    // @ 0x1000d8d82
    void* GetCustomData() override { return customData_; }         // @ 0x1000d8e24
    void SetCustomData(void* data) override { customData_ = data; }   // @ 0x1000d8e48
    void GetBounds(cS3DBoundingBox& out) override;                 // @ 0x1000d8e6c
    void GetWorldBounds(cS3DBoundingBox& out) override;            // @ 0x1000d8f20
    void GetTransform(cS3DTransform& out) override;                // @ 0x1000d8fb4
    void SetTransform(const cS3DTransform& transform) override;    // @ 0x1000d9028
    bool Pick(cSC4PickContext* context) override;                  // @ 0x1000d909e
    int Draw(SC4DrawContext* context, bool flag) override;         // @ 0x1000d90e4
    int GetProperties() override { return properties_; }           // @ 0x1000d9154
    void SetProperties(int properties) override { properties_ = static_cast<std::uint16_t>(properties); }   // @ 0x1000d9178
    SC4DrawAttributes GetDrawAttributes() const override { return attributes_; }   // @ 0x1000d8c5e
    void SetDrawAttributes(const SC4DrawAttributes& attributes) override { attributes_ = attributes; }   // @ 0x1000d8ca2
    void GetSortValue(std::uint32_t& out) const override;          // @ 0x1000d8d16
    cIS3DResModel* GetResModel() const override { return resModel_; }   // @ 0x1001a6fdc

    void RecalculateBounds();                                      // @ 0x1000d8ee0
    void SetModelResource(cIS3DResModel* resource);                // @ 0x1000d91ea

private:
    cIGZUnknown* owner_ = nullptr;           // +0x40
    cIS3DResModel* resModel_ = nullptr;      // +0x48
    cS3DTransform transform_;                // +0x50
    cS3DBoundingBox bounds_{{1, 1, 1}, {0, 0, 0}};   // +0x88
    SC4DrawAttributes attributes_;           // +0xa0
    void* customData_ = nullptr;             // +0xb8
    [[maybe_unused]] std::uint16_t word_c0_ = 0;   // +0xc0
    std::uint16_t properties_ = 2;           // +0xc2
    [[maybe_unused]] bool flag_c4_ = true, flag_c5_ = false;   // +0xc4, +0xc5
    bool boundsDirty_ = true;                // +0xc6
};

}  // namespace s3d
