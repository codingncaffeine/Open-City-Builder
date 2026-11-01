// cSC4ConstructionOccupant — see ConstructionOccupant.h for the layout, the seams and the quirks kept. This file
// follows the original's function order (0x100177328..0x10017df2c).
#include "sc4/ConstructionOccupant.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "gz/cGZMessageServer.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZDate.h"
#include "sc4/DBSegment.h"
#include "sc4/DrawContext.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SimStreamOps.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "rz/cRZMessage2Standard.h"
#include "s3d/S3DMath.h"

namespace sc4 {

// ---- the statics (the original keeps them in .data / .bss; see the symbol table) --------------------------------
float sfSegmentHeight = kDefaultSegmentHeight;
float sfPostWidth = kDefaultPostWidth;
float sfScaffoldGap = kDefaultScaffoldGap;
float sfPlankWidth = kDefaultPlankWidth;
float sfScaffoldGrowthRate = kDefaultScaffoldGrowthRate;
float sfScaffoldRemovalRate = kDefaultScaffoldRemovalRate;
float sfCranePostWidth = kDefaultCranePostWidth;
float sfCranePostToBuildingGap = kDefaultCranePostToBuildingGap;
float sfCraneHeightAboveScaffold = kDefaultCraneHeightAboveScaffold;
float sfCraneTimeOfFullRotation = kDefaultCraneTimeOfFullRotation;
float sfPlankTextureScale = kDefaultPlankTextureScale;
float sfScaffoldTextureScale = kDefaultScaffoldTextureScale;
float sfGirderTextureScale = kDefaultGirderTextureScale;
float sfCraneLength[3] = {0, 0, 0};
float sfCraneFrontArmLength[3] = {0, 0, 0};
float sfCraneBackArmLength[3] = {0, 0, 0};
bool sbStaticInitialized = false;
std::uint32_t snTotalPropCount = 0;
std::uint32_t snUniversalPropCount = 0;
std::vector<ConstructionPropEntry> sPropList;

namespace {

IConstructionOccupantHost* g_host = nullptr;

// 0x100b73888: cRZTime::mRefDate.julianDay, the epoch StaticInit's seed counts days from.
constexpr std::uint32_t kJulianReferenceDay = 249522;
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

// The scaffold texture resource ids (0x100a196f0): four sides, the planks and the girders. Init only creates
// bindings for the first five rows (0x1001779fd compares with 5), so the girder row stays empty.
constexpr std::uint32_t kScaffoldTextureResId[kScaffoldTextureRows] = {
    0x696f3000, 0x696f2000, 0x696f1000, 0x696f2000, 0x696f4000, 0x696f5000};
// 0x100a19708: the three crane models.
constexpr std::uint32_t kCraneModelId[3] = {0x1c210000, 0x1c220000, 0x282a0000};
// 0x100a19720 / 0x100a19734: the props every construction site may use, and the ones only the ground may use.
constexpr std::uint32_t kUniversalProps[5] = {0x02000000, 0x02010000, 0x02050000, 0x03040000, 0x03050000};
constexpr std::uint32_t kGroundOnlyProps[3] = {0x03030000, 0x02050000, 0x02060000};
// 0x100a197e0 / 0x100a197ec: the crew and roof automata.
constexpr std::uint32_t kGroundAutomata[3] = {0x0210b000, 0x0210c000, 0x0210a000};
constexpr std::uint32_t kRoofAutomata[2] = {0x02109000, 0x0210d000};
// 0x100a19800 / 0x100a197d0: the prop orientation order and the girder corner order.
constexpr int kPropOrientationTable[4] = {0, 2, 1, 3};
constexpr int kGirderVertexToCorner[4] = {2, 0, 1, 3};
// 0x100a19790: the corner indices of the four sides (a vertical quad between two box corners).
constexpr int kVertexToCorner[16] = {0, 0, 1, 1, 2, 2, 3, 3, 3, 3, 1, 1, 0, 0, 2, 2};
// 0x100a19740 / 0x100a19780: the side draw order per view rotation and the depth offsets.
constexpr int kDrawOrderByRotation[16] = {0, 3, 1, 2, 2, 0, 3, 1, 1, 2, 0, 3, 3, 1, 2, 0};
constexpr int kSideDepthOffset[4] = {1, 1, -1, -1};

// The original's `(int)(v + 12582912.0f) - 12582912` (round to nearest, ties to even).
int RoundNearest(float value) { return static_cast<int>(value + 12582912.0f) - 12582912; }
template <class T>
void ReleaseRef(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

// ---- SC4WriteGZSerializable / SC4ReadGZSerializable (0x1000435e3 / 0x100043267) --------------------------------
// Both only work on a DB segment stream: the helper queries the stream for the segment interface and forwards.
bool WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) {
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &raw)) return false;
    auto* out = static_cast<cISC4DBSegmentOStream*>(raw);
    cIGZSerializable* serializable = nullptr;
    if (object && object->QueryInterface(kGZIID_cIGZSerializable, reinterpret_cast<void**>(&serializable))) {
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

}  // namespace

IConstructionOccupantHost* ConstructionOccupantHost() { return g_host; }
void SetConstructionOccupantHost(IConstructionOccupantHost* host) { g_host = host; }

// ==== 0x100177328 ==============================================================================================
cSC4ConstructionOccupant::cSC4ConstructionOccupant() {
    type_ = kOccupantTypeConstruction;   // the original stores it at +0x58 (the base's +0x48)
    SetProperties(5);                    // +0x8c: the UI/draw properties of the model-instance view
}

// ==== 0x10017dd2a / 0x10017dd3c / 0x10017df2c ==================================================================
// The two small destructors are the compiler's own (a tail call and the deleting variant); the body releases the
// members the original frees explicitly.
cSC4ConstructionOccupant::~cSC4ConstructionOccupant() {
    ReleaseRef(crane_);
    ReleaseRef(dust_);
    for (auto*& spark : sparks_) ReleaseRef(spark);
    sparks_.clear();
    for (auto& row : textures_) {
        for (auto*& binding : row) ReleaseRef(binding);
    }
    textures_.clear();
    crews_.clear();
    groundProps_.clear();
    roofProps_.clear();
}

// ==== 0x1001776ee ==============================================================================================
bool cSC4ConstructionOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4ConstructionOccupant) {
        *out = static_cast<cISC4ConstructionOccupant*>(this);
        AddRef();
        return true;
    }
    // Quirk kept: the original delegates straight to the occupant base, so the model-instance iid (0x26a68449)
    // and the message-target iid are not answered.
    return cSC4Occupant::QueryInterface(iid, out);
}

// ==== 0x1001777fe ==============================================================================================
bool cSC4ConstructionOccupant::Init() {
    if (IsInitialized()) return true;
    cSC4Occupant::Init();
    StaticInit();
    // The original dereferences the texture-binding service and the message server without a check
    // (0x1001779f3); the port treats a missing host as headless.
    IConstructionOccupantHost* host = g_host;
    if (host) {
        if (cIGZMessageServer2* messages = host->MessageServer()) {
            messages->AddNotification(this, kMessageDayTick);
            messages->AddNotification(this, kMessageViewZoomRotationDeferred);
            messages->AddNotification(this, kMessageViewFilterChange);
            messages->AddNotification(this, kMessageCityPostInit);
        }
        host->AddTickNotification(this, kAnimationTickChannel);
        if (textures_.empty()) {
            textures_.assign(kScaffoldTextureRows, std::vector<IConstructionTexture*>(kScaffoldTextureFrames, nullptr));
            // Quirk kept: rows 0..4 only (0x1001779fd), the girder row never gets a binding.
            for (std::uint32_t row = 0; row + 1 < kScaffoldTextureRows; ++row) {
                for (std::uint32_t frame = 0; frame < kScaffoldTextureFrames; ++frame) {
                    const cGZPersistResourceKey key{kTextureBindingType, kTextureBindingGroup,
                                                   kScaffoldTextureResId[row] + frame};
                    textures_[row][frame] = host->CreateTextureBinding(key);
                }
            }
        }
    }
    renderState_.PackRenderState(true, true, true, false, true, 1, false, 3, 4, 5, -1, -1.0f, 1, -1);
    AddOccupantGroup(kOccupantGroupConstruction);
    return true;
}

// ==== 0x100177a46 ==============================================================================================
void cSC4ConstructionOccupant::StaticInit() {
    if (sbStaticInitialized) return;
    sbStaticInitialized = true;

    // Seeds the process-wide generator from the wall clock (0x100177a46: time / localtime / cRZDate).
    std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);
    std::uint32_t seed = 0;
    if (local) {
        const cRZDate today(static_cast<std::uint32_t>(local->tm_mon + 1),
                            static_cast<std::uint32_t>(local->tm_mday),
                            static_cast<std::uint32_t>(local->tm_year + 1900));
        const std::uint32_t julian = today.JulianDay();
        if (julian != 0) {
            seed = static_cast<std::uint32_t>(local->tm_min * 60 + local->tm_sec + local->tm_hour * 3600) +
                   (julian - kJulianReferenceDay) * 86400u;
        }
    }
    if (g_host) {
        if (cRZRandom* random = g_host->Random()) random->Seed(seed);
    }

    // The tuning exemplar ({0x6534284a, 0x2989fd57, 0x8989dbe3}, seek id 0xa52160f5).
    if (g_host) {
        if (const cISCPropertyHolder* holder = g_host->ConstructionSettings()) {
            cSCPropertyHelp::GetPropertyValue(holder, kPropertySegmentHeight, sfSegmentHeight);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyPostWidth, sfPostWidth);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyScaffoldGap, sfScaffoldGap);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyPlankWidth, sfPlankWidth);
            if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyPlankTextureScale, sfPlankTextureScale)) {
                sfPlankTextureScale = 1.0f / sfPlankTextureScale;
            }
            if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyScaffoldTextureScale, sfScaffoldTextureScale)) {
                sfScaffoldTextureScale = 1.0f / sfScaffoldTextureScale;
            }
            if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyGirderTextureScale, sfGirderTextureScale)) {
                sfGirderTextureScale = 1.0f / sfGirderTextureScale;
            }
            if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyScaffoldGrowthRate, sfScaffoldGrowthRate)) {
                sfScaffoldGrowthRate = 1.0f / sfScaffoldGrowthRate;
            }
            if (cSCPropertyHelp::GetPropertyValue(holder, kPropertyScaffoldRemovalRate, sfScaffoldRemovalRate)) {
                sfScaffoldRemovalRate = 1.0f / sfScaffoldRemovalRate;
            }
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyCranePostWidth, sfCranePostWidth);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyCranePostToBuildingGap, sfCranePostToBuildingGap);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyCraneHeightAboveScaffold, sfCraneHeightAboveScaffold);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyCraneTimeOfFullRotation, sfCraneTimeOfFullRotation);
        }
        // The three crane models' bounding boxes give the arm lengths (0x100177bxx).
        for (int i = 0; i < 3; ++i) {
            const cGZPersistResourceKey key{kCraneModelType, kCraneModelGroup, kCraneModelId[i]};
            s3d::cIS3DModelInstance* model = g_host->CreateModelInstance(key, kResourceTypeS3DModel);
            if (!model) continue;
            cS3DBoundingBox box;
            box.min[0] = box.min[1] = box.min[2] = 1.0f;
            box.max[0] = box.max[1] = box.max[2] = 0.0f;
            model->GetBounds(box);
            sfCraneLength[i] = box.max[0] - box.min[0];
            const float low = std::fabs(box.min[0]);
            const float high = std::fabs(box.max[0]);
            sfCraneFrontArmLength[i] = std::max(low, high);
            sfCraneBackArmLength[i] = std::min(low, high);
            model->Release();
        }
        // The construction prop table (0x100177cxx): the universal props first, then the ground-only ones.
        sPropList.clear();
        sPropList.reserve(8);
        snUniversalPropCount = 0;
        for (std::uint32_t type : kUniversalProps) {
            const PropProfile* profile = g_host->PropProfile(type);
            if (!profile) continue;
            sPropList.push_back({type, profile->size[0], profile->size[2]});
            ++snUniversalPropCount;
        }
        snTotalPropCount = snUniversalPropCount;
        for (std::uint32_t type : kGroundOnlyProps) {
            const PropProfile* profile = g_host->PropProfile(type);
            if (!profile) continue;
            sPropList.push_back({type, profile->size[0], profile->size[2]});
            ++snTotalPropCount;
        }
    }
}

// ==== 0x100177fee ==============================================================================================
bool cSC4ConstructionOccupant::Shutdown() {
    if (!IsInitialized()) return true;
    SendConstructionMessage(kMessageConstructionComplete, false);
    cSC4Occupant::Shutdown();
    ReleaseRef(lot_);
    ReleaseRef(building_);
    if (IConstructionOccupantHost* host = g_host) {
        if (cIGZMessageServer2* messages = host->MessageServer()) {
            messages->RemoveNotification(this, kMessageDayTick);
            messages->RemoveNotification(this, kMessageViewZoomRotationDeferred);
            messages->RemoveNotification(this, kMessageViewFilterChange);
            messages->RemoveNotification(this, kMessageCityPostInit);
        }
        host->RemoveTickNotification(this, kAnimationTickChannel);
        for (auto& row : textures_) {
            for (auto*& binding : row) ReleaseRef(binding);
        }
        textures_.clear();
        for (auto& side : sides_) side.clear();
        planks_.clear();
        girders_.clear();
        posts_.clear();
        cranePosts_.clear();
        ReleaseRef(crane_);
        ClearPropList(groundProps_);
        ClearPropList(roofProps_);
        for (auto*& spark : sparks_) {
            host->RemoveProp(spark ? spark->AsOccupant() : nullptr);
            ReleaseRef(spark);
        }
        sparks_.clear();
        if (dust_) {
            host->RemoveProp(dust_->AsOccupant());
            ReleaseRef(dust_);
        }
        for (CrewData& crew : crews_) {
            if (host) host->RemoveAutomaton(crew.automaton);
            ReleaseRef(crew.automaton);
        }
        crews_.clear();
    } else {
        textures_.clear();
        ReleaseRef(crane_);
    }
    return true;
}

// ==== 0x1001783d6 ==============================================================================================
void cSC4ConstructionOccupant::SendConstructionMessage(std::uint32_t messageType, bool flag) {
    if (!g_host) return;
    cIGZMessageServer2* messages = g_host->MessageServer();
    if (!messages) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(messageType);
    message->SetIGZUnknown(static_cast<cIGZUnknown*>(AsOccupant()));
    message->SetData1(flag ? 1 : 0);
    messages->MessagePost(message, false);
    message->Release();
}

// ==== 0x1001785ac ==============================================================================================
bool cSC4ConstructionOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(kConstructionRecordVersion);
    if (!cSC4Occupant::Write(stream)) return !stream.GetError();
    WriteSerializable(stream, const_cast<IConstructionLot*>(lot_));
    WriteSerializable(stream, building_);
    stream << transform_;
    stream << bounds_;
    stream << roofBounds_;
    stream << static_cast<bool>(crane_ != nullptr);
    stream << cS3DVector3{cranePosition_[0], cranePosition_[1], cranePosition_[2]};
    stream.SetUint32(static_cast<std::uint32_t>(groundProps_.size()));
    for (IConstructionProp* prop : groundProps_) WriteSerializable(stream, prop);
    stream.SetUint32(static_cast<std::uint32_t>(roofProps_.size()));
    for (IConstructionProp* prop : roofProps_) WriteSerializable(stream, prop);
    stream.SetUint32(static_cast<std::uint32_t>(sparks_.size()));
    for (IConstructionEffect* spark : sparks_) WriteSerializable(stream, spark);
    WriteSerializable(stream, dust_);
    stream.SetSint8(static_cast<std::int8_t>(stage_));
    stream.SetUint32(stageStartDay_);
    stream.SetUint32(saved208_);
    stream.SetUint32(nextStageDay_);
    stream.SetUint32(segmentCount_);
    stream.SetUint32(currentSegment_);
    stream.SetFloat32(segmentsHeight_);
    stream.SetFloat32(buildingBaseY_);
    stream.SetUint32(firstStageDuration_);
    stream.SetUint32(segmentDuration_);
    stream.SetUint32(scaffoldRemovalDuration_);
    stream.SetUint32(interiorDuration_);
    stream.SetUint32(animTime_);
    stream.SetSint8(static_cast<std::int8_t>(craneState_));
    stream.SetFloat32(craneYawMin_);
    stream.SetFloat32(craneYawMax_);
    stream << saved240_;
    stream.SetUint32(craneRotTime_);
    stream.SetUint32(craneRotDuration_);
    stream.SetFloat32(craneYawTarget_);
    stream.SetFloat32(craneYawStart_);
    stream.SetUint8(craneType_);
    stream.SetUint32(static_cast<std::uint32_t>(crews_.size()));
    for (const CrewData& crew : crews_) {
        stream << crew.first;
        stream << crew.onRoof;
        WriteSerializable(stream, crew.automaton);
    }
    return !stream.GetError();
}

// ==== 0x100178918 ==============================================================================================
bool cSC4ConstructionOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < 4) return false;
    needsRebuild_ = true;
    if (!cSC4Occupant::Read(stream)) return !stream.GetError();
    ReleaseRef(lot_);
    // The original asks the read object for cISC4Lot; the port's lot is a plain class behind a serializable adapter, so the
    // host turns whatever came in into the lot interface (one reference).
    cIGZUnknown* lotRaw = nullptr;
    ReadSerializable(stream, kGZIID_cIGZUnknown, &lotRaw);
    if (lotRaw) {
        lot_ = g_host ? g_host->AdoptLot(lotRaw) : nullptr;
        lotRaw->Release();
    }
    ReleaseRef(building_);
    cIGZUnknown* buildingRaw = nullptr;
    ReadSerializable(stream, kGZIID_cISC4BuildingOccupant, &buildingRaw);
    if (buildingRaw) building_ = static_cast<cISC4BuildingOccupant*>(buildingRaw);
    if (version < kConstructionLegacyTransformVersion) {
        LoadLegacyS3DTransform(stream, transform_);
    } else {
        stream >> transform_;
    }
    stream >> bounds_;
    stream >> roofBounds_;
    stream >> hasCrane_;
    cS3DVector3 cranePosition;
    stream >> cranePosition;
    cranePosition_[0] = cranePosition.x;
    cranePosition_[1] = cranePosition.y;
    cranePosition_[2] = cranePosition.z;
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        cIGZUnknown* raw = nullptr;
        ReadSerializable(stream, kGZIID_cISC4PropOccupant, &raw);
        if (raw) groundProps_.push_back(static_cast<IConstructionProp*>(raw));
    }
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        cIGZUnknown* raw = nullptr;
        ReadSerializable(stream, kGZIID_cISC4PropOccupant, &raw);
        if (raw) roofProps_.push_back(static_cast<IConstructionProp*>(raw));
    }
    stream.GetUint32(count);
    sparks_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        cIGZUnknown* raw = nullptr;
        ReadSerializable(stream, kGZIID_cISC4PropOccupantVisualEffect, &raw);
        sparks_.push_back(static_cast<IConstructionEffect*>(raw));
    }
    cIGZUnknown* dustRaw = nullptr;
    ReadSerializable(stream, kGZIID_cISC4PropOccupantVisualEffect, &dustRaw);
    dust_ = static_cast<IConstructionEffect*>(dustRaw);
    std::int8_t stage = 0;
    stream.GetSint8(stage);
    stage_ = static_cast<std::uint32_t>(static_cast<std::int32_t>(stage));
    stream.GetUint32(stageStartDay_);
    stream.GetUint32(saved208_);
    stream.GetUint32(nextStageDay_);
    stream.GetUint32(segmentCount_);
    stream.GetUint32(currentSegment_);
    stream.GetFloat32(segmentsHeight_);
    stream.GetFloat32(buildingBaseY_);
    stream.GetUint32(firstStageDuration_);
    stream.GetUint32(segmentDuration_);
    stream.GetUint32(scaffoldRemovalDuration_);
    stream.GetUint32(interiorDuration_);
    stream.GetUint32(animTime_);
    std::int8_t craneState = 0;
    stream.GetSint8(craneState);
    craneState_ = static_cast<std::uint32_t>(static_cast<std::int32_t>(craneState));
    stream.GetFloat32(craneYawMin_);
    stream.GetFloat32(craneYawMax_);
    stream >> saved240_;
    stream.GetUint32(craneRotTime_);
    stream.GetUint32(craneRotDuration_);
    stream.GetFloat32(craneYawTarget_);
    stream.GetFloat32(craneYawStart_);
    stream.GetUint8(craneType_);
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        CrewData crew;
        stream >> crew.first;
        stream >> crew.onRoof;
        cIGZUnknown* raw = nullptr;
        ReadSerializable(stream, kGZIID_cISC4Automaton, &raw);
        crew.automaton = static_cast<IConstructionAutomaton*>(raw);
        crews_.push_back(crew);
    }
    return !stream.GetError();
}

// ==== 0x100178ea2 ==============================================================================================
bool cSC4ConstructionOccupant::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    if (type == kMessageAnimationTick) {
        DoAnimationTick(static_cast<std::uint32_t>(standard->GetData2() / 1000));
    } else if (type == kMessageViewZoomRotationDeferred) {
        const auto* oldZoom = static_cast<const int*>(standard->GetVoid1());
        const auto* newZoom = static_cast<const int*>(standard->GetVoid2());
        // Guard: the original compares both payloads without checking them (0x100178fxx).
        if (oldZoom && newZoom && oldZoom[1] != newZoom[1]) rotationChanged_ = true;
    } else if (type == kMessageViewFilterChange) {
        // The payload is cSC4ViewFilterInformation, whose first field is the occupant filter.
        auto* filter = static_cast<cISC4OccupantFilter**>(standard->GetVoid2());
        cISC4OccupantFilter* occupantFilter = filter ? *filter : nullptr;
        const bool included = occupantFilter && occupantFilter->IsOccupantIncluded(AsOccupant());
        const bool flagged = IsFlagSet(kOccupantFlagInRenderer);
        IConstructionRenderer* renderer = g_host ? g_host->Renderer() : nullptr;
        if (!included) {
            if (flagged) {
                ClearFlag(kOccupantFlagInRenderer);
                if (renderer && renderer->IsModelInserted(this)) renderer->RemoveModel(this);
            }
        } else if (!flagged) {
            SetFlag(kOccupantFlagInRenderer);
            if (renderer && !renderer->IsModelInserted(this)) renderer->InsertModel(this);
        }
    } else if (type == kMessageCityPostInit) {
        if (needsRebuild_) {
            RebuildModel();
            if (crane_) CreateCrane(cranePosition_[0], cranePosition_[2], craneType_);
            AddToRenderer();
            if (stage_ == 2) {
                stage_ = 1;
                DoAnimationTick(0);
                stage_ = 2;
            } else {
                DoAnimationTick(0);
            }
            needsRebuild_ = false;
        }
    } else if (type == kMessageDayTick) {
        DoDailySim();
    }
    return true;
}

// ==== 0x10017909a ==============================================================================================
void cSC4ConstructionOccupant::DoDailySim() {
    if (!building_ || stage_ == 4 || !g_host) return;
    const std::uint32_t today = g_host->SimulatorDay();
    if (nextStageDay_ > today) return;
    animTime_ = 0;
    switch (stage_) {
        case 0:
        case 2: {
            const std::uint32_t done = currentSegment_ + 1;
            currentSegment_ = done;
            if (segmentCount_ < done) {
                stage_ = 3;
                nextStageDay_ = today + scaffoldRemovalDuration_;
                ClearPropList(roofProps_);
                if (dust_) dust_->StopEffect(true);
            } else {
                SendConstructionMessage(kMessageConstructionStage, true);
                stage_ = 1;
                nextStageDay_ = today + segmentDuration_;
            }
            for (IConstructionEffect* spark : sparks_) {
                if (spark) spark->StopEffect(true);
            }
            break;
        }
        case 1:
            stage_ = 2;
            nextStageDay_ = today + interiorDuration_;
            RepositionSparks();
            return;
        case 3: {
            stage_ = 4;
            SetBuildingCompletion(SC4Percentage(1.0f));
            if (IsFlagSet(kOccupantFlagInRenderer)) {
                if (IConstructionRenderer* renderer = g_host->Renderer()) renderer->RemoveModel(this);
                ClearFlag(kOccupantFlagInRenderer);
            }
            g_host->RemoveOccupant(AsOccupant(), false, 0);
            if (lot_) lot_->SetState(kLotStateLivable, 0, 0);
            Shutdown();
            break;
        }
        default:
            break;
    }
}

// ==== 0x100179308 ==============================================================================================
void cSC4ConstructionOccupant::DoAnimationTick(std::uint32_t elapsed) {
    const std::uint32_t previous = animTime_;
    animTime_ = previous + elapsed;
    if (stage_ >= 4) return;
    const float time = static_cast<float>(animTime_) * 0.002f;
    float base = 0.0f;
    float target = 0.0f;
    switch (stage_) {
        case 0: {
            const float fraction = time < static_cast<float>(firstStageDuration_)
                                       ? time / static_cast<float>(firstStageDuration_)
                                       : 1.0f;
            base = bounds_.min[1];
            target = (buildingBaseY_ - base) * fraction;
            break;
        }
        case 1:
            base = static_cast<float>(currentSegment_ - 1) * sfSegmentHeight + buildingBaseY_;
            target = std::min(1.0f, time / static_cast<float>(segmentDuration_)) * sfSegmentHeight;
            break;
        case 2: {
            const float fraction = std::min(1.0f, time / static_cast<float>(interiorDuration_));
            const float perSegment = 1.0f / static_cast<float>(segmentCount_);
            const SC4Percentage completion(fraction * perSegment +
                                           static_cast<float>(currentSegment_ - 1) * perSegment);
            SetBuildingCompletion(completion);
            if (crane_) AnimateCrane(elapsed);
            return;
        }
        case 3: {
            const float fraction = std::min(1.0f, time / static_cast<float>(scaffoldRemovalDuration_));
            SetScaffoldHeight((1.0f - fraction) * ((buildingBaseY_ - bounds_.min[1]) + segmentsHeight_) +
                              bounds_.min[1]);
            return;
        }
        default:
            return;
    }
    SetScaffoldHeight(target + base);
    if (crane_) AnimateCrane(elapsed);
}

// ==== 0x1001794e4 ==============================================================================================
void cSC4ConstructionOccupant::RebuildModel() {
    if (!building_ || !g_host) return;
    if (sides_[0].size() != 4) {
        for (auto& side : sides_) side.assign(4, ScaffoldVertex{});
    }
    if (planks_.size() != 16) planks_.assign(16, ScaffoldVertex{});
    if (girders_.size() != 4) girders_.assign(4, ScaffoldVertex{});
    if (posts_.size() != 80) posts_.assign(80, PostVertex{});

    cISC4Occupant* occupant = building_->AsOccupant();
    if (!occupant) return;
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    occupant->GetBoundingBox(min, max);

    // +0x21c: the building's base, min Y + 0.1, computed in double like the original.
    const float baseY = static_cast<float>(static_cast<double>(min[1]) + 0.1);
    buildingBaseY_ = baseY;

    // The four corner altitudes; the lowest one is the ground the scaffold stands on.
    const float a0 = g_host->TerrainAltitude(min[0], min[2]);
    const float a1 = g_host->TerrainAltitude(max[0], min[2]);
    const float a2 = g_host->TerrainAltitude(min[0], max[2]);
    const float a3 = g_host->TerrainAltitude(max[0], max[2]);
    float ground = std::min(a0, a1);
    ground = std::min(ground, a2);
    ground = std::min(ground, a3);
    min[1] = ground;

    // The footprint grows by the scaffold gap on every side.
    const float gap = sfScaffoldGap;
    min[0] -= gap;
    min[2] -= gap;
    max[0] += gap;
    max[2] += gap;
    const float segmentHeight = sfSegmentHeight;
    // segments = floor((maxY - base) / segmentHeight) + 1 (the original's round-trick at 0x100179760).
    const float segments = (max[1] - baseY) / segmentHeight;
    // The original's round-trick (0x100179754..0x100179792) computes floor(segments) with a tie fix; std::floor is
    // the same value for every finite input.
    int count = std::isnan(segments) ? -1 : static_cast<int>(std::floor(segments));
    std::uint32_t segmentCount = static_cast<std::uint32_t>(count + 1);
    if (segmentCount == 0) segmentCount = 1;
    segmentCount_ = segmentCount;
    segmentsHeight_ = static_cast<float>(segmentCount) * segmentHeight;

    // Corner 0 (minX, ground, minZ), 1 (maxX, ground, minZ), 2 (minX, ground, maxZ), 3 (maxX, ground, maxZ).
    float corner[4][3] = {{min[0], ground, min[2]},
                          {max[0], ground, min[2]},
                          {min[0], ground, max[2]},
                          {max[0], ground, max[2]}};

    // The four side quads: the bottom vertices sit on the ground, the top ones at the building's max Y.
    for (int side = 0; side < 4; ++side) {
        for (int vertex = 0; vertex < 4; ++vertex) {
            const int cornerIndex = kVertexToCorner[side * 4 + vertex];
            ScaffoldVertex& out = sides_[side][static_cast<std::size_t>(vertex)];
            out.x = corner[cornerIndex][0];
            out.z = corner[cornerIndex][2];
            out.y = (vertex == 0 || vertex == 3) ? ground : max[1];
            out.u = 0.0f;
            out.v = 0.0f;
        }
    }

    // The planks: one horizontal strip along each side at max Y, sfPlankWidth wide, with texture coordinates.
    const float plankWidth = sfPlankWidth;
    for (int side = 0; side < 4; ++side) {
        const float* c0 = corner[kVertexToCorner[side * 4]];        // the side's first corner
        const float* c1 = corner[kVertexToCorner[side * 4 + 3]];    // the side's second corner
        float p[4][3];
        switch (side) {
            case 0:
                p[0][0] = c0[0]; p[0][1] = max[1]; p[0][2] = c0[2] + plankWidth;
                p[1][0] = c0[0]; p[1][1] = max[1]; p[1][2] = c0[2];
                p[2][0] = c1[0]; p[2][1] = max[1]; p[2][2] = c1[2];
                p[3][0] = c1[0]; p[3][1] = max[1]; p[3][2] = c1[2] + plankWidth;
                break;
            case 1:
                p[0][0] = c1[0] - plankWidth; p[0][1] = max[1]; p[0][2] = c1[2] - plankWidth;
                p[1][0] = c0[0] - plankWidth; p[1][1] = max[1]; p[1][2] = c0[2] + plankWidth;
                p[2][0] = c0[0]; p[2][1] = max[1]; p[2][2] = c0[2] + plankWidth;
                p[3][0] = c1[0]; p[3][1] = max[1]; p[3][2] = c1[2] - plankWidth;
                break;
            case 2:
                p[0][0] = c0[0]; p[0][1] = max[1]; p[0][2] = c0[2];
                p[1][0] = c0[0]; p[1][1] = max[1]; p[1][2] = c0[2] - plankWidth;
                p[2][0] = c1[0]; p[2][1] = max[1]; p[2][2] = c1[2] - plankWidth;
                p[3][0] = c1[0]; p[3][1] = max[1]; p[3][2] = c1[2];
                break;
            default:
                p[0][0] = c1[0] - plankWidth; p[0][1] = max[1]; p[0][2] = c1[2] - plankWidth;
                p[1][0] = c0[0]; p[1][1] = max[1]; p[1][2] = c0[2] + plankWidth;
                p[2][0] = c0[0] + plankWidth; p[2][1] = max[1]; p[2][2] = c0[2] + plankWidth;
                p[3][0] = c1[0] + plankWidth; p[3][1] = max[1]; p[3][2] = c1[2] - plankWidth;
                break;
        }
        for (int vertex = 0; vertex < 4; ++vertex) {
            ScaffoldVertex& out = planks_[static_cast<std::size_t>(side * 4 + vertex)];
            out.x = p[vertex][0];
            out.y = p[vertex][1];
            out.z = p[vertex][2];
            out.u = p[vertex][0] * sfPlankTextureScale;
            out.v = p[vertex][2] * sfPlankTextureScale;
        }
    }

    // The girders: one quad at max Y inset by the plank width.
    const float insetX[4] = {plankWidth, plankWidth, -plankWidth, -plankWidth};
    const float insetZ[4] = {-plankWidth, plankWidth, plankWidth, -plankWidth};
    for (int vertex = 0; vertex < 4; ++vertex) {
        const int cornerIndex = kGirderVertexToCorner[vertex];
        ScaffoldVertex& out = girders_[static_cast<std::size_t>(vertex)];
        out.x = corner[cornerIndex][0] + insetX[vertex];
        out.y = max[1];
        out.z = corner[cornerIndex][2] + insetZ[vertex];
        out.u = 0.0f;
        out.v = 0.0f;
    }

    // The four corner posts: a 1 x 1 x (maxY + postWidth) box on each corner, five quads each.
    const float postTop = sfPostWidth + max[1];
    for (int post = 0; post < 4; ++post) {
        const float cx = corner[post][0];
        const float cz = corner[post][2];
        const float px[4] = {cx - 0.5f, cx + 0.5f, cx - 0.5f, cx + 0.5f};
        const float pz[4] = {cz - 0.5f, cz - 0.5f, cz + 0.5f, cz + 0.5f};
        for (int side = 0; side < 4; ++side) {
            for (int vertex = 0; vertex < 4; ++vertex) {
                const int cornerIndex = kVertexToCorner[side * 4 + vertex];
                PostVertex& out = posts_[static_cast<std::size_t>(post * 20 + side * 4 + vertex)];
                out.x = px[cornerIndex];
                out.z = pz[cornerIndex];
                out.y = (vertex == 0 || vertex == 3) ? ground : postTop;
                out.colour = 0xff000000u;   // the alpha byte; DoRotationChange fills r/g/b
            }
        }
        for (int vertex = 0; vertex < 4; ++vertex) {
            const int cornerIndex = kGirderVertexToCorner[vertex];
            PostVertex& out = posts_[static_cast<std::size_t>(post * 20 + 16 + vertex)];
            out.x = px[cornerIndex];
            out.z = pz[cornerIndex];
            out.y = postTop;
            out.colour = 0xff000000u;
        }
    }

    rotationChanged_ = true;
    roofBounds_.min[0] = min[0];
    roofBounds_.min[1] = ground;
    roofBounds_.min[2] = min[2];
    roofBounds_.max[0] = max[0];
    roofBounds_.max[1] = max[1];
    roofBounds_.max[2] = max[2];
    bounds_.min[0] = min[0] - sfPostWidth;
    bounds_.min[1] = ground;
    bounds_.min[2] = min[2] - sfPostWidth;
    bounds_.max[0] = max[0] + sfPostWidth;
    bounds_.max[1] = max[1];
    bounds_.max[2] = max[2] + sfPostWidth;
}

// ==== 0x100179e50 ==============================================================================================
// PARTIAL: the crane model instance, its position / yaw and its reference are ported; the 16-vertex base plate at
// +0x188 is rebuilt from the documented intent because the eight-entry corner table it reads lives in .bss and is
// only partly initialised by the original's guard (0x100179f4a..0x100179f9e).
void cSC4ConstructionOccupant::CreateCrane(float x, float z, int modelIndex) {
    if (!g_host || modelIndex < 0 || modelIndex > 2) return;
    const cGZPersistResourceKey key{kCraneModelType, kCraneModelGroup, kCraneModelId[modelIndex]};
    s3d::cIS3DModelInstance* model = g_host->CreateModelInstance(key, kResourceTypeS3DModel);
    if (model) {
        crane_ = model;
        cranePosition_[0] = x;
        cranePosition_[1] = 0.0f;
        cranePosition_[2] = z;
        cS3DTransform transform;
        model->GetTransform(transform);
        // The initial yaw: -yawMin * 0.5 + yawMax + yawMin (0x100179ecb).
        SetCraneYaw(-craneYawMin_ * 0.5f + craneYawMax_ + craneYawMin_);
        transform.translation[0] = x;
        transform.translation[2] = z;
        transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
        ++transform.changeCounter;
        model->SetTransform(transform);
        model->SetOwner(AsOccupant());
    }
    CreateCranePosts(x, z);
}

// The crane's base plate: four 0.1 m thick quads around (x, z), coloured 0x80 grey.
void cSC4ConstructionOccupant::CreateCranePosts(float x, float z) {
    if (cranePosts_.size() != 16) cranePosts_.assign(16, PostVertex{});
    const float half = sfCranePostWidth * 0.5f;
    const float corners[4][2] = {{-half, half}, {half, -half}, {half, half}, {-half, -half}};
    const float ground = bounds_.min[1];
    for (int quad = 0; quad < 4; ++quad) {
        const float* c = corners[quad];
        const float px[4] = {x + c[0], x + corners[(quad + 1) & 3][0], x + corners[(quad + 1) & 3][0], x + c[0]};
        const float pz[4] = {z + c[1], z + corners[(quad + 1) & 3][1], z + corners[(quad + 1) & 3][1], z + c[1]};
        for (int vertex = 0; vertex < 4; ++vertex) {
            PostVertex& out = cranePosts_[static_cast<std::size_t>(quad * 4 + vertex)];
            out.x = px[vertex];
            out.z = pz[vertex];
            out.y = (vertex < 2) ? ground : ground + 0.1f;
            out.colour = 0xff808080u;
        }
    }
}

// ==== 0x10017a218 ==============================================================================================
void cSC4ConstructionOccupant::AddToRenderer() {
    if (!g_host) return;
    IConstructionRenderer* renderer = g_host->Renderer();
    if (!renderer) return;
    cISC4OccupantFilter* filter = g_host->ViewFilter();
    bool included = true;
    if (filter) included = filter->IsOccupantIncluded(AsOccupant());
    if (!included) {
        if (IsFlagSet(kOccupantFlagInRenderer)) {
            ClearFlag(kOccupantFlagInRenderer);
            if (renderer->IsModelInserted(this)) renderer->RemoveModel(this);
        }
        return;
    }
    SetFlag(kOccupantFlagInRenderer);
    if (!renderer->IsModelInserted(this)) renderer->InsertModel(this);
}

// ==== 0x10017a342 ==============================================================================================
// The original draws one Gaussian coordinate for the face's two free axes from the roof box, plus the Y coordinate
// (0x10017a342); the four cases pick the face (minX / maxX / minZ / maxZ) with a shuffled 0..3 iterator.
void cSC4ConstructionOccupant::RepositionSparks() {
    if (sparks_.empty() || !g_host) return;
    cRZRandom* random = g_host->Random();
    if (!random) return;
    LinearRandomIterator sideIterator(4);
    sideIterator.Reset(true);
    for (IConstructionEffect* spark : sparks_) {
        std::uint32_t side = 0;
        if (!sideIterator.Next(side)) {
            sideIterator.Reset(true);
            if (!sideIterator.Next(side)) side = 0;
        }
        const float gaussianX = static_cast<float>(random->RandomDoubleRangeGaussianFast(roofBounds_.min[0],
                                                                                         roofBounds_.max[0]));
        const float gaussianZ = static_cast<float>(random->RandomDoubleRangeGaussianFast(roofBounds_.min[2],
                                                                                         roofBounds_.max[2]));
        const float gaussianY = static_cast<float>(random->RandomDoubleRangeGaussianFast(roofBounds_.min[1],
                                                                                         roofBounds_.max[1]));
        float position[3] = {0, 0, 0};
        switch (side) {
            case 0:
                position[0] = gaussianX;
                position[1] = gaussianY;
                position[2] = roofBounds_.min[2];
                break;
            case 1:
                position[0] = roofBounds_.max[0];
                position[1] = gaussianY;
                position[2] = gaussianZ;
                break;
            case 2:
                position[0] = gaussianX;
                position[1] = gaussianY;
                position[2] = roofBounds_.max[2];
                break;
            default:
                position[0] = roofBounds_.min[0];
                position[1] = gaussianY;
                position[2] = gaussianZ;
                break;
        }
        spark->SetPosition(position);
        spark->StartEffect(true);
    }
}

// ==== 0x10017a63c ==============================================================================================
void cSC4ConstructionOccupant::SetBuildingCompletion(const SC4Percentage& completion) {
    if (!building_) return;
    const float current = building_->GetCompletionPercent();
    const bool changed = (current != completion.value) || std::isnan(current) || std::isnan(completion.value);
    if (!changed) return;
    building_->SetCompletionPercent(completion.value);
    if (!g_host) return;
    cIGZMessageServer2* messages = g_host->MessageServer();
    if (!messages) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMessageBuildingCompletionChanged);
    message->SetVoid1(static_cast<void*>(building_));
    message->SetIGZUnknown(static_cast<cIGZUnknown*>(building_));
    messages->MessagePost(message, false);
    message->Release();
}

// ==== 0x10017a710 ==============================================================================================
int cSC4ConstructionOccupant::SetScaffoldHeight(float height) {
    scaffoldHeight_ = height;
    roofBounds_.max[1] = height;
    for (auto& side : sides_) {
        if (side.size() != 4) continue;
        side[1].y = height;
        side[2].y = height;
    }
    for (ScaffoldVertex& vertex : planks_) vertex.y = height;
    for (ScaffoldVertex& vertex : girders_) vertex.y = height;
    const float postTop = sfPostWidth + height;
    for (std::size_t post = 0; post < 4 && posts_.size() >= (post + 1) * 20; ++post) {
        for (std::size_t vertex : {std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{6},
                                   std::size_t{9}, std::size_t{10}, std::size_t{13}, std::size_t{14},
                                   std::size_t{16}, std::size_t{17}, std::size_t{18}, std::size_t{19}}) {
            posts_[post * 20 + vertex].y = postTop;
        }
    }
    if (crane_ && g_host) {
        cranePosition_[1] = sfCraneHeightAboveScaffold + height;
        cS3DTransform transform;
        crane_->GetTransform(transform);
        transform.translation[0] = cranePosition_[0];
        transform.translation[1] = cranePosition_[1];
        transform.translation[2] = cranePosition_[2];
        transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
        ++transform.changeCounter;
        crane_->SetTransform(transform);
    }
    for (PostVertex& vertex : cranePosts_) {
        // The block layout the original walks (four 0x40 blocks, the y at +0x14 / +0x24 of each) is the same as
        // "every vertex except the first and the fourth of each block" in the port's array.
        const std::size_t index = static_cast<std::size_t>(&vertex - cranePosts_.data());
        const std::size_t within = index & 3;
        if (within == 1 || within == 2) vertex.y = cranePosition_[1];
    }
    if (g_host) {
        for (const CrewData& crew : crews_) {
            if (!crew.onRoof || !crew.automaton) continue;
            cISC4Occupant* occupant = crew.automaton->AsOccupant();
            if (!occupant) continue;
            float position[3] = {0, 0, 0};
            occupant->GetPosition(position);
            position[1] = height;
            occupant->SetPosition(position);
            g_host->MoveOccupant(occupant, false);
            occupant->PostOccupantMessage(kMessageOccupantRemoved, 0);
        }
    }
    return 1;
}

// ==== 0x10017aa36 ==============================================================================================
void cSC4ConstructionOccupant::AnimateCrane(std::uint32_t elapsed) {
    const std::uint32_t time = elapsed + craneRotTime_;
    craneRotTime_ = time;
    if (time < craneRotDuration_) {
        if (craneState_ != 0) return;
        const float fraction = static_cast<float>(time) / static_cast<float>(craneRotDuration_);
        SetCraneYaw((craneYawTarget_ - craneYawStart_) * (3.0f - (fraction + fraction)) * fraction * fraction +
                    craneYawStart_);
        return;
    }
    craneRotTime_ = 0;
    std::uint32_t duration = 0;
    if (craneState_ == 0) {
        craneState_ = 1;
        duration = static_cast<std::uint32_t>(craneRandomBelow(1000) + 1000);
    } else if (craneState_ == 1) {
        craneState_ = 2;
        return;
    } else if (craneState_ == 2) {
        craneState_ = 0;
        const float low = craneYawMin_;
        const float middle = (craneYawMax_ - low) * 0.5f + low;
        const float start = craneYawTarget_;
        craneYawStart_ = start;
        const float lower = (middle <= start) ? low : middle;
        const float upper = (start < middle) ? craneYawMax_ : middle;
        float target = lower;
        if (g_host) {
            if (cRZRandom* random = g_host->Random()) {
                target = static_cast<float>(random->RandomDoubleRangeUniform(lower, upper));
            }
        }
        craneYawTarget_ = target;
        duration = static_cast<std::uint32_t>(
            RoundNearest(std::fabs(craneYawTarget_ - craneYawStart_) * sfCraneTimeOfFullRotation / kTwoPi));
        if (duration == 0) duration = 1;   // guard: a zero divisor would break the tick
    } else {
        return;
    }
    craneRotDuration_ = duration;
}

// ==== 0x10017abe0 ==============================================================================================
void cSC4ConstructionOccupant::SetCraneYaw(float yaw) {
    if (!crane_) return;
    float angle = yaw + kPi;
    while (angle > kTwoPi) angle -= kTwoPi;
    cS3DTransform transform;
    crane_->GetTransform(transform);
    const float half = angle * -0.5f;
    const float sine = std::sin(half);
    const float cosine = std::cos(half);
    // The original builds the quaternion (0, sin, 0, cos) and its matrix (0x10017acxx); the result is a rotation
    // about Y by `angle`, which is what this writes.
    const float s2 = (sine * cosine) + (sine * cosine);
    const float c2 = 1.0f - (sine * sine + sine * sine);
    transform.rotation[0][0] = c2;
    transform.rotation[0][1] = 0.0f;
    transform.rotation[0][2] = -s2;
    transform.rotation[1][0] = 0.0f;
    transform.rotation[1][1] = 1.0f;
    transform.rotation[1][2] = 0.0f;
    transform.rotation[2][0] = s2;
    transform.rotation[2][1] = 0.0f;
    transform.rotation[2][2] = c2;
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformRotation);
    ++transform.changeCounter;
    crane_->SetTransform(transform);
}

// ==== 0x10017ad78 ==============================================================================================
void cSC4ConstructionOccupant::SetLot(IConstructionLot* lot) {
    if (!IsInitialized()) Init();
    if (lot_ != lot) {
        if (lot) lot->AddRef();
        IConstructionLot* previous = lot_;
        lot_ = lot;
        if (previous) previous->Release();
    }
    ReleaseRef(building_);
    if (!lot_) {
        AddToRenderer();
        return;
    }
    cISC4BuildingOccupant* building = lot_->GetBuilding();
    if (!building) {
        // Quirk kept: the original detaches and releases the lot and returns without AddToRenderer (0x10017b0xx).
        ReleaseRef(lot_);
        return;
    }
    building->AddRef();
    building_ = building;
    RebuildModel();
    cS3DBoundingBox boxes[12];
    for (auto& box : boxes) {
        box.min[0] = box.min[1] = box.min[2] = 1.0f;
        box.max[0] = box.max[1] = box.max[2] = 0.0f;
    }
    DivideLot(boxes);
    AttemptAddCrane(boxes);
    AddGroundProps(boxes);
    cS3DBoundingBox roof = roofBounds_;
    CreateConstructionCrew(boxes, roof);
    AddRoofProps(roof);
    CreateDustClouds();
    CreateSparks();
    CreateLotTexture();
    if (!g_host) return;
    stageStartDay_ = g_host->SimulatorDay();
    const BuildingProfile& profile = building_->GetBuildingProfile();
    std::uint32_t growthStage = profile.growthStage;
    if (growthStage == 0) growthStage = 10;
    const float rate = sfScaffoldGrowthRate;
    int first = RoundNearest((buildingBaseY_ - bounds_.min[1]) * rate);
    if (1 < first) first = 1;
    firstStageDuration_ = static_cast<std::uint32_t>(first);
    int segment = RoundNearest(rate * sfSegmentHeight);
    if (1 < segment) segment = 1;
    segmentDuration_ = static_cast<std::uint32_t>(segment);
    int removal = RoundNearest((bounds_.max[1] - bounds_.min[1]) * sfScaffoldRemovalRate);
    if (1 < removal) removal = 1;
    scaffoldRemovalDuration_ = static_cast<std::uint32_t>(removal);
    const std::uint32_t count = segmentCount_;
    std::uint32_t interior = (growthStage - static_cast<std::uint32_t>(first)) -
                             static_cast<std::uint32_t>(segment) * count - static_cast<std::uint32_t>(removal);
    if (static_cast<std::int32_t>(interior) < static_cast<std::int32_t>(count)) interior = count;
    interiorDuration_ = interior / count;
    stage_ = 0;
    nextStageDay_ = static_cast<std::uint32_t>(first) + stageStartDay_;
    currentSegment_ = 0;
    animTime_ = 0;
    SetBuildingCompletion(SC4Percentage(0.0f));
    SetScaffoldHeight(bounds_.min[1] + 0.001f);
    if (crane_) {
        craneRotTime_ = 1;
        craneRotDuration_ = 0;
        craneState_ = 2;
        craneYawTarget_ = craneYawMin_;
        craneYawStart_ = craneYawMax_;
        SetCraneYaw(craneYawMin_);
    }
    SendConstructionMessage(kMessageConstructionStage, true);
    SendConstructionMessage(kMessageConstructionComplete, true);
    AddToRenderer();
}

// ==== 0x10017b102 ==============================================================================================
void cSC4ConstructionOccupant::DivideLot(cS3DBoundingBox* boxes) {
    if (!lot_ || !building_ || !g_host) return;
    std::int32_t rect[4] = {0, 0, 0, 0};
    lot_->GetCellRect(rect);
    float lotMinX = 0, lotMinZ = 0, lotMaxX = 0, lotMaxZ = 0;
    g_host->CityCellToPosition(rect[0], rect[1], lotMinX, lotMinZ);
    g_host->CityCellToPosition(rect[2] + 1, rect[3] + 1, lotMaxX, lotMaxZ);
    float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};
    if (cISC4Occupant* occupant = building_->AsOccupant()) occupant->GetBoundingBox(min, max);
    const float gap = sfScaffoldGap;
    const float x0 = std::max(lotMinX, min[0] - gap);
    const float x1 = std::min(lotMaxX, max[0] + gap);
    const float z0 = std::max(lotMinZ, min[2] - gap);
    const float z1 = std::min(lotMaxZ, max[2] + gap);
    auto set = [](cS3DBoundingBox& box, float ax, float az, float bx, float bz) {
        box.min[0] = ax;
        box.min[1] = 0.0f;
        box.min[2] = az;
        box.max[0] = bx;
        box.max[1] = 1.0f;
        box.max[2] = bz;
    };
    set(boxes[0], x0, lotMinZ, x1, z0);
    set(boxes[1], x1, z0, lotMaxX, z1);
    set(boxes[2], x0, z1, x1, lotMaxZ);
    set(boxes[3], lotMinX, z0, x0, z1);
    set(boxes[4], lotMinX, lotMinZ, x0, z0);
    set(boxes[5], x1, lotMinZ, lotMaxX, z0);
    set(boxes[6], x1, z1, lotMaxX, lotMaxZ);
    set(boxes[7], lotMinX, z1, x0, lotMaxZ);
}

// ==== 0x10017b33c ==============================================================================================
int cSC4ConstructionOccupant::AttemptAddCrane(cS3DBoundingBox* boxes) {
    if (!building_) return -1;
    std::uint8_t type = 0;
    if (cISC4Occupant* occupant = building_->AsOccupant()) {
        if (cISCPropertyHolder* holder = occupant->AsPropertyHolder()) {
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyConstructionCraneType, type);
        }
    }
    if (type == 3) {
        const int result = AttemptAddOuterCrane(boxes);
        if (result != -1) return result;
    } else if (type != 2) {
        if (type != 1) return -1;
        return AttemptAddOuterCrane(boxes);   // quirk kept: a plain outer crane does not fall back to the inner one
    }
    AttemptAddInnerCrane();
    return -1;
}

// ==== 0x10017b3c2 ==============================================================================================
void cSC4ConstructionOccupant::AddGroundProps(cS3DBoundingBox* boxes) {
    if (sPropList.empty() || !g_host) return;
    cRZRandom* random = g_host->Random();
    if (!random) return;
    int remaining = random->RandomSint32RangeUniform(1, 4);
    if (remaining == 0) return;
    bool placedAny = false;
    do {
        placedAny = false;
        LinearRandomIterator boxIterator(8);
        boxIterator.Reset(true);
        std::uint32_t boxIndex = 0;
        while (boxIterator.Next(boxIndex)) {
            cS3DBoundingBox& box = boxes[boxIndex];
            const float width = box.max[0] - box.min[0];
            const float depth = box.max[2] - box.min[2];
            if (width <= 0.0f || depth <= 0.0f) continue;
            LinearRandomIterator propIterator(static_cast<int>(sPropList.size()));
            propIterator.Reset(true);
            std::uint32_t index = 0;
            bool placedInBox = false;
            while (propIterator.Next(index)) {
                const ConstructionPropEntry& entry = sPropList[index];
                if (!(width > entry.sizeX) || !(depth > entry.sizeZ)) continue;
                IConstructionProp* prop = nullptr;
                if (CreateAndPlaceProp(box, index, &prop, true) && prop) {
                    AddPropToList(groundProps_, prop);
                }
                --remaining;
                placedInBox = true;
            }
            placedAny = placedAny || placedInBox;
        }
    } while (placedAny && remaining != 0);
}

// ==== 0x10017b708 ==============================================================================================
void cSC4ConstructionOccupant::CreateConstructionCrew(cS3DBoundingBox* boxes, cS3DBoundingBox& roof) {
    if (!g_host || g_host->RenderDetailLevel() <= 2) return;
    cRZRandom* random = g_host->Random();
    if (!random) return;
    const std::uint32_t count = static_cast<std::uint32_t>(random->RandomSint32RangeUniform(2, 4));
    for (std::uint32_t i = 0; i < count; ++i) {
        LinearRandomIterator iterator(8);
        iterator.Reset(true);
        bool found = false;
        std::uint32_t boxIndex = 0;
        while (iterator.Next(boxIndex)) {
            if (boxes[boxIndex].max[2] - boxes[boxIndex].min[2] > 2.0f &&
                boxes[boxIndex].max[0] - boxes[boxIndex].min[0] > 2.0f) {
                found = true;
                break;
            }
        }
        if (!found) break;
        const std::uint32_t type = random->RandomUint32Uniform(3);
        CreateAutomata(boxes[boxIndex], false, kGroundAutomata[type]);
    }
    const int roofCount = random->RandomUint32Uniform(2);
    if (roofCount == -1) return;
    for (int i = 0; i < roofCount + 1; ++i) {
        if (roof.max[0] - roof.min[0] <= 2.0f) return;
        if (roof.max[2] - roof.min[2] <= 2.0f) return;
        const std::uint32_t type = random->RandomUint32Uniform(2);
        CreateAutomata(roof, true, kRoofAutomata[type]);
    }
}

// ==== 0x10017b964 ==============================================================================================
void cSC4ConstructionOccupant::AddRoofProps(cS3DBoundingBox& roof) {
    if (sPropList.empty() || !g_host) return;
    cRZRandom* random = g_host->Random();
    if (!random) return;
    int count = random->RandomSint32RangeUniform(0, 2);
    const float area = (roof.max[2] - roof.min[2]) * (roof.max[0] - roof.min[0]);
    if (area > 256.0f) count += (area <= 1024.0f) ? 1 : 2;
    if (count == 0) return;
    bool placedAny = false;
    do {
        placedAny = false;
        const float width = roof.max[0] - roof.min[0];
        const float depth = roof.max[2] - roof.min[2];
        LinearRandomIterator iterator(static_cast<int>(sPropList.size()));
        iterator.Reset(true);
        std::uint32_t index = 0;
        while (iterator.Next(index)) {
            const ConstructionPropEntry& entry = sPropList[index];
            if (!(width > entry.sizeX) || !(depth > entry.sizeZ)) continue;
            IConstructionProp* prop = nullptr;
            if (CreateAndPlaceProp(roof, index, &prop, false) && prop) AddPropToList(roofProps_, prop);
            --count;
            placedAny = true;
        }
    } while (count != 0 && placedAny);
}

// ==== 0x10017bb84 ==============================================================================================
void cSC4ConstructionOccupant::CreateDustClouds() {
    if (!g_host || !building_) return;
    IConstructionEffect* effect = g_host->CreatePropEffect();
    if (!effect) return;
    effect->Init();
    if (cISC4Occupant* occupant = building_->AsOccupant()) {
        float position[3] = {0, 0, 0};
        occupant->GetPosition(position);
        effect->SetPosition(position);
    }
    const float extent = std::max(roofBounds_.max[0] - roofBounds_.min[0],
                                  roofBounds_.max[2] - roofBounds_.min[2]);
    const char* name = extent < 10.0f ? "small_dust" : (extent < 20.0f ? "medium_dust" : "large_dust");
    effect->SetEffectType(name);
    g_host->AddLotProp(lot_, effect->AsOccupant());
    effect->StartEffect(true);
    dust_ = effect;
}

// ==== 0x10017bcee ==============================================================================================
void cSC4ConstructionOccupant::CreateSparks() {
    if (!g_host || !building_) return;
    const float extent = std::max(roofBounds_.max[0] - roofBounds_.min[0],
                                  roofBounds_.max[2] - roofBounds_.min[2]);
    int count = 3 - (extent < 30.0f ? 1 : 0);
    if (extent < 20.0f) count = 1;
    float position[3] = {0, 0, 0};
    if (cISC4Occupant* occupant = building_->AsOccupant()) occupant->GetPosition(position);
    for (int i = 0; i < count; ++i) {
        IConstructionEffect* spark = g_host->CreatePropEffect();
        if (!spark) continue;
        spark->Init();
        spark->SetPosition(position);
        spark->SetEffectType("welding_sparks");
        spark->StartEffect(true);
        sparks_.push_back(spark);
        g_host->AddLotProp(lot_, spark->AsOccupant());
    }
}

// ==== 0x10017be96 ==============================================================================================
void cSC4ConstructionOccupant::CreateLotTexture() {
    if (!g_host || !lot_) return;
    const int cellsX = g_host->TerrainCellCountX();
    const int cellsZ = g_host->TerrainCellCountZ();
    std::int32_t rect[4] = {0, 0, 0, 0};
    lot_->GetCellRect(rect);
    std::vector<LotBaseTextureSpecification> specs;
    specs.reserve(static_cast<std::size_t>((rect[2] + 1 - rect[0]) * (rect[3] + 1 - rect[1])));
    cRZRandom* random = g_host->Random();
    for (std::int32_t x = rect[0]; x <= rect[2]; ++x) {
        for (std::int32_t z = rect[1]; z <= rect[3]; ++z) {
            if (x < 0 || z < 0 || x >= cellsX || z >= cellsZ) continue;
            if (g_host->TerrainCellHasWater(x, z)) continue;
            LotBaseTextureSpecification spec;
            const std::uint32_t frame = random ? random->RandomUint32Uniform(4) : 0;
            spec.textureId = 0xa990c100u + frame * 0x100u;
            spec.cellX = static_cast<std::uint8_t>(x);
            spec.cellZ = static_cast<std::uint8_t>(z);
            spec.rotation = 0;
            spec.flip = 0;
            spec.red = 0xff;
            spec.green = 0xff;
            spec.blue = 0xff;
            spec.alpha = 0xff;
            spec.combineMode = 0xff;
            spec.frame = 0xff;
            specs.push_back(spec);
        }
    }
    cISC4LotBaseTextureOccupant* tiles = nullptr;
    if (!g_host->CreateLotBaseTextureOccupant(lot_, &tiles) || !tiles) return;
    tiles->SetTextureSpecification(specs.data(), static_cast<std::uint32_t>(specs.size()), nullptr);
    if (cISC4Occupant* occupant = tiles->AsOccupant()) {
        if (occupant->IsFlagSet(1)) {
            g_host->MoveOccupant(occupant, false);
        } else {
            g_host->InsertOccupant(occupant, 0);
        }
    }
    tiles->Release();
}

// ==== 0x10017c0e2 ==============================================================================================
void cSC4ConstructionOccupant::GetBounds(cS3DBoundingBox& out) {
    out = bounds_;
    cS3DTransform transform;
    transform.rotation[0][0] = transform_.rotation[0][0];
    transform.rotation[0][1] = transform_.rotation[0][1];
    transform.rotation[0][2] = transform_.rotation[0][2];
    transform.rotation[1][0] = transform_.rotation[1][0];
    transform.rotation[1][1] = transform_.rotation[1][1];
    transform.rotation[1][2] = transform_.rotation[1][2];
    transform.rotation[2][0] = transform_.rotation[2][0];
    transform.rotation[2][1] = transform_.rotation[2][1];
    transform.rotation[2][2] = transform_.rotation[2][2];
    transform.translation[0] = -transform_.translation[0];
    transform.translation[1] = -transform_.translation[1];
    transform.translation[2] = -transform_.translation[2];
    transform.scale = transform_.scale;
    if (transform_.flags & nS3D::kTransformRotation) {
        // The original transposes the rotation and rotates the negated translation (0x10017c1xx): the box comes
        // back in the model's local space.
        const float m[3][3] = {{transform_.rotation[0][0], transform_.rotation[0][1], transform_.rotation[0][2]},
                               {transform_.rotation[1][0], transform_.rotation[1][1], transform_.rotation[1][2]},
                               {transform_.rotation[2][0], transform_.rotation[2][1], transform_.rotation[2][2]}};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) transform.rotation[row][column] = m[column][row];
        }
        float translation[3] = {0, 0, 0};
        for (int row = 0; row < 3; ++row) {
            translation[row] = transform.rotation[row][0] * -transform_.translation[0] +
                               transform.rotation[row][1] * -transform_.translation[1] +
                               transform.rotation[row][2] * -transform_.translation[2];
        }
        transform.translation[0] = translation[0];
        transform.translation[1] = translation[1];
        transform.translation[2] = translation[2];
    }
    if (transform_.flags & nS3D::kTransformScale) {
        // The original divides unconditionally (0x10017c72x); the guard keeps a zero scale from turning the box
        // into NaN (a scale of zero is never written by the class itself).
        if (transform.scale != 0.0f) transform.scale = 1.0f / transform.scale;
    }
    transform.changeCounter = 1;
    transform.flags = 0;
    if (g_host) g_host->TransformBoundingBox(transform, out);
}

// ==== 0x10017c2ce ==============================================================================================
void cSC4ConstructionOccupant::GetWorldBounds(cS3DBoundingBox& out) { out = bounds_; }

// ==== 0x10017c34e ==============================================================================================
void cSC4ConstructionOccupant::GetTransform(cS3DTransform& out) {
    out = transform_;
    ++out.changeCounter;
}

// ==== 0x10017c3e4 ==============================================================================================
void cSC4ConstructionOccupant::SetTransform(const cS3DTransform& transform) {
    transform_ = transform;
    ++transform_.changeCounter;
}

// ==== 0x10017c47e ==============================================================================================
void cSC4ConstructionOccupant::GetSortValue(std::uint32_t& out) const {
    std::uint32_t value = 0;
    if (!textures_.empty() && !textures_[0].empty() && textures_[0][0]) {
        value = static_cast<std::uint32_t>(textures_[0][0]->TextureId());
    }
    out = value;
}

// ==== 0x10017c4d2 ==============================================================================================
int cSC4ConstructionOccupant::Draw(SC4DrawContext* context, bool flag) {
    if (!context || flag) return 1;
    if (rotationChanged_) DoRotationChange(0);
    context->ResetModelViewTransform();
    context->SetRenderState(renderState_);
    s3d::SC4DrawAttributes attributes = GetDrawAttributes();
    context->SetModelShade(this, reinterpret_cast<const cS3DVector3*>(attributes.shade));
    const int zoom = context->Zoom();
    const int rotation = context->ViewRotation() & 3;
    for (int entry = 0; entry < 4; ++entry) {
        const int side = kDrawOrderByRotation[rotation * 4 + entry];
        if (sides_[side].size() != 4) continue;
        std::int32_t texture = 0;
        if (static_cast<std::size_t>(entry) < textures_.size() &&
            static_cast<std::size_t>(zoom) < textures_[entry].size()) {
            IConstructionTexture* binding = textures_[entry][static_cast<std::size_t>(zoom)];
            if (binding) texture = binding->TextureId();
        }
        context->SetTexture(texture, 0);
        context->SetTexWrapModes(3, 3, 0);
        context->EnableBlendStateFlag(entry == 2);
        context->SetDepthOffset(kSideDepthOffset[entry]);
        const ScaffoldVertex& first = sides_[side][0];
        float point[3] = {first.x, first.y, first.z};
        if (g_host) g_host->ProjectToViewCoordinates(point);
        cS3DMatrix4 matrix;   // identity
        matrix.m[0][0] = sfScaffoldTextureScale;
        matrix.m[1][1] = -sfScaffoldTextureScale;
        matrix.m[2][2] = sfScaffoldTextureScale;
        matrix.m[3][0] = -point[0] * sfScaffoldTextureScale;
        matrix.m[3][1] = point[1] * sfScaffoldTextureScale;
        matrix.m[3][2] = -point[2] * sfScaffoldTextureScale;
        context->SetTexTransform4(matrix, 0);
        context->DrawPrims(6, 2, 4, sides_[side].data());
    }
    if (!girders_.empty()) {
        // Quirk: Init never creates the girders' texture row (0x1001779fd stops at row 5), so the original would
        // dereference a null binding here (0x10017c7a1). The port only draws the girders when the row exists.
        std::int32_t texture = 0;
        bool hasTexture = false;
        if (textures_.size() > 5 && static_cast<std::size_t>(zoom) < textures_[5].size() && textures_[5][zoom]) {
            texture = textures_[5][zoom]->TextureId();
            hasTexture = true;
        }
        if (hasTexture) {
            float point[3] = {girders_[0].x, girders_[0].y, girders_[0].z};
            if (g_host) g_host->ProjectToViewCoordinates(point);
            cS3DMatrix4 matrix;
            matrix.m[0][0] = sfGirderTextureScale;
            matrix.m[1][1] = -sfGirderTextureScale;
            matrix.m[2][2] = sfGirderTextureScale;
            matrix.m[3][0] = -point[0] * sfGirderTextureScale;
            matrix.m[3][1] = point[1] * sfGirderTextureScale;
            matrix.m[3][2] = -point[2] * sfGirderTextureScale;
            context->SetTexTransform4(matrix, 0);
            context->EnableBlendStateFlag(true);
            context->SetDepthOffset(-1);
            context->SetTexture(texture, 0);
            context->DrawPrims(6, 2, 4, girders_.data());
        }
    }
    context->ClearTexTransform(0);
    if (!planks_.empty()) {
        std::int32_t texture = 0;
        if (textures_.size() > 4 && static_cast<std::size_t>(zoom) < textures_[4].size() && textures_[4][zoom]) {
            texture = textures_[4][zoom]->TextureId();
        }
        context->EnableBlendStateFlag(false);
        context->SetDepthOffset(-1);
        context->SetTexture(texture, 0);
        context->DrawPrims(6, 2, 0x10, planks_.data());
    }
    if (!posts_.empty()) {
        context->EnableTextureStateFlag(false, 0);
        context->DrawPrims(6, 1, 0x50, posts_.data());
        context->EnableTextureStateFlag(true, 0);
    }
    if (stage_ < 3 && !cranePosts_.empty()) {
        context->EnableTextureStateFlag(false, 0);
        context->DrawPrims(6, 1, 0x10, cranePosts_.data());
        context->EnableTextureStateFlag(true, 0);
    }
    context->ResetShade();
    context->SetDefaultRenderState();
    context->SetDepthOffset(0);
    if (stage_ < 3 && crane_) crane_->Draw(context, false);
    return 1;
}

// ==== 0x10017c93e ==============================================================================================
void cSC4ConstructionOccupant::DoRotationChange(int) {
    rotationChanged_ = false;
    if (posts_.empty() && cranePosts_.empty()) return;
    if (!g_host) return;
    // The five light directions (0x100a196b0 / 0x100a16ae0 / 0x100a196c0 / the qword at +0x30):
    // -Z, +Z, +X, -X, +Y. Each gives one RGB colour, halved into a shade byte.
    const float directions[5][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}};
    std::uint8_t shade[15] = {0};
    for (int light = 0; light < 5; ++light) {
        std::uint8_t rgb[3] = {0, 0, 0};
        g_host->LightColourForDirection(directions[light], rgb);
        shade[light * 3] = static_cast<std::uint8_t>(rgb[0] >> 1);
        shade[light * 3 + 1] = static_cast<std::uint8_t>(rgb[1] >> 1);
        shade[light * 3 + 2] = static_cast<std::uint8_t>(rgb[2] >> 1);
    }
    if (!posts_.empty()) {
        // Per side the 20 vertices are five quads: the four vertical faces get shade[face] (one colour per face,
        // 0x10017ca5c walks the quad index) and the top quad gets shade[4].
        for (std::size_t side = 0; side < 4; ++side) {
            for (std::size_t quad = 0; quad < 5; ++quad) {
                const std::uint8_t r = shade[quad * 3];
                const std::uint8_t g = shade[quad * 3 + 1];
                const std::uint8_t b = shade[quad * 3 + 2];
                for (std::size_t vertex = 0; vertex < 4; ++vertex) {
                    const std::size_t index = side * 20 + quad * 4 + vertex;
                    if (index >= posts_.size()) continue;
                    posts_[index].colour = 0xff000000u | (static_cast<std::uint32_t>(b) << 16) |
                                           (static_cast<std::uint32_t>(g) << 8) | r;
                }
            }
        }
    }
    if (!cranePosts_.empty()) {
        for (std::size_t quad = 0; quad < 4; ++quad) {
            const int corner = kPropOrientationTable[quad];
            const std::uint8_t r = shade[corner * 3];
            const std::uint8_t g = shade[corner * 3 + 1];
            const std::uint8_t b = shade[corner * 3 + 2];
            for (std::size_t vertex = 0; vertex < 4; ++vertex) {
                const std::size_t index = quad * 4 + vertex;
                if (index >= cranePosts_.size()) continue;
                cranePosts_[index].colour = 0xff000000u | (static_cast<std::uint32_t>(b) << 16) |
                                            (static_cast<std::uint32_t>(g) << 8) | r;
            }
        }
    }
}

// ==== 0x10017cb4e ==============================================================================================
void cSC4ConstructionOccupant::GetPosition(float* xyz) {
    xyz[0] = (bounds_.min[0] + bounds_.max[0]) * 0.5f;
    xyz[1] = (bounds_.min[1] + bounds_.max[1]) * 0.5f;
    xyz[2] = (bounds_.min[2] + bounds_.max[2]) * 0.5f;
}

// ==== 0x10017cc0a ==============================================================================================
void cSC4ConstructionOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {
    for (int i = 0; i < 3; ++i) minXyz[i] = bounds_.min[i];
    for (int i = 0; i < 3; ++i) maxXyz[i] = bounds_.max[i];
}

// ==== 0x10017cc7e ==============================================================================================
// PARTIAL: the side availability tests, the yaw range and the CreateCrane call are ported; the final strip
// rectangle the original clips the crane's footprint to (0x10017d0c0..0x10017d15b) is reconstructed from the
// stack slots Ghidra rendered separately, so the port clips the square (centre +- arm) to the strip box.
int cSC4ConstructionOccupant::AttemptAddOuterCrane(cS3DBoundingBox* boxes) {
    if (!building_ || !g_host) return -1;
    cISC4Occupant* occupant = building_->AsOccupant();
    if (!occupant) return -1;
    float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};
    occupant->GetBoundingBox(min, max);
    const float width = max[0] - min[0];
    const float depth = max[2] - min[2];
    if (!(12.0f <= roofBounds_.max[1] - bounds_.min[1])) return -1;   // the crane needs a 12 m tall scaffold

    // Which of the eight lot rectangles have room for an outer crane on the side they represent.
    bool found = false;
    int modelIndex = -1;
    int side = -1;
    for (int model = 0; model < 3; ++model) {
        if (!(sfCraneLength[model] != 0.0f) && !std::isnan(sfCraneLength[model])) continue;
        const float arm = sfCraneFrontArmLength[model];
        if (!(12.0f <= depth || arm * 0.8f <= width)) continue;
        bool flags[4] = {true, true, true, true};
        // The four side tests of 0x10017cd75: lot edge minus the opposite inner edge >= the front arm.
        flags[3] = boxes[1].max[0] - (min[0] - sfScaffoldGap) >= arm;   // east
        flags[1] = (max[0] + sfScaffoldGap) - boxes[3].min[0] >= arm;   // west
        flags[2] = (max[2] + sfScaffoldGap) - boxes[0].min[2] >= arm;   // south
        flags[0] = boxes[2].max[2] - (min[2] - sfScaffoldGap) >= arm;   // north
        // Side 0 must also fit the back arm plus the post offsets in its strip.
        const float arm2 = sfCraneBackArmLength[model] + sfCranePostWidth + sfCranePostToBuildingGap;
        const float stripDepth[4] = {boxes[0].max[2] - boxes[0].min[2], boxes[1].max[0] - boxes[1].min[0],
                                     boxes[2].max[2] - boxes[2].min[2], boxes[3].max[0] - boxes[3].min[0]};
        int availableCount = 0;
        for (int i = 0; i < 4; ++i) {
            if (flags[i] && stripDepth[i] < arm2) flags[i] = false;
            if (flags[i]) ++availableCount;
        }
        if (availableCount == 0) continue;
        const std::uint32_t pick = g_host->Random()->RandomUint32Uniform(static_cast<std::uint32_t>(availableCount));
        int chosen = -1;
        std::uint32_t seen = 0;
        for (int step = 0; step < 4; ++step) {
            const int index = step;
            if (!flags[index]) continue;
            if (seen == pick) {
                chosen = index;
                break;
            }
            ++seen;
        }
        if (chosen < 0) continue;
        modelIndex = model;
        side = chosen;
        found = true;
        break;
    }
    if (!found || side < 0) return -1;

    cS3DBoundingBox& strip = boxes[side];
    const float offset = sfCranePostWidth * 0.5f + sfCranePostToBuildingGap;
    float x = (strip.max[0] - strip.min[0]) * 0.5f + strip.min[0];
    float z = (strip.max[2] - strip.min[2]) * 0.5f + strip.min[2];
    float direction[2] = {0, 0};
    switch (side) {
        case 0: z = strip.max[2] - offset; direction[0] = 0; direction[1] = 1; break;
        case 1: x = strip.min[0] + offset; direction[0] = -1; direction[1] = 0; break;
        case 2: z = strip.min[2] + offset; direction[0] = 0; direction[1] = -1; break;
        default: x = strip.max[0] - offset; direction[0] = 1; direction[1] = 0; break;
    }
    const float arm = sfCraneFrontArmLength[modelIndex];
    float yawMin = 0.0f;
    float yawMax = 0.0f;
    if (direction[0] != 0.0f) {
        // The original clamps to the lot's edges, boxes[4].min.z and boxes[6].max.z (0x10017d0xx).
        const float distance = std::min(z - boxes[4].min[2], boxes[6].max[2] - z);
        if (arm <= distance) {
            yawMin = direction[0] < 0.0f ? 2.3561945f : 0.7853982f;
            yawMax = direction[0] < 0.0f ? 4.712389f : 2.3561945f;
        } else {
            const float angle = std::asin(distance / arm);
            const float centre = direction[0] < 0.0f ? 1.5707964f : kPi;
            yawMin = centre - angle;
            yawMax = centre + angle;
        }
    } else {
        const float distance = std::min(x - boxes[4].min[0], boxes[6].min[0] - x);
        if (distance < arm) {
            const float angle = std::asin(distance / arm);
            const float centre = direction[1] < 0.0f ? 2.3561945f : 0.7853982f;
            yawMin = centre - angle;
            yawMax = centre + angle;
        } else {
            yawMin = direction[1] < 0.0f ? 0.0f : kPi;
            yawMax = direction[1] < 0.0f ? 1.5707964f : kPi;
        }
    }
    craneYawMin_ = yawMin;
    craneYawMax_ = yawMax;
    craneType_ = static_cast<std::uint8_t>(modelIndex);
    CreateCrane(x, z, modelIndex);
    const float arm2 = sfCraneBackArmLength[modelIndex] + sfCranePostWidth + sfCranePostToBuildingGap;
    cS3DBoundingBox removed;
    removed.min[0] = std::max(x - arm2, strip.min[0]);
    removed.min[1] = strip.max[1];
    removed.min[2] = std::max(z - arm2, strip.min[2]);
    removed.max[0] = std::min(x + arm2, strip.max[0]);
    removed.max[1] = strip.max[1];
    removed.max[2] = std::min(z + arm2, strip.max[2]);
    RemoveRectangle(strip, removed);
    return side;
}

// ==== 0x10017d204 ==============================================================================================
int cSC4ConstructionOccupant::AttemptAddInnerCrane() {
    if (!building_ || !g_host) return 0;
    cISC4Occupant* occupant = building_->AsOccupant();
    if (!occupant) return 0;
    float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};
    occupant->GetBoundingBox(min, max);
    const float width = max[0] - min[0];
    const float depth = max[2] - min[2];
    cRZRandom* random = g_host->Random();
    const std::uint32_t raw = random ? random->RandomUint32Uniform() : 0;
    // 0/2 when the footprint is wider than deep, 1/3 when it is deeper than wide.
    const bool horizontal = depth <= width;
    const int orientation = (raw > 0x7ffffffe ? 2 : 0) + (horizontal ? 0 : 1);
    int modelIndex = -1;
    for (int model = 0; model < 3; ++model) {
        if (!(sfCraneLength[model] > 0.0f)) continue;
        if (sfCraneFrontArmLength[model] + sfCraneBackArmLength[model] < std::max(width, depth)) {
            modelIndex = model;
            break;
        }
    }
    if (modelIndex < 0) return 0;
    const float front = sfCraneFrontArmLength[modelIndex];
    const float back = sfCraneBackArmLength[modelIndex];
    float x = 0.0f;
    float z = 0.0f;
    float centre = 0.0f;
    float angle = 0.0f;
    if (width < depth) {
        const float armOffset = (front + back) * 0.5f - back;
        z = (max[2] - min[2]) * 0.5f + min[2] + (orientation == 3 ? -armOffset : armOffset);
        const float halfWidth = (max[0] - min[0]) * 0.5f;
        x = min[0] + halfWidth;
        if (front <= halfWidth) {
            craneYawMin_ = orientation == 1 ? 0.0f : kPi;
            craneYawMax_ = orientation == 1 ? 1.5707964f : kPi;
        } else {
            angle = std::asin(halfWidth / front);
            centre = orientation == 1 ? 2.3561945f : 0.7853982f;
            craneYawMin_ = centre - angle;
            craneYawMax_ = centre + angle;
        }
    } else {
        const float armOffset = (front + back) * 0.5f - back;
        x = (max[0] - min[0]) * 0.5f + min[0] + (orientation == 0 ? -armOffset : armOffset);
        const float halfDepth = (max[2] - min[2]) * 0.5f;
        z = min[2] + halfDepth;
        if (halfDepth < front) {
            angle = std::asin(halfDepth / front);
            centre = orientation == 0 ? 1.5707964f : 3.1415927f;
            craneYawMin_ = centre - angle;
            craneYawMax_ = centre + angle;
        } else {
            craneYawMin_ = orientation == 2 ? 0.7853982f : 2.3561945f;
            craneYawMax_ = orientation == 2 ? 2.3561945f : 4.712389f;
        }
    }
    craneType_ = static_cast<std::uint8_t>(modelIndex);
    CreateCrane(x, z, modelIndex);
    return 1;
}

// ==== 0x10017d4e4 ==============================================================================================
void cSC4ConstructionOccupant::RemoveRectangle(cS3DBoundingBox& box, const cS3DBoundingBox& removed) {
    // The four slabs that remain when `removed` is cut out of `box`; the largest one wins.
    cS3DBoundingBox candidates[4];
    for (auto& candidate : candidates) candidate = box;
    candidates[0].max[2] = removed.min[2];
    candidates[1].min[0] = removed.max[0];
    candidates[2].min[2] = removed.max[2];
    candidates[3].max[0] = removed.min[0];
    float best = 0.0f;
    int bestIndex = -1;
    for (int i = 0; i < 4; ++i) {
        const float area = (candidates[i].max[0] - candidates[i].min[0]) *
                           (candidates[i].max[2] - candidates[i].min[2]);
        if (area > best) {
            best = area;
            bestIndex = i;
        }
    }
    if (bestIndex < 0) return;   // none overlaps: the box is left alone (the original keeps it too)
    box = candidates[bestIndex];
}

// ==== 0x10017d63a ==============================================================================================
bool cSC4ConstructionOccupant::CreateAndPlaceProp(cS3DBoundingBox& box, std::uint32_t index,
                                                  IConstructionProp** out, bool ground) {
    if (!out || !g_host || index >= sPropList.size()) return false;
    *out = nullptr;
    const ConstructionPropEntry& entry = sPropList[index];
    IConstructionProp* prop = nullptr;
    if (!g_host->CreateProp(entry.type, &prop) || !prop) return false;
    const float width = box.max[0] - box.min[0];
    const float depth = box.max[2] - box.min[2];
    // Four orientations when the box is narrower than the prop, two otherwise (0x10017d6xx).
    const std::uint32_t orientations =
        (width < entry.sizeZ || width == entry.sizeZ || depth < entry.sizeX || depth == entry.sizeX) ? 2 : 4;
    const std::uint32_t pick = g_host->Random()->RandomUint32Uniform(orientations);
    const std::uint32_t orientation = static_cast<std::uint32_t>(kPropOrientationTable[pick]);
    float sizeX = entry.sizeX;
    float sizeZ = entry.sizeZ;
    if ((orientation | 2u) != 2u) std::swap(sizeX, sizeZ);   // the 90-degree orientations swap the footprint
    const float halfX = sizeX * 0.5f;
    const float halfZ = sizeZ * 0.5f;
    const float x = static_cast<float>(g_host->Random()->RandomDoubleRangeUniform(
                         0.0, static_cast<double>(width - sizeX))) + halfX + box.min[0];
    const float z = static_cast<float>(g_host->Random()->RandomDoubleRangeUniform(
                         0.0, static_cast<double>(depth - sizeZ))) + halfZ + box.min[2];
    float y = 0.0f;
    if (cISC4BuildingOccupant* lotBuilding = g_host->LotBuilding(lot_)) {
        if (cISC4Occupant* lotOccupant = lotBuilding->AsOccupant()) {
            float position[3] = {0, 0, 0};
            lotOccupant->GetPosition(position);
            y = position[1];
        }
    } else {
        const float a0 = g_host->TerrainAltitude(x - halfX, z - halfZ);
        const float a1 = g_host->TerrainAltitude(x + halfX, z - halfZ);
        const float a2 = g_host->TerrainAltitude(x - halfX, z + halfZ);
        const float a3 = g_host->TerrainAltitude(x + halfX, z + halfZ);
        y = std::max(std::max(a0, a1), std::max(a2, a3));
    }
    if (cISC4Occupant* occupant = prop->AsOccupant()) {
        const float position[3] = {x, y, z};
        occupant->SetPosition(position);
        if (!ground) occupant->SetFlag(0x10);
    }
    prop->SetOrientation(static_cast<std::int32_t>(orientation));
    g_host->AddLotProp(lot_, prop->AsOccupant());
    cS3DBoundingBox removed;
    removed.min[0] = x - halfX;
    removed.min[1] = 0.0f;
    removed.min[2] = z - halfZ;
    removed.max[0] = x + halfX;
    removed.max[1] = 1.0f;
    removed.max[2] = z + halfZ;
    RemoveRectangle(box, removed);
    *out = prop;
    return true;
}

// ==== 0x10017d9f6 ==============================================================================================
void cSC4ConstructionOccupant::CreateAutomata(cS3DBoundingBox& box, bool roof, std::uint32_t automataType) {
    if (!g_host) return;
    cRZRandom* random = g_host->Random();
    const float x = static_cast<float>(random->RandomDoubleRangeUniform(box.min[0] + 0.5,
                                                                        box.max[0] - 1.5));
    const float z = static_cast<float>(random->RandomDoubleRangeUniform(box.min[2] + 0.5,
                                                                        box.max[2] - 1.5));
    cS3DBoundingBox small;
    small.min[0] = x;
    small.min[1] = 0.0f;
    small.min[2] = z;
    small.max[0] = x + 1.0f;
    small.max[1] = 1.0f;
    small.max[2] = z + 1.0f;
    RemoveRectangle(box, small);
    const float cx = (small.min[0] + small.max[0]) * 0.5f;
    const float cz = (small.min[2] + small.max[2]) * 0.5f;
    const float position[3] = {cx, g_host->TerrainAltitude(cx, cz), cz};
    const cGZPersistResourceKey key{0x49a593e7, 0x29a5d1ec, automataType};
    IConstructionAutomaton* automaton = g_host->CreateAutomaton(0x4101, position, 0x32000, key, 0x32);
    if (!automaton) return;
    automaton->SetAutomataFlag(2, false);
    const cS3DVector2 direction = nS3D::RandomDirection2(*random);
    const float dir[3] = {direction.x, 0.0f, direction.y};
    automaton->SetOrientation(dir);
    CrewData crew;
    crew.first = false;
    crew.onRoof = roof;
    crew.automaton = automaton;
    automaton->AddRef();
    crews_.push_back(crew);
}

// ==== 0x10017dd04 ==============================================================================================
cIGZUnknown* cSC4ConstructionOccupant::GetOwner() { return AsOccupant(); }

// ---- helpers --------------------------------------------------------------------------------------------------
void cSC4ConstructionOccupant::AddPropToList(std::list<IConstructionProp*>& list, IConstructionProp* prop) {
    if (!prop) return;
    // The one reference cSC4PropManager::CreateProp hands out goes straight to the list (no AddRef, 0x10017b6xx).
    list.push_back(prop);
}

void cSC4ConstructionOccupant::ClearPropList(std::list<IConstructionProp*>& list) {
    if (g_host) {
        for (IConstructionProp* prop : list) {
            if (prop) g_host->RemoveProp(prop->AsOccupant());
        }
    }
    for (IConstructionProp* prop : list) ReleaseRef(prop);
    list.clear();
}

// The original's RandomUint32UniformBelow(1000) (AnimateCrane's crane pause).
std::uint32_t cSC4ConstructionOccupant::craneRandomBelow(std::uint32_t range) const {
    if (!g_host) return 0;
    if (cRZRandom* random = g_host->Random()) return random->RandomUint32Uniform(range);
    return 0;
}

}  // namespace sc4
