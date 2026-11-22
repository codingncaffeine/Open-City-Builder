#include "sc4/CellTextureModel.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>

#include "platform/Unwired.h"
#include "sc4/DrawContext.h"
#include "sc4/PackedRenderState.h"

namespace sc4 {
namespace {
ICellTextureModelHost* g_host = nullptr;

// SetupPackedRenderState (0x1000b583a): the three states, packed once.
struct PackedStates {
    cSC4PackedRenderState opaque, translucent, combine;
    PackedStates() {
        opaque.PackRenderState(true, true, true, false, false, 1, false, 3, -1, -1, -1, -1.0f, 1, -1);
        translucent.PackRenderState(true, true, true, true, true, 1, false, 3, 4, 5, 4, 0.0f, 1, -1);
        combine.PackRenderState(true, false, true, false, true, 1, false, 1, 4, 5, -1, -1.0f, -1, -1);
    }
};
const PackedStates& States() {
    static const PackedStates states;
    return states;
}

// The uv of each corner per rotation, the four tables at 0x100a16ad0 / ae0 / af0 / b00 (read from the binary):
// v0 = (a, b), v1 = (c, a), v2 = (b, d), v3 = (d, c).
constexpr float kRotA[4] = {0, 0, 1, 1}, kRotB[4] = {0, 1, 1, 0}, kRotC[4] = {1, 0, 0, 1}, kRotD[4] = {1, 1, 0, 0};
}  // namespace

ICellTextureModelHost* CellTextureModelHost() { return g_host; }
void SetCellTextureModelHost(ICellTextureModelHost* host) { g_host = host; }

bool cSC4CellTextureModel::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4CellTextureModel) {
        AddRef();
        *out = static_cast<ILotCellTextureModel*>(this);
        return true;
    }
    return s3d::cS3DModelInstanceBasic::QueryInterface(iid, out);
}

void cSC4CellTextureModel::Init(const LotBaseTextureSpecification& spec) {
    initialized_ = true;
    spec_ = spec;
    SetProperties(10);
    SetupVertices(true, false, nullptr, nullptr);
}

void cSC4CellTextureModel::GetWorldBounds(cS3DBoundingBox& out) {
    for (int i = 0; i < 3; ++i) {
        out.min[i] = boundsMin_[i];
        out.max[i] = boundsMax_[i];
    }
}

void cSC4CellTextureModel::GetSortValue(std::uint32_t& out) const {
    out = binding_ && binding_->frames ? binding_->frames[0] : spec_.textureId;
}

void cSC4CellTextureModel::UpdateTextures(int zoom, const LotBaseTextureSpecification& spec) {
    spec_ = spec;
    LoadTextures(zoom);
    UpdateTextureRotation();
}

void cSC4CellTextureModel::DoZoomChange(int zoom) {
    SetupVertices(loadedZoom_ == 0xff, true, nullptr, nullptr);
    loadedZoom_ = static_cast<std::uint8_t>(zoom);
    LoadTextures(zoom);
}

bool cSC4CellTextureModel::LoadTextures(int zoom) {
    const CellTextureBinding* binding = nullptr;
    bool found = true;
    if (spec_.textureId != 0) {
        if (g_host) {
            binding = g_host->TextureBinding(spec_.textureId + static_cast<std::uint32_t>(zoom));
            if (!binding) binding = g_host->TextureBinding(spec_.textureId);
        }
        found = binding != nullptr;
    }
    binding_ = binding;
    textureHasAlpha_ = binding_ && binding_->hasAlpha;
    return found;
}

void cSC4CellTextureModel::UpdateTextureRotation() {
    const auto rotation = static_cast<std::uint8_t>(spec_.rotation);
    if (rotation < 4) {
        const float a = kRotA[rotation], b = kRotB[rotation], c = kRotC[rotation], d = kRotD[rotation];
        vertices_[0].u = a; vertices_[0].v = b;
        vertices_[1].u = c; vertices_[1].v = a;
        vertices_[2].u = b; vertices_[2].v = d;
        vertices_[3].u = d; vertices_[3].v = c;
    }
    if (spec_.flip)
        for (Vertex& vertex : vertices_) vertex.u = 1.0f - vertex.u;
    for (int i = 0; i < 4; ++i) {
        cellUVs_[i * 2] = vertices_[i].u;
        cellUVs_[i * 2 + 1] = vertices_[i].v;
    }
}

void cSC4CellTextureModel::SetupVertices(bool updateVertices, bool, void*, void*) {
    if (!updateVertices) return;
    const int cellX = spec_.cellX, cellZ = spec_.cellZ;
    float altitude = 0;
    float corners[4] = {0, 0, 0, 0};
    onLockedLot_ = g_host && g_host->LockedLotAltitude(cellX, cellZ, altitude);
    if (onLockedLot_) {
        SC4_UNWIRED("cSC4CellTextureModel::SetupVertices: the lighting manager's coloring of the locked-lot quad is not ported (terrain shade used)");
        std::fill(corners, corners + 4, altitude);
    } else if (g_host) {
        g_host->CellCornerAltitudes(cellX, cellZ, corners);
    }
    const float x0 = static_cast<float>(cellX * 16), z0 = static_cast<float>(cellZ * 16);
    const float x1 = static_cast<float>(cellX * 16 + 16), z1 = static_cast<float>(cellZ * 16 + 16);
    const float xs[4] = {x0, x1, x0, x1}, zs[4] = {z0, z0, z1, z1};
    std::uint8_t shades[4] = {0xff, 0xff, 0xff, 0xff};
    if (g_host) g_host->CellCornerShades(cellX, cellZ, shades);
    for (int i = 0; i < 4; ++i) {
        vertices_[i].x = xs[i];
        vertices_[i].y = corners[i];
        vertices_[i].z = zs[i];
        // the terrain's light (the lighting manager colours the locked-lot quad in the original), the spec's alpha
        vertices_[i].rgba[0] = vertices_[i].rgba[1] = vertices_[i].rgba[2] = shades[i];
        vertices_[i].rgba[3] = spec_.alpha;
    }
    UpdateTextureRotation();
    boundsMin_[1] = FLT_MAX;
    boundsMax_[1] = -FLT_MAX;
    boundsMin_[0] = x0 + 0.1f;
    boundsMin_[2] = z0 + 0.1f;
    boundsMax_[0] = x1 - 0.1f;
    boundsMax_[2] = z1 - 0.1f;
    float low = FLT_MAX, high = -FLT_MAX;
    if (!onLockedLot_) {
        for (const Vertex& vertex : vertices_) {
            low = std::min(low, vertex.y);
            high = std::max(high, vertex.y);
        }
    } else {
        low = high = vertices_[0].y;
    }
    boundsMin_[1] = low - 2.0f;
    boundsMax_[1] = high + 2.0f;
    if (boundsMax_[1] <= boundsMin_[1]) boundsMax_[1] = boundsMin_[1] + 0.1f;
}

int cSC4CellTextureModel::Draw(SC4DrawContext* context, bool highlight) {
    if (highlight || !context) return 1;
    if (static_cast<std::uint32_t>(context->Zoom()) != loadedZoom_) DoZoomChange(context->Zoom());
    const auto rotation = static_cast<std::uint32_t>(context->ViewRotation());
    if (rotation != viewRotation_) {
        SetupVertices(false, true, nullptr, nullptr);
        viewRotation_ = static_cast<std::uint8_t>(rotation);
    }
    if (!binding_) return 1;
    int mode = 2;
    if (spec_.combineMode == 0xff) {
        mode = 1;
        if (!textureHasAlpha_ && spec_.alpha == 0xff) mode = 0;
    }
    if (mode != 0 && context->RenderPass() == 0) return 2;   // translucent cells go after the opaque ones
    context->ResetModelViewTransform();
    context->ResetShade();
    const std::uint32_t texture = binding_->frames
        ? (spec_.frame < binding_->frameCount ? binding_->frames[spec_.frame] : binding_->frames[0]) : 0;
    context->SetTexture(static_cast<int>(texture), 0);
    if (mode == 0) {
        context->SetRenderState(States().opaque);
        context->SetTexWrapModes(2, 2, 0);
    } else if (mode == 1) {
        context->SetRenderState(States().translucent);
        context->SetTexWrapModes(2, 2, 0);
    } else {
        // The lot texture combiner: the spec colour (times the light, white here) and alpha through the texture factor,
        // combined with the texture in the spec's combine mode (mLotTextureCombiner, byte 0 = the mode).
        SC4_UNWIRED("cSC4CellTextureModel::Draw: the combine color is not multiplied by the draw context's lighting (+0x68 vslot 0x50)");
        context->SetRenderState(States().combine);
        context->SetTexWrapModes(2, 2, 0);
        context->SetTexFiltering(1, 1, 0);
        const cS3DVector4 colour{spec_.red / 255.0f, spec_.green / 255.0f, spec_.blue / 255.0f, spec_.alpha / 255.0f};
        context->SetTexColor(colour);
        const gd::cGDCombiner combiner{0x21020000u | spec_.combineMode, 0x00030001u, 0};
        context->SetTexCombiner(combiner, 0);
    }
    context->DrawPrims(1 /* triangle strip */, 10 /* V3F_C4UB_T2F */, 4, vertices_);
    return 1;
}

}  // namespace sc4
