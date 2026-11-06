// cS3DModelInstanceAnimated (i386 0x00137d70..): a static S3D instance that plays its resource's ANIM frames.
#include <cstdio>
#include <vector>

#include "s3d/ModelInstanceAnimatedMesh.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace s3d;

namespace {
struct Model final : cIS3DResModel {
    int refs = 1, frames = 4, rate = 250;
    std::vector<int> selected;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void ComputeBounds(cS3DBoundingBox& out) override { out = {{0, 0, 0}, {1, 1, 1}}; }
    bool CreateModelInstance(std::uint32_t, cIS3DModelInstance** out) override { *out = nullptr; return false; }
    int FrameCount() const override { return frames; }
    int FrameRate() const override { return rate; }
    bool SelectFrame(cIS3DModelInstance*, int frame) override { selected.push_back(frame); return frame < frames; }
};
}  // namespace

int main() {
    Model model;
    auto* instance = new cS3DModelInstanceAnimated();
    instance->AddRef();
    instance->SetModelResource(&model);

    // the animated interface answers 0xa818edd8 and is the same object as the model instance
    void* raw = nullptr;
    CHECK(instance->QueryInterface(kGZIID_cIS3DModelInstanceAnimated, &raw) && raw == static_cast<cIS3DModelInstanceAnimated*>(instance));
    if (raw) static_cast<cIS3DModelInstanceAnimated*>(raw)->Release();
    CHECK(instance->AsModelInstance() == static_cast<cIS3DModelInstance*>(instance));
    CHECK(instance->GetFrameCount() == 4 && instance->GetFrame() == 0);

    // the model maker's 0.01 frames per millisecond: 100 ms = one frame, loaded through the resource
    instance->SetAutoFrameRate(0.01f);
    CHECK(instance->DoAutoFrameUpdate(50.0f) == 0 && model.selected.empty());
    CHECK(instance->DoAutoFrameUpdate(50.0f) == 2 && model.selected.size() == 1 && model.selected[0] == 1 && instance->GetFrame() == 1);
    for (int i = 0; i < 3; ++i) instance->DoAutoFrameUpdate(100.0f);
    CHECK(instance->GetFrame() == 0 && model.selected.back() == 0);   // loop (the constructor's play mode 3) wraps to 0

    // ping-pong turns back at the last frame
    instance->SetPlayMode(kPlayPingPong);
    for (int i = 0; i < 3; ++i) instance->IncrementFrame();
    CHECK(instance->GetFrame() == 3);
    instance->IncrementFrame();
    CHECK(instance->GetFrame() == 2);

    // the default rate is the ANIM section's value x 0.001
    instance->SetAutoFrameRate(-200.0f);
    CHECK(instance->State().autoFrameRate == 0.25f);

    // the draw attributes are the static instance's: translucent bit + shade alpha, highlight bit + value
    CHECK(instance->SetOpacity(true, 0.5f) == 2);
    SC4DrawAttributes a = instance->GetDrawAttributes();
    CHECK((a.flags & 2) && a.shade[3] == 0.5f);
    instance->SetOpacity(false, 0.5f);
    a = instance->GetDrawAttributes();
    CHECK(!(a.flags & 2) && a.shade[3] == 1.0f);
    CHECK(instance->SetHighlight(3) == 2 && instance->SetHighlight(3) == 0);
    a = instance->GetDrawAttributes();
    CHECK((a.flags & 1) && ((a.flags >> 8) & 0xff) == 3);
    CHECK(instance->SetSelfIlluminated(true) == 2 && (instance->GetDrawAttributes().flags & 4));

    // position goes into the instance's transform
    instance->SetPosition(cS3DVector3{10.0f, 20.0f, 30.0f});
    cS3DTransform t;
    instance->GetTransform(t);
    CHECK(t.translation[0] == 10.0f && t.translation[1] == 20.0f && t.translation[2] == 30.0f);

    instance->Release();
    std::puts(failures ? "s3d_modelinstanceanimatedmesh: FAILED" : "s3d_modelinstanceanimatedmesh ok");
    return failures != 0;
}
