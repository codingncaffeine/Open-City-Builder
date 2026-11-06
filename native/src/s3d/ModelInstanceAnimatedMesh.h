// cS3DModelInstanceAnimated — an S3D model instance whose meshes play the model's ANIM frames (i386 0x00137d70..0x0013a17a,
// made by cS3DResModel::GetModelInstance(id, cIS3DModelInstanceAnimated**) 0x0034df5e = resource slot 5). It is the static
// cS3DModelInstance (same mesh / material fill, same draw) plus the frame state of s3d/AnimatedInstance: a frame change
// loads that frame's meshes through the resource (GetModelFrame, vslot 0x40 = cIS3DResModel::SelectFrame here).
//
// Original layout (i386): +0 the animated interface, +4 cS3DModelInstance (transform at +0x2c.., draw attributes +0x7c:
// flags byte, +0x7d highlight, +0x8c opacity), +0x94 loaded frame, +0x96 model flags (1), +0x9c frameset info, +0xa0 zoom ->
// instance table, +0xa4 key, +0xac current instance, +0xb0 zoom, +0xb1 rotation (0xff), +0xb4 auto rate (-100), +0xb8
// accumulator, +0xbc play mode (3 loop), +0xbd direction (1), +0xbe catch up (0).
//
// Quirks kept: nothing fills the frameset info or the zoom table (GetModelInstance leaves them null), so frames run over
// the whole model and SetZoomAndRotation only stores the rotation. The opacity / highlight / self-lit setters write the
// static instance's draw attributes (flags bit 0 highlighted, bit 1 translucent, bit 2 self-lit; highlight in bits 8..15;
// opacity = shade alpha).
#pragma once

#include "s3d/AnimatedInstance.h"
#include "s3d/ModelInstance.h"
#include "s3d/ModelInstanceAnimatedSprite.h"

namespace s3d {

class cS3DModelInstanceAnimated final : public cIS3DModelInstanceAnimated, public cS3DModelInstance, private IAnimatedModelHost {
public:
    cS3DModelInstanceAnimated() : state_(this) {}

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x00137d70
    std::uint32_t AddRef() override { return cS3DModelInstance::AddRef(); }
    std::uint32_t Release() override { return cS3DModelInstance::Release(); }

    cIS3DModelInstance* AsModelInstance() override { return static_cast<cIS3DModelInstance*>(this); }
    int SetPosition(const cS3DVector3& position) override;                       // 0x00139766
    int SetOrientation(const cS3DVector3& angles) override;                      // 0x0013a17a
    int SetOrientation(const cS3DVector3& target, const cS3DVector3& up) override;   // 0x00138bde
    void ModelPointToWorldPoint(cS3DVector3& point) override;
    void ModelPointToWorldPoint(float* point) override;
    void ModelVectorToWorldVector(cS3DVector3& vector) override;
    void ModelVectorToWorldVector(float* vector) override;
    int SetZoomAndRotation(int zoom, int rotation) override { return state_.SetZoomAndRotation(zoom, rotation); }   // 0x0013895c
    int SetFrameset(int index) override { return state_.SetFrameset(index); }    // 0x0013987c
    int SetFrame(int frame) override { return state_.SetFrame(frame); }          // 0x00139936
    int GetFrame() const override { return state_.GetFrame(); }                  // 0x0013984e
    int GetFrameCount() const override { return state_.GetFrameCount(); }        // 0x00138712
    int IncrementFrame() override { return state_.IncrementFrame(); }            // 0x0013873c
    int SetAutoFrameRate(float rate) override { return state_.SetAutoFrameRate(rate); }   // 0x00138860
    int DoAutoFrameUpdate(float seconds) override { return state_.DoAutoFrameUpdate(seconds); }   // 0x00137dfa
    void SetPlayMode(int mode) override { state_.SetPlayMode(mode); }            // 0x00137f14
    const void* GetAnimInfo() const override { return nullptr; }                 // 0x001388a6: the resource's anim info (not kept)
    int SetOpacity(bool translucent, float opacity) override;                    // 0x00137f32
    int SetHighlight(int value) override;                                        // 0x00137f9a
    int SetSelfIlluminated(bool on) override;                                    // 0x00137fe0

    cAnimatedInstanceState& State() { return state_; }

private:
    // IAnimatedModelHost: the resource's ANIM section
    int ModelFrameCount() override;
    int ModelDefaultRate() override;
    bool LoadModelFrame(int frame) override;
    bool LoadModelInstance(std::uint32_t, int) override { return false; }   // the zoom table is never filled
    void PushAttributes();

    cAnimatedInstanceState state_;
};

}  // namespace s3d
