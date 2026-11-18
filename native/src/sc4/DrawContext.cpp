// SC4DrawContext — see DrawContext.h for the layout, the quirks and the seams. Compile without FMA
// contraction so the matrix and colour products round like the original single-precision code.
#pragma STDC FP_CONTRACT OFF

#include "sc4/DrawContext.h"

#include <cstring>

#include "gz/GZCOMInterface.h"
#include "s3d/S3DMath.h"

// cISC4Occupant is only needed for the debug bounding-box path of RenderModelInstance; its header pulls in
// the exemplar headers, whose pre-existing overloaded-virtual warnings are not interesting here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"
#include "sc4/Occupant.h"
#pragma clang diagnostic pop

// The layout mirrors the binary object: 0x288 bytes, the texture-stage record 0x68, the material 0x50 and the
// runtime mesh record 0x18. A new field must keep these exact.
static_assert(sizeof(SC4DrawContext) == 0x288, "SC4DrawContext layout");
static_assert(sizeof(SC4DrawContext::cStageInfo) == 0x68, "cStageInfo layout");
static_assert(sizeof(s3d::cS3DMaterial) == 0x50, "cS3DMaterial layout");
static_assert(sizeof(SC4Mesh) == 0x18, "cS3DMesh layout");

namespace {

// cSC4RenderProperties indices. The original walks the three property vectors record by record:
// bool records are 0x40 bytes (value at +0x38), int and float records 0x48 bytes (value at +0x38), so the
// byte offsets the decompiler shows are index * stride + 0x38.
constexpr std::uint32_t kPropForcePointSampling = 5;        // +0x178
constexpr std::uint32_t kPropForceBilinear = 6;             // +0x1b8
constexpr std::uint32_t kPropForceNoAlphaBlend = 7;         // +0x1f8
constexpr std::uint32_t kPropForceNoAlphaTest = 8;          // +0x238
constexpr std::uint32_t kPropUseSecondStage = 11;           // +0x2f8
constexpr std::uint32_t kPropForceTextureRepeat = 21;       // +0x578
constexpr std::uint32_t kPropShowBoundingBoxes = 26;        // +0x6b8
constexpr std::uint32_t kPropCullSmallModels = 30;          // +0x7b8 (kNoPartialBackingStoreCopies in the table)
constexpr std::uint32_t kPropDepthOffset = 16;              // +0x4b8, ints
constexpr std::uint32_t kPropRenderModelThreshold = 9;      // +0x2c0, floats

constexpr float kFloatMax = 3.4028235e38f;
constexpr float kTexCoordScale = 0.06666667f;               // 1/15
constexpr std::uint32_t kIndexTypeUShort = 3;               // eGDType the original hands DrawElements

IMeshBuilderHost* g_meshBuilderHost = nullptr;

// cS3DMeshBuilder's directional light for the debug boxes (0x3e800000, 0x3f000000, 0x3f800000).
constexpr cS3DVector3 kBoxLight{0.25f, 0.5f, 1.0f};

// cS3DMatrix4::operator*=(cS3DMatrix4 const&) @ 0x1007749c0: this = this * other, summing the four products
// from the last coefficient to the first in the original's order.
void MultiplyMatrix4(float a[16], const cS3DMatrix4& b) {
    float out[16];
    const float* m = &b.m[0][0];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[row * 4 + col] = a[row * 4 + 3] * m[12 + col] + a[row * 4 + 2] * m[8 + col] +
                                 a[row * 4 + 1] * m[4 + col] + a[row * 4 + 0] * m[col];
        }
    }
    std::memcpy(a, out, sizeof out);
}

// The matrix ResetModelViewTransform / SetModelTransform(cS3DTransform) build: the rotation transposed with the
// translation in the last column (m[12..14]) and 1.0 in m[15]. @ 0x1007726ee / 0x100774512.
void BuildModelViewMatrix(const cS3DTransform& view, float out[16]) {
    const float factor = (view.flags & nS3D::kTransformScale) != 0 ? view.scale : 1.0f;
    const float(*r)[3] = view.rotation;
    out[0] = r[0][0] * factor;    out[1] = r[1][0] * factor;    out[2] = r[2][0] * factor;    out[3] = 0.0f;
    out[4] = r[0][1] * factor;    out[5] = r[1][1] * factor;    out[6] = r[2][1] * factor;    out[7] = 0.0f;
    out[8] = r[0][2] * factor;    out[9] = r[1][2] * factor;    out[10] = r[2][2] * factor;   out[11] = 0.0f;
    out[12] = view.translation[0];
    out[13] = view.translation[1];
    out[14] = view.translation[2];
    out[15] = 1.0f;
}

}  // namespace

IMeshBuilderHost* MeshBuilderHost() { return g_meshBuilderHost; }
void SetMeshBuilderHost(IMeshBuilderHost* host) { g_meshBuilderHost = host; }

// ---------------------------------------------------------------------------------------------------------
// cStageInfo
// ---------------------------------------------------------------------------------------------------------
SC4DrawContext::cStageInfo::cStageInfo() {
    combiner = gd::cGDCombiner{0x01030001u, 0x01030001u, 0x00010000u};
    for (int i = 0; i < 16; ++i) texMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}

// ---------------------------------------------------------------------------------------------------------
// Construction / lifetime
// ---------------------------------------------------------------------------------------------------------
SC4DrawContext::SC4DrawContext() {
    // The eleven highlight colours start out white; Init replaces them with the table at 0x100a161d0..
    highlightColors_.assign(11, cS3DVector4{1.0f, 1.0f, 1.0f, 1.0f});
    // The fog colour and the word after it are pre-filled with FLT_MAX (memset_pattern16 of 0x10 bytes at +0xfc).
    fogColor_[0] = fogColor_[1] = fogColor_[2] = kFloatMax;
    reserved108_ = 0x7f7fffffu;
    fogStart_ = fogEnd_ = kFloatMax;
    // The original leaves the model-view matrix uninitialised; zero it so an early GetModelViewMatrix is defined.
    for (float& value : modelViewMatrix_) value = 0.0f;
    defaultState_ = cSC4PackedRenderState{0, 0};      // the constructor clears both packed states
    highlightState_ = cSC4PackedRenderState{0, 0};
}

SC4DrawContext::~SC4DrawContext() = default;

// ---------------------------------------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::Init() {
    if (initialized_) return;
    if (!driver_) return;   // port guard: the original dereferences the driver without a check

    // The original asks the driver for the current video mode (vslot 0x208) and reads three fields of the
    // 0x34-byte sGDMode: dword +0x24 (texture stages; 0 in this build) and bytes +0x29 / +0x2a.
    gd::sGDMode mode{};
    driver_->GetVideoModeInfo(mode);
    const auto* modeBytes = reinterpret_cast<const std::uint8_t*>(&mode);
    deviceCap29_ = modeBytes[0x29];
    deviceCap2a_ = modeBytes[0x2a];
    maxTextureStages_ = static_cast<std::int32_t>(mode.flags[1]);
    std::uint32_t stages = 2;
    if (2u < static_cast<std::uint32_t>(maxTextureStages_)) stages = static_cast<std::uint32_t>(maxTextureStages_);
    maxStages_ = static_cast<std::int32_t>(stages);
    stages_.resize(stages);

    // 0x100a161d0 (white), 0x100a26510 (green), 0x100a16af0 (red), 0x100a2a340..0x100a2a3a0.
    static const cS3DVector4 kHighlights[11] = {
        {1.0f, 1.0f, 1.0f, 1.0f},   {0.0f, 1.0f, 0.0f, 1.0f},   {1.0f, 0.0f, 0.0f, 1.0f},
        {0.2f, 0.2f, 0.4f, 0.6f},   {0.66f, 0.0f, 0.0f, 0.7f},  {0.66f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.0f, 0.0f, 1.0f},   {0.0f, 0.37f, 0.0f, 1.0f},
        {0.5f, 0.0f, 0.0f, 1.0f},   // quirk: index 8 repeats index 6 (the constant register is not reloaded)
        {0.0f, 0.7f, 0.2f, 0.3f},   {0.7f, 0.0f, 0.2f, 0.3f}};
    for (std::size_t i = 0; i < highlightColors_.size() && i < 11; ++i) highlightColors_[i] = kHighlights[i];

    defaultState_.PackRenderState(true, true, true, false, false, 0, false, 3, 4, 5, 1, 0.5f, 1, 1);
    defaultCombiner_ = kSC4DefaultTexCombiner;
    highlightState_.PackRenderState(true, false, true, false, true, 1, false, 3, 4, 5, 1, 0.5f, 1, -1);
    highlightCombiner_ = kSC4HighlightTexCombiner;
    initialized_ = true;
}

void SC4DrawContext::Shutdown() {
    if (initialized_) {
        initialized_ = false;
        renderProperties_ = nullptr;    // +0x50 is cleared, nothing else
    }
}

void SC4DrawContext::SetHighlightColor(int index, const cS3DVector4& color) {
    highlightColors_[static_cast<std::size_t>(index)] = color;   // no bounds check in the original
}

void SC4DrawContext::SetZoomAndRotation(std::int32_t zoom, std::int32_t rotation) {
    zoomScale_ = static_cast<float>(zoom);
    zoom_ = zoom;
    rotation_ = rotation;
}

bool SC4DrawContext::DeviceSecondStage() const {
    return renderProperties_ ? renderProperties_->BoolValue(kPropUseSecondStage) : true;
}

// SC4ViewUtilities::viewportUnitsPerWorldUnit_ (+0x15c), written by UpdateCameraPosition from
// cSC4CameraControl +0x140.
float SC4DrawContext::ViewportUnitsPerWorld() const {
    return viewUtilities_ ? viewUtilities_->ViewportUnitsPerWorld() : 0.0f;
}

// ---------------------------------------------------------------------------------------------------------
// Camera and the model-view matrix
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::SetCamera(s3d::cS3DCamera* camera) {
    camera_ = camera;
    if (!camera || !driver_) return;
    // The projection matrix goes to the driver transposed (the original copies proj[col][row] to out[row][col]).
    const float* projection = camera_->ProjectionMatrix();
    float matrix[16];
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) matrix[row * 4 + col] = projection[col * 4 + row];
    driver_->MatrixMode(1);
    driver_->LoadMatrix(matrix);
    driver_->MatrixMode(0);
    modelViewValid_ = false;
    ResetModelViewTransform();
}

void SC4DrawContext::UpdateViewTransformFromCamera() {
    if (!camera_) return;
    const cS3DTransform& view = camera_->ViewXform();
    viewTransform_.changeCounter = static_cast<std::uint8_t>(viewTransform_.changeCounter + 1);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) viewTransform_.rotation[row][col] = view.rotation[row][col];
    for (int i = 0; i < 3; ++i) viewTransform_.translation[i] = view.translation[i];
    viewTransform_.scale = view.scale;
    viewTransform_.flags = view.flags;
}

void SC4DrawContext::ResetModelViewTransform() {
    if (modelViewValid_ || !driver_) return;
    UpdateViewTransformFromCamera();
    BuildModelViewMatrix(viewTransform_, modelViewMatrix_);
    driver_->LoadMatrix(modelViewMatrix_);
    modelViewValid_ = true;
}

void SC4DrawContext::GetModelViewMatrix(float* matrix) const {
    std::memcpy(matrix, modelViewMatrix_, sizeof modelViewMatrix_);
}

void SC4DrawContext::SetModelViewTransformChanged() { modelViewValid_ = false; }

// ---------------------------------------------------------------------------------------------------------
// Render-state flags
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::EnableColorMaskFlag(bool enable) {
    if (enable) {
        if ((stateWord_ & 1u) == 0) {
            driver_->ColorMask(true);
            stateWord_ |= 1u;
        }
    } else if ((stateWord_ & 1u) != 0) {
        driver_->ColorMask(false);
        stateWord_ &= ~1u;
    }
}

void SC4DrawContext::EnableAlphaTestFlag(bool enable) {
    const bool forcedOff = renderProperties_ && renderProperties_->BoolValue(kPropForceNoAlphaTest);
    if (enable && !forcedOff) {
        if ((stateWord_ & 8u) == 0) {
            driver_->Enable(gd::state::AlphaTest);
            stateWord_ |= 8u;
        }
    } else if ((stateWord_ & 8u) != 0) {
        driver_->Disable(gd::state::AlphaTest);
        stateWord_ &= ~8u;
    }
}

void SC4DrawContext::EnableDepthTestFlag(bool enable) {
    if (enable) {
        if ((stateWord_ & 4u) == 0) {
            driver_->Enable(gd::state::DepthTest);
            stateWord_ |= 4u;
        }
    } else if ((stateWord_ & 4u) != 0) {
        driver_->Disable(gd::state::DepthTest);
        stateWord_ &= ~4u;
    }
}

void SC4DrawContext::EnableDepthMaskFlag(bool enable) {
    if (enable) {
        if ((stateWord_ & 2u) == 0) {
            driver_->DepthMask(true);
            stateWord_ |= 2u;
        }
    } else if ((stateWord_ & 2u) != 0) {
        driver_->DepthMask(false);
        stateWord_ &= ~2u;
    }
}

void SC4DrawContext::EnableBlendStateFlag(bool enable) {
    const bool forcedOff = renderProperties_ && renderProperties_->BoolValue(kPropForceNoAlphaBlend);
    if (enable && !forcedOff) {
        if ((stateWord_ & 0x10u) == 0) {
            driver_->Enable(gd::state::Blend);
            stateWord_ |= 0x10u;
        }
    } else if ((stateWord_ & 0x10u) != 0) {
        driver_->Disable(gd::state::Blend);
        stateWord_ &= ~0x10u;
    }
}

void SC4DrawContext::EnableCullFaceFlag(bool enable) {
    if (enable) {
        if ((stateWord_ & 0x40u) == 0) {
            driver_->Enable(gd::state::CullFace);
            stateWord_ |= 0x40u;
        }
    } else if ((stateWord_ & 0x40u) != 0) {
        driver_->Disable(gd::state::CullFace);
        stateWord_ &= ~0x40u;
    }
}

// The original mirrors the stage-0 bits of the state word into stage 1 only when the render properties allow a
// second stage. A stage number above 1 goes straight to TexStage / Enable.
void SC4DrawContext::EnableTextureStateFlag(bool enable, int stage) {
    if (enable) {
        if (stage > 1) {
            driver_->TexStage(static_cast<std::uint32_t>(stage));
            driver_->Enable(gd::state::Texturing);
            return;
        }
        if ((stateWord_ & 0x20u) == 0) {
            stateWord_ |= 0x20u;
            driver_->TexStage(0);
            driver_->Enable(gd::state::Texturing);
        }
        if (stage != 1) return;
        if ((stateWord_ & 0x10000000u) != 0) return;
        stateWord_ |= 0x10000000u;
        if (!DeviceSecondStage()) return;
        driver_->TexStage(1);
        driver_->Enable(gd::state::Texturing);
        return;
    }
    std::uint32_t word = stateWord_;
    if (stage > 0) {
        if (stage != 1) return;
    } else if ((word & 0x20u) != 0) {
        driver_->TexStage(0);
        driver_->Disable(gd::state::Texturing);
        word = stateWord_ & ~0x20u;
        stateWord_ = word;
    }
    if ((word & 0x10000000u) == 0) return;
    if (DeviceSecondStage()) {
        driver_->TexStage(1);
        driver_->Disable(gd::state::Texturing);
        word = stateWord_;
    }
    stateWord_ = word & ~0x10000000u;
}

// ---------------------------------------------------------------------------------------------------------
// Texture state
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::SetTexture(int texture, int stage) {
    if (stage == 0 || DeviceSecondStage()) driver_->SetTexture(static_cast<std::uint32_t>(texture), static_cast<std::uint32_t>(stage));
    stages_[static_cast<std::size_t>(stage)].texture = texture;   // no bounds check in the original
}

void SC4DrawContext::SetDepthFunc(int function) {
    if (function == -1) return;
    if (((stateWord_ >> 7) & 7u) == static_cast<std::uint32_t>(function)) return;
    stateWord_ = (static_cast<std::uint32_t>(function) & 7u) << 7 | (stateWord_ & 0xfffffc7fu);
    driver_->DepthFunc(static_cast<std::uint32_t>(function));
}

void SC4DrawContext::SetBlendFunc(int source, int destination) {
    const std::uint32_t word = stateWord_;
    std::uint32_t currentSource = (word >> 13) & 0xfu;
    std::uint32_t currentDestination = (word >> 17) & 0xfu;
    if (((currentSource == static_cast<std::uint32_t>(source)) || source == -1) &&
        ((destination == -1) || (currentDestination == static_cast<std::uint32_t>(destination))))
        return;
    if (source != -1) currentSource = static_cast<std::uint32_t>(source);
    if (destination != -1) currentDestination = static_cast<std::uint32_t>(destination);
    stateWord_ = (currentDestination & 0xfu) << 17 | (currentSource & 0xfu) << 13 | (word & 0xffe01fffu);
    // the driver is told the effective pair, not the caller's (-1) placeholders
    driver_->BlendFunc(currentSource, currentDestination);
}

void SC4DrawContext::SetAlphaFunc(int function, float reference) {
    const std::uint32_t word = stateWord_;
    const std::uint32_t currentFunction = (word >> 10) & 7u;
    const float currentReference = static_cast<float>((word >> 21) & 0xfu) * kTexCoordScale;
    // A changed function applies; otherwise the reference has to differ (and not be the -1 "don't care").
    bool apply = currentFunction != static_cast<std::uint32_t>(function) && function != -1;
    if (!apply) apply = reference != -1.0f && currentReference != reference;
    if (!apply) return;
    const std::uint32_t newFunction = function != -1 ? static_cast<std::uint32_t>(function) : currentFunction;
    const float newReference = reference != -1.0f ? reference : currentReference;
    const float biased = newReference * 15.0f + 12582912.0f;
    std::uint32_t quantized;
    std::memcpy(&quantized, &biased, sizeof quantized);
    stateWord_ = (quantized & 0xfu) << 21 | (newFunction & 7u) << 10 | (word & 0xfe1fe3ffu);
    driver_->AlphaFunc(newFunction, newReference);   // the effective function, not the -1 placeholder
}

void SC4DrawContext::SetTexEnvMode(int mode, int stage) {
    if (mode == -1) return;
    cStageInfo& info = stages_[static_cast<std::size_t>(stage)];
    // The combiner's high half is forced to 1 whenever a TexEnv mode is set by hand.
    info.combiner.word2 = (info.combiner.word2 & 0xffffu) | 0x00010000u;
    std::uint32_t word = stateWord_;
    if (stage == 1) {
        if ((word >> 29) == static_cast<std::uint32_t>(mode)) return;
        stateWord_ = static_cast<std::uint32_t>(mode) << 29 | (word & 0x1fffffffu);
        if (!DeviceSecondStage()) return;
        driver_->TexStage(1);
    } else if (stage == 0) {
        if (((word >> 25) & 7u) == static_cast<std::uint32_t>(mode)) return;
        stateWord_ = (static_cast<std::uint32_t>(mode) & 7u) << 25 | (word & 0xf1ffffffu);
        driver_->TexStage(0);
    } else {
        driver_->TexStage(static_cast<std::uint32_t>(stage));
    }
    driver_->TexEnv(0, 0, static_cast<std::uint32_t>(mode));
}

void SC4DrawContext::SetTexCoord(int source, int stage) {
    cStageInfo& info = stages_[static_cast<std::size_t>(stage)];
    if ((stage == 0 || DeviceSecondStage()) && info.texCoord != source) {
        driver_->TexStage(static_cast<std::uint32_t>(stage));
        driver_->TexStageCoord(static_cast<std::uint32_t>(source));
    }
    info.texCoord = source;
}

void SC4DrawContext::SetTexWrapModes(int wrapU, int wrapV, int stage) {
    cStageInfo& info = stages_[static_cast<std::size_t>(stage)];
    if (info.wrapU == wrapU && info.wrapV == wrapV) return;
    if (stage == 0 || DeviceSecondStage()) {
        driver_->TexStage(static_cast<std::uint32_t>(stage));
        if (!(renderProperties_ && renderProperties_->BoolValue(kPropForceTextureRepeat))) {
            driver_->TexParameter(0, 2, static_cast<std::uint32_t>(wrapU));
            driver_->TexParameter(0, 3, static_cast<std::uint32_t>(wrapV));
        } else {
            driver_->TexParameter(0, 2, 3);
            driver_->TexParameter(0, 3, 3);
        }
    }
    info.wrapU = wrapU;
    info.wrapV = wrapV;
}

void SC4DrawContext::SetTexFiltering(int minFilter, int magFilter, int stage) {
    cStageInfo& info = stages_[static_cast<std::size_t>(stage)];
    if (info.minFilter == minFilter && info.magFilter == magFilter) return;
    if (stage == 0 || DeviceSecondStage()) {
        driver_->TexStage(static_cast<std::uint32_t>(stage));
        if (renderProperties_ && renderProperties_->BoolValue(kPropForcePointSampling)) {
            driver_->TexParameter(0, 0, 0);
            driver_->TexParameter(0, 1, 0);
        } else if (renderProperties_ && renderProperties_->BoolValue(kPropForceBilinear)) {
            driver_->TexParameter(0, 0, 1);
            driver_->TexParameter(0, 1, 7);
        } else {
            driver_->TexParameter(0, 0, static_cast<std::uint32_t>(minFilter));
            driver_->TexParameter(0, 1, static_cast<std::uint32_t>(magFilter));
        }
    }
    info.minFilter = minFilter;
    info.magFilter = magFilter;
}

void SC4DrawContext::SetTexCombiner(const gd::cGDCombiner& combiner, int stage) {
    driver_->SetCombiner(combiner, static_cast<std::uint32_t>(stage));
    if (stage == 0) {
        stateWord_ = (stateWord_ & 0xf1ffffffu) | 0x8000000u;
    } else if (stage == 1) {
        stateWord_ = (stateWord_ & 0x1fffffffu) | 0x80000000u;
    }
    cStageInfo& info = stages_[static_cast<std::size_t>(stage)];
    info.combiner = combiner;
}

void SC4DrawContext::SetTexColor(const cS3DVector4& color) { driver_->TexEnv(0, 1, &color.x); }

void SC4DrawContext::SetTexTransform4(const cS3DMatrix4& matrix, int stage) {
    if (stage == 0 || DeviceSecondStage()) {
        SetTexCoord(0x10, stage);
        driver_->TexStage(static_cast<std::uint32_t>(stage));
        driver_->TexStageMatrix(&matrix.m[0][0], 4, 2, 1);
    }
    cStageInfo& info = stages_[static_cast<std::size_t>(stage)];
    info.transform = true;
    // Only stage 1 caches the matrix; stage 0 keeps its texture scale in the first matrix slot.
    if (stage == 1) std::memcpy(info.texMatrix, &matrix.m[0][0], sizeof info.texMatrix);
}

void SC4DrawContext::ClearTexTransform(int stage) {
    cStageInfo& info = stages_[static_cast<std::size_t>(stage)];
    if (!info.transform) return;
    SetTexCoord(0, stage);
    driver_->TexStage(static_cast<std::uint32_t>(stage));
    driver_->TexStageMatrix(nullptr, 3, 2, 1);
    info.transform = false;
}

// ---------------------------------------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::SetVertexBuffer(int format, std::uint32_t /*count*/, const void* vertices) {
    driver_->InterleavedArrays(static_cast<std::uint32_t>(format), 0, vertices);   // the count is ignored
}

void SC4DrawContext::SetIndexBuffer(std::uint32_t /*count*/, const std::uint16_t* indices) { indexBuffer_ = indices; }

void SC4DrawContext::DrawPrims(int primitive, int format, int count, const void* vertices) {
    driver_->InterleavedArrays(static_cast<std::uint32_t>(format), 0, vertices);
    driver_->DrawArrays(static_cast<std::uint32_t>(primitive), 0, count);
}

void SC4DrawContext::DrawPrims(int primitive, int first, int count) {
    driver_->DrawArrays(static_cast<std::uint32_t>(primitive), first, count);
}

void SC4DrawContext::DrawPrimsIndexed(int primitive, int first, int count) {
    driver_->DrawElements(static_cast<std::uint32_t>(primitive), count, kIndexTypeUShort, indexBuffer_ + first);
}

void SC4DrawContext::DrawPrimsIndexed(int primitive, int count, const std::uint16_t* indices) {
    driver_->DrawElements(static_cast<std::uint32_t>(primitive), count, kIndexTypeUShort, indices);
}

// ---------------------------------------------------------------------------------------------------------
// Lighting, shading, transparency, fog
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::SetLighting(bool enable) {
    if (lighting_ == enable) return;
    if (enable) {
        cS3DVector3 ambient{ambientLight_[0], ambientLight_[1], ambientLight_[2]};
        if (lightingManager_) lightingManager_->GetGlobalLight(ambient);
        ambientLight_[0] = ambient.x;
        ambientLight_[1] = ambient.y;
        ambientLight_[2] = ambient.z;
        driver_->ColorMultiplier(ambient.x, ambient.y, ambient.z);
    } else {
        driver_->ColorMultiplier(1.0f, 1.0f, 1.0f);
    }
    lighting_ = enable;
}

bool SC4DrawContext::GetLighting() const { return lighting_; }

void SC4DrawContext::SetShade(const cS3DVector3& shade) {
    driver_->ColorMultiplier(shade.x * ambientLight_[0], shade.y * ambientLight_[1], shade.z * ambientLight_[2]);
    shadeOverride_ = true;
}

void SC4DrawContext::SetSelfLitShade(const cS3DVector3& shade) {
    driver_->ColorMultiplier(shade.x, shade.y, shade.z);
    shadeOverride_ = true;
}

void SC4DrawContext::SetModelShade(s3d::cIS3DModelInstance* instance, const cS3DVector3* shade) {
    cS3DVector3 light{0.0f, 0.0f, 0.0f};
    if (!lightingManager_ || !lightingManager_->GetModelLight(instance, light))
        light = cS3DVector3{ambientLight_[0], ambientLight_[1], ambientLight_[2]};
    if (shade) {
        light.x *= shade->x;
        light.y *= shade->y;
        light.z *= shade->z;
    }
    driver_->ColorMultiplier(light.x, light.y, light.z);
    shadeOverride_ = true;
}

void SC4DrawContext::ResetShade() {
    if (!shadeOverride_) return;
    driver_->ColorMultiplier(ambientLight_[0], ambientLight_[1], ambientLight_[2]);
    shadeOverride_ = false;
}

void SC4DrawContext::SetTransparency(float alpha) { driver_->AlphaMultiplier(alpha); }

void SC4DrawContext::ResetTransparency() { driver_->AlphaMultiplier(1.0f); }

void SC4DrawContext::SetFog(bool enable, const float* color, float start, float end) {
    if (fog_ != enable) {
        if (enable) {
            driver_->Enable(gd::state::Fog);
            driver_->Fog(0, 2);   // the shipped driver treats fog mode 0 as a stub (it only records an error code)
        } else {
            driver_->Disable(gd::state::Fog);
        }
        fog_ = enable;
    }
    if (color && !(fogColor_[0] == color[0] && fogColor_[1] == color[1] && fogColor_[2] == color[2])) {
        driver_->Fog(1, color);
        fogColor_[0] = color[0];
        fogColor_[1] = color[1];
        fogColor_[2] = color[2];
    }
    if (start != kFloatMax && fogStart_ != start) {
        const float values[4] = {start, end, end, start};
        driver_->Fog(3, values);
        fogStart_ = values[0];
    }
    if (end != kFloatMax && fogEnd_ != end) {
        const float values[4] = {end, end, start, 0.0f};
        driver_->Fog(4, values);
        fogEnd_ = values[0];
    }
}

// ---------------------------------------------------------------------------------------------------------
// Packed render states
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::SetDefaultRenderState() {
    if (!stateStack_.empty()) {
        SetRenderState(stateStack_.back());
        return;
    }
    SetRenderState(defaultState_);
}

void SC4DrawContext::SetRenderState(const cSC4PackedRenderState& state) {
    const std::uint32_t changed = (state.value ^ stateWord_) & state.mask;
    if (changed != 0) {
        stateWord_ = (stateWord_ & ~state.mask) | state.value;
        driver_->TexStage(0);
        auto toggle = [this](bool on, gd::eGDDriverState which) {
            if (on) driver_->Enable(which);
            else driver_->Disable(which);
        };
        if ((changed & renderstate::kColorWrite) != 0) driver_->ColorMask((stateWord_ & renderstate::kColorWrite) != 0);
        if ((changed & renderstate::kDepthWrite) != 0) driver_->DepthMask((stateWord_ & renderstate::kDepthWrite) != 0);
        if ((changed & renderstate::kDepthTest) != 0) toggle((stateWord_ & renderstate::kDepthTest) != 0, gd::state::DepthTest);
        if ((changed & renderstate::kAlphaTest) != 0) {
            const bool forcedOff = renderProperties_ && renderProperties_->BoolValue(kPropForceNoAlphaTest);
            toggle((stateWord_ & renderstate::kAlphaTest) != 0 && !forcedOff, gd::state::AlphaTest);
        }
        if ((changed & renderstate::kBlend) != 0) {
            const bool forcedOff = renderProperties_ && renderProperties_->BoolValue(kPropForceNoAlphaBlend);
            toggle((stateWord_ & renderstate::kBlend) != 0 && !forcedOff, gd::state::Blend);
        }
        if ((changed & renderstate::kTexture0) != 0) toggle((stateWord_ & renderstate::kTexture0) != 0, gd::state::Texturing);
        if ((changed & renderstate::kCull) != 0) toggle((stateWord_ & renderstate::kCull) != 0, gd::state::CullFace);
        if ((changed & 0x380u) != 0) driver_->DepthFunc((stateWord_ >> 7) & 7u);
        if ((changed & 0x1e01c00u) != 0)
            driver_->AlphaFunc((stateWord_ >> 10) & 7u, static_cast<float>((stateWord_ >> 21) & 0xfu) * kTexCoordScale);
        if ((changed & 0x1fe000u) != 0) driver_->BlendFunc((stateWord_ >> 13) & 0xfu, (stateWord_ >> 17) & 0xfu);
        if ((changed & 0xe000000u) != 0) driver_->TexEnv(0, 0, (stateWord_ >> 25) & 7u);
        if (changed >= 0x10000000u && DeviceSecondStage()) {
            driver_->TexStage(1);
            if ((changed & renderstate::kTexture1) != 0) toggle((stateWord_ & renderstate::kTexture1) != 0, gd::state::Texturing);
            if (changed > 0x1fffffffu) driver_->TexEnv(0, 0, stateWord_ >> 29);
            driver_->TexStage(0);
        }
    }
    // The tail runs on every call: stage 0 goes back to repeat wrapping, bilinear filtering and no transform.
    SetTexWrapModes(3, 3, 0);
    SetTexFiltering(1, 7, 0);
    ClearTexTransform(1);
    ClearTexTransform(0);
}

void SC4DrawContext::SetDefaultRenderStateUnilaterally() {
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        driver_->TexStage(static_cast<std::uint32_t>(i));
        driver_->TexStageMatrix(nullptr, 3, 2, 1);
        driver_->TexStageCoord(0);
    }
    for (cStageInfo& stage : stages_) stage = cStageInfo();
    stateWord_ = ~defaultState_.value;
    SetRenderState(defaultState_);
}

void SC4DrawContext::SetRenderState(const s3d::cS3DMaterial& material, const s3d::SC4DrawAttributes* attributes) {
    if (!attributes) return;
    const std::uint32_t attributeFlags = attributes->flags;
    const float shadeAlpha = attributes->shade[3];
    const std::uint16_t flags = material.flags;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&material);

    // +8 is the material's translucency (0..255). The division is done in double like the original.
    // the original converts to double, multiplies by 1/255 and converts back
    const float alpha = static_cast<float>(static_cast<double>(bytes[8]) * (1.0 / 255.0));
    bool translucent = false;
    float alphaReference = alpha;
    if ((attributeFlags & 8u) == 0) {
        translucent = (attributeFlags & 2u) != 0;
        if (translucent && shadeAlpha < 1.0f) {
            alphaReference = alpha * shadeAlpha;
            driver_->AlphaMultiplier(shadeAlpha);
        }
    } else {
        driver_->ColorMultiplier(shadeAlpha, shadeAlpha, shadeAlpha);
    }

    bool depthWrite = false;
    bool depthTest = (attributeFlags & 0x10u) == 0;
    if ((attributeFlags & 0x30u) == 0) {
        depthWrite = (flags & 0x80u) != 0;
        depthTest = (flags & 2u) != 0;
    }
    std::uint32_t blendSource = bytes[9];
    if (translucent) blendSource = 4;
    std::uint32_t blendDestination = 5;
    if (!translucent) blendDestination = bytes[10];
    const std::uint32_t rawLayerCount = material.layerCount;
    const std::uint32_t layerCount = rawLayerCount > 2 ? 2 : rawLayerCount;   // the record holds two layers
    std::uint32_t depthFunction = bytes[6];
    if ((attributeFlags & 0x20u) != 0) depthFunction = 4;
    const std::uint32_t alphaFunction = bytes[7];
    std::int32_t texEnv0 = -1;
    std::int32_t texEnv1 = -1;
    if (layerCount != 0) {
        texEnv0 = bytes[0x24];
        if (layerCount != 1) texEnv1 = bytes[0x44];
    }

    cSC4PackedRenderState state;
    state.PackRenderState((flags & 0x100u) != 0, depthWrite, depthTest, (flags & 1u) != 0,
                          (flags & 0x10u) != 0 || translucent, static_cast<int>(layerCount), (flags & 8u) != 0,
                          static_cast<int>(depthFunction), static_cast<int>(blendSource),
                          static_cast<int>(blendDestination), static_cast<int>(alphaFunction), alphaReference,
                          texEnv0, texEnv1);
    SetRenderState(state);

    if ((flags & 0x20u) == 0 || layerCount == 0) return;
    // Each layer: bind the texture, then its coordinate source, wrapping, filtering, environment mode and
    // (optionally) the inverse-scale texture transform.
    for (std::uint32_t index = 0; index < layerCount; ++index) {
        const s3d::S3DMaterialLayer& layer = material.layers[index];
        // +8 points at the texture binding, whose first word points at the texture record that starts with the
        // texture id (**(+0x18), as cS3DModelInstance::GetSortValue reads it; the original checks neither pointer)
        const auto* binding = *reinterpret_cast<const void* const* const*>(&layer.texture);
        const auto* textureRecord = binding ? *binding : nullptr;
        const std::uint32_t texture = textureRecord ? *static_cast<const std::uint32_t*>(textureRecord) : 0;
        if (index == 0 || DeviceSecondStage()) driver_->SetTexture(texture, index);
        stages_[index].texture = static_cast<std::int32_t>(texture);
        SetTexCoord(layer.pad15, static_cast<int>(index));                    // +0x15 texture-coordinate source
        SetTexWrapModes(layer.sampler[0], layer.sampler[1], static_cast<int>(index));
        SetTexFiltering(layer.sampler[2], layer.sampler[3], static_cast<int>(index));
        if (layer.used == 4) SetTexCombiner(defaultCombiner_, static_cast<int>(index));
        SetTexEnvMode(layer.used, static_cast<int>(index));
        if ((layer.pad16 & 1u) == 0) {
            ClearTexTransform(static_cast<int>(index));
        } else {
            // Inverse of the layer's texture scale, with the view transform's translation folded in.
            float matrix[16] = {};
            const float inverseScale = 1.0f / layer.scale;
            matrix[0] = inverseScale;
            matrix[5] = -inverseScale;
            matrix[12] = -(viewTransform_.translation[0] * inverseScale);
            matrix[13] = viewTransform_.translation[1] * inverseScale;
            cS3DMatrix4 transform;
            std::memcpy(&transform.m[0][0], matrix, sizeof matrix);
            SetTexTransform4(transform, static_cast<int>(index));
        }
    }
}

void SC4DrawContext::SetRenderStateHighlight(const s3d::cS3DMaterial& material,
                                             const s3d::SC4DrawAttributes* attributes) {
    if (!attributes) return;
    // The highlight type is the byte after the attribute flags (+1).
    SetRenderStateHighlight((attributes->flags >> 8) & 0xffu);
    if (material.layerCount == 0) return;
    const auto* binding = *reinterpret_cast<const void* const* const*>(&material.layers[0].texture);
    const auto* textureRecord = binding ? *binding : nullptr;
    const std::uint32_t texture = textureRecord ? *static_cast<const std::uint32_t*>(textureRecord) : 0;
    driver_->SetTexture(texture, 0);
    stages_[0].texture = static_cast<std::int32_t>(texture);
    SetTexCoord(material.layers[0].pad15, 0);
    SetTexWrapModes(material.layers[0].sampler[0], material.layers[0].sampler[1], 0);
    SetTexFiltering(material.layers[0].sampler[2], material.layers[0].sampler[3], 0);
}

void SC4DrawContext::SetRenderStateHighlight(std::uint32_t type) {
    SetRenderState(highlightState_);
    driver_->TexEnv(0, 1, &highlightColors_[type].x);   // no bounds check in the original
    driver_->SetCombiner(highlightCombiner_, 0);
    stateWord_ = (stateWord_ & 0xf1ffffffu) | 0x8000000u;
    stages_[0].combiner = highlightCombiner_;
}

void SC4DrawContext::SetDepthOffset(int offset) {
    if (depthOffset_ == offset) return;
    depthOffset_ = offset;
    const std::int32_t scale = renderProperties_ ? renderProperties_->IntValue(kPropDepthOffset) : 2;
    if (!camera_ || !driver_) return;
    camera_->SetDepthOffset(static_cast<float>(offset * scale));
    driver_->MatrixMode(1);
    const float* projection = camera_->ProjectionMatrix();
    float matrix[16];
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) matrix[row * 4 + col] = projection[col * 4 + row];
    driver_->LoadMatrix(matrix);
    driver_->MatrixMode(0);
}

// Stage 1 is copied over stage 0 so that the second texture can be drawn with fixed-function stage 0: the depth
// function is forced to the cached "less or equal" value while the driver is told 2 (the original does exactly
// that), alpha test is switched off and the blend mode follows the stage-1 TexEnv.
void SC4DrawContext::SetEmulatedSecondStageRenderState() {
    if ((stateWord_ & 0x10000000u) == 0) return;
    const cStageInfo& stage1 = stages_[1];
    SetTexture(stage1.texture, 0);
    SetTexCoord(stage1.texCoord, 0);
    SetTexWrapModes(stage1.wrapU, stage1.wrapV, 0);
    SetTexFiltering(stage1.minFilter, stage1.magFilter, 0);
    if (!stage1.transform) {
        ClearTexTransform(0);
    } else {
        cS3DMatrix4 matrix;
        std::memcpy(&matrix.m[0][0], stage1.texMatrix, sizeof stage1.texMatrix);
        SetTexTransform4(matrix, 0);
    }
    if ((stateWord_ & 0x380u) != 0x100u) {
        stateWord_ = (stateWord_ & 0xfffffc7fu) | 0x100u;
        driver_->DepthFunc(2);
    }
    if ((stateWord_ & 8u) != 0) {
        driver_->Disable(gd::state::AlphaTest);
        stateWord_ &= ~8u;
    }
    const std::uint32_t texEnv1 = (stateWord_ >> 29) & 7u;
    bool blend;
    if (texEnv1 == 2) {
        SetTexEnvMode(0, 0);
        SetBlendFunc(4, 5);
        blend = true;
    } else if (texEnv1 == 1) {
        SetBlendFunc(0, 2);
        blend = true;
    } else if (texEnv1 == 0) {
        SetTexEnvMode(0, 0);
        blend = false;
    } else {
        blend = (stateWord_ & 0x10u) != 0;
    }
    EnableBlendStateFlag(blend);
}

// ---------------------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------------------
void SC4DrawContext::RenderMesh(const SC4Mesh& mesh) {
    const SC4MeshVertexGroup* vertices = mesh.vertices;
    if (!vertices || vertices->count == 0) return;
    driver_->InterleavedArrays(vertices->format, 0, vertices->data);
    if (!mesh.draws || !mesh.draws->begin) return;
    const std::ptrdiff_t count = mesh.draws->end - mesh.draws->begin;
    const SC4MeshDrawRecord* records = mesh.draws->begin;
    if (mesh.indexGroup == nullptr) {
        for (std::ptrdiff_t i = 0; i < count; ++i)
            driver_->DrawArrays(records[i].primitive, static_cast<int>(records[i].start), static_cast<int>(records[i].count));
    } else {
        const std::uint16_t* indices = *mesh.indexGroup;
        indexBuffer_ = indices;
        for (std::ptrdiff_t i = 0; i < count; ++i)
            driver_->DrawElements(records[i].primitive, static_cast<int>(records[i].count), kIndexTypeUShort,
                                  indices + records[i].start);
    }
}

void SC4DrawContext::SetModelTransform(const cS3DTransform& transform) {
    if (!modelViewValid_) UpdateViewTransformFromCamera();
    modelTransform_.changeCounter = static_cast<std::uint8_t>(modelTransform_.changeCounter + 1);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) modelTransform_.rotation[row][col] = transform.rotation[row][col];
    for (int i = 0; i < 3; ++i) modelTransform_.translation[i] = transform.translation[i];
    modelTransform_.scale = transform.scale;
    modelTransform_.flags = transform.flags;
    nS3D::MultiplyTransform(viewTransform_, transform);
    BuildModelViewMatrix(viewTransform_, modelViewMatrix_);
    driver_->LoadMatrix(modelViewMatrix_);
    modelViewValid_ = false;
}

void SC4DrawContext::SetModelTransform(const cS3DMatrix4& transform) {
    if (!modelViewValid_) UpdateViewTransformFromCamera();
    const cS3DTransform& view = viewTransform_;
    // The view as a row-major cS3DMatrix4 (cS3DTransform::GetMatrix4: the rotation as stored, the translation in
    // m[3], m[7], m[11]) times the caller's matrix, copied transposed into the GL (column-major) matrix as
    // i386 0x0051299a does.
    float matrix[16] = {view.rotation[0][0], view.rotation[0][1], view.rotation[0][2], view.translation[0],
                        view.rotation[1][0], view.rotation[1][1], view.rotation[1][2], view.translation[1],
                        view.rotation[2][0], view.rotation[2][1], view.rotation[2][2], view.translation[2],
                        0.0f,                0.0f,                0.0f,                1.0f};
    MultiplyMatrix4(matrix, transform);
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) modelViewMatrix_[col * 4 + row] = matrix[row * 4 + col];
    driver_->LoadMatrix(modelViewMatrix_);
    modelViewValid_ = false;
}

void SC4DrawContext::RenderModelInstance(s3d::cIS3DModelInstance* instance, s3d::cS3DMeshAndMaterial* meshes,
                                         const s3d::SC4DrawAttributes* attributes, bool highlight) {
    if (!instance || !meshes || !attributes) return;
    const std::uint32_t attributeFlags = attributes->flags;
    if (highlight) {
        if ((attributeFlags & 8u) != 0) return;
    } else if (renderProperties_ && renderProperties_->BoolValue(kPropShowBoundingBoxes)) {
        // Debug view: the model's world bounds in red, then the owner occupant's bounding box in blue.
        cS3DBoundingBox box;
        box.min[0] = box.min[1] = box.min[2] = 1.0f;
        box.max[0] = box.max[1] = box.max[2] = 0.0f;
        instance->GetWorldBounds(box);
        DrawBoundingBox(box, cS3DVector4{1.0f, 0.0f, 0.0f, 1.0f});
        if (!instance->GetOwner()) return;
        cRZAutoRefCount<sc4::cISC4Occupant> occupant =
            gz::GZCOM_Interface<sc4::cISC4Occupant>(instance->GetOwner(), sc4::kGZIID_cISC4Occupant);
        if (!occupant) return;
        occupant->GetBoundingBox(box.min, box.max);
        DrawBoundingBox(box, cS3DVector4{0.0f, 0.0f, 1.0f, 0.5f});
        return;
    }
    if (renderProperties_ && renderProperties_->BoolValue(kPropCullSmallModels)) {
        cS3DBoundingBox box;
        box.min[0] = box.min[1] = box.min[2] = 1.0f;
        box.max[0] = box.max[1] = box.max[2] = 0.0f;
        instance->GetWorldBounds(box);
        const float extentX = box.max[0] - box.min[0];
        const float extentY = box.max[1] - box.min[1];
        const float extentZ = box.max[2] - box.min[2];
        // The original scans four floats starting at the z extent; slots 3 and 4 are another stack dword and the
        // uninitialised slot after it, so only the three real extents are used here.
        float largest = extentX;
        if (extentY > largest) largest = extentY;
        if (extentZ > largest) largest = extentZ;
        const float threshold = renderProperties_ ? renderProperties_->FloatValue(kPropRenderModelThreshold) : 6.0f;
        if (largest * ViewportUnitsPerWorld() < threshold) return;
    }

    const int materialCount = meshes->MeshCount();
    cS3DTransform transform;   // identity: the instance fills it in
    instance->GetTransform(transform);
    SetModelTransform(transform);

    if (!highlight && (attributeFlags & 4u) == 0) {
        const cS3DVector3 shade{attributes->shade[0], attributes->shade[1], attributes->shade[2]};
        SetModelShade(instance, &shade);
    } else {
        driver_->ColorMultiplier(attributes->shade[0], attributes->shade[1], attributes->shade[2]);
        shadeOverride_ = true;
    }

    for (int index = 0; index < materialCount; ++index) {
        s3d::cS3DMaterial material;
        std::memcpy(&material, meshes->Material(index).data(), sizeof material);
        SC4Mesh mesh;
        std::memcpy(&mesh, meshes->Mesh(index).data(), sizeof mesh);
        if (highlight) {
            SetRenderStateHighlight(material, attributes);
        } else {
            SetRenderState(material, attributes);
        }
        RenderMesh(mesh);
        if (!DeviceSecondStage() && material.layerCount == 2) {
            SetEmulatedSecondStageRenderState();
            RenderMesh(mesh);
        }
    }
    if (shadeOverride_) {
        driver_->ColorMultiplier(ambientLight_[0], ambientLight_[1], ambientLight_[2]);
        shadeOverride_ = false;
    }
    driver_->AlphaMultiplier(1.0f);
}

void SC4DrawContext::DrawBoundingBox(const cS3DBoundingBox& box, const cS3DVector4& color) {
    // Function-local statics in the original: the box render state is packed once, the mesh is rebuilt per call.
    static cSC4PackedRenderState sBoxRenderState;
    static bool sBoxRenderStateSet = false;
    if (!sBoxRenderStateSet) {
        sBoxRenderStateSet = true;
        sBoxRenderState.PackRenderState(true, true, true, false, true, 0, false, 3, 4, 5, -1, -1.0f, 1, -1);
    }
    if (!(box.min[0] <= box.max[0])) return;
    IMeshBuilderHost* host = MeshBuilderHost();
    if (!host) return;
    IS3DMeshBuilder* builder = host->BoxMeshBuilder();
    if (!builder) return;
    builder->Clear();
    builder->SetDirLight(kBoxLight);
    builder->SetColor(color);
    cS3DVector3 corners[8];
    nS3D::MakeBoxCorners(box, corners);
    builder->AddBoxMesh(corners);
    SetRenderState(sBoxRenderState);
    const SC4Mesh* mesh = builder->Mesh();
    if (mesh) RenderMesh(*mesh);
}
