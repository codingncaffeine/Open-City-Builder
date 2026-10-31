#include "sc4/NetworkOccupant.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
INetworkOccupantHost* g_host = nullptr;
constexpr float kTextureU[4] = {0.0f, 0.0f, 1.0f, 1.0f};
constexpr float kTextureV[4] = {0.0f, 1.0f, 1.0f, 0.0f};
constexpr std::uint32_t kAvenueIntersections[13] = {
    67122432, 67122688, 67129856, 67130112, 67130368, 67135744, 67137792,
    67138560, 67138816, 67142912, 67145728, 67146496, 67146752};
constexpr std::uint32_t kRaisedTypeMask = 0x304;     // network types 2, 8, 9 (raised / elevated networks)

void WriteCorner(cIGZOStream& s, const SC4TileCorner& c) {
    s.SetFloat32(c.x);
    s.SetFloat32(c.y);
    s.SetFloat32(c.z);
    s.SetFloat32(c.u);
    s.SetFloat32(c.v);
    for (int i = 0; i < 4; ++i) s.SetUint8(c.color[i]);
}

void ReadCorner(cIGZIStream& s, SC4TileCorner& c) {
    s.GetFloat32(c.x);
    s.GetFloat32(c.y);
    s.GetFloat32(c.z);
    s.GetFloat32(c.u);
    s.GetFloat32(c.v);
    for (int i = 0; i < 4; ++i) s.GetUint8(c.color[i]);
}

std::uint32_t PackEdgeBits(std::uint32_t config, std::uint32_t mask) {
    std::uint32_t out = 0;
    for (int edge = 0; edge < 4; ++edge)
        if ((config >> (edge * 8)) & mask) out |= 1u << edge;
    return out;
}
}  // namespace

INetworkOccupantHost* NetworkOccupantHost() { return g_host; }
void SetNetworkOccupantHost(INetworkOccupantHost* host) { g_host = host; }

// ------------------------------------------------------------------------------ edge connection store

void cSC4EdgeConnectionStore::SetEdgeTypeConfig(std::uint8_t type, std::uint32_t config) {
    if (primaryType_ != type) {
        if (primaryConfig_ != 0) {
            if (config == 0) {
                for (std::size_t i = 0; i < extras_.size();) {
                    if (extras_[i].type == type) {
                        extras_[i] = extras_.back();
                        extras_.pop_back();
                        ++i;                 // (sic) the original advances without re-checking the moved entry
                    } else {
                        ++i;
                    }
                }
                return;
            }
            for (auto& extra : extras_) {
                if (extra.type == type) {
                    extra.config = config;
                    return;
                }
            }
            extras_.push_back({type, config});
            return;
        }
        primaryType_ = type;
    }
    primaryConfig_ = config;
}

std::uint32_t cSC4EdgeConnectionStore::GetEdgeTypeConfig(std::uint8_t type) const {
    if (primaryType_ == type) return primaryConfig_;
    for (const auto& extra : extras_)
        if (extra.type == type) return extra.config;
    return 0;
}

std::uint32_t cSC4EdgeConnectionStore::CombinedConfig() const {
    std::uint32_t config = primaryConfig_;
    for (const auto& extra : extras_) config |= extra.config;
    return config;
}

std::uint32_t cSC4EdgeConnectionStore::GetRaisedEdgeTypeConfig() const {
    std::uint32_t config = 0;
    if (primaryType_ < 32 && ((kRaisedTypeMask >> primaryType_) & 1)) config = primaryConfig_;
    for (const auto& extra : extras_)
        if (extra.type < 32 && ((kRaisedTypeMask >> extra.type) & 1)) config |= extra.config;
    return config;
}

bool cSC4EdgeConnectionStore::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    stream.SetUint8(static_cast<std::uint8_t>(extras_.size()));
    stream.SetUint8(primaryType_);
    stream.SetUint32(primaryConfig_);
    for (const auto& extra : extras_) {
        stream.SetUint8(extra.type);
        stream.SetUint32(extra.config);
    }
    return !stream.GetError();
}

bool cSC4EdgeConnectionStore::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    extras_.clear();
    std::uint8_t count = 0;
    stream.GetUint8(count);
    stream.GetUint8(primaryType_);
    stream.GetUint32(primaryConfig_);
    extras_.resize(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        stream.GetUint8(extras_[i].type);
        stream.GetUint32(extras_[i].config);
    }
    return !stream.GetError();
}

// ------------------------------------------------------------------------------------ the occupant

cSC4NetworkOccupant::cSC4NetworkOccupant() {
    type_ = kOccupantTypeNetwork;
}

cSC4NetworkOccupant::~cSC4NetworkOccupant() {
    if (persistRef_) persistRef_->Release();
}

std::uint32_t cSC4NetworkOccupant::FlagForNetworkType(std::uint32_t networkType) {
    return networkType < 32 ? (1u << networkType) : 0;
}

std::uint32_t cSC4NetworkOccupant::HasConnectionOnEdge(std::uint32_t config, int edge) {
    return (config & (3u << ((edge & 3) << 3))) != 0;
}

bool cSC4NetworkOccupant::Init() {
    if (cSC4Occupant::IsInitialized()) {
        if (networkFlags_ & kNetworkFlagUsable) {
            networkFlags_ &= ~kNetworkFlagUsable;
            SetNetworkFlag(kNetworkFlagUsable);
        }
        if (networkFlags_ & 0x800000) {
            networkFlags_ &= ~0x800000u;
            SetNetworkFlag(0x800000);
        }
        return true;
    }
    ClearNetworkFlag(kNetworkFlagGeometryDirty);
    modelFlags_ = 0x1a;
    if (CheckForRetrofit() && g_host) g_host->RegisterWithNetworkManager(this);
    return cSC4Occupant::Init();
}

bool cSC4NetworkOccupant::Init(const float (*corners)[3]) {
    if (!Init()) return false;
    for (int i = 0; i < 4; ++i) {
        corners_[i].x = corners[i][0];
        corners_[i].y = corners[i][1];
        corners_[i].z = corners[i][2];
    }
    ComputeBoundingBox(&bounds_[0], &bounds_[3]);
    return true;
}

void cSC4NetworkOccupant::PostChanged() {
    if (g_host) g_host->PostMessage(kMessageOccupantChanged, this, 1);
}

void cSC4NetworkOccupant::ReleasePathInfo() {   // 0x10019f722
    if (persistRef_) {
        persistRef_->Release();
        persistRef_ = nullptr;
    }
}

void cSC4NetworkOccupant::AddPathConnection(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit) {
    // 0x10019f830: the entry byte of the type's mask plus the entry byte of the shared row (type 0), then flag 0x2000.
    if (entry != 0xff) {
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << (exit & 0x1f));
        pathConnections_[pathType * 4 + entry] |= bit;
        pathConnections_[entry] |= bit;
    }
    SetNetworkFlag(0x2000);
}

// ------------------------------------------------------------------------------------- flags / props

void cSC4NetworkOccupant::SetNetworkFlag(std::uint32_t flags) {
    if ((flags & kNetworkFlagUsable) && !(networkFlags_ & kNetworkFlagUsable)) {
        if ((GetFlags() & 1) && g_host) g_host->PostMessage(kMessageNetworkUsableChanged, this, 1);
    }
    if (flags & kNetworkFlagTranslucent) SetTranslucency(true, 0x80);
    networkFlags_ |= flags;
}

void cSC4NetworkOccupant::ClearNetworkFlag(std::uint32_t flags) {
    if ((flags & kNetworkFlagUsable) && (networkFlags_ & kNetworkFlagUsable) && g_host)
        g_host->PostMessage(kMessageNetworkUsableChanged, this, 0);
    if (flags & kNetworkFlagTranslucent) SetTranslucency(false, 0xff);
    networkFlags_ &= ~flags;
}

bool cSC4NetworkOccupant::IsOfType(std::uint32_t networkType) const {
    return HasNetworkFlag(FlagForNetworkType(networkType));
}

bool cSC4NetworkOccupant::GetConnectionNodeEdgeTypes(std::uint32_t& a, std::uint32_t& b) {
    std::vector<std::uint32_t> values;
    if (!cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kNetworkPropEdgeTypes, values) || values.size() < 2)
        return false;
    a = values[0];
    b = values[1];
    return true;
}

void cSC4NetworkOccupant::SetConnectionNodeEdgeTypes(std::uint32_t a, std::uint32_t b) {
    cSCPropertyHelp::SetPropertyValue(AsPropertyHolder(), kNetworkPropEdgeTypes, std::vector<std::uint32_t>{a, b});
}

std::uint32_t cSC4NetworkOccupant::GetConnectionPathIDOverride() {
    std::uint32_t id = 0;
    cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kNetworkPropPathOverride, id);
    return id;
}

void cSC4NetworkOccupant::SetConnectionPathIDOverride(std::uint32_t id) {
    if (id == 0) AsPropertyHolder()->RemoveProperty(kNetworkPropPathOverride, false);
    else cSCPropertyHelp::SetPropertyValue(AsPropertyHolder(), kNetworkPropPathOverride, id);
    pathOverride_ = id;
    ReleasePathInfo();
    ClearNetworkFlag(kNetworkFlagGeometryDirty);
}

bool cSC4NetworkOccupant::CheckForRetrofit() { return AsPropertyHolder()->GetProperty(kNetworkPropRetrofit) != nullptr; }

void cSC4NetworkOccupant::SetRetrofitFlag(bool on) {
    if (on) cSCPropertyHelp::SetPropertyValue(AsPropertyHolder(), kNetworkPropRetrofit, true);
    else AsPropertyHolder()->RemoveProperty(kNetworkPropRetrofit, false);
}

bool cSC4NetworkOccupant::GetPassageCeiling(float& ceiling) {
    return cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kNetworkPropPassageCeiling, ceiling);
}

void cSC4NetworkOccupant::SetPassageCeiling(float ceiling) {
    cSCPropertyHelp::SetPropertyValue(AsPropertyHolder(), kNetworkPropPassageCeiling, ceiling);
}

std::uint8_t cSC4NetworkOccupant::GetOneWayDirections() {
    std::uint8_t value = 0;
    cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kNetworkPropOneWay, value);
    return value;
}

void cSC4NetworkOccupant::SetOneWayDirections(std::uint8_t directions) {
    cISCPropertyHolder* holder = AsPropertyHolder();
    if (holder->GetProperty(kNetworkPropOneWay) == nullptr && directions == 0) return;
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkPropOneWay, directions);
}

bool cSC4NetworkOccupant::GetComplexPieceOrigin(int& x, int& z) {
    std::uint32_t packed = 0;
    if (!cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kNetworkPropComplexOrigin, packed)) return false;
    x = static_cast<int>(packed & 0xffff);
    z = static_cast<int>(packed >> 16);
    return true;
}

void cSC4NetworkOccupant::SetComplexPieceOrigin(int x, int z) {
    cSCPropertyHelp::SetPropertyValue(AsPropertyHolder(), kNetworkPropComplexOrigin,
                                      static_cast<std::uint32_t>(z * 0x10000 + x));
}

bool cSC4NetworkOccupant::IsPartOfTheSameStructure(cSC4NetworkOccupant& other) {
    int ax = 0, az = 0, bx = 0, bz = 0;
    GetComplexPieceOrigin(ax, az);
    other.GetComplexPieceOrigin(bx, bz);
    return ax == bx && az == bz;
}

bool cSC4NetworkOccupant::IsIntersection() const {
    if (!HasNetworkFlag(kNetworkFlagPiece)) return false;
    return g_host ? g_host->IntersectionPieceId(pieceId_)
                  : std::find(std::begin(kAvenueIntersections), std::end(kAvenueIntersections), pieceId_) !=
                        std::end(kAvenueIntersections);
}

// ------------------------------------------------------------------------------------- appearance

void cSC4NetworkOccupant::SetVariation(std::uint8_t variation) {
    if (variation_ == variation) return;
    variation_ = variation;
    PostChanged();
}

void cSC4NetworkOccupant::SetUnderTexture(std::uint32_t texture) {
    if (underTexture_ == texture) return;
    underTexture_ = texture;
    PostChanged();
}

void cSC4NetworkOccupant::SetLit(bool lit) {
    if (CanBeLit() && IsLit() != lit) {
        networkFlags_ ^= kNetworkFlagLit;
        if (g_host) g_host->PostMessage(kMessageNetworkLitChanged, this, 0);
    }
}

void cSC4NetworkOccupant::SetTranslucency(bool on, std::uint8_t alpha) {
    if (on) {
        modelFlags_ |= 2;
    } else {
        modelFlags_ &= ~2;
        alpha = 0xff;
    }
    for (auto& corner : corners_) corner.color[3] = alpha;
    PostOccupantMessage(kMessageOccupantChanged, 0);
}

void cSC4NetworkOccupant::SetTextureCoordinates(std::uint8_t rf) {
    for (int i = 0; i < 4; ++i) {
        corners_[i].u = kTextureU[(rf + i) & 3];
        corners_[i].v = kTextureV[(rf + i) & 3];
    }
    if (rf & 0x80) {
        // flipped: mirror one texture axis (u for even rotations, v for odd ones)
        if (rf & 1) {
            std::swap(corners_[0].v, corners_[3].v);
            std::swap(corners_[1].v, corners_[2].v);
        } else {
            std::swap(corners_[0].u, corners_[3].u);
            std::swap(corners_[1].u, corners_[2].u);
        }
    }
}

void cSC4NetworkOccupant::SetQuadDraw(std::uint32_t pieceId, std::uint8_t rotationFlip) {
    if (!HasNetworkFlag(0x200000)) {
        SetNetworkFlag(kNetworkFlagQuadDrawA);
        SetNetworkFlag(kNetworkFlagQuadDrawB);
    }
    ReleasePathInfo();
    ClearNetworkFlag(kNetworkFlagGeometryDirty);
    pieceId_ = pieceId;
    rotationFlip_ = rotationFlip;
    SetTextureCoordinates(rotationFlip);
    for (auto& corner : corners_) corner.color[3] = 0xff;
}

void cSC4NetworkOccupant::AddPart(const float (*positions)[3], const float (*uvs)[2], const std::uint32_t* indices,
                                  std::uint32_t id) {
    SC4AddOnPart part;
    part.id = id;
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t index = indices[i];
        part.corner[i].x = positions[index][0];
        part.corner[i].y = positions[index][1];
        part.corner[i].z = positions[index][2];
        part.corner[i].u = uvs[index][0];
        part.corner[i].v = uvs[index][1];
        for (auto& c : part.corner[i].color) c = 0xff;
    }
    parts_.push_back(part);
    ComputeBoundingBox(&bounds_[0], &bounds_[3]);
    MarkLightingUpdateNeeded();
}

// ------------------------------------------------------------------------------------- geometry

void cSC4NetworkOccupant::GetPosition(float* xyz) {
    xyz[0] = xyz[2] = 0.0f;
    xyz[1] = -FLT_MAX;
    float sumX = 0.0f, sumZ = 0.0f, top = -FLT_MAX;
    for (const auto& corner : corners_) {
        sumX += corner.x;
        sumZ += corner.z;
        if (top < corner.y) top = corner.y;
    }
    xyz[0] = sumX * 0.25f;
    xyz[1] = top;
    xyz[2] = sumZ * 0.25f;
}

void cSC4NetworkOccupant::GetPosition(float* xyz) const {   // 0x10019ecba (vslot 0x298)
    xyz[0] = xyz[2] = 0.0f;
    xyz[1] = -FLT_MAX;
    float sumX = 0.0f, sumZ = 0.0f, top = -FLT_MAX;
    for (const auto& corner : corners_) {
        sumX += corner.x;
        sumZ += corner.z;
        if (top < corner.y) top = corner.y;
    }
    xyz[0] = sumX * 0.25f;
    xyz[1] = top;
    xyz[2] = sumZ * 0.25f;
}

void cSC4NetworkOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {
    for (int i = 0; i < 3; ++i) {
        minXyz[i] = bounds_[i];
        maxXyz[i] = bounds_[3 + i];
    }
}

void cSC4NetworkOccupant::ComputeBoundingBox(float* min, float* max) {
    GetPosition(min);
    for (int i = 0; i < 3; ++i) max[i] = min[i];
    auto grow = [&](float x, float y, float z) {
        if (x < min[0]) min[0] = x;
        if (y < min[1]) min[1] = y;
        if (z < min[2]) min[2] = z;
        if (max[0] <= x && x != max[0]) max[0] = x;
        if (max[1] <= y && y != max[1]) max[1] = y;
        if (max[2] <= z && z != max[2]) max[2] = z;
    };
    for (const auto& corner : corners_) grow(corner.x, corner.y, corner.z);
    for (const auto& part : parts_)
        for (const auto& corner : part.corner) grow(corner.x, corner.y, corner.z);
}

void cSC4NetworkOccupant::GetVertexHeights(float* heights) const {
    for (int i = 0; i < 4; ++i) heights[i] = corners_[i].y;
}

void cSC4NetworkOccupant::SetVertexHeights(const float* heights) {
    for (int i = 0; i < 4; ++i) corners_[i].y = heights[i];
}

float cSC4NetworkOccupant::GetAltitude(float x, float z) const {
    float a = x - corners_[0].x;
    float b = z - corners_[0].z;
    const SC4TileCorner* diagonal = &corners_[3];       // +0x120
    float major = b;
    if (a <= b) {
        major = a;
        a = b;
        diagonal = &corners_[1];                        // +0xf0 (y of the second corner)
    }
    return ((corners_[2].y - diagonal->y) * major + (diagonal->y - corners_[0].y) * a) * 0.0625f + corners_[0].y;
}

void cSC4NetworkOccupant::GetNormal(float x, float z, float out[3]) const {
    const float a = x - corners_[0].x;
    const float b = z - corners_[0].z;
    float slopeZ;
    float slopeX;
    if (a < b) {
        slopeZ = corners_[0].y - corners_[1].y;
        slopeX = corners_[0].y - corners_[3].y;
    } else {
        slopeZ = corners_[3].y - corners_[2].y;
        slopeX = corners_[1].y - corners_[2].y;
    }
    out[0] = slopeX;
    out[1] = 16.0f;
    out[2] = slopeZ;
}

void cSC4NetworkOccupant::GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const {
    x = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::nearbyint(corners_[0].x * 0.0625f)));
    z = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::nearbyint(corners_[0].z * 0.0625f)));
}

std::uint32_t cSC4NetworkOccupant::GetSortValue() const {
    if (!HasAnyNetworkFlag(0x6000000)) return parts_.empty() ? 0xa821ef94u : parts_.front().id;
    return pieceId_;
}

void cSC4NetworkOccupant::GetTransform(cS3DTransform& out) const {
    out = transform_;
    ++out.changeCounter;
    out.flags = transform_.flags;
}

void cSC4NetworkOccupant::SetTransform(const cS3DTransform& value) {
    const std::uint8_t counter = transform_.changeCounter + 1;
    transform_ = value;
    transform_.changeCounter = counter;
}

void cSC4NetworkOccupant::GetModelInstanceTransform(cS3DTransform& out) {
    float position[3];
    GetPosition(position);
    for (int i = 0; i < 3; ++i) transform_.translation[i] = position[i];
    transform_.flags |= 1;
    ++transform_.changeCounter;
    out = transform_;
}

// ------------------------------------------------------------------------------------------ edges

void cSC4NetworkOccupant::SetEdgeType(std::uint8_t networkType, int edge, std::uint8_t type) {
    const std::uint32_t current = edges_.GetEdgeTypeConfig(networkType);
    const int shift = (edge << 3) & 0x1f;
    edges_.SetEdgeTypeConfig(networkType, (static_cast<std::uint32_t>(type) << shift) |
                                              (~(0xffu << shift) & current));
}

std::uint32_t cSC4NetworkOccupant::GetActiveEdges() const {
    return PackEdgeBits(edges_.CombinedConfig(), 7);
}

std::uint32_t cSC4NetworkOccupant::GetActiveEdgesForType(std::uint8_t networkType) const {
    return PackEdgeBits(edges_.GetEdgeTypeConfig(networkType), 7);
}

std::uint32_t cSC4NetworkOccupant::GetActiveEdgesForTypeNoGroup(std::uint8_t networkType) const {
    return PackEdgeBits(edges_.GetEdgeTypeConfig(networkType), 3);
}

std::uint32_t cSC4NetworkOccupant::GetGroupedEdges() const {
    return PackEdgeBits(edges_.CombinedConfig(), 4);
}

bool cSC4NetworkOccupant::ConnectsToNeighbor(int edge, std::uint8_t networkType) const {
    if (!IsOfType(networkType)) return false;
    return HasConnectionOnEdge(GetEdgeTypesFlag(networkType), edge) != 0;
}

// ---------------------------------------------------------------------------------------- record

bool cSC4NetworkOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(8);
    if (cSC4Occupant::Write(stream)) {
        stream << transform_;
        for (const auto& corner : corners_) WriteCorner(stream, corner);
        stream.SetUint32(pieceId_);
        stream.SetUint8(variation_);
        stream.SetUint32(underTexture_);
        stream.SetUint8(rotationFlip_);
        edges_.Write(stream);
        stream.SetUint32(static_cast<std::uint32_t>(parts_.size()));
        for (const auto& part : parts_) {
            stream.SetUint32(part.id);
            for (const auto& corner : part.corner) WriteCorner(stream, corner);
        }
        for (int i = 0; i < 3; ++i) {
            stream.SetFloat32(bounds_[i]);
            stream.SetFloat32(bounds_[3 + i]);
        }
        stream.SetUint32(networkFlags_);
        stream.SetUint32(pathOverride_);
        if (g_host) g_host->WritePersistRef(stream, persistRef_);
        stream.SetSint64(costOfDemolition_);
    }
    return !stream.GetError();
}

bool cSC4NetworkOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < 4) return false;
    if (cSC4Occupant::Read(stream)) {
        if (version < 5) LoadLegacyS3DTransform(stream, transform_);
        else stream >> transform_;
        for (auto& corner : corners_) ReadCorner(stream, corner);
        stream.GetUint32(pieceId_);
        stream.GetUint8(variation_);
        stream.GetUint32(underTexture_);
        stream.GetUint8(rotationFlip_);
        edges_.Read(stream);
        std::uint32_t partCount = 0;
        stream.GetUint32(partCount);
        parts_.assign(partCount, {});
        for (auto& part : parts_) {
            stream.GetUint32(part.id);
            for (auto& corner : part.corner) {
                ReadCorner(stream, corner);
                corner.color[3] = 0xff;
            }
        }
        for (int i = 0; i < 3; ++i) {
            stream.GetFloat32(bounds_[i]);
            stream.GetFloat32(bounds_[3 + i]);
        }
        std::uint32_t flags = 0;
        stream.GetUint32(flags);
        if (version < 6) {
            std::uint32_t shifted = flags << 14;
            std::uint32_t mapped = (shifted & 0x4000000) | (shifted & 0x2000000) | (shifted & 0x1000000) |
                                   (shifted & 0x800000) | ((flags << 13) & 0x80000) | ((flags << 13) & 0x40000) |
                                   (flags & 0x1f);
            if (flags & 0x2000) mapped += 0x40;
            if (flags & 0x4000) mapped += 0x20;
            if (flags & 0x8000) mapped += 0x400000;
            if (flags & 0x10000) mapped += 0x10000000;
            if (flags & 0x40000) mapped += 0x20000000;
            if (flags & 0x80000) mapped += 0x40000000;
            if (flags & 0x100000) mapped += 0x80;
            if (flags & 0x200000) mapped += 0x100;
            if (flags & 0x400000) mapped += 0x10000;
            if (flags & 0x800000) mapped += 0x20000;
            if (flags & 0x1000000) mapped += 0x100000;
            if (flags & 0x4000000) mapped ^= 0x80000000u;
            if (flags & 0x20000000) mapped += 0x200000;
            if (!(flags & 0x80)) mapped = (mapped & 0xf1ffffffu) | 0x8000000u;
            flags = mapped;
        }
        networkFlags_ = flags & 0xffff1fffu;
        stream.GetUint32(pathOverride_);
        if (pathOverride_ != 0) {
            if (version < 7) {
                float offsetX = 0, offsetZ = 0;
                stream.GetFloat32(offsetX);
                stream.GetFloat32(offsetZ);
                const float fx = ((corners_[0].x + corners_[2].x) * 0.5f + offsetX) * 0.0625f;
                const float fz = ((corners_[0].z + corners_[2].z) * 0.5f + offsetZ) * 0.0625f;
                SetComplexPieceOrigin(static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fz)));
            }
            if (version < 8) SetRetrofitFlag(true);
        }
        if (g_host) g_host->ReadPersistRef(stream, 0xa9667793, persistRef_);
        stream.GetSint64(costOfDemolition_);
        if (corners_[0].color[3] != 0xff || (modelFlags_ & 2)) {
            modelFlags_ &= ~2;
            for (auto& corner : corners_) corner.color[3] = 0xff;
        }
    }
    return !stream.GetError();
}

// i386 0x001344b2: every corner takes the terrain lighting at its position; every add-on part takes the lighting of its
// triangle's normal (all three corners alike). Then network flag 0x8000.
void cSC4NetworkOccupant::UpdateLighting(INetworkOccupantLighting& lighting) {
    for (SC4TileCorner& corner : corners_) {
        const float position[3] = {corner.x, corner.y, corner.z};
        lighting.TerrainLighting(position, corner.color[0], corner.color[1], corner.color[2]);
    }
    for (SC4AddOnPart& part : parts_) {
        float triangle[9];
        for (int i = 0; i < 3; ++i) {
            triangle[i * 3] = part.corner[i].x;
            triangle[i * 3 + 1] = part.corner[i].y;
            triangle[i * 3 + 2] = part.corner[i].z;
        }
        // SC4GeometryUtilities::GetNormalToTriangle + cS3DVector3::Normalize
        const float ax = triangle[3] - triangle[0], ay = triangle[4] - triangle[1], az = triangle[5] - triangle[2];
        const float bx = triangle[6] - triangle[0], by = triangle[7] - triangle[1], bz = triangle[8] - triangle[2];
        float normal[3] = {ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx};
        const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (length > 0.0f) for (float& n : normal) n /= length;
        std::uint8_t r = 0, g = 0, b = 0;
        lighting.SurfaceLighting(triangle, normal, r, g, b);
        for (SC4TileCorner& corner : part.corner) {
            corner.color[0] = r;
            corner.color[1] = g;
            corner.color[2] = b;
        }
    }
    SetNetworkFlag(0x8000);
}

}  // namespace sc4
