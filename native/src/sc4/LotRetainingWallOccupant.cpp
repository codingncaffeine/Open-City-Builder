#include "sc4/LotRetainingWallOccupant.h"

#include <cstring>
#include <list>

#include "sc4/DBSegment.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

ILotRetainingWallHost* g_host = nullptr;

// The float-to-int conversion of the original: add the 2^23 magic constant, take the bit pattern and subtract
// its integer value. That rounds to nearest (ties to even) and, unlike a C cast, works the same for negatives.
// 12582912.0f is 0x4b400000.
std::int32_t RoundAltitudeToInt(float value) {
    const float biased = value + 12582912.0f;
    std::int32_t bits = 0;
    std::memcpy(&bits, &biased, sizeof bits);
    return bits - 0x4b400000;
}

SC4CellRect BoundingRectOf(cSC4Lot* lot) {
    std::int32_t rect[4] = {0, 0, 0, 0};
    lot->GetBoundingRect(rect);
    return SC4CellRect{rect[0], rect[1], rect[2], rect[3]};
}

}  // namespace

ILotRetainingWallHost* LotRetainingWallHost() { return g_host; }
void SetLotRetainingWallHost(ILotRetainingWallHost* host) { g_host = host; }

// -------------------------------------------------------------------------------------------- cSC4LotRetainingWallOccupant

cSC4LotRetainingWallOccupant::~cSC4LotRetainingWallOccupant() { SetLot(nullptr); }   // @ 0x10015cb9e

bool cSC4LotRetainingWallOccupant::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x10015cf26
    if (iid == kGZIID_cISC4LotRetainingWallOccupant) {
        *out = static_cast<cISC4LotRetainingWallOccupant*>(this);
        AddRef();
        return true;
    }
    return cSC4Occupant::QueryInterface(iid, out);
}

cISC4Occupant* cSC4LotRetainingWallOccupant::AsOccupant() {   // @ 0x10015e084
    return static_cast<cISC4Occupant*>(static_cast<cSC4Occupant*>(this));
}

bool cSC4LotRetainingWallOccupant::SetSize(const float* xyz) {   // @ 0x10015d26e
    size_.x = xyz[0];
    size_.y = xyz[1];
    size_.z = xyz[2];
    return true;
}

void cSC4LotRetainingWallOccupant::SetLot(cSC4Lot* lot) {   // @ 0x10015d290
    cSC4Lot* previous = lot_;
    if (previous == lot) return;
    if (lot) lot->AddRef();
    lot_ = lot;
    if (previous) previous->Release();
}

void cSC4LotRetainingWallOccupant::GetEdgeHashArray(int* out) const {   // @ 0x10015d2de
    for (int i = 0; i < 4; ++i) out[i] = edgeHash_[i];
}

void cSC4LotRetainingWallOccupant::SetEdgeHashArray(const int* in) {   // @ 0x10015d2f6
    for (int i = 0; i < 4; ++i) edgeHash_[i] = in[i];
}

bool cSC4LotRetainingWallOccupant::Shutdown() {   // @ 0x10015ccc6
    if (IsInitialized()) {
        SetLot(nullptr);
        cSC4Occupant::Shutdown();
    }
    return true;
}

void cSC4LotRetainingWallOccupant::GetPosition(float* xyz) {   // @ 0x10015d14e
    xyz[0] = position_.x;
    xyz[1] = position_.y;
    xyz[2] = position_.z;
}

void cSC4LotRetainingWallOccupant::SetPosition(const float* xyz) {   // @ 0x10015d18e
    position_.x = xyz[0];
    position_.y = xyz[1];
    position_.z = xyz[2];
}

void cSC4LotRetainingWallOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {   // @ 0x10015d1ce
    minXyz[0] = position_.x - size_.x * 0.5f;
    maxXyz[0] = size_.x * 0.5f + position_.x;
    minXyz[1] = position_.y - size_.y;
    maxXyz[1] = position_.y;
    minXyz[2] = position_.z - size_.z * 0.5f;
    maxXyz[2] = size_.z * 0.5f + position_.z;
}

bool cSC4LotRetainingWallOccupant::Write(cIGZOStream& stream) const {   // @ 0x10015cd1a
    stream.SetUint16(2);
    if (cSC4Occupant::Write(stream)) {
        stream.SetUint32(retainingWallType_);
        stream.SetSint32(edgeFlags_);
        for (int i = 0; i < 4; ++i) stream.SetSint32(edgeHash_[i]);
        stream << position_ << size_;
        if (g_host) g_host->WriteLotReference(stream, lot_);
    }
    return !stream.GetError();
}

bool cSC4LotRetainingWallOccupant::Read(cIGZIStream& stream) {   // @ 0x10015cde8
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (cSC4Occupant::Read(stream)) {
        stream.GetUint32(retainingWallType_);
        stream.GetSint32(edgeFlags_);
        for (int i = 0; i < 4; ++i) {
            stream.GetSint32(edgeHash_[i]);
            if (edgeHash_[i] != 0) edgeFlags_ |= 1 << i;
        }
        stream >> position_ >> size_;
        SetLot(nullptr);
        // SC4ReadGZSerializable(stream, 0xe818fced, &lot): the lot reference (in a city file, the lot record the segment resolves).
        if (g_host) {
            lot_ = g_host->ReadLotReference(stream);
        } else {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, 0xe818fced, &raw, false);
            if (raw) static_cast<cIGZUnknown*>(raw)->Release();
        }
    }
    // Quirk kept: a nested occupant record with a wrong version makes cSC4Occupant::Read return false without
    // setting the stream error, so this still reports success (and the wall fields stay untouched).
    return !stream.GetError();
}

// ------------------------------------------------------------------------------------------------ wall models

bool cSC4LotRetainingWallOccupant::FindRetainingWallModel(cISC43DPlaceableObject* placeable, int lotEdge,
                                                          ILotRetainingWallModel*& out) {   // @ 0x10015d30e
    cIS3DModelInstance* models[16] = {};
    std::uint32_t count = 16;
    placeable->GetModels(models, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!models[i]) continue;
        ILotRetainingWallModel* wallInterface = nullptr;
        if (!models[i]->QueryInterface(kGZIID_cISC4LotRetainingWallModel,
                                       reinterpret_cast<void**>(&wallInterface)) ||
            !wallInterface) {
            continue;
        }
        ILotRetainingWallModel* model = wallInterface->GetWallModel();
        out = model;   // (sic) also written when the edge does not match
        const std::int8_t modelEdge = model->WallEdge();
        if (modelEdge == lotEdge) model->AddRef();
        wallInterface->Release();
        if (modelEdge == lotEdge) return true;
    }
    return false;
}

bool cSC4LotRetainingWallOccupant::AddRetainingWallModel(cISC4Occupant* occupant,
                                                         cISC4LotRetainingWallOccupant* wall, int edge,
                                                         bool create, bool addToRenderer) {   // @ 0x10015d466
    ILotRetainingWallHost* host = LotRetainingWallHost();
    if (!host || !occupant) return true;
    cISCPropertyHolder* properties = occupant->AsPropertyHolder();
    if (!properties) return true;

    if (create) {
        cISC43DPlaceableObject* placeable = occupant->GetOrCreatePlaceableObject();
        if (!placeable) return true;
        placeable->AddRef();

        ILotRetainingWallModel* model = host->CreateRetainingWallModel();
        if (!model) {
            placeable->Release();
            return true;
        }
        model->AddRef();

        std::vector<std::uint32_t> textureIds(5);
        float textureScale = 0.0f;
        if (!properties->GetProperty(kPropertyRetainingWallTextures)) {
            cISCResExemplar* exemplar = nullptr;
            if (host->LoadTextureExemplar(exemplar) && exemplar) {
                cISCPropertyHolder* source = exemplar->AsISCPropertyHolder();
                cSCPropertyHelp::GetPropertyValue(source, kPropertyRetainingWallTextures, textureIds);
                cSCPropertyHelp::GetPropertyValue(source, kPropertyRetainingWallTextureScale, textureScale);
                exemplar->Release();
            }
            // (no else: without the base exemplar neither property is read, as in the original)
        } else {
            cSCPropertyHelp::GetPropertyValue(properties, kPropertyRetainingWallTextures, textureIds);
            cSCPropertyHelp::GetPropertyValue(properties, kPropertyRetainingWallTextureScale, textureScale);
        }

        cS3DBoundingBox bounds;
        occupant->GetBoundingBox(bounds.min, bounds.max);
        model->SetTextureData(textureIds.data(), textureScale);

        bool built = false;
        model->Init(bounds, edge, built);
        if (!built) {
            wall->SetEdgeFlags(wall->GetEdgeFlags() & ~(1 << edge));
        } else {
            cIS3DModelInstance* instance = model->AsModelInstance();
            instance->SetOwner(occupant);
            placeable->AddModel(instance);
            if (addToRenderer && host->RendererAvailable()) {
                if (!host->RendererHasModel(instance)) host->RendererAddModel(instance);
            }
            wall->SetEdgeFlags(wall->GetEdgeFlags() | (1 << edge));
        }
        model->Release();
        placeable->Release();
    } else {
        cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject();
        if (!placeable) return true;
        placeable->AddRef();
        ILotRetainingWallModel* model = nullptr;
        if (FindRetainingWallModel(placeable, edge, model)) {
            cIS3DModelInstance* instance = model ? model->AsModelInstance() : nullptr;
            // Quirk kept: the original reads SL::spRenderer without checking it for null here.
            if (host->RendererHasModel(instance)) host->RendererRemoveModel(instance);
            placeable->RemoveModel(instance);
            if (instance) instance->SetOwner(nullptr);
            model->Release();
        }
        placeable->Release();
    }
    return true;
}

bool cSC4LotRetainingWallOccupant::AddRetainingWallModels(cISC4Occupant* occupant, bool create,
                                                          bool addToRenderer) {   // @ 0x10015d802
    if (!occupant) return true;
    if (create) {
        cISC4LotRetainingWallOccupant* wall = nullptr;
        if (!occupant->QueryInterface(kGZIID_cISC4LotRetainingWallOccupant,
                                      reinterpret_cast<void**>(&wall)) ||
            !wall) {
            return true;
        }
        const std::uint32_t flags = static_cast<std::uint32_t>(wall->GetEdgeFlags());
        for (int edge = 0; edge < 4; ++edge) {
            if ((flags >> edge) & 1u) AddRetainingWallModel(occupant, wall, edge, true, addToRenderer);
        }
        wall->Release();
    } else {
        cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject();
        if (!placeable) return true;
        placeable->AddRef();
        placeable->RemoveAll();
        placeable->Release();
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ lot edges

bool cSC4LotRetainingWallOccupant::GetLotWallEdgeFlags(cSC4Lot* lot, float altitude, const SC4CellRect& lotRect,
                                                       int* edgeFlags, float& maxAltitude) {   // @ 0x10015d8ca
    (void)lot;   // the original does not read the lot either; the lot manager answers from the rect
    ILotRetainingWallHost* host = LotRetainingWallHost();
    if (!host) return false;

    maxAltitude = altitude + 0.1f;
    bool sampled = false;
    for (int edge = 0; edge < 4; ++edge) {
        const int edgeMask = 1 << edge;
        edgeFlags[edge] = 0;
        SC4CellRect cells{};
        if (!host->EdgeCellRect(lotRect, edgeMask, cells)) continue;

        // One rotated bit per scanned cell, starting at the top bit and wrapping around every 32 cells.
        std::uint32_t mask = 0;
        std::uint32_t rotate = 0x80000000u;
        bool invalid = false;
        for (int x = cells.x0; x <= cells.x1; ++x) {
            for (int z = cells.z0; z <= cells.z1; ++z) {
                cSC4Lot* other = host->LotAtCell(x, z);
                if (!other) {
                    invalid = true;                    // no lot at all
                } else {
                    if ((other->GetFlags() & 1) == 0) invalid = true;   // lot not locked
                    if (other->GetState() != kLotStateDemolishable) mask |= rotate;
                }
                rotate >>= 1;
                if (rotate == 0) rotate = 0x80000000u;
            }
        }
        if (!invalid) continue;   // a locked lot on every cell: no wall on this edge

        edgeFlags[edge] = static_cast<int>(mask);
        SC4CellRect terrainCells{};
        host->EdgeTerrainRect(lotRect, edgeMask, terrainCells);
        for (int x = terrainCells.x0; x <= terrainCells.x1; ++x) {
            for (int z = terrainCells.z0; z <= terrainCells.z1; ++z) {
                const float height = host->TerrainAltitudeAtVertex(x, z);
                if (height > maxAltitude) maxAltitude = height;
                edgeFlags[edge] += RoundAltitudeToInt(height) * (x * z) + 1;
                sampled = true;
            }
        }
    }
    return sampled;
}

std::uint32_t cSC4LotRetainingWallOccupant::GetRetainingWallType(cSC4Lot* lot) {   // @ 0x10015db0c
    ILotRetainingWallHost* host = LotRetainingWallHost();
    if (!host) return 0;
    const std::vector<std::uint32_t>& ids = host->RetainingWallIds(lot);
    if (ids.empty()) return 0;
    return ids[host->RandomBelow(static_cast<std::uint32_t>(ids.size()))];
}

// ---------------------------------------------------------------------------------------------- occupants

bool cSC4LotRetainingWallOccupant::AddRetainingWallOccupant(cSC4Lot* lot, bool create) {   // @ 0x10015db62
    ILotRetainingWallHost* host = LotRetainingWallHost();
    if (!host) return false;

    if (!create) {
        int x = 0;
        int z = 0;
        lot->GetLocation(x, z);
        cISC4Occupant* occupant = nullptr;
        bool removed = false;
        if (host->FirstOccupantAtCell(occupant, x, z, kOccupantTypeLotRetainingWall) && occupant) {
            host->RemoveOccupant(occupant);
            occupant->Release();
            removed = true;
        }
        lot->SetFlags(lot->GetFlags() & ~static_cast<int>(kLotFlagRetainingWall));
        return removed;
    }

    if ((lot->GetFlags() & 1) == 0) return false;   // only locked lots get a retaining wall

    const SC4CellRect lotRect = BoundingRectOf(lot);
    const float altitude = lot->GetAltitude();
    float maxAltitude = 0.0f;
    int edgeFlags[4] = {0, 0, 0, 0};
    if (!GetLotWallEdgeFlags(lot, altitude, lotRect, edgeFlags, maxAltitude)) return false;

    auto* wall = new cSC4LotRetainingWallOccupant();
    wall->AddRef();
    const std::uint32_t type = GetRetainingWallType(lot);
    if (type != 0 && !host->LoadRetainingWallProperties(wall, type)) {
        wall->Release();
        return false;
    }

    wall->SetEdgeFlags((edgeFlags[0] != 0) | ((edgeFlags[1] != 0) << 1) | ((edgeFlags[2] != 0) << 2) |
                       ((edgeFlags[3] != 0) << 3));

    const float center[3] = {
        static_cast<float>((lotRect.x1 + lotRect.x0) * 16 + 16) * 0.5f,
        altitude,
        static_cast<float>((lotRect.z1 + lotRect.z0) * 16 + 16) * 0.5f,
    };
    const float size[3] = {
        static_cast<float>((lotRect.x1 - lotRect.x0) * 16 + 16),
        maxAltitude - altitude,
        static_cast<float>((lotRect.z1 - lotRect.z0) * 16 + 16),
    };
    wall->SetPosition(center);
    wall->SetSize(size);
    wall->SetLot(lot);
    wall->SetEdgeHashArray(edgeFlags);
    wall->SetRetainingWallType(type);
    wall->Init();
    host->InsertOccupant(wall->AsOccupant());
    lot->SetFlags(lot->GetFlags() | static_cast<int>(kLotFlagRetainingWall));
    wall->Release();
    return false;   // (sic) the create path always reports false
}

void cSC4LotRetainingWallOccupant::UpdateRetainingWalls(cSC4Lot* lot) {   // @ 0x10015de56
    ILotRetainingWallHost* host = LotRetainingWallHost();
    if (!host) return;
    if ((lot->GetFlags() & 1) == 0) return;

    cISC4Occupant* occupant = nullptr;
    int x = 0;
    int z = 0;
    lot->GetLocation(x, z);
    if (!host->FirstOccupantAtCell(occupant, x, z, kOccupantTypeLotRetainingWall)) {
        AddRetainingWallOccupant(lot, true);
        return;
    }

    cISC4LotRetainingWallOccupant* wall = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4LotRetainingWallOccupant, reinterpret_cast<void**>(&wall)) || !wall) {
        AddRetainingWallOccupant(lot, true);
        occupant->Release();
        return;
    }

    const SC4CellRect lotRect = BoundingRectOf(lot);
    int oldHashes[4] = {0, 0, 0, 0};
    wall->GetEdgeHashArray(oldHashes);
    int newHashes[4] = {0, 0, 0, 0};
    float maxAltitude = 0.0f;
    GetLotWallEdgeFlags(lot, lot->GetAltitude(), lotRect, newHashes, maxAltitude);

    bool changed = false;
    for (int edge = 0; edge < 4; ++edge) {
        if (oldHashes[edge] == newHashes[edge]) continue;
        if (oldHashes[edge] != 0) AddRetainingWallModel(occupant, wall, edge, false, false);
        changed = true;
        if (newHashes[edge] != 0) AddRetainingWallModel(occupant, wall, edge, true, true);
    }
    if (changed) wall->SetEdgeHashArray(newHashes);
    wall->Release();
    occupant->Release();
}

void cSC4LotRetainingWallOccupant::UpdateSurroundingRetainingWalls(cSC4Lot* lot) {   // @ 0x10015e004
    ILotRetainingWallHost* host = LotRetainingWallHost();
    if (!host) return;
    std::list<cRZAutoRefCount<cSC4Lot>> lots;
    if (host->LotsAround(lot, lots)) {
        for (const auto& neighbor : lots) {
            if (neighbor) UpdateRetainingWalls(neighbor.get());
        }
    }
    lots.clear();
}

}  // namespace sc4
