// cSC4ModelMaker — see ModelMaker.h for the object layout, the quirks kept and the seam map. The original vtable
// (address point 0x100a971f0) is: 0x000 QueryInterface, 0x008 AddRef, 0x010 Release, 0x018 DoMessage, 0x020 / 0x028
// the two destructors, 0x030 Write, 0x038 Read, 0x040 GetGZCLSID; the second group (+8, 0x100a97248) is the
// cIGZSerializable view with the three thunks and Write / Read / GetGZCLSID.
#include "sc4/ModelMaker.h"
#include "platform/PerfLog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

#include "gz/cGZMessageServer.h"
#include "rz/cRZVersion.h"
#include "s3d/S3DMath.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/Lot.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"

namespace sc4 {

int nModelLODOffset = 2;

namespace {

IModelMakerHost* g_host = nullptr;
IModelMakerUtility* g_utility = nullptr;

template <class T>
void release(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

// ---- stream helpers (SC4WriteGZSerializable 0x1000435e3 / SC4ReadGZSerializable 0x100043267) -----------------
// Both only work on a DB segment stream: the helper queries the stream for the segment interface and forwards.
bool WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) {
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &raw)) return false;
    auto* out = static_cast<cISC4DBSegmentOStream*>(raw);
    cIGZSerializable* serializable = nullptr;
    if (object &&
        object->QueryInterface(kGZIID_cIGZSerializable, reinterpret_cast<void**>(&serializable))) {
        const bool ok = out->WriteGZSerializable(serializable);
        serializable->Release();
        out->Release();
        return ok;
    }
    const bool ok = out->WriteGZSerializable(nullptr);
    out->Release();
    return ok;
}

bool ReadSerializable(cIGZIStream& stream, std::uint32_t iid, cIGZUnknown** slot) {
    *slot = nullptr;
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &raw)) return false;
    auto* in = static_cast<cISC4DBSegmentIStream*>(raw);
    cIGZSerializable* object = nullptr;
    const bool ok = in->ReadGZSerializable(&object, false);
    in->Release();
    if (!ok || !object) return ok;
    void* out = nullptr;
    const bool matches = object->QueryInterface(iid, &out);
    object->Release();
    if (!matches) return false;
    *slot = static_cast<cIGZUnknown*>(out);
    return true;
}

// ---- the original's "(int)(v - 0.5 + 12582912) - 12582912" rounding ----------------------------------------------
int RoundMagic(float value) {
    const float biased = value + -0.5f + 12582912.0f;
    const int low = static_cast<int>(biased) - 12582912;
    const int high = static_cast<int>(biased) - 12582911;
    if (std::isnan(value) || std::isnan(biased)) return low;
    return static_cast<float>(high) == value ? high : low;
}

// ---- model instance list helpers -------------------------------------------------------------------------------
// One referenced instance list from an occupant's placeable object (the original's
// cRZStackAllocator<cIS3DModelInstance*,10,1> + GetModelCount / GetModels).
class PlaceableModels {
public:
    explicit PlaceableModels(cISC43DPlaceableObject* object) : object_(object) {
        if (object_) object_->AddRef();
    }
    ~PlaceableModels() {
        if (object_) object_->Release();
    }
    PlaceableModels(const PlaceableModels&) = delete;
    PlaceableModels& operator=(const PlaceableModels&) = delete;

    cISC43DPlaceableObject* object() const { return object_; }
    std::uint32_t count() const { return count_; }
    s3d::cIS3DModelInstance** data() { return buffer_ ? buffer_->data() : nullptr; }
    // The original calls GetModelCount twice: once for the buffer, once for the loop bound.
    bool Fill() {
        if (!object_) return false;
        count_ = object_->GetModelCount();
        if (count_ == 0) return false;
        buffer_.reset(new ModelInstanceBuffer(count_));
        std::uint32_t count = count_;
        // the placeable object keeps the sc4::cIS3DModelInstance view of the same object (Occupant.h's minimal
        // interface; the object is a s3d::cIS3DModelInstance with the same vtable prefix).
        object_->GetModels(reinterpret_cast<sc4::cIS3DModelInstance**>(buffer_->data()), count);
        count_ = count;
        return true;
    }

private:
    cISC43DPlaceableObject* object_ = nullptr;
    std::unique_ptr<ModelInstanceBuffer> buffer_;
    std::uint32_t count_ = 0;
};

cISC43DPlaceableObject* Placeable(cISC4Occupant* occupant) {
    return occupant ? occupant->GetPlaceableObject() : nullptr;
}

}  // namespace

IModelMakerHost* ModelMakerHost() { return g_host; }
void SetModelMakerHost(IModelMakerHost* host) { g_host = host; }
IModelMakerUtility* ModelMakerUtility() { return g_utility; }
void SetModelMakerUtility(IModelMakerUtility* utility) { g_utility = utility; }
namespace {
NetworkModelFactory g_networkModelFactory = nullptr;
}
void SetNetworkModelFactory(NetworkModelFactory factory) { g_networkModelFactory = factory; }

// ---- the deferred occupant operation (0x100314e90) --------------------------------------------------------------
DeferredOccupantOperation::DeferredOccupantOperation(cISC4Occupant* value, OperationType type)
    : occupant(value), operation(type) {
    if (occupant) occupant->AddRef();
}
DeferredOccupantOperation::DeferredOccupantOperation(const DeferredOccupantOperation& other)
    : occupant(other.occupant), operation(other.operation), arg1(other.arg1), arg2(other.arg2) {
    if (occupant) occupant->AddRef();
}
DeferredOccupantOperation& DeferredOccupantOperation::operator=(const DeferredOccupantOperation& other) {
    if (this == &other) return *this;
    cISC4Occupant* previous = occupant;
    occupant = other.occupant;
    operation = other.operation;
    arg1 = other.arg1;
    arg2 = other.arg2;
    if (occupant) occupant->AddRef();
    if (previous) previous->Release();
    return *this;
}
DeferredOccupantOperation::~DeferredOccupantOperation() {
    if (occupant) occupant->Release();
}

ModelInstanceBuffer::ModelInstanceBuffer(std::uint32_t count) {
    if (count > 10) {
        heap_ = new s3d::cIS3DModelInstance*[count]();
        data_ = heap_;
    } else {
        data_ = small_;
    }
}
ModelInstanceBuffer::~ModelInstanceBuffer() { delete[] heap_; }

// ---- construction / destruction ---------------------------------------------------------------------------------
cSC4ModelMaker::cSC4ModelMaker() {
    zoom_ = rotation_ = -1;                                        // +0x38 / +0x3c
    previousZoom_ = previousRotation_ = currentZoom_ = currentRotation_ = -1;   // +0x16c..+0x178
    backgroundLoading_ = true;                                     // +0xf8
    for (int& radius : loadRadius_) radius = 0x100;                // memset_pattern16(.., 0x01000100, 0x14)
    zoomMask_ = 0x1f;                                              // +0xf0
    rotationMask_ = 0xf;                                           // +0xf4
}

cSC4ModelMaker::~cSC4ModelMaker() {
    // +0x140 first, then the five texture bindings (the original walks them backwards from +0x138).
    deferred_.clear();
    for (int i = 4; i >= 0; --i) {
        if (dynamicTexture_[i]) {
            dynamicTexture_[i]->Release();
            dynamicTexture_[i] = nullptr;
        }
    }
}

bool cSC4ModelMaker::QueryInterface(std::uint32_t iid, void** out) {
    if (out == nullptr) return false;
    if (static_cast<std::int32_t>(iid) < 1) {
        if (iid != kGZCLSID_cSC4ModelMaker && iid != kGZIID_cIGZSerializable) return false;
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        if (iid != kGZIID_cIGZUnknown && iid != 0x452294aa) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
    }
    AddRef();
    return true;
}

std::uint32_t cSC4ModelMaker::AddRef() { return ++refCount_; }

std::uint32_t cSC4ModelMaker::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;   // the original calls vslot 0x28, the deleting destructor
        return 0;
    }
    return --refCount_;
}

// ---- Init / Shutdown --------------------------------------------------------------------------------------------
bool cSC4ModelMaker::Init() {
    if (initialized_) return true;
    if (!viewFilter_ && g_host) viewFilter_ = g_host->CurrentViewFilter();
    CreateOccupantFilters();
    if (g_host) {
        if (cIGZMessageServer2* server = g_host->MessageServer()) {
            server->AddNotification(this, kMessageCityPostInit);
            server->AddNotification(this, kMessageCityLoaded);
            server->AddNotification(this, kMessageCityUnloaded);
        }
    }
    if (!resourceManager_) {
        resourceManager_ = SL::spRM;
        if (resourceManager_) resourceManager_->AddRef();
    }
    if (!renderer_ && g_host) {
        renderer_ = g_host->Renderer();
        if (renderer_) renderer_->AddRef();
    }
    if (renderer_ && g_host) viewUtilities_ = g_host->ViewUtilities();
    lighting_[0] = lighting_[1] = lighting_[2] = 0;                 // +0x10c / +0x110 / +0x114
    if (g_host) {
        const int level = g_host->RenderDetailLevel();
        nModelLODOffset = level < 3 ? level : 2;
        if (level < 1) nModelLODOffset = 0;
    }
    DetermineLoadRadiusAroundVisibleArea();
    initialized_ = true;
    return true;
}

bool cSC4ModelMaker::Shutdown() {
    if (!initialized_) return true;
    if (animationCoordinator_) {
        animationCoordinator_->Shutdown();
        animationCoordinator_->Release();
        animationCoordinator_ = nullptr;
    }
    if (backgroundLoader_) {
        backgroundLoader_->Shutdown();
        backgroundLoader_->Release();
        backgroundLoader_ = nullptr;
    }
    release(animationTimer_);      // +0x30
    release(terrain_);             // +0xa8
    release(buildingDevelopment_); // +0xa0
    release(lotManager_);          // +0x98
    release(occupantManager_);     // +0x70
    if (renderer_) {
        viewUtilities_ = nullptr;                                   // +0x28 (borrowed) is dropped with the renderer
        renderer_->Release();
        renderer_ = nullptr;
    }
    release(resourceManager_);     // +0x18
    if (g_host) {
        cIGZMessageServer2* server = g_host->MessageServer();
        if (cityInitialized_) {
            cityInitialized_ = false;
            if (cIGZMessageServer2* tick = g_host->AnimationTickManager()) tick->RemoveNotification(this, 0xc0);
            if (server) {
                static const std::uint32_t kLongList[] = {
                    0x99ef1140, 0x99ef1141, 0x99ef1145, 0x99ef1146, 0xe99b3d4d, 0xe6e85114, 0x4ab04cbe,
                    0x4ab04cbf, 0xa6b79601, 0xa6b79602, 0xa6b79611, 0xa6b79620, 0x99ef1142, 0x99ef1143,
                    0x99ef1144, 0x6cdb65b, 0x2977ae2f, 0x9d8e0b6, 0xe9c86ddb, 0x49d968c4, 0xc9da96ea,
                    0xba44eb51, 0xcad61e00, 0xead61e16, 0xc9f775bb,
                };
                for (std::uint32_t type : kLongList) server->RemoveNotification(this, type);
            }
        }
        if (server) {
            server->RemoveNotification(this, kMessageCityPostInit);
            server->RemoveNotification(this, kMessageCityLoaded);
            server->RemoveNotification(this, kMessageCityUnloaded);
        }
    }
    ReleaseViewFilter();                                            // +0x78
    release(zoomRotationFilter_);  // +0x90
    release(terrainHidingFilter_); // +0x88
    release(modelFilter_);         // +0x80
    deferred_.clear();                                              // releases the occupant references
    initialized_ = false;
    return true;
}

void cSC4ModelMaker::ReleaseViewFilter() {
    if (viewFilter_) {
        viewFilter_->Release();
        viewFilter_ = nullptr;
    }
}

// ---- the occupant filters (0x10030cebc) -------------------------------------------------------------------------
void cSC4ModelMaker::CreateOccupantFilters() {
    if (!modelFilter_) {
        auto* filter = new cSC4OccupantTypesFilterHashed();
        modelFilter_ = filter;
        filter->AddRef();   // the original AddRefs a brand-new filter (count 1)
        static const std::uint32_t kTypes[] = {
            kModelOccupantTypeBuilding,     kModelOccupantTypeFoundation, kModelOccupantTypeRetainingWall,
            kModelOccupantTypeFlora,        kModelOccupantTypeProp,       kModelOccupantTypeLotBaseTexture,
            kModelOccupantTypeNetwork,      kModelOccupantTypeNetworkShadow, kModelOccupantTypeUnderground,
            kModelOccupantTypeMiscA,        kModelOccupantTypeMiscB,
        };
        for (std::uint32_t type : kTypes) filter->AddType(type);
    }
    if (!terrainHidingFilter_) {
        auto* filter = new cSC4OccupantTypesFilterHashed();
        terrainHidingFilter_ = filter;
        filter->AddRef();
        static const std::uint32_t kTypes[] = {
            kModelOccupantTypeFoundation, kModelOccupantTypeLotBaseTexture, kModelOccupantTypeNetwork,
            kModelOccupantTypeUnderground, kModelOccupantTypeMiscA,
        };
        for (std::uint32_t type : kTypes) filter->AddType(type);
    }
    if (!zoomRotationFilter_ && g_host) {
        // cSC4ZoomRotationModelChangeOccupantFilter (0x1002c119a) is not recovered; the host builds it.
        zoomRotationFilter_ = g_host->CreateZoomRotationModelChangeOccupantFilter();
        if (zoomRotationFilter_) zoomRotationFilter_->AddRef();
    }
}

// ---- the load radius (0x10030d0fa) ------------------------------------------------------------------------------
void cSC4ModelMaker::DetermineLoadRadiusAroundVisibleArea() {
    static const float kRadiusIncrementPerMegaByte[5] = {0.5f, 0.25f, 0.125f, 0.0625f, 0.03125f};
    static const int kMinLoadRadius[5] = {8, 8, 6, 6, 6};
    cRZVersion version;
    std::uint32_t total = 0;
    std::uint32_t available = 0;
    if (!version.GetSystemMemory(total, available)) return;
    for (int i = 0; i < 5; ++i) {
        const float radius = kRadiusIncrementPerMegaByte[i] * static_cast<float>(total);
        int cells = RoundMagic(radius);
        if (cells < kMinLoadRadius[i]) cells = kMinLoadRadius[i];
        loadRadius_[i] = cells;
    }
}

// ---- persistence ------------------------------------------------------------------------------------------------
bool cSC4ModelMaker::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    WriteSerializable(stream, renderer_);                                   // +0x20
    stream.SetSint32(zoom_);                                               // +0x38
    stream.SetSint32(rotation_);                                           // +0x3c
    stream.SetSint32(citySizeX_);                                          // +0x40
    stream.SetSint32(citySizeZ_);                                          // +0x44
    const cRZRect rect{cityRect_.x0, cityRect_.z0, cityRect_.x1, cityRect_.z1};
    stream << rect;                                                        // +0x48
    stream.SetUint32(stream58_);                                           // +0x58
    WriteSerializable(stream, reinterpret_cast<cIGZUnknown*>(occupantManager_));   // +0x70
    WriteSerializable(stream, reinterpret_cast<cIGZUnknown*>(lotManager_));        // +0x98
    WriteSerializable(stream, buildingDevelopment_);                       // +0xa0
    WriteSerializable(stream, terrain_);                                   // +0xa8
    stream.SetFloat32(yaw_);                                               // +0x15c
    stream.SetFloat32(rotationAngle_);                                     // +0x160
    stream.SetFloat32(pitch_);                                             // +0x164
    stream.SetFloat32(screenPitch_);                                       // +0x168
    stream.SetSint32(previousZoom_);                                       // +0x16c
    stream.SetSint32(previousRotation_);                                   // +0x170
    stream.SetSint32(currentZoom_);                                        // +0x174
    stream.SetSint32(currentRotation_);                                    // +0x178
    return !stream.GetError();
}

bool cSC4ModelMaker::Read(cIGZIStream& stream) {
    readFromSave_ = true;
    std::uint16_t version = 0;
    if (!stream.GetUint16(version) || version != 2) return false;
    ReadSerializable(stream, 0x681bd529, reinterpret_cast<cIGZUnknown**>(&renderer_));   // +0x20
    stream.GetSint32(zoom_);                                               // +0x38
    stream.GetSint32(rotation_);                                           // +0x3c
    stream.GetSint32(citySizeX_);                                          // +0x40
    stream.GetSint32(citySizeZ_);                                          // +0x44
    stream.GetSint32(cityRect_.x0);                                        // +0x48
    stream.GetSint32(cityRect_.z0);                                        // +0x4c
    stream.GetSint32(cityRect_.x1);                                        // +0x50
    stream.GetSint32(cityRect_.z1);                                        // +0x54
    stream.GetUint32(stream58_);                                           // +0x58
    ReadSerializable(stream, 0xc6bb8334, reinterpret_cast<cIGZUnknown**>(&occupantManager_));   // +0x70
    ReadSerializable(stream, 0x681905bd, reinterpret_cast<cIGZUnknown**>(&lotManager_));        // +0x98
    ReadSerializable(stream, 0x4818e5d2, &buildingDevelopment_);           // +0xa0
    ReadSerializable(stream, 0xc70bd37e, &terrain_);                       // +0xa8
    stream.GetFloat32(yaw_);                                               // +0x15c
    stream.GetFloat32(rotationAngle_);                                     // +0x160
    stream.GetFloat32(pitch_);                                             // +0x164
    stream.GetFloat32(screenPitch_);                                       // +0x168
    stream.GetSint32(previousZoom_);                                       // +0x16c
    stream.GetSint32(previousRotation_);                                   // +0x170
    stream.GetSint32(currentZoom_);                                        // +0x174
    stream.GetSint32(currentRotation_);                                    // +0x178
    return !stream.GetError();
}

// ---- DoMessage (0x10030d75a) ------------------------------------------------------------------------------------
bool cSC4ModelMaker::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    // The original calls the cIGZMessage2Standard payload slots on this very pointer (the messages the server
    // delivers are cRZMessage2Standard objects, whose standard interface sits at offset 0).
    auto* msg = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = msg->GetType();
    auto occupant = [&]() -> cISC4Occupant* { return static_cast<cISC4Occupant*>(msg->GetVoid1()); };
    auto data2 = [&]() -> std::uint32_t { return static_cast<std::uint32_t>(msg->GetData2()); };
    auto data3 = [&]() -> std::uint32_t { return static_cast<std::uint32_t>(msg->GetData3()); };

    if (type == kMessageOccupantInitialized) {
        cISC4Occupant* occ = occupant();
        if (occ && occ->IsInitialized()) {
            msg->GetData2();
            DoOccupantCreated(occ, false, data3());
        }
    } else if (type == kMessageOccupantShutdown) {
        cISC4Occupant* occ = occupant();
        msg->GetData2();
        DoOccupantDestroyed(occ, false, data3());
    } else if (type == kMessageOccupantInserted) {
        cISC4Occupant* occ = occupant();
        const std::uint64_t flag = msg->GetData2();
        DoOccupantInserted(occ, flag != 0, data3());
    } else if (type == kMessageOccupantRemovedFromManager) {
        cISC4Occupant* occ = occupant();
        msg->GetData2();
        DoOccupantRemoved(occ, false, data3());
    } else if (type == kMessageOccupantShown) {
        cISC4Occupant* occ = occupant();
        const std::uint64_t shown = msg->GetData2();
        DoOccupantShown(occ, shown != 0, data3());
    } else if (type == kMessageOccupantChanged) {
        cISC4Occupant* occ = occupant();
        const std::uint64_t changed = msg->GetData2();
        DoOccupantChanged(occ, changed != 0, data3());
    } else if (type == kMessageOccupantMoved) {
        cISC4Occupant* occ = occupant();
        msg->GetData2();
        DoOccupantMoved(occ, false, data3());
    } else if (type == kMessageBackgroundLoaderCommunication) {
        msg->GetData2();
        if (backgroundLoader_) backgroundLoader_->ProcessBackgroundThreadToMainThreadCommunications();
    } else if (type == kMessageViewZoomRotation) {
        const int* oldPoint = static_cast<const int*>(msg->GetVoid1());
        const int* newPoint = static_cast<const int*>(msg->GetVoid2());
        if (!deferViewUpdate_) {
            UpdateModelLoadRect();
            if (oldPoint && newPoint) DoZoomAndRotationChange(oldPoint[0], oldPoint[1], newPoint[0], newPoint[1]);
        } else if (newPoint) {
            pendingZoom_ = newPoint[0];
            pendingRotation_ = newPoint[1];
        }
    } else if (type == kMessageViewZoomRotationDeferred || type == kMessageViewTranslationDeferred) {
        if (!deferViewUpdate_) {
            const int* point = static_cast<const int*>(msg->GetVoid2());
            if (point) {
                currentZoom_ = point[0];
                zoom_ = point[0];
                currentRotation_ = point[1];
                rotation_ = point[1];
            }
        }
    } else if (type == kMessageViewTranslation) {
        UpdateModelLoadRect();
        DoViewTranslationChange();
    } else if (type == kMessageViewFilterChange) {
        msg->GetVoid1();
        DoViewFilterChange(message, msg->GetVoid2());
    } else if (type == kMessageLightingChanged) {
        DoLightingChanged();
    } else if (type == kMessageRenderDetailLevelChanged) {
        const int level = g_host ? g_host->RenderDetailLevel() : 0;
        nModelLODOffset = level < 3 ? level : 2;
        if (level < 1) nModelLODOffset = 0;
    } else if (type == kMessageSuspendLoading) {
        loadingSuspended_ = true;
        if (backgroundLoading_ && backgroundLoader_) backgroundLoader_->SuspendLoading();
    } else if (type == kMessageResumeLoading) {
        loadingSuspended_ = false;
        UpdateModelLoadRect();
        if (backgroundLoading_ && backgroundLoader_) backgroundLoader_->ResumeLoading();
        DoViewTranslationChange();
    } else if (type == kMessageOccupantHighlight) {
        cISC4Occupant* occ = occupant();
        const int from = static_cast<int>(msg->GetData2());
        const int to = static_cast<int>(msg->GetData3());
        DoOccupantHilighted(occ, from, to);
    } else if (type == kMessagePropStateChangedM) {
        auto* prop = static_cast<cISC4Occupant*>(msg->GetVoid1());
        msg->GetData2();
        DoPropStateChanged(prop, static_cast<std::uint8_t>(msg->GetData1()));
    } else if (type == kMessageLotConditionChanged) {
        // The original QueryInterfaces the message's unknown for cISC4Lot (0xe818fced) and calls DoLotStateChanged
        // with it; the port resolves the lot from the occupant's cell instead (cSC4Lot is a plain class here).
        auto* raw = static_cast<cIGZUnknown*>(msg->GetIGZUnknown());
        cISC4Occupant* occ = nullptr;
        if (raw && raw->QueryInterface(kGZIID_cISC4Occupant, reinterpret_cast<void**>(&occ))) {
            const std::uint32_t oldCondition = static_cast<std::uint32_t>(msg->GetData1());
            const std::uint32_t newCondition = data2();
            msg->GetData3();
            float position[3] = {};
            occ->GetPosition(position);
            int cx = 0;
            int cz = 0;
            if (g_host) g_host->PositionToCityCell(position[0], position[2], cx, cz);
            cSC4Lot* lot = lotManager_ ? lotManager_->GetLot(cx, cz, false) : nullptr;
            if (lot) DoLotStateChanged(lot, oldCondition, newCondition);
            occ->Release();
        }
    } else if (type == kMessageCityLoaded) {
        cityLoaded_ = true;
    } else if (type == kMessageCityUnloaded) {
        cityLoaded_ = false;
    } else if (type == kMessageCityPostInit) {
        DoPostCityInit();
    } else if (type == kMessageBuildingCompletionChanged) {
        cISC4Occupant* occ = occupant();
        void* raw = nullptr;
        if (occ && occ->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw)) {
            DoBuildingCompletionChanged(static_cast<cISC4BuildingOccupant*>(raw));
            static_cast<cISC4BuildingOccupant*>(raw)->Release();
        }
    } else if (type == kMessageDeferredOccupantOperations) {
        EnableDeferredOccupantOperations(msg->GetData1() != 0);
    } else if (type == kMessageUnloadModels) {
        UnloadUnnecessaryModels();
    } else if (type == kMessageLotStateChangedM) {
        // 0x9d8e0b6: the occupant's lot changed state; the effective condition comes from the lot under it.
        auto* raw = static_cast<cIGZUnknown*>(msg->GetIGZUnknown());
        cISC4Occupant* occ = nullptr;
        if (raw && raw->QueryInterface(kGZIID_cISC4Occupant, reinterpret_cast<void**>(&occ))) {
            float position[3] = {};
            occ->GetPosition(position);
            int cx = 0;
            int cz = 0;
            if (g_host) g_host->PositionToCityCell(position[0], position[2], cx, cz);
            std::uint32_t condition = 0;
            if (cSC4Lot* lot = lotManager_ ? lotManager_->GetLot(cx, cz, false) : nullptr) {
                condition = lot->GetCondition();
            }
            occ->GetOrCreatePlaceableObject();   // the original calls it for its side effect
            ApplyLotStateEffect(occ, 0, condition);
            occ->Release();
        }
    }
    return true;
}

// ---- deferred-operation bookkeeping -----------------------------------------------------------------------------
void cSC4ModelMaker::PushDeferred(cISC4Occupant* occupant, DeferredOccupantOperation::OperationType type) {
    deferred_.push_back(DeferredOccupantOperation(occupant, type));
}

void cSC4ModelMaker::EnableDeferredOccupantOperations(bool enable) {
    const int previous = deferredDepth_;
    deferredDepth_ = previous - 1 + (enable ? 2 : 0);
    if (previous > 0 && deferredDepth_ < 1 && !deferred_.empty()) CommitDeferredOccupantOperations();
}

void cSC4ModelMaker::CommitDeferredOccupantOperations() {
    if (deferred_.empty()) return;
    // Pass 1: a "destroyed" record cancels an earlier "created" record for the same occupant (and vice versa); the
    // cancelled pair is dropped from both slots.
    for (std::size_t i = deferred_.size(); i-- > 0;) {
        if (deferred_[i].operation != DeferredOccupantOperation::kDestroyed) continue;
        for (std::size_t j = i; j-- > 0;) {
            if (deferred_[j].occupant != deferred_[i].occupant) continue;
            const std::uint8_t earlier = deferred_[j].operation;
            DeferredOccupantOperation blank;
            std::swap(deferred_[j], blank);
            if (earlier == DeferredOccupantOperation::kCreated) {
                DeferredOccupantOperation blankLater;
                std::swap(deferred_[i], blankLater);
            }
            break;
        }
    }
    // Pass 2: a "changed" record is dropped when a later "inserted" record for the same occupant exists.
    for (std::size_t i = deferred_.size(); i-- > 0;) {
        if (deferred_[i].operation != DeferredOccupantOperation::kChanged) continue;
        for (std::size_t j = i; ++j < deferred_.size();) {
            if (deferred_[j].occupant == deferred_[i].occupant &&
                deferred_[j].operation == DeferredOccupantOperation::kInserted) {
                DeferredOccupantOperation blank;
                std::swap(deferred_[i], blank);
                break;
            }
        }
    }
    for (const DeferredOccupantOperation& record : deferred_) {
        if (!record.occupant || record.operation == 0) continue;
        const bool visible = (record.occupant->GetFlags() & kOccupantFlagVisible) != 0;
        const std::uint32_t type = record.occupant->GetType();
        switch (record.operation) {
            case DeferredOccupantOperation::kCreated: DoOccupantCreated(record.occupant, false, type); break;
            case DeferredOccupantOperation::kDestroyed: DoOccupantDestroyed(record.occupant, false, type); break;
            case DeferredOccupantOperation::kShown: DoOccupantShown(record.occupant, visible, type); break;
            case DeferredOccupantOperation::kChanged: DoOccupantChanged(record.occupant, visible, type); break;
            case DeferredOccupantOperation::kMoved: DoOccupantMoved(record.occupant, false, type); break;
            case DeferredOccupantOperation::kHilighted:
                DoOccupantHilighted(record.occupant, static_cast<int>(static_cast<std::int8_t>(record.arg1)),
                                    static_cast<int>(static_cast<std::int8_t>(record.arg2)));
                break;
            case DeferredOccupantOperation::kInserted: DoOccupantInserted(record.occupant, visible, type); break;
            case DeferredOccupantOperation::kRemoved: DoOccupantRemoved(record.occupant, false, type); break;
            default: break;
        }
    }
    deferred_.clear();
}

// ---- the occupant handlers --------------------------------------------------------------------------------------
void cSC4ModelMaker::DoOccupantCreated(cISC4Occupant* occupant, bool flag, std::uint32_t type) {
    if (!occupant || !modelFilter_ || !modelFilter_->IsOccupantTypeIncluded(type)) return;
    (void)flag;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kCreated);
        return;
    }
    occupant->GetOrCreatePlaceableObject();   // the original calls it and drops the reference
    currentZoom_ = zoom_;
    if (auto* placeable = Placeable(occupant)) {
        if (placeable->GetModelCount() != 0) return;
    }
    AddModels(occupant, zoom_, rotation_, type, false);
}

void cSC4ModelMaker::DoOccupantDestroyed(cISC4Occupant* occupant, bool flag, std::uint32_t type) {
    if (!occupant || !modelFilter_ || !modelFilter_->IsOccupantTypeIncluded(type)) return;
    (void)flag;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kDestroyed);
        return;
    }
    RemoveModels(occupant, type);
}

void cSC4ModelMaker::DoOccupantChanged(cISC4Occupant* occupant, bool flag, std::uint32_t type) {
    if (!occupant || !modelFilter_ || !modelFilter_->IsOccupantTypeIncluded(type)) return;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kChanged);
        return;
    }
    if (!occupant->IsFlagSet(1)) return;   // the original asks for flag 1 (+0x100 slot with value 1)
    if (flag) {
        const std::uint32_t occupantType = occupant->GetType();
        RemoveModels(occupant, occupantType);
        if (viewFilter_ && viewFilter_->IsOccupantIncluded(occupant)) {
            AddModels(occupant, zoom_, rotation_, occupantType, true);
        }
    } else if (cISC43DPlaceableObject* placeable = Placeable(occupant)) {
        placeable->AddRef();
        placeable->RemoveAll();
        placeable->SetVisibility(false);
        placeable->Release();
    }
}

void cSC4ModelMaker::DoOccupantMoved(cISC4Occupant* occupant, bool flag, std::uint32_t type) {
    if (!occupant) return;
    if (type != kModelOccupantTypeFlora && type != kModelOccupantTypeProp) return;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kMoved);
        return;
    }
    float position[3] = {};
    occupant->GetPosition(position);
    PlaceableModels models(Placeable(occupant));
    if (!models.Fill()) return;
    for (std::uint32_t i = 0; i < models.count(); ++i) {
        s3d::cIS3DModelInstance* instance = models.data()[i];
        cS3DTransform transform;
        instance->GetTransform(transform);
        transform.translation[0] = position[0];
        transform.translation[1] = position[1];
        transform.translation[2] = position[2];
        ++transform.changeCounter;
        transform.flags |= nS3D::kTransformTranslation;
        instance->SetTransform(transform);
        if (g_host && g_host->RendererHasModelInstance(instance)) g_host->RendererUpdateModelInstance(instance);
    }
    (void)flag;
}

void cSC4ModelMaker::DoOccupantHilighted(cISC4Occupant* occupant, int from, int to) {
    if (!occupant || from == to) return;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kHilighted);
        deferred_.back().arg1 = static_cast<std::uint8_t>(from);
        deferred_.back().arg2 = static_cast<std::uint8_t>(to);
        return;
    }
    PlaceableModels models(Placeable(occupant));
    if (!models.Fill()) return;
    for (std::uint32_t i = 0; i < models.count(); ++i) {
        s3d::cIS3DModelInstance* instance = models.data()[i];
        s3d::SC4DrawAttributes attributes = instance->GetDrawAttributes();
        // +0x00 bit 0 = "highlighted", +0x01 byte = the highlight value (the original writes both raw).
        attributes.flags = (attributes.flags & ~1u) | (to != 0 ? 1u : 0u);
        attributes.flags = (attributes.flags & 0xffff00ffu) | (static_cast<std::uint32_t>(to & 0xff) << 8);
        instance->SetDrawAttributes(attributes);
        if (g_host && g_host->RendererHasModelInstance(instance)) g_host->RendererAddModelInstance(instance, 1);
    }
}

void cSC4ModelMaker::DoOccupantInserted(cISC4Occupant* occupant, bool flag, std::uint32_t type) {
    if (!occupant || !modelFilter_ || !modelFilter_->IsOccupantTypeIncluded(type) || !flag) return;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kInserted);
        return;
    }
    DoOccupantShown(occupant, true, type);
}

void cSC4ModelMaker::DoOccupantRemoved(cISC4Occupant* occupant, bool flag, std::uint32_t type) {
    if (!occupant || !modelFilter_ || !modelFilter_->IsOccupantTypeIncluded(type)) return;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kRemoved);
        return;
    }
    DoOccupantShown(occupant, false, type);
    (void)flag;
}

void cSC4ModelMaker::DoOccupantShown(cISC4Occupant* occupant, bool shown, std::uint32_t type) {
    if (!occupant || !modelFilter_ || !modelFilter_->IsOccupantTypeIncluded(type)) return;
    if (deferredDepth_ > 0) {
        PushDeferred(occupant, DeferredOccupantOperation::kShown);
        return;
    }
    bool visible = shown;
    if (shown) {
        if (viewFilter_ && viewFilter_->IsOccupantIncluded(occupant)) {
            occupant->SetFlag(2);
            visible = true;
        } else {
            occupant->ClearFlag(2);
            visible = false;
        }
    } else {
        occupant->ClearFlag(2);
        visible = false;
    }
    if (terrainHidingFilter_ && terrainHidingFilter_->IsOccupantTypeIncluded(type)) {
        DoTerrainHidingForOccupantCells(occupant, type, visible);
    }
    if (cISC43DPlaceableObject* placeable = Placeable(occupant)) {
        placeable->AddRef();
        placeable->SetVisibility(visible);
        placeable->Release();
    }
    if (animationCoordinator_) animationCoordinator_->SetOccupantAnimation(occupant, shown);
}

void cSC4ModelMaker::DoPropStateChanged(cISC4Occupant* prop, std::uint8_t state) {
    if (!prop) return;
    cISCPropertyHolder* holder = prop->AsPropertyHolder();
    if (!holder || !holder->HasProperty(0x4a70d491)) {
        const std::uint32_t type = prop->GetType();
        RemoveModels(prop, type);
        AddModels(prop, zoom_, rotation_, type, true);
    } else {
        ChangeModels(prop, 0, 2);
    }
    (void)state;
}

void cSC4ModelMaker::DoLotStateChanged(cSC4Lot* lot, std::uint32_t oldState, std::uint32_t newState) {
    if (!lot || !occupantManager_) return;
    std::int32_t rect[4] = {};
    lot->GetBoundingRect(rect);
    OccupantList list;
    occupantManager_->GetOccupantsByStandardCityCells(list, rect, rect + 2, nullptr, 0xffffffff);
    for (cISC4Occupant* occupant : list) {
        if (occupant->GetPlaceableObject()) ApplyStateEffectToModels(occupant, occupant->GetType(), newState);
    }
    occupantManager_->ReleaseOccupantList(list);
    (void)oldState;
}

// ---- the view updates -------------------------------------------------------------------------------------------
void cSC4ModelMaker::UpdateModelLoadRect() {
    if (!g_host) return;
    float focusX = 0;
    float focusZ = 0;
    float rect[4] = {};
    if (!g_host->ViewGroundRectangle(focusX, focusZ, rect)) return;
    loadRect_.x0 = RoundMagic(rect[0] * 0.0625f);
    loadRect_.z0 = RoundMagic(rect[1] * 0.0625f);
    loadRect_.x1 = RoundMagic(rect[2] * 0.0625f);
    loadRect_.z1 = RoundMagic(rect[3] * 0.0625f);
    focusX_ = RoundMagic(focusX * 0.0625f);
    focusZ_ = RoundMagic(focusZ * 0.0625f);
    // the original reads the zoom straight out of the view utilities (+0x14), not the cached +0x38
    const int viewZoom = g_host->ViewZoom();
    const int radius = loadRadius_[viewZoom < 0 ? 0 : (viewZoom > 4 ? 4 : viewZoom)];
    if (!backgroundLoading_ || !backgroundLoader_) {
        loadRect_.x0 -= radius;
        loadRect_.z0 -= radius;
        loadRect_.x1 += radius;
        loadRect_.z1 += radius;
    } else {
        backgroundLoader_->ExpandLoadRect(loadRect_, radius);
    }
    if (loadRect_.x0 < 0) loadRect_.x0 = 0;
    if (loadRect_.z0 < 0) loadRect_.z0 = 0;
    if (citySizeX_ <= loadRect_.x1) loadRect_.x1 = citySizeX_ - 1;
    if (citySizeZ_ <= loadRect_.z1) loadRect_.z1 = citySizeZ_ - 1;
}

void cSC4ModelMaker::DoZoomAndRotationChange(int oldZoom, int oldRotation, int zoom, int rotation) {
    if (zoom == oldZoom) {
        if (rotation == oldRotation) return;
    } else {
        for (cIGZUnknown*& binding : dynamicTexture_) {
            if (binding) {
                binding->Release();
                binding = nullptr;
            }
        }
    }
    unloadPending_ = true;
    if (!occupantManager_) return;
    currentZoom_ = zoom;
    currentRotation_ = rotation;
    previousZoom_ = oldZoom;
    previousRotation_ = oldRotation;
    if (g_host) {
        SC4ViewUtilities::GetCameraViewAnglesForRotation(rotation, zoom, yaw_, pitch_, screenPitch_);
        rotationAngle_ = g_host->ViewRotationAngle();
    }
    if (!backgroundLoading_) {
        if (zoomRotationFilter_ && g_host) g_host->SetZoomRotationFilter(zoomRotationFilter_, zoom, rotation);
        OccupantList list;
        occupantManager_->GetOccupantsByOccupantManagerCells(list, nullptr, nullptr, zoomRotationFilter_,
                                                            0xffffffff);
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "ModelMaker: zoom/rotation %d,%d -> %d,%d: %zu occupants\n", oldZoom, oldRotation, zoom, rotation, list.size());
        if (!list.empty()) {
            inViewIteration_ = true;
            for (cISC4Occupant* occupant : list) LoadOccupantModelsForZoomAndRotation(occupant, zoom, rotation);
            occupantManager_->ReleaseOccupantList(list);
            inViewIteration_ = false;
            const SC4RectT<int> city{0, 0, citySizeX_ - 1, citySizeZ_ - 1};
            DoTerrainHidingForCells(city, false);
            if (terrain_) DoTerrainHidingForCells(city, false);
        }
        return;
    }
    const bool cityReady = cityLoaded_;
    if (rotation != oldRotation && !cityReady) RemoveModelsInappropriateForRotation(rotation);
    if (cityReady) {
        if (backgroundLoader_) {
            backgroundLoader_->CancelAllLoadTasks();
            loadTaskActive_ = false;
        }
    } else {
        loadTaskActive_ = true;
        if (backgroundLoader_) {
            const SC4Point<int> focus{focusX_, focusZ_};
            backgroundLoader_->StartLoadingForZoomAndRotation(zoom, rotation, loadRect_, focus);
            loadFocusX_ = focusX_;
            loadFocusZ_ = focusZ_;
        }
        return;
    }
    if (zoomRotationFilter_ && g_host) g_host->SetZoomRotationFilter(zoomRotationFilter_, zoom, rotation);
    OccupantList list;
    occupantManager_->GetOccupantsByOccupantManagerCells(list, nullptr, nullptr, zoomRotationFilter_, 0xffffffff);
    if (!list.empty()) {
        inViewIteration_ = true;
        for (cISC4Occupant* occupant : list) LoadOccupantModelsForZoomAndRotation(occupant, zoom, rotation);
        occupantManager_->ReleaseOccupantList(list);
        inViewIteration_ = false;
        const SC4RectT<int> city{0, 0, citySizeX_ - 1, citySizeZ_ - 1};
        DoTerrainHidingForCells(city, false);
    }
}

void cSC4ModelMaker::DoViewTranslationChange() {
    if (loadingSuspended_) return;
    unloadPending_ = true;
    if (!backgroundLoading_ || cityLoaded_) {
        if (!occupantManager_) return;
        const double start = platform::PerfLogOn() ? platform::PerfNowMs() : 0.0;
        OccupantList list;
        occupantManager_->GetOccupantsByOccupantManagerCells(list, nullptr, nullptr, modelFilter_, 0xffffffff);
        for (cISC4Occupant* occupant : list) LoadOccupantModelsForZoomAndRotation(occupant, currentZoom_, currentRotation_);
        if (platform::PerfLogOn())
            platform::PerfNote("model maker: view moved, %zu occupants checked in %.1f ms", list.size(), platform::PerfNowMs() - start);
        occupantManager_->ReleaseOccupantList(list);
        return;
    }
    if (loadTaskActive_) {
        UpdateFocusOfBackgroundLoad();
        return;
    }
    if (backgroundLoader_) {
        const SC4Point<int> focus{focusX_, focusZ_};
        backgroundLoader_->StartLoadingForZoomAndRotation(currentZoom_, currentRotation_, loadRect_, focus);
        loadFocusX_ = focusX_;
        loadFocusZ_ = focusZ_;
    }
    loadTaskActive_ = true;
}

void cSC4ModelMaker::DoViewFilterChange(void* oldInfo, void* newInfo) {
    if (!newInfo) return;
    // cSC4ViewFilterInformation is one referenced filter pointer at +0.
    cISC4OccupantFilter* filter = *static_cast<cISC4OccupantFilter**>(newInfo);
    if (filter == viewFilter_) return;
    if (filter) filter->AddRef();
    if (viewFilter_) viewFilter_->Release();
    viewFilter_ = filter;
    if (deferViewUpdate_ || !occupantManager_) return;
    OccupantList list;
    occupantManager_->GetOccupantsByOccupantManagerCells(list, nullptr, nullptr, modelFilter_, 0xffffffff);
    if (!list.empty()) {
        inViewIteration_ = true;
        for (cISC4Occupant* occupant : list) OccupantFilterIterationFunction(occupant);
        occupantManager_->ReleaseOccupantList(list);
        inViewIteration_ = false;
        const SC4RectT<int> city{0, 0, citySizeX_ - 1, citySizeZ_ - 1};
        DoTerrainHidingForCells(city, false);
    }
    (void)oldInfo;
}

void cSC4ModelMaker::UpdateFocusOfBackgroundLoad() {
    if (!backgroundLoading_) return;
    int dx = focusX_ - loadFocusX_;
    if (dx < 0) dx = -dx;
    int dz = focusZ_ - loadFocusZ_;
    if (dz < 0) dz = -dz;
    if (static_cast<std::uint32_t>(dx + dz) <= 4) return;
    if (!backgroundLoader_) return;
    const SC4Point<int> focus{focusX_, focusZ_};
    backgroundLoader_->RefocusLoadToNewViewLocation(loadRect_, focus);
    loadFocusX_ = focusX_;
    loadFocusZ_ = focusZ_;
}

// ---- model creation ---------------------------------------------------------------------------------------------
bool cSC4ModelMaker::ChangeModels(cISC4Occupant* occupant, std::uint32_t type, int layer) {
    if (!occupant) return false;
    if (layer == 0) layer = 3;
    if (type == 0) (void)occupant->GetType();   // quirk: the type is computed and thrown away
    PlaceableModels models(Placeable(occupant));
    if (!models.object()) return false;
    if (!models.Fill()) return true;
    for (std::uint32_t i = 0; i < models.count(); ++i) {
        s3d::cIS3DModelInstance* instance = models.data()[i];
        if (g_host && g_host->RendererHasModelInstance(instance)) {
            g_host->RendererAddModelInstance(instance, layer);
        }
    }
    return true;
}

bool cSC4ModelMaker::RemoveModels(cISC4Occupant* occupant, std::uint32_t type) {
    if (!occupant) return true;
    if (!inViewIteration_ && terrainHidingFilter_ && terrainHidingFilter_->IsOccupantTypeIncluded(type)) {
        DoTerrainHidingForOccupantCells(occupant, type, false);
    }
    if (cISC43DPlaceableObject* placeable = Placeable(occupant)) {
        placeable->AddRef();
        placeable->RemoveAll();
        placeable->Release();
    }
    return true;
}

void cSC4ModelMaker::RemoveModelsInappropriateForRotation(int rotation) {
    if (!occupantManager_) return;
    OccupantList list;
    occupantManager_->GetOccupantsByOccupantManagerCells(list, nullptr, nullptr, modelFilter_, 0xffffffff);
    for (cISC4Occupant* occupant : list) {
        if (cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject()) {
            placeable->AddRef();
            if (placeable->GetModelCount() != 0) {
                const std::uint32_t rotations = placeable->GetRotationsCreatedFor();
                if ((rotations & (1u << (rotation & 0x1f))) == 0) placeable->RemoveAll();
            }
            placeable->Release();
        }
    }
    occupantManager_->ReleaseOccupantList(list);
}

bool cSC4ModelMaker::OccupantFilterIterationFunction(cISC4Occupant* occupant) {
    if (!occupant) return true;
    const bool included = viewFilter_ && viewFilter_->IsOccupantIncluded(occupant);
    if (included) {
        occupant->SetFlag(2);
    } else {
        occupant->ClearFlag(2);
    }
    const std::uint32_t type = occupant->GetType();
    if (!inViewIteration_ && terrainHidingFilter_ && terrainHidingFilter_->IsOccupantTypeIncluded(type)) {
        DoTerrainHidingForOccupantCells(occupant, type, included);
    }
    if (cISC43DPlaceableObject* placeable = Placeable(occupant)) {
        placeable->AddRef();
        if (included && placeable->GetModelCount() == 0) AddModels(occupant, zoom_, rotation_, type, false);
        placeable->SetVisibility(included);
        placeable->Release();
    }
    if (animationCoordinator_) animationCoordinator_->SetOccupantAnimation(occupant, included);
    return true;
}

bool cSC4ModelMaker::AddModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type, bool visible) {
    if (!occupant) return false;
    SC4CellRect cells{};
    occupant->GetBoundingCityCells(cells);
    if (cells.x1 < loadRect_.x0 || loadRect_.x1 < cells.x0 || cells.z1 < loadRect_.z0 ||
        loadRect_.z1 < cells.z0) {
        return false;
    }
    int zoomMask = 1 << (zoom & 0x1f);
    int rotationMask = 1 << (rotation & 0x1f);
    cISC43DPlaceableObject* placeable = occupant->GetOrCreatePlaceableObject();
    if (placeable) placeable->AddRef();
    bool result = false;
    switch (type) {
        case kModelOccupantTypeRetainingWall:
            result = LoadModelsCommon(occupant, zoom, rotation, type);   // cSC4LotRetainingWallOccupant::AddRetainingWallModels
            zoomMask = static_cast<int>(zoomMask_);
            rotationMask = static_cast<int>(rotationMask_);
            break;
        case kModelOccupantTypeMiscA:
        case kModelOccupantTypeUnderground:
            result = LoadUndergroundConnectionModels(occupant, zoom, rotation);
            ApplyLotStateEffect(occupant, type, 0);
            break;
        case kModelOccupantTypeProp: {
            LoadPropModels(occupant, zoom, rotation, type);
            std::uint32_t condition = 0;
            if (GetOccupantLotCondition(occupant, condition)) {
                ApplyLotStateEffect(occupant, type, condition);
            }
            if (!g_utility || !g_utility->OccupantUsesBuildingStyleModels(occupant)) {
                zoomMask = static_cast<int>(zoomMask_);
                rotationMask = static_cast<int>(rotationMask_);
            }
            result = true;
            break;
        }
        case kModelOccupantTypeNetwork:
            result = LoadNetworkModels(occupant, zoom, rotation);
            ApplyLotStateEffect(occupant, type, 0);
            zoomMask = static_cast<int>(zoomMask_);
            rotationMask = static_cast<int>(rotationMask_);
            break;
        case kModelOccupantTypeBuilding:
            result = LoadBuildingModels(occupant, zoom, rotation, type);
            if (result) {
                void* raw = nullptr;
                if (occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw)) {
                    auto* building = static_cast<cISC4BuildingOccupant*>(raw);
                    DoBuildingCompletionChanged(building);
                    building->Release();
                }
                std::uint32_t condition = 0;
                if (GetOccupantLotCondition(occupant, condition)) {
                    ApplyLotStateEffect(occupant, type, condition);
                }
            }
            break;
        case kModelOccupantTypeNetworkShadow:
            result = LoadNetworkModels(occupant, zoom, rotation);
            CreateOccupantShadow(occupant, zoom, rotation);
            ApplyLotStateEffect(occupant, type, 0);
            break;
        case kModelOccupantTypeLotBaseTexture:
            result = LoadLotBaseTextureModels(occupant, zoom, rotation, type);
            zoomMask = static_cast<int>(zoomMask_);
            rotationMask = static_cast<int>(rotationMask_);
            result = true;
            break;
        case kModelOccupantTypeFoundation:
            result = LoadFoundationModels(occupant, zoom, rotation, type);
            if (result) {
                std::uint32_t condition = 0;
                if (GetOccupantLotCondition(occupant, condition)) {
                    ApplyStateEffectToModels(occupant, type, condition);
                }
                if (!g_utility || !g_utility->FoundationOccupantUsesBuildingStyleModels(occupant)) {
                    zoomMask = static_cast<int>(zoomMask_);
                    rotationMask = static_cast<int>(rotationMask_);
                }
            }
            break;
        case kModelOccupantTypeMiscB:
            result = LoadModelsCommon(occupant, zoom, rotation, type);
            break;
        case kModelOccupantTypeFlora:
            result = LoadFloraModels(occupant, zoom, rotation, type);
            break;
        default:
            break;
    }
    if (visible && (occupant->GetFlags() & kOccupantFlagVisible) != 0 && viewFilter_ &&
        viewFilter_->IsOccupantIncluded(occupant)) {
        if (!inViewIteration_ && terrainHidingFilter_ && terrainHidingFilter_->IsOccupantTypeIncluded(type)) {
            DoTerrainHidingForOccupantCells(occupant, type, true);
        }
        if (placeable) placeable->SetVisibility(true);
        if (animationCoordinator_) animationCoordinator_->SetOccupantAnimation(occupant, true);
    }
    if (occupant->GetHighlight() != 0) DoOccupantHilighted(occupant, 0, static_cast<int>(occupant->GetHighlight()));
    if (placeable) {
        placeable->SetZoomLevelsCreatedFor(static_cast<std::uint32_t>(zoomMask));
        placeable->SetRotationsCreatedFor(static_cast<std::uint32_t>(rotationMask));
        placeable->Release();
    }
    return result;
}

bool cSC4ModelMaker::GetOccupantLotCondition(cISC4Occupant* occupant, std::uint32_t& condition) {
    condition = 0;
    if (!occupant) return false;
    float position[3] = {};
    occupant->GetPosition(position);
    int cx = 0;
    int cz = 0;
    if (g_host) g_host->PositionToCityCell(position[0], position[2], cx, cz);
    if (cx < cityRect_.x0 || cx > cityRect_.x1 || cz < cityRect_.z0 || cz > cityRect_.z1) return false;
    cSC4Lot* lot = lotManager_ ? lotManager_->GetLot(cx, cz, false) : nullptr;
    if (!lot) return false;
    condition = lot->GetCondition();
    return true;
}

bool cSC4ModelMaker::LoadBuildingModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type) {
    const bool ok = LoadModelsCommon(occupant, zoom, rotation, type);
    if (ok) CreateOccupantShadow(occupant, zoom, rotation);
    return ok;
}

bool cSC4ModelMaker::LoadFloraModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type) {
    const bool ok = LoadModelsCommon(occupant, zoom, rotation, type);
    if (ok) CreateOccupantShadow(occupant, zoom, rotation);
    return ok;
}

bool cSC4ModelMaker::LoadPropModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type) {
    if (g_utility && g_utility->OccupantUsesBuildingStyleModels(occupant)) {
        if (LoadModelsCommon(occupant, zoom, rotation, type)) CreateOccupantShadow(occupant, zoom, rotation);
    }
    return true;   // the original always reports success here
}

bool cSC4ModelMaker::LoadModelsCommon(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type) {
    if (!occupant || !g_utility) return false;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    const bool lit = holder && holder->HasProperty(0xe83a081d);
    float position[3] = {};
    occupant->GetPosition(position);
    if (type == kModelOccupantTypeFoundation) position[1] += 0.2f;
    std::uint32_t orientation = 0;
    int state = 0;
    g_utility->GetOccupantOrientationAndState(occupant, orientation, state);
    std::vector<std::uint32_t> ids;
    std::vector<float> offsets;
    std::uint32_t extraA = 0;
    std::uint32_t extraB = 0;
    g_utility->GetAllModelInstanceIDs(ids, &offsets, occupant, zoom, rotation, state, lit, orientation, &extraA,
                                      &extraB);
    const int modelCount = static_cast<int>(ids.size());
    if (modelCount < 1) return false;
    const cGZPersistResourceKey key{extraA, extraB, 0};   // the pair the utility handed back
    bool result = false;
    for (int i = 0; i < modelCount; ++i) {
        const std::uint32_t instanceId = ids[i];
        if (instanceId == 0) {
            if (i == 0) result = false;
            continue;
        }
        const float* offset = nullptr;
        float offsetStorage[3] = {0, 0, 0};
        if (offsets.size() >= static_cast<std::size_t>(i) * 3 + 3) {
            offsetStorage[0] = offsets[static_cast<std::size_t>(i) * 3 + 0];
            offsetStorage[1] = offsets[static_cast<std::size_t>(i) * 3 + 1];
            offsetStorage[2] = offsets[static_cast<std::size_t>(i) * 3 + 2];
            if (offsetStorage[0] != 0.0f || offsetStorage[1] != 0.0f || offsetStorage[2] != 0.0f) offset = offsetStorage;
        }
        cGZPersistResourceKey modelKey = key;
        modelKey.instance = instanceId;
        s3d::cIS3DModelInstance* instance =
            AddModel(occupant, modelKey, rotation, state, lit, position, offset);
        if (i == 0) result = instance != nullptr;
        if (!instance) continue;
        if (type == kModelOccupantTypeBuilding && i == 0) {
            void* raw = nullptr;
            if (occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw)) {
                auto* building = static_cast<cISC4BuildingOccupant*>(raw);
                building->Release();
            }
            instance->SetProperties(instance->GetProperties() | 0x40);
        } else if (type == kModelOccupantTypeProp) {
            // 0x100313670: a prop carrying 0x4a70d491 (not animatable) gets 0x22 with bit 0 and bit 1 cleared first;
            // every other prop only gains 0x20.
            const std::uint32_t properties = static_cast<std::uint32_t>(instance->GetProperties());
            const bool notAnimatable = holder && holder->HasProperty(0x4a70d491);
            instance->SetProperties(static_cast<int>(notAnimatable ? ((properties & 0xffffffdcu) | 0x22u) : (properties | 0x20u)));
        } else if (type == kModelOccupantTypeFlora) {
            instance->SetProperties(static_cast<int>(static_cast<std::uint32_t>(instance->GetProperties()) | 0x80u));
        }
        std::uint8_t tint = 0;
        if (holder) cSCPropertyHelp::GetPropertyValue(holder, 0xe9f0fa86, tint);
        if (tint != 0) {
            s3d::SC4DrawAttributes attributes = instance->GetDrawAttributes();
            attributes.flags |= 4u;
            instance->SetDrawAttributes(attributes);
        }
    }
    return result;
}

bool cSC4ModelMaker::LoadFoundationModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type) {
    // PARTIAL: the original builds a cSC4FoundationModel (0x10035f09c, not recovered) from the foundation's
    // texture ids and world bounds and adds that as the occupant's model; only the building-style fallback is
    // recovered here.
    if (g_utility && g_utility->FoundationOccupantUsesBuildingStyleModels(occupant)) {
        return LoadModelsCommon(occupant, zoom, rotation, type);
    }
    return false;
}

bool cSC4ModelMaker::LoadLotBaseTextureModels(cISC4Occupant* occupant, int zoom, int rotation, std::uint32_t type) {
    // The lot base texture occupant (0x97f6d98) builds its cell models itself: vslot 0x50 LoadModels(false).
    void* raw = nullptr;
    if (occupant && occupant->QueryInterface(kGZIID_cISC4LotBaseTextureOccupant, &raw)) {
        auto* lotTexture = static_cast<cISC4LotBaseTextureOccupant*>(raw);
        lotTexture->LoadModels(false);
        lotTexture->Release();
    }
    (void)zoom;
    (void)rotation;
    (void)type;
    return true;
}

bool cSC4ModelMaker::LoadNetworkModels(cISC4Occupant* occupant, int zoom, int rotation) {
    // The original asks the occupant for cISC4NetworkOccupant (iid 0xa821ef94) and calls its vslot 0x108
    // GetModelInstances(zoom, rotation, out, 10, count), which hands back the occupant's own model instance; the
    // port's stand-in for that instance is cSC4NetworkOccupantModel (see sc4/NetworkOccupantModel.h). PARTIAL: the
    // 0x2890d4de piece (the pre-built model occupant, vslot 0xc0 of its cSC4NetworkOccupant base) is not handled.
    (void)zoom;
    (void)rotation;
    if (!occupant || !g_networkModelFactory) return false;
    s3d::cIS3DModelInstance* model = g_networkModelFactory(occupant);   // one reference, or null
    if (!model) return false;
    cISC43DPlaceableObject* placeable = occupant->GetOrCreatePlaceableObject();
    if (placeable) {
        placeable->AddRef();
        placeable->AddModel(reinterpret_cast<cIS3DModelInstance*>(model));
        placeable->Release();
    }
    model->Release();
    return placeable != nullptr;
}

bool cSC4ModelMaker::LoadUndergroundConnectionModels(cISC4Occupant* occupant, int zoom, int rotation) {
    // PARTIAL: the original calls cISC4UndergroundOccupant vslot 0x150 GetModelInstances(zoom, rotation, out, 10,
    // count) for types 0x2a183167 / 0x8a13918a; that interface is not recovered, so the list stays empty.
    if (!occupant) return false;
    (void)zoom;
    (void)rotation;
    return false;
}

bool cSC4ModelMaker::ApplyStateEffectToModels(cISC4Occupant* occupant, std::uint32_t type,
                                              std::uint32_t condition) {
    if (type == kModelOccupantTypeMiscA || type == kModelOccupantTypeProp || type == kModelOccupantTypeNetwork) {
        ApplyLotStateEffect(occupant, type, condition);
        return true;
    }
    if (type == kModelOccupantTypeBuilding || type == kModelOccupantTypeUnderground) {
        ApplyLotStateEffect(occupant, type, condition);
        return true;
    }
    if (type == kModelOccupantTypeFoundation) {
        void* raw = nullptr;
        // cISC4FoundationOccupant vslot 0x48 == 2 (the "not a foundation" case) keeps the models as they are
        if (occupant->QueryInterface(0xc8fce101, &raw)) {
            static_cast<cIGZUnknown*>(raw)->Release();
        }
        if (condition == 1) ApplyLotStateEffect(occupant, type, condition);
        return true;
    }
    return true;
}

void cSC4ModelMaker::ApplyLotStateEffect(cISC4Occupant* occupant, std::uint32_t type, std::uint32_t condition) {
    // PARTIAL: the effect replaces / tints the dynamic texture of every material of every model instance
    // (0x10030fe50, 2360 bytes). It needs the S3D texture binding factory (service 0x1ac0e11a), the four occupant
    // interfaces it reads the lot texture ids from, and the material record internals. The port keeps the
    // dispatch (which condition maps to which dynamic texture slot) and the "already has the texture, nothing to
    // do" early out.
    if (!occupant) return;
    cISC43DPlaceableObject* placeable = Placeable(occupant);
    if (placeable) placeable->AddRef();
    static const int kConditionToTexture[7] = {-1, -1, 0, 1, 4, 2, 3};
    if (condition < 7 && kConditionToTexture[condition] >= 0) {
        const int slot = kConditionToTexture[condition];
        if (!dynamicTexture_[slot] && occupantManager_) {
            RebuildDynamicTexture(slot);
            if (dynamicTexture_[slot]) ChangeModels(occupant, kModelOccupantTypeBuilding, 2);
        }
    }
    if (placeable) placeable->Release();
    (void)type;
}

void cSC4ModelMaker::RebuildDynamicTexture(int slot) {
    // PARTIAL: 0x1003116d8 reads the tinted texture resource (key {0x7ab50e44, 0x1ab50c3f}), walks its mip chain
    // with nRZSimGL::TintTexture_* (all recovered) and creates or updates the binding through the texture binding
    // factory (service 0x1ac0e11a, not recovered). The port keeps the slot -> texture id offset table and the
    // tint colour packing; the resource walk and the factory call are left to the factory seam.
    static const std::uint32_t sTextureIDs[5] = {0x0, 0x1, 0x40, 0x80, 0x100};
    static_assert(sizeof(sTextureIDs) == 20, "five dynamic texture ids");
    (void)sTextureIDs;
    if (slot < 0 || slot > 4) return;
    // the tint colour from the three lighting values (see 0x100311a8x): 0xAARRGGBB with the alpha of tint.
    const auto channel = [](float value) -> std::uint32_t {
        const std::uint32_t raw = static_cast<std::uint32_t>(static_cast<int>(value + 49152.0f) - 49152);
        return (raw - (raw >> 8)) & 0xff;
    };
    const std::uint32_t tint = 0xff000000u | (channel(lighting_[0]) << 16) | (channel(lighting_[1]) << 8) |
                               channel(lighting_[2]);
    (void)tint;
}

bool cSC4ModelMaker::DoTerrainHidingForOccupantCells(cISC4Occupant* occupant, std::uint32_t type, bool hide) {
    if (!occupant) return false;
    SC4CellRect cells{};
    occupant->GetBoundingCityCells(cells);
    SC4RectT<int> rect{cells.x0, cells.z0, cells.x1, cells.z1};
    if (rect.x1 < rect.x0 || rect.z1 < rect.z0) return false;
    SC4RectT<int> clipped;
    if (!clipped.IntersectRect(rect, cityRect_)) return false;
    bool waterCells = false;
    if (type == kModelOccupantTypeFoundation && hide) {
        void* raw = nullptr;
        if (occupant->QueryInterface(0xc8fce101, &raw)) {
            static_cast<cIGZUnknown*>(raw)->Release();
        }
    }
    if (!DoTerrainHidingForCells(clipped, waterCells)) return false;
    if (terrain_ && g_host) g_host->TerrainRefreshCell(clipped.x0, clipped.z0);
    return true;
}

bool cSC4ModelMaker::DoTerrainHidingForCells(const SC4RectT<int>& rect, bool water) {
    // PARTIAL: 0x10031200e reads the terrain cell flag word (terrain vslot 0x290), keeps its low three bits, and
    // ORs a per-occupant hiding mask built from the network flags (0xa821ef94 vslot 0x40), the lot base texture
    // occupancy (0x97f6d98 vslot 0x38) and the foundation state (0xc8fce101 vslot 0x48), then writes the word back
    // through terrain vslot 0x288 and refreshes changed cells. Only the flag-preserving write and the change test
    // are recovered here; the three occupant masks need interfaces that are not recovered.
    if (!g_host) return false;
    if (water) {
        for (int x = rect.x0; x <= rect.x1; ++x) {
            for (int z = rect.z0; z <= rect.z1; ++z) {
                const std::uint32_t flags = g_host->TerrainCellFlags(x, z);
                if ((flags & 3u) != 3u) g_host->TerrainSetCellFlags(x, z);
            }
        }
    }
    // gather the current flags, build the mask per occupant and write the changed cells back
    bool changed = false;
    const int sizeX = rect.x1 - rect.x0 + 1;
    const int sizeZ = rect.z1 - rect.z0 + 1;
    std::vector<std::uint32_t> before(static_cast<std::size_t>(sizeX) * sizeZ);
    std::vector<std::uint32_t> after(before.size());
    if (sizeX > 0 && sizeZ > 0) {
        for (int x = 0; x < sizeX; ++x) {
            for (int z = 0; z < sizeZ; ++z) {
                const std::size_t index = static_cast<std::size_t>(x) * sizeZ + z;
                before[index] = g_host->TerrainCellFlags(rect.x0 + x, rect.z0 + z);
                after[index] = before[index];
            }
        }
        // The occupant pass (0x100312564..0x1003127c0) is not recovered: without the three occupant masks the
        // cells keep their flags.
        for (int x = 0; x < sizeX; ++x) {
            for (int z = 0; z < sizeZ; ++z) {
                const std::size_t index = static_cast<std::size_t>(x) * sizeZ + z;
                if (after[index] == before[index]) continue;
                changed = true;
                g_host->TerrainSetCellFlags(rect.x0 + x, rect.z0 + z);
            }
        }
    }
    return changed;
}

// 0x100313a00. Buildings (unless flagged 0xea3209f9), props (only at shadow quality 3 and up and with property 0x8a5e5db8), flora and
// the occupants of type 0x2890d4de cast shadows; a "no shadow" property (0x29244db) stops all of them. The model of the occupant at the
// highest zoom that has one (counting down from the view's) must be at least as tall on screen as the minimum; its textured meshes
// then become shadow decals on the terrain's land overlay, projected along the light by CalcShadowProjection inside the overlay manager.
bool cSC4ModelMaker::CreateOccupantShadow(cISC4Occupant* occupant, int zoom, int rotation) {
    if (!occupant || !g_host || !resourceManager_) return false;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    if (holder) {
        bool noShadow = false;
        const bool found = cSCPropertyHelp::GetPropertyValue(holder, 0x29244db, noShadow);
        if (noShadow && found) return false;
    }
    std::uint32_t orientation = 0;
    int state = 0;
    if (g_utility) g_utility->GetOccupantOrientationAndState(occupant, orientation, state);
    const int quality = g_host->ShadowRenderQuality();
    void* raw = nullptr;
    if (occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) && raw) {
        static_cast<cIGZUnknown*>(raw)->Release();
        bool flagged = false;
        const bool found = holder && cSCPropertyHelp::GetPropertyValue(holder, 0xea3209f9, flagged);
        if (flagged && found) return false;
    } else {
        raw = nullptr;
        if (occupant->QueryInterface(0xe9793a65, &raw) && raw) {                     // a prop
            static_cast<cIGZUnknown*>(raw)->Release();
            bool castsShadow = false;
            const bool found = quality >= 3 && holder && cSCPropertyHelp::GetPropertyValue(holder, 0x8a5e5db8, castsShadow);
            if (!(found && castsShadow)) return false;
        } else {
            raw = nullptr;
            if (occupant->QueryInterface(0xa9c05c85, &raw) && raw) {                  // flora
                static_cast<cIGZUnknown*>(raw)->Release();
            } else {
                if (occupant->GetType() != 0x2890d4de) return false;
                state = 0;
            }
        }
    }
    const bool lit = holder && holder->HasProperty(0xe83a081d);
    if (zoom < 0) return false;
    const int nextRotation = (rotation + 1) % 4;
    for (int z = zoom; z >= 0; --z) {
        cGZPersistResourceKey key{0x5ad0e817, 0xbadb57f1, 0};
        key.instance = g_host->GetModelInstanceID(occupant, z, nextRotation, state, lit, &key.type, &key.group);
        if (key.instance == 0) continue;
        void* resourceRaw = nullptr;
        int verdict = 4;                                                  // 4: try a lower zoom, 2: cast the shadow, 1: no shadow at all
        cS3DBoundingBox modelBounds{{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        std::vector<s3d::ShadowMeshData> meshes;
        if (resourceManager_->GetResource(key, 0x9ad0e7e3, &resourceRaw, 0, nullptr) && resourceRaw) {
            auto* model = static_cast<s3d::cIS3DResModel*>(static_cast<cIGZUnknown*>(resourceRaw));
            const bool haveMeshes = model->GetShadowMeshes(meshes);
            if (haveMeshes && !meshes.empty() && (quality > 3 || meshes.size() == 1)) {
                model->ComputeBounds(modelBounds);
                verdict = 2 - (((modelBounds.max[1] - modelBounds.min[1]) * g_host->ShadowViewScale() < g_host->ShadowMinimumHeight()) ? 1 : 0);
            }
            static_cast<cIGZUnknown*>(resourceRaw)->Release();
        }
        if (verdict == 4) continue;
        if (verdict != 2) return false;

        // the model's transform, turned a quarter (the shadow code's own correction of the model's facing)
        cS3DTransform transform;
        float position[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(position);
        FindModelTransform(holder, lit, state, position, nullptr, transform);
        constexpr float kQuarterCos = 4.371139e-08f;                      // 0x100a18174 (and its negative at 0x100a18a40)
        float* r = &transform.rotation[0][0];
        for (int row = 0; row < 3; ++row) {
            const float a = r[3 * row], c = r[3 * row + 2];
            r[3 * row + 2] = c * -kQuarterCos - a;
            r[3 * row] = c - a * kQuarterCos;
        }
        transform.flags = static_cast<std::uint8_t>(transform.flags | 4);
        transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 1);
        cS3DBoundingBox worldModelBounds = modelBounds;
        SC4ViewUtilities::TransformBoundingBox(transform, worldModelBounds);

        IShadowOverlay* overlay = g_host->ShadowOverlay(0);
        cS3DBoundingBox occupantBounds{{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        for (const s3d::ShadowMeshData& mesh : meshes) {
            if (mesh.textureLayers == 0 || mesh.points.size() < 3) continue;
            if (!overlay) continue;
            occupant->GetBoundingBox(occupantBounds.min, occupantBounds.max);
            const std::uint32_t id = overlay->AddShadow(mesh.texture, static_cast<int>(mesh.points.size()), mesh.points.data(), mesh.uvs.data(),
                                                        quality >= 5 ? worldModelBounds : occupantBounds, transform);
            if (id != 0xffffffffu)
                if (cISC43DPlaceableObject* placeable = Placeable(occupant)) placeable->AddOverlay(id);
        }
        return true;
    }
    return false;
}

void cSC4ModelMaker::LoadOccupantModelsForZoomAndRotation(cISC4Occupant* occupant, int zoom, int rotation) {
    if (!occupant || !g_utility) return;
    const std::uint32_t zoomBit = 1u << (zoom & 0x1f);
    const std::uint32_t rotationBit = 1u << (rotation & 0x1f);
    cISC43DPlaceableObject* placeable = Placeable(occupant);
    bool rebuild = false;
    if (placeable) {
        placeable->AddRef();
        const std::uint32_t zooms = placeable->GetZoomLevelsCreatedFor();
        const std::uint32_t rotations = placeable->GetRotationsCreatedFor();
        if ((zooms & zoomBit) == 0 || (rotations & rotationBit) == 0) {
            rebuild = true;
        }
        placeable->Release();
    } else {
        rebuild = true;
    }
    if (!rebuild) return;
    if (placeable && placeable->GetModelCount() != 0) {
        const int modelClass = g_utility->GetOccupantModelClass(occupant);
        if (modelClass == -0x55edc407) return;   // the occupant's model class has no per-view instances
        // The zoom and rotation the existing models were made for: the lowest bit of each mask (5 = none).
        auto lowestBit = [](std::uint32_t mask, int last) {
            for (int i = 0; i <= last; ++i)
                if (mask & (1u << i)) return i;
            return 5;
        };
        const int madeZoom = lowestBit(placeable->GetZoomLevelsCreatedFor(), 4);
        const int madeRotation = lowestBit(placeable->GetRotationsCreatedFor(), 3);
        std::uint32_t orientation = 0;
        int state = 0;
        g_utility->GetOccupantOrientationAndState(occupant, orientation, state);
        cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        const bool lit = holder && holder->HasProperty(0xe83a081d);
        std::vector<std::uint32_t> existing;
        std::vector<std::uint32_t> target;
        g_utility->GetAllModelInstanceIDs(target, nullptr, occupant, zoom, rotation, state, lit, orientation, nullptr, nullptr);
        if (madeZoom != 5 && madeRotation != 5)
            g_utility->GetAllModelInstanceIDs(existing, nullptr, occupant, madeZoom, madeRotation, state, lit, orientation,
                                              nullptr, nullptr);
        if (existing == target) {
            // The same models serve the new view. A rotation only turns them in place (unless the occupant carries
            // property 0x4835c6a6 or is lit), facing the camera's new angle.
            if (madeRotation == rotation) return;
            if (holder && holder->GetProperty(0x4835c6a6)) {
                // falls through to the rebuild below
            } else {
                if (lit) return;
                std::uint32_t count = static_cast<std::uint32_t>(placeable->GetModelCount());
                std::vector<cIS3DModelInstance*> models(count);
                if (count) placeable->GetModels(models.data(), count);
                const float angle = -rotationAngle_;
                const float sine = std::sin(angle), cosine = std::cos(angle);
                for (cIS3DModelInstance* model : models) {
                    auto* instance = reinterpret_cast<s3d::cIS3DModelInstance*>(model);
                    cS3DTransform transform;
                    instance->GetTransform(transform);
                    transform.rotation[0][0] = cosine; transform.rotation[0][1] = 0; transform.rotation[0][2] = sine;
                    transform.rotation[1][0] = 0;      transform.rotation[1][1] = 1; transform.rotation[1][2] = 0;
                    transform.rotation[2][0] = -sine;  transform.rotation[2][1] = 0; transform.rotation[2][2] = cosine;
                    transform.flags |= nS3D::kTransformRotation;
                    instance->SetTransform(transform);
                }
                placeable->SetZoomLevelsCreatedFor(zoomBit);
                placeable->SetRotationsCreatedFor(rotationBit);
                return;
            }
        }
    }
    const std::uint32_t type = occupant->GetType();
    RemoveModels(occupant, type);
    AddModels(occupant, zoom, rotation, type, true);
}

// ---- lighting and the building completion -----------------------------------------------------------------------
void cSC4ModelMaker::DoLightingChanged() {
    if (!g_host) return;
    float colour[4] = {};
    g_host->LightingColour(colour);
    const float dr = colour[0] - lighting_[0];
    const float dg = colour[1] - lighting_[1];
    const float db = colour[2] - lighting_[2];
    const auto magnitude = [](float value) -> float { return value < 0.0f ? -value : value; };
    if (magnitude(dr) < 0.0001f && magnitude(dg) < 0.0001f && magnitude(db) < 0.0001f) return;
    lighting_[0] = colour[0];
    lighting_[1] = colour[1];
    lighting_[2] = colour[2];
    for (int slot = 0; slot < 5; ++slot) RebuildDynamicTexture(slot);
    if (!occupantManager_) return;
    // props whose models use the building style are rebuilt with a one-type filter (0x278128a0 in the original,
    // which is the model filter's first type).
    OccupantList list;
    occupantManager_->GetOccupantsByOccupantManagerCells(list, nullptr, nullptr, modelFilter_, 0xffffffff);
    for (cISC4Occupant* occupant : list) {
        if (occupant->GetType() != kModelOccupantTypeProp) continue;
        if (cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject()) {
            placeable->AddRef();
            if (placeable->GetModelCount() != 0) ChangeModels(occupant, kModelOccupantTypeProp, 2);
            placeable->Release();
        }
    }
    occupantManager_->ReleaseOccupantList(list);
}

void cSC4ModelMaker::DoBuildingCompletionChanged(cISC4BuildingOccupant* building) {
    if (!building || !g_host) return;
    cISC4Occupant* occupant = building->AsOccupant();
    cISC43DPlaceableObject* placeable = occupant ? occupant->GetPlaceableObject() : nullptr;
    if (!placeable) return;
    placeable->AddRef();
    const float completion = building->GetCompletionPercent();
    placeable->SetOverlaysVisible(completion >= 1.0f);
    PlaceableModels models(placeable);
    if (!models.Fill()) {
        placeable->Release();
        return;
    }
    float position[3] = {};
    occupant->GetPosition(position);
    const float missing = 1.0f - completion;
    for (std::uint32_t i = 0; i < models.count(); ++i) {
        s3d::cIS3DModelInstance* instance = models.data()[i];
        cS3DBoundingBox bounds;
        instance->GetWorldBounds(bounds);
        cS3DTransform transform;
        instance->GetTransform(transform);
        // 0x10030faba: the model rests on the occupant's base; while it is being built it sinks by the unbuilt part of its height
        float top = position[1];
        if (completion < 1.0f) top += (bounds.max[1] - bounds.min[1]) * missing * -1.05f;
        transform.translation[0] = position[0];
        transform.translation[1] = top;
        transform.translation[2] = position[2];
        ++transform.changeCounter;
        transform.flags |= nS3D::kTransformTranslation;
        instance->SetTransform(transform);
        std::uint32_t properties = static_cast<std::uint32_t>(instance->GetProperties());
        if (completion >= 1.0f) {
            if ((properties & 1u) != 0) instance->SetProperties(static_cast<int>(properties & ~1u));
        } else {
            if ((properties & 1u) == 0) instance->SetProperties(static_cast<int>(properties | 1u));
        }
        if (g_host->RendererHasModelInstance(instance)) g_host->RendererAddModelInstance(instance, 5);
    }
    placeable->Release();
}

// ---- the post-city initialization (0x10030f36c) -----------------------------------------------------------------
void cSC4ModelMaker::DoPostCityInit() {
    if (!g_host) return;
    std::string value;
    if (g_host->ReadConfigOption("backgroundLoad", value)) {
        if (value == "off") {
            backgroundLoading_ = false;
        } else if (value == "on") {
            backgroundLoading_ = true;
        }
    }
    if (g_host->ReadConfigOption("backgroundLoadIgnoreMissingModels", value)) {
        if (value == "off") {
            ignoreMissingModels_ = false;
        } else if (value == "on") {
            ignoreMissingModels_ = true;
        }
    }
    if (backgroundLoading_) {
        backgroundLoader_ = g_host->CreateBackgroundLoader();
        if (backgroundLoader_) {
            backgroundLoader_->AddRef();
            backgroundLoader_->Init(this);
        }
    }
    if (!animationCoordinator_) {
        animationCoordinator_ = g_host->CreateAnimationCoordinator();
        if (animationCoordinator_) {
            animationCoordinator_->AddRef();
            animationCoordinator_->Init();
        }
    }
    if (!occupantManager_) {
        occupantManager_ = g_host->OccupantManager();
        if (occupantManager_) occupantManager_->AddRef();
    }
    if (!lotManager_) {
        lotManager_ = g_host->LotManager();
        if (lotManager_) lotManager_->AddRef();
    }
    if (!buildingDevelopment_) {
        buildingDevelopment_ = g_host->BuildingDevelopmentSim();
        if (buildingDevelopment_) buildingDevelopment_->AddRef();
    }
    if (!terrain_) {
        terrain_ = reinterpret_cast<cIGZUnknown*>(g_host->Terrain());
        if (terrain_) terrain_->AddRef();
        if (g_host) {
            citySizeX_ = g_host->TerrainSizeX();
            citySizeZ_ = g_host->TerrainSizeZ();
            cityRect_ = SC4RectT<int>{0, 0, citySizeX_ - 1, citySizeZ_ - 1};
        }
    }
    if (!animationTimer_) {
        if (cIGZMessageServer2* tick = g_host->AnimationTickManager()) {
            animationTimer_ = reinterpret_cast<cIGZUnknown*>(tick);
            animationTimer_->AddRef();
        }
    }
    currentZoom_ = zoom_ = g_host->ViewZoom();
    currentRotation_ = rotation_ = g_host->ViewRotation();
    UpdateModelLoadRect();
    ReleaseViewFilter();
    g_host->RefreshViewFilter();
    // view vslot 0x90 is the view filter information: the occupants below are shown through its filter, so the
    // model maker takes it again here (with a null filter nothing would ever become visible).
    viewFilter_ = g_host->CurrentViewFilter();
    OccupantList list;
    if (occupantManager_) {
        occupantManager_->GetOccupantsByOccupantManagerCells(list, nullptr, nullptr, modelFilter_, 0xffffffff);
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "ModelMaker: post city init, %zu model occupants, load rect %d,%d..%d,%d zoom %d rot %d\n", list.size(), loadRect_.x0, loadRect_.z0, loadRect_.x1, loadRect_.z1, zoom_, rotation_);
        for (cISC4Occupant* occupant : list) {
            const bool visible = (occupant->GetFlags() & kOccupantFlagVisible) != 0;
            const std::uint32_t type = occupant->GetType();
            DoOccupantCreated(occupant, false, type);
            DoOccupantShown(occupant, visible, type);
        }
        occupantManager_->ReleaseOccupantList(list);
    }
    if (cIGZMessageServer2* server = g_host->MessageServer()) {
        static const std::uint32_t kLongList[] = {
            0x99ef1140, 0x99ef1141, 0x99ef1145, 0x99ef1146, 0xe99b3d4d, 0xe6e85114, 0x4ab04cbe, 0x4ab04cbf,
            0xa6b79601, 0xa6b79602, 0xa6b79611, 0xa6b79620, 0x99ef1142, 0x99ef1143, 0x99ef1144, 0x6cdb65b,
            0x2977ae2f, 0x9d8e0b6, 0xe9c86ddb, 0x49d968c4, 0xc9da96ea, 0xba44eb51, 0xcad61e00, 0xead61e16,
            0xc9f775bb,
        };
        for (std::uint32_t type : kLongList) server->AddNotification(this, type);
    }
    cityInitialized_ = true;
}

void cSC4ModelMaker::UnloadUnnecessaryModels() {
    if (!unloadPending_) return;
    cRZVersion version;
    std::uint32_t total = 0;
    std::uint32_t available = 0;
    if (version.GetSystemMemory(total, available)) {
        if (available < 0x20) unloadPending_ = false;   // plenty of memory: keep the models
    }
    if (!unloadPending_) return;
    unloadPending_ = false;
    if (!occupantManager_) return;
    // The original walks the four rectangles around the load rectangle (left, right, top, bottom) and unloads every
    // occupant there; the rectangle table is rebuilt below in that order.
    const int x0 = loadRect_.x0;
    const int z0 = loadRect_.z0;
    const int x1 = loadRect_.x1;
    const int z1 = loadRect_.z1;
    const SC4RectT<int> outside[4] = {
        SC4RectT<int>{0, z0, x0 - 1, z1},
        SC4RectT<int>{x1 + 1, z0, citySizeX_ - 1, z1},
        SC4RectT<int>{0, 0, citySizeX_ - 1, z0 - 1},
        SC4RectT<int>{0, z1 + 1, citySizeX_ - 1, citySizeZ_ - 1},
    };
    for (const SC4RectT<int>& rect : outside) {
        if (rect.x0 > rect.x1 || rect.z0 > rect.z1) continue;
        std::int32_t a[2] = {rect.x0, rect.z0};
        std::int32_t b[2] = {rect.x1, rect.z1};
        OccupantList list;
        occupantManager_->GetOccupantsByStandardCityCells(list, a, b, modelFilter_, 0xffffffff);
        for (cISC4Occupant* occupant : list) {
            cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject();
            if (!placeable) continue;
            placeable->AddRef();
            if (placeable->GetModelCount() != 0) {
                const std::uint32_t zooms = placeable->GetZoomLevelsCreatedFor();
                const std::uint32_t rotations = placeable->GetRotationsCreatedFor();
                if (zooms != zoomMask_ || rotations != rotationMask_) placeable->RemoveAll();
            }
            placeable->Release();
        }
        occupantManager_->ReleaseOccupantList(list);
    }
    if (backgroundLoader_) backgroundLoader_->CancelAllLoadTasks();
}

s3d::cIS3DModelInstance* cSC4ModelMaker::CreatePlaceHolderModelInstance(cISC4Occupant* occupant, int zoom,
                                                                         int rotation, int facing, bool lit) {
    // The footprint's longer side picks the placeholder base instance id (table at 0x100a1d140): one cell uses
    // 0x2a0e0000, 2..4 cells the table, five cells or more 0x2a0a0000. The id then goes through the model-property-1
    // lookup with (facing + rotation * 3) % 4.
    static const std::uint32_t kBaseInstIDs[5] = {0x2a0e0000, 0x2a0d0000, 0x2a0c0000, 0x2a0b0000, 0x2a0a0000};
    if (!occupant || !resourceManager_) return nullptr;
    SC4CellRect cells{};
    occupant->GetBoundingCityCells(cells);
    const int sizeX = cells.x1 - cells.x0;
    const int sizeZ = cells.z1 - cells.z0;
    const int longer = sizeX < sizeZ ? sizeZ : sizeX;
    std::uint32_t base = 0x2a0e0000;
    if (longer > 1) base = 0x2a0a0000;
    if (longer > 1 && longer < 5) base = kBaseInstIDs[longer];
    const int effectiveRotation = lit ? 0 : rotation;
    std::uint32_t instanceId = base;
    if (g_utility) instanceId = g_utility->GetModelProperty1Instance(base, effectiveRotation, (facing + effectiveRotation * 3) % 4);
    const cGZPersistResourceKey key{0x5ad0e817, 0xbadb57f1, instanceId};   // the standard model group
    void* raw = nullptr;
    if (!resourceManager_->GetResource(key, 0x9ad0e7e3, &raw, 0, nullptr) || !raw) return nullptr;
    auto* resource = static_cast<s3d::cIS3DResModel*>(static_cast<cIGZUnknown*>(raw));
    s3d::cIS3DModelInstance* instance = nullptr;
    resource->CreateModelInstance(0, &instance);
    static_cast<cIGZUnknown*>(raw)->Release();
    (void)zoom;
    return instance;
}

s3d::cIS3DModelInstance* cSC4ModelMaker::AddModel(cISC4Occupant* occupant, const cGZPersistResourceKey& key,
                                                  int rotation, int state, bool lit, const float* position,
                                                  const float* offset) {
    // PARTIAL: the animated-model branch (model class 0xea123be1 / 0xaa123bf9 -> GetAnimatedModelFromResourceKey,
    // then the animated object's vslot 0x98 / 0xd8 / 0x108 / 0x118) is not recovered here; every model goes through
    // the S3D resource path. The resource lookup, the size cull, the placeholder fallback, the transform and the
    // placeable-object insert are the original's.
    if (!occupant) return nullptr;
    (void)rotation;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    // The model class picks the animated path (0xea123be1 / 0xaa123bf9, not ported); class 0xaa123bf9 models turn
    // with the occupant like the lit ones (0x1003143b6).
    const int modelClass = g_utility ? g_utility->GetOccupantModelClass(occupant) : 0;
    const bool objectRotation = lit || modelClass == static_cast<int>(0xaa123bf9u);
    cGZPersistResourceKey modelKey = key;
    void* raw = nullptr;
    if (resourceManager_ && resourceManager_->GetResource(modelKey, 0x9ad0e7e3, &raw, 0, nullptr) && raw) {
        // the resource answered
    } else if (resourceManager_ && (modelKey.type != 0x5ad0e817 || modelKey.group != 0xbadb57f1)) {
        // then the same instance as an S3D model of the standard model group {0x5ad0e817, 0xbadb57f1}
        modelKey.type = 0x5ad0e817;
        modelKey.group = 0xbadb57f1;
        if (!resourceManager_->GetResource(modelKey, 0x9ad0e7e3, &raw, 0, nullptr)) raw = nullptr;
    }
    s3d::cIS3DModelInstance* instance = nullptr;
    if (raw) {
        auto* resource = static_cast<s3d::cIS3DResModel*>(static_cast<cIGZUnknown*>(raw));
        cS3DBoundingBox bounds;
        resource->ComputeBounds(bounds);
        const float extentX = bounds.max[0] - bounds.min[0];
        const float extentY = bounds.max[1] - bounds.min[1];
        const float extentZ = bounds.max[2] - bounds.min[2];
        float largest = extentX > extentZ ? extentX : extentZ;
        if (largest < extentY) largest = extentY;
        if (g_host && largest * g_host->ViewportUnitsPerWorldUnit() < g_host->ModelSizeThreshold()) {
            static_cast<cIGZUnknown*>(raw)->Release();
            return nullptr;
        }
        resource->CreateModelInstance(0, &instance);
        static_cast<cIGZUnknown*>(raw)->Release();
    }
    // Port: the animated models (model class 0xea123be1 / 0xaa123bf9, or an animation key 0x29a5d1ec) have no
    // animated-model path here, so they are left out instead of being shown as the placeholder box.
    const bool animated = modelClass == static_cast<int>(0xea123be1u) || modelClass == static_cast<int>(0xaa123bf9u) ||
                          key.type == 0x29a5d1ec;
    if (!instance && !animated && (!ignoreMissingModels_ || (holder && holder->HasProperty(0x6a871b82)))) {
        instance = CreatePlaceHolderModelInstance(occupant, currentZoom_, currentRotation_, state, objectRotation);
    }
    if (!instance) return nullptr;
    cS3DTransform transform;
    FindModelTransform(holder, objectRotation, state, position, offset, transform);
    instance->SetTransform(transform);
    if (occupant->IsFlagSet(0x10)) {
        instance->SetProperties(static_cast<int>(static_cast<std::uint32_t>(instance->GetProperties()) & 1u));
    }
    instance->SetOwner(occupant);
    if (cISC43DPlaceableObject* placeable = occupant->GetOrCreatePlaceableObject()) {
        placeable->AddRef();
        placeable->AddModel(reinterpret_cast<sc4::cIS3DModelInstance*>(instance));
        placeable->Release();
        instance->Release();
        return instance;   // borrowed, like the original
    }
    instance->Release();
    return nullptr;
}

void cSC4ModelMaker::FindModelTransform(cISCPropertyHolder* holder, bool objectRotation, int rotation,
                                        const float* position, const float* offset, cS3DTransform& transform) {
    // 0x100314822. `rotation` is the occupant's orientation (0..3). Object-rotated models turn with the occupant and
    // take their offset as it is; the others face the camera (minus the view's rotation angle) and the offset turns
    // with the occupant's orientation (y unrotated).
    static const float kOrigin[3] = {0, 0, 0};
    bool followTerrain = false;
    if (holder) cSCPropertyHelp::GetPropertyValue(holder, 0x69f14d33, followTerrain);
    float translation[3] = {position[0], position[1], position[2]};
    float angle = 0;
    if (objectRotation) {
        angle = SC4ViewUtilities::GetObjectYRotationAngle(rotation);
        if (offset) {
            translation[0] = position[0] + offset[0];
            translation[1] = position[1] + offset[1];
            translation[2] = position[2] + offset[2];
        }
    } else {
        angle = -rotationAngle_;
        if (offset) {
            float rotated[3] = {0, 0, 0};
            SC4GeometryUtilities::RotatePoint(kOrigin, offset, -SC4ViewUtilities::GetObjectYRotationAngle(rotation), 1, rotated);
            translation[0] = position[0] + rotated[0];
            translation[1] = position[1] + offset[1];
            translation[2] = position[2] + rotated[2];
        }
    }
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    transform.rotation[0][0] = cosine;
    transform.rotation[0][1] = -(cosine * 0.0f);
    transform.rotation[0][2] = sine;
    transform.rotation[1][0] = sine * 0.0f + 0.0f;
    transform.rotation[1][1] = 1.0f - sine * 0.0f * 0.0f;
    transform.rotation[1][2] = -(cosine * 0.0f);
    transform.rotation[2][0] = 0.0f - sine;
    transform.rotation[2][1] = sine * 0.0f + 0.0f;
    transform.rotation[2][2] = cosine;
    ++transform.changeCounter;
    transform.flags |= nS3D::kTransformRotation;
    if (followTerrain && g_host) {
        // 0x100314a74..0x100314cbf: lay the model on the ground slope (the network piece's normal when the cell has a
        // network - the host has no network normal, so the terrain's stands in - else the terrain's, skipped on water).
        // A normal steeper than 30 degrees is pulled back towards a 0.5 horizontal component first.
        const bool network = g_host->NetworkAt(position[0], position[2]);
        if (network || !g_host->TerrainCellHasWater(static_cast<int>(position[0]), static_cast<int>(position[2]))) {
            float n[3] = {0, 0, 0};
            g_host->TerrainNormalAt(position[0], position[2], n);
            if (network) {
                const float inverse = 1.0f / std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                n[0] *= inverse; n[1] *= inverse; n[2] *= inverse;
            }
            if (n[1] != 1.0f) {
                if (n[1] < 0.866f && 0.5f < n[0]) {
                    const float f = 1.0f / std::sqrt(n[2] * n[2] + n[1] * n[1] + 0.25f);
                    n[0] = f * 0.5f; n[1] *= f; n[2] *= f;
                }
                if (n[1] < 0.866f && 0.5f < n[2]) {
                    const float f = 1.0f / std::sqrt(n[1] * n[1] + n[0] * n[0] + 0.25f);
                    n[0] *= f; n[1] *= f; n[2] = f * 0.5f;
                }
                float forward[3] = {0, 0, 1};
                SC4GeometryUtilities::RotatePoint(kOrigin, forward, -angle, 1, forward);
                float c[3] = {n[1] * forward[2] - n[2] * forward[1], n[2] * forward[0] - forward[2] * n[0],
                              forward[1] * n[0] - forward[0] * n[1]};
                const float inverse = 1.0f / std::sqrt(c[2] * c[2] + c[1] * c[1] + c[0] * c[0]);
                c[0] *= inverse; c[1] *= inverse; c[2] *= inverse;
                transform.rotation[0][0] = c[0];
                transform.rotation[0][1] = n[0];
                transform.rotation[0][2] = n[2] * c[1] - n[1] * c[2];
                transform.rotation[1][0] = c[1];
                transform.rotation[1][1] = n[1];
                transform.rotation[1][2] = n[0] * c[2] - n[2] * c[0];
                transform.rotation[2][0] = c[2];
                transform.rotation[2][1] = n[2];
                transform.rotation[2][2] = n[1] * c[0] - n[0] * c[1];
                transform.flags |= nS3D::kTransformRotation;
                ++transform.changeCounter;
            }
        }
    }
    transform.translation[0] = translation[0];
    transform.translation[1] = translation[1];
    transform.translation[2] = translation[2];
    transform.flags |= nS3D::kTransformTranslation;
    ++transform.changeCounter;
}

}  // namespace sc4
