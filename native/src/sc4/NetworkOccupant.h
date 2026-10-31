// cSC4NetworkOccupant — one network tile (road / rail / pipe / power piece) placed in the city, plus the
// edge connection store it keeps its per-network-type edge configuration in. 150 + 5 functions at
// 0x10019bc16..0x1001d2b10. The rendering half (Draw, DrawOneWayArrow, GetTexture, UpdateLighting, Pick via
// the 3D model instance) and path info (GetPathInfo/ReleasePathInfo) depend on the draw context and the
// path simulator and stay unrecovered; everything stateful is here: network flags, piece id, rotation /
// flip, edge types, vertex heights, add-on parts, bounds, property-backed values and the version 8 record.
//
// Object layout (0x1d0 bytes): +0x08 cSC4Occupant (type 0xc772bf98), +0x68 cS3DModelInstanceBasic,
// +0x9c transform, +0xd4 four tile corners {x,y,z,c0..c3,u,v}, +0x134 piece id, +0x138 under texture,
// +0x13c rotation|flip<<7, +0x13d variation, +0x140 edge store, +0x158 add-on parts, +0x170 bounds,
// +0x188 network flags, +0x190 persist ref, +0x1b8 path id override, +0x1c8 cost of demolition.
#pragma once
#include <cstdint>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/Occupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4NetworkOccupant = 0xc9c05c6e;
constexpr std::uint32_t kOccupantTypeNetwork = 0xc772bf98;
constexpr std::uint32_t kMessageNetworkUsableChanged = 0x89d3e9f3;
constexpr std::uint32_t kMessageNetworkLitChanged = 0x09d8e0b6;

// network flag bits (+0x188)
constexpr std::uint32_t kNetworkFlagPiece = 0x00000040;           // IsIntersection prerequisite
constexpr std::uint32_t kNetworkFlagTranslucent = 0x00004000;
constexpr std::uint32_t kNetworkFlagQuadDrawA = 0x02000000;
constexpr std::uint32_t kNetworkFlagQuadDrawB = 0x04000000;
constexpr std::uint32_t kNetworkFlagImmovable = 0x00400000;
constexpr std::uint32_t kNetworkFlagUsable = 0x10000000;
constexpr std::uint32_t kNetworkFlagLit = 0x20000000;
constexpr std::uint32_t kNetworkFlagSupportPylon = 0x40000000;
constexpr std::uint32_t kNetworkFlagGeometryDirty = 0x00002000;
constexpr std::uint32_t kNetworkFlagLightingDirty = 0x00008000;

// property ids kept on the occupant's property holder
constexpr std::uint32_t kNetworkPropEdgeTypes = 0x4835c6a7;       // u32[2]
constexpr std::uint32_t kNetworkPropPassageCeiling = 0x4835c6a8;  // float
constexpr std::uint32_t kNetworkPropPathOverride = 0x4835c6a9;    // u32
constexpr std::uint32_t kNetworkPropOneWay = 0x4835c6aa;          // u8
constexpr std::uint32_t kNetworkPropComplexOrigin = 0x4835c6ab;   // u32: x | z << 16
constexpr std::uint32_t kNetworkPropRetrofit = 0x4835c6ac;        // presence

// cSC4EdgeConnectionStore: the edge configuration (4 edges x 8 bits) of the tile for its primary network
// type plus up to a few other network types (type byte -> config).
class cSC4EdgeConnectionStore {
public:
    cSC4EdgeConnectionStore() = default;
    cSC4EdgeConnectionStore(const cSC4EdgeConnectionStore&) = default;
    cSC4EdgeConnectionStore& operator=(const cSC4EdgeConnectionStore&) = default;

    void SetEdgeTypeConfig(std::uint8_t networkType, std::uint32_t config);
    std::uint32_t GetEdgeTypeConfig(std::uint8_t networkType) const;
    std::uint32_t GetRaisedEdgeTypeConfig() const;       // config bits of the raised network types (mask 0x304)
    std::uint32_t CombinedConfig() const;                // primary | every extra
    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);

    std::uint8_t PrimaryType() const { return primaryType_; }
    std::uint32_t PrimaryConfig() const { return primaryConfig_; }
    std::size_t ExtraCount() const { return extras_.size(); }

private:
    struct Extra { std::uint8_t type; std::uint32_t config; };
    std::uint8_t primaryType_ = 0;                       // +4
    std::uint32_t primaryConfig_ = 0;                    // +8
    std::vector<Extra> extras_;                          // +0x10, count at +0
};

struct SC4TileCorner {
    float x = 0, y = 0, z = 0;           // +0 position
    std::uint8_t color[4] = {};          // +0xc rgba (a is the translucency)
    float u = 0, v = 0;                  // +0x10
};

struct SC4AddOnPart {
    std::uint32_t id = 0;                // texture / piece id
    SC4TileCorner corner[3];
};

class INetworkOccupantHost {
public:
    virtual ~INetworkOccupantHost() = default;
    virtual void PostMessage(std::uint32_t message, cISC4Occupant* occupant, std::uint32_t value) = 0;
    virtual void RegisterWithNetworkManager(cISC4Occupant* occupant) = 0;      // NetworkManager vslot 0xb8
    virtual bool IntersectionPieceId(std::uint32_t pieceId) = 0;               // sAvenueIntersectionPieceIds
    virtual void ReadPersistRef(cIGZIStream& stream, std::uint32_t clsid, cIGZUnknown*& out) = 0;
    virtual void WritePersistRef(cIGZOStream& stream, cIGZUnknown* object) = 0;
};
INetworkOccupantHost* NetworkOccupantHost();
void SetNetworkOccupantHost(INetworkOccupantHost* host);

// cISC4LightingManager as cSC4NetworkOccupant::UpdateLighting calls it (i386 vslots 0x30 and 0x60).
class INetworkOccupantLighting {
public:
    virtual ~INetworkOccupantLighting() = default;
    virtual void TerrainLighting(const float position[3], std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) = 0;      // 0x30
    virtual void SurfaceLighting(const float* triangle, const float normal[3], std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) = 0;   // 0x60
};

class cSC4NetworkOccupant : public cSC4Occupant {   // not final: cSC4UndergroundOccupant derives from it
public:
    cSC4NetworkOccupant();
    ~cSC4NetworkOccupant() override;

    // cSC4Occupant
    bool Init() override;
    bool Init(const float (*corners)[3]);                // positions of the 4 tile corners (12 floats)
    bool Shutdown() override { return cSC4Occupant::Shutdown(); }
    void GetPosition(float* xyz) override;
    void GetPosition(float* xyz) const;                  // 0x10019ecba (vslot 0x298): same values, const overload
    void SetPosition(const float*) override {}
    void GetBoundingBox(float* minXyz, float* maxXyz) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NetworkOccupant; }
    void SetHighlight(int highlight, bool post) { cSC4Occupant::SetHighlight(highlight, post); }

    // cISC4NetworkOccupant
    bool IsManagedGlobally() const { return false; }
    bool CheckForRetrofit();
    void SetRetrofitFlag(bool on);
    bool HasNetworkFlag(std::uint32_t flags) const { return (networkFlags_ & flags) == flags; }
    bool HasAnyNetworkFlag(std::uint32_t flags) const { return (networkFlags_ & flags) != 0; }
    virtual void SetNetworkFlag(std::uint32_t flags);   // cISC4NetworkOccupant vslot 0x50
    virtual void ClearNetworkFlag(std::uint32_t flags); // cISC4NetworkOccupant vslot 0x58
    std::uint32_t GetNetworkFlag() const { return networkFlags_; }
    bool GetConnectionNodeEdgeTypes(std::uint32_t& a, std::uint32_t& b);
    void SetConnectionNodeEdgeTypes(std::uint32_t a, std::uint32_t b);
    std::uint32_t GetConnectionPathIDOverride();
    void SetConnectionPathIDOverride(std::uint32_t id);
    void SetPathIDOverride(std::uint32_t id) { pathOverride_ = id; }              // @ 0x10019f878 (+0x1b8)
    std::uint32_t GetPathIDOverride() const { return pathOverride_; }   // 0x10019f884 (vslot 0x1a0)
    bool IsImmovable() const { return HasNetworkFlag(kNetworkFlagImmovable); }
    void SetImmovable(bool on) { on ? SetNetworkFlag(kNetworkFlagImmovable) : ClearNetworkFlag(kNetworkFlagImmovable); }
    bool IsUsable() const { return HasNetworkFlag(kNetworkFlagUsable); }
    void SetUsable(bool on) { on ? SetNetworkFlag(kNetworkFlagUsable) : ClearNetworkFlag(kNetworkFlagUsable); }
    bool IsPlaced() { return (GetFlags() & 1) != 0; }
    // +0x198: one byte per network type, an eight-byte-per-type block of entry-edge masks (AddPathConnection).
    void AddPathConnection(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit);   // 0x10019f830 (vslot 0x1b0)
    std::uint8_t PathConnections(std::uint32_t pathType, std::uint8_t entry) const {
        return pathConnections_[pathType * 4 + entry];
    }
    void ClearPathConnections() { for (auto& word : pathConnections_) word = 0; }
    // +0x1bc / +0x1c0: the path-origin offset used when a piece has a connection path id override. No recovered
    // cSC4NetworkOccupant function writes them; the derived occupants that create the offset do (the network tool
    // calls the setter), so the port keeps them on the base where the original stores them.
    float PathOffsetX() const { return pathOffsetX_; }
    float PathOffsetZ() const { return pathOffsetZ_; }
    void SetPathOffset(float x, float z) { pathOffsetX_ = x; pathOffsetZ_ = z; }
    // +0x84 (the cS3DModelInstanceBasic word at +0x1c): the pre-built-model init turns 0x50 off and 0x40 on.
    std::uint32_t ModelStateFlags() const { return modelStateFlags_; }
    void SetModelStateFlags(std::uint32_t clear, std::uint32_t set) { modelStateFlags_ = (modelStateFlags_ & ~clear) | set; }
    bool IsOfType(std::uint32_t networkType) const;
    std::uint32_t PieceId() const { return pieceId_; }
    std::uint8_t GetRotation() const { return rotationFlip_ & 0x7f; }
    std::uint8_t GetFlip() const { return rotationFlip_ >> 7; }
    std::uint8_t GetRotationAndFlip() const { return rotationFlip_; }
    void SetRotationAndFlip(std::uint8_t value) { rotationFlip_ = value; }
    void SetVariation(std::uint8_t variation);
    std::uint8_t GetVariation() const { return variation_; }
    void SetUnderTexture(std::uint32_t texture);
    std::uint32_t UnderTexture() const { return underTexture_; }
    void SetQuadDraw(std::uint32_t pieceId, std::uint8_t rotationFlip);
    void SetTextureCoordinates(std::uint8_t rotationFlip);
    void AddPart(const float (*positions)[3], const float (*uvs)[2], const std::uint32_t* indices,
                 std::uint32_t id);
    void ComputeBoundingBox(float* min, float* max);
    void GetVertexHeights(float* heights) const;
    float GetVertexHeight(int corner) const { return corners_[corner].y; }
    void SetVertexHeights(const float* heights);
    float GetAltitude(float x, float z) const;
    void GetNormal(float x, float z, float out[3]) const;
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const;
    virtual void SetTranslucency(bool on, std::uint8_t alpha);
    void MarkLightingUpdateNeeded() { SetNetworkFlag(kNetworkFlagLightingDirty); }
    void UpdateLighting(INetworkOccupantLighting& lighting);   // i386 0x001344b2 (vslot 0x188)
    bool IsLit() const { return HasNetworkFlag(kNetworkFlagLit); }
    bool CanBeLit() const { return false; }
    void SetLit(bool lit);
    bool IsSupportPylonPresent() const { return HasNetworkFlag(kNetworkFlagSupportPylon); }
    bool IsIntersection() const;
    bool IsPartOfTheSameStructure(cSC4NetworkOccupant& other);
    bool GetPassageCeiling(float& ceiling);
    void SetPassageCeiling(float ceiling);
    std::uint8_t GetOneWayDirections();
    void SetOneWayDirections(std::uint8_t directions);
    bool GetComplexPieceOrigin(int& x, int& z);
    void SetComplexPieceOrigin(int x, int z);
    std::uint32_t GetSortValue() const;
    void GetTransform(cS3DTransform& out) const;
    void SetTransform(const cS3DTransform& value);
    void GetModelInstanceTransform(cS3DTransform& out);          // GetModelInstances' transform update

    // edges
    std::uint32_t GetEdgeTypesFlag(std::uint8_t networkType) const { return edges_.GetEdgeTypeConfig(networkType); }
    void SetEdgeTypesFlag(std::uint8_t networkType, std::uint32_t config) { edges_.SetEdgeTypeConfig(networkType, config); }
    void SetEdgeType(std::uint8_t networkType, int edge, std::uint8_t type);
    std::uint32_t GetActiveEdges() const;
    std::uint32_t GetActiveEdgesForType(std::uint8_t networkType) const;
    std::uint32_t GetActiveEdgesForTypeNoGroup(std::uint8_t networkType) const;
    std::uint32_t GetGroupedEdges() const;
    bool ConnectsToNeighbor(int edge, std::uint8_t networkType) const;
    const cSC4EdgeConnectionStore& GetEdgeStore() const { return edges_; }
    void SetEdgeStore(const cSC4EdgeConnectionStore& store) { edges_ = store; }
    std::uint8_t PrimaryNetworkType() const { return static_cast<std::uint8_t>(edges_.PrimaryType()); }

    std::int64_t GetCostOfDemolition() const { return costOfDemolition_; }
    void SetCostOfDemolition(std::int64_t cost) { costOfDemolition_ = cost; }
    const std::vector<SC4AddOnPart>& AddOnParts() const { return parts_; }
    const SC4TileCorner& Corner(int i) const { return corners_[i]; }

    static std::uint32_t FlagForNetworkType(std::uint32_t networkType);
    static std::uint32_t HasConnectionOnEdge(std::uint32_t config, int edge);

protected:
    virtual void ReleasePathInfo();                              // 0x10019f722 (vslot 0x190)
    void PostChanged();
    // +0x190 also holds the path info the pre-built-model occupant resolves; its Write persists it.
    cIGZUnknown* PathInfoRef() const { return persistRef_; }
    void SetPathInfoRef(cIGZUnknown* ref) { persistRef_ = ref; }

    // the +0x170 bounds array (min xyz at +0, max xyz at +0xc); the underground geometry fills it directly
    float* Bounds() { return bounds_; }

    cS3DTransform transform_;                    // +0x9c
    SC4TileCorner corners_[4];                   // +0xd4
    std::uint32_t pieceId_ = 0;                  // +0x134
    std::uint32_t underTexture_ = 0;             // +0x138
    std::uint8_t rotationFlip_ = 0;              // +0x13c
    std::uint8_t variation_ = 0;                 // +0x13d
    cSC4EdgeConnectionStore edges_;              // +0x140
    std::vector<SC4AddOnPart> parts_;            // +0x158
    float bounds_[6] = {};                       // +0x170 min xyz, +0x17c max xyz
    std::uint32_t networkFlags_ = 0x06000000;    // +0x188
    cIGZUnknown* persistRef_ = nullptr;          // +0x190 the path info (persisted), released by the destructor
    std::uint8_t pathConnections_[0x20] = {};    // +0x198 eight network types x four entry bytes
    float pathOffsetX_ = 0.0f;                   // +0x1bc
    float pathOffsetZ_ = 0.0f;                   // +0x1c0
    std::uint32_t pathOverride_ = 0;             // +0x1b8
    std::int64_t costOfDemolition_ = 0;          // +0x1c8
    std::uint32_t modelStateFlags_ = 0;          // +0x84 (inside the +0x68 cS3DModelInstanceBasic)
    std::uint8_t modelFlags_ = 0;                // +0x88 model instance flags (bit 1 = translucent)
};

}  // namespace sc4
