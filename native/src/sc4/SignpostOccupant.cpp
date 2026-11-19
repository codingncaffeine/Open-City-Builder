#include "sc4/SignpostOccupant.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>

#include "sc4/DrawContext.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/ViewUtilities.h"

namespace sc4 {

namespace {

ISignpostHost* g_host = nullptr;

// The sign's ten frame / post textures @ 0x100a293f4 (resource type 0x7ab50e44, group 0x1abe787d).
constexpr std::uint32_t kSignpostTextureInstances[10] = {0x8b4a6560, 0x8b4a6561, 0x8b4a6562, 0x8b4a6563,
                                                         0x8b4a6564, 0x8b4a6565, 0x8b4a6564, 0x8b4a6565,
                                                         0x8b4a6566, 0x8b4a6567};

// The board's shade Draw sets before the glyph quads @ 0x100a293e0.
const cS3DVector3 kSignpostTextColor{0.31f, 0.34f, 0.42f};

// The edit cursor's vertex colour (the write at 0x10070f866).
constexpr std::uint32_t kSignpostCursorColor = 0xff6a554e;

// Draw's primitive and vertex formats (6 = triangle strip, 1 = sVF_V3F_C4B, 2 = sVF_V3F_T2F).
constexpr int kSignpostPrimitive = 6;
constexpr int kSignpostColorFormat = 1;
constexpr int kSignpostTextureFormat = 2;

void SetQuad(SignVertexT2F (&quad)[4], float x0, float x1, float yTop, float yBottom) {
    quad[0] = SignVertexT2F{x0, yTop, 0.0f, 0.0f, 0.0f};
    quad[1] = SignVertexT2F{x0, yBottom, 0.0f, 0.0f, 1.0f};
    quad[2] = SignVertexT2F{x1, yBottom, 0.0f, 1.0f, 1.0f};
    quad[3] = SignVertexT2F{x1, yTop, 0.0f, 1.0f, 0.0f};
}

// The two side frame pieces of RebuildModel have their u coordinate mirrored (the patches at 0x10070f674..0x10070f6a5).
void SetMirroredQuad(SignVertexT2F (&quad)[4], float x0, float x1, float yTop, float yBottom) {
    quad[0] = SignVertexT2F{x0, yTop, 0.0f, 1.0f, 0.0f};
    quad[1] = SignVertexT2F{x0, yBottom, 0.0f, 0.0f, 0.0f};
    quad[2] = SignVertexT2F{x1, yBottom, 0.0f, 0.0f, 1.0f};
    quad[3] = SignVertexT2F{x1, yTop, 0.0f, 1.0f, 1.0f};
}

}  // namespace

const float kSignpostScaleByZoom[5] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};   // @ 0x100a29430
const std::uint32_t kSignpostBoardColors[2] = {0xffbeac90u, 0xccffffffu};   // @ 0x100a2941c

ISignpostHost* SignpostHost() { return g_host; }
void SetSignpostHost(ISignpostHost* host) { g_host = host; }

// 0x10070e6ae
cSC4SignpostOccupant::cSC4SignpostOccupant() {
    // The original zeroes the bounds' "empty" form (min = 1, max = 0) here and Init replaces it with (0,0,0)-(1,1,1).
    for (int i = 0; i < 3; ++i) {
        localBounds_[i] = worldBounds_[i] = 1.0f;
        localBounds_[i + 3] = worldBounds_[i + 3] = 0.0f;
    }
    for (int box = 0; box < 2; ++box) {
        pickBoxes_[box][0] = pickBoxes_[box][1] = pickBoxes_[box][2] = 1.0f;
        pickBoxes_[box][3] = pickBoxes_[box][4] = pickBoxes_[box][5] = 0.0f;
    }
    // editMode_ / cursorVisible_ / tickMs_ start at zero; the original only stores editMode_ (the other two are
    // left as malloc garbage until SetEditMode) — the port zeroes them, see the header's quirks.
}

// 0x1007110ca (+ the wrappers at 0x100710d00 / 0x100710d12)
cSC4SignpostOccupant::~cSC4SignpostOccupant() {
    // the original releases +0x600 down to +0x5b8 (highest index first)
    for (int i = 9; i >= 0; --i) {
        if (ISignpostTextureBinding* binding = textures_[i]) {
            textures_[i] = nullptr;
            binding->Release();
        }
    }
}

// 0x1007100a8
bool cSC4SignpostOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4SignpostOccupant) {
        *out = static_cast<cISC4SignpostOccupant*>(this);
        cSC4PropOccupant::AddRef();
        return true;
    }
    return cSC4PropOccupant::QueryInterface(iid, out);
}

// 0x10070ea3e
bool cSC4SignpostOccupant::Init() {
    if (IsInitialized()) return true;
    if ((cSC4Occupant::state_ & 2) == 0) {   // not restored from a save (cSC4PropOccupant hides state_ with its own)
        localBounds_[0] = localBounds_[1] = localBounds_[2] = 0.0f;
        localBounds_[3] = localBounds_[4] = localBounds_[5] = 1.0f;
        worldBounds_[0] = worldBounds_[1] = worldBounds_[2] = 0.0f;
        worldBounds_[3] = worldBounds_[4] = worldBounds_[5] = 1.0f;
        const float lo[3] = {0.0f, 0.0f, 0.0f};
        const float hi[3] = {1.0f, 1.0f, 1.0f};   // the original writes only the prop's max (+0x84..+0x8c)
        SetBoundingBox(lo, hi);
        SetPropType(kSignpostPropType);
        SetFlag(kSignpostFlagInitialized);
    }
    cSC4PropOccupant::Init();
    renderState_.PackRenderState(true, true, true, false, true, 1, false, 3, 4, 5, -1, -1.0f, 1, -1);

    ISignpostHost* host = g_host;
    if (host) {
        for (int i = 0; i < 10; ++i) {
            if (ISignpostTextureBinding* previous = textures_[i]) {
                textures_[i] = nullptr;
                previous->Release();
            }
            textures_[i] = host->CreateTextureBinding(kSignpostTextureInstances[i]);
        }
        if (cIGZMessageServer2* server = host->Messages()) {
            server->AddNotification(this, kSignpostMessageRefresh);
            server->AddNotification(this, kSignpostMessageVisibility);
        }
    }
    LoadTextures();
    RebuildModel();
    return true;
}

// 0x10070ebf4
void cSC4SignpostOccupant::LoadTextures() {
    glyphTextures_.clear();
    if (text_.Strlen() == 0) {
        glyphTextures_.clear();   // the original clears the list twice on the empty path
        return;
    }
    if (g_host) g_host->StringToGlyphTextureArray(kSignpostFontId, text_, glyphTextures_);
}

// 0x10070ed0c
//
// PARTIAL: the second pick box and the tail of the vectorised bounds block are a geometric reconstruction (see the
// header). Everything below is in the original's local space (the billboard is flat: z = 0) and is rotated into the
// world by RebuildModel's own yaw composition, which is why the bounds come out of WorldBoxesFromTransform.
void cSC4SignpostOccupant::RebuildModel() {
    ISignpostHost* host = g_host;

    // ---- the glyph metrics (viewport units) -------------------------------------------------------------
    const int glyphCount = static_cast<int>(glyphTextures_.size());
    int textWidth = 0, textHeight = 0;
    for (int i = 0; i < glyphCount; ++i) {
        int size[2] = {0, 0};
        glyphTextures_[i]->GetGlyphSize(size);
        textWidth += size[0];
        if (size[1] > textHeight) textHeight = size[1];
    }
    if (editMode_) {
        textWidth += 4;
        if (textHeight == 0) textHeight = 24;
    } else if (textWidth == 0) {
        textHeight = 24;
        textWidth = 1;
    }

    int zoom = host ? host->ViewZoom() : 0;
    zoom = std::clamp(zoom, 0, 4);
    const float scale = kSignpostScaleByZoom[zoom];
    const auto units = [host](float value) { return host ? host->ViewportUnitsToWorldUnits(value) : value; };

    const float boardWidth = units(static_cast<float>(textWidth)) * scale;
    const float boardHeight = units(static_cast<float>(textHeight)) * scale;
    const float margin = units(8.0f) * scale;
    const float postHeight = units(64.0f) * scale;
    const float totalHeight = 3.0f * margin + postHeight + boardHeight;   // fVar32 in the original
    const float boardTop = totalHeight - margin;
    const float boardBottom = boardTop - boardHeight;
    const float postTop = boardBottom - margin;
    const float leftInner = -boardWidth * 0.5f, rightInner = boardWidth * 0.5f;
    const float leftOuter = leftInner - margin, rightOuter = rightInner + margin;
    const float halfPost = margin * 0.5f;

    // ---- the vertex buffers -----------------------------------------------------------------------------
    SetQuad(textQuad_, leftInner, rightInner, boardTop, boardBottom);   // +0x1a8 (never drawn, see the header)

    SetQuad(pieceQuads_[0], leftOuter, leftInner, totalHeight, boardTop);
    SetQuad(pieceQuads_[1], rightInner, rightOuter, totalHeight, boardTop);
    SetQuad(pieceQuads_[2], leftOuter, leftInner, boardBottom, postTop);
    SetQuad(pieceQuads_[3], rightInner, rightOuter, boardBottom, postTop);
    SetQuad(pieceQuads_[4], leftInner, rightInner, totalHeight, boardTop);
    SetQuad(pieceQuads_[5], leftInner, rightInner, boardBottom, postTop);
    SetMirroredQuad(pieceQuads_[6], leftOuter, leftInner, boardTop, boardBottom);
    SetMirroredQuad(pieceQuads_[7], rightInner, rightOuter, boardTop, boardBottom);
    SetQuad(pieceQuads_[8], -halfPost, halfPost, postTop, margin);
    SetQuad(pieceQuads_[9], -halfPost, halfPost, margin, 0.0f);

    const std::uint32_t boardColor = kSignpostBoardColors[GetProperties() & 1];
    backgroundQuad_[0] = SignVertexC4B{leftInner, boardTop, 0.0f, boardColor};
    backgroundQuad_[1] = SignVertexC4B{leftInner, boardBottom, 0.0f, boardColor};
    backgroundQuad_[2] = SignVertexC4B{rightInner, boardBottom, 0.0f, boardColor};
    backgroundQuad_[3] = SignVertexC4B{rightInner, boardTop, 0.0f, boardColor};

    // ---- the glyph quads (four vertices per character) --------------------------------------------------
    glyphQuads_.resize(static_cast<std::size_t>(glyphCount) * 4);
    float textEnd = leftInner;
    if (glyphCount > 0) {
        for (int i = 0; i < glyphCount; ++i) {
            float rect[4] = {0, 0, 0, 0};   // left, top, right, bottom
            glyphTextures_[i]->GetGlyphTextureCoordinates(rect);
            int size[2] = {0, 0};
            glyphTextures_[i]->GetGlyphSize(size);
            const float x1 = units(static_cast<float>(size[0])) * scale + textEnd;
            SignVertexT2F* quad = &glyphQuads_[static_cast<std::size_t>(i) * 4];
            quad[0] = SignVertexT2F{textEnd, boardTop, 0.0f, rect[0], rect[1]};
            quad[1] = SignVertexT2F{textEnd, boardBottom, 0.0f, rect[0], rect[3]};
            quad[2] = SignVertexT2F{x1, boardBottom, 0.0f, rect[2], rect[3]};
            quad[3] = SignVertexT2F{x1, boardTop, 0.0f, rect[2], rect[1]};
            textEnd = x1;
        }
    }

    // ---- the edit cursor (only while editing) -----------------------------------------------------------
    if (editMode_) {
        const float cursorX0 = textEnd + units(2.0f);
        const float cursorX1 = textEnd + units(2.0f) + units(2.0f) * scale;
        cursorQuad_[0] = SignVertexC4B{cursorX0, boardTop, 0.0f, kSignpostCursorColor};
        cursorQuad_[1] = SignVertexC4B{cursorX0, boardBottom, 0.0f, kSignpostCursorColor};
        cursorQuad_[2] = SignVertexC4B{cursorX1, boardBottom, 0.0f, kSignpostCursorColor};
        cursorQuad_[3] = SignVertexC4B{cursorX1, boardTop, 0.0f, kSignpostCursorColor};
    }

    // ---- the transform: force a unit scale, then compose the camera-facing yaw ---------------------------
    if (transform_.scale != 1.0f) {
        transform_.scale = 1.0f;
        transform_.flags &= static_cast<std::uint8_t>(~2u);
        ++transform_.changeCounter;
    }
    float previous[3][3];
    if ((transform_.flags & 4) != 0) {
        // the flag means "RebuildModel composed this rotation": drop it and start from the identity
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) previous[r][c] = (r == c) ? 1.0f : 0.0f;
        transform_.flags &= static_cast<std::uint8_t>(~4u);
        ++transform_.changeCounter;
    } else {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) previous[r][c] = transform_.rotation[r][c];
    }
    float viewX = 0.0f, viewZ = 0.0f;
    if (host) host->CameraViewDirection(viewX, viewZ);
    const float yaw = std::atan2(-viewX, -viewZ);
    const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
    for (int r = 0; r < 3; ++r) {   // transform.rotation = previous * R_y(yaw)
        transform_.rotation[r][0] = previous[r][0] * cosYaw - previous[r][2] * sinYaw;
        transform_.rotation[r][1] = previous[r][1];
        transform_.rotation[r][2] = previous[r][0] * sinYaw + previous[r][2] * cosYaw;
    }
    transform_.flags |= 4;
    ++transform_.changeCounter;

    // ---- the bounds and pick boxes ----------------------------------------------------------------------
    localBounds_[0] = leftOuter;
    localBounds_[1] = 0.0f;
    localBounds_[2] = 0.0f;
    localBounds_[3] = rightOuter;
    localBounds_[4] = totalHeight;
    localBounds_[5] = 0.0f;

    WorldBoxesFromTransform(localBounds_, localBounds_ + 3, worldBounds_, worldBounds_ + 3);
    SetBoundingBox(worldBounds_, worldBounds_ + 3);   // the original writes the prop's min/max from the same values
    if (std::getenv("SC4_SIGNDEBUG"))
        std::fprintf(stderr, "signpost: local %.1f,%.1f,%.1f..%.1f,%.1f,%.1f world %.1f,%.1f,%.1f..%.1f,%.1f,%.1f at %.1f,%.1f,%.1f\n", localBounds_[0],
                     localBounds_[1], localBounds_[2], localBounds_[3], localBounds_[4], localBounds_[5], worldBounds_[0], worldBounds_[1],
                     worldBounds_[2], worldBounds_[3], worldBounds_[4], worldBounds_[5], transform_.translation[0], transform_.translation[1],
                     transform_.translation[2]);

    WorldBoxesFromTransform(localBounds_, localBounds_ + 3, pickBoxes_[0], pickBoxes_[0] + 3);
    const float postMin[3] = {-halfPost, 0.0f, 0.0f};
    const float postMax[3] = {halfPost, postTop, 0.0f};
    WorldBoxesFromTransform(postMin, postMax, pickBoxes_[1], pickBoxes_[1] + 3);

    if (IsFlagSet(1)) {
        if (host) {
            if (cISC4OccupantManager* manager = host->OccupantManager())
                manager->MoveOccupant(static_cast<cISC4Occupant*>(this), false);
        }
    }
    if (IsFlagSet(2) && host) {
        host->RendererMoveModel(this);
        host->RendererChangeModel(this, 3);
    }
}

// The axis-aligned world box of a flat local rectangle under transform_ (0x10070fbad..0x10070fed2, reconstructed:
// the original packs the corner coordinates across vector lanes).
void cSC4SignpostOccupant::WorldBoxesFromTransform(const float* localMin, const float* localMax, float* outMin,
                                                   float* outMax) const {
    float mn[3] = {3.4028235e38f, 3.4028235e38f, 3.4028235e38f};
    float mx[3] = {-3.4028235e38f, -3.4028235e38f, -3.4028235e38f};
    for (int corner = 0; corner < 4; ++corner) {
        const float x = (corner & 1) ? localMax[0] : localMin[0];
        const float y = (corner & 2) ? localMax[1] : localMin[1];
        const float z = localMin[2];   // the billboard is flat (min z == max z == 0)
        const float wx = transform_.rotation[0][0] * x + transform_.rotation[0][1] * y + transform_.rotation[0][2] * z +
                         transform_.translation[0];
        const float wy = transform_.rotation[1][0] * x + transform_.rotation[1][1] * y + transform_.rotation[1][2] * z +
                         transform_.translation[1];
        const float wz = transform_.rotation[2][0] * x + transform_.rotation[2][1] * y + transform_.rotation[2][2] * z +
                         transform_.translation[2];
        mn[0] = std::min(mn[0], wx);
        mn[1] = std::min(mn[1], wy);
        mn[2] = std::min(mn[2], wz);
        mx[0] = std::max(mx[0], wx);
        mx[1] = std::max(mx[1], wy);
        mx[2] = std::max(mx[2], wz);
    }
    for (int i = 0; i < 3; ++i) {
        outMin[i] = mn[i];
        outMax[i] = mx[i];
    }
}

// 0x10070ff94
bool cSC4SignpostOccupant::Shutdown() {
    if (!IsInitialized()) return true;
    ISignpostHost* host = g_host;
    if (host) {
        if (cIGZMessageServer2* server = host->Messages()) {
            server->RemoveNotification(this, kSignpostMessageRefresh);
            server->RemoveNotification(this, kSignpostMessageVisibility);
        }
    }
    for (int i = 0; i < 10; ++i) {
        if (ISignpostTextureBinding* binding = textures_[i]) {
            textures_[i] = nullptr;
            binding->Release();
        }
    }
    glyphTextures_.clear();
    if (editMode_ && host) host->RemoveTickNotification(this, kSignpostTickChannel);
    return cSC4PropOccupant::Shutdown();
}

// 0x100710136
bool cSC4SignpostOccupant::DoMessage(cIGZMessage2* message) {
    ISignpostHost* host = g_host;
    cISC4Occupant* self = static_cast<cISC4Occupant*>(this);
    const std::uint32_t type = message->GetType();
    if (type == kSignpostMessageVisibility) {
        // data2 is the address of the view-set pointer (the same shape TextDecalOccupant's DoMessage reads)
        auto** slot = static_cast<ISignpostViewSet**>(static_cast<cIGZMessage2Standard*>(message)->GetVoid2());
        ISignpostViewSet* view = slot ? *slot : nullptr;
        if (view && view->ContainsOccupant(self) && host && host->PropDeveloperAllowsSigns()) {
            if (!IsFlagSet(kSignpostFlagShown)) SetFlag(kSignpostFlagShown);
        } else if (IsFlagSet(kSignpostFlagShown)) {
            ClearFlag(kSignpostFlagShown);
        }
    } else if (type == kSignpostMessageRefresh) {
        RebuildModel();
    } else if (type == kSignpostMessageTick) {
        const std::uint64_t elapsedMs = static_cast<cIGZMessage2Standard*>(message)->GetData2();
        const std::uint32_t accumulated =
            static_cast<std::uint32_t>(elapsedMs / 1000u) + static_cast<std::uint32_t>(tickMs_);
        tickMs_ = static_cast<int>(accumulated);
        if (accumulated > 499u) {
            cursorVisible_ = !cursorVisible_;   // this[0x109] ^= 1
            tickMs_ = 0;
        }
    }
    return DoPropMessage(message);
}

// 0x10071027e
void cSC4SignpostOccupant::SetAllFlags(std::uint32_t flags) {
    std::uint32_t changed = flags_ ^ flags;
    if ((flags & changed & kSignpostFlagShown) != 0) {
        // turning flag 2 on needs the prop developer's permission; otherwise keep the old flag set
        if (!(g_host && g_host->PropDeveloperAllowsSigns())) {
            flags &= ~kSignpostFlagShown;
            changed = flags_;
        }
    }
    if ((changed & kSignpostFlagShown) != 0) {
        ISignpostHost* host = g_host;
        const bool inserted = host && host->RendererHasModel(this);
        if ((flags & kSignpostFlagShown) == 0) {
            if (inserted) host->RendererRemoveModel(this);
        } else if (!inserted) {
            host->RendererInsertModel(this);
        }
    }
    cSC4Occupant::SetAllFlags(flags);
}

// 0x10071034c
void cSC4SignpostOccupant::SetText(const cIGZString& text) {
    if (text_.CompareToString(text, true) == 0 && text_.Strlen() != 0) return;
    text_.AssignFromIGZString(text);
    LoadTextures();
    RebuildModel();
}

// The four board vertices' colours (SetDynamic @ 0x1007103c0 writes them at +0x53c, +0x54c, +0x55c, +0x56c).
void cSC4SignpostOccupant::SetBoardColors(std::uint32_t color) {
    for (SignVertexC4B& vertex : backgroundQuad_) vertex.color = color;
}

// 0x1007103c0
void cSC4SignpostOccupant::SetDynamic(bool dynamic) {
    const int properties = GetProperties();
    if (((properties & 1) ^ (dynamic ? 1 : 0)) != 1) return;
    ISignpostHost* host = g_host;
    const bool shown = IsFlagSet(kSignpostFlagShown);
    SetProperties((properties & ~1) | (dynamic ? 1 : 0));
    SetBoardColors(kSignpostBoardColors[dynamic ? 1 : 0]);
    if (shown && host) host->RendererChangeModel(this, 5);
    if (editMode_ && !dynamic) SetEditMode(false);
}

// 0x100710510
int cSC4SignpostOccupant::Draw(SC4DrawContext* context, bool) {
    if (!IsInitialized() || !context) return 1;
    context->SetModelTransform(transform_);
    context->ClearTexTransform(0);
    context->SetRenderState(renderState_);
    context->ResetTransparency();
    context->SetTexWrapModes(2, 2, 0);
    context->EnableTextureStateFlag(false, 0);
    context->DrawPrims(kSignpostPrimitive, kSignpostColorFormat, 4, backgroundQuad_);
    context->EnableTextureStateFlag(true, 0);
    context->SetShade(kSignpostTextColor);
    const int glyphCount = static_cast<int>(glyphTextures_.size());
    context->SetDepthOffset(1);
    for (int i = 0; i < glyphCount; ++i) {
        ISignpostTextureBinding* binding = glyphTextures_[i]->GetTextureBinding();
        context->SetTexture(binding ? static_cast<int>(binding->TextureId()) : 0, 0);
        context->DrawPrims(kSignpostPrimitive, kSignpostTextureFormat, 4, &glyphQuads_[static_cast<std::size_t>(i) * 4]);
    }
    context->ResetShade();
    if (editMode_ && cursorVisible_) {
        context->EnableTextureStateFlag(false, 0);
        context->DrawPrims(kSignpostPrimitive, kSignpostColorFormat, 4, cursorQuad_);
        context->EnableTextureStateFlag(true, 0);
    }
    for (int i = 0; i < 10; ++i) {
        ISignpostTextureBinding* binding = textures_[i];
        context->SetTexture(binding ? static_cast<int>(binding->TextureId()) : 0, 0);
        context->DrawPrims(kSignpostPrimitive, kSignpostTextureFormat, 4, pieceQuads_[i]);
    }
    context->SetDepthOffset(0);
    return 1;
}

// 0x100710712
bool cSC4SignpostOccupant::Pick(cSC4PickContext* context) {
    if (!context) return false;
    for (int box = 0; box < 2; ++box) {
        float hit[3] = {0.0f, 0.0f, 0.0f};
        if (!SC4GeometryUtilities::RayIntersectsBoundingBox(&context->origin.x, &context->direction.x,
                                                            &pickBoxes_[box][0], &pickBoxes_[box][3], hit))
            continue;
        const float distance = SC4GeometryUtilities::LengthOfLine(&context->origin.x, hit);
        if (distance <= context->maxDistance) {
            context->maxDistance = distance;
            return true;   // the first box within the running maximum wins (the original does not look further)
        }
    }
    return false;
}

// 0x10071081c
void cSC4SignpostOccupant::SetPosition(const float* xyz) {
    const float dx = xyz[0] - transform_.translation[0];
    const float dy = xyz[1] - transform_.translation[1];
    const float dz = xyz[2] - transform_.translation[2];
    const float delta[3] = {dx, dy, dz};
    for (int i = 0; i < 3; ++i) {
        worldBounds_[i] += delta[i];
        worldBounds_[i + 3] += delta[i];
        pickBoxes_[0][i] += delta[i];
        pickBoxes_[0][i + 3] += delta[i];
        pickBoxes_[1][i] += delta[i];
        pickBoxes_[1][i + 3] += delta[i];
    }
    transform_.translation[0] = xyz[0];
    transform_.translation[1] = xyz[1];
    transform_.translation[2] = xyz[2];
    transform_.flags |= 1;
    ++transform_.changeCounter;
    cSC4PropOccupant::SetPosition(xyz);
    if (IsFlagSet(2) && g_host) g_host->RendererChangeModel(this, 4);
}

// 0x100710978
void cSC4SignpostOccupant::SetEditMode(bool editing) {
    if (editMode_ == editing) return;
    editMode_ = editing;
    ISignpostHost* host = g_host;
    if (editing) {
        SetDynamic(true);
        if (host) host->AddTickNotification(this, kSignpostTickChannel);
        tickMs_ = 0;
        cursorVisible_ = true;
    } else {
        if (host) host->RemoveTickNotification(this, kSignpostTickChannel);
        SetDynamic(false);
    }
    RebuildModel();
}

// 0x100710a0c
bool cSC4SignpostOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    if (cSC4PropOccupant::Write(stream)) {
        stream.SetGZStr(text_);
        stream.SetFloat32(transform_.translation[0]);
        stream.SetFloat32(transform_.translation[1]);
        stream.SetFloat32(transform_.translation[2]);
    }
    return !stream.GetError();
}

// 0x100710ae4
bool cSC4SignpostOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (cSC4PropOccupant::Read(stream)) {
        stream.GetGZStr(text_);
        if (version == 1) {
            std::uint32_t skipped = 0;   // version 1 carried an extra u32 before the position
            stream.GetUint32(skipped);
        }
        float position[3] = {0.0f, 0.0f, 0.0f};
        stream.GetFloat32(position[0]);
        stream.GetFloat32(position[1]);
        stream.GetFloat32(position[2]);
        transform_.translation[0] = position[0];
        transform_.translation[1] = position[1];
        transform_.translation[2] = position[2];
        transform_.flags |= 1;
        ++transform_.changeCounter;
    }
    return !stream.GetError();
}

// 0x100710c10
cISC4Occupant* cSC4SignpostOccupant::AsOccupant() { return static_cast<cISC4Occupant*>(this); }

// 0x100710c1a
void cSC4SignpostOccupant::GetText(cIGZString& text) { text.AssignFromIGZString(text_); }

// 0x100710c36
cIGZUnknown* cSC4SignpostOccupant::GetOwner() { return static_cast<cISC4Occupant*>(this); }

// 0x100710c40 (the original does nothing at all, not even release the previous owner)
void cSC4SignpostOccupant::SetOwner(cIGZUnknown*) {}

// 0x100710c46
void cSC4SignpostOccupant::GetBounds(cS3DBoundingBox& out) {
    out.min[0] = localBounds_[0];
    out.min[1] = localBounds_[1];
    out.min[2] = localBounds_[2];
    out.max[0] = localBounds_[3];
    out.max[1] = localBounds_[4];
    out.max[2] = localBounds_[5];
}

// 0x100710c82
void cSC4SignpostOccupant::GetWorldBounds(cS3DBoundingBox& out) {
    out.min[0] = worldBounds_[0];
    out.min[1] = worldBounds_[1];
    out.min[2] = worldBounds_[2];
    out.max[0] = worldBounds_[3];
    out.max[1] = worldBounds_[4];
    out.max[2] = worldBounds_[5];
}

// 0x100710cbe
void cSC4SignpostOccupant::GetTransform(cS3DTransform& out) { out = transform_; }

// 0x100710cd6
void cSC4SignpostOccupant::SetTransform(const cS3DTransform& transform) { transform_ = transform; }

// 0x100710ce8
void cSC4SignpostOccupant::GetSortValue(std::uint32_t& out) const { out = 0; }

}  // namespace sc4
