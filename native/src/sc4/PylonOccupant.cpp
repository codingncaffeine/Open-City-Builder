#include "sc4/PylonOccupant.h"
#include "platform/Unwired.h"

#include <cmath>
#include <cstddef>

#include "gz/Buffer.h"
#include "sc4/DrawContext.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/OccupantManager.h"
#include "sc4/SLBuffer.h"
#include "sc4/ViewUtilities.h"
#include "s3d/S3DMath.h"

namespace sc4 {

namespace {

IPylonOccupantHost* g_host = nullptr;

// nsTexUtil::GetNearestPowerOfTwo @ 0x1002ff5ba: the smallest power of two >= value (1 for value <= 1).
int NearestPowerOfTwo(int value) {
    int candidate = 1;
    while (candidate < value) candidate += candidate;
    return candidate;
}

// SC4ViewUtilities::ViewportUnitsToWorldUnits @ 0x10017374c (SL::spViewUtilities +0x158).
float UnitsToWorld(IPylonOccupantHost* host, float units) {
    return host ? host->ViewportUnitsToWorldUnits(units) : units;
}

// The resource instance the current texture is created from: the table of the dispatch type, indexed by the
// dispatch state. The original indexes the three contiguous .rodata tables without a bounds check (a state above 7
// reads the neighbouring table); the port wraps an out-of-range index instead of reading past the tables.
std::uint32_t PylonTextureInstance(std::int32_t dispatchType, std::int32_t dispatchState) {
    const std::uint32_t* base = nullptr;
    switch (dispatchType) {
        case 0:   // police
        case 6:   // police helicopter
            base = kPoliceDispatchPylonResId;
            break;
        case 1:   // fire
        case 5:   // fire plane
            base = kFireDispatchPylonResId;
            break;
        case 4:   // MySim
            base = kMySimDispatchPylonResId;
            break;
        default:  // the original still creates a binding, with instance 0
            return 0;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(dispatchState) % 8u;
    return base[index];
}

void ReplaceSourceOccupant(cISC4Occupant*& slot, cISC4Occupant* value) {
    if (slot == value) return;
    if (value) value->AddRef();
    cISC4Occupant* previous = slot;
    slot = value;
    if (previous) previous->Release();
}

void ReplaceUnknown(cIGZUnknown*& slot, cIGZUnknown* value) {
    if (slot == value) return;
    if (value) value->AddRef();
    cIGZUnknown* previous = slot;
    slot = value;
    if (previous) previous->Release();
}

}  // namespace

IPylonOccupantHost* PylonOccupantHost() { return g_host; }
void SetPylonOccupantHost(IPylonOccupantHost* host) { g_host = host; }

// 0x10022def8
cSC4PylonOccupant::cSC4PylonOccupant() {
    // the cS3DModelInstanceBasic the original builds at +0x70 keeps properties 3 (bit 0 "dynamic")
    SetProperties(3);
    type_ = kPylonOccupantType;
    dirty_ = true;          // +0xa4
    uiState_ = 1;           // +0xa8
    dispatchState_ = 0;     // +0xac
    // +0xb0 (mDispatchType) is left uninitialised by the original; the port starts it at 0 (police)
    // the transform starts at the identity with no flags and change counter 0
    transform_ = cS3DTransform{};
    for (int i = 0; i < 3; ++i) {
        boundsMin_[i] = worldMin_[i] = 1.0f;
        boundsMax_[i] = worldMax_[i] = 0.0f;
    }
}

// 0x10022e0ec (+ the D0 wrapper at 0x10022e22c and the deleting destructor at 0x10022e280)
cSC4PylonOccupant::~cSC4PylonOccupant() {
    // the binding (+0x188) and the two textures are NOT released here; Shutdown owns them (quirk)
    if (pylonOwner_) {
        cIGZUnknown* owner = pylonOwner_;
        pylonOwner_ = nullptr;
        owner->Release();
    }
    if (sourceOccupant_) {
        cISC4Occupant* occupant = sourceOccupant_;
        sourceOccupant_ = nullptr;
        occupant->Release();
    }
}

// 0x10022e3a8
bool cSC4PylonOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    if (iid == kGZIID_cISC4PylonOccupant) {
        *out = static_cast<cISC4PylonOccupant*>(this);
        AddRef();
        return true;
    }
    if (cSC4Occupant::QueryInterface(iid, out)) return true;
    return s3d::cS3DModelInstanceBasic::QueryInterface(iid, out);
}

// 0x10022e476
bool cSC4PylonOccupant::Init() {
    if (!IsInitialized()) {
        cSC4Occupant::Init();
        IPylonOccupantHost* host = g_host;
        if (host) {
            if (cIGZMessageServer2* server = host->Messages()) {
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kPylonMessageViewChanged);
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kPylonMessageViewSet);
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kPylonMessageRebuild);
            }
            host->AddTickNotification(static_cast<cIGZMessageTarget2*>(this), kPylonTickChannel);
            if (host->RendererPresent()) {
                host->RendererGetDrawContext();
                SetRenderStates();
            }
        }
    }
    return true;
}

// The two cSC4PackedRenderState objects Init packs (inside the renderer check): the pass-1 state and the transparent
// pass-0 state.
void cSC4PylonOccupant::SetRenderStates() {
    renderStatePass1_.PackRenderState(true, true, true, true, true, 1, true, 3, 4, 5, 4, 0.0f, 1, -1);
    renderStatePass0_.PackRenderState(true, false, false, true, true, 1, true, -1, 4, 5, 4, 0.0f, 1, -1);
}

// 0x10022e5d6
bool cSC4PylonOccupant::Shutdown() {
    if (IsInitialized()) {
        cSC4Occupant::Shutdown();
        if ((flags_ & 1) != 0) PlaceInOccupantManager(false);
        IPylonOccupantHost* host = g_host;
        if (host) {
            if (cIGZMessageServer2* server = host->Messages()) {
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kPylonMessageViewChanged);
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kPylonMessageViewSet);
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kPylonMessageRebuild);
            }
            host->RemoveTickNotification(static_cast<cIGZMessageTarget2*>(this), kPylonTickChannel);
            if (callNumberTexture_ != 0) {
                host->ReleaseTexture(callNumberTexture_);
                callNumberTexture_ = 0;
            }
        }
        ReleaseCurrentTexture();
        if (sourceOccupant_) {
            cISC4Occupant* occupant = sourceOccupant_;
            sourceOccupant_ = nullptr;
            occupant->Release();
        }
        if (pylonOwner_) {
            cIGZUnknown* owner = pylonOwner_;
            pylonOwner_ = nullptr;
            owner->Release();
        }
    }
    return true;
}

// 0x10022e6da
bool cSC4PylonOccupant::PlaceInOccupantManager(bool place) {
    IPylonOccupantHost* host = g_host;
    cISC4OccupantManager* manager = host ? host->OccupantManager() : nullptr;
    if ((((flags_ & 1) != 0) ? true : false) == place) return true;
    if (!manager) return false;
    if ((flags_ & 1) == 0) {
        if (!manager->InsertOccupant(static_cast<cISC4Occupant*>(this), 0)) return false;
        flags_ |= 1;
    } else {
        if ((flags_ & 2) != 0) {
            if (host && host->RendererPresent())
                host->RendererRemoveModel(static_cast<s3d::cIS3DModelInstance*>(this));
            flags_ &= static_cast<std::uint8_t>(~2u);
        }
        manager->RemoveOccupant(static_cast<cISC4Occupant*>(this), false, 0);
        flags_ &= static_cast<std::uint8_t>(~1u);
    }
    return true;
}

// 0x10022e79c
bool cSC4PylonOccupant::ReleaseCurrentTexture() {
    if (textureBinding_ == nullptr) {
        if (currentTexture_ != 0) {
            if (g_host) g_host->ReleaseTexture(currentTexture_);   // the original checks SL::spGDriver first
        }
    } else {
        textureBinding_->Release();
        textureBinding_ = nullptr;
    }
    return true;
}

// 0x10022e7fc
bool cSC4PylonOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(kPylonRecordVersion);
    if (cSC4Occupant::Write(stream)) {
        stream.SetUint8(dirty_ ? 1 : 0);
        stream.SetSint8(static_cast<std::int8_t>(uiState_));
        stream.SetSint8(static_cast<std::int8_t>(dispatchState_));
        stream.SetSint8(static_cast<std::int8_t>(dispatchType_));
        stream.SetUint8(blink_ ? 1 : 0);
        stream.SetUint16(callNumber_);
        SC4WriteGZSerializable(stream, sourceOccupant_);
        stream << transform_;
        stream.SetUint8(dragState_ ? 1 : 0);
        stream.SetSint32(bitmapId_);
        stream.SetGZStr(bitmapName_);
        SC4WriteGZSerializable(stream, pylonOwner_);
    }
    return !stream.GetError();
}

// 0x10022e92a
bool cSC4PylonOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < 3) return false;
    if (cSC4Occupant::Read(stream)) {
        std::uint8_t flag = 0;
        std::int8_t state = 0;
        stream.GetUint8(flag);
        dirty_ = flag != 0;
        stream.GetSint8(state);
        uiState_ = state;
        stream.GetSint8(state);
        dispatchState_ = state;
        stream.GetSint8(state);
        dispatchType_ = state;
        stream.GetUint8(flag);
        blink_ = flag != 0;
        stream.GetUint16(callNumber_);
        if (sourceOccupant_) {
            cISC4Occupant* previous = sourceOccupant_;
            sourceOccupant_ = nullptr;
            previous->Release();
        }
        SC4ReadGZSerializable(stream, kGZIID_cISC4Occupant, reinterpret_cast<void**>(&sourceOccupant_), false);
        if (version < 4)
            LoadLegacyS3DTransform(stream, transform_);
        else
            stream >> transform_;
        stream.GetUint8(flag);
        dragState_ = flag != 0;
        if (version > 4) {
            stream.GetSint32(bitmapId_);
            stream.GetGZStr(bitmapName_);
            if (pylonOwner_) {
                cIGZUnknown* previous = pylonOwner_;
                pylonOwner_ = nullptr;
                previous->Release();
            }
            SC4ReadGZSerializable(stream, 1, reinterpret_cast<void**>(&pylonOwner_), false);
        }
    }
    return !stream.GetError();
}

// 0x10022eaf6
bool cSC4PylonOccupant::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    if (type == kPylonMessageAnimationTick) {
        const std::uint64_t elapsedMs = static_cast<cIGZMessage2Standard*>(message)->GetData2();
        DoAnimationTick(static_cast<std::uint32_t>(elapsedMs / 1000u));
    } else if (type == kPylonMessageViewChanged) {
        dirty_ = true;
    } else if (type == kPylonMessageViewSet) {
        auto** slot = static_cast<IPylonViewSet**>(static_cast<cIGZMessage2Standard*>(message)->GetVoid2());
        IPylonViewSet* view = slot ? *slot : nullptr;
        if (view == nullptr) return true;   // the original dereferences it; the port skips a null view
        const bool inView = view->ContainsOccupant(static_cast<cISC4Occupant*>(this));
        const bool shown = (flags_ & 2) != 0;
        if (!inView) {
            if (shown) {
                flags_ &= static_cast<std::uint8_t>(~2u);
                if (g_host && g_host->RendererPresent())
                    g_host->RendererRemoveModel(static_cast<s3d::cIS3DModelInstance*>(this));
            }
        } else if (!shown) {
            flags_ |= 2;
            if (g_host && g_host->RendererPresent())
                g_host->RendererInsertModel(static_cast<s3d::cIS3DModelInstance*>(this));
        }
    } else if (type == kPylonMessageRebuild) {
        RebuildModel();
    }
    return true;
}

// 0x10022ec28
void cSC4PylonOccupant::DoAnimationTick(std::uint32_t seconds) {
    static std::uint32_t flashTimer = 0;   // the original's function-local static: shared by every pylon
    const std::int32_t state = GetDispatchState();
    if ((state == 4 || state == 1)) {
        flashTimer += seconds;
        if (flashTimer > 499) {
            flashTimer = 0;
            dirty_ = true;
            blink_ = !blink_;
        }
    }
}

// The axis-aligned world box of the billboard's two corners (-halfWidth, 0, 0) and (+halfWidth, height, 0), as
// RebuildModel's vectorised tail builds it (0x10022f037..0x10022f157).
void cSC4PylonOccupant::ComputeWorldBounds(float halfWidth, float height) {
    const float corners[2][3] = {{-halfWidth, 0.0f, 0.0f}, {halfWidth, height, 0.0f}};
    for (int axis = 0; axis < 3; ++axis) {
        worldMin_[axis] = 3.4028235e38f;
        worldMax_[axis] = -3.4028235e38f;
    }
    for (const auto& corner : corners) {
        float point[3];
        for (int row = 0; row < 3; ++row) {
            point[row] = transform_.translation[row];
            for (int column = 0; column < 3; ++column)
                point[row] += transform_.rotation[row][column] * corner[column];
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (point[axis] < worldMin_[axis]) worldMin_[axis] = point[axis];
            if (point[axis] > worldMax_[axis]) worldMax_[axis] = point[axis];
        }
    }
}

// 0x10022ec78
void cSC4PylonOccupant::RebuildModel() {
    IPylonOccupantHost* host = g_host;

    const float halfWidth = UnitsToWorld(host, 44.0f) * 0.5f;
    const float cosPitch = std::cos(host ? host->ViewPitchAngle() : 0.0f);
    const float height = UnitsToWorld(host, 150.0f) / cosPitch;

    RebuildCurrentTexture();

    const int index = (dispatchType_ == 4) ? 1 : 0;
    float stateFactor = (uiState_ != 0) ? static_cast<float>(uiState_) : 1.0f;
    if (blink_) stateFactor = kPylonBlinkStateFactor;
    const float u0 = stateFactor * kPylonQuadUSpan[index];
    const float u1 = kPylonQuadUSpan[index] + u0;
    const float v1 = kPylonQuadV[index];

    quad_[0] = PylonVertexT2F{-halfWidth, height, 0.0f, u0, 0.0f};
    quad_[1] = PylonVertexT2F{-halfWidth, 0.0f, 0.0f, u0, v1};
    quad_[2] = PylonVertexT2F{halfWidth, 0.0f, 0.0f, u1, v1};
    quad_[3] = PylonVertexT2F{halfWidth, height, 0.0f, u1, 0.0f};

    // force a unit scale (flag 2 = uniform scale)
    if (transform_.scale != 1.0f) {
        transform_.scale = 1.0f;
        transform_.flags &= static_cast<std::uint8_t>(~2u);
        ++transform_.changeCounter;
    }

    // rotation: flag 4 means "RebuildModel composed this rotation", so start again from the identity in that case
    float previous[3][3];
    std::uint8_t flags = transform_.flags;
    if ((flags & 4) != 0) {
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column) previous[row][column] = (row == column) ? 1.0f : 0.0f;
        flags &= static_cast<std::uint8_t>(~4u);
        transform_.flags = flags;
        ++transform_.changeCounter;
    } else {
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column) previous[row][column] = transform_.rotation[row][column];
    }
    float viewX = 0.0f, viewZ = 0.0f;
    if (host) host->ViewDirection(viewX, viewZ);
    const float yaw = std::atan2(-viewX, -viewZ);
    const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
    for (int row = 0; row < 3; ++row) {
        transform_.rotation[row][0] = previous[row][0] * cosYaw - previous[row][2] * sinYaw;
        transform_.rotation[row][1] = previous[row][1];
        transform_.rotation[row][2] = previous[row][0] * sinYaw + previous[row][2] * cosYaw;
    }
    transform_.flags |= 4;
    ++transform_.changeCounter;

    boundsMin_[0] = -halfWidth;
    boundsMin_[1] = 0.0f;
    boundsMin_[2] = 0.0f;
    boundsMax_[0] = halfWidth;
    boundsMax_[1] = height;
    boundsMax_[2] = 0.0f;
    ComputeWorldBounds(halfWidth, height);

    if (callNumber_ != 0) BuildCallNumberTexture(height);

    // placement
    cISC4OccupantManager* manager = host ? host->OccupantManager() : nullptr;
    if ((flags_ & 2) == 0) {
        if ((flags_ & 1) == 0)
            PlaceInOccupantManager(true);
        else if (manager)
            manager->MoveOccupant(static_cast<cISC4Occupant*>(this), false);
        if ((flags_ & 1) != 0) PlaceInRenderer(true);
    } else {
        if (host) {
            host->RendererMoveModel(static_cast<s3d::cIS3DModelInstance*>(this));
            host->RendererChangeModel(static_cast<s3d::cIS3DModelInstance*>(this), 3);
        }
        if ((flags_ & 1) != 0 && manager)
            manager->MoveOccupant(static_cast<cISC4Occupant*>(this), false);
    }
    dirty_ = false;
}

// 0x10022f744
bool cSC4PylonOccupant::RebuildCurrentTexture() {
    ReleaseCurrentTexture();
    IPylonOccupantHost* host = g_host;
    if (dispatchType_ == 4) {
        BuildMySimTexture();
    } else if (host) {
        const std::uint32_t instance = PylonTextureInstance(dispatchType_, dispatchState_);
        textureBinding_ = host->CreateTextureBinding(instance);
        if (textureBinding_) currentTexture_ = textureBinding_->TextureId();
    }
    return currentTexture_ != 0;
}

// 0x10022f858 (PARTIAL: the font / buffer block that rasterises the digits calls RZ::WCM()'s font object and
// nsTexUtil::SetupTexture, both behind IPylonOccupantHost; the geometry it builds is the original's.)
void cSC4PylonOccupant::BuildCallNumberTexture(float topY) {
    IPylonOccupantHost* host = g_host;
    if (!host) return;

    IPylonFont* font = host->CallNumberFont();
    if (!font) {
        SC4_UNWIRED("cSC4PylonOccupant::BuildCallNumberTexture: call number font / texture upload not wired (no digits on the dispatch pylon)");
        return;
    }
    font->AddRef();
    const int previousSize = font->GetFontSize();
    font->SetFontSize(kPylonCallNumberFontSize);

    cRZString text;
    text.Sprintf("%d", static_cast<int>(callNumber_));
    const int width = font->TextWidth(text.ToChar(), text.Strlen());
    const int height = font->TextHeight(text.ToChar(), text.Strlen());
    int textureSize = 0x20;
    if (width > 32 || height > 32)
        textureSize = NearestPowerOfTwo(width > height ? width : height);

    if (callNumberTexture_ != 0) {
        host->ReleaseTexture(callNumberTexture_);
        callNumberTexture_ = 0;
    }
    cIGZBuffer* buffer = nullptr;
    if (host->CreateBuffer(&buffer) && buffer) {
        const cGZBufferColorType colorType{kBufferColorARGB8888, 0x20};
        if (buffer->Init(static_cast<std::uint32_t>(textureSize), static_cast<std::uint32_t>(textureSize), colorType)) {
            if (buffer->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) {
                buffer->Fill(0, nullptr);
                buffer->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
            }
            font->DrawText(buffer, 0, 0, text.ToChar(), text.Strlen(), 0xff000000u, 0x08000000u, nullptr, 0);
            host->UploadTexture(buffer, callNumberTexture_);
        }
        buffer->Release();
    }

    // the quad hangs under topY: 14 and 9 viewport units above the top edge, both scaled out of the pitch
    const float cosPitch = std::cos(host->ViewPitchAngle());
    const float inset14 = UnitsToWorld(host, 14.0f) / cosPitch;
    const float inset9 = UnitsToWorld(host, 9.0f) / cosPitch;
    const float halfWidth = UnitsToWorld(host, static_cast<float>(width)) * 0.5f;
    const float top = topY - inset14 - inset9;
    const float bottom = top - inset14;
    const float invTexture = 1.0f / static_cast<float>(textureSize);
    const float u = invTexture * static_cast<float>(width);
    const float v = invTexture * static_cast<float>(height);
    callNumberQuad_[0] = PylonVertexT2F{-halfWidth, top, 0.01f, 0.0f, 0.0f};
    callNumberQuad_[1] = PylonVertexT2F{-halfWidth, bottom, 0.01f, 0.0f, v};
    callNumberQuad_[2] = PylonVertexT2F{halfWidth, bottom, 0.01f, u, v};
    callNumberQuad_[3] = PylonVertexT2F{halfWidth, top, 0.01f, u, 0.0f};

    font->SetFontSize(previousSize);
    font->Release();
}

// 0x100230076 (PARTIAL: the two source pictures come from cSLBuffer and, for the text case, from RZ::GZCOM()'s
// image decoder — both unrecovered neighbours behind the host; the four tile blits are the original's.)
bool cSC4PylonOccupant::BuildMySimTexture() {
    IPylonOccupantHost* host = g_host;
    if (!host) return false;
    const std::uint32_t baseInstance = kMySimDispatchPylonResId[static_cast<std::uint32_t>(dispatchState_) % 8u];
    cSLBuffer base(kPylonBitmapType, kPylonBaseBitmapGroup, baseInstance, kPylonBitmapColorType, true);
    cIGZBuffer* destination = base.GetBuffer();
    if (!destination) return false;
    destination->AddRef();

    cIGZBuffer* source = nullptr;
    if (bitmapId_ == 0) {
        if (!host->LoadBitmapFromFile(bitmapName_, &source))
            SC4_UNWIRED("cSC4PylonOccupant::BuildMySimTexture: MySim picture from file (image decoder) not wired");
    } else {
        cSLBuffer bitmap(kPylonBitmapType, kPylonBitmapGroup, static_cast<std::uint32_t>(bitmapId_),
                         kPylonBitmapColorType, false);
        bitmap.Load();
        source = bitmap.GetBuffer();
        if (source) source->AddRef();
    }

    if (source) {
        const cRZRect* area = source->GetBufferArea();
        const std::uint32_t sourceWidth = source->Width();
        const int startX = area->left + static_cast<int>((0x34u - sourceWidth) >> 1);
        int destinationX = area->top + static_cast<int>((0x34u - sourceWidth) >> 2);
        const std::uint32_t sourceHeight = source->Height();
        const std::uint32_t sourceStride = source->GetColorSurfaceStride();
        auto* sourceBits = static_cast<std::uint32_t*>(source->GetColorSurfaceBits());
        const std::uint32_t destinationStride = destination->GetColorSurfaceStride();
        const std::uint32_t destinationBytesPerPixel = destination->GetBytesPerPixel();
        auto* destinationBits = static_cast<std::uint32_t*>(destination->GetColorSurfaceBits());
        if (sourceHeight > 0) {
            for (std::uint32_t tile = 1; tile < 4; ++tile) {
                destination->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
                source->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
                // the tile index moves the *column* by bytesPerPixel, the running start-y moves the row by
                // the stride (0x10023024a..0x100230264)
                auto* row = reinterpret_cast<std::uint32_t*>(
                    reinterpret_cast<std::uint8_t*>(destinationBits) +
                    destinationStride * static_cast<std::uint32_t>(destinationX) +
                    destinationBytesPerPixel * (tile * 0x34u + static_cast<std::uint32_t>(startX)));
                auto* sourceRow = sourceBits;
                for (std::uint32_t y = 0; y < sourceHeight; ++y) {
                    for (std::uint32_t x = 0; x < sourceWidth; ++x) {
                        if (row[x] > 0x3fffffffu) continue;                 // keep pixels with alpha >= 0x40
                        const std::uint32_t pixel = sourceRow[x];
                        if ((pixel & 0x00ffffffu) == 0x00ff00ffu) continue;  // magenta is the colour key
                        row[x] = pixel;
                    }
                    row = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(row) + destinationStride);
                    sourceRow = reinterpret_cast<std::uint32_t*>(
                        reinterpret_cast<std::uint8_t*>(sourceRow) + sourceStride);
                }
                destination->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
                source->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
                destinationX += 0x34;
            }
        }
    }
    const bool uploaded = host->UploadTexture(destination, currentTexture_);
    if (source) source->Release();
    destination->Release();
    return uploaded;
}

// 0x10022fc98
bool cSC4PylonOccupant::PlaceInRenderer(bool place) {
    IPylonOccupantHost* host = g_host;
    if (host) host->OccupantManager();   // the original asks SL::spCity for it and throws the result away
    if (((flags_ & 2) != 0) == place) return true;
    if (!host || !host->RendererPresent()) return false;
    if ((flags_ & 2) != 0) {
        host->RendererRemoveModel(static_cast<s3d::cIS3DModelInstance*>(this));
        flags_ &= static_cast<std::uint8_t>(~2u);
        return true;
    }
    if ((flags_ & 1) == 0) PlaceInOccupantManager(true);
    if ((flags_ & 1) != 0 && cSC4Occupant::IsFlagSet(kOccupantFlagVisible) &&
        host->RendererInsertModel(static_cast<s3d::cIS3DModelInstance*>(this))) {
        flags_ |= 2;
        return true;
    }
    return false;
}

// 0x10022fd60
void cSC4PylonOccupant::GetPosition(float* xyz) {
    if (!xyz) return;
    xyz[0] = transform_.translation[0];
    xyz[1] = transform_.translation[1];
    xyz[2] = transform_.translation[2];
}

// 0x10022fd9c
void cSC4PylonOccupant::SetPosition(const float* xyz) {
    if (!xyz) return;
    PlaceInOccupantManager(false);
    PlaceInRenderer(false);
    transform_.translation[0] = xyz[0];
    transform_.translation[1] = xyz[1];
    transform_.translation[2] = xyz[2];
    transform_.flags |= 1;
    ++transform_.changeCounter;
    RebuildModel();
    PlaceInOccupantManager(true);
    PlaceInRenderer(true);
}

// 0x10022fe1a
void cSC4PylonOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {
    for (int i = 0; i < 3; ++i) {
        minXyz[i] = worldMin_[i];
        maxXyz[i] = worldMax_[i];
    }
}

// 0x10022fe8e
void cSC4PylonOccupant::SetVisibility(bool visible, bool postNow) {
    cSC4Occupant::SetVisibility(visible, postNow);
    if ((flags_ & 1) != 0) PlaceInRenderer(visible);
}

// 0x10022fed8
bool cSC4PylonOccupant::Pick(cSC4PickContext* context) {
    if (!context) return false;
    if (uiState_ == 0) return false;
    IPylonOccupantHost* host = g_host;
    const float drop = host ? host->ViewportUnitsToWorldUnits(50.0f) / std::cos(host->ViewPitchAngle()) : 50.0f;
    const float boxMin[3] = {worldMin_[0], worldMax_[1] - drop, worldMin_[2]};
    const float boxMax[3] = {worldMax_[0], worldMax_[1], worldMax_[2]};
    float hit[3] = {0.0f, 0.0f, 0.0f};
    if (!SC4GeometryUtilities::RayIntersectsBoundingBox(&context->origin.x, &context->direction.x, boxMin, boxMax,
                                                        hit))
        return false;
    const float distance = SC4GeometryUtilities::LengthOfLine(&context->origin.x, hit);
    if (distance <= context->maxDistance) {
        context->maxDistance = distance;
        return true;
    }
    return false;
}

// 0x10022ffe8
void cSC4PylonOccupant::SetPylonOwner(cIGZUnknown* owner) { ReplaceUnknown(pylonOwner_, owner); }

// 0x10023004a
void cSC4PylonOccupant::SetBitmapInfo(std::int32_t bitmapId, const cIGZString& name) {
    bitmapId_ = bitmapId;
    bitmapName_.AssignFromIGZString(name);
    dirty_ = true;
}

// 0x1002304be
void cSC4PylonOccupant::SetCallNumber(std::uint16_t callNumber) {
    callNumber_ = callNumber;
    dirty_ = true;
}

// 0x10022f24c
void cSC4PylonOccupant::SetUIState(std::int32_t state) {
    if (uiState_ != state) {
        dirty_ = true;
        blink_ = false;
    }
    uiState_ = state;
}

// 0x10022f27a
void cSC4PylonOccupant::SetDispatchState(std::int32_t state) {
    if (dispatchState_ != state) {
        dirty_ = true;
        blink_ = false;
    }
    dispatchState_ = state;
}

// 0x10022f2a8
void cSC4PylonOccupant::SetDispatchType(std::int32_t type) {
    if (dispatchType_ != type) {
        dirty_ = true;
        blink_ = false;
    }
    dispatchType_ = type;
}

// 0x10022f2d6
void cSC4PylonOccupant::SetSourceOccupant(cISC4Occupant* occupant) {
    ReplaceSourceOccupant(sourceOccupant_, occupant);
}

// 0x10022f32a
void cSC4PylonOccupant::GetBounds(cS3DBoundingBox& out) {
    for (int i = 0; i < 3; ++i) {
        out.min[i] = boundsMin_[i];
        out.max[i] = boundsMax_[i];
    }
}

// 0x10022f3aa
void cSC4PylonOccupant::GetWorldBounds(cS3DBoundingBox& out) {
    for (int i = 0; i < 3; ++i) {
        out.min[i] = worldMin_[i];
        out.max[i] = worldMax_[i];
    }
}

// 0x10022f42a
void cSC4PylonOccupant::GetTransform(cS3DTransform& out) { nS3D::AssignTransform(out, transform_); }

// 0x10022f4c0
void cSC4PylonOccupant::SetTransform(const cS3DTransform& value) { nS3D::AssignTransform(transform_, value); }

// 0x10022f55a
void cSC4PylonOccupant::GetSortValue(std::uint32_t& out) const { out = currentTexture_; }

// 0x10022f580
int cSC4PylonOccupant::Draw(SC4DrawContext* context, bool highlight) {
    if (highlight) return 1;
    if (!context) return 1;
    if (g_host && g_host->OccupantDrawSuppressed()) return 1;
    const std::int32_t state = GetDispatchState();
    const bool drawCallNumber = !(state == 1 || state == 4 || callNumber_ == 0 || dispatchType_ == 4);
    if (context->RenderPass() == 1) {
        context->SetModelTransform(transform_);
        context->ClearTexTransform(0);
        context->ResetTransparency();
        context->SetRenderState(renderStatePass1_);
        context->SetTexture(static_cast<int>(currentTexture_), 0);
        context->DrawPrims(6, 2, 4, quad_);
        if (drawCallNumber) {
            context->SetTexture(static_cast<int>(callNumberTexture_), 0);
            context->DrawPrims(6, 2, 4, callNumberQuad_);
        }
        return 1;
    }
    if (context->RenderPass() == 0) {
        if (dirty_) RebuildModel();
        context->SetModelTransform(transform_);
        context->ClearTexTransform(0);
        context->SetTransparency(0.4f);
        context->SetRenderState(renderStatePass0_);
        context->SetTexture(static_cast<int>(currentTexture_), 0);
        context->DrawPrims(6, 2, 4, quad_);
        context->ResetTransparency();
        return 2;
    }
    return 1;
}

// 0x1002304b4
cISC4Occupant* cSC4PylonOccupant::AsOccupant() { return static_cast<cISC4Occupant*>(this); }

// 0x100230514
cIGZUnknown* cSC4PylonOccupant::GetOwner() { return static_cast<cISC4Occupant*>(this); }

}  // namespace sc4
