// cSC4ViewInputControlPlaceLot — see the header for the layout, the quirks and the seams. The addresses in
// the comments are the original's (Sim City 4 Deluxe Editionsub 1.2.1, x86_64).
#include "sc4/ViewInputControlPlaceLot.h"

#include <algorithm>
#include <cmath>

namespace sc4 {
namespace {

IPlaceLotHost* g_placeLotHost = nullptr;

constexpr int kFacingCount = 4;
constexpr std::uint32_t kCoverageRed = 6;     // cSC4LotConfigurationObject::type of the red constraint
constexpr std::uint32_t kCoverageBlue = 5;    // ... and of the blue one
constexpr std::uint32_t kFootprintTextureParam = 6;
constexpr std::uint32_t kSelectionRectParam = 0xc;

// The transform the original builds from a position, a y rotation and a scale (the effect slots take it by
// reference). The original's change counter is 2 or 3 and its flag word 1/3/5/7.
cS3DTransform MakeEffectTransform(const cS3DVector3& position, float yRotation, float scale,
                                  std::uint8_t changeCounter, std::uint8_t flags) {
    cS3DTransform transform;
    transform.changeCounter = changeCounter;
    transform.flags = flags;
    const float c = std::cos(yRotation);
    const float s = std::sin(yRotation);
    transform.rotation[0][0] = c;
    transform.rotation[0][1] = 0.0f;
    transform.rotation[0][2] = s;
    transform.rotation[1][0] = 0.0f;
    transform.rotation[1][1] = 1.0f;
    transform.rotation[1][2] = 0.0f;
    transform.rotation[2][0] = -s;
    transform.rotation[2][1] = 0.0f;
    transform.rotation[2][2] = c;
    transform.translation[0] = position.x;
    transform.translation[1] = position.y;
    transform.translation[2] = position.z;
    transform.scale = scale;
    return transform;
}

const char* CoverageCategoryName(std::uint8_t categories) {
    if (categories & 1) return "Police";
    if (categories & 2) return "Fire";
    if (categories & 4) return "Health";
    if (categories & 8) return "Education";
    return "";
}

}  // namespace

IPlaceLotHost* PlaceLotHost() { return g_placeLotHost; }
void SetPlaceLotHost(IPlaceLotHost* host) { g_placeLotHost = host; }

// ---------------------------------------------------------------------------------------------------
// Slice 1: construction, reference counting, Init / Shutdown
// ---------------------------------------------------------------------------------------------------

// @ 0x1003bb6ba
cSC4ViewInputControlPlaceLot::cSC4ViewInputControlPlaceLot(int toolKind, std::uint32_t lotConfigurationId,
                                                          std::uint32_t buildingType, bool isReward)
    : toolKind_(toolKind),
      lotConfigurationId_(lotConfigurationId),
      buildingType_(buildingType),
      isReward_(isReward) {
    // The original's member list (the base already initialised +0x08..+0x38):
    // +0x54 and +0x58 stay 0, +0x5c = -1, +0x98 = 1.0f, +0x1e0 / +0x1f0 = -100000, +0x1d4 = 32.0f,
    // +0x21c = 1000. Everything else is zero. Those are the member defaults above.
}

// @ 0x1003bb94a (plus the deleting destructor at 0x1003bbb8c and the alias at 0x1003bbb82)
cSC4ViewInputControlPlaceLot::~cSC4ViewInputControlPlaceLot() {
    for (PreviewProp& preview : previewProps_) {
        if (preview.prop) preview.prop->Release();
    }
    previewProps_.clear();
    for (IPlaceLotEffect* effect : selectionRed_) if (effect) effect->Release();
    for (IPlaceLotEffect* effect : selectionBlue_) if (effect) effect->Release();
    selectionRed_.clear();
    selectionBlue_.clear();
    if (previewProp_) { previewProp_->Release(); previewProp_ = nullptr; }
    if (coverageEffect_) { coverageEffect_->Release(); coverageEffect_ = nullptr; }
    if (footprintEffect_) { footprintEffect_->Release(); footprintEffect_ = nullptr; }
    if (directionEffect_) { directionEffect_->Release(); directionEffect_ = nullptr; }
    // The local grid helper is embedded in the original object (+0x108); the seam hands out a
    // host-owned object, so the tool does not delete it.
    localGrid_ = nullptr;
    // The grid texture (cS3DTextureBinding) is never created by this port: GetBuildingPreviewModel stops
    // before the texture block (see the PARTIAL note there), so there is nothing to release.
}

// @ 0x1003bdcf0 / @ 0x1003bdcfe (the +0x40 thunk)
std::uint32_t cSC4ViewInputControlPlaceLot::AddRef() { return ++refCount_; }
std::uint32_t cSC4ViewInputControlPlaceLot::AddRefFromMessageTarget() { return AddRef(); }

// @ 0x1003bdd0c / @ 0x1003bdd2c (the +0x40 thunk)
std::uint32_t cSC4ViewInputControlPlaceLot::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}
std::uint32_t cSC4ViewInputControlPlaceLot::ReleaseFromMessageTarget() { return Release(); }

// @ 0x1003bdd50 / @ 0x1003bdd84 (the +0x40 thunk)
bool cSC4ViewInputControlPlaceLot::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kPlaceLotMessageTargetId || iid == kGZIID_cIGZUnknown) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cISC4ViewInputControl) {
        *out = static_cast<cSC4ViewInputControl*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}
bool cSC4ViewInputControlPlaceLot::QueryInterfaceFromMessageTarget(std::uint32_t iid, void** out) {
    return QueryInterface(iid, out);
}

IPlaceLotView* cSC4ViewInputControlPlaceLot::View() const {
    IPlaceLotHost* host = Host();
    return host ? host->View() : nullptr;
}
IPlaceLotTerrain* cSC4ViewInputControlPlaceLot::Terrain() const {
    IPlaceLotHost* host = Host();
    return host ? host->Terrain() : nullptr;
}
IPlaceLotCity* cSC4ViewInputControlPlaceLot::City() const {
    IPlaceLotHost* host = Host();
    return host ? host->City() : nullptr;
}
cSC4LotConfiguration* cSC4ViewInputControlPlaceLot::Configuration() const {
    IPlaceLotHost* host = Host();
    return host && lotConfigurationId_ != 0 ? host->GetLotConfiguration(lotConfigurationId_) : nullptr;
}

// @ 0x1003bbbba
bool cSC4ViewInputControlPlaceLot::Init() {
    if (initialized_) return true;
    // 0x88e70b7688f154fb: tool id 0x88f154fb and cursor id 0x88e70b76 in one store.
    id_ = kPlaceLotToolId;
    cursorId_ = kPlaceLotCursorId;
    cSC4ViewInputControl::Init();
    IPlaceLotHost* host = Host();
    if (!host) return true;

    if (cIGZMessageServer2* server = host->MessageServer()) {
        for (std::uint32_t message : kPlaceLotMessages) {
            server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
        }
    }
    // The lot configuration manager (+0x68) and the building development simulator (+0xa0) are referenced by
    // the original; in this port the host owns them and every call goes through IPlaceLotHost.

    if (toolKind_ == 0) {
        // Civic / landmark mode: only when no configuration was handed in does the original filter the
        // configuration list and let the user pick one.
        if (lotConfigurationId_ == 0) {
            std::uint32_t current = lotConfigurationId_;
            if (lotConfigurationIds_.empty()) {
                IPlaceLotFilter* filter = host->CreatePlopFilter(isReward_, 0);
                host->GetLotConfigurationIDsByFilter(lotConfigurationIds_, filter);
                if (filter) filter->Release();
            }
            if (!lotConfigurationIds_.empty()) {
                current = *lotConfigurationIds_.begin();
                lotConfigurationId_ = current;
            }
            lotConfigurationId_ = DoUserLotConfigurationTypePick(current);
        }
        if (lotConfigurationId_ == kTollBoothLotConfiguration) tollBoothMode_ = true;
    } else {
        // Building-type mode (the reward / landmark tools already know the building).
        if (buildingType_ == 0) {
            buildingType_ = View() ? View()->LastSelectedBuilding() : 0;
            if (buildingTypes_.empty() && host->BuildingDevelopment()) {
                host->BuildingDevelopment()->GetAllBuildingTypes(buildingTypes_);
            }
            if (!buildingTypes_.empty() && buildingType_ == 0) buildingType_ = buildingTypes_.front();
            buildingType_ = DoUserBuildingTypePick(buildingType_);
            if (View()) View()->SetLastSelectedBuilding(buildingType_);
            if (buildingType_ == 0) goto configuration_done;
        }
        lotConfigurationIds_.clear();
        {
            IPlaceLotFilter* filter = host->CreatePlopFilter(isReward_, buildingType_);
            host->GetLotConfigurationIDsByFilter(lotConfigurationIds_, filter);
            if (filter) filter->Release();
        }
        if (lotConfigurationIds_.empty()) {
            // The original releases the half-built tool (vtable slot 0x20) and answers false.
            Release();
            return false;
        }
        lotConfigurationId_ = View() ? View()->LastSelectedLot() : 0;
        lotConfigurationId_ = DoUserLotConfigurationTypePick(lotConfigurationId_);
        if (View()) View()->SetLastSelectedLot(lotConfigurationId_);
    }

configuration_done:
    {
        // The plop cost comes from the building's profile: the given building type, or the first building of the lot
        // configuration's primary building object (Mac i386 0x2d7984: a local, the configuration id is kept).
        std::uint32_t priced = buildingType_;
        if (priced == 0 && lotConfigurationId_ != 0) {
            if (cSC4LotConfiguration* configuration = host->GetLotConfiguration(lotConfigurationId_)) {
                if (cSC4LotConfigurationObject* object = configuration->GetPrimaryBuildingConfigurationObject()) {
                    std::vector<std::uint32_t> ids;
                    host->GetObjectIDArray(object, ids);
                    if (!ids.empty()) priced = ids.front();
                }
            }
        }
        if (priced != 0 && host->BuildingDevelopment()) buildingPrice_ = host->BuildingDevelopment()->BuildingPrice(priced);
    }

    GetBuildingPreviewModel();
    if (localGrid_ == nullptr) localGrid_ = host->CreateLocalGrid();
    if (localGrid_) localGrid_->Init();
    // +0x110 holds the coverage effect name written by GetBuildingPreviewModel ("PlopMode_*_Inactive").
    if (coverageEffect_ == nullptr) coverageEffect_ = CreateEffect(effectName_.ToChar());
    if (globalEffectId_ != 0 && View()) View()->AddGlobalEffect(globalEffectId_);
    return true;
}

// @ 0x1003bd89a
bool cSC4ViewInputControlPlaceLot::Shutdown() {
    if (!initialized_) return true;
    IPlaceLotHost* host = Host();
    if (coverageActive_ != 0) {
        coverageActive_ = 0;
        if (View()) {
            if (IPlaceLotCoverageHelper* helper = View()->CoverageHelper()) helper->RecalcCoverage();
        }
    }
    if (globalEffectId_ != 0 && View()) View()->RemoveGlobalEffect(globalEffectId_);
    if (View()) {
        View()->ClearCursorText(kPlaceLotCursorTextId);
        View()->ClearCostIndicator(false);
        if (IPlaceLotCoverageHelper* helper = View()->CoverageHelper()) {
            helper->SetCategories(0);
            helper->RecalcCoverage();
        }
    }
    if (localGrid_) localGrid_->Shutdown();
    ReleaseLotConfigurationEffects();
    if (host) {
        if (cIGZMessageServer2* server = host->MessageServer()) {
            for (std::uint32_t message : kPlaceLotMessages) {
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), message);
            }
        }
    }
    cSC4ViewInputControl::Shutdown();
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Slice 2: preview state, feedback and the drag / footprint tracking
// ---------------------------------------------------------------------------------------------------

// @ 0x1003bda28
void cSC4ViewInputControlPlaceLot::ReleaseLotConfigurationEffects() {
    if (coverageEffect_) {
        if (coverageEffect_->IsStarted()) coverageEffect_->Stop(0);
        coverageEffect_->Release();
        coverageEffect_ = nullptr;
    }
    if (footprintEffect_) {
        if (footprintEffect_->IsStarted()) footprintEffect_->Stop(0);
        footprintEffect_->Release();
        footprintEffect_ = nullptr;
    }
    if (directionEffect_) {
        if (directionEffect_->IsStarted()) directionEffect_->Stop(0);
        directionEffect_->Release();
        directionEffect_ = nullptr;
    }
    // The original releases the grid texture binding here (cS3DTextureBinding::Release); this port never
    // creates it (GetBuildingPreviewModel is PARTIAL at the texture block).
    gridTexture_ = nullptr;
    ClearEffectList(selectionBlue_);
    ClearEffectList(selectionRed_);
    if (previewProp_) {
        if (previewProp_->IsPlaced()) previewProp_->Hide();
        previewProp_->Release();
        previewProp_ = nullptr;
    }
    for (PreviewProp& preview : previewProps_) {
        if (!preview.prop) continue;
        if (preview.prop->IsPlaced()) preview.prop->Hide();
        preview.prop->Release();
        preview.prop = nullptr;
    }
    previewProps_.clear();
}

void cSC4ViewInputControlPlaceLot::ClearEffectList(std::vector<IPlaceLotEffect*>& effects) {
    for (IPlaceLotEffect* effect : effects) {
        if (!effect) continue;
        effect->Stop(0);   // the original stops every element of both selection lists
        effect->Release();
    }
    effects.clear();
}

IPlaceLotEffect* cSC4ViewInputControlPlaceLot::CreateEffect(const char* name) {
    IPlaceLotHost* host = Host();
    if (!host || !name) return nullptr;
    IPlaceLotEffects* effects = host->Effects();
    return effects ? effects->Create(name) : nullptr;
}

void cSC4ViewInputControlPlaceLot::ReleaseEffect(IPlaceLotEffect*& effect) {
    if (!effect) return;
    if (effect->IsStarted()) effect->Stop(0);
    effect->Release();
    effect = nullptr;
}

// @ 0x1003bdd92
void cSC4ViewInputControlPlaceLot::SetPropOccupantDrawAttributes(const cS3DVector3& position,
                                                                s3d::SC4DrawAttributes& attributes,
                                                                bool show) {
    // The original rotates the point into the lot's orientation, then moves every prop to it. The switch at
    // +0x58 is the facing (0 north, 1 east, 2 south, 3 west).
    const float dx = modelCenterX_;   // +0x1c0
    const float dz = modelCenterZ_;   // +0x1c8
    cS3DVector3 point = position;
    switch (facing_) {
        case 0: point.x -= dx; point.z -= dz; break;
        case 1: point.x += dz; point.z -= dx; break;
        case 2: point.x += dx; point.z += dz; break;
        case 3: point.x -= dz; point.z += dx; break;
        default: break;
    }
    if (previewProp_) {
        const int orientation = (facing_ + static_cast<int>(buildingRotation_)) % 4;
        previewProp_->SetOrientation(orientation);
        if (show) {
            previewProp_->Show(point, attributes);
        } else if (previewProp_->IsPlaced()) {
            previewProp_->Hide();
        }
    }
    // The preview props keep their own offsets from the lot centre (they were stored relative to it).
    for (PreviewProp& preview : previewProps_) {
        if (!preview.prop) continue;
        // The original rotates the offset and then adds the lot position; the offsets are already in world
        // units, relative to the lot centre.
        float px = preview.x;
        float pz = preview.z;
        const float ox = px;
        switch (facing_) {
            case 0: px = position.x - ox; pz = position.z - preview.z; break;
            case 1: px = position.x + preview.z; pz = position.z - ox; break;
            case 2: px = position.x + ox; pz = position.z + preview.z; break;
            case 3: px = position.x - preview.z; pz = position.z + ox; break;
            default: break;
        }
        if (show) {
            const int orientation = (facing_ + static_cast<int>(preview.orientation)) % 4;
            preview.prop->SetOrientation(orientation);
            preview.prop->Show(cS3DVector3{px, preview.y, pz}, attributes);
        } else if (preview.prop->IsPlaced()) {
            preview.prop->Hide();
        }
    }
}

// @ 0x1003be50c
void cSC4ViewInputControlPlaceLot::SetVisualUserFeedback(bool valid, bool secondary,
                                                         const cS3DVector3& position) {
    IPlaceLotHost* host = Host();
    if (!host) return;
    if (!secondary) {
        if (localGrid_) localGrid_->ShowEffect(false);
        for (IPlaceLotEffect* effect : {coverageEffect_, footprintEffect_, directionEffect_}) {
            if (effect && effect->IsStarted()) effect->Stop(0);
        }
        if (previewProp_) {
            s3d::SC4DrawAttributes attributes;
            attributes.flags = 2;
            attributes.shade[0] = attributes.shade[1] = attributes.shade[2] = attributes.shade[3] = 1.0f;
            SetPropOccupantDrawAttributes(position, attributes, false);
        }
        ClearEffectList(selectionBlue_);
        ClearEffectList(selectionRed_);
        if (valid != feedbackValid_) {
            SetCursor(valid ? kPlaceLotCursorValid : kPlaceLotCursorId);
        }
        return;
    }

    if (localGrid_) {
        localGrid_->SetPosition(position);
        bool water = false;
        if (IPlaceLotTerrain* terrain = Terrain()) {
            if (IPlaceLotTerrainQuery* query = terrain->Query()) water = query->IsWater(position.x, position.z);
        }
        localGrid_->ShowEffect(!water);
    }

    // 1. The coverage ring (created by Init from the "PlopMode_*_Inactive" name).
    if (coverageEffect_ && coverageRingRadius_ > 0.0f) {
        SetCoverageEffectTransform(position);
    }

    // 2. The footprint grid effect: recreated when the validity changed, only moved otherwise.
    if (valid != feedbackValid_ || !footprintEffect_ || !footprintEffect_->IsStarted()) {
        ReleaseEffect(footprintEffect_);
        footprintEffect_ = CreateEffect(valid ? "building_footprint_ok" : "building_footprint_notok");
        if (footprintEffect_ && gridTexture_) {
            footprintEffect_->SetVoidParams(kFootprintTextureParam, &gridTexture_, 1);
        }
    }
    if (footprintEffect_) {
        SetFootprintEffectTransform(position, facing_);
        if (!footprintEffect_->IsStarted()) footprintEffect_->Start(0);
    }

    // 3. The direction arrow: same lifecycle.
    if (valid != feedbackValid_ || !directionEffect_ || !directionEffect_->IsStarted()) {
        ReleaseEffect(directionEffect_);
        directionEffect_ = CreateEffect("Lot_Direction_Arrow");
    }
    if (directionEffect_) {
        std::uint32_t sizeX = 0, sizeZ = 0;
        cSC4LotConfiguration* configuration = Configuration();
        if (configuration) configuration->GetSize(sizeX, sizeZ);
        SetDirectionEffectTransform(position, facing_, static_cast<int>(sizeX), static_cast<int>(sizeZ));
        if (!directionEffect_->IsStarted()) directionEffect_->Start(0);
    }

    // 4. The preview props.
    if (previewProp_) {
        s3d::SC4DrawAttributes attributes;
        attributes.flags = static_cast<std::uint32_t>(3 | ((4 - (valid ? 1 : 0)) << 8));
        attributes.shade[0] = attributes.shade[1] = attributes.shade[2] = 1.0f;
        attributes.shade[3] = 0.4f;
        SetPropOccupantDrawAttributes(position, attributes, true);
    }

    // 5. The constraint (red / blue) rectangles and the cursor.
    SetConstraintFeedback(position);
    if (valid != feedbackValid_) {
        SetCursor(valid ? kPlaceLotCursorValid : kPlaceLotCursorId);
    }
}

void cSC4ViewInputControlPlaceLot::SetCoverageEffectTransform(const cS3DVector3& position) {
    const float scale = coverageRingRadius_ * 2.0f;
    const cS3DTransform transform =
        MakeEffectTransform(position, 0.0f, scale, 2, scale != 1.0f ? 3 : 1);
    coverageEffect_->SetEffectTransform(transform);
    if (!coverageEffect_->IsStarted()) coverageEffect_->Start(0);
}

void cSC4ViewInputControlPlaceLot::SetFootprintEffectTransform(const cS3DVector3& position, int facing) {
    const float angle = static_cast<float>(facing) * 3.14159265f * 0.5f;
    const float scale = gridSize_ * 0.99999f;
    const cS3DTransform transform =
        MakeEffectTransform(position, angle, scale, 3, scale != 1.0f ? 7 : 5);
    footprintEffect_->SetEffectTransform(transform);
}

void cSC4ViewInputControlPlaceLot::SetDirectionEffectTransform(const cS3DVector3& position, int facing,
                                                               int sizeX, int sizeZ) {
    IPlaceLotTerrain* terrain = Terrain();
    const float cell = terrain ? terrain->CellSize() : 0.0f;
    const float alongX = static_cast<float>(sizeX) * cell;
    const float alongZ = static_cast<float>(sizeZ) * cell;
    const int distance = static_cast<int>((facing == 0 || facing == 2 ? alongZ : alongX) * 0.5f - 6.0f);
    cS3DVector3 point = position;
    switch (facing) {
        case 0: point.z -= static_cast<float>(distance); break;
        case 1: point.x += static_cast<float>(distance); break;
        case 2: point.z += static_cast<float>(distance); break;
        case 3: point.x -= static_cast<float>(distance); break;
        default: break;
    }
    const float angle = -static_cast<float>(facing) * 3.14159265f * 0.5f;
    const cS3DTransform transform = MakeEffectTransform(point, angle, 1.0f, 2, 5);
    directionEffect_->SetEffectTransform(transform);
}

// @ 0x1003c1220
void cSC4ViewInputControlPlaceLot::SetConstraintFeedback(const cS3DVector3& position) {
    cSC4LotConfiguration* configuration = Configuration();
    if (!configuration) return;
    const std::vector<cSC4LotConfigurationObject>& objects = configuration->GetLotConfigurationObjectArray();
    std::uint32_t sizeX = 0, sizeZ = 0;
    configuration->GetSize(sizeX, sizeZ);
    float centerX = 0.0f, centerZ = 0.0f;
    SC4Rect<int> rect;
    const bool valid = GetLotCenterAndCellCoverage(facing_, position.x, position.z, centerX, centerZ,
                                                   rect);
    (void)centerX;
    (void)centerZ;
    int redUsed = 0;
    int blueUsed = 0;
    if (valid) {
        IPlaceLotCity* city = City();
        if (!city) return;
        for (const cSC4LotConfigurationObject& object : objects) {
            if (object.type != kCoverageRed && object.type != kCoverageBlue) continue;
            // The object's footprint corners, as cells. The +8 centres the lot-local coordinates on a cell.
            int minCellX = 0, minCellZ = 0, maxCellX = 0, maxCellZ = 0;
            city->PositionToCell(object.minX + 8.0f, object.minZ + 8.0f, minCellX, minCellZ);
            city->PositionToCell(object.maxX + 8.0f, object.maxZ + 8.0f, maxCellX, maxCellZ);
            --maxCellX;
            --maxCellZ;
            int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
            switch (facing_) {
                case 0:
                    x0 = static_cast<int>(sizeX) - 1 - maxCellX;
                    x1 = static_cast<int>(sizeX) - 1 - minCellX;
                    z0 = static_cast<int>(sizeZ) - 1 - maxCellZ;
                    z1 = static_cast<int>(sizeZ) - 1 - minCellZ;
                    break;
                case 1:
                    z0 = static_cast<int>(sizeX) - 1 - maxCellX;
                    z1 = static_cast<int>(sizeX) - 1 - minCellX;
                    x0 = minCellZ;
                    x1 = maxCellZ;
                    break;
                case 2:
                    x0 = minCellX;
                    x1 = maxCellX;
                    z0 = minCellZ;
                    z1 = maxCellZ;
                    break;
                case 3:
                    x0 = static_cast<int>(sizeZ) - 1 - maxCellZ;
                    x1 = static_cast<int>(sizeZ) - 1 - minCellZ;
                    z0 = minCellX;
                    z1 = maxCellX;
                    break;
                default:
                    break;
            }
            x0 += rect.x0;
            z0 += rect.z0;
            const float values[4] = {static_cast<float>(x0), static_cast<float>(z0),
                                     static_cast<float>(rect.x0 + 1 + x1),
                                     static_cast<float>(rect.z0 + 1 + z1)};
            std::vector<IPlaceLotEffect*>& effects =
                object.type == kCoverageRed ? selectionRed_ : selectionBlue_;
            int& used = object.type == kCoverageRed ? redUsed : blueUsed;
            while (static_cast<std::size_t>(used) >= effects.size()) {
                effects.push_back(CreateEffect(object.type == kCoverageRed ? "selection_red"
                                                                          : "selection_blue"));
            }
            if (IPlaceLotEffect* effect = effects[static_cast<std::size_t>(used)]) {
                effect->SetVectorParams(kSelectionRectParam, values, 4);
                effect->Start(0);
            }
            ++used;
        }
    }
    // Stop the rectangles that are no longer used.
    for (std::size_t i = static_cast<std::size_t>(std::max(redUsed, 0)); i < selectionRed_.size(); ++i) {
        if (selectionRed_[i]) selectionRed_[i]->Stop(0);
    }
    for (std::size_t i = static_cast<std::size_t>(std::max(blueUsed, 0)); i < selectionBlue_.size(); ++i) {
        if (selectionBlue_[i]) selectionBlue_[i]->Stop(0);
    }
}

// @ 0x1003c0c74
void cSC4ViewInputControlPlaceLot::ChangeLotConfiguration(std::uint32_t lotConfigurationId) {
    if (lotConfigurationId_ == lotConfigurationId) return;
    ReleaseLotConfigurationEffects();
    lotConfigurationId_ = lotConfigurationId;
    GetBuildingPreviewModel();
}

// @ 0x1003bed24
bool cSC4ViewInputControlPlaceLot::OnMouseDownL(int x, int y, std::uint32_t) {
    bool placed = false;
    if (IPlaceLotView* view = View()) {
        cS3DVector3 position;
        if (view->PickTerrain(x, y, &position.x, false) && feedbackValid_) {
            SC4Rect<int> rect;
            if (PlaceLot(position, facing_, true, rect)) {
                const float cell = Terrain() ? Terrain()->CellSize() : 0.0f;
                // Remember the placed lot's world rectangle (shrunk by two units): CanPlaceLot refuses to
                // validate while the preview centre stays inside it.
                clearX0_ = static_cast<int>(cell * static_cast<float>(rect.x0) + 2.0f);
                clearZ0_ = static_cast<int>(cell * static_cast<float>(rect.z0) + 2.0f);
                clearX1_ = static_cast<int>(cell * static_cast<float>(rect.x1 + 1) - 2.0f);
                clearZ1_ = static_cast<int>(cell * static_cast<float>(rect.z1 + 1) - 2.0f);
                SetVisualUserFeedback(true, false, position);
                ReleaseEffect(footprintEffect_);
                ReleaseEffect(directionEffect_);
                view->ClearCostIndicator(true);
                feedbackValid_ = false;
                placed = true;
            }
        }
    }
    if (!placed && Host()) Host()->PlaySound(kPlaceLotSoundGroup, kPlaceLotSoundFailure);
    if (placed) {
        const int before = placementsRemaining_;
        placementsRemaining_ = before - 1;
        if (before < 2) EndInput();
    }
    return placed;
}

// @ 0x1003c0ca4
bool cSC4ViewInputControlPlaceLot::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    const bool previousValid = feedbackValid_;
    bool secondary = feedbackSecondary_;
    cS3DVector3 position{previewX_, previewY_, previewZ_};
    cRZString error = errorMessage_;

    mouseX_ = x;
    mouseY_ = y;
    bool valid = previousValid;
    if (IPlaceLotView* view = View()) {
        feedbackSecondary_ = false;
        if (!view->PickTerrain(x, y, &position.x, false)) {
            secondary = false;
            cRZString text;
            if (Host()) Host()->LoadString(kPlaceLotErrorStringGroup, 0xca711914, text);
            error = text;
            valid = false;
            view->ClearCostIndicator(false);
        } else {
            SC4Rect<int> rect{-1000, -1000, -1000, -1000};
            int facing = initialFacing_;
            bool haveGeometry = true;   // (char)uVar8
            if ((modifiers & 0x100) != 0) {
                // Shift held: no automatic facing; the centre is the picked point.
                float centerX = position.x;
                float centerZ = position.z;
                haveGeometry = GetLotCenterAndCellCoverage(facing, position.x, position.z, centerX,
                                                           centerZ, rect);
                if (haveGeometry) {
                    position.x = centerX;
                    position.z = centerZ;
                }
            } else if (tollBoothMode_) {
                std::uint32_t configurationId = 0;
                haveGeometry = PickTollBoothLotAndOrientation(configurationId, facing, position, rect);
                if (configurationId != lotConfigurationId_) ChangeLotConfiguration(configurationId);
                facing_ = facing;
            } else {
                cSC4LotConfiguration* configuration = Configuration();
                if (!configuration ||
                    !GetAutomaticFacingAndCellCoverage(configuration, position, facing, rect)) {
                    haveGeometry = false;
                } else {
                    facing_ = facing;
                }
            }
            const int usedFacing = facing;
            if (!haveGeometry) {
                int ignored = 0;
                CanPlaceLot(position, usedFacing, rect, secondary, lotCost_, ignored, error);
                facing_ = usedFacing;
                view->ClearCostIndicator(false);
                valid = false;
            } else {
                int resultCode = 0;
                bool geometry = false;
                const bool placed = CanPlaceLot(position, usedFacing, rect, geometry, lotCost_,
                                                resultCode, error);
                secondary = geometry;
                if (!placed) {
                    facing_ = initialFacing_;
                } else {
                    clearX0_ = 0;
                    clearZ0_ = 0;
                    clearX1_ = -1;
                    clearZ1_ = -1;
                }
                if (lotCost_ < 1) {
                    view->ClearCostIndicator(false);
                } else {
                    view->SetCostIndicator(static_cast<std::uint32_t>(lotCost_));
                }
                valid = placed;
            }
        }
    }

    // Cursor text: the tool only refreshes it when the reason changed.
    if (errorMessage_.CompareToString(error, true) != 0) {
        errorMessage_ = error;
        if (IPlaceLotView* view = View()) {
            view->SetCursorText(kPlaceLotCursorTextId, 0, 0, &errorMessage_, 1);
        }
    }

    // Feedback: only when something the feedback depends on changed.
    const auto differs = [](float a, float b) { return std::fabs(a - b) >= 0.0001f; };
    if (feedbackValid_ != valid || feedbackSecondary_ != secondary ||
        differs(previewX_, position.x) || differs(previewY_, position.y) ||
        differs(previewZ_, position.z) || feedbackFacing_ != initialFacing_) {
        SetVisualUserFeedback(valid, secondary, position);
        feedbackValid_ = valid;
        feedbackSecondary_ = secondary;
        previewX_ = position.x;
        previewY_ = position.y;
        previewZ_ = position.z;
        feedbackFacing_ = initialFacing_;
    }
    if (Host()) Host()->MouseMoved();
    return true;
}

// @ 0x1003c11bc
bool cSC4ViewInputControlPlaceLot::OnMouseExit() {
    feedbackSecondary_ = false;
    SetVisualUserFeedback(feedbackValid_, false, cS3DVector3{previewX_, previewY_, previewZ_});
    errorMessage_.Erase(0, 0xffffffffu);
    if (IPlaceLotView* view = View()) {
        view->ClearCursorText(kPlaceLotCursorTextId);
        view->ClearCostIndicator(false);
    }
    return true;
}

// @ 0x1003be3f0
bool cSC4ViewInputControlPlaceLot::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    if (IsOnTop()) {
        if (IPlaceLotView* view = View()) {
            if (cIGZWinKeyAccelerator* accelerator = view->KeyAccelerator()) {
                // vslot 0x58 TestForAccelerator(key, modifiers, true, command)
                std::uint32_t command = 0;
                accelerator->TestForAccelerator(key, modifiers, true, command);
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------
// Slice 3: the footprint / validity checks
// ---------------------------------------------------------------------------------------------------

// @ 0x1003c0282
bool cSC4ViewInputControlPlaceLot::GetLotCenterAndCellCoverage(int facing, float x, float z,
                                                              float& centerX, float& centerZ,
                                                              SC4Rect<int>& rect) {
    cSC4LotConfiguration* configuration = Configuration();
    if (!configuration) return false;
    std::uint32_t sizeX = 0, sizeZ = 0;
    configuration->GetSize(sizeX, sizeZ);
    if ((facing | 2) == 3) std::swap(sizeX, sizeZ);
    IPlaceLotTerrain* terrain = Terrain();
    if (!terrain) return false;
    const float cell = terrain->CellSize();
    int x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    if ((sizeX & 1) == 0) {
        const int i = static_cast<int>((cell * 0.5f + x) / cell);
        centerX = static_cast<float>(i) * cell;
        x1 = static_cast<int>(sizeX) / 2 - 1 + i;
        x0 = i - static_cast<int>(sizeX) / 2;
    } else {
        const int i = static_cast<int>(x / cell);
        centerX = (static_cast<float>(i) + 0.5f) * cell;
        x0 = i - static_cast<int>(sizeX) / 2;
        x1 = static_cast<int>(sizeX) / 2 + i;
    }
    rect.x0 = x0;
    rect.x1 = x1;
    if ((sizeZ & 1) == 0) {
        const int i = static_cast<int>((cell * 0.5f + z) / cell);
        centerZ = static_cast<float>(i) * cell;
        z1 = static_cast<int>(sizeZ) / 2 - 1 + i;
        z0 = i - static_cast<int>(sizeZ) / 2;
    } else {
        const int i = static_cast<int>(z / cell);
        centerZ = (static_cast<float>(i) + 0.5f) * cell;
        z0 = i - static_cast<int>(sizeZ) / 2;
        z1 = static_cast<int>(sizeZ) / 2 + i;
    }
    rect.z0 = z0;
    rect.z1 = z1;
    if (rect.x0 < 0 || rect.z0 < 0) return false;
    if (rect.x1 >= terrain->CellCountX()) return false;
    if (rect.z1 >= terrain->CellCountZ()) return false;
    return true;
}

// @ 0x1003c0440
int cSC4ViewInputControlPlaceLot::GetLotFacingStreetCount(const SC4Rect<int>& rect,
                                                          cSC4LotConfiguration* configuration,
                                                          const cS3DVector3& center, int facing) {
    (void)center;
    IPlaceLotCity* city = City();
    IPlaceLotTerrain* terrain = Terrain();
    if (!city || !terrain || !configuration) return 0;
    IPlaceLotNetworkMap* map = city->NetworkMap();
    if (!map) return 0;
    const int cellsX = terrain->CellCountX();
    const int cellsZ = terrain->CellCountZ();
    std::uint32_t sizeX = 0, sizeZ = 0;
    configuration->GetSize(sizeX, sizeZ);
    if ((facing | 2) == 3) std::swap(sizeX, sizeZ);
    int x = 0, z = 0, dx = 0, dz = 0, count = 0;
    switch (facing) {
        case 0: x = rect.x0; z = rect.z0 - 1; dx = 1; count = static_cast<int>(sizeX); break;
        case 1: x = rect.x1 + 1; z = rect.z0; dz = 1; count = static_cast<int>(sizeZ); break;
        case 2: x = rect.x0; z = rect.z1 + 1; dx = 1; count = static_cast<int>(sizeX); break;
        case 3: x = rect.x0 - 1; z = rect.z0; dz = 1; count = static_cast<int>(sizeZ); break;
        default: return 0;
    }
    int score = 0;
    if (count == 0) return 0;
    while (true) {
        --count;
        if (z < cellsZ && x < cellsX && (x | z) >= 0) {
            if (IPlaceLotNetworkCell* cell = map->GetCell(x, z, 0x449, 1)) {
                const std::uint32_t flags = cell->Flags();
                if ((flags & 0x40) != 0) {
                    score += 4;
                } else if ((flags & 1) != 0) {
                    score += 2;
                } else if ((flags & 0x408) != 0) {
                    score += 1;
                }
            }
        }
        x += dx;
        z += dz;
        if (count == 0) break;
    }
    return score;
}

// @ 0x1003bfe4e
bool cSC4ViewInputControlPlaceLot::CanDemolishLot(cSC4LotConfiguration* configuration,
                                                  const cS3DVector3& position, int facing,
                                                  const SC4Rect<int>& rect, std::int64_t* cost) {
    (void)configuration;
    (void)position;
    (void)facing;
    IPlaceLotHost* host = Host();
    if (!host || !host->Demolition()) return false;
    // +0x1f8 == 0xf2 (a landmark / reward tool) demolishes with the 0x800 privilege, everything else 0x2800.
    const std::uint32_t flags = globalEffectId_ == 0xf2 ? 0x800 : 0x2800;
    return host->Demolition()->DemolishRegion(rect, flags, cost);
}

// @ 0x1003c00a0
bool cSC4ViewInputControlPlaceLot::GetAutomaticFacingAndCellCoverage(cSC4LotConfiguration* configuration,
                                                                    cS3DVector3& position, int& facing,
                                                                    SC4Rect<int>& rect) {
    cSC4LotConfiguration* config = configuration;
    if (!config) return false;
    IPlaceLotLotManager* lotManager = Host() ? Host()->LotManager() : nullptr;
    if (!lotManager) return false;
    int bestScore = -1000;
    SC4Rect<int> bestRect{-1000, -1000, -1000, -1000};
    SC4Rect<int> currentRect{-1000, -1000, -1000, -1000};
    float bestX = 0.0f, bestZ = 0.0f, currentX = 0.0f, currentZ = 0.0f;
    std::uint32_t bestFacing = 0;
    int currentScore = 0;
    bool any = false;
    for (std::uint32_t candidate = 0; candidate < kFacingCount; ++candidate) {
        float centerX = 0.0f, centerZ = 0.0f;
        SC4Rect<int> candidateRect{};
        int score = -8;
        if (GetLotCenterAndCellCoverage(static_cast<int>(candidate), position.x, position.z, centerX,
                                        centerZ, candidateRect)) {
            const int result =
                lotManager->CanCreatePloppedLot(candidateRect, static_cast<int>(candidate), config);
            if (result == 8 &&
                CanDemolishLot(config, position, static_cast<int>(candidate), candidateRect, nullptr)) {
                const cS3DVector3 center{centerX, 0.0f, centerZ};
                score = GetLotFacingStreetCount(candidateRect, config, center, static_cast<int>(candidate));
                any = true;
            }
        }
        if (bestScore < score) {
            bestRect = candidateRect;
            bestX = centerX;
            bestZ = centerZ;
            bestScore = score;
            bestFacing = candidate;
        }
        if (candidate == static_cast<std::uint32_t>(initialFacing_)) {
            currentRect = candidateRect;
            currentX = centerX;
            currentZ = centerZ;
            currentScore = score;
        }
    }
    const bool useBest = currentScore < bestScore - 4 || (bestScore > 0 && currentScore == 0);
    std::uint32_t resultFacing = static_cast<std::uint32_t>(initialFacing_);
    float resultX = currentX, resultZ = currentZ;
    SC4Rect<int> resultRect = currentRect;
    if (useBest) {
        resultFacing = bestFacing;
        resultX = bestX;
        resultZ = bestZ;
        resultRect = bestRect;
    }
    facing = static_cast<int>(resultFacing);
    position.x = resultX;
    position.z = resultZ;
    rect = resultRect;
    return any;
}

// @ 0x1003c05cc
bool cSC4ViewInputControlPlaceLot::CanPlaceLot(const cS3DVector3& position, int facing,
                                               SC4Rect<int>& rect, bool& valid, std::int64_t& cost,
                                               int& resultCode, cRZString& error) {
    rect = SC4Rect<int>{-1, -1, -1, -1};
    valid = false;
    cost = 0;
    resultCode = 0;
    error.Erase(0, 0xffffffffu);
    IPlaceLotHost* host = Host();
    cSC4LotConfiguration* configuration = Configuration();
    if (!host || !configuration) return false;
    float centerX = position.x;
    float centerZ = position.z;
    valid = GetLotCenterAndCellCoverage(facing, position.x, position.z, centerX, centerZ, rect);
    if (!valid) return false;
    // The world rectangle of the lot placed by the last click: while the preview centre is inside it the
    // tool refuses to validate (0x1003c05cc).
    if (!(centerX < static_cast<float>(clearX0_) || centerZ < static_cast<float>(clearZ0_) ||
          static_cast<float>(clearX1_) < centerX || static_cast<float>(clearZ1_) < centerZ)) {
        return false;
    }
    std::int64_t base = buildingPrice_;
    if (base != 0) {
        IPlaceLotLotDeveloper* developer = host->BuildingDevelopment() ? host->BuildingDevelopment()->LotDeveloper()
                                                                      : nullptr;
        if (developer) base += developer->LotPrice(rect);
    }
    cost = base;
    const int result = host->LotManager() ? host->LotManager()->CanCreatePloppedLot(rect, facing, configuration) : 0;
    resultCode = result;
    std::uint32_t message = 0xca711914;
    switch (result) {
        case 0:
            break;
        case 1: message = 0xaa71194e; break;
        case 2: message = 0xaa711938; break;
        case 3: message = 0x6a71193f; break;
        case 4: message = 0xcabc118f; break;
        case 5: message = 0x6a711955; break;
        case 6: message = 0xaa71195c; break;
        case 7: message = 0x6a711944; break;
        case 8: {
            std::int64_t demolitionCost = 0;
            if (CanDemolishLot(configuration, position, facing, rect, &demolitionCost)) {
                cost += demolitionCost;
                return true;
            }
            message = 0x6a711969;
            break;
        }
        default:
            return false;
    }
    cRZString text;
    host->LoadString(kPlaceLotErrorStringGroup, message, text);
    error.AssignFromIGZString(text);
    return false;
}

// ---------------------------------------------------------------------------------------------------
// Slice 4: the commit path
// ---------------------------------------------------------------------------------------------------

// @ 0x1003beef8
bool cSC4ViewInputControlPlaceLot::PlaceLot(const cS3DVector3& position, int facing, bool playEffects,
                                            SC4Rect<int>& rect) {
    IPlaceLotHost* host = Host();
    if (!host) return false;
    rect = SC4Rect<int>{-1, -1, -1, -1};
    std::int64_t cost = lotCost_;
    bool checked = false;   // the quirk: the return value falls back to "CanPlaceLot ran" when no lot is made
    if (cost == -100000) {
        bool valid = false;
        int resultCode = 0;
        cRZString error;
        if (!CanPlaceLot(position, facing, rect, valid, cost, resultCode, error)) return false;
        lotCost_ = cost;
        checked = true;
    }
    cSC4LotConfiguration* configuration = Configuration();
    if (!configuration) return false;
    cS3DVector3 point = position;
    bool geometry = true;
    if (tollBoothMode_) {
        std::uint32_t configurationId = 0;
        geometry = PickTollBoothLotAndOrientation(configurationId, facing, point, rect);
        float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        if (geometry && City()) {
            City()->CellToPosition(rect.x0, rect.z0, x0, z0);
            City()->CellToPosition(rect.x1, rect.z1, x1, z1);
            point.x = (x1 - x0) * 0.5f + x0;
            point.z = (z1 - z0) * 0.5f + z0;
        }
        if (configurationId != lotConfigurationId_) ChangeLotConfiguration(configurationId);
        facing_ = facing;
    } else {
        float centerX = 0.0f, centerZ = 0.0f;
        geometry = GetLotCenterAndCellCoverage(facing, point.x, point.z, centerX, centerZ, rect);
    }

    // The mission hook: while a mission is active only its target rectangle may be built on.
    bool success = true;
    if (IPlaceLotCity* city = City()) {
        if (IPlaceLotMission* mission = city->Mission()) {
            if (mission->IsActive()) {
                if (IPlaceLotMissionTarget* target = mission->Target()) {
                    const std::int32_t* targetRect = target->Rect();
                    if (targetRect[0] != rect.x0 || targetRect[1] != rect.z0 || targetRect[2] != rect.x1) {
                        return false;
                    }
                    success = targetRect[3] == rect.z1;
                }
            }
        }
    }
    if (!(geometry && success)) return false;
    if (cost != 0 && host->Budget() && cost > host->Budget()->Funds()) return false;

    cSC4Lot* lot = nullptr;
    bool demolished = false;
    if (host->Demolition()) demolished = host->Demolition()->DemolishRegion(rect, 0x2000, nullptr);
    if (demolished && host->ZoneManager()) {
        if (host->ZoneManager()->PlaceZone(rect, 0, true, cost == 0, true, true, false)) {
            int zoneType = 0x0f;
            if (previewProp_) {
                const std::uint32_t type = previewBuildingType_;
                const bool civicGroup = host->OccupantGroup(type, 0x1508);
                const bool otherGroup = host->OccupantGroup(type, 0x1509);
                zoneType = 0x0b + (civicGroup ? 0 : 4);
                if (otherGroup) zoneType = 0x0c;
                if (configuration->IsCompatibleWithWater()) zoneType = 0x0c;
            }
            if (host->ZoneManager()->PlaceZone(rect, zoneType, true, true, true, true, false)) {
                const int sizeX = rect.x1 + 1 - rect.x0;
                const int sizeZ = rect.z1 + 1 - rect.z0;
                if (host->LotManager()) {
                    host->LotManager()->CreateLotIfPossible(rect.x0, rect.z0, sizeX, sizeZ, facing, true,
                                                            playEffects, configuration, lot);
                }
            }
        }
    }

    bool result = checked;
    if (lot) {
        IPlaceLotLotDeveloper* developer = host->BuildingDevelopment() ? host->BuildingDevelopment()->LotDeveloper()
                                                                      : nullptr;
        const bool constructed =
            developer && developer->ConstructDefaultLot(lot, configuration, buildingType_,
                                                        configuration->IsDoConstruction());
        result = constructed;
        if (!constructed) {
            // Construction failed after the zoning charged for the lot: the budget simulator is credited back.
            if (host->Budget()) host->Budget()->Deposit(cost);
        } else {
            if (cost != 0 && host->Budget()) host->Budget()->Withdraw(cost);
            host->ConnectLotUtilities(lot, point);
            if (playEffects) {
                const int largest = std::max(rect.x1 - rect.x0, rect.z1 - rect.z0);
                const char* name = largest < 1   ? "placement_effects_1x1"
                                   : largest == 1 ? "placement_effects_2x2"
                                   : largest < 3  ? "placement_effects_3x3"
                                                  : "placement_effects_4x4";
                if (IPlaceLotEffect* effect = CreateEffect(name)) {
                    cS3DTransform transform = MakeEffectTransform(point, 0.0f, 1.0f, 1, 1);
                    effect->SetEffectTransform(transform);
                    effect->Start(0);
                    effect->Release();
                }
                if (IPlaceLotEffect* effect = CreateEffect(selectionEffectName_.ToChar())) {
                    const float scale = coverageRingRadius_ * 2.0f;
                    cS3DTransform transform =
                        MakeEffectTransform(point, 0.0f, scale, 2, scale != 1.0f ? 3 : 1);
                    coverageActive_ = 1;
                    effect->SetEffectTransform(transform);
                    effect->SetFinishMessage(0x49d5b83f, 0);
                    effect->Start(0);
                    effect->Release();
                }
            }
        }
    }
    return result;
}

// @ 0x1003c084c
bool cSC4ViewInputControlPlaceLot::PickTollBoothLotAndOrientation(std::uint32_t& lotConfigurationId,
                                                                 int& facing, cS3DVector3& position,
                                                                 SC4Rect<int>& rect) {
    // kTollBoothLotConfigForNetworkType (0x100a1e980), indexed by network type: 0 road, 2 avenue,
    // 6 ground highway, 0xc highway; the other entries are 0.
    static const std::uint32_t kTollBoothConfig[16] = {
        0xebf9a440, 0, 0x8bf9b356, 0, 0, 0, 0xabf9a45c, 0, 0, 0, 0, 0, 0x2bf9b222, 0, 0, 0};
    lotConfigurationId = 0xebf9a440;
    facing = 2;
    IPlaceLotCity* city = City();
    IPlaceLotHost* host = Host();
    IPlaceLotNetworkMap* map = host ? host->NetworkMap() : nullptr;
    if (!city || !map) return false;
    int cellX = 0, cellZ = 0;
    city->PositionToCell(position.x, position.z, cellX, cellZ);
    float worldX = 0.0f, worldZ = 0.0f;
    city->CellToPosition(cellX, cellZ, worldX, worldZ);
    position.x = worldX;
    position.z = worldZ;
    rect = SC4Rect<int>{cellX, cellZ, cellX, cellZ + 1};
    position.z += 8.0f;
    IPlaceLotNetworkCell* cell = map->GetCell(cellX, cellZ, 0x1045, 1);
    if (!cell) return false;
    int networkType = 0;
    if (!cell->HasAnyFlag(1u << networkType)) {
        networkType = 2;
        if (!cell->HasAnyFlag(1u << networkType)) {
            networkType = 6;
            if (!cell->HasAnyFlag(1u << networkType)) {
                networkType = 0xc;
                if (!cell->HasAnyFlag(1u << networkType)) return false;
            }
        }
    }
    const std::uint32_t flags = cell->Flags();
    if ((flags & 0x1f4f) != ((1u << networkType) & flags & 0x1f4f)) return false;
    std::uint32_t edges = cell->EdgeTypes(static_cast<std::uint32_t>(networkType));
    bool straight[4] = {false, false, false, false};
    bool far[4] = {false, false, false, false};
    int straightCount = 0;
    for (int i = 0; i < 4; ++i) {
        const std::uint32_t type = edges & 3;
        if ((type | 2) == 3) return false;   // a single end bit: not a through connection
        straight[i] = type == 2;
        if (straight[i]) ++straightCount;
        far[i] = ((edges >> 2) & 1) != 0;
        edges >>= 8;
    }
    if (straightCount != 2) return false;
    const std::uint32_t network = static_cast<std::uint32_t>(networkType);
    switch ((networkType + 4u & 0xeu) >> 1) {
        case 0:
        case 3:
        case 5:
            if (!straight[0] || !straight[2]) {
                if (!straight[1]) return false;
                if (!straight[3]) return false;
                facing = 2;
                if (!far[0]) {
                    rect = SC4Rect<int>{cellX, cellZ, cellX + 1, cellZ + 1};
                    position.x += 8.0f;
                    position.z -= 16.0f;
                } else {
                    rect = SC4Rect<int>{cellX - 1, cellZ, cellX, cellZ + 1};
                    position.x -= 8.0f;
                }
            } else {
                facing = 1;
                if (!far[1]) {
                    rect = SC4Rect<int>{cellX, cellZ, cellX + 1, cellZ + 1};
                    position.z -= 8.0f;
                } else {
                    rect = SC4Rect<int>{cellX, cellZ - 1, cellX + 1, cellZ};
                    position.x += 8.0f;
                    position.z += 16.0f;
                }
            }
            lotConfigurationId = kTollBoothConfig[network];
            break;
        case 2:
            if (!straight[0] || !straight[2]) {
                if (!straight[1]) return false;
                if (!straight[3]) return false;
                facing = 2;
                lotConfigurationId = kTollBoothConfig[network];
                rect = SC4Rect<int>{cellX, cellZ, cellX, cellZ + 1};
            } else {
                facing = 1;
                lotConfigurationId = kTollBoothConfig[network];
                rect = SC4Rect<int>{cellX, cellZ, cellX + 1, cellZ};
                position.x += 8.0f;
                position.z -= 8.0f;
            }
            break;
        default:
            return false;
    }
    return SeekCompatibleNetworks(rect, 2, networkType);
}

// @ 0x1003c1b0e
bool cSC4ViewInputControlPlaceLot::SeekCompatibleNetworks(SC4Rect<int>& rect, int maxDepth,
                                                          int networkType) {
    IPlaceLotHost* host = Host();
    IPlaceLotNetworkMap* map = host ? host->NetworkMap() : nullptr;
    if (!map) return true;
    // kDX = {-1, 0, 1}, kDZ = {0, -1, 0}: west, north, east.
    static const int kDX[3] = {-1, 0, 1};
    static const int kDZ[3] = {0, -1, 0};
    const std::uint32_t mask = ((1u << (networkType & 0x1f)) & 0x1fffu) ^ 0x401fffu;
    struct Tile {
        int x, z, depth;
    };
    for (int x = rect.x0; x <= rect.x1; ++x) {
        for (int z = rect.z0; z <= rect.z1; ++z) {
            if (!map->GetCell(x, z, 0x1045, 1)) continue;
            std::vector<Tile> queue;
            queue.push_back(Tile{x, z, 0});
            for (std::size_t head = 0; head < queue.size(); ++head) {
                const Tile tile = queue[head];
                IPlaceLotNetworkCell* cell = map->GetCell(tile.x, tile.z, 0x1045, 1);
                if (!cell) continue;
                // Any connection of a *different* network type around the footprint means the toll booth
                // would sit on a mixed junction.
                if (cell->HasAnyFlag(mask)) return false;
                if (tile.depth >= maxDepth) continue;
                const std::uint8_t edgeFlags = cell->EdgeFlags();
                for (int i = 0; i < 3; ++i) {
                    if ((edgeFlags & (1u << i)) != 0) {
                        queue.push_back(Tile{tile.x + kDX[i], tile.z + kDZ[i], tile.depth + 1});
                    }
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// Slice 5: the preview model, the constraint feedback inputs, the messages and the two pick dialogs
// ---------------------------------------------------------------------------------------------------

// @ 0x1003bc9ac
void cSC4ViewInputControlPlaceLot::GetBuildingPreviewModel() {
    IPlaceLotHost* host = Host();
    cSC4LotConfiguration* configuration = Configuration();
    if (!host || !configuration) return;
    std::uint32_t sizeX = 0, sizeZ = 0;
    configuration->GetSize(sizeX, sizeZ);
    IPlaceLotTerrain* terrain = Terrain();
    const float cell = terrain ? terrain->CellSize() : 16.0f;

    // PARTIAL: the preview building model itself (the local grid helper's occupant, the model instances and
    // the cS3DTextureBinding grid) is not built. The port creates the preview building prop one step further
    // down and derives the same coverage categories / effect id / placement count from the building type.
    if (previewProp_ == nullptr) {
        if (cSC4LotConfigurationObject* object = configuration->GetPrimaryBuildingConfigurationObject()) {
            buildingRotation_ = object->orientation;
            modelCenterX_ = object->x - cell * static_cast<float>(sizeX) * 0.5f;
            modelCenterY_ = object->y;
            modelCenterZ_ = object->z - cell * static_cast<float>(sizeZ) * 0.5f;
            std::uint32_t buildingType = buildingType_;
            if (buildingType == 0 && host->BuildingDevelopment()) {
                std::vector<std::uint32_t> families = object->extra;
                if (families.empty()) families.push_back(object->id);
                if (!host->BuildingDevelopment()->ResolveBuildingType(families, buildingType)) {
                    buildingType = 0;
                }
            }
            previewBuildingType_ = buildingType;
            if (buildingType != 0 && host->PropManager()) {
                if (IPlaceLotProp* prop = host->PropManager()->CreateBuildingProp(buildingType)) {
                    previewProp_ = prop;
                    s3d::SC4DrawAttributes attributes;
                    attributes.flags = 2;
                    attributes.shade[0] = attributes.shade[1] = attributes.shade[2] = attributes.shade[3] = 1.0f;
                    SetPropOccupantDrawAttributes(cS3DVector3{previewX_, previewY_, previewZ_}, attributes,
                                                  false);
                }
            }
        }
    }

    // The props that decorate the lot.
    for (PreviewProp& preview : previewProps_) {
        if (preview.prop) preview.prop->Release();
    }
    previewProps_.clear();
    for (const cSC4LotConfigurationObject& object : configuration->GetLotConfigurationObjectArray()) {
        if (object.type != 1 || (object.flags & 1) != 0 || object.extra.size() != 1) continue;
        const std::uint32_t propId = object.extra[0];
        if (!host->PropManager() || !host->PropManager()->PropTypeExists(propId)) continue;
        IPlaceLotProp* prop = host->PropManager()->CreateProp(propId);
        if (!prop) continue;
        if (!prop->ShowInPreview()) {
            prop->Release();
            continue;
        }
        PreviewProp preview;
        preview.orientation = object.orientation;
        preview.x = object.x - cell * static_cast<float>(sizeX) * 0.5f;
        preview.y = object.y;
        preview.z = object.z - cell * static_cast<float>(sizeZ) * 0.5f;
        preview.prop = prop;
        previewProps_.push_back(preview);
    }

    // The grid texture (>256 world units is scaled down into the 0x100-byte grid; otherwise the smallest
    // power-of-two texture that fits). The texture and its cS3DTextureBinding are not created (PARTIAL).
    const std::uint32_t gridX = static_cast<std::uint32_t>(cell * static_cast<float>(sizeX));
    const std::uint32_t gridZ = static_cast<std::uint32_t>(cell * static_cast<float>(sizeZ));
    std::uint32_t largest = std::max(gridX, gridZ);
    if (largest >= 0x101) {
        gridSize_ = static_cast<float>(largest);
    } else {
        std::uint32_t textureSize = 0x10;
        if (largest >= 0x11) {
            textureSize = 0x20;
            if (largest >= 0x21) {
                textureSize = 0x40;
                if (largest >= 0x41) textureSize = largest < 0x81 ? 0x80 : 0x100;
            }
        }
        gridSize_ = static_cast<float>(textureSize);
    }

    // Coverage categories, the global effect id and the placement count.
    std::uint32_t buildingType = previewBuildingType_ != 0 ? previewBuildingType_ : buildingType_;
    if (buildingType == 0) {
        if (cSC4LotConfigurationObject* object = configuration->GetPrimaryBuildingConfigurationObject()) {
            buildingType = object->id;
        }
    }
    std::uint8_t categories = 0;
    if (buildingType != 0 && host->BuildingDevelopment()) {
        if (host->BuildingDevelopment()->BuildingIsPoliceStation(buildingType)) categories |= 1;
        if (host->BuildingDevelopment()->BuildingIsFireStation(buildingType)) categories |= 2;
        if (host->BuildingPropertyBool(buildingType, 0x91b42a2)) categories |= 4;
        if (host->BuildingPropertyBool(buildingType, 0x691b42b3)) categories |= 8;
    }
    coverageCategories_ = categories;
    if (categories != 0) {
        const char* name = CoverageCategoryName(categories);
        effectName_.Sprintf("PlopMode_%s_Inactive", name);
        selectionEffectName_.Sprintf("PlopMode_%s_Plop", name);
        const float fraction = (categories & (1 | 2)) != 0 ? 0.5f : 0.7f;
        const int category = (categories & 1) ? 0 : (categories & 2) ? 1 : (categories & 4) ? 2 : 3;
        if (!host->BuildingDevelopment()->BuildingCoverageRadius(buildingType, category, fraction,
                                                                 coverageRadius_, coverageRingRadius_)) {
            coverageRingRadius_ = 32.0f;
        }
        if (View()) {
            if (IPlaceLotCoverageHelper* helper = View()->CoverageHelper()) {
                helper->SetCategories(categories);
                helper->RecalcCoverage();
            }
        }
        if (host->OccupantGroup(buildingType, 0x1401)) {
            globalEffectId_ = 10;
        } else if (host->OccupantGroup(buildingType, 0x1302)) {
            globalEffectId_ = 0xf2;
        }
        placementsRemaining_ = 1000;
        if (host->BuildingPropertyBool(buildingType, 0xea2e078b) ||
            host->BuildingPropertyBool(buildingType, 0xea3209f8)) {
            placementsRemaining_ = 1;
        }
        std::uint32_t rewardCategory = 0;
        if (host->BuildingPropertyUint32(buildingType, 0xbd86fd3, rewardCategory)) {
            placementsRemaining_ = 16 - host->RewardNetworkType(static_cast<std::uint8_t>(rewardCategory));
        }
    }
}

// @ 0x1003c1998 / @ 0x1003c1afc (the +0x40 thunk)
bool cSC4ViewInputControlPlaceLot::DoMessage(cIGZMessage2* message) {
    if (!message) return false;
    const std::uint32_t type = message->GetType();
    constexpr std::uint32_t kHideFeedbackMessages[] = {0xa6b79602, 0xc9f775bb};
    const bool hideFeedback = type == kHideFeedbackMessages[0] || type == kHideFeedbackMessages[1];
    bool recalcCoverage = false;
    if (type == 0x277a9364) {
        if (localGrid_) {
            bool water = false;
            if (IPlaceLotTerrain* terrain = Terrain()) {
                if (IPlaceLotTerrainQuery* query = terrain->Query()) water = query->IsWater(previewX_, previewZ_);
            }
            localGrid_->ShowEffect(!water);
        }
        return false;
    }
    if (type == 0x49d5b83f) {
        coverageActive_ = 0;
        recalcCoverage = true;
    } else if (type == 0x99ef1142 || type == 0x99ef1143) {
        if (coverageActive_ != 0) return false;
        recalcCoverage = true;
    } else if (!hideFeedback) {
        return false;
    }
    if (hideFeedback) {
        ReleaseEffect(footprintEffect_);
        ReleaseEffect(directionEffect_);
        SetVisualUserFeedback(feedbackValid_, feedbackSecondary_,
                              cS3DVector3{previewX_, previewY_, previewZ_});
        return false;
    }
    if (recalcCoverage && View()) {
        if (IPlaceLotCoverageHelper* helper = View()->CoverageHelper()) helper->RecalcCoverage();
    }
    return false;
}
bool cSC4ViewInputControlPlaceLot::DoMessageFromMessageTarget(cIGZMessage2* message) {
    return DoMessage(message);
}

// @ 0x1003bc070 — PARTIAL
std::uint32_t cSC4ViewInputControlPlaceLot::DoUserLotConfigurationTypePick(std::uint32_t current) {
    // PARTIAL: the original builds a cGZWinGen + cGZWinGrid dialog here (listing "0x%08x" of every filtered
    // configuration), runs it through RZ::WM() and the window-location saver and parses the selection with
    // RZAtoUint32HexA. The dialog construction is not recovered; the host owns it.
    IPlaceLotHost* host = Host();
    if (!host || !host->Chooser()) return current;
    return host->Chooser()->PickLotConfiguration(current, lotConfigurationIds_);
}

// @ 0x1003bc502 — PARTIAL
std::uint32_t cSC4ViewInputControlPlaceLot::DoUserBuildingTypePick(std::uint32_t current) {
    // PARTIAL: same dialog construction as DoUserLotConfigurationTypePick (two columns: the building type id
    // as "0x%08x" and its name through the building development simulator's vslot 0x128).
    IPlaceLotHost* host = Host();
    if (!host || !host->Chooser()) return current;
    return host->Chooser()->PickBuildingType(current, buildingTypes_);
}

}  // namespace sc4
