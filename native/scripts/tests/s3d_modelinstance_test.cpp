#include <cstdio>
#include <cstring>
#include <vector>

#include "s3d/ModelInstance.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace s3d;

namespace {
struct Owner final : cIGZUnknown {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};
struct Model final : cIS3DResModel {
    int refs = 1, computed = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void ComputeBounds(cS3DBoundingBox& out) override { ++computed; out = {{-1, -2, -3}, {4, 5, 6}}; }
    bool CreateModelInstance(std::uint32_t, cIS3DModelInstance** out) override { *out = nullptr; return false; }
};
struct Host final : IModelInstanceHost {
    std::vector<const char*> calls;
    cIS3DModelInstance* instance = nullptr;
    cS3DMeshAndMaterial* meshes = nullptr;
    SC4DrawAttributes attributes;
    bool flag = false;
    bool pickResult = false;
    bool ModelPick(cSC4PickContext*, cIS3DModelInstance* i, cS3DMeshAndMaterial* m) override { calls.push_back("pick"); instance = i; meshes = m; return pickResult; }
    void RenderModelInstance(SC4DrawContext*, cIS3DModelInstance* i, cS3DMeshAndMaterial* m, const SC4DrawAttributes* a, bool f) override {
        calls.push_back("render"); instance = i; meshes = m; attributes = *a; flag = f;
    }
    void TransformBoundingBox(const cS3DTransform& t, cS3DBoundingBox& box) override {
        calls.push_back("transform");
        for (int i = 0; i < 3; ++i) { box.min[i] += t.translation[i]; box.max[i] += t.translation[i]; }
    }
};
bool g_basicDeleted = false;
struct Basic final : cS3DModelInstanceBasic {
    int worldCalls = 0;
    ~Basic() override { g_basicDeleted = true; }
    void GetWorldBounds(cS3DBoundingBox& out) override { ++worldCalls; out = {{9, 9, 9}, {10, 10, 10}}; }
    void GetSortValue(std::uint32_t& out) const override { out = 77; }
};
}  // namespace

int main() {
    Host host;
    SetModelInstanceHost(&host);

    // ---- the bare base
    {
        auto* basic = new Basic();
        cIS3DModelInstance* face = basic;
        CHECK(face->AddRef() == 1 && face->AddRef() == 2);
        void* raw = nullptr;
        CHECK(face->QueryInterface(kGZIID_cIS3DModelInstance, &raw) && raw == face);   // a third reference
        CHECK(!face->QueryInterface(0x1234, &raw));
        CHECK(face->GetProperties() == 2 && face->GetResModel() == nullptr && face->GetCustomData() == nullptr && face->GetOwner() == nullptr);
        face->SetProperties(9);
        CHECK(face->GetProperties() == 9);
        SC4DrawAttributes attributes = face->GetDrawAttributes();
        CHECK(attributes.flags == 0 && attributes.shade[0] == 1.0f && attributes.shade[3] == 1.0f);
        attributes.flags = 5;
        attributes.shade[1] = 0.5f;
        face->SetDrawAttributes(attributes);
        CHECK(face->GetDrawAttributes().flags == 5 && face->GetDrawAttributes().shade[1] == 0.5f);
        int data = 3;
        face->SetCustomData(&data);
        CHECK(face->GetCustomData() == &data);
        Owner a, b;
        face->SetOwner(&a);
        CHECK(a.refs == 2 && face->GetOwner() == &a);
        face->SetOwner(&a);                                   // same owner: nothing changes
        CHECK(a.refs == 2);
        face->SetOwner(&b);
        CHECK(a.refs == 1 && b.refs == 2);
        CHECK(!face->Pick(nullptr) && face->Draw(nullptr, true) == 1);
        cS3DBoundingBox box;
        face->GetBounds(box);                                 // asks for the world bounds
        CHECK(basic->worldCalls == 1 && box.min[0] == 9);
        cS3DTransform t;
        t.flags = 3;
        t.translation[1] = 5;
        t.scale = 2;
        t.rotation[0][1] = 7;
        face->GetTransform(t);
        CHECK(t.flags == 0 && t.changeCounter == 1 && t.translation[1] == 0 && t.scale == 1 && t.rotation[0][1] == 0 && t.rotation[1][1] == 1);
        face->GetTransform(t);                                // already the identity
        CHECK(t.changeCounter == 1);
        face->SetTransform(t);                                // does nothing
        CHECK(face->Release() == 3 - 1 && face->Release() == 1);
        CHECK(!g_basicDeleted);
        face->Release();                                      // the last reference deletes it
        CHECK(g_basicDeleted && b.refs == 1);                 // the owner was released
    }

    // ---- the full instance
    {
        auto* instance = new cS3DModelInstance();
        instance->AddRef();
        cIS3DModelInstance* face = instance;
        void* raw = nullptr;
        CHECK(instance->QueryInterface(kGZIID_cIS3DModelInstance, &raw) && raw == static_cast<cIS3DModelInstance*>(instance)); face->Release();
        CHECK(instance->QueryInterface(kGZIID_cIS3DMeshAndMaterial, &raw) && raw == static_cast<cS3DMeshAndMaterial*>(instance)); face->Release();
        CHECK(!instance->QueryInterface(0x4321, &raw));
        CHECK(face->GetProperties() == 2 && face->GetResModel() == nullptr);
        cS3DBoundingBox box;
        face->GetBounds(box);
        CHECK(box.min[0] == 1 && box.min[2] == 1 && box.max[0] == 0 && box.max[2] == 0);   // the empty (inverted) box until a model is set

        Model model;
        instance->SetModelResource(&model);
        CHECK(model.refs == 2 && face->GetResModel() == &model);
        face->GetBounds(box);
        CHECK(model.computed == 1 && box.min[1] == -2 && box.max[2] == 6);
        face->GetBounds(box);
        CHECK(model.computed == 1);                          // cached
        instance->RecalculateBounds();
        CHECK(model.computed == 2);

        // transform
        cS3DTransform t;
        t.translation[0] = 3; t.translation[1] = 4; t.translation[2] = 5;
        t.scale = 2; t.flags = 7; t.rotation[1][1] = 0.5f;
        face->SetTransform(t);
        cS3DTransform out;
        out.flags = 0;
        face->GetTransform(out);
        CHECK(out.changeCounter == 1 && out.flags == 7 && out.scale == 2 && out.translation[2] == 5 && out.rotation[1][1] == 0.5f);
        cS3DBoundingBox world;
        face->GetWorldBounds(world);
        CHECK(host.calls.back() == std::string("transform") && world.min[0] == -1 + 3 && world.max[2] == 6 + 5);

        // draw attributes, custom data, properties
        SC4DrawAttributes attributes;
        attributes.flags = 0x21;
        face->SetDrawAttributes(attributes);
        CHECK(face->GetDrawAttributes().flags == 0x21);
        face->SetProperties(0x12345);
        CHECK(face->GetProperties() == 0x2345);               // kept as 16 bits
        face->SetProperties(2);
        int data = 1;
        face->SetCustomData(&data);
        CHECK(face->GetCustomData() == &data);

        // owner
        Owner owner;
        face->SetOwner(&owner);
        CHECK(owner.refs == 2 && face->GetOwner() == &owner);
        face->SetOwner(&owner);
        CHECK(owner.refs == 2);

        // picking and drawing go to the host with the instance and its mesh list
        CHECK(!face->Pick(nullptr) && host.calls.back() == std::string("pick") && host.instance == face && host.meshes == static_cast<cS3DMeshAndMaterial*>(instance));
        host.pickResult = true;                             // the instance answers what the model pick answers
        CHECK(face->Pick(nullptr));
        host.pickResult = false;
        CHECK(face->Draw(nullptr, true) == 1 && host.calls.back() == std::string("render") && host.flag && host.attributes.flags == 0x21);

        // sort value: the first material's texture binding
        std::uint32_t sort = 99;
        face->GetSortValue(sort);
        CHECK(sort == 0);                                     // no materials
        std::uint32_t textureId = 0xabcd1234;
        void* record = &textureId;
        void* binding = &record;
        instance->Materials().emplace_back();
        std::memset(instance->Materials().back().data(), 0, 0x50);
        std::memcpy(instance->Materials().back().data() + 0x18, &binding, sizeof binding);
        face->GetSortValue(sort);
        CHECK(sort == 0xabcd1234);

        // a model whose first material is a "decal" (kind 8, class 1) turns on translucency
        auto& material = instance->Materials().back();
        std::memset(material.data(), 0, 0x50);
        material[9] = 8; material[10] = 1; material[4] = 0xff;
        Model other;
        instance->SetModelResource(&other);
        CHECK(model.refs == 1 && other.refs == 2);
        CHECK(face->GetProperties() == 4 && (instance->Materials().back()[4] & 0x80) == 0 && face->GetDrawAttributes().flags == (0x21 | 0xe) && material[0x24] == 0);
        // ... and a material with the 0x10 flag only sets the property bits
        face->SetProperties(3);
        material[9] = 0; material[4] = 0x10;
        instance->SetModelResource(&other);
        CHECK(face->GetProperties() == 5);

        // shared reference count: releases down to the last one delete the instance and release owner and model
        CHECK(face->AddRef() == 2);
        CHECK(face->Release() == 1);
        face->Release();
        CHECK(owner.refs == 1 && other.refs == 1);
    }

    SetModelInstanceHost(nullptr);
    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
