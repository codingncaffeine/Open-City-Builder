// cSC4PowerPoleOccupant - a power pole standing on the terrain (class id 0x09c05c6a; QueryInterface also answers 0xa8c86142
// and 0xa9764c8c, the message-target view 0x452294aa and the model's 0x26a68448). A cSC4Occupant plus a cS3DModelInstanceBasic
// (the renderer draws the pole through it: the pole model, the foundation under it and its wires), its list of connections to
// other poles (each with the power-line occupants strung along it and two sets of curves: the wires and the river buoys), the
// direction mask that picked its model, its cell and the vertex data of its foundation. Recovered from the Mac build
// (0x10031f406..0x100324abc, constructor 0x100320cf6; i386 0x0025499a..0x00259668).
//
// Record (version 5; version 4 differs only in the transform's legacy encoding): the occupant, the transform, the position
// (three floats), the connection count and each connection {pole, line count + lines, flag, flag, int, int, curve set A,
// curve set B}, the direction mask, four ints (the zoom and rotation of the loaded model and the cell), the four connection
// points, the three vertex vectors, a float, a flag, the two draw-attribute flag bytes, the draw shade and a flag.
//
// PARTIAL: the "powerpolezap" visual effect of a distressed pole (SetDistressed) and Pick are not recovered, and a version-4
// record (legacy transform) is refused.
#pragma once
#include <array>
#include <list>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "sc4/OccupantManager.h"
#include "s3d/ModelInstance.h"
#include "s3d/S3DMath.h"
#include "sc4/Occupant.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/PowerLineTool.h"
#include "sc4/UndergroundOccupant.h"

class SC4DrawContext;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PowerPoleOccupantRecord = 0x09c05c6a;   // GetGZCLSID @ 0x100324abc
constexpr std::uint32_t kGZIID_PowerPoleMessageTarget = 0x452294aa;
constexpr std::uint32_t kPowerPoleSortValue = 0xaa9da78e;
constexpr std::uint32_t kPowerPoleViewMessage = 0x99ef1142;
constexpr std::uint16_t kPowerPoleRecordVersion = 5;

// The engine services the pole reaches (one method per original call; app/NativePowerLineTool.cpp implements them).
class IPowerPoleOccupantHost {
public:
    virtual ~IPowerPoleOccupantHost() = default;
    virtual float CityCellWidthX() = 0;                                        // SL::spCity +0x16c (i386)
    virtual float CityCellWidthZ() = 0;                                        // SL::spCity +0x170 (i386)
    virtual float TerrainCellSize() = 0;                                       // SL::spTerrain +0x18 (i386)
    virtual void TerrainCornerHeights(int x, int z, float out[4]) = 0;         // SL::spTerrain +0x44 (i386)
    virtual bool CellHasWater(int x, int z) = 0;                               // SL::spTerrain +0x88 (i386)
    virtual int Get0To3Direction(int x1, int z1, int x2, int z2) = 0;          // cSC4NetworkRoutines::Get0To3Direction
    virtual int ViewZoom() = 0;                                                // SL::spViewUtilities +0x0c (i386)
    virtual int ViewRotation() = 0;                                            // SL::spViewUtilities +0x10 (i386)
    virtual float CameraYaw() = 0;                                             // SL::spViewUtilities +0x1c (i386): the rotation angle
    // cSC4ModelMakerUtility::GetModelInstanceID(occupant, zoom, rotation, state, false, null, null).
    virtual std::uint32_t ModelInstanceID(cISC4Occupant* occupant, int zoom, int rotation, int state) = 0;
    // spResMan +0x0c GetResource(key, 0x9ad0e7e3) then the model's +0x10 CreateModelInstance(0): one reference, or null.
    virtual s3d::cIS3DModelInstance* CreateModelInstance(const cGZPersistResourceKey& key) = 0;
    // The texture binding factory's texture {0x7ab50e44, 0x1abe787d, instance} as a driver texture id (0: missing).
    virtual int TextureId(std::uint32_t instance) = 0;
    // SL::spRenderer +0x24 lighting manager, +0x54 GetColor(normal, position) (i386).
    virtual cS3DVector3 LightColor(const cS3DVector3& normal, const cS3DVector3& position) = 0;
    // SL::spOccupantManager +0x50 InsertOccupant(occupant, 0) / SL::spCity occupant manager +0x54 RemoveOccupant(occupant,
    // true, 0) (i386).
    virtual void InsertOccupant(cISC4Occupant* occupant) = 0;
    virtual void RemoveOccupant(cISC4Occupant* occupant) = 0;
    // SL::spRM +0x0c GetPropertyExemplar({0x6534284a, 0xe7e2c2db, 0xc911e35b}, 0xa52160f5): the power-line tuning, or null.
    virtual const cISCPropertyHolder* TuningPropertyHolder() = 0;
    // SL::spEffectsManager vslot 0x18 (i386): a named visual effect (cISC4VisualEffect), one reference, or null; then its
    // vslots 0x20 SetSourceTransform + 0x0c Start(0), 0x14 IsStarted, 0x10 Stop.
    virtual cIGZUnknown* CreateVisualEffect(const char* name) { (void)name; return nullptr; }
    virtual void StartVisualEffect(cIGZUnknown* effect, const float* xyz) { (void)effect; (void)xyz; }
    virtual bool VisualEffectStarted(cIGZUnknown* effect) { (void)effect; return false; }
    virtual void StopVisualEffect(cIGZUnknown* effect, int transition) { (void)effect; (void)transition; }
};
IPowerPoleOccupantHost* PowerPoleOccupantHost();
void SetPowerPoleOccupantHost(IPowerPoleOccupantHost* host);

class cSC4PowerPoleOccupant : public IPowerPoleOccupant, public cSC4Occupant, public s3d::cS3DModelInstanceBasic,
                              public cIGZMessageTarget2 {
public:
    // One wire between this pole and another (0x60 bytes in the original).
    struct tConnection {
        cSC4PowerPoleOccupant* pole = nullptr;                 // +0x00 (a held reference)
        std::list<cSC4PowerLineOccupant*> lines;               // +0x08 the line occupants strung along it (held)
        bool linesInserted = false;                            // +0x20 the line occupants are in the city
        bool drawsWire = false;                                // +0x21 this pole draws the wire (the lower cell)
        std::int32_t otherX = 0;                               // +0x24 the other pole's cell
        std::int32_t otherZ = 0;                               // +0x28
        std::vector<std::vector<cS3DVector3>> curvesA;         // +0x30 the four wires
        std::vector<std::vector<cS3DVector3>> curvesB;         // +0x48 the buoys of a wire over water
    };

    cSC4PowerPoleOccupant();                                    // 0x100320cf6
    ~cSC4PowerPoleOccupant() override;                          // 0x100320efa

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x10032392a
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }     // 0x100323a38
    std::uint32_t Release() override { return cSC4Occupant::Release(); }   // 0x100323a9c

    bool Init() override;                                       // 0x10032261a
    bool Shutdown() override;                                   // 0x1003229fa
    void GetPosition(float* xyz) override;                      // 0x100321ffc
    void SetPosition(const float* xyz) override;                // 0x100323b6c
    void GetBoundingBox(float* minXyz, float* maxXyz) override;     // 0x100321b08
    void GetBounds(cS3DBoundingBox& out) override;                  // 0x100322268
    void GetWorldBounds(cS3DBoundingBox& out) override;             // 0x1003222a4
    void GetTransform(cS3DTransform& out) override { out = transform_; }              // 0x1003222d8
    void SetTransform(const cS3DTransform& transform) override { transform_ = transform; }   // 0x10032236e
    int Draw(SC4DrawContext* context, bool highlight) override;     // 0x1003211ac
    void GetSortValue(std::uint32_t& out) const override { out = kPowerPoleSortValue; }          // 0x100323b48
    void SetDefaultExemplar(cISCResExemplar* exemplar) override;    // 0x100321e42
    bool DoMessage(cIGZMessage2* message) override;                 // 0x100323dbe
    bool Write(cIGZOStream& stream) const override;             // 0x100322af6
    bool Read(cIGZIStream& stream) override;                    // 0x10032315c
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PowerPoleOccupantRecord; }   // 0x100324abc

    // --- cISC4PowerPoleOccupant (IPowerPoleOccupant) ---
    int GetNumberOfConnections() override { return static_cast<int>(connections_.size()); }   // 0x100321fa8
    IPowerPoleOccupant* GetConnectionOccupant(int index) override;                             // 0x10032059c
    void GetCell(std::uint32_t& x, std::uint32_t& z) override { x = static_cast<std::uint32_t>(cellX_); z = static_cast<std::uint32_t>(cellZ_); }
    void InitFromExemplar(cIGZUnknown* exemplar, std::uint32_t directionMask) override;  // Init(cISCResExemplar*, int) 0x1003226c6
    void SetExemplar(cIGZUnknown* exemplar) override;                                     // SetDefaultExemplar
    void SetPreview(bool preview) override { preview_ = preview; }                        // 0x10032433e
    void SetModelState(int highlight, bool immediate) override;                           // SetHighlight 0x100323b00
    void OnInserted() override { InsertAllPowerLineOccupants(); }                          // 0x100323ce8
    std::uint32_t DirectionFlags() const override { return style_; }
    void SetDirectionFlags(std::uint32_t flags) override { style_ = flags; }

    void GetCell(int& x, int& z) const { x = cellX_; z = cellZ_; }   // 0x100324012
    const std::vector<tConnection>& Connections() const { return connections_; }
    std::uint32_t Style() const { return style_; }
    bool IsPreview() const { return preview_; }                 // 0x100324332
    bool HasModel() const { return model_ != nullptr; }
    std::int64_t GetCostOfDemolition() const;                   // i386 0x006d910a: snCostOfDemolition (tuning 0x0a2eced8)
    bool GetDistressed() const { return distressed_; }          // i386 0x00254aa6
    bool HasDistressEffect() const { return effect_ != nullptr; }
    // debug: the record's four connection points as read equal the first four of the pole's table (what Write writes)
    bool SavedPointsMatchTable() const;
    void SetDistressed(bool distressed);                        // i386 0x00255134: the "powerpolezap" sparks

    // --- connections ---
    void AddConnection(cSC4PowerPoleOccupant* pole, std::vector<tLineInfo>* lines);     // 0x10031f4aa
    void UpdateConnection(cSC4PowerPoleOccupant* pole);                                 // 0x10031fe0c
    void RemoveConnection(tConnection& connection, bool quiet);                         // 0x100320270
    void RemoveConnection(cSC4PowerPoleOccupant* pole, bool quiet);                     // 0x100320340
    void DrawConnection(cSC4PowerPoleOccupant* pole, bool byCellOrder);                 // 0x1003204a4
    void BreakAllConnectionsAtDestination(bool quiet);                                  // 0x100320544
    void InsertAllPowerLineOccupants();                                                 // 0x100323ce8
    bool GetWireCells(cSC4PowerPoleOccupant* pole, std::vector<SC4PointU>& out);        // 0x10032203c
    bool GetWireCells(int connection, std::vector<SC4PointU>& out);                     // 0x100322088
    // The pole's own model instance for the model maker (the transform carries the position).
    void GetModelInstances(s3d::cIS3DModelInstance** out, int& count);                  // 0x100321efc

    static bool StaticInit();                                   // 0x1003227aa
    static bool StaticShutdown();                               // 0x100323f20
    // The class statics the drawing reads (the per-zoom wire widths, the four texture ids): for the kit report.
    static void DrawStatics(std::vector<float>& widths, int textures[4]);

private:
    static void UpdateLineOccupants(tConnection& connection, std::vector<tLineInfo>* lines);   // 0x10031f406
    void GetLineConnectionPoints(std::uint8_t direction, cS3DVector3 (&out)[4]);   // 0x10031faaa
    void InitConnectionPoints();                                // 0x100321c76
    bool LoadModel();                                           // 0x10032069e
    void SetModelPosition();                                    // 0x100321ce6
    void CreateFloor();                                         // 0x1003208d6
    void CreateWalls();                                         // 0x100320be8
    void AddWallQuad(std::size_t from, std::size_t to, std::vector<UndergroundVertex>& out);   // 0x100322408
    void ChangeZoomLevel(SC4DrawContext* context);              // 0x1003205b8
    void DrawPowerlines(SC4DrawContext* context);               // 0x100321672
    void CellOfPosition(std::uint32_t& x, std::uint32_t& z) const;
    void BuildCurves(cSC4PowerPoleOccupant* other, std::uint8_t direction, bool overWater, tConnection& connection);

    cS3DTransform transform_;                       // +0xa4
    float position_[3] = {0, 0, 0};                 // +0xdc
    std::vector<tConnection> connections_;          // +0xe8
    std::uint32_t style_ = 0;                       // +0x100 the direction mask
    s3d::cIS3DModelInstance* model_ = nullptr;      // +0x108 the pole model (LoadModel)
    std::int32_t modelRotation_ = -1, modelZoom_ = -1;   // +0x110 / +0x114 the view the model was loaded for
    std::int32_t cellX_ = 0, cellZ_ = 0;            // +0x118 / +0x11c
    bool boundsDirty_ = true;                       // +0x120
    std::int32_t boundsMinX_ = 0, boundsMinZ_ = 0, boundsMaxX_ = 0, boundsMaxZ_ = 0;   // +0x124..+0x130
    const float (*connectionPoints_)[4][3] = nullptr;   // +0x138 cardinal or diagonal table
    std::array<cS3DVector3, 4> savedPoints_{};      // the record's four connection points as read (written back without a table)
    std::vector<UndergroundVertex> floor_;          // +0x140 the foundation's top
    std::vector<UndergroundVertex> walls_[2];       // +0x158 / +0x170 the two visible foundation sides
    float floorHeight_ = 0.0f;                      // +0x188 the lowest corner of the cell
    bool field18c_ = false;                         // +0x18c
    bool subscribed_ = false;                       // +0x18d
    bool distressed_ = false;                       // +0x18e
    cIGZUnknown* effect_ = nullptr;                 // +0x190 the "powerpolezap" effect (stopped and released by Shutdown)
    bool preview_ = false;                          // +0x198
};

}  // namespace sc4
