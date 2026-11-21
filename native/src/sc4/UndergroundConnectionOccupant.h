// cSC4UndergroundConnectionOccupant - the piece that joins an underground network (pipe or subway) to a building above it
// (class id / interface 0x8a139184, object 0x300 bytes). A cSC4Occupant plus a cS3DModelInstanceBasic and a message target
// (view +0x60 subscribed to four view messages) that keeps the geometry of the connection piece (walls, trench, highlight,
// piece), the four corner heights, the cell and the two occupants it joins. Recovered from SimCity 4 Deluxe Edition 1.2.1
// (0x1002ef17a..0x1002f28b0).
//
// Record (version 2): the occupant, the wall quad count, seven vertex vectors, the cell, the four corner heights, the model
// height, an int, a flag, the building occupant, the network occupant, the interface id and the position.
//
// A lot that is a subway station or has a water connection gets one when it is plopped (cSC4ViewInputControlPlaceLot::
// PlaceLot): SetPosition at the lot's center cell, Init (the cell, the trench floor quad, the network piece of the cell,
// or a new piece joining a neighbor piece), SetBuildingOccupant, then the occupant manager. DoMessage follows the network
// pieces inserted on and removed from its cell and removes the connection with its building. Mac i386
// 0x0022f0f2..0x0023239e; the offsets in the comments below are i386.
//
// Drawing (0x00230aa4..0x00232394): CreateGeometry (on the first draw after a change and on a view rotation) builds the shaft from
// the connection's geometry table (see tUndergroundConnectionGeometryInfo), the walls and trench floor of a cell without a network
// piece, and loads the table's models; Draw paints the trench and walls in render pass 0 and the shaft and models in pass 1.
// The model maker takes the connection itself as its one model instance (GetModelInstances).
#pragma once
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "s3d/ModelInstance.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/UndergroundOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4UndergroundConnectionOccupant = 0x8a139184;
constexpr std::uint32_t kGZIID_cISC4PreBuiltUndergroundView = 0xa9764c8c;
constexpr std::uint32_t kUndergroundConnectionOccupantIID = 0x26bb767e;   // cISC4Occupant (the stored neighbours)
constexpr std::uint16_t kUndergroundConnectionRecordVersion = 2;
constexpr std::uint32_t kUndergroundConnectionViewMessages[4] = {0x8b87a974, 0x8b87a975, 0x8b8b2cee, 0xa6b79602};
constexpr float kTrenchFloorDepth = 15.0f;   // 0x00ab98a4

// The connection's geometry table (cSC4PipeConnectionOccupant / cSC4SubwayConnectionOccupant::sCustomGeometryInfo, i386 0x00ab9ba0 /
// 0x00ab9d20; the original hands it to Init as a tCustomGeometryInfo*, but the connection reads this layout from it).
struct tUndergroundConnectionGeometryInfo {
    std::uint32_t wallTexture = 0;             // +0x00 texture binding instance (group 0x891b0e1a) of the walls
    std::uint32_t floorTexture = 0;            // +0x04 the same, of the trench floor
    std::uint32_t pieceTexture = 0;            // +0x08 the shaft's texture (group 0x1abe787d)
    std::uint32_t topModel = 0;                // +0x0c the model drawn at the connection's height ({0x5ad0e817, 0xbadb57f1, id})
    std::uint32_t bottomModel = 0;             // +0x10 the model drawn at the trench floor
    const UndergroundVertex* vertices = nullptr;   // +0x14 the shaft (piece space)
    std::uint32_t vertexCount = 0;             // +0x18
    const std::uint32_t* topIndices = nullptr;     // +0x1c the shaft vertices at the connection's height
    const std::uint32_t* bottomIndices = nullptr;  // +0x20 the shaft vertices at the trench floor
    std::uint32_t indexCount = 0;              // +0x24 (of each list)
    bool faceCamera = false;                   // +0x28 the shaft turns to face the camera
    bool quadStrip = false;                    // +0x29 the shaft is drawn quad by quad (vertex pairs), each shaded by its normal
    float bottomOffset = 0.0f;                 // +0x2c how far below the trench floor the shaft ends
};

// What the drawing reaches (one method per original call).
class IUndergroundConnectionViewHost {
public:
    virtual ~IUndergroundConnectionViewHost() = default;
    virtual void PrepareRenderer() = 0;                                       // SL::spRenderer vslot 0x28 (SetupPackedRenderState)
    // the texture binding factory (service 0xfac0e219) vslot 0x10({0x7ab50e44, group, instance}); the caller owns the result
    virtual IUndergroundTextureBinding* CreateTextureBinding(std::uint32_t group, std::uint32_t instance) = 0;
    // spResMan vslot 0xc({0x5ad0e817, 0xbadb57f1, instance}, 0x9ad0e7e3) and the resource's vslot 0x10(0, &model); one reference
    virtual s3d::cIS3DModelInstance* CreateModel(std::uint32_t instance) = 0;
    // GZCOM_Interface<cIS3DModelInstance, cIS3DMeshAndMaterial>(0x26a68448): the model's meshes, released by the caller
    virtual IUndergroundModelMeshes* ModelMeshes(s3d::cIS3DModelInstance* instance) = 0;
    virtual void GetZoomAndRotation(int& zoom, int& rotation) = 0;          // SL::spRenderer vslot 0x20 (view utilities)
    virtual void GetCameraViewAngles(float& yaw, float& pitch, float& screenPitch) = 0;   // the same
    // spRenderer vslot 0x24 (the lighting manager) vslot 0x50 GetColor(direction): the light on a surface facing `normal`
    virtual cS3DVector3 LightColor(const cS3DVector3& normal) = 0;
    // FindBuildingOccupant (0x0023011a): spOccupantManager vslot 0xa4 over the one cell, the last building (type 0x278128a0)
    // found; borrowed (the original releases every listed occupant, the one it returns too)
    virtual cISC4Occupant* FindBuildingOccupant(int cellX, int cellZ) = 0;
};
void SetUndergroundConnectionViewHost(IUndergroundConnectionViewHost* host);

class cSC4UndergroundConnectionOccupant;

// The engine globals the connection reaches (one method per original call).
class IUndergroundConnectionHost {
public:
    virtual ~IUndergroundConnectionHost() = default;
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;           // spCity vslot 0x17c
    virtual void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) = 0;    // spCity vslot 0x180
    virtual float CellWidthX() = 0;                                                    // spCity vslot 0x16c
    virtual float CellWidthZ() = 0;                                                    // spCity vslot 0x170
    // spCity vslot 0x150 (the terrain) vslot 0x44: a cell's corner heights, (x, z), (x, z+1), (x+1, z+1), (x+1, z).
    virtual void CellCornerHeights(int cellX, int cellZ, float* heights) = 0;
    // cSC4UndergroundNetworkTool::FindUndergroundOccupant (static): the cell's piece answering `gziid`, one
    // reference, or null.
    virtual cSC4UndergroundOccupant* FindUndergroundOccupant(int cellX, int cellZ, std::uint32_t gziid) = 0;
    // cSC4UndergroundNetworkTool::FindUndergroundConnectionOccupant (static): the cell's connection whose network piece id
    // is `gziid`, one reference, or null.
    virtual cSC4UndergroundConnectionOccupant* FindUndergroundConnectionOccupant(int cellX, int cellZ, std::uint32_t gziid) = 0;
    // SL::NetworkManager() vslot 0x14 GetNetworkTool(type, false); AddRef, Init, PlaceNetworkOccupants(x0, z0, x1, z1, 0, null,
    // true, -1) (cISC4NetworkTool vslot 0x1c), Shutdown, Release.
    virtual void PlaceNetworkOccupants(std::uint32_t networkType, int x0, int z0, int x1, int z1) = 0;
    virtual void RemoveOccupant(cISC4Occupant* occupant, bool shutdown, std::uint32_t data) = 0;   // spOccupantManager vslot 0x54
    virtual void ChangeModelInRenderer(cSC4UndergroundConnectionOccupant* connection, int change) = 0;   // spRenderer vslot 0x7c
};
void SetUndergroundConnectionHost(IUndergroundConnectionHost* host);

class cSC4UndergroundConnectionOccupant : public cSC4Occupant, public cIGZMessageTarget2, public s3d::cS3DModelInstanceBasic {
public:
    cSC4UndergroundConnectionOccupant();                       // 0x0023239e
    ~cSC4UndergroundConnectionOccupant() override;             // 0x00230510

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x0022f364
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }     // 0x0022f4a6
    std::uint32_t Release() override { return cSC4Occupant::Release(); }   // 0x0022f4b0

    bool Init() override;                                      // 0x0022f87a
    bool Init(std::uint32_t gziid, const tUndergroundConnectionGeometryInfo* info);   // 0x0022f916
    bool Shutdown() override;                                  // 0x0022ff94
    bool Write(cIGZOStream& stream) const override;            // 0x00230344
    bool Read(cIGZIStream& stream) override;                   // 0x0023089a
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4UndergroundConnectionOccupant; }
    void GetSortValue(std::uint32_t& out) const override { out = kGZCLSID_cSC4UndergroundConnectionOccupant; }   // 0x0022f3cc
    void GetPosition(float* xyz) override;                     // 0x0022f458
    void SetPosition(const float* xyz);                        // 0x0022f47e
    void GetBoundingBox(float* minXyz, float* maxXyz) override;   // 0x0022f174
    void ComputeBoundingBox(float* minXyz, float* maxXyz);     // 0x0022f280 (vslot 0xd8; the same body)
    bool DoMessage(cIGZMessage2* message) override;            // 0x0023197a
    void GetWorldBounds(cS3DBoundingBox& out) override;        // 0x0022f4ba
    void PostOccupantMessage(std::uint32_t message, std::uint32_t data) override;   // 0x0022f5aa
    void ModelStateChanged();                                  // 0x0022f3da
    void UpdateNetworkOccupant();                              // 0x0022f4e0 (vslot 0xdc)
    void SetBuildingOccupant(cISC4Occupant* building);         // 0x0022f508 (vslot 0xd0)
    // drawing
    static bool StaticInit();                                  // 0x0022f0f2
    static void SetupPackedRenderState();                      // 0x0022f6e8
    void ChangeZoomLevel(SC4DrawContext* context);             // 0x0022fefc
    void CreateGeometry();                                     // 0x002315e4
    void CreatePieceModel();                                   // 0x00230c2e
    void CreateWalls();                                        // 0x00231506
    void AddWallQuad(std::uint32_t cornerA, std::uint32_t cornerB, std::vector<UndergroundVertex>& out);   // 0x00231386
    void CreateFloor();                                        // 0x00230b96
    void CreateHighlight();                                    // 0x00230aa4
    void SetHighlight(int highlight, bool post) override;      // 0x00230b60
    int Draw(SC4DrawContext* context, bool highlight) override;   // 0x00231ba6
    // 0x0022f258: the connection is its own (one) model instance; one reference.
    void GetModelInstances(int zoom, int rotation, s3d::cIS3DModelInstance** out, int capacity, int& count);
    void GetTransform(cS3DTransform& out) override { out = transform_; }              // 0x0022f58a
    void SetTransform(const cS3DTransform& transform) override { transform_ = transform; }   // 0x0022f56a

    // 0x0022f440: a connection piece hides both triangles of its cell (the first answer is always false).
    void GetHiddenTriangles(bool& first, bool& even, bool& odd) const { first = false; even = true; odd = true; }

    // the pure virtuals the pipe / subway connection classes answer
    virtual std::uint32_t GetGZIID() const = 0;                // slot of the derived class id
    virtual std::uint32_t GetNetworkOccupantGZIID() const = 0; // vslot 0xc8
    virtual std::uint32_t GetNetworkType() const = 0;          // vslot 0xcc
    // vslot 0xe0: the occupant as this connection's network piece (QueryInterface the piece id, then 0x4a10afc2), one
    // reference.
    virtual bool QueryOccupantInterface(cISC4Occupant* occupant, cSC4UndergroundOccupant*& out) = 0;

    const std::vector<UndergroundVertex>& Walls(int edge) const { return walls_[edge]; }
    std::uint32_t WallQuadCount() const { return wallCount_; }
    cISC4Occupant* NetworkOccupant() const { return network_; }     // +0xc4
    cISC4Occupant* BuildingOccupant() const { return building_; }   // +0xc8
    int CellX() const { return cellX_; }
    int CellZ() const { return cellZ_; }
    const UndergroundVertex& FloorVertex(int corner) const { return floor_[corner]; }
    float TrenchFloorY() const { return trenchFloorY_; }
    float ModelY() const { return modelY_; }
    const std::vector<UndergroundVertex>& Trench() const { return trench_; }
    const std::vector<UndergroundVertex>& Highlight() const { return highlight_; }
    const std::vector<UndergroundVertex>& Piece() const { return piece_; }
    const cS3DMatrix4& PieceMatrix() const { return pieceMatrix_; }
    const cS3DMatrix4& ViewMatrix() const { return viewMatrix_; }
    s3d::cIS3DModelInstance* TopModel() const { return topModel_; }
    bool ModelDirty() const { return modelDirty_; }
    IUndergroundTextureBinding* PieceTexture() const { return pieceTexture_; }
    static const cSC4PackedRenderState& TrenchPackedRenderState();

protected:
    // The body of both QueryOccupantInterface overrides (0x002f01fa / 0x002f0436): QueryInterface the piece id, then the
    // underground occupant id 0x4a10afc2 on what answered.
    static bool QueryPieceInterface(cISC4Occupant* occupant, std::uint32_t pieceId, cSC4UndergroundOccupant*& out);
    void SetGZIIDField(std::uint32_t id) { gziid_ = id; }

private:
    IUndergroundTextureBinding* wallTexture_ = nullptr; // +0x60
    std::uint32_t wallCount_ = 0;                       // +0x64
    std::vector<UndergroundVertex> walls_[4];           // +0x68 .. +0x8c
    IUndergroundTextureBinding* floorTexture_ = nullptr;   // +0x98
    std::vector<UndergroundVertex> trench_;             // +0x9c
    std::vector<UndergroundVertex> highlight_;          // +0xa8
    IUndergroundTextureBinding* pieceTexture_ = nullptr;   // +0xb4
    std::vector<UndergroundVertex> piece_;              // +0xb8
    cISC4Occupant* network_ = nullptr;                  // +0xc4 the network piece of the cell (held)
    cISC4Occupant* building_ = nullptr;                 // +0xc8 the building above (held)
    cS3DMatrix4 viewMatrix_;                            // +0xcc the models' transform (turned with the view)
    cS3DMatrix4 pieceMatrix_;                           // +0x10c the shaft's transform
    std::int32_t cellX_ = 0, cellZ_ = 0;                // +0x14c / +0x150
    s3d::cIS3DModelInstance* topModel_ = nullptr;       // +0x154
    s3d::cIS3DModelInstance* bottomModel_ = nullptr;    // +0x158
    float cornerHeights_[4] = {};                       // +0x15c the terrain heights at the cell's corners
    float modelY_ = 0.0f;                               // +0x16c the position's height
    float trenchFloorY_ = 0.0f;                         // +0x170 modelY - kTrenchFloorDepth
    std::int32_t value174_ = 0;                         // +0x174 (Init sets 1)
    bool modelDirty_ = false;                           // +0x178
    float bounds_[6] = {};                              // +0x17c min, +0x188 max
    cS3DTransform transform_;                           // +0x194
    UndergroundVertex floor_[4];                        // +0x1cc the trench floor quad
    float position_[3] = {};                            // +0x22c
    const tUndergroundConnectionGeometryInfo* geometryInfo_ = nullptr;   // +0x238
    std::uint32_t gziid_ = 0;                           // +0x23c
    bool loaded_ = false;                               // +0x240 (Read sets it: Init keeps the stored pieces)
    bool subscribed_ = false;
};

}  // namespace sc4
